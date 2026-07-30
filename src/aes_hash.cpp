#include "armrx/aes_hash.hpp"
#include "armrx/aes_keys.hpp"
#include "armrx/assert.hpp"
#include <algorithm>
#include <cstring>
#include <cstdint>

namespace armrx {
namespace {

// AesHash1R's own initial state and finalization keys — unique to this file,
// not shared with AesGenerator1R/4R (those live in aes_keys.hpp).
constexpr AesBlock hash_state_0 = detail::build_aes_key(0xd7983aad, 0xcc82db47, 0x9fa856de, 0x92b52c0d);
constexpr AesBlock hash_state_1 = detail::build_aes_key(0xace78057, 0xf59e125a, 0x15c7b798, 0x338d996e);
constexpr AesBlock hash_state_2 = detail::build_aes_key(0xe8a07ce4, 0x5079506b, 0xae62c7d0, 0x6a770017);
constexpr AesBlock hash_state_3 = detail::build_aes_key(0x7e994948, 0x79a10005, 0x07ad828d, 0x630a240c);

constexpr AesBlock hash_xkey_0 = detail::build_aes_key(0x06890201, 0x90dc56bf, 0x8b24949f, 0xf6fa8389);
constexpr AesBlock hash_xkey_1 = detail::build_aes_key(0xed18f99b, 0xee1043c6, 0x51f4e03c, 0x61b263d1);

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
    ARMRX_ASSERT(output.size() % 64 == 0, "output size must be multiple of 64");
    AesBlock s0 = read_block(state, 0);
    AesBlock s1 = read_block(state, 1);
    AesBlock s2 = read_block(state, 2);
    AesBlock s3 = read_block(state, 3);

    for (std::size_t offset = 0; offset < output.size(); offset += 64) {
#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_TTABLE_AES)
        // Scalar T-table transforms (same algorithm, bit-identical), then NEON batch AddRoundKey
        s0 = decrypt_transform(s0);
        s1 = encrypt_transform(s1);
        s2 = decrypt_transform(s2);
        s3 = encrypt_transform(s3);
        {
            const uint8x16_t k0 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey0.data()));
            const uint8x16_t k1 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey1.data()));
            const uint8x16_t k2 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey2.data()));
            const uint8x16_t k3 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey3.data()));
            uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s0.data())), k0);
            uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s1.data())), k1);
            uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s2.data())), k2);
            uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s3.data())), k3);
            vst1q_u8(reinterpret_cast<uint8_t*>(s0.data()), v0);
            vst1q_u8(reinterpret_cast<uint8_t*>(s1.data()), v1);
            vst1q_u8(reinterpret_cast<uint8_t*>(s2.data()), v2);
            vst1q_u8(reinterpret_cast<uint8_t*>(s3.data()), v3);
        }
#else
        s0 = aes_decrypt_round(s0, kAesGen1RKey0);
        s1 = aes_encrypt_round(s1, kAesGen1RKey1);
        s2 = aes_decrypt_round(s2, kAesGen1RKey2);
        s3 = aes_encrypt_round(s3, kAesGen1RKey3);
#endif

        write_block_to_span(output, offset + 0, s0);
        write_block_to_span(output, offset + 16, s1);
        write_block_to_span(output, offset + 32, s2);
        write_block_to_span(output, offset + 48, s3);
    }

    write_block(state, 0, s0);
    write_block(state, 1, s1);
    write_block(state, 2, s2);
    write_block(state, 3, s3);
}

