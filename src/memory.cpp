#include "armrx/memory.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace armrx {
namespace {

[[nodiscard]] std::optional<std::uint64_t> read_number(std::string_view path) {
    std::ifstream input{std::string(path)};
    std::string value;
    if (!(input >> value) || value == "max") return std::nullopt;
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}

[[nodiscard]] std::optional<std::uint64_t> mem_available() {
    std::ifstream input{"/proc/meminfo"};
    std::string key;
    std::uint64_t kib{};
    std::string unit;
    while (input >> key >> kib >> unit) {
        if (key == "MemAvailable:") return kib * 1024ULL;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> cgroup_available(bool& limited) {
    // cgroup v2, then v1. An absent or unlimited controller is not a limit.
    if (const auto limit = read_number("/sys/fs/cgroup/memory.max")) {
        if (const auto used = read_number("/sys/fs/cgroup/memory.current"); used && *used <= *limit) {
            limited = true;
            return *limit - *used;
        }
    }
    if (const auto limit = read_number("/sys/fs/cgroup/memory/memory.limit_in_bytes")) {
        if (const auto used = read_number("/sys/fs/cgroup/memory/memory.usage_in_bytes"); used && *used <= *limit) {
            // v1 commonly reports an effectively unlimited sentinel value.
            if (*limit < (1ULL << 60U)) {
                limited = true;
                return *limit - *used;
            }
        }
    }
    return std::nullopt;
}

} // namespace

MemorySnapshot available_memory() {
    const auto host_available = mem_available().value_or(0);
    bool cgroup_limited = false;
    const auto group_available = cgroup_available(cgroup_limited);
    if (!group_available) return {static_cast<std::size_t>(host_available), false};
    const auto available = std::min(host_available, *group_available);
    return {static_cast<std::size_t>(available), cgroup_limited};
}

PartialDatasetValidation validate_partial_dataset_request(
    std::size_t dataset_mb, unsigned workers, std::size_t available_bytes) {
    PartialDatasetValidation v;
    if (dataset_mb == 0) return v; // disabled — always valid, nothing to allocate

    constexpr std::size_t kMiB = 1024ULL * 1024ULL;
    if (dataset_mb > std::numeric_limits<std::size_t>::max() / kMiB) {
        v.ok = false;
        v.error = "--dataset-mb value overflows converting MiB to bytes";
        return v;
    }
    const std::size_t dataset_bytes = dataset_mb * kMiB;
    v.dataset_bytes = dataset_bytes;
    v.item_count = dataset_bytes / kRandomXDatasetItemBytes;

    // A partial dataset is a prefix of the real RandomX dataset; it cannot be
    // larger than the thing it is a prefix of. Requests above this extent
    // used to reach initialize_dataset()'s own range check inside a
    // background fill thread (an uncaught exception there terminates the
    // process) instead of being rejected here, synchronously, before any
    // allocation.
    const std::size_t full_dataset_bytes =
        static_cast<std::size_t>(randomx_dataset_item_count()) * kRandomXDatasetItemBytes;
    if (dataset_bytes > full_dataset_bytes) {
        v.ok = false;
        v.error = "--dataset-mb=" + std::to_string(dataset_mb) + " (" +
                   std::to_string(dataset_bytes / kMiB) +
                   " MiB) exceeds the full RandomX dataset extent (" +
                   std::to_string(full_dataset_bytes / kMiB) + " MiB)";
        return v;
    }

    // Total memory budget: the 256 MiB Argon2 cache (light mode always
    // allocates one), every worker's ACTUAL 4 MiB scratchpad footprint (two
    // double-buffered 2 MiB scratchpads per worker -- see
    // mining_worker_actual_scratchpad_bytes()'s doc comment; using the
    // single-buffer randomx_worker_memory() here undercounted this by
    // 2 MiB/worker in the first version of this fix), the partial dataset
    // buffer itself, and the same fixed OS reserve choose_randomx_mode()
    // already applies to fast mode. Overflow-checked even though none of
    // these terms can realistically overflow a 64-bit size_t on their own
    // (dataset_bytes is already capped above at ~2 GiB, workers is CLI-
    // bounded to 4096) -- an explicit check costs nothing and removes the
    // assumption.
    bool overflow = false;
    std::size_t required = kRandomXCacheBytes;
    const auto add = [&](std::size_t x) {
        if (x > std::numeric_limits<std::size_t>::max() - required) {
            overflow = true;
        } else {
            required += x;
        }
    };
    add(static_cast<std::size_t>(workers) * mining_worker_actual_scratchpad_bytes());
    add(dataset_bytes);
    add(kAutoModeSafetyReserve);
    if (overflow) {
        v.ok = false;
        v.error = "--dataset-mb memory budget computation overflowed";
        return v;
    }
    v.required_bytes = required;

    if (required > available_bytes) {
        v.ok = false;
        v.error = "--dataset-mb=" + std::to_string(dataset_mb) + " needs " +
                   std::to_string(required / kMiB) +
                   " MiB total (Argon2 cache + worker scratchpads + partial "
                   "dataset + OS reserve) but only " +
                   std::to_string(available_bytes / kMiB) + " MiB is available";
        return v;
    }

    return v;
}

} // namespace armrx
