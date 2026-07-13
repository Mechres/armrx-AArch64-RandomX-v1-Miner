#include "armrx/aes_hash.hpp"
#include <algorithm>
#include <cstring>
#include <cassert>
#include <cstdint>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
#include <arm_neon.h>
#include <arm_aes.h>
#endif

namespace armrx {
namespace {

constexpr std::byte byte_at(std::uint32_t val, int index) {
    return static_cast<std::byte>((val >> (index * 8)) & 0xff);
}

constexpr AesBlock build_aes_block(std::uint32_t i3, std::uint32_t i2, std::uint32_t i1, std::uint32_t i0) {
    return AesBlock{
        byte_at(i0, 0), byte_at(i0, 1), byte_at(i0, 2), byte_at(i0, 3),
        byte_at(i1, 0), byte_at(i1, 1), byte_at(i1, 2), byte_at(i1, 3),
        byte_at(i2, 0), byte_at(i2, 1), byte_at(i2, 2), byte_at(i2, 3),
        byte_at(i3, 0), byte_at(i3, 1), byte_at(i3, 2), byte_at(i3, 3)
    };
}

constexpr AesBlock key1r_0 = build_aes_block(0xb4f44917, 0xdbb5552b, 0x62716609, 0x6daca553);
constexpr AesBlock key1r_1 = build_aes_block(0x0da1dc4e, 0x1725d378, 0x846a710d, 0x6d7caf07);
constexpr AesBlock key1r_2 = build_aes_block(0x3e20e345, 0xf4c0794f, 0x9f947ec6, 0x3f1262f1);
constexpr AesBlock key1r_3 = build_aes_block(0x49169154, 0x16314c88, 0xb1ba317c, 0x6aef8135);

constexpr AesBlock key4r_0 = build_aes_block(0x99e5d23f, 0x2f546d2b, 0xd1833ddb, 0x6421aadd);
constexpr AesBlock key4r_1 = build_aes_block(0xa5dfcde5, 0x06f79d53, 0xb6913f55, 0xb20e3450);
constexpr AesBlock key4r_2 = build_aes_block(0x171c02bf, 0x0aa4679f, 0x515e7baf, 0x5c3ed904);
constexpr AesBlock key4r_3 = build_aes_block(0xd8ded291, 0xcd673785, 0xe78f5d08, 0x85623763);
constexpr AesBlock key4r_4 = build_aes_block(0x229effb4, 0x3d518b6d, 0xe3d6a7a6, 0xb5826f73);
constexpr AesBlock key4r_5 = build_aes_block(0xb272b7d2, 0xe9024d4e, 0x9c10b3d9, 0xc7566bf3);
constexpr AesBlock key4r_6 = build_aes_block(0xf63befa7, 0x2ba9660a, 0xf765a38b, 0xf273c9e7);
constexpr AesBlock key4r_7 = build_aes_block(0xc0b0762d, 0x0c06d1fd, 0x915839de, 0x7a7cd609);

constexpr AesBlock hash_state_0 = build_aes_block(0xd7983aad, 0xcc82db47, 0x9fa856de, 0x92b52c0d);
constexpr AesBlock hash_state_1 = build_aes_block(0xace78057, 0xf59e125a, 0x15c7b798, 0x338d996e);
constexpr AesBlock hash_state_2 = build_aes_block(0xe8a07ce4, 0x5079506b, 0xae62c7d0, 0x6a770017);
constexpr AesBlock hash_state_3 = build_aes_block(0x7e994948, 0x79a10005, 0x07ad828d, 0x630a240c);

constexpr AesBlock hash_xkey_0 = build_aes_block(0x06890201, 0x90dc56bf, 0x8b24949f, 0xf6fa8389);
constexpr AesBlock hash_xkey_1 = build_aes_block(0xed18f99b, 0xee1043c6, 0x51f4e03c, 0x61b263d1);

inline AesBlock read_block(const AesState& state, std::size_t index) {
    AesBlock block{};
    std::copy_n(state.begin() + index * 16, 16, block.begin());
    return block;
}

inline void write_block(AesState& state, std::size_t index, const AesBlock& block) {
    std::copy(block.begin(), block.end(), state.begin() + index * 16);
}

inline AesBlock read_block_from_span(std::span<const std::byte> input, std::size_t offset) {
    AesBlock block{};
    std::copy_n(input.begin() + offset, 16, block.begin());
    return block;
}

inline void write_block_to_span(std::span<std::byte> output, std::size_t offset, const AesBlock& block) {
    std::copy(block.begin(), block.end(), output.begin() + offset);
}

} // namespace

void fill_aes_1r_x4(AesState& state, std::span<std::byte> output) {
    assert(output.size() % 64 == 0);
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
    uint8x16_t s0 = vld1q_u8(reinterpret_cast<const uint8_t*>(state.data() + 0));
    uint8x16_t s1 = vld1q_u8(reinterpret_cast<const uint8_t*>(state.data() + 16));
    uint8x16_t s2 = vld1q_u8(reinterpret_cast<const uint8_t*>(state.data() + 32));
    uint8x16_t s3 = vld1q_u8(reinterpret_cast<const uint8_t*>(state.data() + 48));

    const uint8x16_t k0 = vld1q_u8(reinterpret_cast<const uint8_t*>(key1r_0.data()));
    const uint8x16_t k1 = vld1q_u8(reinterpret_cast<const uint8_t*>(key1r_1.data()));
    const uint8x16_t k2 = vld1q_u8(reinterpret_cast<const uint8_t*>(key1r_2.data()));
    const uint8x16_t k3 = vld1q_u8(reinterpret_cast<const uint8_t*>(key1r_3.data()));

    const uint8x16_t zero = vdupq_n_u8(0);

    for (std::size_t offset = 0; offset < output.size(); offset += 64) {
        s0 = veorq_u8(vaesimcq_u8(vaesdq_u8(s0, zero)), k0);
        s1 = veorq_u8(vaesmcq_u8(vaeseq_u8(s1, zero)), k1);
        s2 = veorq_u8(vaesimcq_u8(vaesdq_u8(s2, zero)), k2);
        s3 = veorq_u8(vaesmcq_u8(vaeseq_u8(s3, zero)), k3);

        vst1q_u8(reinterpret_cast<uint8_t*>(output.data() + offset + 0), s0);
        vst1q_u8(reinterpret_cast<uint8_t*>(output.data() + offset + 16), s1);
        vst1q_u8(reinterpret_cast<uint8_t*>(output.data() + offset + 32), s2);
        vst1q_u8(reinterpret_cast<uint8_t*>(output.data() + offset + 48), s3);
    }

    vst1q_u8(reinterpret_cast<uint8_t*>(state.data() + 0), s0);
    vst1q_u8(reinterpret_cast<uint8_t*>(state.data() + 16), s1);
    vst1q_u8(reinterpret_cast<uint8_t*>(state.data() + 32), s2);
    vst1q_u8(reinterpret_cast<uint8_t*>(state.data() + 48), s3);
#else
    AesBlock s0 = read_block(state, 0);
    AesBlock s1 = read_block(state, 1);
    AesBlock s2 = read_block(state, 2);
    AesBlock s3 = read_block(state, 3);

    for (std::size_t offset = 0; offset < output.size(); offset += 64) {
        s0 = aes_decrypt_round(s0, key1r_0);
        s1 = aes_encrypt_round(s1, key1r_1);
        s2 = aes_decrypt_round(s2, key1r_2);
        s3 = aes_encrypt_round(s3, key1r_3);

        write_block_to_span(output, offset + 0, s0);
        write_block_to_span(output, offset + 16, s1);
        write_block_to_span(output, offset + 32, s2);
        write_block_to_span(output, offset + 48, s3);
    }

    write_block(state, 0, s0);
    write_block(state, 1, s1);
    write_block(state, 2, s2);
    write_block(state, 3, s3);
#endif
}

void fill_aes_4r_x4(AesState& state, std::span<std::byte> output) {
    assert(output.size() % 64 == 0);
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
    uint8x16_t s0 = vld1q_u8(reinterpret_cast<const uint8_t*>(state.data() + 0));
    uint8x16_t s1 = vld1q_u8(reinterpret_cast<const uint8_t*>(state.data() + 16));
    uint8x16_t s2 = vld1q_u8(reinterpret_cast<const uint8_t*>(state.data() + 32));
    uint8x16_t s3 = vld1q_u8(reinterpret_cast<const uint8_t*>(state.data() + 48));

    const uint8x16_t k4r_0_v = vld1q_u8(reinterpret_cast<const uint8_t*>(key4r_0.data()));
    const uint8x16_t k4r_1_v = vld1q_u8(reinterpret_cast<const uint8_t*>(key4r_1.data()));
    const uint8x16_t k4r_2_v = vld1q_u8(reinterpret_cast<const uint8_t*>(key4r_2.data()));
    const uint8x16_t k4r_3_v = vld1q_u8(reinterpret_cast<const uint8_t*>(key4r_3.data()));
    const uint8x16_t k4r_4_v = vld1q_u8(reinterpret_cast<const uint8_t*>(key4r_4.data()));
    const uint8x16_t k4r_5_v = vld1q_u8(reinterpret_cast<const uint8_t*>(key4r_5.data()));
    const uint8x16_t k4r_6_v = vld1q_u8(reinterpret_cast<const uint8_t*>(key4r_6.data()));
    const uint8x16_t k4r_7_v = vld1q_u8(reinterpret_cast<const uint8_t*>(key4r_7.data()));

    const uint8x16_t zero = vdupq_n_u8(0);

    for (std::size_t offset = 0; offset < output.size(); offset += 64) {
        s0 = veorq_u8(vaesimcq_u8(vaesdq_u8(s0, zero)), k4r_0_v);
        s1 = veorq_u8(vaesmcq_u8(vaeseq_u8(s1, zero)), k4r_0_v);
        s0 = veorq_u8(vaesimcq_u8(vaesdq_u8(s0, zero)), k4r_1_v);
        s1 = veorq_u8(vaesmcq_u8(vaeseq_u8(s1, zero)), k4r_1_v);
        s0 = veorq_u8(vaesimcq_u8(vaesdq_u8(s0, zero)), k4r_2_v);
        s1 = veorq_u8(vaesmcq_u8(vaeseq_u8(s1, zero)), k4r_2_v);
        s0 = veorq_u8(vaesimcq_u8(vaesdq_u8(s0, zero)), k4r_3_v);
        s1 = veorq_u8(vaesmcq_u8(vaeseq_u8(s1, zero)), k4r_3_v);

        s2 = veorq_u8(vaesimcq_u8(vaesdq_u8(s2, zero)), k4r_4_v);
        s3 = veorq_u8(vaesmcq_u8(vaeseq_u8(s3, zero)), k4r_4_v);
        s2 = veorq_u8(vaesimcq_u8(vaesdq_u8(s2, zero)), k4r_5_v);
        s3 = veorq_u8(vaesmcq_u8(vaeseq_u8(s3, zero)), k4r_5_v);
        s2 = veorq_u8(vaesimcq_u8(vaesdq_u8(s2, zero)), k4r_6_v);
        s3 = veorq_u8(vaesmcq_u8(vaeseq_u8(s3, zero)), k4r_6_v);
        s2 = veorq_u8(vaesimcq_u8(vaesdq_u8(s2, zero)), k4r_7_v);
        s3 = veorq_u8(vaesmcq_u8(vaeseq_u8(s3, zero)), k4r_7_v);

        vst1q_u8(reinterpret_cast<uint8_t*>(output.data() + offset + 0), s0);
        vst1q_u8(reinterpret_cast<uint8_t*>(output.data() + offset + 16), s1);
        vst1q_u8(reinterpret_cast<uint8_t*>(output.data() + offset + 32), s2);
        vst1q_u8(reinterpret_cast<uint8_t*>(output.data() + offset + 48), s3);
    }

    vst1q_u8(reinterpret_cast<uint8_t*>(state.data() + 0), s0);
    vst1q_u8(reinterpret_cast<uint8_t*>(state.data() + 16), s1);
    vst1q_u8(reinterpret_cast<uint8_t*>(state.data() + 32), s2);
    vst1q_u8(reinterpret_cast<uint8_t*>(state.data() + 48), s3);
#else
    AesBlock s0 = read_block(state, 0);
    AesBlock s1 = read_block(state, 1);
    AesBlock s2 = read_block(state, 2);
    AesBlock s3 = read_block(state, 3);

    for (std::size_t offset = 0; offset < output.size(); offset += 64) {
        for (const auto& key : {key4r_0, key4r_1, key4r_2, key4r_3}) {
            s0 = aes_decrypt_round(s0, key);
            s1 = aes_encrypt_round(s1, key);
        }
        for (const auto& key : {key4r_4, key4r_5, key4r_6, key4r_7}) {
            s2 = aes_decrypt_round(s2, key);
            s3 = aes_encrypt_round(s3, key);
        }

        write_block_to_span(output, offset + 0, s0);
        write_block_to_span(output, offset + 16, s1);
        write_block_to_span(output, offset + 32, s2);
        write_block_to_span(output, offset + 48, s3);
    }

    write_block(state, 0, s0);
    write_block(state, 1, s1);
    write_block(state, 2, s2);
    write_block(state, 3, s3);
#endif
}

void hash_aes_1r_x4(std::span<const std::byte> input, AesState& hash) {
    assert(input.size() % 64 == 0);
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
    uint8x16_t s0 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_state_0.data()));
    uint8x16_t s1 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_state_1.data()));
    uint8x16_t s2 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_state_2.data()));
    uint8x16_t s3 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_state_3.data()));

    const uint8x16_t xk0 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_xkey_0.data()));
    const uint8x16_t xk1 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_xkey_1.data()));

    const uint8x16_t zero = vdupq_n_u8(0);

    for (std::size_t offset = 0; offset < input.size(); offset += 64) {
        uint8x16_t in0 = vld1q_u8(reinterpret_cast<const uint8_t*>(input.data() + offset + 0));
        uint8x16_t in1 = vld1q_u8(reinterpret_cast<const uint8_t*>(input.data() + offset + 16));
        uint8x16_t in2 = vld1q_u8(reinterpret_cast<const uint8_t*>(input.data() + offset + 32));
        uint8x16_t in3 = vld1q_u8(reinterpret_cast<const uint8_t*>(input.data() + offset + 48));

        s0 = veorq_u8(vaesmcq_u8(vaeseq_u8(s0, zero)), in0);
        s1 = veorq_u8(vaesimcq_u8(vaesdq_u8(s1, zero)), in1);
        s2 = veorq_u8(vaesmcq_u8(vaeseq_u8(s2, zero)), in2);
        s3 = veorq_u8(vaesimcq_u8(vaesdq_u8(s3, zero)), in3);
    }

    s0 = veorq_u8(vaesmcq_u8(vaeseq_u8(s0, zero)), xk0);
    s1 = veorq_u8(vaesimcq_u8(vaesdq_u8(s1, zero)), xk0);
    s2 = veorq_u8(vaesmcq_u8(vaeseq_u8(s2, zero)), xk0);
    s3 = veorq_u8(vaesimcq_u8(vaesdq_u8(s3, zero)), xk0);

    s0 = veorq_u8(vaesmcq_u8(vaeseq_u8(s0, zero)), xk1);
    s1 = veorq_u8(vaesimcq_u8(vaesdq_u8(s1, zero)), xk1);
    s2 = veorq_u8(vaesmcq_u8(vaeseq_u8(s2, zero)), xk1);
    s3 = veorq_u8(vaesimcq_u8(vaesdq_u8(s3, zero)), xk1);

    vst1q_u8(reinterpret_cast<uint8_t*>(hash.data() + 0), s0);
    vst1q_u8(reinterpret_cast<uint8_t*>(hash.data() + 16), s1);
    vst1q_u8(reinterpret_cast<uint8_t*>(hash.data() + 32), s2);
    vst1q_u8(reinterpret_cast<uint8_t*>(hash.data() + 48), s3);