void fill_aes_4r_x4(AesState& state, std::span<std::byte> output) {
    ARMRX_ASSERT(output.size() % 64 == 0, "output size must be multiple of 64");
    AesBlock s0 = read_block(state, 0);
    AesBlock s1 = read_block(state, 1);
    AesBlock s2 = read_block(state, 2);
    AesBlock s3 = read_block(state, 3);

    for (std::size_t offset = 0; offset < output.size(); offset += 64) {
#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_TTABLE_AES)
        // First 4 rounds: s0, s2 decrypt; s1, s3 encrypt; same key per pair
        for (int ri = 0; ri < 4; ++ri) {
            const auto& key = (ri == 0) ? kAesGen4RKey0 : (ri == 1) ? kAesGen4RKey1
                            : (ri == 2) ? kAesGen4RKey2 : kAesGen4RKey3;
            s0 = decrypt_transform(s0);
            s1 = encrypt_transform(s1);
            {
                const uint8x16_t kv = vld1q_u8(reinterpret_cast<const uint8_t*>(key.data()));
                uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s0.data())), kv);
                uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s1.data())), kv);
                vst1q_u8(reinterpret_cast<uint8_t*>(s0.data()), v0);
                vst1q_u8(reinterpret_cast<uint8_t*>(s1.data()), v1);
            }
        }
        // Second 4 rounds: only s2, s3 active
        for (int ri = 0; ri < 4; ++ri) {
            const auto& key = (ri == 0) ? kAesGen4RKey4 : (ri == 1) ? kAesGen4RKey5
                            : (ri == 2) ? kAesGen4RKey6 : kAesGen4RKey7;
            s2 = decrypt_transform(s2);
            s3 = encrypt_transform(s3);
            {
                const uint8x16_t kv = vld1q_u8(reinterpret_cast<const uint8_t*>(key.data()));
                uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s2.data())), kv);
                uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s3.data())), kv);
                vst1q_u8(reinterpret_cast<uint8_t*>(s2.data()), v2);
                vst1q_u8(reinterpret_cast<uint8_t*>(s3.data()), v3);
            }
        }
#else
        for (const auto& key : {kAesGen4RKey0, kAesGen4RKey1, kAesGen4RKey2, kAesGen4RKey3}) {
            s0 = aes_decrypt_round(s0, key);
            s1 = aes_encrypt_round(s1, key);
        }
        for (const auto& key : {kAesGen4RKey4, kAesGen4RKey5, kAesGen4RKey6, kAesGen4RKey7}) {
            s2 = aes_decrypt_round(s2, key);
            s3 = aes_encrypt_round(s3, key);
        }
#endif

        write_block_to_span(output, offset + 0, s0);
        write_block_to_span(output, offset + 16, s1);
        write_block_to_span(output, offset + 32, s2);
        write_block_to_span(output, offset + 48, s3);
    }

    write_block(state, 0, s0);
    write_block(state, 1, s1);
    write_block(state, 2, s2);
    write_block(state, 3, s3);
}

void hash_aes_1r_x4(std::span<const std::byte> input, AesState& hash) {
    ARMRX_ASSERT(input.size() % 64 == 0, "input size must be multiple of 64");
    AesBlock s0 = hash_state_0;
    AesBlock s1 = hash_state_1;
    AesBlock s2 = hash_state_2;
    AesBlock s3 = hash_state_3;

    for (std::size_t offset = 0; offset < input.size(); offset += 64) {
        AesBlock in0 = read_block_from_span(input, offset + 0);
        AesBlock in1 = read_block_from_span(input, offset + 16);
        AesBlock in2 = read_block_from_span(input, offset + 32);
        AesBlock in3 = read_block_from_span(input, offset + 48);

#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_TTABLE_AES)
        s0 = encrypt_transform(s0);
        s1 = decrypt_transform(s1);
        s2 = encrypt_transform(s2);
        s3 = decrypt_transform(s3);
        {
            const uint8x16_t k0 = vld1q_u8(reinterpret_cast<const uint8_t*>(in0.data()));
            const uint8x16_t k1 = vld1q_u8(reinterpret_cast<const uint8_t*>(in1.data()));
            const uint8x16_t k2 = vld1q_u8(reinterpret_cast<const uint8_t*>(in2.data()));
            const uint8x16_t k3 = vld1q_u8(reinterpret_cast<const uint8_t*>(in3.data()));
            uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s0.data())), k0);
            uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s1.data())), k1);
            uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s2.data())), k2);
            uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s3.data())), k3);
            vst1q_u8(reinterpret_cast<uint8_t*>(s0.data()), v0);
            vst1q_u8(reinterpret_cast<uint8_t*>(s1.data()), v1);
            vst1q_u8(reinterpret_cast<uint8_t*>(s2.data()), v2);
            vst1q_u8(reinterpret_cast<uint8_t*>(s3.data()), v3);
        }
