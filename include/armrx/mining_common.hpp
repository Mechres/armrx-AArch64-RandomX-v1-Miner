#pragma once

#include <array>
#include <vector>
#include <string>
#include <cstddef>
#include <cstdint>
#include <span>

namespace armrx {

struct Target {
    std::array<std::byte, 32> bytes{};
};

struct Job {
    std::string job_id;
    std::vector<std::byte> block_template;
    std::size_t nonce_offset = 0;
    std::size_t nonce_size = 4; // Size of the nonce field (typically 4 or 8 bytes)
    Target target;
    std::vector<std::byte> seed_key; // Key used for initializing the Argon2 cache
};

inline bool meets_target(std::span<const std::byte, 32> hash, const Target& target) {
    for (int i = 31; i >= 0; --i) {
        const auto hash_val = static_cast<std::uint8_t>(hash[i]);
        const auto target_val = static_cast<std::uint8_t>(target.bytes[i]);
        if (hash_val < target_val) {
            return true;
        }
        if (hash_val > target_val) {
            return false;
        }
    }
    return true;
}

} // namespace armrx
