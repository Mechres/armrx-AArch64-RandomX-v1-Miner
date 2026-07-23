#include "armrx/aes.hpp"
#include <cstdio>
#include <cstddef>
#include <array>

int main() {
    armrx::AesBlock initial{
        std::byte{0x00}, std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
        std::byte{0x40}, std::byte{0x50}, std::byte{0x60}, std::byte{0x70},
        std::byte{0x80}, std::byte{0x90}, std::byte{0xa0}, std::byte{0xb0},
        std::byte{0xc0}, std::byte{0xd0}, std::byte{0xe0}, std::byte{0xf0}};
    armrx::AesBlock rk{
        std::byte{0xd6}, std::byte{0xaa}, std::byte{0x74}, std::byte{0xfd},
        std::byte{0xd2}, std::byte{0xaf}, std::byte{0x72}, std::byte{0xfa},
        std::byte{0xda}, std::byte{0xa6}, std::byte{0x78}, std::byte{0xf1},
        std::byte{0xd6}, std::byte{0xab}, std::byte{0x76}, std::byte{0xfe}};
    auto enc = armrx::aes_encrypt_round(initial, rk);
    printf("armrx Result: ");
    for (int i = 0; i < 16; i++) printf("%02x", static_cast<unsigned>(enc[i]));
    printf("\n");

    // Also verify: full hash via upstream library
    return 0;
}
