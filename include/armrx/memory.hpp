#pragma once

#include "armrx/dataset.hpp"
#include "armrx/randomx_config.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

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

// The actual per-worker scratchpad footprint the mining engine allocates.
// worker_loop() (mining_engine.cpp) keeps TWO kRandomXScratchpadBytes
// buffers per worker (`dual_scratchpad`, sized `kScratchpadSize * 2`) for the
// double-buffered pipelined hash+fill design (Track D2) -- unconditionally,
// in both light and fast mode. randomx_worker_memory() intentionally still
// returns the single-buffer figure: it feeds choose_randomx_mode()'s
// pre-existing fast-mode threshold decision, which is unrelated to this fix
// and left unchanged here to avoid an unrelated behavior change (a different
// fast/light threshold needs its own measurement, per this project's
// performance-change discipline). validate_partial_dataset_request() uses
// this constant instead so its own memory budget reflects what the engine
// actually allocates (audit follow-up, round 2: the first version of that
// function undercounted this by 2 MiB/worker).
[[nodiscard]] constexpr std::size_t mining_worker_actual_scratchpad_bytes() {
    return 2ULL * randomx_worker_memory();
}

/// Result of validating a requested light-mode partial-dataset (`--dataset-mb`)
/// size against both the RandomX dataset's real extent and the process's
/// total memory budget (Argon2 cache + per-worker scratchpads + the partial
/// dataset itself + a fixed OS reserve). `ok == false` means the request must
/// be rejected before any allocation/prefault is attempted; `error` then
/// holds a human-readable reason. `item_count`/`dataset_bytes`/`required_bytes`
/// are always populated (even when rejected) for diagnostics.
struct PartialDatasetValidation {
    bool ok = true;
    std::string error;
    std::size_t item_count = 0;
    std::size_t dataset_bytes = 0;
    std::size_t required_bytes = 0;
};

/// Validates a `--dataset-mb=N` request (0 = disabled, always valid) against:
///   1. arithmetic overflow converting MiB -> bytes -> item count,
///   2. the actual RandomX dataset extent (a partial prefix cannot exceed the
///      whole dataset it is a prefix of),
///   3. the total memory budget available right now, including the 256 MiB
///      Argon2 cache, every worker's actual 4 MiB scratchpad footprint
///      (two double-buffered 2 MiB scratchpads -- see
///      mining_worker_actual_scratchpad_bytes(), not the single-buffer
///      randomx_worker_memory() choose_randomx_mode() uses for its own,
///      unrelated fast-mode threshold decision), the partial dataset
///      buffer itself, and the fixed OS safety reserve (kAutoModeSafetyReserve,
///      the same constant choose_randomx_mode() applies to fast mode).
/// Must be called (and its `ok` checked) before constructing a PartialDataset
/// or otherwise mmap'ing/prefaulting the requested size.
[[nodiscard]] PartialDatasetValidation validate_partial_dataset_request(
    std::size_t dataset_mb, unsigned workers, std::size_t available_bytes);

} // namespace armrx
