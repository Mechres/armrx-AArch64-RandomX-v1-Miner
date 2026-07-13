#pragma once

#include "armrx/aes.hpp"
#include <span>
#include <cstddef>

namespace armrx {

using AesState = std::array<std::byte, 64>;

// Fills output buffer with pseudorandom data based on state using 1-round AES in 4 lanes
void fill_aes_1r_x4(AesState& state, std::span<std::byte> output);

// Fills output buffer with pseudorandom data based on state using 4-round AES in 4 lanes
void fill_aes_4r_x4(AesState& state, std::span<std::byte> output);

// Hashes inputs using 1-round AES in 4 lanes
void hash_aes_1r_x4(std::span<const std::byte> input, AesState& hash);

// Combines hash and fill operations on the scratchpad
void hash_and_fill_aes_1r_x4(std::span<std::byte> scratchpad, AesState& hash, AesState& fill_state);

} // namespace armrx
