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

// Identical to blake2b() but writes the digest into a caller-supplied buffer.
// `output` must point to at least `output_bytes` bytes. Use this in hot
// per-hash paths to avoid the heap allocation performed by the vector-return
// overload. `output_bytes` must be in [1, 64].
void blake2b(std::span<const std::byte> input, std::byte* output,
             std::size_t output_bytes);

// BLAKE2b-512, as required by the RandomX specification. The caller owns all
// input memory; this function keeps no global state and is safe to call from
// mining worker threads.
[[nodiscard]] Hash512 blake2b_512(std::span<const std::byte> input);

} // namespace armrx
