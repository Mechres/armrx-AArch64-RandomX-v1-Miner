#include "armrx/vm.hpp"
#include "armrx/superscalar.hpp"
#include "armrx/blake2b.hpp"
#include "armrx/dataset.hpp"
#include "armrx/aes_generator.hpp"
#include "armrx/assert.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <sys/mman.h>
#include <unistd.h>
#include <chrono>

namespace armrx {
namespace {

constexpr std::uint64_t mantissaSize = 52;
constexpr std::uint64_t exponentSize = 11;
constexpr std::uint64_t mantissaMask = (1ULL << mantissaSize) - 1;
constexpr std::uint64_t exponentMask = (1ULL << exponentSize) - 1;
constexpr int exponentBias = 1023;
constexpr int dynamicExponentBits = 4;
constexpr int staticExponentBits = 4;
constexpr std::uint64_t constExponentBits = 0x300;
constexpr std::uint64_t dynamicMantissaMask = (1ULL << (mantissaSize + dynamicExponentBits)) - 1;

static inline std::uint64_t getSmallPositiveFloatBits(std::uint64_t entropy) {
    auto exponent = entropy >> 59; // 0..31
    auto mantissa = entropy & mantissaMask;
    exponent += exponentBias;
    exponent &= exponentMask;
    exponent <<= mantissaSize;
    return exponent | mantissa;
}

static inline std::uint64_t getStaticExponent(std::uint64_t entropy) {
    auto exponent = constExponentBits;
    exponent |= (entropy >> (64 - staticExponentBits)) << dynamicExponentBits;
    exponent <<= mantissaSize;
    return exponent;
}

static inline std::uint64_t getFloatMask(std::uint64_t entropy) {
    constexpr std::uint64_t mask22bit = (1ULL << 22) - 1;
    return (entropy & mask22bit) | getStaticExponent(entropy);
}

static inline std::uint64_t signExtend2sCompl(std::uint32_t x) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(x)));
}

static inline std::uint64_t rotr(std::uint64_t value, unsigned int shift) {
    return std::rotr(value, shift);
}

static inline std::uint64_t mulh(std::uint64_t a, std::uint64_t b) {
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b)) >> 64);
}

static inline std::int64_t smulh(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>((static_cast<__int128>(a) * static_cast<__int128>(b)) >> 64);
}

static inline void rx_set_rounding_mode(std::uint32_t mode) {
    static std::uint32_t last_mode = 0xFF; // invalid sentinel
    mode &= 3;
    if (mode == last_mode) return;
    last_mode = mode;
    switch (mode) {
        case 1: std::fesetround(FE_DOWNWARD); break;
        case 2: std::fesetround(FE_UPWARD); break;
        case 3: std::fesetround(FE_TOWARDZERO); break;
        default: std::fesetround(FE_TONEAREST); break;
    }
}

static inline double unsigned32ToSigned2sCompl(std::uint32_t x) {
    return static_cast<double>(static_cast<std::int32_t>(x));
}

static inline std::uint32_t load32(const void* src) {
    std::uint32_t val;
    std::memcpy(&val, src, 4);
    return val;
}

static inline std::uint64_t load64(const void* src) {
    std::uint64_t val;
    std::memcpy(&val, src, 8);
    return val;
}

static inline void store64(void* dst, std::uint64_t val) {
    std::memcpy(dst, &val, 8);
}

constexpr std::uint32_t kScratchpadL1Mask = 16376U;
constexpr std::uint32_t kScratchpadL2Mask = 262136U;
constexpr std::uint32_t kScratchpadL3Mask = 2097144U;
constexpr std::uint32_t kScratchpadL3Mask64 = 2097088U;

static inline std::byte* getScratchpadAddress(const InstructionByteCode& ibc, std::byte* scratchpad) {
    std::uint32_t addr = (static_cast<std::uint32_t>(*ibc.isrc) + static_cast<std::uint32_t>(ibc.imm)) & ibc.memMask;
    return scratchpad + addr;
}

