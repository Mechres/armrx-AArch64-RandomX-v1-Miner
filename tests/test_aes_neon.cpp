// Regression + parity tests for the NEON vector-permute AES implementation
// (include/armrx/aes.hpp's encrypt_transform_neon/decrypt_transform_neon), added to explore
// PLAN.md Phase 5 / NEXT_STEPS.md SS5a's adopted "NEON software AES" lead. See
// docs/experiments/neon-vector-permute-aes.md for the full mathematical derivation (done independently in
// Python before any C++ was written, verifying the tower-field S-box construction against the
// standard FIPS-197 S-box for all 256 byte values, and the full round structure against this
// project's actual T-table semantics for thousands of random trials).
//
// This file is AArch64/NEON-only (the NEON functions don't exist on x86_64) and always compiles
// both the scalar and NEON code paths directly by name, independent of ARMRX_ENABLE_NEON_AES --
// that CMake flag only controls which path aes_encrypt_round/aes_decrypt_round *dispatch* to in
// production; this test bypasses the dispatch and compares both implementations head-to-head
// regardless of how the flag is set.

#include "armrx/aes.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>

namespace {

// Standard FIPS-197 S-box / inverse S-box, independently reconstructed and verified against the
// AES/Rijndael spec's own GF(2^8) definition (inverse + affine transform) in this session's
// derivation script -- not copied from this project's own randomx_aes_lut_enc/dec (which encode
// SubBytes fused with ShiftRows/MixColumns, not the standalone S-box), so this is a genuinely
// independent reference for the SubBytes-only check below.
static const std::uint8_t kReferenceSBox[256] = {
    99, 124, 119, 123, 242, 107, 111, 197, 48, 1, 103, 43, 254, 215, 171, 118,
    202, 130, 201, 125, 250, 89, 71, 240, 173, 212, 162, 175, 156, 164, 114, 192,
    183, 253, 147, 38, 54, 63, 247, 204, 52, 165, 229, 241, 113, 216, 49, 21,
    4, 199, 35, 195, 24, 150, 5, 154, 7, 18, 128, 226, 235, 39, 178, 117,
    9, 131, 44, 26, 27, 110, 90, 160, 82, 59, 214, 179, 41, 227, 47, 132,
    83, 209, 0, 237, 32, 252, 177, 91, 106, 203, 190, 57, 74, 76, 88, 207,
    208, 239, 170, 251, 67, 77, 51, 133, 69, 249, 2, 127, 80, 60, 159, 168,
    81, 163, 64, 143, 146, 157, 56, 245, 188, 182, 218, 33, 16, 255, 243, 210,
    205, 12, 19, 236, 95, 151, 68, 23, 196, 167, 126, 61, 100, 93, 25, 115,
    96, 129, 79, 220, 34, 42, 144, 136, 70, 238, 184, 20, 222, 94, 11, 219,
    224, 50, 58, 10, 73, 6, 36, 92, 194, 211, 172, 98, 145, 149, 228, 121,
    231, 200, 55, 109, 141, 213, 78, 169, 108, 86, 244, 234, 101, 122, 174, 8,
    186, 120, 37, 46, 28, 166, 180, 198, 232, 221, 116, 31, 75, 189, 139, 138,
    112, 62, 181, 102, 72, 3, 246, 14, 97, 53, 87, 185, 134, 193, 29, 158,
    225, 248, 152, 17, 105, 217, 142, 148, 155, 30, 135, 233, 206, 85, 40, 223,
    140, 161, 137, 13, 191, 230, 66, 104, 65, 153, 45, 15, 176, 84, 187, 22,
};

static const std::uint8_t kReferenceInvSBox[256] = {
    82, 9, 106, 213, 48, 54, 165, 56, 191, 64, 163, 158, 129, 243, 215, 251,
    124, 227, 57, 130, 155, 47, 255, 135, 52, 142, 67, 68, 196, 222, 233, 203,
    84, 123, 148, 50, 166, 194, 35, 61, 238, 76, 149, 11, 66, 250, 195, 78,
    8, 46, 161, 102, 40, 217, 36, 178, 118, 91, 162, 73, 109, 139, 209, 37,
    114, 248, 246, 100, 134, 104, 152, 22, 212, 164, 92, 204, 93, 101, 182, 146,
    108, 112, 72, 80, 253, 237, 185, 218, 94, 21, 70, 87, 167, 141, 157, 132,
    144, 216, 171, 0, 140, 188, 211, 10, 247, 228, 88, 5, 184, 179, 69, 6,
    208, 44, 30, 143, 202, 63, 15, 2, 193, 175, 189, 3, 1, 19, 138, 107,
    58, 145, 17, 65, 79, 103, 220, 234, 151, 242, 207, 206, 240, 180, 230, 115,
    150, 172, 116, 34, 231, 173, 53, 133, 226, 249, 55, 232, 28, 117, 223, 110,
    71, 241, 26, 113, 29, 41, 197, 137, 111, 183, 98, 14, 170, 24, 190, 27,
    252, 86, 62, 75, 198, 210, 121, 32, 154, 219, 192, 254, 120, 205, 90, 244,
    31, 221, 168, 51, 136, 7, 199, 49, 177, 18, 16, 89, 39, 128, 236, 95,
    96, 81, 127, 169, 25, 181, 74, 13, 45, 229, 122, 159, 147, 201, 156, 239,
    160, 224, 59, 77, 174, 42, 245, 176, 200, 235, 187, 60, 131, 83, 153, 97,
    23, 43, 4, 126, 186, 119, 214, 38, 225, 105, 20, 99, 85, 33, 12, 125,
};

armrx::AesBlock make_block(std::uint8_t fill) {
    armrx::AesBlock b{};
    b.fill(static_cast<std::byte>(fill));
    return b;
}

} // namespace

