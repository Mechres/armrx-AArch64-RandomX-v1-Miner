#include "armrx/argon2.hpp"

#include "armrx/blake2b.hpp"

#include <algorithm>
#include <cstdint>
#include <bit>
#include <stdexcept>
#include <vector>

namespace armrx {
namespace {

void append_le32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        output.push_back(static_cast<std::byte>(value >> (i * 8U)));
    }
}

} // namespace

std::vector<std::byte> argon2_hprime(std::span<const std::byte> input, std::size_t output_bytes) {
    if (output_bytes == 0U || output_bytes > UINT32_MAX) {
        throw std::invalid_argument{"Argon2 H' output length is invalid"};
    }
    std::vector<std::byte> initial;
    initial.reserve(input.size() + 4U);
    append_le32(initial, static_cast<std::uint32_t>(output_bytes));
    initial.insert(initial.end(), input.begin(), input.end());
    if (output_bytes <= 64U) return blake2b(initial, output_bytes);

    const auto rounds = (output_bytes + 31U) / 32U - 2U;
    auto value = blake2b(initial, 64);
    std::vector<std::byte> output;
    output.reserve(output_bytes);
    for (std::size_t i = 0; i < rounds; ++i) {
        output.insert(output.end(), value.begin(), value.begin() + 32);
        if (i + 1U < rounds) value = blake2b(value, 64);
    }
    const auto final = blake2b(value, output_bytes - rounds * 32U);
    output.insert(output.end(), final.begin(), final.end());
    return output;
}

namespace {

[[nodiscard]] constexpr std::uint64_t blamka_add(std::uint64_t left, std::uint64_t right) {
    const auto product = static_cast<std::uint64_t>(static_cast<std::uint32_t>(left)) *
                         static_cast<std::uint64_t>(static_cast<std::uint32_t>(right));
    return left + right + 2U * product;
}

void gb(std::uint64_t& a, std::uint64_t& b, std::uint64_t& c, std::uint64_t& d) {
    a = blamka_add(a, b);
    d = std::rotr(d ^ a, 32);
    c = blamka_add(c, d);
    b = std::rotr(b ^ c, 24);
    a = blamka_add(a, b);
    d = std::rotr(d ^ a, 16);
    c = blamka_add(c, d);
    b = std::rotr(b ^ c, 63);
}

void permute_16(std::uint64_t* words) {
    gb(words[0], words[4], words[8], words[12]);
    gb(words[1], words[5], words[9], words[13]);
    gb(words[2], words[6], words[10], words[14]);
    gb(words[3], words[7], words[11], words[15]);
    gb(words[0], words[5], words[10], words[15]);
    gb(words[1], words[6], words[11], words[12]);
    gb(words[2], words[7], words[8], words[13]);
    gb(words[3], words[4], words[9], words[14]);
}

void permute_block(Argon2Block& block) {
    for (unsigned row = 0; row < 8; ++row) permute_16(block.data() + 16U * row);
    for (unsigned column = 0; column < 8; ++column) {
        std::array<std::uint64_t, 16> words{};
        for (unsigned row = 0; row < 8; ++row) {
            words[2U * row] = block[16U * row + 2U * column];
            words[2U * row + 1U] = block[16U * row + 2U * column + 1U];
        }
        permute_16(words.data());
        for (unsigned row = 0; row < 8; ++row) {
            block[16U * row + 2U * column] = words[2U * row];
            block[16U * row + 2U * column + 1U] = words[2U * row + 1U];
        }
    }
}

} // namespace

Argon2Block argon2_compress(const Argon2Block& previous, const Argon2Block& reference,
                            const Argon2Block* destination) {
    Argon2Block result{};
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = previous[i] ^ reference[i];
    auto permuted = result;
    permute_block(permuted);
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] ^= permuted[i];
        if (destination != nullptr) result[i] ^= (*destination)[i];
    }
    return result;
}

} // namespace armrx
