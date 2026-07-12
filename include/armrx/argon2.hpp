#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace armrx {

using Argon2Block = std::array<std::uint64_t, 128>;

// Argon2's variable-length hash H' (RFC 9106 §3.3). This is used for the two
// 1 KiB starting blocks of each lane and for the eventual Argon2d tag.
[[nodiscard]] std::vector<std::byte> argon2_hprime(std::span<const std::byte> input,
                                                    std::size_t output_bytes);

// Argon2 compression G(X,Y). When xor_existing is true, XOR the result into
// destination as required for passes after the first.
[[nodiscard]] Argon2Block argon2_compress(const Argon2Block& previous,
                                          const Argon2Block& reference,
                                          const Argon2Block* destination = nullptr);

// One-lane Argon2d memory fill used by RandomX cache initialization. It keeps
// the memory blocks rather than producing a password-hashing tag.
class Argon2dCache {
public:
    explicit Argon2dCache(std::size_t memory_blocks = 262144U, std::size_t passes = 3U);

    void initialize(std::span<const std::byte> key);
    [[nodiscard]] std::span<const Argon2Block> blocks() const { return blocks_; }

private:
    std::size_t passes_;
    std::vector<Argon2Block> blocks_;
};

} // namespace armrx