#else
        s0 = aes_encrypt_round(s0, in0);
        s1 = aes_decrypt_round(s1, in1);
        s2 = aes_encrypt_round(s2, in2);
        s3 = aes_decrypt_round(s3, in3);
#endif
    }

#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_TTABLE_AES)
    // Finalization round 1
    s0 = encrypt_transform(s0);
    s1 = decrypt_transform(s1);
    s2 = encrypt_transform(s2);
    s3 = decrypt_transform(s3);
    {
        const uint8x16_t k0 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_xkey_0.data()));
        uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s0.data())), k0);
        uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s1.data())), k0);
        uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s2.data())), k0);
        uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s3.data())), k0);
        vst1q_u8(reinterpret_cast<uint8_t*>(s0.data()), v0);
        vst1q_u8(reinterpret_cast<uint8_t*>(s1.data()), v1);
        vst1q_u8(reinterpret_cast<uint8_t*>(s2.data()), v2);
        vst1q_u8(reinterpret_cast<uint8_t*>(s3.data()), v3);
    }
    // Finalization round 2
    s0 = encrypt_transform(s0);
    s1 = decrypt_transform(s1);
    s2 = encrypt_transform(s2);
    s3 = decrypt_transform(s3);
    {
        const uint8x16_t k1 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_xkey_1.data()));
        uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s0.data())), k1);
        uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s1.data())), k1);
        uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s2.data())), k1);
        uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(s3.data())), k1);
        vst1q_u8(reinterpret_cast<uint8_t*>(s0.data()), v0);
        vst1q_u8(reinterpret_cast<uint8_t*>(s1.data()), v1);
        vst1q_u8(reinterpret_cast<uint8_t*>(s2.data()), v2);
        vst1q_u8(reinterpret_cast<uint8_t*>(s3.data()), v3);
    }
#else
    s0 = aes_encrypt_round(s0, hash_xkey_0);
    s1 = aes_decrypt_round(s1, hash_xkey_0);
    s2 = aes_encrypt_round(s2, hash_xkey_0);
    s3 = aes_decrypt_round(s3, hash_xkey_0);

    s0 = aes_encrypt_round(s0, hash_xkey_1);
    s1 = aes_decrypt_round(s1, hash_xkey_1);
    s2 = aes_encrypt_round(s2, hash_xkey_1);
    s3 = aes_decrypt_round(s3, hash_xkey_1);
#endif

    write_block(hash, 0, s0);
    write_block(hash, 1, s1);
    write_block(hash, 2, s2);
    write_block(hash, 3, s3);
}

