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

constexpr AesBlock key4r0{
    std::byte{0xdd}, std::byte{0xaa}, std::byte{0x21}, std::byte{0x64}, std::byte{0xdb}, std::byte{0x3d}, std::byte{0x83}, std::byte{0xd1},
    std::byte{0x2b}, std::byte{0x6d}, std::byte{0x54}, std::byte{0x2f}, std::byte{0x3f}, std::byte{0xd2}, std::byte{0xe5}, std::byte{0x99}};
constexpr AesBlock key4r1{
    std::byte{0x50}, std::byte{0x34}, std::byte{0x0e}, std::byte{0xb2}, std::byte{0x55}, std::byte{0x3f}, std::byte{0x91}, std::byte{0xb6},
    std::byte{0x53}, std::byte{0x9d}, std::byte{0xf7}, std::byte{0x06}, std::byte{0xe5}, std::byte{0xcd}, std::byte{0xdf}, std::byte{0xa5}};
constexpr AesBlock key4r2{
    std::byte{0x04}, std::byte{0xd9}, std::byte{0x3e}, std::byte{0x5c}, std::byte{0xaf}, std::byte{0x7b}, std::byte{0x5e}, std::byte{0x51},
    std::byte{0x9f}, std::byte{0x67}, std::byte{0xa4}, std::byte{0x0a}, std::byte{0xbf}, std::byte{0x02}, std::byte{0x1c}, std::byte{0x17}};
constexpr AesBlock key4r3{
    std::byte{0x63}, std::byte{0x37}, std::byte{0x62}, std::byte{0x85}, std::byte{0x08}, std::byte{0x5d}, std::byte{0x8f}, std::byte{0xe7},
    std::byte{0x85}, std::byte{0x37}, std::byte{0x67}, std::byte{0xcd}, std::byte{0x91}, std::byte{0xd2}, std::byte{0xde}, std::byte{0xd8}};
constexpr AesBlock key4r4{
    std::byte{0x73}, std::byte{0x6f}, std::byte{0x82}, std::byte{0xb5}, std::byte{0xa6}, std::byte{0xa7}, std::byte{0xd6}, std::byte{0xe3},
    std::byte{0x6d}, std::byte{0x8b}, std::byte{0x51}, std::byte{0x3d}, std::byte{0xb4}, std::byte{0xff}, std::byte{0x9e}, std::byte{0x22}};
constexpr AesBlock key4r5{
    std::byte{0xf3}, std::byte{0x6b}, std::byte{0x56}, std::byte{0xc7}, std::byte{0xd9}, std::byte{0xb3}, std::byte{0x10}, std::byte{0x9c},
    std::byte{0x4e}, std::byte{0x4d}, std::byte{0x02}, std::byte{0xe9}, std::byte{0xd2}, std::byte{0xb7}, std::byte{0x72}, std::byte{0xb2}};
constexpr AesBlock key4r6{
    std::byte{0xe7}, std::byte{0xc9}, std::byte{0x73}, std::byte{0xf2}, std::byte{0x8b}, std::byte{0xa3}, std::byte{0x65}, std::byte{0xf7},
    std::byte{0x0a}, std::byte{0x66}, std::byte{0xa9}, std::byte{0x2b}, std::byte{0xa7}, std::byte{0xef}, std::byte{0x3b}, std::byte{0xf6}};
constexpr AesBlock key4r7{
    std::byte{0x09}, std::byte{0xd6}, std::byte{0x7c}, std::byte{0x7a}, std::byte{0xde}, std::byte{0x39}, std::byte{0x58}, std::byte{0x91},
    std::byte{0xfd}, std::byte{0xd1}, std::byte{0x06}, std::byte{0x0c}, std::byte{0x2d}, std::byte{0x76}, std::byte{0xb0}, std::byte{0xc0}};

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

AesGenerator4R::AesGenerator4R(const AesState& seed) : state_(seed) {}

AesState AesGenerator4R::next() {
    auto s0 = read_block(state_, 0);
    auto s1 = read_block(state_, 1);
    auto s2 = read_block(state_, 2);
    auto s3 = read_block(state_, 3);
    for (const auto& key : {key4r0, key4r1, key4r2, key4r3}) {
        s0 = aes_decrypt_round(s0, key);
        s1 = aes_encrypt_round(s1, key);
    }
    for (const auto& key : {key4r4, key4r5, key4r6, key4r7}) {
        s2 = aes_decrypt_round(s2, key);
        s3 = aes_encrypt_round(s3, key);
    }
    write_block(state_, 0, s0);
    write_block(state_, 1, s1);
    write_block(state_, 2, s2);
    write_block(state_, 3, s3);
    return state_;
}

void AesGenerator4R::fill(std::span<std::byte> output) {
    if ((output.size() % state_.size()) != 0U) {
        throw std::invalid_argument{"AesGenerator4R output must be a multiple of 64 bytes"};
    }
    for (std::size_t offset = 0; offset < output.size(); offset += state_.size()) {
        const auto block = next();
        std::copy(block.begin(), block.end(), output.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

} // namespace armrx