// Opcode cumulative frequencies ceiling
constexpr int ceil_IADD_RS = 16;
constexpr int ceil_IADD_M = 23;
constexpr int ceil_ISUB_R = 39;
constexpr int ceil_ISUB_M = 46;
constexpr int ceil_IMUL_R = 62;
constexpr int ceil_IMUL_M = 66;
constexpr int ceil_IMULH_R = 70;
constexpr int ceil_IMULH_M = 71;
constexpr int ceil_ISMULH_R = 75;
constexpr int ceil_ISMULH_M = 76;
constexpr int ceil_IMUL_RCP = 84;
constexpr int ceil_INEG_R = 86;
constexpr int ceil_IXOR_R = 101;
constexpr int ceil_IXOR_M = 106;
constexpr int ceil_IROR_R = 114;
constexpr int ceil_IROL_R = 116;
constexpr int ceil_ISWAP_R = 120;
constexpr int ceil_FSWAP_R = 124;
constexpr int ceil_FADD_R = 140;
constexpr int ceil_FADD_M = 145;
constexpr int ceil_FSUB_R = 161;
constexpr int ceil_FSUB_M = 166;
constexpr int ceil_FSCAL_R = 172;
constexpr int ceil_FMUL_R = 204;
constexpr int ceil_FDIV_M = 208;
constexpr int ceil_FSQRT_R = 214;
constexpr int ceil_CBRANCH = 239;
constexpr int ceil_CFROUND = 240;
constexpr int ceil_ISTORE = 256;
constexpr int ceil_NOP = 256;

static inline bool isZeroOrPowerOf2(std::uint64_t x) {
    return (x & (x - 1)) == 0;
}

static std::uint32_t map_to_randomx_flags(std::uint32_t flags) {
    std::uint32_t rx_flags = 0;
    if (flags & kRandOMXFlagFullMem) {
        rx_flags |= 4; // RANDOMX_FLAG_FULL_MEM
    }
    if (flags & kRandOMXFlagHardAes) {
        rx_flags |= 2; // RANDOMX_FLAG_HARD_AES
    }
    if (flags & kRandOMXFlagJit) {
        rx_flags |= 8; // RANDOMX_FLAG_JIT
    }
    return rx_flags;
}

} // namespace

