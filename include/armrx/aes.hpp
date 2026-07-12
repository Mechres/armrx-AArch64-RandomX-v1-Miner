#pragma once

#include <array>
#include <cstddef>

namespace armrx {

using AesBlock = std::array<std::byte, 16>;

// One AES round in the byte order used by the RandomX specification. These are
// intentionally separate from AES-128: RandomX supplies independent round
// keys and never performs an AES key schedule.
[[nodiscard]] AesBlock aes_encrypt_round(const AesBlock& state, const AesBlock& round_key);
[[nodiscard]] AesBlock aes_decrypt_round(const AesBlock& state, const AesBlock& round_key);

} // namespace armrx
