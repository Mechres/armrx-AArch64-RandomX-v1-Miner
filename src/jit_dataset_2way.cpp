/*
Track D1 (docs/plans/track-d1-superscalar-interleave-plan-20260728.md):
2-way interleaved superscalar dataset-item derivation -- implementation.

See jit_dataset_2way.hpp for the module's scope/isolation rationale and
the register-contract comment at randomx_calc_dataset_item_aarch64_2way
in jit_compiler_a64_static.S for the full physical-register assignment
this file emits against. This file deliberately duplicates (rather than
reuses) JitCompilerA64's emitMovImmediate/emitAddImmediate: those are
hardcoded to JitCompilerA64's own `code` member and its shared
num32bitLiterals/literalPos NEON-literal-pool state, neither of which
applies to this module's separate buffer -- see the per-function
comments below for why the simplified (always MOVZ/MOVK, no NEON pool)
behavior is not just simpler but already what production does in
practice for this exact code region (num32bitLiterals is pinned at 64
for the whole superscalar path in generateSuperscalarHash()).
*/

#include "armrx/jit_dataset_2way.hpp"

#include <cstring>
#include <stdexcept>

#include "armrx/instruction.hpp"
#include "armrx/jit_compiler_a64_static.hpp"
#include "armrx/virtual_memory.h"