VirtualMachine::VirtualMachine(std::uint32_t flags) : flags_(flags) {
    // Allocate 2 MiB scratchpad via mmap for direct huge-page control
    const std::size_t sp_size = 2097152U;
    scratchpad_data_ = static_cast<std::byte*>(
        ::mmap(nullptr, sp_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (scratchpad_data_ == MAP_FAILED) {
        throw std::bad_alloc();
    }
    scratchpad_size_ = sp_size;
    ::madvise(scratchpad_data_, sp_size, MADV_HUGEPAGE);
#ifdef ARMRX_HAVE_JIT
    if (flags_ & kRandOMXFlagJit) {
        jit_ = std::make_unique<JitCompilerA64>();
        jit_->setFlags(map_to_randomx_flags(flags_));
    }
#endif
}

VirtualMachine::~VirtualMachine() {
    if (scratchpad_data_) {
        ::munmap(scratchpad_data_, scratchpad_size_);
        scratchpad_data_ = nullptr;
        scratchpad_size_ = 0;
    }
}

void VirtualMachine::set_cache(const Argon2dCache* cache) {
    cache_ = cache;
#ifdef ARMRX_HAVE_JIT
    if (jit_ && cache) {
        jit_->enableWriting();
        jit_->generateSuperscalarHash(cache->programs(), cache->reciprocal_cache());
        jit_->enableExecution();
    }
#endif
}

bool VirtualMachine::set_dataset(std::span<const std::byte> dataset) {
    if ((flags_ & kRandOMXFlagFullMem) && dataset.size() != kRandomXDatasetBytes) {
        return false;
    }
    dataset_ = dataset;
    return true;
}

void VirtualMachine::allocate() {
    // Scratchpad is already allocated via std::vector resizing in constructor
}

void VirtualMachine::init_scratchpad(void* seed) {
    auto* seed_bytes = reinterpret_cast<AesState*>(seed);
    fill_aes_1r_x4(*seed_bytes, std::span<std::byte>(scratchpad_data_, scratchpad_size_));
}

void VirtualMachine::reset_rounding_mode() {
    std::fesetround(FE_TONEAREST);
}

void VirtualMachine::initialize_vm_state() {
    std::memset(reg_.r, 0, sizeof(reg_.r));
    std::uint64_t a0_lo = getSmallPositiveFloatBits(entropy_[0]);
    std::uint64_t a0_hi = getSmallPositiveFloatBits(entropy_[1]);
    std::uint64_t a1_lo = getSmallPositiveFloatBits(entropy_[2]);
    std::uint64_t a1_hi = getSmallPositiveFloatBits(entropy_[3]);
    std::uint64_t a2_lo = getSmallPositiveFloatBits(entropy_[4]);
    std::uint64_t a2_hi = getSmallPositiveFloatBits(entropy_[5]);
    std::uint64_t a3_lo = getSmallPositiveFloatBits(entropy_[6]);
    std::uint64_t a3_hi = getSmallPositiveFloatBits(entropy_[7]);

    std::memcpy(&reg_.a[0].lo, &a0_lo, 8);
    std::memcpy(&reg_.a[0].hi, &a0_hi, 8);
    std::memcpy(&reg_.a[1].lo, &a1_lo, 8);
    std::memcpy(&reg_.a[1].hi, &a1_hi, 8);
    std::memcpy(&reg_.a[2].lo, &a2_lo, 8);
    std::memcpy(&reg_.a[2].hi, &a2_hi, 8);
    std::memcpy(&reg_.a[3].lo, &a3_lo, 8);
    std::memcpy(&reg_.a[3].hi, &a3_hi, 8);

    ma_ = static_cast<std::uint32_t>(entropy_[8] & 0x7fffffc0ULL);
    mx_ = static_cast<std::uint32_t>(entropy_[10]);

    auto addressRegisters = entropy_[12];
    read_reg0_ = 0 + (addressRegisters & 1);
    addressRegisters >>= 1;
    read_reg1_ = 2 + (addressRegisters & 1);
    addressRegisters >>= 1;
    read_reg2_ = 4 + (addressRegisters & 1);
    addressRegisters >>= 1;
    read_reg3_ = 6 + (addressRegisters & 1);

    dataset_offset_ = (entropy_[13] % 524288ULL) * 64;
    e_mask_[0] = getFloatMask(entropy_[14]);
    e_mask_[1] = getFloatMask(entropy_[15]);
}

void VirtualMachine::compile_instruction(const Instruction& instr, int i, InstructionByteCode& ibc) {
    int opcode = instr.opcode;

    if (opcode < ceil_IADD_RS) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::IADD_RS;
        ibc.idst = &reg_.r[dst];
        ibc.isrc = &reg_.r[src];
        ibc.shift = instr.getModShift();
        if (dst != 5) {
            ibc.imm = 0;
        } else {
            ibc.imm = signExtend2sCompl(instr.getImm32());
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_IADD_M) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::IADD_M;
        ibc.idst = &reg_.r[dst];
        ibc.imm = signExtend2sCompl(instr.getImm32());
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
            ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
        } else {
            static const std::uint64_t zero_val = 0;
            ibc.isrc = &zero_val;
            ibc.memMask = kScratchpadL3Mask;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_ISUB_R) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::ISUB_R;
        ibc.idst = &reg_.r[dst];
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
        } else {
            ibc.imm = signExtend2sCompl(instr.getImm32());
            ibc.isrc = &ibc.imm;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_ISUB_M) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::ISUB_M;
        ibc.idst = &reg_.r[dst];
        ibc.imm = signExtend2sCompl(instr.getImm32());
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
            ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
        } else {
            static const std::uint64_t zero_val = 0;
            ibc.isrc = &zero_val;
            ibc.memMask = kScratchpadL3Mask;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_IMUL_R) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::IMUL_R;
        ibc.idst = &reg_.r[dst];
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
        } else {
            ibc.imm = signExtend2sCompl(instr.getImm32());
            ibc.isrc = &ibc.imm;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_IMUL_M) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::IMUL_M;
        ibc.idst = &reg_.r[dst];
        ibc.imm = signExtend2sCompl(instr.getImm32());
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
            ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
        } else {
            static const std::uint64_t zero_val = 0;
            ibc.isrc = &zero_val;
            ibc.memMask = kScratchpadL3Mask;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_IMULH_R) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::IMULH_R;
        ibc.idst = &reg_.r[dst];
        ibc.isrc = &reg_.r[src];
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_IMULH_M) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::IMULH_M;
        ibc.idst = &reg_.r[dst];
        ibc.imm = signExtend2sCompl(instr.getImm32());
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
            ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
        } else {
            static const std::uint64_t zero_val = 0;
            ibc.isrc = &zero_val;
            ibc.memMask = kScratchpadL3Mask;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_ISMULH_R) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::ISMULH_R;
        ibc.idst = &reg_.r[dst];
        ibc.isrc = &reg_.r[src];
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_ISMULH_M) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::ISMULH_M;
        ibc.idst = &reg_.r[dst];
        ibc.imm = signExtend2sCompl(instr.getImm32());
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
            ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
        } else {
            static const std::uint64_t zero_val = 0;
            ibc.isrc = &zero_val;
            ibc.memMask = kScratchpadL3Mask;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_IMUL_RCP) {
        const std::uint32_t divisor = instr.getImm32();
        if (!isZeroOrPowerOf2(divisor)) {
            auto dst = instr.dst % 8;
            ibc.type = InstructionType::IMUL_R;
            ibc.idst = &reg_.r[dst];
            ibc.imm = randomx_reciprocal(divisor);
            ibc.isrc = &ibc.imm;
            register_usage_[dst] = i;
        } else {
            ibc.type = InstructionType::NOP;
        }
        return;
    }

    if (opcode < ceil_INEG_R) {
        auto dst = instr.dst % 8;
        ibc.type = InstructionType::INEG_R;
        ibc.idst = &reg_.r[dst];
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_IXOR_R) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::IXOR_R;
        ibc.idst = &reg_.r[dst];
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
        } else {
            ibc.imm = signExtend2sCompl(instr.getImm32());
            ibc.isrc = &ibc.imm;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_IXOR_M) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::IXOR_M;
        ibc.idst = &reg_.r[dst];
        ibc.imm = signExtend2sCompl(instr.getImm32());
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
            ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
        } else {
            static const std::uint64_t zero_val = 0;
            ibc.isrc = &zero_val;
            ibc.memMask = kScratchpadL3Mask;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_IROR_R) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::IROR_R;
        ibc.idst = &reg_.r[dst];
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
        } else {
            ibc.imm = instr.getImm32();
            ibc.isrc = &ibc.imm;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_IROL_R) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::IROL_R;
        ibc.idst = &reg_.r[dst];
        if (src != dst) {
            ibc.isrc = &reg_.r[src];
        } else {
            ibc.imm = instr.getImm32();
            ibc.isrc = &ibc.imm;
        }
        register_usage_[dst] = i;
        return;
    }

    if (opcode < ceil_ISWAP_R) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        if (src != dst) {
            ibc.idst = &reg_.r[dst];
            ibc.isrc = &reg_.r[src];
            ibc.type = InstructionType::ISWAP_R;
            register_usage_[dst] = i;
            register_usage_[src] = i;
        } else {
            ibc.type = InstructionType::NOP;
        }
        return;
    }

    if (opcode < ceil_FSWAP_R) {
        auto dst = instr.dst % 8;
        ibc.type = InstructionType::FSWAP_R;
        if (dst < 4) {
            ibc.fdst = &reg_.f[dst];
        } else {
            ibc.fdst = &reg_.e[dst - 4];
        }
        return;
    }

    if (opcode < ceil_FADD_R) {
        auto dst = instr.dst % 4;
        auto src = instr.src % 4;
        ibc.type = InstructionType::FADD_R;
        ibc.fdst = &reg_.f[dst];
        ibc.fsrc = &reg_.a[src];
        return;
    }

    if (opcode < ceil_FADD_M) {
        auto dst = instr.dst % 4;
        auto src = instr.src % 8;
        ibc.type = InstructionType::FADD_M;
        ibc.fdst = &reg_.f[dst];
        ibc.isrc = &reg_.r[src];
        ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
        ibc.imm = signExtend2sCompl(instr.getImm32());
        return;
    }

    if (opcode < ceil_FSUB_R) {
        auto dst = instr.dst % 4;
        auto src = instr.src % 4;
        ibc.type = InstructionType::FSUB_R;
        ibc.fdst = &reg_.f[dst];
        ibc.fsrc = &reg_.a[src];
        return;
    }

    if (opcode < ceil_FSUB_M) {
        auto dst = instr.dst % 4;
        auto src = instr.src % 8;
        ibc.type = InstructionType::FSUB_M;
        ibc.fdst = &reg_.f[dst];
        ibc.isrc = &reg_.r[src];
        ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
        ibc.imm = signExtend2sCompl(instr.getImm32());
        return;
    }

    if (opcode < ceil_FSCAL_R) {
        auto dst = instr.dst % 4;
        ibc.fdst = &reg_.f[dst];
        ibc.type = InstructionType::FSCAL_R;
        return;
    }

    if (opcode < ceil_FMUL_R) {
        auto dst = instr.dst % 4;
        auto src = instr.src % 4;
        ibc.type = InstructionType::FMUL_R;
        ibc.fdst = &reg_.e[dst];
        ibc.fsrc = &reg_.a[src];
        return;
    }

    if (opcode < ceil_FDIV_M) {
        auto dst = instr.dst % 4;
        auto src = instr.src % 8;
        ibc.type = InstructionType::FDIV_M;
        ibc.fdst = &reg_.e[dst];
        ibc.isrc = &reg_.r[src];
        ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
        ibc.imm = signExtend2sCompl(instr.getImm32());
        return;
    }

    if (opcode < ceil_FSQRT_R) {
        auto dst = instr.dst % 4;
        ibc.type = InstructionType::FSQRT_R;
        ibc.fdst = &reg_.e[dst];
        return;
    }

    if (opcode < ceil_CBRANCH) {
        ibc.type = InstructionType::CBRANCH;
        int creg = instr.dst % 8;
        ibc.idst = &reg_.r[creg];
        ibc.target = register_usage_[creg];
        int shift = instr.getModCond() + 8;
        ibc.imm = signExtend2sCompl(instr.getImm32()) | (1ULL << shift);
        if (shift > 0) {
            ibc.imm &= ~(1ULL << (shift - 1));
        }
        ibc.memMask = 255U << shift;
        for (unsigned j = 0; j < 8; ++j) {
            register_usage_[j] = i;
        }
        return;
    }

    if (opcode < ceil_CFROUND) {
        auto src = instr.src % 8;
        ibc.isrc = &reg_.r[src];
        ibc.type = InstructionType::CFROUND;
        ibc.imm = instr.getImm32() & 63;
        return;
    }

    if (opcode < ceil_ISTORE) {
        auto dst = instr.dst % 8;
        auto src = instr.src % 8;
        ibc.type = InstructionType::ISTORE;
        ibc.idst = &reg_.r[dst];
        ibc.isrc = &reg_.r[src];
        ibc.imm = signExtend2sCompl(instr.getImm32());
        if (instr.getModCond() < 14) {
            ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
        } else {
            ibc.memMask = kScratchpadL3Mask;
        }
        return;
    }

    if (opcode < ceil_NOP) {
        ibc.type = InstructionType::NOP;
        return;
    }

    throw std::runtime_error("unreachable opcode");
}

