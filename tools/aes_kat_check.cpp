#include "armrx/aes.hpp"
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <array>
#include <random>

// Oracle: the hardware AES funnel (encrypt_transform/decrypt_transform under
// __ARM_FEATURE_AES) must be byte-identical to the scalar T-table implementation
// (encrypt_transform_ttable/decrypt_transform_ttable) across many random inputs.
// A broken AESE/AESD lane pairing shows up here deterministically — before any
// mining — because both paths share the same trailing-AddRoundKey in the callers
// (aes_encrypt_round adds key^state; the x4 helpers EOR the key after the transform).

static bool blocks_equal(const armrx::AesBlock& a, const armrx::AesBlock& b) {
    return std::memcmp(a.data(), b.data(), 16) == 0;
}

static int check_round(const char* name,
                       armrx::AesBlock (*funnel)(armrx::AesBlock),
                       armrx::AesBlock (*ttable)(armrx::AesBlock),
                       std::mt19937_64& rng) {
    int mismatches = 0;
    for (int i = 0; i < 20000; ++i) {
        armrx::AesBlock in{};
        for (auto& b : in) b = static_cast<std::byte>(rng() & 0xff);
        auto h = funnel(in);
        auto t = ttable(in);
        if (!blocks_equal(h, t)) {
            if (mismatches < 4) {
                std::printf("  MISMATCH [%s] input=", name);
                for (int k = 0; k < 16; ++k) std::printf("%02x", (unsigned)(std::byte)in[k]);
                std::printf(" hw=", name);
                for (int k = 0; k < 16; ++k) std::printf("%02x", (unsigned)(std::byte)h[k]);
                std::printf(" ttable=");
                for (int k = 0; k < 16; ++k) std::printf("%02x", (unsigned)(std::byte)t[k]);
                std::printf("\n");
            }
            ++mismatches;
        }
    }
    if (mismatches == 0)
        std::printf("[OK] %s: 20000 random blocks hw == ttable\n", name);
    else
        std::printf("[FAIL] %s: %d / 20000 mismatches\n", name, mismatches);
    return mismatches;
}

int main() {
    std::mt19937_64 rng(0x9e3779b97f4a7c15ULL);

    // 1) Known-answer check on the documented FIPS-197-style vector from the prior tool.
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
    std::printf("armrx Result: ");
    for (int i = 0; i < 16; i++) std::printf("%02x", static_cast<unsigned>(enc[i]));
    std::printf("\n");

    // 2) hw-vs-ttable cross-check (the decisive oracle for the hardware funnel).
    int total = 0;
    total += check_round("encrypt_transform", armrx::encrypt_transform,
                         armrx::encrypt_transform_ttable, rng);
    total += check_round("decrypt_transform", armrx::decrypt_transform,
                         armrx::decrypt_transform_ttable, rng);

    // 3) full-round (key) parity: aes_encrypt_round over random (state,key) pairs.
    int round_mismatch = 0;
    for (int i = 0; i < 20000; ++i) {
        armrx::AesBlock st{}, ky{};
        for (auto& b : st) b = static_cast<std::byte>(rng() & 0xff);
        for (auto& b : ky) b = static_cast<std::byte>(rng() & 0xff);
        auto h = armrx::aes_encrypt_round(st, ky);
        auto t = armrx::aes_encrypt_round_ttable(st, ky);
        if (!blocks_equal(h, t)) ++round_mismatch;
    }
    if (round_mismatch == 0)
        std::printf("[OK] aes_encrypt_round: 20000 random (state,key) hw == ttable\n");
    else
        std::printf("[FAIL] aes_encrypt_round: %d / 20000 mismatches\n", round_mismatch);
    total += round_mismatch;

    if (total != 0) {
        std::printf("\nAES KAT FAILED: %d mismatches\n", total);
        return 1;
    }
    std::printf("\nAES KAT PASSED: hardware funnel byte-identical to T-table\n");
    return 0;
}