void test_sub_bytes_matches_reference_sbox() {
    // aes_sub_bytes_neon is fully lane-parallel/independent per byte, so broadcasting one
    // candidate value to all 16 lanes and checking every output lane against the known
    // FIPS-197 S-box value exercises the exact same computation the real (varied-per-lane)
    // usage does, for all 256 possible byte values.
    for (int b = 0; b < 256; ++b) {
        uint8x16_t in = vdupq_n_u8(static_cast<std::uint8_t>(b));
        uint8x16_t out = armrx::detail::aes_sub_bytes_neon(in);
        armrx::AesBlock block{};
        vst1q_u8(reinterpret_cast<std::uint8_t*>(block.data()), out);
        for (auto lane : block) {
            assert(static_cast<std::uint8_t>(lane) == kReferenceSBox[b]);
        }
    }
    std::cout << "[test_aes_neon] test_sub_bytes_matches_reference_sbox passed (256/256)\n";
}

void test_inv_sub_bytes_matches_reference_inv_sbox() {
    for (int b = 0; b < 256; ++b) {
        uint8x16_t in = vdupq_n_u8(static_cast<std::uint8_t>(b));
        uint8x16_t out = armrx::detail::aes_inv_sub_bytes_neon(in);
        armrx::AesBlock block{};
        vst1q_u8(reinterpret_cast<std::uint8_t*>(block.data()), out);
        for (auto lane : block) {
            assert(static_cast<std::uint8_t>(lane) == kReferenceInvSBox[b]);
        }
    }
    std::cout << "[test_aes_neon] test_inv_sub_bytes_matches_reference_inv_sbox passed (256/256)\n";
}

void test_full_round_parity_random_trials() {
    std::mt19937 rng(0xA35Bu); // fixed seed: reproducible test, not a security-sensitive use
    std::uniform_int_distribution<int> byte_dist(0, 255);

    constexpr int kTrials = 20000;
    for (int t = 0; t < kTrials; ++t) {
        armrx::AesBlock state{}, key{};
        for (auto& b : state) b = static_cast<std::byte>(byte_dist(rng));
        for (auto& b : key) b = static_cast<std::byte>(byte_dist(rng));

        // Bypass the ARMRX_ENABLE_NEON_AES-gated dispatch entirely: call each transform by
        // name directly, so this test is meaningful regardless of how that flag is set.
        armrx::AesBlock scalar_enc = armrx::encrypt_transform(state);
        for (unsigned i = 0; i < scalar_enc.size(); ++i) scalar_enc[i] ^= key[i];
        armrx::AesBlock neon_enc = armrx::encrypt_transform_neon(state);
        for (unsigned i = 0; i < neon_enc.size(); ++i) neon_enc[i] ^= key[i];
        assert(scalar_enc == neon_enc);

        armrx::AesBlock scalar_dec = armrx::decrypt_transform(state);
        for (unsigned i = 0; i < scalar_dec.size(); ++i) scalar_dec[i] ^= key[i];
        armrx::AesBlock neon_dec = armrx::decrypt_transform_neon(state);
        for (unsigned i = 0; i < neon_dec.size(); ++i) neon_dec[i] ^= key[i];
        assert(scalar_dec == neon_dec);
    }
    std::cout << "[test_aes_neon] test_full_round_parity_random_trials passed ("
              << kTrials << " trials, both directions)\n";
}

void test_edge_case_blocks() {
    // All-zero and all-0xFF blocks, plus a couple of structured patterns -- not covered by
    // uniform random sampling with meaningful probability, but easy edge cases to get wrong
    // (e.g. an off-by-one in a table's zero-index entry).
    const std::uint8_t patterns[] = {0x00, 0xFF, 0x01, 0x80, 0xAA, 0x55};
    armrx::AesBlock key{};
    for (auto& b : key) b = static_cast<std::byte>(0x00);

    for (auto p : patterns) {
        armrx::AesBlock state = make_block(p);
        armrx::AesBlock scalar_enc = armrx::encrypt_transform(state);
        armrx::AesBlock neon_enc = armrx::encrypt_transform_neon(state);
        assert(scalar_enc == neon_enc);

        armrx::AesBlock scalar_dec = armrx::decrypt_transform(state);
        armrx::AesBlock neon_dec = armrx::decrypt_transform_neon(state);
        assert(scalar_dec == neon_dec);
    }
    std::cout << "[test_aes_neon] test_edge_case_blocks passed\n";
}

int main() {
    test_sub_bytes_matches_reference_sbox();
    test_inv_sub_bytes_matches_reference_inv_sbox();
    test_edge_case_blocks();
    test_full_round_parity_random_trials();
    std::cout << "ALL AES_NEON TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