void VirtualMachine::compile_program() {
    std::fill(std::begin(register_usage_), std::end(register_usage_), -1);
    for (std::size_t i = 0; i < 256; ++i) {
        compile_instruction(program_(i), static_cast<int>(i), bytecode_[i]);
    }
}

void VirtualMachine::execute_bytecode() {
    std::byte* scratchpad = scratchpad_data_;
    for (int pc = 0; pc < 256; ++pc) {
        auto& ibc = bytecode_[pc];
        switch (ibc.type) {
            case InstructionType::IADD_RS:
                *ibc.idst += (*ibc.isrc << ibc.shift) + ibc.imm;
                break;
            case InstructionType::IADD_M:
                *ibc.idst += load64(getScratchpadAddress(ibc, scratchpad));
                break;
            case InstructionType::ISUB_R:
                *ibc.idst -= *ibc.isrc;
                break;
            case InstructionType::ISUB_M:
                *ibc.idst -= load64(getScratchpadAddress(ibc, scratchpad));
                break;
            case InstructionType::IMUL_R:
                *ibc.idst *= *ibc.isrc;
                break;
            case InstructionType::IMUL_M:
                *ibc.idst *= load64(getScratchpadAddress(ibc, scratchpad));
                break;
            case InstructionType::IMULH_R:
                *ibc.idst = mulh(*ibc.idst, *ibc.isrc);
                break;
            case InstructionType::IMULH_M:
                *ibc.idst = mulh(*ibc.idst, load64(getScratchpadAddress(ibc, scratchpad)));
                break;
            case InstructionType::ISMULH_R:
                *ibc.idst = static_cast<std::uint64_t>(smulh(static_cast<std::int64_t>(*ibc.idst), static_cast<std::int64_t>(*ibc.isrc)));
                break;
            case InstructionType::ISMULH_M:
                *ibc.idst = static_cast<std::uint64_t>(smulh(static_cast<std::int64_t>(*ibc.idst), static_cast<std::int64_t>(load64(getScratchpadAddress(ibc, scratchpad)))));
                break;
            case InstructionType::IMUL_RCP:
                break;
            case InstructionType::INEG_R:
                *ibc.idst = ~(*ibc.idst) + 1;
                break;
            case InstructionType::IXOR_R:
                *ibc.idst ^= *ibc.isrc;
                break;
            case InstructionType::IXOR_M:
                *ibc.idst ^= load64(getScratchpadAddress(ibc, scratchpad));
                break;
            case InstructionType::IROR_R:
                *ibc.idst = rotr(*ibc.idst, *ibc.isrc & 63);
                break;
            case InstructionType::IROL_R:
                *ibc.idst = std::rotl(*ibc.idst, *ibc.isrc & 63);
                break;
            case InstructionType::ISWAP_R: {
                std::uint64_t temp = *ibc.isrc;
                *const_cast<std::uint64_t*>(ibc.isrc) = *ibc.idst;
                *ibc.idst = temp;
                break;
            }
            case InstructionType::FSWAP_R:
                std::swap(ibc.fdst->lo, ibc.fdst->hi);
                break;
            case InstructionType::FADD_R:
                ibc.fdst->lo += ibc.fsrc->lo;
                ibc.fdst->hi += ibc.fsrc->hi;
                break;
            case InstructionType::FADD_M: {
                auto* addr = getScratchpadAddress(ibc, scratchpad);
                double lo = unsigned32ToSigned2sCompl(load32(addr));
                double hi = unsigned32ToSigned2sCompl(load32(addr + 4));
                ibc.fdst->lo += lo;
                ibc.fdst->hi += hi;
                break;
            }
            case InstructionType::FSUB_R:
                ibc.fdst->lo -= ibc.fsrc->lo;
                ibc.fdst->hi -= ibc.fsrc->hi;
                break;
            case InstructionType::FSUB_M: {
                auto* addr = getScratchpadAddress(ibc, scratchpad);
                double lo = unsigned32ToSigned2sCompl(load32(addr));
                double hi = unsigned32ToSigned2sCompl(load32(addr + 4));
                ibc.fdst->lo -= lo;
                ibc.fdst->hi -= hi;
                break;
            }
            case InstructionType::FSCAL_R: {
                std::uint64_t lo_bits, hi_bits;
                std::memcpy(&lo_bits, &ibc.fdst->lo, 8);
                std::memcpy(&hi_bits, &ibc.fdst->hi, 8);
                lo_bits ^= 0x80F0000000000000ULL;
                hi_bits ^= 0x80F0000000000000ULL;
                std::memcpy(&ibc.fdst->lo, &lo_bits, 8);
                std::memcpy(&ibc.fdst->hi, &hi_bits, 8);
                break;
            }
            case InstructionType::FMUL_R:
                ibc.fdst->lo *= ibc.fsrc->lo;
                ibc.fdst->hi *= ibc.fsrc->hi;
                break;
            case InstructionType::FDIV_M: {
                auto* addr = getScratchpadAddress(ibc, scratchpad);
                double lo = unsigned32ToSigned2sCompl(load32(addr));
                double hi = unsigned32ToSigned2sCompl(load32(addr + 4));
                std::uint64_t lo_bits, hi_bits;
                std::memcpy(&lo_bits, &lo, 8);
                std::memcpy(&hi_bits, &hi, 8);
                lo_bits = (lo_bits & dynamicMantissaMask) | e_mask_[0];
                hi_bits = (hi_bits & dynamicMantissaMask) | e_mask_[1];
                std::memcpy(&lo, &lo_bits, 8);
                std::memcpy(&hi, &hi_bits, 8);

                ibc.fdst->lo /= lo;
                ibc.fdst->hi /= hi;
                break;
            }
            case InstructionType::FSQRT_R:
                ibc.fdst->lo = std::sqrt(ibc.fdst->lo);
                ibc.fdst->hi = std::sqrt(ibc.fdst->hi);
                break;
            case InstructionType::CBRANCH:
                *ibc.idst += ibc.imm;
                if ((*ibc.idst & ibc.memMask) == 0) {
                    pc = ibc.target;
                }
                break;
            case InstructionType::CFROUND: {
                std::uint64_t isrc = rotr(*ibc.isrc, ibc.imm);
                rx_set_rounding_mode(isrc % 4);
                break;
            }
            case InstructionType::ISTORE:
                store64(scratchpad + ((*ibc.idst + ibc.imm) & ibc.memMask), *ibc.isrc);
                break;
            case InstructionType::NOP:
                break;
        }
    }
}

