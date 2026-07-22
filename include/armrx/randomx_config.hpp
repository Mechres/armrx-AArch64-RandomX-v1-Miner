#pragma once

#include <cstddef>
#include <cstdint>

namespace armrx {

// Consensus parameters for the RandomX version currently used by Monero.
// Keep this separate from pool configuration: a future network upgrade must
// select a new consensus module rather than silently changing these values.
inline constexpr std::size_t kRandomXCacheBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kRandomXDatasetBytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL + 32ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kRandomXScratchpadL1Bytes = 16ULL * 1024ULL;
inline constexpr std::size_t kRandomXScratchpadL2Bytes = 256ULL * 1024ULL;
inline constexpr std::size_t kRandomXScratchpadBytes = 2ULL * 1024ULL * 1024ULL; // L3

inline constexpr std::size_t kRandomXCacheAccesses = 8U;
inline constexpr std::size_t kSuperscalarLatency = 170U;
inline constexpr std::size_t kSuperscalarMaxSize = 3U * kSuperscalarLatency + 2U;

// Scratchpad address-wrap masks (specification §4.1's "wrap around" behavior).
// Single source of truth — previously re-derived independently in both
// vm.cpp (the interpreter) and jit_compiler_a64.cpp (as a hardcoded literal
// with a "(RANDOMX_SCRATCHPAD_L3 / 8 - 1) * 8" comment repeating this exact
// formula in prose), which is exactly the kind of drift-prone duplication
// this header exists to close.
[[nodiscard]] constexpr std::uint32_t scratchpad_mask(std::size_t bytes, std::size_t alignment) {
    return static_cast<std::uint32_t>((bytes / alignment - 1U) * alignment);
}

inline constexpr std::uint32_t kScratchpadL1Mask   = scratchpad_mask(kRandomXScratchpadL1Bytes, 8);
inline constexpr std::uint32_t kScratchpadL2Mask   = scratchpad_mask(kRandomXScratchpadL2Bytes, 8);
inline constexpr std::uint32_t kScratchpadL3Mask   = scratchpad_mask(kRandomXScratchpadBytes, 8);
inline constexpr std::uint32_t kScratchpadL3Mask64 = scratchpad_mask(kRandomXScratchpadBytes, 64);

enum class RandomXMode {
    fast,
    light,
};

// Cache memory is shared by all light-mode workers. Scratchpad memory is per
// worker. Fast mode additionally needs a shared, fully initialized dataset.
[[nodiscard]] constexpr std::size_t randomx_shared_memory(RandomXMode mode) {
    return mode == RandomXMode::fast ? kRandomXDatasetBytes : kRandomXCacheBytes;
}

[[nodiscard]] constexpr std::size_t randomx_worker_memory() {
    return kRandomXScratchpadBytes;
}

} // namespace armrx
