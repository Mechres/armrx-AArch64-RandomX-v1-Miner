#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace armrx {

using Hash512 = std::array<std::byte, 64>;

// BLAKE2b-512, as required by the RandomX specification. The caller owns all
// input memory; this function keeps no global state and is safe to call from
// mining worker threads.
[[nodiscard]] Hash512 blake2b_512(std::span<const std::byte> input);

} // namespace armrx
