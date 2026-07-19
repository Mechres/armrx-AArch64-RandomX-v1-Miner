#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include "armrx/aes.hpp"
#include "armrx/blake2b.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <array>
#include <cstddef>
#include <cstring>

static std::string hex(std::span<const std::byte> s) {
    std::ostringstream os;
    for (auto b : s)
        os << std::hex << std::setw(2) << std::setfill('0')
           << std::to_integer<unsigned>(b);
    return os.str();
}

int main() {
    // FIPS-197 AES KAT
    constexpr std::array<std::byte, 16> initial{
        std::byte{0x00}, std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
        std::byte{0x40}, std::byte{0x50}, std::byte{0x60}, std::byte{0x70},
        std::byte{0x80}, std::byte{0x90}, std::byte{0xa0}, std::byte{0xb0},
        std::byte{0xc0}, std::byte{0xd0}, std::byte{0xe0}, std::byte{0xf0}};
    constexpr std::array<std::byte, 16> rk{
        std::byte{0xd6}, std::byte{0xaa}, std::byte{0x74}, std::byte{0xfd},
        std::byte{0xd2}, std::byte{0xaf}, std::byte{0x72}, std::byte{0xfa},
        std::byte{0xda}, std::byte{0xa6}, std::byte{0x78}, std::byte{0xf1},
        std::byte{0xd6}, std::byte{0xab}, std::byte{0x76}, std::byte{0xfe}};
    constexpr std::array<std::byte, 16> exp{
        std::byte{0x89}, std::byte{0xd8}, std::byte{0x10}, std::byte{0xe8},
        std::byte{0x85}, std::byte{0x5a}, std::byte{0xce}, std::byte{0x68},
        std::byte{0x2d}, std::byte{0x18}, std::byte{0x43}, std::byte{0xd8},
        std::byte{0xcb}, std::byte{0x12}, std::byte{0x8f}, std::byte{0xe4}};
    auto enc = armrx::aes_encrypt_round(initial, rk);
    bool aes_ok = (enc == exp);
    std::cout << "AES KAT: " << (aes_ok ? "PASS" : "FAIL") << std::endl;
    if (!aes_ok) std::cout << "  got: " << hex(enc) << std::endl;

    // Full hash check
    std::array<std::byte, 12> key;
    const char k[] = "test key 000";
    for (size_t i = 0; i < 12; i++) key[i] = static_cast<std::byte>(k[i]);
    armrx::Argon2dCache cache;
    cache.initialize(key);

    alignas(16) std::array<std::byte, 32> h;
    armrx::VirtualMachine vm{armrx::kRandOMXFlagHardAes};
    vm.set_cache(&cache);
    armrx::randomx_calculate_hash(&vm, "This is a test", 14, h.data());
    std::string hstr = hex(h);
    bool hash_ok = (hstr == "639183aae1bf4c9a35884cb46b09cad9175f04efd7684e7262a0ac1c2f0b4e3f");
    std::cout << "Input1 hash: " << hstr << std::endl;
    std::cout << "Hash KAT: " << (hash_ok ? "PASS" : "FAIL") << std::endl;

    return (aes_ok && hash_ok) ? 0 : 1;
}