#else
    AesBlock s0 = hash_state_0;
    AesBlock s1 = hash_state_1;
    AesBlock s2 = hash_state_2;
    AesBlock s3 = hash_state_3;

    for (std::size_t offset = 0; offset < input.size(); offset += 64) {
        AesBlock in0 = read_block_from_span(input, offset + 0);
        AesBlock in1 = read_block_from_span(input, offset + 16);
        AesBlock in2 = read_block_from_span(input, offset + 32);
        AesBlock in3 = read_block_from_span(input, offset + 48);

        s0 = aes_encrypt_round(s0, in0);
        s1 = aes_decrypt_round(s1, in1);
        s2 = aes_encrypt_round(s2, in2);
        s3 = aes_decrypt_round(s3, in3);
    }

    s0 = aes_encrypt_round(s0, hash_xkey_0);
    s1 = aes_decrypt_round(s1, hash_xkey_0);
    s2 = aes_encrypt_round(s2, hash_xkey_0);
    s3 = aes_decrypt_round(s3, hash_xkey_0);

    s0 = aes_encrypt_round(s0, hash_xkey_1);
    s1 = aes_decrypt_round(s1, hash_xkey_1);
    s2 = aes_encrypt_round(s2, hash_xkey_1);
    s3 = aes_decrypt_round(s3, hash_xkey_1);

    write_block(hash, 0, s0);
    write_block(hash, 1, s1);
    write_block(hash, 2, s2);
    write_block(hash, 3, s3);
