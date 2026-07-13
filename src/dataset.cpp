#include "armrx/dataset.hpp"
#include "armrx/superscalar.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace armrx {
namespace {

[[nodiscard]] constexpr std::size_t lines_per_block() {
    return sizeof(Argon2Block) / kRandomXDatasetItemBytes;
}

} // namespace

DatasetItem load_cache_line(const Argon2dCache& cache, std::size_t line_index) {
    const auto line_count = cache_line_count(cache);
    if (line_count == 0U) {
        throw std::runtime_error{"cache has no lines"};
    }
    line_index %= line_count;
    const auto block_index = line_index / lines_per_block();
    const auto line_in_block = line_index % lines_per_block();
    DatasetItem line{};
    const auto& block = cache.blocks()[block_index];
    std::memcpy(line.data(), reinterpret_cast<const std::byte*>(block.data()) +
                              line_in_block * kRandomXDatasetItemBytes,
                kRandomXDatasetItemBytes);
    return line;
}

DatasetItem generate_dataset_item(const Argon2dCache& cache, std::uint64_t item_number) {
    const auto line_count = cache_line_count(cache);
    if (line_count == 0U) {
        throw std::runtime_error{"cache has no lines"};
    }

    auto rl = dataset_seed_registers(item_number);
    std::uint64_t register_value = item_number;

    for (std::size_t i = 0; i < kRandomXCacheAccesses; ++i) {
        const std::size_t line_index = static_cast<std::size_t>(register_value % line_count);
        const auto line = load_cache_line(cache, line_index);
        const auto& prog = cache.programs()[i];

        execute_superscalar(rl, prog, &cache.reciprocal_cache());

        for (std::size_t q = 0; q < 8; ++q) {
            std::uint64_t val{};
            std::memcpy(&val, line.data() + q * 8, sizeof(val));
            rl[q] ^= val;
        }

        register_value = rl[static_cast<std::size_t>(prog.address_register())];
    }

    DatasetItem output{};
    for (std::size_t q = 0; q < 8; ++q) {
        std::memcpy(output.data() + q * 8, &rl[q], sizeof(rl[q]));
    }
    return output;
}

void initialize_dataset(std::span<std::byte> output, const Argon2dCache& cache,
                         std::uint64_t start_item, std::uint64_t item_count) {
    if (!dataset_range_is_valid(start_item, item_count)) {
        throw std::invalid_argument{"dataset item range is invalid"};
    }

    if (item_count > (std::numeric_limits<std::size_t>::max() / kRandomXDatasetItemBytes)) {
        throw std::invalid_argument{"dataset item count is too large"};
    }

    const auto required_bytes = dataset_output_bytes(item_count);
    if (output.size() < required_bytes) {
        throw std::invalid_argument{"dataset output buffer is too small"};
    }

#ifdef __aarch64__
    const auto line_count = cache_line_count(cache);
    if (line_count == 0U) {
        throw std::runtime_error{"cache has no lines"};
    }

    std::uint64_t offset = 0;
    for (; offset + 1 < item_count; offset += 2) {
        std::uint64_t item0 = start_item + offset;
        std::uint64_t item1 = start_item + offset + 1;

        auto rl0 = dataset_seed_registers(item0);
        auto rl1 = dataset_seed_registers(item1);

        uint64x2_t vr[8];
        for (std::size_t q = 0; q < 8; ++q) {
            vr[q] = vcombine_u64(vcreate_u64(rl0[q]), vcreate_u64(rl1[q]));
        }

        std::uint64_t reg_val0 = item0;
        std::uint64_t reg_val1 = item1;

        for (std::size_t p = 0; p < kRandomXCacheAccesses; ++p) {
            const std::size_t line_index0 = static_cast<std::size_t>(reg_val0 % line_count);
            const std::size_t line_index1 = static_cast<std::size_t>(reg_val1 % line_count);

            const auto line0 = load_cache_line(cache, line_index0);
            const auto line1 = load_cache_line(cache, line_index1);

            const auto& prog = cache.programs()[p];

            // Execute vectorized Superscalar program
            execute_superscalar_neon(vr, prog, &cache.reciprocal_cache());

            // XOR with cache line bytes
            for (std::size_t q = 0; q < 8; ++q) {
                std::uint64_t val0{}, val1{};
                std::memcpy(&val0, line0.data() + q * 8, 8);
                std::memcpy(&val1, line1.data() + q * 8, 8);
                uint64x2_t v_val = vcombine_u64(vcreate_u64(val0), vcreate_u64(val1));
                vr[q] = veorq_u64(vr[q], v_val);
            }

            reg_val0 = vgetq_lane_u64(vr[prog.address_register()], 0);
            reg_val1 = vgetq_lane_u64(vr[prog.address_register()], 1);
        }

        // Store outputs
        for (std::size_t q = 0; q < 8; ++q) {
            std::uint64_t out0 = vgetq_lane_u64(vr[q], 0);
            std::uint64_t out1 = vgetq_lane_u64(vr[q], 1);
            std::memcpy(output.data() + offset * kRandomXDatasetItemBytes + q * 8, &out0, 8);
            std::memcpy(output.data() + (offset + 1) * kRandomXDatasetItemBytes + q * 8, &out1, 8);
        }
    }

    // Leftover
    for (; offset < item_count; ++offset) {
        const auto item = generate_dataset_item(cache, start_item + offset);
        std::memcpy(output.data() + offset * kRandomXDatasetItemBytes, item.data(),
                    kRandomXDatasetItemBytes);
    }
#else
    for (std::uint64_t offset = 0; offset < item_count; ++offset) {
        const auto item = generate_dataset_item(cache, start_item + offset);
        std::memcpy(output.data() + offset * kRandomXDatasetItemBytes, item.data(),
                    kRandomXDatasetItemBytes);
    }
#endif
}

} // namespace armrx