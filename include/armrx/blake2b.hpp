#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace armrx {

using Hash512 = std::array<std::byte, 64>;

// Unkeyed BLAKE2b with an output length from 1 through 64 bytes. Argon2's H'
// construction needs variable-length BLAKE2b outputs.
[[nodiscard]] std::vector<std::byte> blake2b(std::span<const std::byte> input,
                                             std::size_t output_bytes);

// BLAKE2b-512, as required by the RandomX specification. The caller owns all
// input memory; this function keeps no global state and is safe to call from
// mining worker threads.
[[nodiscard]] Hash512 blake2b_512(std::span<const std::byte> input);

} // namespace armrx
