#pragma once

#include <string>
#include <vector>

namespace armrx {

struct CpuThermalZone {
    std::string name;
    double temp_c;
};

// Reads /sys/class/thermal/thermal_zone*/{type,temp}, returning only zones
// whose type mentions "cpu" (case-insensitive) -- e.g. "cpu0-thermal" /
// "cpu4567-thermal" on multi-cluster ARM SoCs that expose one sensor per
// cluster rather than per core. Empty (not an error) on hosts without
// exposed thermal zones. Linux-only; returns empty elsewhere.
[[nodiscard]] std::vector<CpuThermalZone> read_cpu_temperatures();

// The single highest reading across all matched zones, or -1.0 if none are
// available. For a compact one-number display (console status line, TUI).
[[nodiscard]] double max_cpu_temperature();

} // namespace armrx
