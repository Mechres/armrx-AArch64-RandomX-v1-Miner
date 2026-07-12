#pragma once

#include "armrx/aes.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace armrx {

using AesState = std::array<std::byte, 64>;

// RandomX AesGenerator1R (specification §3.2). It has no hidden global state,
// allowing one generator per worker while sharing a cache or dataset.
class AesGenerator1R {
public:
    explicit AesGenerator1R(const AesState& seed);

    [[nodiscard]] AesState next();
    void fill(std::span<std::byte> output);
    [[nodiscard]] const AesState& state() const { return state_; }

private:
    AesState state_;
};

// RandomX AesGenerator4R (specification §3.3), used to create each VM's
// configuration and 256-instruction program buffer.
class AesGenerator4R {
public:
    explicit AesGenerator4R(const AesState& seed);

    [[nodiscard]] AesState next();
    void fill(std::span<std::byte> output);
    [[nodiscard]] const AesState& state() const { return state_; }

private:
    AesState state_;
};

} // namespace armrx
