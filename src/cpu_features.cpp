#include "armrx/cpu_features.hpp"

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

} // namespace armrx
