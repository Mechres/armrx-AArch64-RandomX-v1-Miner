#include "armrx/argon2.hpp"

#include "armrx/blake2b.hpp"

#include <algorithm>
#include <cstdint>
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

} // namespace armrx
