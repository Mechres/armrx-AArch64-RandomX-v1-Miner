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

} // namespace armrx
