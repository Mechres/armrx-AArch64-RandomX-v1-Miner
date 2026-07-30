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

/// Interleaved hash+fill across two different scratchpads (Track D2).
/// Reads blocks from `hash_scratchpad` (current hash's data) and writes
/// fill output to `fill_scratchpad` (next hash's scratchpad) in lockstep.
/// Produces the same result as calling hash_aes_1r_x4 + fill_aes_1r_x4 separately.
/// Both spans must be the same size and 64-byte aligned.
void hash_and_fill_aes_interleaved_x4(
    std::span<const std::byte> hash_scratchpad,
    std::span<std::byte> fill_scratchpad,
    AesState& hash_state,
    AesState& fill_state
);

} // namespace armrx