void hash_and_fill_aes_1r_x4(std::span<std::byte> scratchpad, AesState& hash, AesState& fill_state) {
    ARMRX_ASSERT(scratchpad.size() % 64 == 0, "scratchpad size must be multiple of 64");
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

#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_TTABLE_AES)
        // Hash phase: s0=encrypt, s1=decrypt, s2=encrypt, s3=decrypt with input blocks as keys
        hs0 = encrypt_transform(hs0);
        hs1 = decrypt_transform(hs1);
        hs2 = encrypt_transform(hs2);
        hs3 = decrypt_transform(hs3);
        {
            const uint8x16_t k0 = vld1q_u8(reinterpret_cast<const uint8_t*>(sp0.data()));
            const uint8x16_t k1 = vld1q_u8(reinterpret_cast<const uint8_t*>(sp1.data()));
            const uint8x16_t k2 = vld1q_u8(reinterpret_cast<const uint8_t*>(sp2.data()));
            const uint8x16_t k3 = vld1q_u8(reinterpret_cast<const uint8_t*>(sp3.data()));
            uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs0.data())), k0);
            uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs1.data())), k1);
            uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs2.data())), k2);
            uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs3.data())), k3);
            vst1q_u8(reinterpret_cast<uint8_t*>(hs0.data()), v0);
            vst1q_u8(reinterpret_cast<uint8_t*>(hs1.data()), v1);
            vst1q_u8(reinterpret_cast<uint8_t*>(hs2.data()), v2);
            vst1q_u8(reinterpret_cast<uint8_t*>(hs3.data()), v3);
        }
        // Fill phase: s0=decrypt, s1=encrypt, s2=decrypt, s3=encrypt with fixed keys
        fs0 = decrypt_transform(fs0);
        fs1 = encrypt_transform(fs1);
        fs2 = decrypt_transform(fs2);
        fs3 = encrypt_transform(fs3);
        {
            const uint8x16_t fk0 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey0.data()));
            const uint8x16_t fk1 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey1.data()));
            const uint8x16_t fk2 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey2.data()));
            const uint8x16_t fk3 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey3.data()));
            uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(fs0.data())), fk0);
            uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(fs1.data())), fk1);
            uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(fs2.data())), fk2);
            uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(fs3.data())), fk3);
            vst1q_u8(reinterpret_cast<uint8_t*>(fs0.data()), v0);
            vst1q_u8(reinterpret_cast<uint8_t*>(fs1.data()), v1);
            vst1q_u8(reinterpret_cast<uint8_t*>(fs2.data()), v2);
            vst1q_u8(reinterpret_cast<uint8_t*>(fs3.data()), v3);
        }
#else
        hs0 = aes_encrypt_round(hs0, sp0);
        hs1 = aes_decrypt_round(hs1, sp1);
        hs2 = aes_encrypt_round(hs2, sp2);
        hs3 = aes_decrypt_round(hs3, sp3);

        fs0 = aes_decrypt_round(fs0, kAesGen1RKey0);
        fs1 = aes_encrypt_round(fs1, kAesGen1RKey1);
        fs2 = aes_decrypt_round(fs2, kAesGen1RKey2);
        fs3 = aes_encrypt_round(fs3, kAesGen1RKey3);
#endif

        write_block_to_span(scratchpad, offset + 0, fs0);
        write_block_to_span(scratchpad, offset + 16, fs1);
        write_block_to_span(scratchpad, offset + 32, fs2);
        write_block_to_span(scratchpad, offset + 48, fs3);
    }

    write_block(fill_state, 0, fs0);
    write_block(fill_state, 1, fs1);
    write_block(fill_state, 2, fs2);
    write_block(fill_state, 3, fs3);

#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_TTABLE_AES)
    // Finalization round 1
    hs0 = encrypt_transform(hs0);
    hs1 = decrypt_transform(hs1);
    hs2 = encrypt_transform(hs2);
    hs3 = decrypt_transform(hs3);
    {
        const uint8x16_t k0 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_xkey_0.data()));
        uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs0.data())), k0);
        uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs1.data())), k0);
        uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs2.data())), k0);
        uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs3.data())), k0);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs0.data()), v0);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs1.data()), v1);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs2.data()), v2);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs3.data()), v3);
    }
    // Finalization round 2
    hs0 = encrypt_transform(hs0);
    hs1 = decrypt_transform(hs1);
    hs2 = encrypt_transform(hs2);
    hs3 = decrypt_transform(hs3);
    {
        const uint8x16_t k1 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_xkey_1.data()));
        uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs0.data())), k1);
        uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs1.data())), k1);
        uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs2.data())), k1);
        uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs3.data())), k1);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs0.data()), v0);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs1.data()), v1);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs2.data()), v2);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs3.data()), v3);
    }
