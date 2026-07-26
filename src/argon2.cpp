#include "armrx/argon2.hpp"

#include "armrx/blake2b.hpp"
#include "armrx/blake2_generator.hpp"
#include "armrx/superscalar.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <bit>
#include <stdexcept>
#include <vector>
#include <sys/mman.h>
#include "armrx/virtual_memory.h"

namespace armrx {
namespace {

void append_le32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        output.push_back(static_cast<std::byte>(value >> (i * 8U)));
    }
}

[[nodiscard]] std::uint64_t load_le64(const std::byte* input) {
    std::uint64_t value{};
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(input[i])) << (8U * i);
    }
    return value;
}

[[nodiscard]] Argon2Block bytes_to_block(std::span<const std::byte> input) {
    Argon2Block block{};
    for (std::size_t word = 0; word < block.size(); ++word) {
        block[word] = load_le64(input.data() + word * 8U);
    }
    return block;
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

#if defined(__aarch64__) && defined(__ARM_NEON)

// NEON: process two Argon2 G-functions in parallel using uint64x2_t lanes.
// Lane 0 holds values for G-function A, lane 1 for G-function B.
static inline uint64x2_t blamka_add_neon(uint64x2_t a, uint64x2_t b) {
    // low32(a) * low32(b) → 64-bit product
    uint64x2_t prod = vmull_u32(vmovn_u64(a), vmovn_u64(b));
    // a + b + 2*prod
    return vaddq_u64(vaddq_u64(a, b), vshlq_n_u64(prod, 1));
}

static inline uint64x2_t rotr32_neon(uint64x2_t x) {
    return vreinterpretq_u64_u32(vrev64q_u32(vreinterpretq_u32_u64(x)));
}

static inline uint64x2_t rotr24_neon(uint64x2_t x) {
    return vsriq_n_u64(vshlq_n_u64(x, 40), x, 24);
}

static inline uint64x2_t rotr16_neon(uint64x2_t x) {
    return vsriq_n_u64(vshlq_n_u64(x, 48), x, 16);
}

static inline uint64x2_t rotr63_neon(uint64x2_t x) {
    return vsriq_n_u64(vshlq_n_u64(x, 1), x, 63);
}

// Process two G-functions (8 uint64_t values) in parallel.
// va = { a0, a1 }, vb = { b0, b1 }, vc = { c0, c1 }, vd = { d0, d1 }
static void gb_neon(uint64x2_t& va, uint64x2_t& vb, uint64x2_t& vc, uint64x2_t& vd) {
    va = blamka_add_neon(va, vb);
    vd = rotr32_neon(veorq_u64(vd, va));
    vc = blamka_add_neon(vc, vd);
    vb = rotr24_neon(veorq_u64(vb, vc));
    va = blamka_add_neon(va, vb);
    vd = rotr16_neon(veorq_u64(vd, va));
    vc = blamka_add_neon(vc, vd);
    vb = rotr63_neon(veorq_u64(vb, vc));
}

// NEON permute_16: processes 16 words as 8 gb_neon calls (4 column + 4 diagonal).
// Each gb_neon processes 2 G-functions at once, so we make half the calls.
static void permute_16_neon(uint64_t* words) {
    auto load = [&](int i0, int i1, int i2, int i3) {
        uint64x2_t va = vld1q_u64(words + i0);
        uint64x2_t vb = vld1q_u64(words + i1);
        uint64x2_t vc = vld1q_u64(words + i2);
        uint64x2_t vd = vld1q_u64(words + i3);
        return std::make_tuple(va, vb, vc, vd);
    };
    auto store = [&](int i0, int i1, int i2, int i3,
                     uint64x2_t va, uint64x2_t vb, uint64x2_t vc, uint64x2_t vd) {
        vst1q_u64(words + i0, va);
        vst1q_u64(words + i1, vb);
        vst1q_u64(words + i2, vc);
        vst1q_u64(words + i3, vd);
    };

    // Column step (first 4 scalar gb calls → 2 NEON calls)
    {
        auto [va, vb, vc, vd] = load(0, 4, 8, 12);
        gb_neon(va, vb, vc, vd);
        store(0, 4, 8, 12, va, vb, vc, vd);
    }
    {
        auto [va, vb, vc, vd] = load(2, 6, 10, 14);
        gb_neon(va, vb, vc, vd);
        store(2, 6, 10, 14, va, vb, vc, vd);
    }

	// Diagonal step (last 4 scalar gb calls → 2 NEON calls). Of the 8
	// register-pairs the diagonal rounds need — (0,1), (5,6), (10,11),
	// (15,12), (2,3), (7,4), (8,9), (13,14) — only (15,12) and (7,4) are
	// not at consecutive memory positions; the other 6 load/store with
	// plain vld1q_u64/vst1q_u64 exactly like the column step above. The two
	// scattered pairs are gathered into a vector register via
	// vcombine_u64(vld1_u64(a), vld1_u64(b)) (lane 0 = a, lane 1 = b) and
	// scattered back the same way on the way out — gb_neon() itself is
	// unchanged, only how its operands reach/leave the register differ from
	// the column step. This was previously believed to require falling back
	// to 4 sequential scalar gb() calls (see docs/experiments/argon2-neon-diagonal-vectorization.md
	// for the profiling that found this: those scalar calls alone were
	// ~38% of all Argon2dCache::initialize() cycles).
	{
		uint64x2_t va = vld1q_u64(words + 0);   // words[0], words[1]
		uint64x2_t vb = vld1q_u64(words + 5);   // words[5], words[6]
		uint64x2_t vc = vld1q_u64(words + 10);  // words[10], words[11]
		uint64x2_t vd = vcombine_u64(vld1_u64(words + 15), vld1_u64(words + 12)); // words[15], words[12]
		gb_neon(va, vb, vc, vd);
		vst1q_u64(words + 0, va);
		vst1q_u64(words + 5, vb);
		vst1q_u64(words + 10, vc);
		vst1_u64(words + 15, vget_low_u64(vd));
		vst1_u64(words + 12, vget_high_u64(vd));
	}
	{
		uint64x2_t va = vld1q_u64(words + 2);   // words[2], words[3]
		uint64x2_t vb = vcombine_u64(vld1_u64(words + 7), vld1_u64(words + 4)); // words[7], words[4]
		uint64x2_t vc = vld1q_u64(words + 8);   // words[8], words[9]
		uint64x2_t vd = vld1q_u64(words + 13);  // words[13], words[14]
		gb_neon(va, vb, vc, vd);
		vst1q_u64(words + 2, va);
		vst1_u64(words + 7, vget_low_u64(vb));
		vst1_u64(words + 4, vget_high_u64(vb));
		vst1q_u64(words + 8, vc);
		vst1q_u64(words + 13, vd);
	}
}

static void permute_block_neon(Argon2Block& block) {
    for (unsigned row = 0; row < 8; ++row)
        permute_16_neon(block.data() + 16U * row);
    for (unsigned column = 0; column < 8; ++column) {
        std::array<std::uint64_t, 16> words{};
        for (unsigned row = 0; row < 8; ++row) {
            words[2U * row] = block[16U * row + 2U * column];
            words[2U * row + 1U] = block[16U * row + 2U * column + 1U];
        }
        permute_16_neon(words.data());
        for (unsigned row = 0; row < 8; ++row) {
            block[16U * row + 2U * column] = words[2U * row];
            block[16U * row + 2U * column + 1U] = words[2U * row + 1U];
        }
    }
}

#endif // __aarch64__ && __ARM_NEON

void permute_block(Argon2Block& block) {
#if defined(__aarch64__) && defined(__ARM_NEON)
    permute_block_neon(block);
#else
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
#endif
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

Argon2dCache::Argon2dCache(std::size_t memory_blocks, std::size_t passes)
    : passes_(passes), memory_blocks_(memory_blocks) {
    if (memory_blocks < 8U || (memory_blocks % 4U) != 0U || passes == 0U) {
        throw std::invalid_argument{"Argon2d cache needs at least 8 blocks, a multiple of 4, and one pass"};
    }

    allocated_size_ = memory_blocks * sizeof(Argon2Block);
    // Try MAP_HUGETLB first (true huge pages)
    void* ptr = allocLargePagesMemory(allocated_size_);
    if (!ptr) {
        // Fallback: plain anonymous + MADV_HUGEPAGE (relies on THP)
        ptr = ::mmap(nullptr, allocated_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            throw std::runtime_error("mmap failed for Argon2dCache allocation");
        }
        ::madvise(ptr, allocated_size_, MADV_HUGEPAGE);
    }
    blocks_ = static_cast<Argon2Block*>(ptr);
    // Prefault writable pages now rather than on first touch during
    // initialize() -- doesn't help steady-state hashrate, but shortens the
    // seed-rotation pause (new job/seed key) by moving the fault cost here.
    // Mirrors vm.cpp's scratchpad prefault; this cache allocation didn't
    // have it (docs/plans/experimental-performance-ideas-20260725.md #10).
#if defined(MADV_POPULATE_WRITE)
    ::madvise(blocks_, allocated_size_, MADV_POPULATE_WRITE);
#else
    std::memset(blocks_, 0, allocated_size_);
#endif
}

Argon2dCache::~Argon2dCache() {
    if (blocks_ != nullptr) {
        ::munmap(blocks_, allocated_size_);
        blocks_ = nullptr;
    }
}

void Argon2dCache::initialize(std::span<const std::byte> key) {
    constexpr std::array<std::byte, 8> salt{
        std::byte{'R'}, std::byte{'a'}, std::byte{'n'}, std::byte{'d'},
        std::byte{'o'}, std::byte{'m'}, std::byte{'X'}, std::byte{0x03}};
    std::vector<std::byte> prehash;
    prehash.reserve(40U + key.size());
    append_le32(prehash, 1); // lanes
    append_le32(prehash, 0); // RandomX only initializes memory; no output tag
    append_le32(prehash, static_cast<std::uint32_t>(memory_blocks_));
    append_le32(prehash, static_cast<std::uint32_t>(passes_));
    append_le32(prehash, 0x13); // Argon2 v1.3
    append_le32(prehash, 0); // Argon2d
    append_le32(prehash, static_cast<std::uint32_t>(key.size()));
    prehash.insert(prehash.end(), key.begin(), key.end());
    append_le32(prehash, static_cast<std::uint32_t>(salt.size()));
    prehash.insert(prehash.end(), salt.begin(), salt.end());
    append_le32(prehash, 0); // secret length
    append_le32(prehash, 0); // associated-data length
    const auto h0 = blake2b(prehash, 64);

    for (std::uint32_t index = 0; index < 2; ++index) {
        std::vector<std::byte> input = h0;
        append_le32(input, index);
        append_le32(input, 0); // lane
        blocks_[index] = bytes_to_block(argon2_hprime(input, 1024));
    }

    const std::size_t segment_length = memory_blocks_ / 4U;
    for (std::size_t pass = 0; pass < passes_; ++pass) {
        for (std::size_t slice = 0; slice < 4U; ++slice) {
            const std::size_t start_index = (pass == 0U && slice == 0U) ? 2U : 0U;
            for (std::size_t index = start_index; index < segment_length; ++index) {
                const std::size_t current = slice * segment_length + index;
                const std::size_t previous = current == 0U ? memory_blocks_ - 1U : current - 1U;
                const auto j1 = static_cast<std::uint32_t>(blocks_[previous][0]);
                const std::size_t reference_area = pass == 0U
                    ? slice * segment_length + index - 1U
                    : memory_blocks_ - segment_length + index - 1U;
                const auto square = static_cast<std::uint64_t>(j1) * j1;
                const auto x = square >> 32U;
                const auto y = (reference_area * x) >> 32U;
                const auto relative = reference_area - 1U - y;
                const std::size_t start_position = pass == 0U ? 0U
                    : (slice == 3U ? 0U : (slice + 1U) * segment_length);
                const std::size_t reference = (start_position + relative) % memory_blocks_;
                blocks_[current] = argon2_compress(blocks_[previous], blocks_[reference],
                                                   pass == 0U ? nullptr : &blocks_[current]);
            }
        }
    }

    reciprocal_cache_.clear();
    Blake2Generator gen(key.data(), key.size());
    for (std::size_t i = 0; i < kRandomXCacheAccesses; ++i) {
        generate_superscalar(programs_[i], gen);
        for (std::uint32_t j = 0; j < programs_[i].size(); ++j) {
            auto& instr = programs_[i](j);
            if (static_cast<SuperscalarInstructionType>(instr.opcode) == SuperscalarInstructionType::IMUL_RCP) {
                auto rcp = randomx_reciprocal(instr.getImm32());
                instr.setImm32(static_cast<std::uint32_t>(reciprocal_cache_.size()));
                reciprocal_cache_.push_back(rcp);
            }
        }
    }
}

} // namespace armrx
