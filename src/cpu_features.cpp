#include "armrx/cpu_features.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

namespace armrx {

CpuFeatures detect_cpu_features() {
    CpuFeatures result{};
#if defined(__aarch64__)
    result.aarch64 = true;
    result.neon = true; // Advanced SIMD is mandatory in AArch64.
#if defined(__linux__)
    const auto hwcap = getauxval(AT_HWCAP);
    result.aes = (hwcap & HWCAP_AES) != 0;
    result.crc32 = (hwcap & HWCAP_CRC32) != 0;
#endif
#endif
    return result;
}

unsigned int online_cpu_count() {
    std::ifstream file("/sys/devices/system/cpu/online");
    std::string contents;
    if (file && std::getline(file, contents) && !contents.empty()) {
        unsigned int total = 0;
        std::stringstream ss(contents);
        std::string token;
        while (std::getline(ss, token, ',')) {
            const auto dash = token.find('-');
            try {
                if (dash == std::string::npos) {
                    total += 1;
                } else {
                    const unsigned int lo = std::stoul(token.substr(0, dash));
                    const unsigned int hi = std::stoul(token.substr(dash + 1));
                    if (hi >= lo) total += (hi - lo + 1);
                }
            } catch (const std::exception&) {
                total = 0;
                break;
            }
        }
        if (total > 0) return total;
    }
    return std::max(1U, std::thread::hardware_concurrency());
}

} // namespace armrx
