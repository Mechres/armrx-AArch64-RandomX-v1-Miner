#include "armrx/aes.hpp"

#include <cstdint>

namespace armrx {
namespace {

[[nodiscard]] constexpr std::uint8_t value(std::byte byte) {
  return static_cast<std::uint8_t>(byte);
}

[[nodiscard]] constexpr std::byte byte(std::uint8_t number) {
  return static_cast<std::byte>(number);
}

[[nodiscard]] constexpr std::uint8_t rotate_left(std::uint8_t input,
                                                 unsigned count) {
  return static_cast<std::uint8_t>((input << count) | (input >> (8U - count)));
}

[[nodiscard]] constexpr std::uint8_t gf_multiply(std::uint8_t left,
                                                 std::uint8_t right) {
  std::uint8_t result{};
  for (unsigned bit = 0; bit < 8; ++bit) {
    if ((right & 1U) != 0U)
      result ^= left;
    const bool high_bit = (left & 0x80U) != 0U;
    left = static_cast<std::uint8_t>(left << 1U);
    if (high_bit)
      left ^= 0x1bU;
    right = static_cast<std::uint8_t>(right >> 1U);
  }
  return result;
}

[[nodiscard]] constexpr std::uint8_t gf_inverse(std::uint8_t input) {
  if (input == 0U)
    return 0;
  std::uint8_t result = 1;
  std::uint8_t base = input;
  unsigned exponent = 254;
  while (exponent != 0U) {
    if ((exponent & 1U) != 0U)
      result = gf_multiply(result, base);
    base = gf_multiply(base, base);
    exponent >>= 1U;
  }
  return result;
}

[[nodiscard]] constexpr std::uint8_t sbox(std::uint8_t input) {
  const auto inverse = gf_inverse(input);
  return static_cast<std::uint8_t>(
      inverse ^ rotate_left(inverse, 1) ^ rotate_left(inverse, 2) ^
      rotate_left(inverse, 3) ^ rotate_left(inverse, 4) ^ 0x63U);
}

[[nodiscard]] constexpr std::uint8_t inverse_sbox(std::uint8_t input) {
  const auto affine_inverse =
      static_cast<std::uint8_t>(rotate_left(input, 1) ^ rotate_left(input, 3) ^
                                rotate_left(input, 6) ^ 0x05U);
  return gf_inverse(affine_inverse);
}

[[nodiscard]] AesBlock encrypt_transform(const AesBlock &input) {
  AesBlock output{};
  // AES state is column-major: index = row + 4 * column.
  for (unsigned row = 0; row < 4; ++row) {
    for (unsigned column = 0; column < 4; ++column) {
      output[row + 4U * column] =
          byte(sbox(value(input[row + 4U * ((column + row) % 4U)])));
    }
  }
  for (unsigned column = 0; column < 4; ++column) {
    const auto base = 4U * column;
    const auto a0 = value(output[base]);
    const auto a1 = value(output[base + 1U]);
    const auto a2 = value(output[base + 2U]);
    const auto a3 = value(output[base + 3U]);
    output[base] = byte(gf_multiply(a0, 2) ^ gf_multiply(a1, 3) ^ a2 ^ a3);
    output[base + 1U] = byte(a0 ^ gf_multiply(a1, 2) ^ gf_multiply(a2, 3) ^ a3);
    output[base + 2U] = byte(a0 ^ a1 ^ gf_multiply(a2, 2) ^ gf_multiply(a3, 3));
    output[base + 3U] = byte(gf_multiply(a0, 3) ^ a1 ^ a2 ^ gf_multiply(a3, 2));
  }
  return output;
}

[[nodiscard]] AesBlock decrypt_transform(const AesBlock &input) {
  AesBlock shifted{};
  for (unsigned row = 0; row < 4; ++row) {
    for (unsigned column = 0; column < 4; ++column) {
      shifted[row + 4U * column] = byte(
          inverse_sbox(value(input[row + 4U * ((column + 4U - row) % 4U)])));
    }
  }
  AesBlock output{};
  for (unsigned column = 0; column < 4; ++column) {
    const auto base = 4U * column;
    const auto a0 = value(shifted[base]);
    const auto a1 = value(shifted[base + 1U]);
    const auto a2 = value(shifted[base + 2U]);
    const auto a3 = value(shifted[base + 3U]);
    output[base] = byte(gf_multiply(a0, 14) ^ gf_multiply(a1, 11) ^
                        gf_multiply(a2, 13) ^ gf_multiply(a3, 9));
    output[base + 1U] = byte(gf_multiply(a0, 9) ^ gf_multiply(a1, 14) ^
                             gf_multiply(a2, 11) ^ gf_multiply(a3, 13));
    output[base + 2U] = byte(gf_multiply(a0, 13) ^ gf_multiply(a1, 9) ^
                             gf_multiply(a2, 14) ^ gf_multiply(a3, 11));
    output[base + 3U] = byte(gf_multiply(a0, 11) ^ gf_multiply(a1, 13) ^
                             gf_multiply(a2, 9) ^ gf_multiply(a3, 14));
  }
  return output;
}

} // namespace
c AesBlock aes_encrypt_round(const AesBlock &state, const AesBlock &round_key) {
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
