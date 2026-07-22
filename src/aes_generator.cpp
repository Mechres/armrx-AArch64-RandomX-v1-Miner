#include "armrx/aes_generator.hpp"
#include "armrx/aes_keys.hpp"

#include <algorithm>
#include <stdexcept>

namespace armrx {
namespace {

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
    write_block(state_, 0, aes_decrypt_round(read_block(state_, 0), kAesGen1RKey0));
    write_block(state_, 1, aes_encrypt_round(read_block(state_, 1), kAesGen1RKey1));
    write_block(state_, 2, aes_decrypt_round(read_block(state_, 2), kAesGen1RKey2));
    write_block(state_, 3, aes_encrypt_round(read_block(state_, 3), kAesGen1RKey3));
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
    for (const auto& key : {kAesGen4RKey0, kAesGen4RKey1, kAesGen4RKey2, kAesGen4RKey3}) {
        s0 = aes_decrypt_round(s0, key);
        s1 = aes_encrypt_round(s1, key);
    }
    for (const auto& key : {kAesGen4RKey4, kAesGen4RKey5, kAesGen4RKey6, kAesGen4RKey7}) {
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
