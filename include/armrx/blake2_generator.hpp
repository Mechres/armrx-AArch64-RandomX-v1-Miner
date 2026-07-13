#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace armrx {

class Blake2Generator {
public:
    Blake2Generator(const void* seed, std::size_t seed_size, int nonce = 0);

    [[nodiscard]] std::uint8_t get_byte();
    [[nodiscard]] std::uint32_t get_uint32();

private:
    void check_data(std::size_t bytes_needed);

    std::array<std::byte, 64> data_{};
    std::size_t data_index_;
};

} // namespace armrx
