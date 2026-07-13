// Upstream fillAes4Rx4 program dumper with correct seed
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "aes_hash.hpp"

int main() {
    uint64_t seed_u64[8] = {
        0x1b3bd9c4a9e5f646ULL,
        0xa2a13e38b041b5acULL,
        0xc49f5201656bcec2ULL,
        0x77a900b0be1e1946ULL,
        0x4f863cad0657b494ULL,
        0x159482eeaf4d6cb9ULL,
        0x2e417916bf21fc05ULL,
        0x066db274303c4fd4ULL
    };
    uint8_t tempHash[64];
    memcpy(tempHash, seed_u64, 64);

    uint8_t output[128 + 2048];
    memset(output, 0, sizeof(output));

    fillAes4Rx4<true>(tempHash, sizeof(output), output);

    printf("\nProgram opcodes (offset 128, every 8th byte):\n");
    for (int i = 0; i < 256; ++i) {
        printf(" %02x", output[128 + i * 8]);
    }
    printf("\n");

    return 0;
}