#endif
}

void hash_and_fill_aes_1r_x4(std::span<std::byte> scratchpad, AesState& hash, AesState& fill_state) {
    assert(scratchpad.size() % 64 == 0);
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
    uint8x16_t hs0 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_state_0.data()));
    uint8x16_t hs1 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_state_1.data()));
    uint8x16_t hs2 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_state_2.data()));
    uint8x16_t hs3 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_state_3.data()));

    uint8x16_t fs0 = vld1q_u8(reinterpret_cast<const uint8_t*>(fill_state.data() + 0));
    uint8x16_t fs1 = vld1q_u8(reinterpret_cast<const uint8_t*>(fill_state.data() + 16));
    uint8x16_t fs2 = vld1q_u8(reinterpret_cast<const uint8_t*>(fill_state.data() + 32));
    uint8x16_t fs3 = vld1q_u8(reinterpret_cast<const uint8_t*>(fill_state.data() + 48));

    const uint8x16_t fk0 = vld1q_u8(reinterpret_cast<const uint8_t*>(key1r_0.data()));
    const uint8x16_t fk1 = vld1q_u8(reinterpret_cast<const uint8_t*>(key1r_1.data()));
    const uint8x16_t fk2 = vld1q_u8(reinterpret_cast<const uint8_t*>(key1r_2.data()));
    const uint8x16_t fk3 = vld1q_u8(reinterpret_cast<const uint8_t*>(key1r_3.data()));

    const uint8x16_t xk0 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_xkey_0.data()));
    const uint8x16_t xk1 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_xkey_1.data()));

    const uint8x16_t zero = vdupq_n_u8(0);

    for (std::size_t offset = 0; offset < scratchpad.size(); offset += 64) {
        uint8x16_t sp0 = vld1q_u8(reinterpret_cast<const uint8_t*>(scratchpad.data() + offset + 0));
        uint8x16_t sp1 = vld1q_u8(reinterpret_cast<const uint8_t*>(scratchpad.data() + offset + 16));
        uint8x16_t sp2 = vld1q_u8(reinterpret_cast<const uint8_t*>(scratchpad.data() + offset + 32));
        uint8x16_t sp3 = vld1q_u8(reinterpret_cast<const uint8_t*>(scratchpad.data() + offset + 48));

        hs0 = veorq_u8(vaesmcq_u8(vaeseq_u8(hs0, zero)), sp0);
        hs1 = veorq_u8(vaesimcq_u8(vaesdq_u8(hs1, zero)), sp1);
        hs2 = veorq_u8(vaesmcq_u8(vaeseq_u8(hs2, zero)), sp2);
        hs3 = veorq_u8(vaesimcq_u8(vaesdq_u8(hs3, zero)), sp3);

        fs0 = veorq_u8(vaesimcq_u8(vaesdq_u8(fs0, zero)), fk0);
        fs1 = veorq_u8(vaesmcq_u8(vaeseq_u8(fs1, zero)), fk1);
        fs2 = veorq_u8(vaesimcq_u8(vaesdq_u8(fs2, zero)), fk2);
        fs3 = veorq_u8(vaesmcq_u8(vaeseq_u8(fs3, zero)), fk3);

        vst1q_u8(reinterpret_cast<uint8_t*>(scratchpad.data() + offset + 0), fs0);
        vst1q_u8(reinterpret_cast<uint8_t*>(scratchpad.data() + offset + 16), fs1);
        vst1q_u8(reinterpret_cast<uint8_t*>(scratchpad.data() + offset + 32), fs2);
        vst1q_u8(reinterpret_cast<uint8_t*>(scratchpad.data() + offset + 48), fs3);
    }

    vst1q_u8(reinterpret_cast<uint8_t*>(fill_state.data() + 0), fs0);
    vst1q_u8(reinterpret_cast<uint8_t*>(fill_state.data() + 16), fs1);
    vst1q_u8(reinterpret_cast<uint8_t*>(fill_state.data() + 32), fs2);
    vst1q_u8(reinterpret_cast<uint8_t*>(fill_state.data() + 48), fs3);

    hs0 = veorq_u8(vaesmcq_u8(vaeseq_u8(hs0, zero)), xk0);
    hs1 = veorq_u8(vaesimcq_u8(vaesdq_u8(hs1, zero)), xk0);
    hs2 = veorq_u8(vaesmcq_u8(vaeseq_u8(hs2, zero)), xk0);
    hs3 = veorq_u8(vaesimcq_u8(vaesdq_u8(hs3, zero)), xk0);

    hs0 = veorq_u8(vaesmcq_u8(vaeseq_u8(hs0, zero)), xk1);
    hs1 = veorq_u8(vaesimcq_u8(vaesdq_u8(hs1, zero)), xk1);
    hs2 = veorq_u8(vaesmcq_u8(vaeseq_u8(hs2, zero)), xk1);
    hs3 = veorq_u8(vaesimcq_u8(vaesdq_u8(hs3, zero)), xk1);

    vst1q_u8(reinterpret_cast<uint8_t*>(hash.data() + 0), hs0);
    vst1q_u8(reinterpret_cast<uint8_t*>(hash.data() + 16), hs1);
    vst1q_u8(reinterpret_cast<uint8_t*>(hash.data() + 32), hs2);
    vst1q_u8(reinterpret_cast<uint8_t*>(hash.data() + 48), hs3);
