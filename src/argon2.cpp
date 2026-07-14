#include "armrx/argon2.hpp"

#include "armrx/blake2b.hpp"
#include "armrx/blake2_generator.hpp"
#include "armrx/superscalar.hpp"

#include <algorithm>
#include <cstdint>
#include <bit>
#include <stdexcept>
#include <vector>
#include <sys/mman.h>

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

void permute_block(Argon2Block& block) {
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
    void* ptr = ::mmap(nullptr, allocated_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        throw std::runtime_error("mmap failed for Argon2dCache allocation");
    }
    blocks_ = static_cast<Argon2Block*>(ptr);
    ::madvise(blocks_, allocated_size_, MADV_HUGEPAGE);
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
