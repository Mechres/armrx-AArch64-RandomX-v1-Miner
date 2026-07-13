#include "armrx/blake2_generator.hpp"
#include "armrx/blake2b.hpp"

#include <bit>
#include <cstring>
#include <span>

namespace armrx {
namespace {

constexpr std::size_t kMaxSeedSize = 60U;

void store32_le(std::byte* output, std::uint32_t value) {
    if constexpr (std::endian::native == std::endian::big) {
        value = ((value & 0x000000ffU) << 24) |
                ((value & 0x0000ff00U) << 8)  |
                ((value & 0x00ff0000U) >> 8)  |
                ((value & 0xff000000U) >> 24);
    }
    std::memcpy(output, &value, sizeof(value));
}

std::uint32_t load32_le(const std::byte* input) {
    std::uint32_t value{};
    std::memcpy(&value, input, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        value = ((value & 0x000000ffU) << 24) |
                ((value & 0x0000ff00U) << 8)  |
                ((value & 0x00ff0000U) >> 8)  |
                ((value & 0xff000000U) >> 24);
    }
    return value;
}

} // namespace

Blake2Generator::Blake2Generator(const void* seed, std::size_t seed_size, int nonce)
    : data_index_(data_.size()) {
    std::memset(data_.data(), 0, data_.size());
    std::memcpy(data_.data(), seed, seed_size > kMaxSeedSize ? kMaxSeedSize : seed_size);
    store32_le(reinterpret_cast<std::byte*>(data_.data()) + kMaxSeedSize, static_cast<std::uint32_t>(nonce));
}

std::uint8_t Blake2Generator::get_byte() {
    check_data(1);
    return static_cast<std::uint8_t>(data_[data_index_++]);
}

std::uint32_t Blake2Generator::get_uint32() {
    check_data(4);
    auto ret = load32_le(reinterpret_cast<const std::byte*>(data_.data()) + data_index_);
    data_index_ += 4;
    return ret;
}

void Blake2Generator::check_data(std::size_t bytes_needed) {
    if (data_index_ + bytes_needed > data_.size()) {
        const auto digest = blake2b(std::span<const std::byte>{data_.data(), data_.size()}, 64);
        std::memcpy(data_.data(), digest.data(), 64);
        data_index_ = 0;
    }
}

} // namespace armrx
