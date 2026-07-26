#pragma once

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

} // namespace armrx
