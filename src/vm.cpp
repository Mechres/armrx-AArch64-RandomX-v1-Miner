#include "armrx/vm.hpp"
#include "armrx/superscalar.hpp"
#include "armrx/blake2b.hpp"
#include "armrx/dataset.hpp"
#include "armrx/aes_generator.hpp"
#include "armrx/assert.hpp"
#include "armrx/randomx_config.hpp"
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
#include "armrx/virtual_memory.h"

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

static inline void rx_set_rounding_mode(std::uint32_t mode, std::uint32_t& last_mode) {
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

static inline std::byte* getScratchpadAddress(const InstructionByteCode& ibc, std::byte* scratchpad) {
    std::uint32_t addr = (static_cast<std::uint32_t>(*ibc.isrc) + static_cast<std::uint32_t>(ibc.imm)) & ibc.memMask;
    return scratchpad + addr;
}

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
    // Try MAP_HUGETLB first (2 MiB is exactly one huge page)
    scratchpad_data_ = static_cast<std::byte*>(allocLargePagesMemory(sp_size));
    if (!scratchpad_data_) {
        // Fallback: plain anonymous + MADV_HUGEPAGE
        scratchpad_data_ = static_cast<std::byte*>(
            ::mmap(nullptr, sp_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (scratchpad_data_ == MAP_FAILED) {
            throw std::bad_alloc();
        }
        ::madvise(scratchpad_data_, sp_size, MADV_HUGEPAGE);
    }
    scratchpad_size_ = sp_size;
    // Warm up the scratchpad to avoid cold-start page faults on the first hash.
    // MADV_POPULATE_WRITE (Linux 5.14+) prefaults writable pages without memset.
#if defined(MADV_POPULATE_WRITE)
    ::madvise(scratchpad_data_, sp_size, MADV_POPULATE_WRITE);
#else
    // Fallback: touch every page to fault them in
    std::memset(scratchpad_data_, 0, sp_size);
#endif
#ifdef ARMRX_HAVE_JIT
    if (flags_ & kRandOMXFlagJit) {
        jit_ = std::make_unique<JitCompilerA64>();
        jit_->setFlags(map_to_randomx_flags(flags_));
    }
#endif
}

VirtualMachine::~VirtualMachine() {
    if (scratchpad_data_ && scratchpad_owned_) {
        ::munmap(scratchpad_data_, scratchpad_size_);
        scratchpad_data_ = nullptr;
        scratchpad_size_ = 0;
    }
}

void VirtualMachine::set_scratchpad(std::byte* ptr, std::size_t size) {
    if (scratchpad_data_ && scratchpad_owned_) {
        ::munmap(scratchpad_data_, scratchpad_size_);
    }
    scratchpad_data_ = ptr;
    scratchpad_size_ = size;
    scratchpad_owned_ = false;
}

void VirtualMachine::set_cache(const Argon2dCache* cache) {
    cache_ = cache;
#ifdef ARMRX_HAVE_JIT
    if (jit_ && cache) {
        if (!jit_->enableWriting()) {
            throw std::runtime_error("JIT: enableWriting (set cache) failed");
        }
        jit_->generateSuperscalarHash(cache->programs(), cache->reciprocal_cache());
        if (!jit_->enableExecution()) {
            throw std::runtime_error("JIT: enableExecution (set cache) failed");
        }
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
    // Scratchpad is already allocated via mmap in the constructor (vm.cpp:173-176)
}

void VirtualMachine::init_scratchpad(void* seed) {
    auto* seed_bytes = reinterpret_cast<AesState*>(seed);
    fill_aes_1r_x4(*seed_bytes, std::span<std::byte>(scratchpad_data_, scratchpad_size_));
}

void VirtualMachine::reset_rounding_mode() {
    std::fesetround(FE_TONEAREST);
    last_rounding_mode_ = 0; // invalidate cache so CFROUND always calls fesetround
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

    // Note: In JIT mode, the group-A registers are loaded by the JIT prologue
    // so we must always initialize them.
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
    (this->*kCompileHandlers[instr.opcode])(instr, i, ibc);
}

// ── Instruction compiler handlers ────────────────────────────────────────

void VirtualMachine::h_IADD_RS(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    auto src = instr.src % 8;
    ibc.type = InstructionType::IADD_RS;
    ibc.idst = &reg_.r[dst];
    ibc.isrc = &reg_.r[src];
    ibc.shift = static_cast<std::uint16_t>(instr.getModShift());
    ibc.imm = (dst != 5) ? 0 : signExtend2sCompl(instr.getImm32());
    register_usage_[dst] = i;
}

static void compile_mem_op(InstructionByteCode& ibc, const Instruction& instr,
                           std::uint64_t* dst_ptr, std::uint64_t* src_ptr,
                           InstructionType type, std::uint64_t imm) {
    ibc.type = type;
    ibc.idst = dst_ptr;
    ibc.imm = imm;
    if (src_ptr != dst_ptr) {
        ibc.isrc = src_ptr;
        ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
    } else {
        static const std::uint64_t zero_val = 0;
        ibc.isrc = &zero_val;
        ibc.memMask = kScratchpadL3Mask;
    }
}

static void compile_alu_reg(InstructionByteCode& ibc, const Instruction& /*instr*/,
                            std::uint64_t* dst_ptr, std::uint64_t* src_ptr,
                            InstructionType type, std::uint64_t imm) {
    ibc.type = type;
    ibc.idst = dst_ptr;
    if (src_ptr != dst_ptr) {
        ibc.isrc = src_ptr;
    } else {
        ibc.imm = imm;
        ibc.isrc = &ibc.imm;
    }
}

void VirtualMachine::h_IADD_M(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_mem_op(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                   InstructionType::IADD_M, signExtend2sCompl(instr.getImm32()));
    register_usage_[dst] = i;
}

void VirtualMachine::h_ISUB_R(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_alu_reg(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                    InstructionType::ISUB_R, signExtend2sCompl(instr.getImm32()));
    register_usage_[dst] = i;
}

void VirtualMachine::h_ISUB_M(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_mem_op(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                   InstructionType::ISUB_M, signExtend2sCompl(instr.getImm32()));
    register_usage_[dst] = i;
}

void VirtualMachine::h_IMUL_R(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_alu_reg(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                    InstructionType::IMUL_R, signExtend2sCompl(instr.getImm32()));
    register_usage_[dst] = i;
}

void VirtualMachine::h_IMUL_M(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_mem_op(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                   InstructionType::IMUL_M, signExtend2sCompl(instr.getImm32()));
    register_usage_[dst] = i;
}

void VirtualMachine::h_IMULH_R(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    ibc.type = InstructionType::IMULH_R;
    ibc.idst = &reg_.r[dst];
    ibc.isrc = &reg_.r[instr.src % 8];
    register_usage_[dst] = i;
}

void VirtualMachine::h_IMULH_M(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_mem_op(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                   InstructionType::IMULH_M, signExtend2sCompl(instr.getImm32()));
    register_usage_[dst] = i;
}

void VirtualMachine::h_ISMULH_R(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    ibc.type = InstructionType::ISMULH_R;
    ibc.idst = &reg_.r[dst];
    ibc.isrc = &reg_.r[instr.src % 8];
    register_usage_[dst] = i;
}

void VirtualMachine::h_ISMULH_M(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_mem_op(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                   InstructionType::ISMULH_M, signExtend2sCompl(instr.getImm32()));
    register_usage_[dst] = i;
}

void VirtualMachine::h_IMUL_RCP(const Instruction& instr, int i, InstructionByteCode& ibc) {
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
}

void VirtualMachine::h_INEG_R(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    ibc.type = InstructionType::INEG_R;
    ibc.idst = &reg_.r[dst];
    register_usage_[dst] = i;
}

void VirtualMachine::h_IXOR_R(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_alu_reg(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                    InstructionType::IXOR_R, signExtend2sCompl(instr.getImm32()));
    register_usage_[dst] = i;
}

void VirtualMachine::h_IXOR_M(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_mem_op(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                   InstructionType::IXOR_M, signExtend2sCompl(instr.getImm32()));
    register_usage_[dst] = i;
}

void VirtualMachine::h_IROR_R(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_alu_reg(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                    InstructionType::IROR_R, instr.getImm32());
    register_usage_[dst] = i;
}

void VirtualMachine::h_IROL_R(const Instruction& instr, int i, InstructionByteCode& ibc) {
    auto dst = instr.dst % 8;
    compile_alu_reg(ibc, instr, &reg_.r[dst], &reg_.r[instr.src % 8],
                    InstructionType::IROL_R, instr.getImm32());
    register_usage_[dst] = i;
}

void VirtualMachine::h_ISWAP_R(const Instruction& instr, int i, InstructionByteCode& ibc) {
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
}

void VirtualMachine::h_FSWAP_R(const Instruction& instr, int i, InstructionByteCode& ibc) {
    (void)i;
    auto dst = instr.dst % 8;
    ibc.type = InstructionType::FSWAP_R;
    if (dst < 4) {
        ibc.fdst = &reg_.f[dst];
    } else {
        ibc.fdst = &reg_.e[dst - 4];
    }
}

void VirtualMachine::h_FADD_R(const Instruction& instr, int /*i*/, InstructionByteCode& ibc) {
    ibc.type = InstructionType::FADD_R;
    ibc.fdst = &reg_.f[instr.dst % 4];
    ibc.fsrc = &reg_.a[instr.src % 4];
}

void VirtualMachine::h_FADD_M(const Instruction& instr, int /*i*/, InstructionByteCode& ibc) {
    ibc.type = InstructionType::FADD_M;
    ibc.fdst = &reg_.f[instr.dst % 4];
    ibc.isrc = &reg_.r[instr.src % 8];
    ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
    ibc.imm = signExtend2sCompl(instr.getImm32());
}

void VirtualMachine::h_FSUB_R(const Instruction& instr, int /*i*/, InstructionByteCode& ibc) {
    ibc.type = InstructionType::FSUB_R;
    ibc.fdst = &reg_.f[instr.dst % 4];
    ibc.fsrc = &reg_.a[instr.src % 4];
}

void VirtualMachine::h_FSUB_M(const Instruction& instr, int /*i*/, InstructionByteCode& ibc) {
    ibc.type = InstructionType::FSUB_M;
    ibc.fdst = &reg_.f[instr.dst % 4];
    ibc.isrc = &reg_.r[instr.src % 8];
    ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
    ibc.imm = signExtend2sCompl(instr.getImm32());
}

void VirtualMachine::h_FSCAL_R(const Instruction& instr, int /*i*/, InstructionByteCode& ibc) {
    ibc.fdst = &reg_.f[instr.dst % 4];
    ibc.type = InstructionType::FSCAL_R;
}

void VirtualMachine::h_FMUL_R(const Instruction& instr, int /*i*/, InstructionByteCode& ibc) {
    ibc.type = InstructionType::FMUL_R;
    ibc.fdst = &reg_.e[instr.dst % 4];
    ibc.fsrc = &reg_.a[instr.src % 4];
}

void VirtualMachine::h_FDIV_M(const Instruction& instr, int /*i*/, InstructionByteCode& ibc) {
    ibc.type = InstructionType::FDIV_M;
    ibc.fdst = &reg_.e[instr.dst % 4];
    ibc.isrc = &reg_.r[instr.src % 8];
    ibc.memMask = (instr.getModMem() ? kScratchpadL1Mask : kScratchpadL2Mask);
    ibc.imm = signExtend2sCompl(instr.getImm32());
}

void VirtualMachine::h_FSQRT_R(const Instruction& instr, int /*i*/, InstructionByteCode& ibc) {
    ibc.type = InstructionType::FSQRT_R;
    ibc.fdst = &reg_.e[instr.dst % 4];
}

void VirtualMachine::h_CBRANCH(const Instruction& instr, int i, InstructionByteCode& ibc) {
    ibc.type = InstructionType::CBRANCH;
    int creg = instr.dst % 8;
    ibc.idst = &reg_.r[creg];
    // register_usage_[creg] == -1 means creg was never written earlier in this
    // program -- a real, regularly-occurring case (an earlier ARMRX_ASSERT
    // here, added 2026-07-25 on an external audit's claim that this was
    // "theoretical only", fired repeatedly on a normal test run and was
    // removed the same day). -1 wraps `pc` to 0 via the execute loop's
    // `++pc`, i.e. "restart from VM instruction 0" -- correct, intentional
    // behavior, matching the JIT's own reg_changed_offset[] (reset to
    // PrologueSize, VM instruction 0's own code offset, before every
    // compile in emitPrologueMix). Not a bug.
    ibc.target = static_cast<std::int16_t>(register_usage_[creg]);
    int shift = instr.getModCond() + 8;
    ibc.imm = signExtend2sCompl(instr.getImm32()) | (1ULL << shift);
    if (shift > 0) {
        ibc.imm &= ~(1ULL << (shift - 1));
    }
    ibc.memMask = 255U << shift;
    for (unsigned j = 0; j < 8; ++j) {
        register_usage_[j] = i;
    }
}

void VirtualMachine::h_CFROUND(const Instruction& instr, int /*i*/, InstructionByteCode& ibc) {
    ibc.isrc = &reg_.r[instr.src % 8];
    ibc.type = InstructionType::CFROUND;
    ibc.imm = instr.getImm32() & 63;
}

void VirtualMachine::h_ISTORE(const Instruction& instr, int /*i*/, InstructionByteCode& ibc) {
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
}

void VirtualMachine::h_NOP(const Instruction& /*instr*/, int /*i*/, InstructionByteCode& ibc) {
    ibc.type = InstructionType::NOP;
}

// Dispatch table: maps each opcode (0-255) to its compile handler. Derived
// from instruction_weights.hpp's RANDOMX_FREQ_*/REPN/WT macros -- the same
// mechanism jit_compiler_a64.cpp uses to build its own 256-entry opcode
// table (see INST_HANDLE there) -- instead of an independently hand-written
// literal list of 256 handler pointers. The two tables encode the same
// opcode-to-instruction-type map (a correctness-critical invariant: the JIT
// and interpreter must dispatch every opcode identically), so building both
// from the one spec-derived frequency table means they can no longer
// silently drift apart from a typo in either hand-maintained copy.
#include "instruction_weights.hpp"
#define INST_HANDLE(x) REPN(&VirtualMachine::h_##x, WT(x))

const VirtualMachine::CompileHandler VirtualMachine::kCompileHandlers[256] = {
    INST_HANDLE(IADD_RS)
    INST_HANDLE(IADD_M)
    INST_HANDLE(ISUB_R)
    INST_HANDLE(ISUB_M)
    INST_HANDLE(IMUL_R)
    INST_HANDLE(IMUL_M)
    INST_HANDLE(IMULH_R)
    INST_HANDLE(IMULH_M)
    INST_HANDLE(ISMULH_R)
    INST_HANDLE(ISMULH_M)
    INST_HANDLE(IMUL_RCP)
    INST_HANDLE(INEG_R)
    INST_HANDLE(IXOR_R)
    INST_HANDLE(IXOR_M)
    INST_HANDLE(IROR_R)
    INST_HANDLE(IROL_R)
    INST_HANDLE(ISWAP_R)
    INST_HANDLE(FSWAP_R)
    INST_HANDLE(FADD_R)
    INST_HANDLE(FADD_M)
    INST_HANDLE(FSUB_R)
    INST_HANDLE(FSUB_M)
    INST_HANDLE(FSCAL_R)
    INST_HANDLE(FMUL_R)
    INST_HANDLE(FDIV_M)
    INST_HANDLE(FSQRT_R)
    INST_HANDLE(CBRANCH)
    INST_HANDLE(CFROUND)
    INST_HANDLE(ISTORE)
    INST_HANDLE(NOP)
};

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
            case InstructionType::IMUL_R: {
                const std::uint64_t src_v = *ibc.isrc;
                const std::uint64_t dst_v = *ibc.idst;
                *ibc.idst = dst_v * src_v;
                if (imul_sample_fn_ != nullptr)
                    imul_sample_fn_(src_v, dst_v, ibc.isrc == &ibc.imm);
                break;
            }
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
                std::uint64_t isrc = rotr(*ibc.isrc, static_cast<unsigned int>(ibc.imm));
                rx_set_rounding_mode(static_cast<std::uint32_t>(isrc % 4), last_rounding_mode_);
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
    if (is_fast_mode()) {
        ARMRX_ASSERT(address + 64 <= dataset_.size(), "dataset_read OOB");
        // ARMRX_ASSERT only logs in release builds (NDEBUG) and does not
        // stop execution -- an explicit check is needed here too, since
        // falling through would read past the mapped dataset. `address` is
        // spec-guaranteed in-bounds today by its exact mask/modulo
        // construction in run() (readPtr = dataset_offset_ + (ma_ &
        // 0x7fffffc0), both bounded so address+64 <= dataset_.size()
        // always holds), so this is defense-in-depth against a future
        // regression in that guarantee, not a currently-live bug
        // (audit finding, 2026-07-25).
        if (address + 64 > dataset_.size()) return;
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
        run_jit();
        return;
    }
#endif

    run_interpreted();
}

#ifdef ARMRX_HAVE_JIT
void VirtualMachine::run_jit() {
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
    if (!jit_->enableWriting()) {
        throw std::runtime_error("JIT: enableWriting (run) failed");
    }
    if (!is_fast_mode()) {
        // Light mode: JIT compiler generates inline dataset item derivation
        const bool useHybrid = (partial_dataset_data_ != nullptr);
        jit_->generateProgramLight(program_, config, static_cast<uint32_t>(dataset_offset_), useHybrid);
    } else {
        // Fast mode: JIT compiler reads directly from pre-computed dataset
        jit_->generateProgram(program_, config);
    }
    if (!jit_->enableExecution()) {
        throw std::runtime_error("JIT: enableExecution (run) failed");
    }
#ifdef ARMRX_JIT_PROFILE
    auto t1 = std::chrono::high_resolution_clock::now();
#endif

    MemoryRegisters mem_regs{};
    mem_regs.mx = mx_;
    mem_regs.ma = ma_;
    if (!is_fast_mode()) {
        // Light mode: JIT needs cache pointer to derive dataset items on the fly
        mem_regs.memory = cache_ ? reinterpret_cast<const uint8_t*>(cache_->blocks().data()) : nullptr;
        // Hybrid: pass partial dataset information for bound check.
        // Read item_count fresh from the atomic every hash — the fill
        // publishes chunks with release ordering, this load pairs with
        // acquire, guaranteeing written item bytes are visible.
        if (partial_dataset_data_ != nullptr && partial_dataset_item_count_ptr_) {
            mem_regs.partial_dataset_ = reinterpret_cast<const uint8_t*>(partial_dataset_data_);
            mem_regs.partial_dataset_items_ = partial_dataset_item_count_ptr_->load(std::memory_order_acquire);
        }
    } else {
        // Fast mode: JIT reads from pre-computed dataset
        mem_regs.memory = reinterpret_cast<const uint8_t*>(dataset_.data()) + dataset_offset_;
    }

    // Copy eMask into reg_.f[0] as the JIT assembly expects (offset 64 = [x0, 64])
    std::memcpy(&reg_.f[0], config.eMask, sizeof(config.eMask));

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
    // Bench-only: preserve state for a possible follow-up run_execute_only() call.
    // This copy happens once per hash, outside the JIT's own hot loop, so it
    // doesn't reintroduce the member-field access run_jit() itself now avoids.
    last_mem_regs_ = mem_regs;
}

void VirtualMachine::run_execute_only() {
    // Re-invoke the same compiled program (no enableWriting/generateProgramLight/
    // enableExecution) against whatever scratchpad_data_ currently points to.
    // reg_/last_mem_regs_ carry state forward from the previous invocation,
    // same as consecutive run_jit() calls would.
    jit_->getProgramFunc()(
        &reg_, &last_mem_regs_,
        reinterpret_cast<void*>(scratchpad_data_),
        2048ULL);
    mx_ = last_mem_regs_.mx;
    ma_ = last_mem_regs_.ma;
}
#endif

void VirtualMachine::run_interpreted() {
    compile_program();

    std::uint32_t spAddr0 = mx_;
    std::uint32_t spAddr1 = ma_;

    for (unsigned int ic = 0; ic < 2048; ++ic) {
        std::uint64_t spMix = reg_.r[read_reg0_] ^ reg_.r[read_reg1_];
        spAddr0 ^= static_cast<std::uint32_t>(spMix);
        spAddr0 &= kScratchpadL3Mask64;
        spAddr1 ^= static_cast<std::uint32_t>(spMix >> 32);
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
        mx_ ^= static_cast<std::uint32_t>(reg_.r[read_reg2_] ^ reg_.r[read_reg3_]);

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

void randomx_calculate_hash_pipelined(
    VirtualMachine* machine,
    const void* input, std::size_t input_size, void* output,
    const void* next_input, std::size_t next_input_size,
    std::byte* next_scratchpad, void* next_seed_out
) {
    fenv_t fpstate;
    std::fegetenv(&fpstate);

    constexpr std::size_t scratchpad_size = 2ULL * 1024 * 1024;

    // ── Part A: Compute current hash up to final VM run ──
    // (fill scratchpad + 8× VM programs)
    alignas(16) std::array<std::byte, 64> temp_hash{};
    std::span<const std::byte> input_span(
        reinterpret_cast<const std::byte*>(input), input_size);
    blake2b(input_span, temp_hash.data(), 64);

    machine->init_scratchpad(temp_hash.data());
    machine->reset_rounding_mode();

    alignas(16) std::array<std::byte, sizeof(RegisterFile)> reg_bytes{};
    for (int chain = 0; chain < 7; ++chain) {
        machine->run(temp_hash.data());
        const auto& reg = machine->get_register_file();
        std::memcpy(reg_bytes.data(), &reg, sizeof(reg));
        blake2b(std::span<const std::byte>(reg_bytes), temp_hash.data(), 64);
    }
    machine->run(temp_hash.data());
    // Now: scratchpad has VM execution results, reg_.a has initial AES hash state

    // ── Part B: Prepare next hash's fill seed ──
    alignas(16) std::array<std::byte, 64> next_seed{};
    std::span<const std::byte> next_input_span(
        reinterpret_cast<const std::byte*>(next_input), next_input_size);
    blake2b(next_input_span, next_seed.data(), 64);

    // Save seed for caller's run() sequence (fill modifies the AesState in place)
    std::memcpy(next_seed_out, next_seed.data(), 64);

    // ── Part C: Save hash state before interleave ──
    RegisterFile current_reg = machine->get_register_file();
    AesState hash_state;
    static_assert(sizeof(current_reg.a) == sizeof(AesState),
                  "RegisterFile.a must span exactly 64 bytes");
    std::memcpy(hash_state.data(), &current_reg.a, sizeof(AesState));

    // ── Part D: Interleaved AES hash + fill ──
    hash_and_fill_aes_interleaved_x4(
        machine->scratchpad_span(),
        std::span<std::byte>(next_scratchpad, scratchpad_size),
        hash_state, next_seed    // next_seed is consumed by fill
    );

    // ── Part E: Finalize current hash output ──
    std::memcpy(&current_reg.a, hash_state.data(), sizeof(AesState));
    alignas(16) std::array<std::byte, sizeof(RegisterFile)> final_reg_bytes{};
    std::memcpy(final_reg_bytes.data(), &current_reg, sizeof(RegisterFile));
    blake2b(std::span<const std::byte>(final_reg_bytes),
            static_cast<std::byte*>(output), 32);

    // ── Part F: Prepare VM for next hash's execution ──
    machine->set_scratchpad(next_scratchpad, scratchpad_size);

    std::fesetenv(&fpstate);
}

} // namespace armrx
