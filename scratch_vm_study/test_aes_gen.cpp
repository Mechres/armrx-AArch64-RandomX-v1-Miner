// Standalone test to compare our AesGenerator4R output with upstream fillAes4Rx4
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <array>

// Minimal includes to get both implementations running
#include "armrx/aes.hpp"
#include "armrx/aes_generator.hpp"

int main() {
    // Use the same tempHash as input (from blake2b of "This is a test")
    const uint8_t tempHash[64] = {
        0x15, 0x24, 0x55, 0x75, 0x1b, 0x73, 0xac, 0x21,
        0x67, 0xdd, 0x07, 0xed, 0x8a, 0xde, 0xb4, 0xf4,
        0x0a, 0x18, 0x75, 0xbc, 0xe1, 0xd6, 0x4c, 0xa9,
        0xbc, 0x50, 0x48, 0xf9, 0x4a, 0x70, 0xd2, 0x3f,
        0xf7, 0xd2, 0x6b, 0x86, 0x49, 0x8c, 0x64, 0x5a,
        0x4c, 0x3d, 0x75, 0xc7, 0x4a, 0xef, 0x7b, 0xbb,
        0xaa, 0xbf, 0xad, 0x29, 0x29, 0x8d, 0xdc, 0x0d,
        0xa6, 0xd6, 0x5f, 0x9c, 0xe8, 0x04, 0x35, 0x77
    };

    armrx::AesState seed;
    std::memcpy(seed.data(), tempHash, 64);

    armrx::AesGenerator4R gen{seed};

    // Generate 128 + 2048 bytes = first 2 blocks for entropy (128 bytes)
    std::array<std::byte, 128 + 2048> output{};
    gen.fill(output);

    // Print entropy bytes (first 128 bytes = 16 uint64_t)
    printf("Entropy (16 x uint64_t LE):\n");
    for (int i = 0; i < 16; ++i) {
        uint64_t val;
        std::memcpy(&val, output.data() + i * 8, 8);
        printf("  entropy[%2d] = 0x%016lx\n", i, val);
    }

    // Print key values derived from entropy
    uint64_t entropy[16];
    std::memcpy(entropy, output.data(), 128);

    printf("\nDerived values:\n");
    printf("  ma = 0x%08x\n", (uint32_t)(entropy[8] & 0x7FFFFFC0u));
    printf("  mx = 0x%08x\n", (uint32_t)entropy[10]);
    printf("  readReg0 = %lu\n", entropy[12] & 1);
    printf("  readReg1 = %lu\n", 2 + (entropy[13] & 1));
    printf("  readReg2 = %lu\n", 4 + (entropy[14] & 1));
    printf("  readReg3 = %lu\n", 6 + (entropy[15] & 1));

    return 0;
}
