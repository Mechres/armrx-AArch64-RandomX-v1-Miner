#include "armrx/dataset.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace armrx {
namespace {

[[nodiscard]] constexpr std::size_t lines_per_block() {
    return sizeof(Argon2Block) / kRandomXDatasetItemBytes;
}

[[nodiscard]] std::uint64_t load_le64(const std::byte* input) {
    std::uint64_t value{};
    std::memcpy(&value, input, sizeof(value));
    return value;
}

void store_le64(std::byte* output, std::uint64_t value) {
    std::memcpy(output, &value, sizeof(value));
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
    auto registers = dataset_seed_registers(item_number);
    const auto line_count = cache_line_count(cache);
    if (line_count == 0U) {
        throw std::runtime_error{"cache has no lines"};
    }

    std::uint64_t register_value = item_number;
    for (std::size_t access = 0; access < 8U; ++access) {
        const auto line = load_cache_line(cache, static_cast<std::size_t>(register_value % line_count));
        for (std::size_t register_index = 0; register_index < registers.size(); ++register_index) {
            registers[register_index] ^= load_le64(line.data() + register_index * sizeof(std::uint64_t));
        }
        register_value = registers[(access + 1U) % registers.size()];
    }

    DatasetItem output{};
    for (std::size_t register_index = 0; register_index < registers.size(); ++register_index) {
        store_le64(output.data() + register_index * sizeof(std::uint64_t), registers[register_index]);
    }
    return output;
}

} // namespace armrx