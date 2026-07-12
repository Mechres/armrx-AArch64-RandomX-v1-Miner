#pragma once

#include "armrx/argon2.hpp"
#include "armrx/randomx_config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace armrx {

inline constexpr std::uint64_t kRandomXDatasetItemBytes = 64ULL;

using DatasetRegisters = std::array<std::uint64_t, 8>;
using DatasetItem = std::array<std::byte, kRandomXDatasetItemBytes>;

[[nodiscard]] constexpr std::size_t randomx_dataset_item_count() {
    return kRandomXDatasetBytes / kRandomXDatasetItemBytes;
}

[[nodiscard]] constexpr DatasetRegisters dataset_seed_registers(std::uint64_t item_number) {
    constexpr std::uint64_t kDatasetSeedMultiplier = 6364136223846793005ULL;
    constexpr std::uint64_t kDatasetSeedAdd1 = 9298411001130361340ULL;
    constexpr std::uint64_t kDatasetSeedAdd2 = 12065312585734608966ULL;
    constexpr std::uint64_t kDatasetSeedAdd3 = 9306329213124626780ULL;
    constexpr std::uint64_t kDatasetSeedAdd4 = 5281919268842080866ULL;
    constexpr std::uint64_t kDatasetSeedAdd5 = 10536153434571861004ULL;
    constexpr std::uint64_t kDatasetSeedAdd6 = 3398623926847679864ULL;
    constexpr std::uint64_t kDatasetSeedAdd7 = 9549104520008361294ULL;

    const auto seed = (item_number + 1ULL) * kDatasetSeedMultiplier;
    return {
        seed,
        seed ^ kDatasetSeedAdd1,
        seed ^ kDatasetSeedAdd2,
        seed ^ kDatasetSeedAdd3,
        seed ^ kDatasetSeedAdd4,
        seed ^ kDatasetSeedAdd5,
        seed ^ kDatasetSeedAdd6,
        seed ^ kDatasetSeedAdd7,
    };
}

[[nodiscard]] inline std::size_t cache_line_count(const Argon2dCache& cache) {
    return cache.blocks().size() * (sizeof(Argon2Block) / kRandomXDatasetItemBytes);
}

[[nodiscard]] DatasetItem load_cache_line(const Argon2dCache& cache, std::size_t line_index);

[[nodiscard]] DatasetItem generate_dataset_item(const Argon2dCache& cache,
                                                std::uint64_t item_number);

void initialize_dataset(std::span<std::byte> output, const Argon2dCache& cache,
                        std::uint64_t start_item, std::uint64_t item_count);

} // namespace armrx