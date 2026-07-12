#pragma once

namespace armrx {

struct CpuFeatures {
    bool aarch64{};
    bool neon{};
    bool aes{};
    bool crc32{};
};

[[nodiscard]] CpuFeatures detect_cpu_features();

} // namespace armrx
