#include "armrx/aes.hpp"

#include <cstdint>
#include <cstring>

// Global-scope AES T-tables from soft_aes.cpp
extern "C" const uint32_t randomx_aes_lut_enc[4][256];
extern "C" const uint32_t randomx_aes_lut_dec[4][256];

namespace armrx {
namespace {

[[nodiscard]] AesBlock encrypt_transform(const AesBlock &input) {
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

[[nodiscard]] AesBlock decrypt_transform(const AesBlock &input) {
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

} // namespace
AesBlock aes_encrypt_round(const AesBlock &state, const AesBlock &round_key) {
  auto output = encrypt_transform(state);
  for (unsigned i = 0; i < output.size(); ++i)
    output[i] ^= round_key[i];
  return output;
}

AesBlock aes_decrypt_round(const AesBlock &state, const AesBlock &round_key) {
  auto output = decrypt_transform(state);
  for (unsigned i = 0; i < output.size(); ++i)
    output[i] ^= round_key[i];
  return output;
}

} // namespace armrx
