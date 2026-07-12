#pragma once

#include "armrx/randomx_config.hpp"

#include <cstddef>
#include <cstdint>

namespace armrx {

inline constexpr std::size_t kAutoModeSafetyReserve = 256ULL * 1024ULL * 1024ULL;

struct MemorySnapshot {
    std::size_t available_bytes{};
    bool constrained_by_cgroup{};
};

struct ModeChoice {
    RandomXMode mode{RandomXMode::light};
    std::size_t required_bytes{};
};

// Returns memory the process can realistically acquire now. On Linux this uses
// MemAvailable and, when applicable, the remaining cgroup memory allowance.
[[nodiscard]] MemorySnapshot available_memory();

// Fast mode is selected only when the dataset, every worker scratchpad, and a
// safety reserve fit in currently available memory. Otherwise this always
// returns light mode; a caller may then reduce its worker count if necessary.
[[nodiscard]] constexpr ModeChoice choose_randomx_mode(
    std::size_t available_bytes, std::size_t workers,
    std::size_t safety_reserve = kAutoModeSafetyReserve) {
    const auto worker_bytes = workers * randomx_worker_memory();
    const auto fast_bytes = randomx_shared_memory(RandomXMode::fast) + worker_bytes + safety_reserve;
    if (available_bytes >= fast_bytes) {
        return {RandomXMode::fast, fast_bytes};
    }
    return {RandomXMode::light,
            randomx_shared_memory(RandomXMode::light) + worker_bytes + safety_reserve};
}

[[nodiscard]] constexpr const char* mode_name(RandomXMode mode) {
    return mode == RandomXMode::fast ? "fast" : "light";
}

} // namespace armrx
