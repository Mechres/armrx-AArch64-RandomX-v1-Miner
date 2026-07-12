#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace armrx {

// Argon2's variable-length hash H' (RFC 9106 §3.3). This is used for the two
// 1 KiB starting blocks of each lane and for the eventual Argon2d tag.
[[nodiscard]] std::vector<std::byte> argon2_hprime(std::span<const std::byte> input,
                                                    std::size_t output_bytes);

} // namespace armrx
