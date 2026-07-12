#include "armrx/aes_generator.hpp"

#include <algorithm>
#include <stdexcept>

namespace armrx {
namespace {

constexpr AesBlock key0{
    std::byte{0x53}, std::byte{0xa5}, std::byte{0xac}, std::byte{0x6d}, std::byte{0x09}, std::byte{0x66}, std::byte{0x71}, std::byte{0x62},
    std::byte{0x2b}, std::byte{0x55}, std::byte{0xb5}, std::byte{0xdb}, std::byte{0x17}, std::byte{0x49}, std::byte{0xf4}, std::byte{0xb4}};
constexpr AesBlock key1{
    std::byte{0x07}, std::byte{0xaf}, std::byte{0x7c}, std::byte{0x6d}, std::byte{0x0d}, std::byte{0x71}, std::byte{0x6a}, std::byte{0x84},
    std::byte{0x78}, std::byte{0xd3}, std::byte{0x25}, std::byte{0x17}, std::byte{0x4e}, std::byte{0xdc}, std::byte{0xa1}, std::byte{0x0d}};
constexpr AesBlock key2{
    std::byte{0xf1}, std::byte{0x62}, std::byte{0x12}, std::byte{0x3f}, std::byte{0xc6}, std::byte{0x7e}, std::byte{0x94}, std::byte{0x9f},
    std::byte{0x4f}, std::byte{0x79}, std::byte{0xc0}, std::byte{0xf4}, std::byte{0x45}, std::byte{0xe3}, std::byte{0x20}, std::byte{0x3e}};
constexpr AesBlock key3{
    std::byte{0x35}, std::byte{0x81}, std::byte{0xef}, std::byte{0x6a}, std::byte{0x7c}, std::byte{0x31}, std::byte{0xba}, std::byte{0xb1},
    std::byte{0x88}, std::byte{0x4c}, std::byte{0x31}, std::byte{0x16}, std::byte{0x54}, std::byte{0x91}, std::byte{0x16}, std::byte{0x49}};

[[nodiscard]] AesBlock read_block(const AesState& state, std::size_t index) {
    AesBlock block{};
    std::copy_n(state.begin() + static_cast<std::ptrdiff_t>(index * block.size()), block.size(), block.begin());
    return block;
}

void write_block(AesState& state, std::size_t index, const AesBlock& block) {
    std::copy(block.begin(), block.end(), state.begin() + static_cast<std::ptrdiff_t>(index * block.size()));
}

} // namespace

AesGenerator1R::AesGenerator1R(const AesState& seed) : state_(seed) {}

AesState AesGenerator1R::next() {
    write_block(state_, 0, aes_decrypt_round(read_block(state_, 0), key0));
    write_block(state_, 1, aes_encrypt_round(read_block(state_, 1), key1));
    write_block(state_, 2, aes_decrypt_round(read_block(state_, 2), key2));
    write_block(state_, 3, aes_encrypt_round(read_block(state_, 3), key3));
    return state_;
}

void AesGenerator1R::fill(std::span<std::byte> output) {
    if ((output.size() % state_.size()) != 0U) {
        throw std::invalid_argument{"AesGenerator1R output must be a multiple of 64 bytes"};
    }
    for (std::size_t offset = 0; offset < output.size(); offset += state_.size()) {
        const auto block = next();
        std::copy(block.begin(), block.end(), output.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

} // namespace armrx