#else
    hs0 = aes_encrypt_round(hs0, hash_xkey_0);
    hs1 = aes_decrypt_round(hs1, hash_xkey_0);
    hs2 = aes_encrypt_round(hs2, hash_xkey_0);
    hs3 = aes_decrypt_round(hs3, hash_xkey_0);

    hs0 = aes_encrypt_round(hs0, hash_xkey_1);
    hs1 = aes_decrypt_round(hs1, hash_xkey_1);
    hs2 = aes_encrypt_round(hs2, hash_xkey_1);
    hs3 = aes_decrypt_round(hs3, hash_xkey_1);
#endif

    write_block(hash, 0, hs0);
    write_block(hash, 1, hs1);
    write_block(hash, 2, hs2);
    write_block(hash, 3, hs3);
}

void hash_and_fill_aes_interleaved_x4(
    std::span<const std::byte> hash_scratchpad,
    std::span<std::byte> fill_scratchpad,
    AesState& hash_state,
    AesState& fill_state
) {
    ARMRX_ASSERT(hash_scratchpad.size() == fill_scratchpad.size(),
                 "hash and fill scratchpads must be the same size");
    ARMRX_ASSERT(hash_scratchpad.size() % 64 == 0,
                 "scratchpad size must be multiple of 64");
    AesBlock hs0 = hash_state_0;
    AesBlock hs1 = hash_state_1;
    AesBlock hs2 = hash_state_2;
    AesBlock hs3 = hash_state_3;

    AesBlock fs0 = read_block(fill_state, 0);
    AesBlock fs1 = read_block(fill_state, 1);
    AesBlock fs2 = read_block(fill_state, 2);
    AesBlock fs3 = read_block(fill_state, 3);

    for (std::size_t offset = 0; offset < hash_scratchpad.size(); offset += 64) {
        AesBlock sp0 = read_block_from_span(hash_scratchpad, offset + 0);
        AesBlock sp1 = read_block_from_span(hash_scratchpad, offset + 16);
        AesBlock sp2 = read_block_from_span(hash_scratchpad, offset + 32);
        AesBlock sp3 = read_block_from_span(hash_scratchpad, offset + 48);

#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_TTABLE_AES)
        // Hash phase: s0=encrypt, s1=decrypt, s2=encrypt, s3=decrypt with input blocks as keys
        hs0 = encrypt_transform(hs0);
        hs1 = decrypt_transform(hs1);
        hs2 = encrypt_transform(hs2);
        hs3 = decrypt_transform(hs3);
        {
            const uint8x16_t k0 = vld1q_u8(reinterpret_cast<const uint8_t*>(sp0.data()));
            const uint8x16_t k1 = vld1q_u8(reinterpret_cast<const uint8_t*>(sp1.data()));
            const uint8x16_t k2 = vld1q_u8(reinterpret_cast<const uint8_t*>(sp2.data()));
            const uint8x16_t k3 = vld1q_u8(reinterpret_cast<const uint8_t*>(sp3.data()));
            uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs0.data())), k0);
            uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs1.data())), k1);
            uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs2.data())), k2);
            uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs3.data())), k3);
            vst1q_u8(reinterpret_cast<uint8_t*>(hs0.data()), v0);
            vst1q_u8(reinterpret_cast<uint8_t*>(hs1.data()), v1);
            vst1q_u8(reinterpret_cast<uint8_t*>(hs2.data()), v2);
            vst1q_u8(reinterpret_cast<uint8_t*>(hs3.data()), v3);
        }
        // Fill phase: s0=decrypt, s1=encrypt, s2=decrypt, s3=encrypt with fixed keys
        fs0 = decrypt_transform(fs0);
        fs1 = encrypt_transform(fs1);
        fs2 = decrypt_transform(fs2);
        fs3 = encrypt_transform(fs3);
        {
            const uint8x16_t fk0 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey0.data()));
            const uint8x16_t fk1 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey1.data()));
            const uint8x16_t fk2 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey2.data()));
            const uint8x16_t fk3 = vld1q_u8(reinterpret_cast<const uint8_t*>(kAesGen1RKey3.data()));
            uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(fs0.data())), fk0);
            uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(fs1.data())), fk1);
            uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(fs2.data())), fk2);
            uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(fs3.data())), fk3);
            vst1q_u8(reinterpret_cast<uint8_t*>(fs0.data()), v0);
            vst1q_u8(reinterpret_cast<uint8_t*>(fs1.data()), v1);
            vst1q_u8(reinterpret_cast<uint8_t*>(fs2.data()), v2);
            vst1q_u8(reinterpret_cast<uint8_t*>(fs3.data()), v3);
        }