void VirtualMachine::dataset_read(std::uint64_t address, std::uint64_t (&r)[8]) {
    if (flags_ & kRandOMXFlagFullMem) {
        ARMRX_ASSERT(address + 64 <= dataset_.size(), "dataset_read OOB");
        const std::byte* datasetLine = dataset_.data() + address;
        for (int i = 0; i < 8; ++i) {
            r[i] ^= load64(datasetLine + 8 * i);
        }
    } else {
        std::uint32_t itemNumber = static_cast<std::uint32_t>(address / 64);
        const auto item = generate_dataset_item(*cache_, itemNumber);
        for (int i = 0; i < 8; ++i) {
            std::uint64_t val;
            std::memcpy(&val, item.data() + 8 * i, 8);
            r[i] ^= val;
        }
    }
}

void VirtualMachine::run(const void* seed) {
    AesState program_seed;
    std::memcpy(program_seed.data(), seed, 64);
    AesGenerator4R gen{program_seed};

    std::array<std::byte, sizeof(entropy_) + sizeof(program_.program_buffer_)> prog_bytes{};
    gen.fill(prog_bytes);

    std::memcpy(entropy_.data(), prog_bytes.data(), sizeof(entropy_));
    std::memcpy(program_.program_buffer_.data(), prog_bytes.data() + sizeof(entropy_), sizeof(program_.program_buffer_));

    initialize_vm_state();

#ifdef ARMRX_HAVE_JIT
    if (jit_) {
        // Build ProgramConfiguration from the initialized VM state
        ProgramConfiguration config{};
        config.eMask[0] = e_mask_[0];
        config.eMask[1] = e_mask_[1];
        config.readReg0 = read_reg0_;
        config.readReg1 = read_reg1_;
        config.readReg2 = read_reg2_;
        config.readReg3 = read_reg3_;

#ifdef ARMRX_JIT_PROFILE
        auto t0 = std::chrono::high_resolution_clock::now();
#endif
        jit_->enableWriting();
        if (dataset_.empty()) {
            // Light mode: JIT compiler generates inline dataset item derivation
            jit_->generateProgramLight(program_, config, dataset_offset_);
        } else {
            // Fast mode: JIT compiler reads directly from pre-computed dataset
            jit_->generateProgram(program_, config);
        }
        jit_->enableExecution();
#ifdef ARMRX_JIT_PROFILE
        auto t1 = std::chrono::high_resolution_clock::now();
#endif

        MemoryRegisters mem_regs{};
        mem_regs.mx = mx_;
        mem_regs.ma = ma_;
        if (dataset_.empty()) {
            // Light mode: JIT needs cache pointer to derive dataset items on the fly
            mem_regs.memory = cache_ ? reinterpret_cast<const uint8_t*>(cache_->blocks().data()) : nullptr;
        } else {
            // Fast mode: JIT reads from pre-computed dataset
            mem_regs.memory = reinterpret_cast<const uint8_t*>(dataset_.data()) + dataset_offset_;
        }

        // Copy eMask into the top of reg_.a as the native ABI expects
        std::memcpy(&reg_.a[0], config.eMask, sizeof(config.eMask));

        jit_->getProgramFunc()(
            &reg_, &mem_regs,
            reinterpret_cast<void*>(scratchpad_data_),
            2048ULL);
#ifdef ARMRX_JIT_PROFILE
        auto t2 = std::chrono::high_resolution_clock::now();

        jit_compile_time_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        jit_execute_time_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        jit_total_runs_++;
#endif

        // Extract updated mx/ma back from mem_regs after JIT execution
        mx_ = mem_regs.mx;
        ma_ = mem_regs.ma;
        return;
    }
#endif

    compile_program();

    std::uint32_t spAddr0 = mx_;
    std::uint32_t spAddr1 = ma_;

    for (unsigned int ic = 0; ic < 2048; ++ic) {
        std::uint64_t spMix = reg_.r[read_reg0_] ^ reg_.r[read_reg1_];
        spAddr0 ^= spMix;
        spAddr0 &= kScratchpadL3Mask64;
        spAddr1 ^= spMix >> 32;
        spAddr1 &= kScratchpadL3Mask64;

        for (unsigned int i = 0; i < 8; ++i) {
            reg_.r[i] ^= load64(scratchpad_data_ + spAddr0 + 8 * i);
        }

        for (unsigned int i = 0; i < 4; ++i) {
            auto* addr = scratchpad_data_ + spAddr1 + 8 * i;
            reg_.f[i].lo = unsigned32ToSigned2sCompl(load32(addr + 0));
            reg_.f[i].hi = unsigned32ToSigned2sCompl(load32(addr + 4));
        }

        for (unsigned int i = 0; i < 4; ++i) {
            auto* addr = scratchpad_data_ + spAddr1 + 8 * (4 + i);
            double lo = unsigned32ToSigned2sCompl(load32(addr + 0));
            double hi = unsigned32ToSigned2sCompl(load32(addr + 4));

            std::uint64_t lo_bits, hi_bits;
            std::memcpy(&lo_bits, &lo, 8);
            std::memcpy(&hi_bits, &hi, 8);
            lo_bits = (lo_bits & dynamicMantissaMask) | e_mask_[0];
            hi_bits = (hi_bits & dynamicMantissaMask) | e_mask_[1];
            std::memcpy(&lo, &lo_bits, 8);
            std::memcpy(&hi, &hi_bits, 8);

            reg_.e[i].lo = lo;
            reg_.e[i].hi = hi;
        }

        execute_bytecode();

                const std::uint64_t readPtr = dataset_offset_ + (ma_ & 0x7fffffc0ULL);
        mx_ ^= reg_.r[read_reg2_] ^ reg_.r[read_reg3_];

        dataset_read(readPtr, reg_.r);

        std::swap(mx_, ma_);

        for (unsigned int i = 0; i < 8; ++i) {
            store64(scratchpad_data_ + spAddr1 + 8 * i, reg_.r[i]);
        }

        for (unsigned int i = 0; i < 4; ++i) {
            std::uint64_t f_lo, f_hi, e_lo, e_hi;
            std::memcpy(&f_lo, &reg_.f[i].lo, 8);
            std::memcpy(&f_hi, &reg_.f[i].hi, 8);
            std::memcpy(&e_lo, &reg_.e[i].lo, 8);
            std::memcpy(&e_hi, &reg_.e[i].hi, 8);

            f_lo ^= e_lo;
            f_hi ^= e_hi;

            std::memcpy(&reg_.f[i].lo, &f_lo, 8);
            std::memcpy(&reg_.f[i].hi, &f_hi, 8);
        }

        for (unsigned int i = 0; i < 4; ++i) {
            std::memcpy(scratchpad_data_ + spAddr0 + 16 * i, &reg_.f[i], 16);
        }

        spAddr0 = 0;
        spAddr1 = 0;
    }
}

