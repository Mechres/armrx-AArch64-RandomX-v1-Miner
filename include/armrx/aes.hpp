#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

extern "C" {
extern const uint32_t randomx_aes_lut_enc[4][256];
extern const uint32_t randomx_aes_lut_dec[4][256];
}

namespace armrx {

using AesBlock = std::array<std::byte, 16>;

[[nodiscard]] inline AesBlock encrypt_transform(AesBlock input) {
    uint32_t s0, s1, s2, s3;
    std::memcpy(&s0, &input[0], 4);
    std::memcpy(&s1, &input[4], 4);
    std::memcpy(&s2, &input[8], 4);
    std::memcpy(&s3, &input[12], 4);

    uint32_t t0 = randomx_aes_lut_enc[0][(s0 >>  0) & 0xff] ^
                  randomx_aes_lut_enc[1][(s1 >>  8) & 0xff] ^
                  randomx_aes_lut_enc[2][(s2 >> 16) & 0xff] ^
                  randomx_aes_lut_enc[3][(s3 >> 24) & 0xff];
    uint32_t t1 = randomx_aes_lut_enc[0][(s1 >>  0) & 0xff] ^
                  randomx_aes_lut_enc[1][(s2 >>  8) & 0xff] ^
                  randomx_aes_lut_enc[2][(s3 >> 16) & 0xff] ^
                  randomx_aes_lut_enc[3][(s0 >> 24) & 0xff];
    uint32_t t2 = randomx_aes_lut_enc[0][(s2 >>  0) & 0xff] ^
                  randomx_aes_lut_enc[1][(s3 >>  8) & 0xff] ^
                  randomx_aes_lut_enc[2][(s0 >> 16) & 0xff] ^
                  randomx_aes_lut_enc[3][(s1 >> 24) & 0xff];
    uint32_t t3 = randomx_aes_lut_enc[0][(s3 >>  0) & 0xff] ^
                  randomx_aes_lut_enc[1][(s0 >>  8) & 0xff] ^
                  randomx_aes_lut_enc[2][(s1 >> 16) & 0xff] ^
                  randomx_aes_lut_enc[3][(s2 >> 24) & 0xff];

    AesBlock output;
    std::memcpy(&output[0],  &t0, 4);
    std::memcpy(&output[4],  &t1, 4);
    std::memcpy(&output[8],  &t2, 4);
    std::memcpy(&output[12], &t3, 4);
    return output;
}

[[nodiscard]] inline AesBlock decrypt_transform(AesBlock input) {
    uint32_t s0, s1, s2, s3;
    std::memcpy(&s0, &input[0], 4);
    std::memcpy(&s1, &input[4], 4);
    std::memcpy(&s2, &input[8], 4);
    std::memcpy(&s3, &input[12], 4);

    uint32_t t0 = randomx_aes_lut_dec[0][(s0 >>  0) & 0xff] ^
                  randomx_aes_lut_dec[1][(s3 >>  8) & 0xff] ^
                  randomx_aes_lut_dec[2][(s2 >> 16) & 0xff] ^
                  randomx_aes_lut_dec[3][(s1 >> 24) & 0xff];
    uint32_t t1 = randomx_aes_lut_dec[0][(s1 >>  0) & 0xff] ^
                  randomx_aes_lut_dec[1][(s0 >>  8) & 0xff] ^
                  randomx_aes_lut_dec[2][(s3 >> 16) & 0xff] ^
                  randomx_aes_lut_dec[3][(s2 >> 24) & 0xff];
    uint32_t t2 = randomx_aes_lut_dec[0][(s2 >>  0) & 0xff] ^
                  randomx_aes_lut_dec[1][(s1 >>  8) & 0xff] ^
                  randomx_aes_lut_dec[2][(s0 >> 16) & 0xff] ^
                  randomx_aes_lut_dec[3][(s3 >> 24) & 0xff];
    uint32_t t3 = randomx_aes_lut_dec[0][(s3 >>  0) & 0xff] ^
                  randomx_aes_lut_dec[1][(s2 >>  8) & 0xff] ^
                  randomx_aes_lut_dec[2][(s1 >> 16) & 0xff] ^
                  randomx_aes_lut_dec[3][(s0 >> 24) & 0xff];

    AesBlock output;
    std::memcpy(&output[0],  &t0, 4);
    std::memcpy(&output[4],  &t1, 4);
    std::memcpy(&output[8],  &t2, 4);
    std::memcpy(&output[12], &t3, 4);
    return output;
}

#if defined(__aarch64__) && defined(__ARM_NEON)

// NEON "vector-permute" AES: a from-scratch, independently-derived-and-verified
// (see docs/neon-vector-permute-aes.md) alternative to encrypt_transform/decrypt_transform's
// 256-entry-per-byte T-table lookups, which don't fit ARM NEON's 16-entry vtbl/vqtbl1q
// instructions directly. SubBytes is computed via a field isomorphism between GF(2^8) (the
// AES field) and a "tower" representation GF(2^4)^2, where every sub-step (nibble-split
// forward/backward maps, GF(2^4) inversion, GF(2^4) multiplication via log/antilog) fits a
// 16-entry vtbl. This is NOT the same as the hardware AESE/AESD instructions previously tried
// and reverted (changelogs.md 2026-07-20) -- those failed because AESE fixes AddRoundKey's
// position in the round; this is pure software computing the standard SubBytes->ShiftRows->
// MixColumns order explicitly, same as encrypt_transform/decrypt_transform already do.
namespace detail {

// General GF(2^4) multiply of two vectors (both operands vary per-lane), via log/antilog
// tables (GF(2^4)* is cyclic of order 15) with explicit zero-masking (log(0) is undefined;
// kLog4[0] is an unused sentinel, masked out below rather than relied upon).
[[nodiscard]] inline uint8x16_t aes_gf4_mul_neon(uint8x16_t a, uint8x16_t c) {
    static const uint8_t kLog4[16]     = {0, 0, 1, 4, 2, 8, 5, 10, 3, 14, 9, 7, 6, 13, 11, 12};
    static const uint8_t kAntilog4[16] = {1, 2, 4, 8, 3, 6, 12, 11, 5, 10, 7, 14, 15, 13, 9, 0};
    const uint8x16_t log_tbl = vld1q_u8(kLog4);
    const uint8x16_t antilog_tbl = vld1q_u8(kAntilog4);
    const uint8x16_t zero = vdupq_n_u8(0);
    const uint8x16_t a_zero = vceqq_u8(a, zero);
    const uint8x16_t c_zero = vceqq_u8(c, zero);
    const uint8x16_t la = vqtbl1q_u8(log_tbl, a);
    const uint8x16_t lc = vqtbl1q_u8(log_tbl, c);
    uint8x16_t s = vaddq_u8(la, lc); // 0..28
    const uint8x16_t ge15 = vcgeq_u8(s, vdupq_n_u8(15));
    s = vsubq_u8(s, vandq_u8(ge15, vdupq_n_u8(15))); // mod 15 (single conditional subtract suffices)
    const uint8x16_t result = vqtbl1q_u8(antilog_tbl, s);
    return vbicq_u8(result, vorrq_u8(a_zero, c_zero)); // force 0 where either operand was 0
}

// Nibble-split forward map (GF(2^8) byte -> tower repr, packed as hi<<4|lo) followed by
// GF(2^4)^2 inversion followed by the nibble-split backward map. Shared by both SubBytes
// (which applies the AES affine transform after this) and InvSubBytes (which applies the
// inverse affine transform before this) -- the tower-field math itself is direction-agnostic.
[[nodiscard]] inline uint8x16_t aes_tower_invert_neon(uint8x16_t byte_vec) {
    static const uint8_t kFwdHi[16]  = {0, 60, 213, 233, 52, 8, 225, 221, 229, 217, 48, 12, 209, 237, 4, 56};
    static const uint8_t kFwdLo[16]  = {0, 1, 32, 33, 70, 71, 102, 103, 76, 77, 108, 109, 10, 11, 42, 43};
    static const uint8_t kBackHi[16] = {0, 162, 2, 160, 184, 26, 186, 24, 219, 121, 217, 123, 99, 193, 97, 195};
    static const uint8_t kBackLo[16] = {0, 1, 92, 93, 224, 225, 188, 189, 80, 81, 12, 13, 176, 177, 236, 237};
    static const uint8_t kGsq4[16]   = {0, 1, 4, 5, 3, 2, 7, 6, 12, 13, 8, 9, 15, 14, 11, 10};
    static const uint8_t kGinv4[16]  = {0, 1, 9, 14, 13, 11, 7, 6, 15, 2, 12, 5, 10, 4, 3, 8};

    const uint8x16_t low_mask = vdupq_n_u8(0x0F);
    const uint8x16_t lambda = vdupq_n_u8(0x8);
    const uint8x16_t gsq4_tbl = vld1q_u8(kGsq4);

    const uint8x16_t hi = vshrq_n_u8(byte_vec, 4);
    const uint8x16_t lo = vandq_u8(byte_vec, low_mask);
    const uint8x16_t packed = veorq_u8(vqtbl1q_u8(vld1q_u8(kFwdHi), hi), vqtbl1q_u8(vld1q_u8(kFwdLo), lo));

    const uint8x16_t a = vshrq_n_u8(packed, 4);
    const uint8x16_t c = vandq_u8(packed, low_mask);

    const uint8x16_t gsq_a = vqtbl1q_u8(gsq4_tbl, a);
    const uint8x16_t gsq_a_lambda = aes_gf4_mul_neon(gsq_a, lambda);
    const uint8x16_t ac = aes_gf4_mul_neon(a, c);
    const uint8x16_t gsq_c = vqtbl1q_u8(gsq4_tbl, c);
    const uint8x16_t d = veorq_u8(veorq_u8(gsq_a_lambda, ac), gsq_c); // GF(2^4)^2 norm
    const uint8x16_t dinv = vqtbl1q_u8(vld1q_u8(kGinv4), d);
    const uint8x16_t ainv = aes_gf4_mul_neon(a, dinv);
    const uint8x16_t cinv = aes_gf4_mul_neon(veorq_u8(a, c), dinv);

    return veorq_u8(vqtbl1q_u8(vld1q_u8(kBackHi), ainv), vqtbl1q_u8(vld1q_u8(kBackLo), cinv));
}

[[nodiscard]] inline uint8x16_t aes_sub_bytes_neon(uint8x16_t s) {
    const uint8x16_t inv_std = aes_tower_invert_neon(s);
    // AES affine transform: x ^ rotl(x,1) ^ rotl(x,2) ^ rotl(x,3) ^ rotl(x,4) ^ 0x63, per byte.
    // NEON shift-immediate intrinsics need compile-time constants, so each rotation amount is
    // written out explicitly rather than looped.
    const uint8x16_t r1 = vorrq_u8(vshlq_n_u8(inv_std, 1), vshrq_n_u8(inv_std, 7));
    const uint8x16_t r2 = vorrq_u8(vshlq_n_u8(inv_std, 2), vshrq_n_u8(inv_std, 6));
    const uint8x16_t r3 = vorrq_u8(vshlq_n_u8(inv_std, 3), vshrq_n_u8(inv_std, 5));
    const uint8x16_t r4 = vorrq_u8(vshlq_n_u8(inv_std, 4), vshrq_n_u8(inv_std, 4));
    uint8x16_t out = veorq_u8(inv_std, r1);
    out = veorq_u8(out, r2);
    out = veorq_u8(out, r3);
    out = veorq_u8(out, r4);
    return veorq_u8(out, vdupq_n_u8(0x63));
}

[[nodiscard]] inline uint8x16_t aes_inv_sub_bytes_neon(uint8x16_t s) {
    static const uint8_t kAffineInvHi[16] = {0, 164, 73, 237, 146, 54, 219, 127, 37, 129, 108, 200, 183, 19, 254, 90};
    static const uint8_t kAffineInvLo[16] = {0, 74, 148, 222, 41, 99, 189, 247, 82, 24, 198, 140, 123, 49, 239, 165};
    const uint8x16_t low_mask = vdupq_n_u8(0x0F);
    // Undo the AES affine transform first: d = L^-1(s ^ 0x63).
    const uint8x16_t c_xor = veorq_u8(s, vdupq_n_u8(0x63));
    const uint8x16_t hi = vshrq_n_u8(c_xor, 4);
    const uint8x16_t lo = vandq_u8(c_xor, low_mask);
    const uint8x16_t d0 = veorq_u8(vqtbl1q_u8(vld1q_u8(kAffineInvHi), hi), vqtbl1q_u8(vld1q_u8(kAffineInvLo), lo));
    return aes_tower_invert_neon(d0);
}

[[nodiscard]] inline uint8x16_t aes_shift_rows_neon(uint8x16_t s) {
    static const uint8_t kShiftRows[16] = {0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11};
    return vqtbl1q_u8(s, vld1q_u8(kShiftRows));
}

[[nodiscard]] inline uint8x16_t aes_inv_shift_rows_neon(uint8x16_t s) {
    static const uint8_t kInvShiftRows[16] = {0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3};
    return vqtbl1q_u8(s, vld1q_u8(kInvShiftRows));
}

[[nodiscard]] inline uint8x16_t aes_xtime_neon(uint8x16_t v) {
    const uint8x16_t hi_mask = vcgeq_u8(v, vdupq_n_u8(0x80)); // all-1s where MSB set
    const uint8x16_t shifted = vshlq_n_u8(v, 1);
    const uint8x16_t reduction = vandq_u8(hi_mask, vdupq_n_u8(0x1B));
    return veorq_u8(shifted, reduction);
}

[[nodiscard]] inline uint8x16_t aes_mix_columns_neon(uint8x16_t s) {
    static const uint8_t kRot1[16] = {1, 2, 3, 0, 5, 6, 7, 4, 9, 10, 11, 8, 13, 14, 15, 12};
    static const uint8_t kRot2[16] = {2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13};
    static const uint8_t kRot3[16] = {3, 0, 1, 2, 7, 4, 5, 6, 11, 8, 9, 10, 15, 12, 13, 14};
    const uint8x16_t rot1 = vqtbl1q_u8(s, vld1q_u8(kRot1));
    const uint8x16_t rot2 = vqtbl1q_u8(s, vld1q_u8(kRot2));
    const uint8x16_t rot3 = vqtbl1q_u8(s, vld1q_u8(kRot3));
    const uint8x16_t d = veorq_u8(veorq_u8(s, rot1), veorq_u8(rot2, rot3)); // a0^a1^a2^a3 per column
    const uint8x16_t t = aes_xtime_neon(veorq_u8(s, rot1));                // xtime(a_i ^ a_{i+1})
    return veorq_u8(veorq_u8(s, d), t);
}

[[nodiscard]] inline uint8x16_t aes_inv_mix_columns_neon(uint8x16_t s) {
    static const uint8_t kRot2[16] = {2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13};
    const uint8x16_t rot2 = vqtbl1q_u8(s, vld1q_u8(kRot2));
    const uint8x16_t xor02_13 = veorq_u8(s, rot2); // pos0,2 hold a0^a2; pos1,3 hold a1^a3
    const uint8x16_t uv = aes_xtime_neon(aes_xtime_neon(xor02_13));
    const uint8x16_t adjusted = veorq_u8(s, uv);
    return aes_mix_columns_neon(adjusted); // standard trick: InvMixColumns via forward MixColumns
}

} // namespace detail

[[nodiscard]] inline AesBlock encrypt_transform_neon(AesBlock input) {
    uint8x16_t s = vld1q_u8(reinterpret_cast<const uint8_t*>(input.data()));
    s = detail::aes_sub_bytes_neon(s);
    s = detail::aes_shift_rows_neon(s);
    s = detail::aes_mix_columns_neon(s);
    AesBlock output;
    vst1q_u8(reinterpret_cast<uint8_t*>(output.data()), s);
    return output;
}

[[nodiscard]] inline AesBlock decrypt_transform_neon(AesBlock input) {
    uint8x16_t s = vld1q_u8(reinterpret_cast<const uint8_t*>(input.data()));
    s = detail::aes_inv_shift_rows_neon(s);
    s = detail::aes_inv_sub_bytes_neon(s);
    s = detail::aes_inv_mix_columns_neon(s);
    AesBlock output;
    vst1q_u8(reinterpret_cast<uint8_t*>(output.data()), s);
    return output;
}

#endif // __aarch64__ && __ARM_NEON

[[nodiscard]] inline AesBlock aes_encrypt_round(AesBlock state, AesBlock round_key) {
#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_AES)
    auto output = encrypt_transform_neon(state);
#else
    auto output = encrypt_transform(state);
#endif
    for (unsigned i = 0; i < output.size(); ++i)
        output[i] ^= round_key[i];
    return output;
}

[[nodiscard]] inline AesBlock aes_decrypt_round(AesBlock state, AesBlock round_key) {
#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_AES)
    auto output = decrypt_transform_neon(state);
#else
    auto output = decrypt_transform(state);
#endif
    for (unsigned i = 0; i < output.size(); ++i)
        output[i] ^= round_key[i];
    return output;
}

} // namespace armrx