namespace armrx {

namespace {

constexpr std::uint32_t B_INSTR      = 0x14000000;
constexpr std::uint32_t EOR          = 0xCA000000;
constexpr std::uint32_t ADD          = 0x8B000000;
constexpr std::uint32_t SUB          = 0xCB000000;
constexpr std::uint32_t MUL          = 0x9B007C00;
constexpr std::uint32_t UMULH        = 0x9BC07C00;
constexpr std::uint32_t SMULH        = 0x9B407C00;
constexpr std::uint32_t MOVZ         = 0xD2800000;
constexpr std::uint32_t MOVN         = 0x92800000;
constexpr std::uint32_t MOVK         = 0xF2800000;
constexpr std::uint32_t ADD_IMM_LO   = 0x91000000;
constexpr std::uint32_t ADD_IMM_HI   = 0x91400000;
constexpr std::uint32_t LDR_LITERAL  = 0x58000000;
constexpr std::uint32_t ROR_IMM      = 0x93C00000;
constexpr std::uint32_t MOV_REG      = 0xAA0003E0;
constexpr std::uint32_t AND_IMM_BASE = 0x92400000;

constexpr std::uint32_t CacheLineSize  = 64U;
constexpr std::uint32_t CacheSizeBytes = static_cast<std::uint32_t>(kRandomXCacheBytes);

// Physical registers rl_B[0..7] map to -- see the register-contract
// comment at randomx_calc_dataset_item_aarch64_2way in
// jit_compiler_a64_static.S. Stream A needs no such table: rl_A[k] is
// physical register k directly, identical to the existing single-stream
// path's convention.
constexpr std::uint32_t StreamBRegMap[8] = {19, 20, 21, 22, 23, 24, 25, 26};
constexpr std::uint32_t StreamARegisterValue = 10;
constexpr std::uint32_t StreamBRegisterValue = 28;
constexpr std::uint32_t StreamAMixBlock      = 11;
constexpr std::uint32_t StreamBMixBlock      = 17;
constexpr std::uint32_t StreamATmp           = 12;
constexpr std::uint32_t StreamBTmp           = 16;

template <typename T> constexpr std::size_t Log2(T value) { return (value > 1) ? (Log2(value / 2) + 1) : 0; }

void emit32(std::uint32_t val, std::uint8_t* code, std::uint32_t& pos) {
    std::memcpy(code + pos, &val, sizeof(val));
    pos += sizeof(val);
}

void emit64(std::uint64_t val, std::uint8_t* code, std::uint32_t& pos) {
    std::memcpy(code + pos, &val, sizeof(val));
    pos += sizeof(val);
}

// Simplified relative to JitCompilerA64::emitMovImmediate: always uses
// the MOVZ/MOVN+MOVK path (no NEON-literal-pool fast path). This is not
// a behavioral shortcut -- generateSuperscalarHash() pins
// num32bitLiterals at its 64-entry cap for this entire code region
// specifically so IXOR_C7..9/IADD_C7..9 large-immediate loads always
// take this exact path in production too (see the big comment above
// that pinning in jit_compiler_a64.cpp). Duplicating just this path is
// therefore a faithful match, not an approximation.
void emitMovImmediate2Way(std::uint32_t dst, std::uint32_t imm, std::uint8_t* code, std::uint32_t& pos) {
    if (imm < (1U << 16)) {
        emit32(MOVZ | dst | (imm << 5), code, pos);
        return;
    }

    if (static_cast<std::int32_t>(imm) < 0) {
        emit32(MOVN | dst | (1U << 21) | ((~imm >> 16) << 5), code, pos);
    } else {
        emit32(MOVZ | dst | (1U << 21) | ((imm >> 16) << 5), code, pos);
    }
    emit32(MOVK | dst | ((imm & 0xFFFFU) << 5), code, pos);
}

// Mirrors JitCompilerA64::emitAddImmediate exactly, except the
// large-immediate fallback's scratch register is the caller-supplied
// per-stream temp (tmpReg) instead of the original's hardcoded x20 --
// x20 is stream B's own rl_B[1] here and must not be clobbered.
void emitAddImmediate2Way(std::uint32_t dst, std::uint32_t src, std::uint32_t imm, std::uint32_t tmpReg,
                          std::uint8_t* code, std::uint32_t& pos) {
    if (imm < (1U << 24)) {
        const std::uint32_t imm_lo = imm & ((1U << 12) - 1);
        const std::uint32_t imm_hi = imm >> 12;
        if (imm_lo && imm_hi) {
            emit32(ADD_IMM_LO | dst | (src << 5) | (imm_lo << 10), code, pos);
            emit32(ADD_IMM_HI | dst | (dst << 5) | (imm_hi << 10), code, pos);
        } else if (imm_lo) {
            emit32(ADD_IMM_LO | dst | (src << 5) | (imm_lo << 10), code, pos);
        } else {
            emit32(ADD_IMM_HI | dst | (src << 5) | (imm_hi << 10), code, pos);
        }
    } else {
        emitMovImmediate2Way(tmpReg, imm, code, pos);
        emit32(ADD | dst | (src << 5) | (tmpReg << 16), code, pos);
    }
}

// Emits one superscalar instruction against an arbitrary (dst, src, tmp)
// physical-register triple -- called once per stream per scheduled
// instruction with each stream's own mapping. `literalPos` is that
// stream's own independent walk through the (shared, single-copy)
// IMUL_RCP literal pool for this round -- see the comment in generate()
// for why two independent trackers over one pool is correct here.
void emitSuperscalarInstr(const Instruction& instr, std::uint32_t dst, std::uint32_t src, std::uint32_t tmpReg,
                          std::uint32_t& literalPos, std::uint8_t* code, std::uint32_t& pos) {
    switch (static_cast<SuperscalarInstructionType>(instr.opcode)) {
    case SuperscalarInstructionType::ISUB_R:
        emit32(SUB | dst | (dst << 5) | (src << 16), code, pos);
        break;
    case SuperscalarInstructionType::IXOR_R:
        emit32(EOR | dst | (dst << 5) | (src << 16), code, pos);
        break;
    case SuperscalarInstructionType::IADD_RS:
        emit32(ADD | dst | (dst << 5) | (static_cast<std::uint32_t>(instr.getModShift()) << 10) | (src << 16), code,
               pos);
        break;
    case SuperscalarInstructionType::IMUL_R:
        emit32(MUL | dst | (dst << 5) | (src << 16), code, pos);
        break;
    case SuperscalarInstructionType::IROR_C:
        emit32(ROR_IMM | dst | (dst << 5) | ((instr.getImm32() & 63U) << 10) | (dst << 16), code, pos);
        break;
    case SuperscalarInstructionType::IADD_C7:
    case SuperscalarInstructionType::IADD_C8:
    case SuperscalarInstructionType::IADD_C9:
        emitAddImmediate2Way(dst, dst, instr.getImm32(), tmpReg, code, pos);
        break;
    case SuperscalarInstructionType::IXOR_C7:
    case SuperscalarInstructionType::IXOR_C8:
    case SuperscalarInstructionType::IXOR_C9:
        emitMovImmediate2Way(tmpReg, instr.getImm32(), code, pos);
        emit32(EOR | dst | (dst << 5) | (tmpReg << 16), code, pos);
        break;
    case SuperscalarInstructionType::IMULH_R:
        emit32(UMULH | dst | (dst << 5) | (src << 16), code, pos);
        break;
    case SuperscalarInstructionType::ISMULH_R:
        emit32(SMULH | dst | (dst << 5) | (src << 16), code, pos);
        break;
    case SuperscalarInstructionType::IMUL_RCP: {
        std::int32_t offset = (literalPos - pos) / 4;
        offset &= (1 << 19) - 1;
        literalPos += 8;
        emit32(LDR_LITERAL | tmpReg | (static_cast<std::uint32_t>(offset) << 5), code, pos);
        emit32(MUL | dst | (dst << 5) | (tmpReg << 16), code, pos);
        break;
    }
    default:
        break;
    }
}

// Upper-bound size estimate, mirroring JitCompilerA64's CalcDatasetItemSize
// formula, doubled where the 2-way path genuinely emits twice as much:
// two AND-mask instructions per round (was one) and two copies of every
// scheduled instruction (was one). Only used to size the allocation --
// generate() also defensively checks the actual emitted size against it.
std::size_t calcDataset2WaySize() {
    const auto prologue =
        static_cast<std::size_t>((std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_prefetch -
                                  (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way);
    const auto roundPrologue =
        static_cast<std::size_t>((std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_mix -
                                  (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_prefetch) +
        4 /* AND stream A */ + 4 /* AND stream B */;
    const auto roundEpilogue =
        static_cast<std::size_t>((std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_store_result -
                                  (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_mix) +
        4 /* registerValue update, stream A */ + 4 /* registerValue update, stream B */;
    // kSuperscalarMaxSize instructions/round worst case, 16 bytes/instr
    // worst case (matches the single-stream formula's own margin), x2
    // streams, plus room for the shared literal pool (up to
    // kSuperscalarMaxSize IMUL_RCP entries x 8 bytes, generous).
    const auto roundBody = kSuperscalarMaxSize * 16 * 2 + kSuperscalarMaxSize * 8;
    const auto epilogue =
        static_cast<std::size_t>((std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_end -
                                  (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_store_result);

    return prologue + kRandomXCacheAccesses * (roundPrologue + roundBody + roundEpilogue) + epilogue;
}

} // namespace

JitDataset2Way::JitDataset2Way() {
    bufferBytes_ = calcDataset2WaySize();
    code_ = static_cast<std::uint8_t*>(allocMemoryPages(bufferBytes_));
    if (code_ == nullptr) {
        throw std::runtime_error("JitDataset2Way: allocMemoryPages failed");
    }
    if (setPagesRWX(code_, bufferBytes_) != 0) {
        if (setPagesRW(code_, bufferBytes_) != 0) {
            freePagedMemory(code_, bufferBytes_);
            code_ = nullptr;
            throw std::runtime_error("JitDataset2Way: setPagesRW failed");
        }
    }
}

JitDataset2Way::~JitDataset2Way() {
    if (code_ != nullptr) {
        freePagedMemory(code_, bufferBytes_);
    }
}

void JitDataset2Way::generate(const SuperscalarProgramList2Way& programs,
                               const std::vector<std::uint64_t>& reciprocalCache,
                               const SuperscalarScheduler& scheduler) {
    if (setPagesRW(code_, bufferBytes_) != 0) {
        // Best-effort: some platforms return success on an already-writable
        // RWX mapping's redundant RW request; ignore failure here the same
        // way JitCompilerA64's rwx_ path treats it as already satisfied.
    }

    std::uint32_t codePos = 0;

    std::uint8_t* p1 = (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way;
    std::uint8_t* p2 = (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_prefetch;
    std::memcpy(code_ + codePos, p1, static_cast<std::size_t>(p2 - p1));
    codePos += static_cast<std::uint32_t>(p2 - p1);

    for (std::size_t i = 0; i < programs.size(); ++i) {
        const std::uint32_t mask = static_cast<std::uint32_t>(Log2(CacheSizeBytes / CacheLineSize) - 1);

        // and x11, x10, mask   (stream A registerValue -> mixBlock mask)
        emit32(AND_IMM_BASE | StreamAMixBlock | (StreamARegisterValue << 5) | (mask << 10), code_, codePos);
        // and x17, x28, mask   (stream B registerValue -> mixBlock mask)
        emit32(AND_IMM_BASE | StreamBMixBlock | (StreamBRegisterValue << 5) | (mask << 10), code_, codePos);

        p1 = (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_prefetch;
        p2 = (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_mix;
        std::memcpy(code_ + codePos, p1, static_cast<std::size_t>(p2 - p1));
        codePos += static_cast<std::uint32_t>(p2 - p1);

        const SuperscalarProgram& prog = programs[i];
        const std::size_t progSize = prog.size();

        const std::uint32_t jmp_pos = codePos;
        codePos += 4;

        // Fill in the (single, shared-by-both-streams) literal pool, in
        // program order -- identical to the single-stream path.
        for (std::size_t j = 0; j < progSize; ++j) {
            const Instruction& instr = prog(j);
            if (static_cast<SuperscalarInstructionType>(instr.opcode) == SuperscalarInstructionType::IMUL_RCP) {
                emit64(reciprocalCache[instr.getImm32()], code_, codePos);
            }
        }

        std::uint32_t branchWritePos = jmp_pos;
        emit32(B_INSTR | ((codePos - jmp_pos) / 4), code_, branchWritePos);
        // branchWritePos has now advanced by 4 (emit32's side effect),
        // landing exactly at the first literal pool byte -- both
        // streams' independent trackers start there.
        std::uint32_t literalPosA = branchWritePos;
        std::uint32_t literalPosB = branchWritePos;

        const std::vector<std::uint32_t> emit_order = scheduler(prog);
        for (std::uint32_t idx : emit_order) {
            const Instruction& instr = prog(idx);
            const std::uint32_t srcA = instr.src;
            const std::uint32_t dstA = instr.dst;
            const std::uint32_t srcB = StreamBRegMap[instr.src];
            const std::uint32_t dstB = StreamBRegMap[instr.dst];

            emitSuperscalarInstr(instr, dstA, srcA, StreamATmp, literalPosA, code_, codePos);
            emitSuperscalarInstr(instr, dstB, srcB, StreamBTmp, literalPosB, code_, codePos);
        }

        p1 = (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_mix;
        p2 = (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_store_result;
        std::memcpy(code_ + codePos, p1, static_cast<std::size_t>(p2 - p1));
        codePos += static_cast<std::uint32_t>(p2 - p1);

        // registerValue update, both streams.
        emit32(MOV_REG | StreamARegisterValue | (static_cast<std::uint32_t>(prog.address_register()) << 16), code_,
               codePos);
        emit32(MOV_REG | StreamBRegisterValue | (StreamBRegMap[prog.address_register()] << 16), code_, codePos);
    }

    p1 = (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_store_result;
    p2 = (std::uint8_t*)randomx_calc_dataset_item_aarch64_2way_end;
    std::memcpy(code_ + codePos, p1, static_cast<std::size_t>(p2 - p1));
    codePos += static_cast<std::uint32_t>(p2 - p1);

    if (codePos > bufferBytes_) {
        throw std::runtime_error("JitDataset2Way: emitted code exceeded allocated buffer");
    }
    codeSize_ = codePos;

    if (setPagesRX(code_, bufferBytes_) != 0) {
        // Same best-effort note as the RW call above (RWX mapping).
    }

#ifdef __GNUC__
    __builtin___clear_cache(reinterpret_cast<char*>(code_), reinterpret_cast<char*>(code_ + codeSize_));
#endif
}

Dataset2WayFunc JitDataset2Way::getFunc() const {
    return reinterpret_cast<Dataset2WayFunc>(reinterpret_cast<void*>(code_));
}

} // namespace armrx
