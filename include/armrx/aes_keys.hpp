#pragma once

// RandomX spec AES round-key constants (specification §3.2 AesGenerator1R,
// §3.3 AesGenerator4R). These are used both by the AesGenerator1R/AesGenerator4R
// classes (aes_generator.cpp) and by the free-function fill/hash helpers
// (aes_hash.cpp) — previously duplicated as two independently-maintained
// literal encodings of the same 12 blocks (raw byte arrays vs. build_aes_block
// from big-endian words), which is exactly the kind of drift-prone duplication
// this header exists to close.

#include "armrx/aes.hpp"

#include <cstdint>

namespace armrx {

namespace detail {

constexpr std::byte aes_key_byte_at(std::uint32_t val, int index) {
    return static_cast<std::byte>((val >> (index * 8)) & 0xff);
}

// Assembles a 16-byte AES round key from four 32-bit words, most-significant
// word first — the layout the RandomX spec's own constant tables use.
constexpr AesBlock build_aes_key(std::uint32_t i3, std::uint32_t i2, std::uint32_t i1, std::uint32_t i0) {
    return AesBlock{
        aes_key_byte_at(i0, 0), aes_key_byte_at(i0, 1), aes_key_byte_at(i0, 2), aes_key_byte_at(i0, 3),
        aes_key_byte_at(i1, 0), aes_key_byte_at(i1, 1), aes_key_byte_at(i1, 2), aes_key_byte_at(i1, 3),
        aes_key_byte_at(i2, 0), aes_key_byte_at(i2, 1), aes_key_byte_at(i2, 2), aes_key_byte_at(i2, 3),
        aes_key_byte_at(i3, 0), aes_key_byte_at(i3, 1), aes_key_byte_at(i3, 2), aes_key_byte_at(i3, 3)
    };
}

} // namespace detail

// AesGenerator1R round keys (4 blocks, one per lane).
inline constexpr AesBlock kAesGen1RKey0 = detail::build_aes_key(0xb4f44917, 0xdbb5552b, 0x62716609, 0x6daca553);
inline constexpr AesBlock kAesGen1RKey1 = detail::build_aes_key(0x0da1dc4e, 0x1725d378, 0x846a710d, 0x6d7caf07);
inline constexpr AesBlock kAesGen1RKey2 = detail::build_aes_key(0x3e20e345, 0xf4c0794f, 0x9f947ec6, 0x3f1262f1);
inline constexpr AesBlock kAesGen1RKey3 = detail::build_aes_key(0x49169154, 0x16314c88, 0xb1ba317c, 0x6aef8135);

// AesGenerator4R round keys (8 blocks, applied in two groups of 4 per lane pair).
inline constexpr AesBlock kAesGen4RKey0 = detail::build_aes_key(0x99e5d23f, 0x2f546d2b, 0xd1833ddb, 0x6421aadd);
inline constexpr AesBlock kAesGen4RKey1 = detail::build_aes_key(0xa5dfcde5, 0x06f79d53, 0xb6913f55, 0xb20e3450);
inline constexpr AesBlock kAesGen4RKey2 = detail::build_aes_key(0x171c02bf, 0x0aa4679f, 0x515e7baf, 0x5c3ed904);
inline constexpr AesBlock kAesGen4RKey3 = detail::build_aes_key(0xd8ded291, 0xcd673785, 0xe78f5d08, 0x85623763);
inline constexpr AesBlock kAesGen4RKey4 = detail::build_aes_key(0x229effb4, 0x3d518b6d, 0xe3d6a7a6, 0xb5826f73);
inline constexpr AesBlock kAesGen4RKey5 = detail::build_aes_key(0xb272b7d2, 0xe9024d4e, 0x9c10b3d9, 0xc7566bf3);
inline constexpr AesBlock kAesGen4RKey6 = detail::build_aes_key(0xf63befa7, 0x2ba9660a, 0xf765a38b, 0xf273c9e7);
inline constexpr AesBlock kAesGen4RKey7 = detail::build_aes_key(0xc0b0762d, 0x0c06d1fd, 0x915839de, 0x7a7cd609);

} // namespace armrx
