#include "armrx/cpu_thermal.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#if defined(__linux__)
#include <dirent.h>
#endif

namespace armrx {

#if defined(__linux__)

namespace {
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

std::vector<CpuThermalZone> read_cpu_temperatures() {
    std::vector<CpuThermalZone> zones;
    DIR* dir = ::opendir("/sys/class/thermal");
    if (!dir) return zones;
    while (auto* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("thermal_zone", 0) != 0) continue;
        const std::string base = "/sys/class/thermal/" + name;

        std::ifstream type_file(base + "/type");
        std::string type;
        if (!type_file || !std::getline(type_file, type)) continue;
        if (to_lower(type).find("cpu") == std::string::npos) continue;

        std::ifstream temp_file(base + "/temp");
        long milli_c = 0;
        if (!temp_file || !(temp_file >> milli_c)) continue;

        zones.push_back({type, static_cast<double>(milli_c) / 1000.0});
    }
    ::closedir(dir);
    std::sort(zones.begin(), zones.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return zones;
}

#else

std::vector<CpuThermalZone> read_cpu_temperatures() { return {}; }

#endif

double max_cpu_temperature() {
    const auto zones = read_cpu_temperatures();
    if (zones.empty()) return -1.0;
    double max_temp = zones.front().temp_c;
    for (const auto& z : zones) max_temp = std::max(max_temp, z.temp_c);
    return max_temp;
}

} // namespace armrx