#else
    AesBlock hs0 = hash_state_0;
    AesBlock hs1 = hash_state_1;
    AesBlock hs2 = hash_state_2;
    AesBlock hs3 = hash_state_3;

    AesBlock fs0 = read_block(fill_state, 0);
    AesBlock fs1 = read_block(fill_state, 1);
    AesBlock fs2 = read_block(fill_state, 2);
    AesBlock fs3 = read_block(fill_state, 3);

    for (std::size_t offset = 0; offset < scratchpad.size(); offset += 64) {
        AesBlock sp0 = read_block_from_span(scratchpad, offset + 0);
        AesBlock sp1 = read_block_from_span(scratchpad, offset + 16);
        AesBlock sp2 = read_block_from_span(scratchpad, offset + 32);
        AesBlock sp3 = read_block_from_span(scratchpad, offset + 48);

        hs0 = aes_encrypt_round(hs0, sp0);
        hs1 = aes_decrypt_round(hs1, sp1);
        hs2 = aes_encrypt_round(hs2, sp2);
        hs3 = aes_decrypt_round(hs3, sp3);

        fs0 = aes_decrypt_round(fs0, key1r_0);
        fs1 = aes_encrypt_round(fs1, key1r_1);
        fs2 = aes_decrypt_round(fs2, key1r_2);
        fs3 = aes_encrypt_round(fs3, key1r_3);

        write_block_to_span(scratchpad, offset + 0, fs0);
        write_block_to_span(scratchpad, offset + 16, fs1);
        write_block_to_span(scratchpad, offset + 32, fs2);
        write_block_to_span(scratchpad, offset + 48, fs3);
    }

    write_block(fill_state, 0, fs0);
    write_block(fill_state, 1, fs1);
    write_block(fill_state, 2, fs2);
    write_block(fill_state, 3, fs3);

    hs0 = aes_encrypt_round(hs0, hash_xkey_0);
    hs1 = aes_decrypt_round(hs1, hash_xkey_0);
    hs2 = aes_encrypt_round(hs2, hash_xkey_0);
    hs3 = aes_decrypt_round(hs3, hash_xkey_0);

    hs0 = aes_encrypt_round(hs0, hash_xkey_1);
    hs1 = aes_decrypt_round(hs1, hash_xkey_1);
    hs2 = aes_encrypt_round(hs2, hash_xkey_1);
    hs3 = aes_decrypt_round(hs3, hash_xkey_1);

    write_block(hash, 0, hs0);
    write_block(hash, 1, hs1);
    write_block(hash, 2, hs2);
    write_block(hash, 3, hs3);
#endif
}

} // namespace armrx
