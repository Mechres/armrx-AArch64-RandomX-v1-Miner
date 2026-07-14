#include "armrx/blake2b.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace armrx {
namespace {

constexpr std::array<std::uint64_t, 8> iv{
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

constexpr std::array<std::array<unsigned, 16>, 12> sigma{{
    {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
    {{14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}},
    {{11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}},
    {{7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8}},
    {{9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}},
    {{2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9}},
    {{12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}},
    {{13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10}},
    {{6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}},
    {{10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}},
    {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
    {{14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}},
}};

[[nodiscard]] constexpr std::uint64_t byte_swap64(std::uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(value);
#else
    return ((value & 0x00000000000000ffULL) << 56U) |
           ((value & 0x000000000000ff00ULL) << 40U) |
           ((value & 0x0000000000ff0000ULL) << 24U) |
           ((value & 0x00000000ff000000ULL) << 8U) |
           ((value & 0x000000ff00000000ULL) >> 8U) |
           ((value & 0x0000ff0000000000ULL) >> 24U) |
           ((value & 0x00ff000000000000ULL) >> 40U) |
           ((value & 0xff00000000000000ULL) >> 56U);
#endif
}

[[nodiscard]] std::uint64_t load64(const std::byte* input) {
    std::uint64_t value{};
    std::memcpy(&value, input, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        value = byte_swap64(value);
    }
    return value;
}

void store64(std::byte* output, std::uint64_t value) {
    if constexpr (std::endian::native == std::endian::big) {
        value = byte_swap64(value);
    }
    std::memcpy(output, &value, sizeof(value));
}

void compress(std::array<std::uint64_t, 8>& h, const std::byte* block,
              std::uint64_t bytes, bool last) {
#if defined(__aarch64__) && defined(__ARM_NEON)
    // NEON-accelerated BLAKE2b compress
    alignas(16) std::array<std::uint64_t, 16> m{};
    alignas(16) std::array<std::uint64_t, 16> v{};
    for (unsigned i = 0; i < 16; ++i) m[i] = load64(block + 8U * i);
    for (unsigned i = 0; i < 8; ++i) v[i] = h[i];
    for (unsigned i = 0; i < 8; ++i) v[i + 8U] = iv[i];
    v[12] ^= bytes;
    if (last) v[14] = ~v[14];

    for (const auto& s : sigma) {
        // Row 0: G(0,4,8,12,x=0,y=1) + G(1,5,9,13,x=2,y=3) — all consecutive
        {
            uint64x2_t va = vcombine_u64(vcreate_u64(v[0]), vcreate_u64(v[1]));
            uint64x2_t vb = vcombine_u64(vcreate_u64(v[4]), vcreate_u64(v[5]));
            uint64x2_t vc = vcombine_u64(vcreate_u64(v[8]), vcreate_u64(v[9]));
            uint64x2_t vd = vcombine_u64(vcreate_u64(v[12]), vcreate_u64(v[13]));
            uint64x2_t mx = vcombine_u64(vcreate_u64(m[s[0]]), vcreate_u64(m[s[2]]));
            uint64x2_t my = vcombine_u64(vcreate_u64(m[s[1]]), vcreate_u64(m[s[3]]));

            va = vaddq_u64(va, vaddq_u64(vb, mx));
            vd = vsriq_n_u64(vshlq_n_u64(veorq_u64(vd, va), 32), veorq_u64(vd, va), 32);
            vc = vaddq_u64(vc, vd);
            vb = vsriq_n_u64(vshlq_n_u64(veorq_u64(vb, vc), 40), veorq_u64(vb, vc), 24);
            va = vaddq_u64(va, vaddq_u64(vb, my));
            vd = vsriq_n_u64(vshlq_n_u64(veorq_u64(vd, va), 48), veorq_u64(vd, va), 16);
            vc = vaddq_u64(vc, vd);
            vb = vsriq_n_u64(vshlq_n_u64(veorq_u64(vb, vc), 1), veorq_u64(vb, vc), 63);

            vst1q_u64(&v[0], va);
            vst1q_u64(&v[4], vb);
            vst1q_u64(&v[8], vc);
            vst1q_u64(&v[12], vd);
        }

        // Row 1: G(2,6,10,14,x=4,y=5) + G(3,7,11,15,x=6,y=7) — all consecutive
        {
            uint64x2_t va = vcombine_u64(vcreate_u64(v[2]), vcreate_u64(v[3]));
            uint64x2_t vb = vcombine_u64(vcreate_u64(v[6]), vcreate_u64(v[7]));
            uint64x2_t vc = vcombine_u64(vcreate_u64(v[10]), vcreate_u64(v[11]));
            uint64x2_t vd = vcombine_u64(vcreate_u64(v[14]), vcreate_u64(v[15]));
            uint64x2_t mx = vcombine_u64(vcreate_u64(m[s[4]]), vcreate_u64(m[s[6]]));
            uint64x2_t my = vcombine_u64(vcreate_u64(m[s[5]]), vcreate_u64(m[s[7]]));

            va = vaddq_u64(va, vaddq_u64(vb, mx));
            vd = vsriq_n_u64(vshlq_n_u64(veorq_u64(vd, va), 32), veorq_u64(vd, va), 32);
            vc = vaddq_u64(vc, vd);
            vb = vsriq_n_u64(vshlq_n_u64(veorq_u64(vb, vc), 40), veorq_u64(vb, vc), 24);
            va = vaddq_u64(va, vaddq_u64(vb, my));
            vd = vsriq_n_u64(vshlq_n_u64(veorq_u64(vd, va), 48), veorq_u64(vd, va), 16);
            vc = vaddq_u64(vc, vd);
            vb = vsriq_n_u64(vshlq_n_u64(veorq_u64(vb, vc), 1), veorq_u64(vb, vc), 63);

            vst1q_u64(&v[2], va);
            vst1q_u64(&v[6], vb);
            vst1q_u64(&v[10], vc);
            vst1q_u64(&v[14], vd);
        }

        // Row 2: G(0,5,10,15,x=8,y=9) + G(1,6,11,12,x=10,y=11)
        // vd reversed: lane0=v[15], lane1=v[12]
        {
            uint64x2_t va = vcombine_u64(vcreate_u64(v[0]), vcreate_u64(v[1]));
            uint64x2_t vb = vcombine_u64(vcreate_u64(v[5]), vcreate_u64(v[6]));
            uint64x2_t vc = vcombine_u64(vcreate_u64(v[10]), vcreate_u64(v[11]));
            uint64x2_t vd = vcombine_u64(vcreate_u64(v[15]), vcreate_u64(v[12]));
            uint64x2_t mx = vcombine_u64(vcreate_u64(m[s[8]]), vcreate_u64(m[s[10]]));
            uint64x2_t my = vcombine_u64(vcreate_u64(m[s[9]]), vcreate_u64(m[s[11]]));

            va = vaddq_u64(va, vaddq_u64(vb, mx));
            vd = vsriq_n_u64(vshlq_n_u64(veorq_u64(vd, va), 32), veorq_u64(vd, va), 32);
            vc = vaddq_u64(vc, vd);
            vb = vsriq_n_u64(vshlq_n_u64(veorq_u64(vb, vc), 40), veorq_u64(vb, vc), 24);
            va = vaddq_u64(va, vaddq_u64(vb, my));
            vd = vsriq_n_u64(vshlq_n_u64(veorq_u64(vd, va), 48), veorq_u64(vd, va), 16);
            vc = vaddq_u64(vc, vd);
            vb = vsriq_n_u64(vshlq_n_u64(veorq_u64(vb, vc), 1), veorq_u64(vb, vc), 63);

            vst1q_u64(&v[0], va);
            vst1q_u64(&v[5], vb);
            vst1q_u64(&v[10], vc);
            v[15] = vgetq_lane_u64(vd, 0);
            v[12] = vgetq_lane_u64(vd, 1);
        }

        // Row 3: G(2,7,8,13,x=12,y=13) + G(3,4,9,14,x=14,y=15)
        // vb non-consecutive: lane0=v[7], lane1=v[4]
        {
            uint64x2_t va = vcombine_u64(vcreate_u64(v[2]), vcreate_u64(v[3]));
            uint64x2_t vb = vcombine_u64(vcreate_u64(v[7]), vcreate_u64(v[4]));
            uint64x2_t vc = vcombine_u64(vcreate_u64(v[8]), vcreate_u64(v[9]));
            uint64x2_t vd = vcombine_u64(vcreate_u64(v[13]), vcreate_u64(v[14]));
            uint64x2_t mx = vcombine_u64(vcreate_u64(m[s[12]]), vcreate_u64(m[s[14]]));
            uint64x2_t my = vcombine_u64(vcreate_u64(m[s[13]]), vcreate_u64(m[s[15]]));

            va = vaddq_u64(va, vaddq_u64(vb, mx));
            vd = vsriq_n_u64(vshlq_n_u64(veorq_u64(vd, va), 32), veorq_u64(vd, va), 32);
            vc = vaddq_u64(vc, vd);
            vb = vsriq_n_u64(vshlq_n_u64(veorq_u64(vb, vc), 40), veorq_u64(vb, vc), 24);
            va = vaddq_u64(va, vaddq_u64(vb, my));
            vd = vsriq_n_u64(vshlq_n_u64(veorq_u64(vd, va), 48), veorq_u64(vd, va), 16);
            vc = vaddq_u64(vc, vd);
            vb = vsriq_n_u64(vshlq_n_u64(veorq_u64(vb, vc), 1), veorq_u64(vb, vc), 63);

            vst1q_u64(&v[2], va);
            v[7] = vgetq_lane_u64(vb, 0);
            v[4] = vgetq_lane_u64(vb, 1);
            vst1q_u64(&v[8], vc);
            vst1q_u64(&v[13], vd);
        }
    }

    for (unsigned i = 0; i < 8; ++i) h[i] ^= v[i] ^ v[i + 8U];
#else
    // Scalar path (x86_64, non-NEON AArch64, etc.)
    std::array<std::uint64_t, 16> m{};
    std::array<std::uint64_t, 16> v{};
    for (unsigned i = 0; i < m.size(); ++i) m[i] = load64(block + 8U * i);
    for (unsigned i = 0; i < h.size(); ++i) v[i] = h[i];
    for (unsigned i = 0; i < iv.size(); ++i) v[i + 8U] = iv[i];
    v[12] ^= bytes;
    if (last) v[14] = ~v[14];

    const auto g = [&v, &m](unsigned a, unsigned b, unsigned c, unsigned d,
                             unsigned x, unsigned y) {
        v[a] = v[a] + v[b] + m[x];
        v[d] = std::rotr(v[d] ^ v[a], 32);
        v[c] += v[d];
        v[b] = std::rotr(v[b] ^ v[c], 24);
        v[a] = v[a] + v[b] + m[y];
        v[d] = std::rotr(v[d] ^ v[a], 16);
        v[c] += v[d];
        v[b] = std::rotr(v[b] ^ v[c], 63);
    };
    for (const auto& s : sigma) {
        g(0, 4, 8, 12, s[0], s[1]); g(1, 5, 9, 13, s[2], s[3]);
        g(2, 6, 10, 14, s[4], s[5]); g(3, 7, 11, 15, s[6], s[7]);
        g(0, 5, 10, 15, s[8], s[9]); g(1, 6, 11, 12, s[10], s[11]);
        g(2, 7, 8, 13, s[12], s[13]); g(3, 4, 9, 14, s[14], s[15]);
    }
    for (unsigned i = 0; i < h.size(); ++i) h[i] ^= v[i] ^ v[i + 8U];
#endif
}

} // namespace

std::vector<std::byte> blake2b(std::span<const std::byte> input, std::size_t output_bytes) {
    if (output_bytes == 0U || output_bytes > 64U) {
        throw std::invalid_argument{"BLAKE2b output length must be between 1 and 64 bytes"};
    }
    std::array<std::uint64_t, 8> state = iv;
    state[0] ^= 0x01010000ULL ^ static_cast<std::uint64_t>(output_bytes);
    std::uint64_t bytes{};
    while (input.size() > 128U) {
        bytes += 128U;
        compress(state, input.data(), bytes, false);
        input = input.subspan(128U);
    }
    std::array<std::byte, 128> last{};
    std::memcpy(last.data(), input.data(), input.size());
    bytes += static_cast<std::uint64_t>(input.size());
    compress(state, last.data(), bytes, true);
    std::array<std::byte, 64> digest{};
    for (unsigned i = 0; i < state.size(); ++i) store64(digest.data() + 8U * i, state[i]);
    std::vector<std::byte> output(output_bytes);
    std::copy_n(digest.begin(), static_cast<std::ptrdiff_t>(output_bytes), output.begin());
    return output;
}

void blake2b(std::span<const std::byte> input, std::byte* output, std::size_t output_bytes) {
    if (output_bytes == 0U || output_bytes > 64U) {
        throw std::invalid_argument{"BLAKE2b output length must be between 1 and 64 bytes"};
    }
    std::array<std::uint64_t, 8> state = iv;
    state[0] ^= 0x01010000ULL ^ static_cast<std::uint64_t>(output_bytes);
    std::uint64_t bytes{};
    while (input.size() > 128U) {
        bytes += 128U;
        compress(state, input.data(), bytes, false);
        input = input.subspan(128U);
    }
    std::array<std::byte, 128> last{};
    std::memcpy(last.data(), input.data(), input.size());
    bytes += static_cast<std::uint64_t>(input.size());
    compress(state, last.data(), bytes, true);
    std::array<std::byte, 64> digest{};
    for (unsigned i = 0; i < state.size(); ++i) {
        store64(digest.data() + 8U * i, state[i]);
    }
    std::memcpy(output, digest.data(), output_bytes);
}

Hash512 blake2b_512(std::span<const std::byte> input) {
    Hash512 output{};
    blake2b(input, output.data(), 64U);
    return output;
}

} // namespace armrx
