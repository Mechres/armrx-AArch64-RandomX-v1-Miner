#include "armrx/aes.hpp"
#include "armrx/aes_generator.hpp"
#include "armrx/argon2.hpp"
#include "armrx/blake2b.hpp"
#include "armrx/memory.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

std::string hex(const armrx::Hash512& hash) {
    std::ostringstream result;
    for (const auto byte : hash) {
        result << std::hex << std::setw(2) << std::setfill('0')
               << std::to_integer<unsigned>(byte);
    }
    return result.str();
}

std::string hex(std::span<const std::byte> bytes) {
    std::ostringstream result;
    for (const auto byte : bytes) {
        result << std::hex << std::setw(2) << std::setfill('0')
               << std::to_integer<unsigned>(byte);
    }
    return result.str();
}

} // namespace

int main() {
    constexpr std::array<std::byte, 0> empty{};
    assert(hex(armrx::blake2b_512(empty)) ==
           "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419"
           "d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");
    assert(hex(armrx::blake2b(empty, 32)) ==
           "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");
    const auto hprime = armrx::argon2_hprime(empty, 1024);
    assert(hprime.size() == 1024U);
    assert(hprime == armrx::argon2_hprime(empty, 1024));
    constexpr auto mib = 1024ULL * 1024ULL;
    const auto small = armrx::choose_randomx_mode(2ULL * 1024ULL * mib, 1);
    assert(small.mode == armrx::RandomXMode::light);
    const auto ample = armrx::choose_randomx_mode(3ULL * 1024ULL * mib, 4);
    assert(ample.mode == armrx::RandomXMode::fast);

    // FIPS-197 AES-128 known-answer test: state after its initial AddRoundKey,
    // then the first encryption round with round key 1.
    constexpr armrx::AesBlock initial{
        std::byte{0x00}, std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
        std::byte{0x40}, std::byte{0x50}, std::byte{0x60}, std::byte{0x70},
        std::byte{0x80}, std::byte{0x90}, std::byte{0xa0}, std::byte{0xb0},
        std::byte{0xc0}, std::byte{0xd0}, std::byte{0xe0}, std::byte{0xf0}};
    constexpr armrx::AesBlock round_key{
        std::byte{0xd6}, std::byte{0xaa}, std::byte{0x74}, std::byte{0xfd},
        std::byte{0xd2}, std::byte{0xaf}, std::byte{0x72}, std::byte{0xfa},
        std::byte{0xda}, std::byte{0xa6}, std::byte{0x78}, std::byte{0xf1},
        std::byte{0xd6}, std::byte{0xab}, std::byte{0x76}, std::byte{0xfe}};
    constexpr armrx::AesBlock expected{
        std::byte{0x89}, std::byte{0xd8}, std::byte{0x10}, std::byte{0xe8},
        std::byte{0x85}, std::byte{0x5a}, std::byte{0xce}, std::byte{0x68},
        std::byte{0x2d}, std::byte{0x18}, std::byte{0x43}, std::byte{0xd8},
        std::byte{0xcb}, std::byte{0x12}, std::byte{0x8f}, std::byte{0xe4}};
    const auto encrypted = armrx::aes_encrypt_round(initial, round_key);
    assert(encrypted == expected);
    assert(armrx::aes_decrypt_round(encrypted, round_key) == initial);

    armrx::AesState seed{};
    armrx::AesGenerator1R single_step{seed};
    armrx::AesGenerator1R buffered{seed};
    const auto generated = single_step.next();
    std::array<std::byte, 64> output{};
    buffered.fill(output);
    assert(generated == output);
    assert(buffered.state() == output);
    assert(generated != seed);

    armrx::AesGenerator4R secure_step{seed};
    armrx::AesGenerator4R secure_buffered{seed};
    const auto secure_generated = secure_step.next();
    secure_buffered.fill(output);
    assert(secure_generated == output);
    assert(secure_buffered.state() == output);
    assert(secure_generated != seed);
    return 0;
}
