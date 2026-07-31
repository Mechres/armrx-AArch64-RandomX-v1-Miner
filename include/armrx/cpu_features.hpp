#pragma once

#include <vector>

namespace armrx {

struct CpuFeatures {
    bool aarch64{};
    bool neon{};
    bool aes{};
    bool crc32{};
};

[[nodiscard]] CpuFeatures detect_cpu_features();

// True online-CPU count, read from /sys/devices/system/cpu/online (e.g.
// "0-7" or "0-3,6-7"). Unlike std::thread::hardware_concurrency(), this is
// unaffected by the calling process's own affinity mask -- musl's
// hardware_concurrency() is implemented via sched_getaffinity(), so under
// isolcpus (which restricts new processes' default affinity to the
// non-isolated cores) it silently returns 1 instead of the true core count.
// Falls back to hardware_concurrency() if the sysfs file is unavailable or
// unparsable.
[[nodiscard]] unsigned int online_cpu_count();

// Returns the set of CPUs isolated via isolcpus kernel parameter.
// Returns empty vector if isolcpus is not in use or sysfs is unavailable.
std::vector<unsigned int> isolated_cpu_list();

} // namespace armrx