#else
        hs0 = aes_encrypt_round(hs0, sp0);
        hs1 = aes_decrypt_round(hs1, sp1);
        hs2 = aes_encrypt_round(hs2, sp2);
        hs3 = aes_decrypt_round(hs3, sp3);

        fs0 = aes_decrypt_round(fs0, kAesGen1RKey0);
        fs1 = aes_encrypt_round(fs1, kAesGen1RKey1);
        fs2 = aes_decrypt_round(fs2, kAesGen1RKey2);
        fs3 = aes_encrypt_round(fs3, kAesGen1RKey3);
#endif

        write_block_to_span(fill_scratchpad, offset + 0, fs0);
        write_block_to_span(fill_scratchpad, offset + 16, fs1);
        write_block_to_span(fill_scratchpad, offset + 32, fs2);
        write_block_to_span(fill_scratchpad, offset + 48, fs3);
    }

    write_block(fill_state, 0, fs0);
    write_block(fill_state, 1, fs1);
    write_block(fill_state, 2, fs2);
    write_block(fill_state, 3, fs3);

#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_TTABLE_AES)
    // Finalization round 1
    hs0 = encrypt_transform(hs0);
    hs1 = decrypt_transform(hs1);
    hs2 = encrypt_transform(hs2);
    hs3 = decrypt_transform(hs3);
    {
        const uint8x16_t k0 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_xkey_0.data()));
        uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs0.data())), k0);
        uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs1.data())), k0);
        uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs2.data())), k0);
        uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs3.data())), k0);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs0.data()), v0);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs1.data()), v1);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs2.data()), v2);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs3.data()), v3);
    }
    // Finalization round 2
    hs0 = encrypt_transform(hs0);
    hs1 = decrypt_transform(hs1);
    hs2 = encrypt_transform(hs2);
    hs3 = decrypt_transform(hs3);
    {
        const uint8x16_t k1 = vld1q_u8(reinterpret_cast<const uint8_t*>(hash_xkey_1.data()));
        uint8x16_t v0 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs0.data())), k1);
        uint8x16_t v1 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs1.data())), k1);
        uint8x16_t v2 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs2.data())), k1);
        uint8x16_t v3 = veorq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(hs3.data())), k1);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs0.data()), v0);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs1.data()), v1);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs2.data()), v2);
        vst1q_u8(reinterpret_cast<uint8_t*>(hs3.data()), v3);
    }
#else
    hs0 = aes_encrypt_round(hs0, hash_xkey_0);
    hs1 = aes_decrypt_round(hs1, hash_xkey_0);
    hs2 = aes_encrypt_round(hs2, hash_xkey_0);
    hs3 = aes_decrypt_round(hs3, hash_xkey_0);

    hs0 = aes_encrypt_round(hs0, hash_xkey_1);
    hs1 = aes_decrypt_round(hs1, hash_xkey_1);
    hs2 = aes_encrypt_round(hs2, hash_xkey_1);
    hs3 = aes_decrypt_round(hs3, hash_xkey_1);
#endif

    write_block(hash_state, 0, hs0);
    write_block(hash_state, 1, hs1);
    write_block(hash_state, 2, hs2);
    write_block(hash_state, 3, hs3);
}

} // namespace armrx
