#pragma once

#include <cstddef>

namespace armrx {

// Consensus parameters for the RandomX version currently used by Monero.
// Keep this separate from pool configuration: a future network upgrade must
// select a new consensus module rather than silently changing these values.
inline constexpr std::size_t kRandomXCacheBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kRandomXDatasetBytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL + 32ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kRandomXScratchpadBytes = 2ULL * 1024ULL * 1024ULL;

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