void VirtualMachine::hash_and_fill(void* out, void* fill_state) {
    AesState new_fill_state;
    std::memcpy(new_fill_state.data(), fill_state, 64);
    hash_and_fill_aes_1r_x4(std::span<std::byte>(scratchpad_data_, scratchpad_size_), reinterpret_cast<AesState&>(reg_.a), new_fill_state);
    std::memcpy(fill_state, new_fill_state.data(), 64);

    alignas(16) std::array<std::byte, sizeof(RegisterFile)> input_bytes{};
    std::memcpy(input_bytes.data(), &reg_, sizeof(RegisterFile));
    blake2b(std::span<const std::byte>(input_bytes), static_cast<std::byte*>(out), 32);
}

void VirtualMachine::get_final_result(void* out) {
    hash_aes_1r_x4(std::span<const std::byte>(scratchpad_data_, scratchpad_size_), reinterpret_cast<AesState&>(reg_.a));

    alignas(16) std::array<std::byte, sizeof(RegisterFile)> input_bytes{};
    std::memcpy(input_bytes.data(), &reg_, sizeof(RegisterFile));
    blake2b(std::span<const std::byte>(input_bytes), static_cast<std::byte*>(out), 32);
}

void randomx_calculate_hash(VirtualMachine* machine, const void* input, std::size_t input_size, void* output) {
    fenv_t fpstate;
    std::fegetenv(&fpstate);

    alignas(16) std::array<std::byte, 64> tempHash{};
    std::span<const std::byte> input_span(reinterpret_cast<const std::byte*>(input), input_size);
    blake2b(input_span, tempHash.data(), 64);

    machine->init_scratchpad(tempHash.data());
    machine->reset_rounding_mode();

    alignas(16) std::array<std::byte, sizeof(RegisterFile)> reg_bytes{};
    for (int chain = 0; chain < 7; ++chain) {
        machine->run(tempHash.data());

        const auto& reg = machine->get_register_file();
        std::memcpy(reg_bytes.data(), &reg, sizeof(reg));
        blake2b(std::span<const std::byte>(reg_bytes), tempHash.data(), 64);
    }

    machine->run(tempHash.data());
    machine->get_final_result(output);

    std::fesetenv(&fpstate);
}

} // namespace armrx
