#include <cstdio>
#include <cstdint>
#include <cstring>
#include <array>
#include "armrx/aes.hpp"
#include "soft_aes.h"

int main() {
    // A test input block
    armrx::AesBlock input{};
    for (int i = 0; i < 16; ++i) {
        input[i] = static_cast<std::byte>(i * 17 + 5);
    }

    armrx::AesBlock round_key{};
    for (int i = 0; i < 16; ++i) {
        round_key[i] = static_cast<std::byte>(i * 23 + 11);
    }

    // 1. Run our aes_decrypt_round
    armrx::AesBlock out_ours = armrx::aes_decrypt_round(input, round_key);

    // 2. Run upstream soft_aesdec
    rx_vec_i128 in_up, key_up;
    std::memcpy(&in_up, input.data(), 16);
    std::memcpy(&key_up, round_key.data(), 16);
    rx_vec_i128 out_up = soft_aesdec(in_up, key_up);

    armrx::AesBlock out_upstream{};
    std::memcpy(out_upstream.data(), &out_up, 16);

    printf("Ours:    ");
    for (int i = 0; i < 16; ++i) printf("%02x ", (int)out_ours[i]);
    printf("\nUpstream:");
    for (int i = 0; i < 16; ++i) printf("%02x ", (int)out_upstream[i]);
    printf("\n");

    if (out_ours == out_upstream) {
        printf("MATCH!\n");
    } else {
        printf("MISMATCH!\n");
    }

    return 0;
}
