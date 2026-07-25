/*
Copyright (c) 2018-2019, tevador <tevador@gmail.com>
Copyright (c) 2019, SChernykh    <https://github.com/SChernykh>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the copyright holder nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "armrx/jit_compiler_a64.hpp"
#include "armrx/assert.hpp"
#include "armrx/log.hpp"
#include "configuration.h"
#include "instruction_weights.hpp"
#include <atomic>

// Verify the JIT code buffer layout: the .fill directive in static.S reserves
// RANDOMX_PROGRAM_MAX_SIZE * 32 * 4 bytes (12288 AArch64 instruction slots).
// Worst-case emission per RandomX instruction is ~20 AArch64 words
// (h_FDIV_M/h_FSQRT_R under ARMRX_JIT_FAST_DIV_SQRT); the 32-word budget
// leaves headroom. Do NOT shrink the .fill in static.S below the fast
// div/sqrt worst case.
// RANDOMX_PROGRAM_MAX_SIZE is fixed at 384 per the RandomX v1 spec.
static_assert(RANDOMX_PROGRAM_MAX_SIZE == 384, "Upstream RandomX v1 constant");
#include "armrx/superscalar.hpp"
#include "armrx/program.hpp"
#include "armrx/virtual_memory.h"
#include "armrx/randomx_config.hpp"

// Upstream RANDOMX_FLAG constants used inside the JIT compiler body.
// These mirror the values in upstream randomx.h and are distinct from
// the armrx flag constants (kRandOMXFlag*) in vm.hpp — see map_to_randomx_flags().
static constexpr uint32_t RANDOMX_FLAG_HARD_AES = 2;
static constexpr uint32_t RANDOMX_FLAG_FULL_MEM  = 4;
static constexpr uint32_t RANDOMX_FLAG_JIT       = 8;
static constexpr uint32_t RANDOMX_FLAG_V2        = 128;

// Upstream configuration constants used inside JIT body and static.S.
// Values that have an armrx equivalent are aliased to the project constant
// for a single source of truth.
static constexpr uint32_t RANDOMX_DATASET_BASE_SIZE  = 2147483648U; // 2 GiB base (kRandomXDatasetBytes adds 32 MiB extra)
static constexpr uint32_t RANDOMX_PROGRAM_ITERATIONS = 2048U;
static constexpr uint32_t RANDOMX_CACHE_ACCESSES     = static_cast<uint32_t>(armrx::kRandomXCacheAccesses);
static constexpr uint32_t RANDOMX_SUPERSCALAR_LATENCY = static_cast<uint32_t>(armrx::kSuperscalarLatency);
static constexpr uint32_t RANDOMX_SCRATCHPAD_L3      = static_cast<uint32_t>(armrx::kRandomXScratchpadBytes);
static constexpr uint32_t RANDOMX_SCRATCHPAD_L2      = 262144U;     // 256 KiB (no armrx equivalent)
static constexpr uint32_t RANDOMX_SCRATCHPAD_L1      = 16384U;      // 16 KiB (no armrx equivalent)
static constexpr uint32_t CacheLineSize              = 64U;
static constexpr uint32_t CacheSize                  = static_cast<uint32_t>(armrx::kRandomXCacheBytes);
static constexpr uint32_t ScratchpadL3Mask           = static_cast<uint32_t>(armrx::kScratchpadL3Mask);
static constexpr uint32_t RegisterNeedsDisplacement   = 5U;
static constexpr uint32_t ConditionMask               = 0xFFU;       // (1 << RANDOMX_JUMP_BITS) - 1
static constexpr int      ConditionOffset             = 8;           // RANDOMX_JUMP_OFFSET
static constexpr int      StoreL3Condition            = 14;
static constexpr uint32_t RegistersCount              = 8U;

// Soft-AES lookup table stubs (only used when RANDOMX_FLAG_V2 + soft AES path)
// On AArch64 with hardware crypto these branches are never taken,
// but the symbols must exist to avoid link errors.
extern uint32_t randomx_aes_lut_enc[4][256];
extern uint32_t randomx_aes_lut_dec[4][256];

namespace armrx {
namespace ARMV8A {

constexpr uint32_t B           = 0x14000000;
constexpr uint32_t EOR         = 0xCA000000;
constexpr uint32_t EOR32       = 0x4A000000;
constexpr uint32_t ADD         = 0x8B000000;
constexpr uint32_t SUB         = 0xCB000000;
constexpr uint32_t MUL         = 0x9B007C00;
constexpr uint32_t UMULH       = 0x9BC07C00;
constexpr uint32_t SMULH       = 0x9B407C00;
constexpr uint32_t MOVZ        = 0xD2800000;
constexpr uint32_t MOVN        = 0x92800000;
constexpr uint32_t MOVK        = 0xF2800000;
constexpr uint32_t ADD_IMM_LO  = 0x91000000;
constexpr uint32_t ADD_IMM_HI  = 0x91400000;
constexpr uint32_t LDR_LITERAL = 0x58000000;
constexpr uint32_t ROR         = 0x9AC02C00;
constexpr uint32_t ROR_IMM     = 0x93C00000;
constexpr uint32_t MOV_REG     = 0xAA0003E0;
constexpr uint32_t MOV_VREG_EL = 0x6E080400;
constexpr uint32_t FADD        = 0x4E60D400;
constexpr uint32_t FSUB        = 0x4EE0D400;
constexpr uint32_t FEOR        = 0x6E201C00;
constexpr uint32_t FMUL        = 0x6E60DC00;
constexpr uint32_t FDIV        = 0x6E60FC00;
constexpr uint32_t FSQRT       = 0x6EE1F800;

} // namespace ARMV8A

static const size_t CodeSize = ((uint8_t*)randomx_init_dataset_aarch64_end) - ((uint8_t*)randomx_program_aarch64);
static const size_t MainLoopBegin = ((uint8_t*)randomx_program_aarch64_main_loop) - ((uint8_t*)randomx_program_aarch64);
static const size_t PrologueSize = ((uint8_t*)randomx_program_aarch64_vm_instructions) - ((uint8_t*)randomx_program_aarch64);
static const size_t ImulRcpLiteralsEnd = ((uint8_t*)randomx_program_aarch64_imul_rcp_literals_end) - ((uint8_t*)randomx_program_aarch64);

static const size_t CalcDatasetItemSize =
	// Prologue
	((uint8_t*)randomx_calc_dataset_item_aarch64_prefetch - (uint8_t*)randomx_calc_dataset_item_aarch64) +
	// Main loop
	RANDOMX_CACHE_ACCESSES * (
		// Main loop prologue
		((uint8_t*)randomx_calc_dataset_item_aarch64_mix - ((uint8_t*)randomx_calc_dataset_item_aarch64_prefetch)) + 4 +
		// Inner main loop (instructions)
		((RANDOMX_SUPERSCALAR_LATENCY * 3) + 2) * 16 +
		// Main loop epilogue
		((uint8_t*)randomx_calc_dataset_item_aarch64_store_result - (uint8_t*)randomx_calc_dataset_item_aarch64_mix) + 4
	) +
	// Epilogue
	((uint8_t*)randomx_calc_dataset_item_aarch64_end - (uint8_t*)randomx_calc_dataset_item_aarch64_store_result);

constexpr uint32_t IntRegMap[8] = { 4, 5, 6, 7, 12, 13, 14, 15 };

template<typename T> static constexpr size_t Log2(T value) { return (value > 1) ? (Log2(value / 2) + 1) : 0; }

// Computed once here instead of at each of its 3 call sites below — same
// value (21), just a single source of truth instead of three independent
// re-derivations of Log2(RANDOMX_SCRATCHPAD_L3).
static constexpr uint32_t ScratchpadL3Log2 = static_cast<uint32_t>(Log2(RANDOMX_SCRATCHPAD_L3));

JitCompilerA64::JitCompilerA64()
	: code((uint8_t*) allocMemoryPages(CodeSize + CalcDatasetItemSize))
	, literalPos(ImulRcpLiteralsEnd)
	, num32bitLiterals(0)
{
	if (code == nullptr)
		throw std::runtime_error("allocMemoryPages");
	memset(reg_changed_offset, 0, sizeof(reg_changed_offset));
	memcpy(code, (void*) randomx_program_aarch64, CodeSize);

#ifdef __GNUC__
	__builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code + CodeSize));
#endif

	// Try RWX once; if the kernel allows it, per-hash mprotect calls become no-ops
	// On secure platforms (OpenBSD, NetBSD, macOS), skip RWX entirely
#ifndef RANDOMX_FORCE_SECURE
	rwx_ = (setPagesRWX(code, CodeSize + CalcDatasetItemSize) == 0);
#else
	rwx_ = false;
#endif

	// One JitCompilerA64 exists per worker thread and all of them land on the
	// same rwx_ result (same process, same kernel policy), so only the first
	// one logs it -- otherwise this would print once per worker. This is a
	// real, deliberate perf/security tradeoff (RWX skips an mprotect syscall
	// pair on every JIT recompile) that was previously silent to operators;
	// see PLAN.md Phase 4 item F / NEXT_STEPS.md item 4.
	static std::atomic<bool> logged{false};
	if (!logged.exchange(true, std::memory_order_relaxed)) {
		if (rwx_) {
			ARMRX_LOG_INFO << "JIT code buffer: RWX (read+write+execute always set; "
			                  "faster, no per-recompile mprotect calls -- rebuild with "
			                  "RANDOMX_FORCE_SECURE to enforce W^X instead)";
		} else {
			ARMRX_LOG_INFO << "JIT code buffer: W^X enforced (RW while compiling, "
			                  "RX while executing, never both)";
		}
	}
}

JitCompilerA64::~JitCompilerA64()
{
	freePagedMemory(code, CodeSize + CalcDatasetItemSize);
}

bool JitCompilerA64::enableWriting()
{
	if (rwx_) return true;
	return setPagesRW(code, CodeSize + CalcDatasetItemSize) == 0;
}

bool JitCompilerA64::enableExecution()
{
	if (rwx_) return true;
	return setPagesRX(code, CodeSize + CalcDatasetItemSize) == 0;
}

// Shared helper: emit the v2 AES tweak block (identical in fast and light paths).
void JitCompilerA64::emitV2AesTweak(JitCompilerA64& jit, uint32_t flags, uint32_t codePos) {
	if (flags & RANDOMX_FLAG_V2) {
		if (flags & RANDOMX_FLAG_HARD_AES) {
			JitCompilerA64::emit32(0x4F00041C, jit.code, codePos);
		} else {
			uint32_t offset = (uint8_t*)randomx_program_aarch64_v2_FE_mix_soft_aes - (uint8_t*)randomx_program_aarch64_v2_FE_mix;
			JitCompilerA64::emit32(ARMV8A::B | (offset / 4), jit.code, codePos);
			offset = (uint8_t*)randomx_program_aarch64_aes_lut_pointers - (uint8_t*)randomx_program_aarch64;
			const void* lut_enc = &randomx_aes_lut_enc[0][0];
			const void* lut_dec = &randomx_aes_lut_dec[0][0];
			memcpy(jit.code + offset + 0, &lut_enc, sizeof(lut_enc));
			memcpy(jit.code + offset + 8, &lut_dec, sizeof(lut_dec));
		}
	} else {
		const uint32_t offset = (uint8_t*)randomx_program_aarch64_v1_FE_mix - (uint8_t*)randomx_program_aarch64_v2_FE_mix;
		JitCompilerA64::emit32(ARMV8A::B | (offset / 4), jit.code, codePos);
	}
}

// ── Emitter lookahead scheduler (PLAN.md Phase 6 item 12/L1, 2026-07-25) ──
//
// Goal: reorder VM-instruction *emission* order (never their computed
// results) to fill the stall right after a long-latency multiply
// (IMUL_R/IMUL_RCP/IMULH_R/ISMULH_R -- item 14's own `perf`-measured
// dominant cost, >35% of all mining cycles combined, not a guess) with
// independent work, instead of letting the very next instruction stall
// on it uselessly. Deliberately narrow in scope (single adjacent-pair
// swaps, not a general list scheduler) to keep the correctness argument
// tractable for consensus-critical code.
//
// Correctness constraints, in order of subtlety:
//
// 1. Register/memory hazards: a swap of two VM instructions is only safe
//    if there's no RAW/WAR/WAW dependency between them on any register
//    they touch (across the int r[], float f[], and float e[] register
//    files independently), and not if either is a memory op (scratchpad
//    addresses are dynamic/unknown at compile time, so ANY two memory
//    ops are conservatively treated as potentially aliasing -- never
//    reordered relative to each other, matching the plan's own rule).
//    computeFootprint()/hasHazard() below implement this, deliberately
//    over-approximating register reads where a per-instance src==dst
//    substitution (see vm.cpp's compile_alu_reg) would otherwise reduce
//    a dependency -- a false "this is a hazard" only forgoes a
//    reordering opportunity, never causes an incorrect reorder.
//
// 2. CBRANCH/CFROUND are hard scheduling barriers: nothing may be
//    reordered across either, in either direction. CFROUND changes the
//    FP rounding mode used by every subsequent FP op (implicit global
//    state); CBRANCH's own dst/src semantics are handled by (3) below.
//
// 3. The CBRANCH "anchor" hazard -- the one that isn't obvious from a
//    simple data-dependency reading. CBRANCH doesn't just branch: per
//    RandomX spec (and this codebase's reg_changed_offset/
//    register_usage_ mechanism, see vm.cpp h_CBRANCH and
//    jit_compiler_a64.cpp h_CBRANCH), it jumps *backward* to the code
//    position of the last instruction that wrote its target register,
//    and everything between that position and the branch is
//    *re-executed* on every taken iteration -- a real loop in the
//    generated code, not a conditional skip. The interpreter's loop body
//    is the *index range* [last-writer-index, branch-index] in original
//    program order, always, regardless of any JIT-side reordering.
//    If a swap moved some instruction X from *after* that last-writer
//    instruction W to *before* it, X would end up *outside* the JIT's
//    physical loop body (only W's position and everything after it are
//    inside), executing once instead of being re-executed each
//    iteration the interpreter would re-execute it -- a silent
//    byte-level divergence between JIT and interpreter output for that
//    specific program shape, undetectable except by running exactly the
//    right seed. This is why W (the "anchor") must never have its
//    position perturbed relative to its neighbors within the domain: the
//    fix is to precompute, for each CBRANCH, which single instruction is
//    its anchor (a plain backward scan for the last writer of its target
//    register within the same reset domain -- CBRANCH resets tracking
//    for *all* registers when it executes, so no anchor search ever
//    needs to cross an earlier CBRANCH; confirmed by both vm.cpp's
//    std::fill(register_usage_, -1) once at program-compile start and
//    every CBRANCH's own for-loop resetting all 8 afterward) and simply
//    never swap either half of a candidate pair against that anchor
//    index.
//
// 4. The src==dst swap exclusion -- found by the stress test below, not
//    anticipated by design. Excluding any instruction with src==dst from
//    the Q/R (moving) positions of a swap eliminates the divergence a
//    bisection isolated to one exact swap. This much is empirically
//    solid and independently re-verified: three independent reviews
//    (2026-07-25, docs/audits/{emitter-scheduler-review,
//    jit_scheduler_code_review_gemini,scheduler-review-2026-07-25}.md)
//    all confirm no failure scenario exists in the shipped code with this
//    exclusion in place.
//
//    An earlier version of this comment claimed the mechanism was a
//    "shared physical scratch register (x20) invisible to the hazard
//    model" -- that explanation does NOT hold up and is WRONG, per two of
//    the three reviews (independently, with line citations): every
//    src==dst handler that touches x20 (h_ISUB_R, h_IMUL_R, h_IXOR_R, and
//    transitively emitAddImmediate/emitMovImmediate) writes and reads it
//    back-to-back within its OWN emission, with no other instruction's
//    code between the write and the read -- a swap reorders whole
//    handlers, never splits one, so there is no observable cross-handler
//    race on x20 regardless of emission order. h_IROL_R is not even an
//    instance of the pattern: it uses x20 when src != dst (SUB+ROR), and
//    takes a *different*, x20-free path (ROR_IMM, rotate by compile-time
//    immediate) specifically when src == dst -- the exact opposite of
//    what this comment used to claim. One of the three reviews (Gemini)
//    restated the original (wrong) claim without checking it against
//    h_IROL_R's actual code; the other two (independently) caught the
//    discrepancy.
//
//    A further argument (from the review flagging this) is worth
//    recording: computeFootprint() already gives src==dst instructions
//    read=write=(1<<dst), identical to src!=dst -- so the ordinary hazard
//    check already blocks any swap where a neighbor shares register dst.
//    The src==dst exclusion can only ever change anything in the
//    remaining case, where the src==dst instruction shares NO register
//    with its neighbor -- and in that case the x20 story gives no
//    hazard either, since x20 never survives past its own handler. So
//    the MECHANISM behind the exclusion remains unresolved -- but its
//    NECESSITY does not: the original bisection that led to this
//    exclusion (see the git history for this file, 2026-07-25) isolated
//    a real, concrete divergence to one exact swap whose R was
//    ISUB_R(dst==src==7) -- a genuine src==dst instance, confirmed by
//    reproducing the divergence with the exclusion disabled and the
//    match with it enabled. That rules out "masks nothing real" for at
//    least this case. What's still open is narrower than it first
//    looks: whether every src==dst instance needs excluding, or only
//    ones sharing this specific (still unidentified) trait with the
//    original failing case -- a question for a future, carefully-built
//    differential harness if this area is revisited, not resolved here.
//
//    Applies to Q and R (the two instructions whose relative order
//    actually changes); P never moves, so P having src==dst is left
//    unrestricted and is covered by the stress test below regardless.
//
// Verified with a dedicated large-N-random-seed JIT/interpreter
// differential stress test (tests/test_jit_scheduler_stress.cpp), not
// just the existing 16-seed test_jit_equivalence -- the failure mode
// here is silent wrong output for specific rare program shapes, which a
// small fixed seed corpus has no particular reason to hit. This is how
// constraint 4 above was actually found: the stress test failed, was
// bisected down to a single swap via an internal debug build (not
// shipped), and root-caused from there.

namespace {

struct InstrFootprint {
	std::uint8_t int_read = 0, int_write = 0;   // bit i = register r[i]
	std::uint8_t f_read = 0, f_write = 0;       // bit i = register f[i]
	std::uint8_t e_read = 0, e_write = 0;       // bit i = register e[i]
	bool is_memory_op = false;
	bool is_barrier = false;   // CBRANCH or CFROUND -- no reordering across
	bool is_cbranch = false;   // specifically CBRANCH -- domain boundary
	bool is_long_latency = false; // IMUL_R / IMUL_RCP / IMULH_R / ISMULH_R
	bool is_imul_rcp = false; // superscalar path only -- see scheduleSuperscalarProgram()
};

InstrFootprint computeFootprint(const Instruction& instr, InstructionType type) {
	InstrFootprint fp;
	const std::uint8_t dst8 = instr.dst;   // already normalized %8 by caller
	const std::uint8_t src8 = instr.src;   // already normalized %8 by caller
	const std::uint8_t dst4 = dst8 % 4;

	switch (type) {
	case InstructionType::IADD_RS:
	case InstructionType::ISUB_R:
	case InstructionType::IMUL_R:
	case InstructionType::IMULH_R:
	case InstructionType::ISMULH_R:
	case InstructionType::IXOR_R:
	case InstructionType::IROR_R:
	case InstructionType::IROL_R:
		fp.int_read = static_cast<std::uint8_t>((1u << dst8) | (1u << src8));
		fp.int_write = static_cast<std::uint8_t>(1u << dst8);
		fp.is_long_latency = (type == InstructionType::IMUL_R ||
		                      type == InstructionType::IMULH_R ||
		                      type == InstructionType::ISMULH_R);
		break;
	case InstructionType::IADD_M:
	case InstructionType::ISUB_M:
	case InstructionType::IMUL_M:
	case InstructionType::IMULH_M:
	case InstructionType::ISMULH_M:
	case InstructionType::IXOR_M:
		fp.int_read = static_cast<std::uint8_t>((1u << dst8) | (1u << src8));
		fp.int_write = static_cast<std::uint8_t>(1u << dst8);
		fp.is_memory_op = true;
		break;
	case InstructionType::IMUL_RCP:
		// No src register read at all: the divisor is a compile-time
		// immediate (instr.getImm32()), not derived from instr.src --
		// see h_IMUL_RCP in both vm.cpp and this file.
		fp.int_read = static_cast<std::uint8_t>(1u << dst8);
		fp.int_write = static_cast<std::uint8_t>(1u << dst8);
		fp.is_long_latency = true;
		break;
	case InstructionType::INEG_R:
		fp.int_read = static_cast<std::uint8_t>(1u << dst8);
		fp.int_write = static_cast<std::uint8_t>(1u << dst8);
		break;
	case InstructionType::ISWAP_R:
		// Conservatively read+write both dst and src, even though
		// src==dst is a runtime NOP (h_ISWAP_R) -- see the
		// over-approximation note above.
		fp.int_read = static_cast<std::uint8_t>((1u << dst8) | (1u << src8));
		fp.int_write = static_cast<std::uint8_t>((1u << dst8) | (1u << src8));
		break;
	case InstructionType::FSWAP_R:
		// dst%8 (not %4!) selects F[0..3] or E[0..3] -- matches h_FSWAP_R
		// exactly (both vm.cpp's and this file's).
		if (dst8 < 4) { fp.f_read = fp.f_write = static_cast<std::uint8_t>(1u << dst8); }
		else          { fp.e_read = fp.e_write = static_cast<std::uint8_t>(1u << (dst8 - 4)); }
		break;
	case InstructionType::FADD_R:
	case InstructionType::FSUB_R:
	case InstructionType::FSCAL_R:
		// src (for FADD_R/FSUB_R) selects an A-group register -- a
		// read-only per-program constant that no instruction ever
		// writes, so it can never hazard and isn't tracked.
		fp.f_read = fp.f_write = static_cast<std::uint8_t>(1u << dst4);
		break;
	case InstructionType::FMUL_R:
	case InstructionType::FSQRT_R:
		fp.e_read = fp.e_write = static_cast<std::uint8_t>(1u << dst4);
		break;
	case InstructionType::FADD_M:
	case InstructionType::FSUB_M:
		fp.f_read = fp.f_write = static_cast<std::uint8_t>(1u << dst4);
		fp.int_read = static_cast<std::uint8_t>(1u << src8); // address register
		fp.is_memory_op = true;
		break;
	case InstructionType::FDIV_M:
		fp.e_read = fp.e_write = static_cast<std::uint8_t>(1u << dst4);
		fp.int_read = static_cast<std::uint8_t>(1u << src8);
		fp.is_memory_op = true;
		break;
	case InstructionType::CBRANCH:
		fp.int_read = fp.int_write = static_cast<std::uint8_t>(1u << dst8);
		fp.is_barrier = true;
		fp.is_cbranch = true;
		break;
	case InstructionType::CFROUND:
		fp.int_read = static_cast<std::uint8_t>(1u << src8);
		fp.is_barrier = true;
		break;
	case InstructionType::ISTORE:
		// Writes scratchpad memory, not a register -- reads dst (address)
		// and src (value to store) as int registers.
		fp.int_read = static_cast<std::uint8_t>((1u << dst8) | (1u << src8));
		fp.is_memory_op = true;
		break;
	case InstructionType::NOP:
	default:
		break;
	}
	return fp;
}

bool hasHazard(const InstrFootprint& a, const InstrFootprint& b) {
	if (a.is_barrier || b.is_barrier) return true;
	if (a.is_memory_op && b.is_memory_op) return true;
	if ((a.int_write & b.int_read) || (a.int_read & b.int_write) || (a.int_write & b.int_write)) return true;
	if ((a.f_write & b.f_read) || (a.f_read & b.f_write) || (a.f_write & b.f_write)) return true;
	if ((a.e_write & b.e_read) || (a.e_read & b.e_write) || (a.e_write & b.e_write)) return true;
	return false;
}

// ── Superscalar/dataset-derivation scheduler (PLAN.md Phase 6 item 12
// extension, 2026-07-25) ────────────────────────────────────────────────
//
// item 14's own perf-correlation finding (>35% of ALL mining cycles in
// IMUL_R/IMULH_R/ISMULH_R/IMUL_RCP) was measured in THIS region
// (generateSuperscalarHash()'s output, executed via `bl
// rx_calc_dataset_item` 16,384x/hash in light mode) -- not the main VM
// program scheduleProgram() above targets, which is a much smaller share
// of total instruction volume. The first version of this scheduler only
// covered the main program and measured as a clean null (see
// changelogs.md's "PLAN.md Phase 6 Item 12" entries) for exactly this
// reason: it was reordering the wrong region.
//
// This region's instruction set (SuperscalarInstructionType, 14 opcodes)
// is structurally simpler than the main VM program's in three ways that
// matter for the scheduler's correctness argument:
//   1. No CBRANCH/CFROUND exist here at all -- superscalar programs are
//      spec-defined as straight-line integer-only sequences (dataset
//      expansion, not general VM execution). No barriers, no anchors.
//   2. No memory operations exist here -- every opcode is register-to-
//      register or register-to-immediate. No memory-aliasing rule needed.
//   3. A single flat 8-register integer file (r[0..7], mapped directly to
//      physical x0..x7 by generateSuperscalarHash()'s emission switch) --
//      no separate f/e float files to track.
// computeSuperscalarFootprint() below reuses InstrFootprint but only ever
// populates int_read/int_write/is_long_latency/is_imul_rcp.
//
// A fourth hazard exists that has NO analogue in the main-program
// scheduler, found by reading generateSuperscalarHash() itself (not by
// stress-test bisection this time): IMUL_RCP's reciprocal literals are
// populated by a PRE-PASS, in original program order, into a pool
// immediately after the program's jump-over-pool instruction; the main
// emission loop then consumes that pool via a simple incrementing
// `literal_pos` pointer, once per IMUL_RCP instruction IT encounters, in
// WHATEVER ORDER IT EMITS THEM. This is safe under scheduling only as
// long as no two IMUL_RCP instructions ever have their relative emission
// order changed -- if two swapped, the k-th IMUL_RCP encountered during
// emission would consume the (k-th original-order IMUL_RCP)'s literal
// slot instead of its own, silently multiplying by the wrong reciprocal.
// (This is the superscalar-path analogue of the main scheduler's src==dst
// hazard -- an implicit ordering assumption invisible to a pure
// register-level hazard model. The main path's own h_IMUL_RCP was
// checked and does NOT have this problem: it computes its `literal_id`
// from its own call count and writes its own reciprocal in the same call,
// entirely self-contained regardless of emission order -- see
// jit_compiler_a64.cpp's h_IMUL_RCP. Only this pre-pass/sequential-
// consume design is order-sensitive.) Since every swap here only ever
// reorders two *adjacent* instructions within one local 3-instruction
// window, excluding any swap where either moving instruction (Q or R) is
// IMUL_RCP is sufficient to guarantee no two IMUL_RCP instructions ever
// have their relative order perturbed (P, the anchor, never moves at
// all) -- simpler to state and verify than the tighter "only unsafe if
// BOTH Q and R are IMUL_RCP" condition, matching this file's established
// bias toward over-approximating hazards rather than under-approximating
// them.
InstrFootprint computeSuperscalarFootprint(const Instruction& instr, SuperscalarInstructionType type) {
	InstrFootprint fp;
	const std::uint8_t dst = static_cast<std::uint8_t>(instr.dst);
	const std::uint8_t src = static_cast<std::uint8_t>(instr.src);

	switch (type) {
	case SuperscalarInstructionType::ISUB_R:
	case SuperscalarInstructionType::IXOR_R:
	case SuperscalarInstructionType::IADD_RS:
	case SuperscalarInstructionType::IMUL_R:
	case SuperscalarInstructionType::IMULH_R:
	case SuperscalarInstructionType::ISMULH_R:
		fp.int_read = static_cast<std::uint8_t>((1u << dst) | (1u << src));
		fp.int_write = static_cast<std::uint8_t>(1u << dst);
		fp.is_long_latency = (type == SuperscalarInstructionType::IMUL_R ||
		                      type == SuperscalarInstructionType::IMULH_R ||
		                      type == SuperscalarInstructionType::ISMULH_R);
		break;
	case SuperscalarInstructionType::IROR_C:
	case SuperscalarInstructionType::IADD_C7:
	case SuperscalarInstructionType::IADD_C8:
	case SuperscalarInstructionType::IADD_C9:
	case SuperscalarInstructionType::IXOR_C7:
	case SuperscalarInstructionType::IXOR_C8:
	case SuperscalarInstructionType::IXOR_C9:
		// Immediate operand (compile-time constant) -- only dst
		// participates as a VM register. IXOR_C*'s transient physical
		// tmp_reg=x12 (MOVZ/MOVK then immediate EOR) is written and
		// consumed entirely within this one instruction's own emission,
		// never read by any other instruction -- not a cross-instruction
		// hazard, so not tracked here.
		fp.int_read = fp.int_write = static_cast<std::uint8_t>(1u << dst);
		break;
	case SuperscalarInstructionType::IMUL_RCP:
		fp.int_read = fp.int_write = static_cast<std::uint8_t>(1u << dst);
		fp.is_long_latency = true;
		fp.is_imul_rcp = true;
		break;
	default:
		break;
	}
	return fp;
}

} // namespace

InstructionType JitCompilerA64::resolveInstructionType(uint8_t opcode) const {
	const InstructionGeneratorA64 h = engine[opcode];
	if (h == &JitCompilerA64::h_IADD_RS) return InstructionType::IADD_RS;
	if (h == &JitCompilerA64::h_IADD_M) return InstructionType::IADD_M;
	if (h == &JitCompilerA64::h_ISUB_R) return InstructionType::ISUB_R;
	if (h == &JitCompilerA64::h_ISUB_M) return InstructionType::ISUB_M;
	if (h == &JitCompilerA64::h_IMUL_R) return InstructionType::IMUL_R;
	if (h == &JitCompilerA64::h_IMUL_M) return InstructionType::IMUL_M;
	if (h == &JitCompilerA64::h_IMULH_R) return InstructionType::IMULH_R;
	if (h == &JitCompilerA64::h_IMULH_M) return InstructionType::IMULH_M;
	if (h == &JitCompilerA64::h_ISMULH_R) return InstructionType::ISMULH_R;
	if (h == &JitCompilerA64::h_ISMULH_M) return InstructionType::ISMULH_M;
	if (h == &JitCompilerA64::h_IMUL_RCP) return InstructionType::IMUL_RCP;
	if (h == &JitCompilerA64::h_INEG_R) return InstructionType::INEG_R;
	if (h == &JitCompilerA64::h_IXOR_R) return InstructionType::IXOR_R;
	if (h == &JitCompilerA64::h_IXOR_M) return InstructionType::IXOR_M;
	if (h == &JitCompilerA64::h_IROR_R) return InstructionType::IROR_R;
	if (h == &JitCompilerA64::h_IROL_R) return InstructionType::IROL_R;
	if (h == &JitCompilerA64::h_ISWAP_R) return InstructionType::ISWAP_R;
	if (h == &JitCompilerA64::h_FSWAP_R) return InstructionType::FSWAP_R;
	if (h == &JitCompilerA64::h_FADD_R) return InstructionType::FADD_R;
	if (h == &JitCompilerA64::h_FADD_M) return InstructionType::FADD_M;
	if (h == &JitCompilerA64::h_FSUB_R) return InstructionType::FSUB_R;
	if (h == &JitCompilerA64::h_FSUB_M) return InstructionType::FSUB_M;
	if (h == &JitCompilerA64::h_FSCAL_R) return InstructionType::FSCAL_R;
	if (h == &JitCompilerA64::h_FMUL_R) return InstructionType::FMUL_R;
	if (h == &JitCompilerA64::h_FDIV_M) return InstructionType::FDIV_M;
	if (h == &JitCompilerA64::h_FSQRT_R) return InstructionType::FSQRT_R;
	if (h == &JitCompilerA64::h_CBRANCH) return InstructionType::CBRANCH;
	if (h == &JitCompilerA64::h_CFROUND) return InstructionType::CFROUND;
	if (h == &JitCompilerA64::h_ISTORE) return InstructionType::ISTORE;
	if (h == &JitCompilerA64::h_NOP) return InstructionType::NOP;

	// Maintenance hazard flagged by independent review (2026-07-25,
	// docs/audits/scheduler-review-2026-07-25.md #4): this if-chain and
	// engine[256] (built from instruction_weights.hpp's INST_HANDLE macro)
	// are not mechanically linked. A future opcode wired into engine[] but
	// not added above would previously fall through to `return NOP`,
	// giving computeFootprint() an all-zero (no-hazard) footprint for a
	// REAL instruction -- silently permitting the scheduler to swap
	// something unsafely across it. Fail loud (debug) and fail SAFE
	// (release): CFROUND's footprint sets is_barrier=true, which blocks
	// all scheduling across this position rather than permitting it.
	ARMRX_ASSERT(false, "resolveInstructionType: unrecognized JIT handler -- add it above");
	return InstructionType::CFROUND;
}

std::vector<uint32_t> JitCompilerA64::scheduleProgram(Program& program, uint32_t size) const {
	std::vector<InstrFootprint> fp(size);
	for (uint32_t i = 0; i < size; ++i) {
		const Instruction& instr = program(i);
		fp[i] = computeFootprint(instr, resolveInstructionType(instr.opcode));
	}

	// Per-domain CBRANCH anchors (see constraint 3 above). A domain is a
	// CBRANCH-to-CBRANCH range (CBRANCH resets tracking for all 8
	// registers, so no anchor search ever needs to cross one).
	std::vector<bool> is_anchor(size, false);
	{
		uint32_t domain_start = 0;
		for (uint32_t i = 0; i < size; ++i) {
			if (fp[i].is_cbranch) {
				const std::uint8_t creg = program(i).dst;
				for (uint32_t j = i; j-- > domain_start; ) {
					if (fp[j].int_write & (1u << creg)) {
						is_anchor[j] = true;
						break;
					}
				}
				domain_start = i + 1;
			}
		}
	}

	// Greedy single-swap scheduling. At each long-latency instruction P,
	// if the very next instruction Q would stall on P anyway, and the
	// instruction after that (R) is independent of both P and Q, and
	// neither Q nor R is a domain anchor or has src==dst (constraint 4),
	// emit P, R, Q instead of P, Q, R -- R fills the slot that would have
	// stalled, Q (which needed to wait regardless) moves one slot later
	// where it no longer costs anything extra.
	std::vector<uint32_t> order;
	order.reserve(size);
	uint32_t i = 0;
	while (i < size) {
		if (fp[i].is_barrier) {
			order.push_back(i);
			++i;
			continue;
		}
		const bool q_src_eq_dst = i + 1 < size && program(i + 1).src == program(i + 1).dst;
		const bool r_src_eq_dst = i + 2 < size && program(i + 2).src == program(i + 2).dst;
		if (fp[i].is_long_latency && i + 2 < size &&
		    !fp[i + 1].is_barrier && !fp[i + 2].is_barrier &&
		    !is_anchor[i + 1] && !is_anchor[i + 2] &&
		    !q_src_eq_dst && !r_src_eq_dst &&
		    hasHazard(fp[i], fp[i + 1]) &&
		    !hasHazard(fp[i], fp[i + 2]) &&
		    !hasHazard(fp[i + 1], fp[i + 2])) {
			order.push_back(i);
			order.push_back(i + 2);
			order.push_back(i + 1);
			i += 3;
		} else {
			order.push_back(i);
			++i;
		}
	}
	return order;
}

// See the doc comment above computeSuperscalarFootprint() (anonymous
// namespace, above) for the full correctness argument -- no barriers, no
// memory ops, no anchors needed here, but IMUL_RCP must never be Q or R
// of a swap (literal-pool ordering hazard, distinct from the main
// program's src==dst hazard).
std::vector<uint32_t> JitCompilerA64::scheduleSuperscalarProgram(const SuperscalarProgram& program) const {
	const uint32_t size = program.size();
	std::vector<InstrFootprint> fp(size);
	for (uint32_t i = 0; i < size; ++i) {
		const Instruction& instr = program(i);
		fp[i] = computeSuperscalarFootprint(instr, static_cast<SuperscalarInstructionType>(instr.opcode));
	}

	std::vector<uint32_t> order;
	order.reserve(size);
	uint32_t i = 0;
	while (i < size) {
		const bool q_is_imul_rcp = i + 1 < size && fp[i + 1].is_imul_rcp;
		const bool r_is_imul_rcp = i + 2 < size && fp[i + 2].is_imul_rcp;
		if (fp[i].is_long_latency && i + 2 < size &&
		    !q_is_imul_rcp && !r_is_imul_rcp &&
		    hasHazard(fp[i], fp[i + 1]) &&
		    !hasHazard(fp[i], fp[i + 2]) &&
		    !hasHazard(fp[i + 1], fp[i + 2])) {
			order.push_back(i);
			order.push_back(i + 2);
			order.push_back(i + 1);
			i += 3;
		} else {
			order.push_back(i);
			++i;
		}
	}
	return order;
}

void JitCompilerA64::emitPrologueMix(Program& program, uint32_t& codePos) {
	codePos = PrologueSize;
	literalPos = ImulRcpLiteralsEnd;
	num32bitLiterals = 0;

	for (uint32_t i = 0; i < RegistersCount; ++i)
		reg_changed_offset[i] = codePos;

	const uint32_t size = static_cast<uint32_t>(program.getSize(flags));

	// Normalize dst/src to register-file range first, as a separate pass
	// (order-independent, so safe to do before scheduling) -- matches
	// what the original single-pass loop did inline for every
	// instruction before its handler ran.
	for (uint32_t i = 0; i < size; ++i) {
		Instruction& instr = program(i);
		instr.src %= RegistersCount;
		instr.dst %= RegistersCount;
	}

	const std::vector<uint32_t> emit_order = scheduleProgram(program, size);

	for (uint32_t idx : emit_order)
	{
		Instruction& instr = program(idx);
		ARMRX_ASSERT(engine[instr.opcode] != nullptr, "null JIT handler for opcode");
		const uint32_t pos_before = codePos;
		(this->*engine[instr.opcode])(instr, codePos);
		if (jit_dump_enabled_) {
			jit_dump_.push_back({instr.opcode, pos_before, codePos - pos_before});
		}
	}
}

void JitCompilerA64::emitSpMix2(ProgramConfiguration& config, uint32_t& codePos) {
	// Update spMix1
	// eor x10, config.readReg0, config.readReg1
	codePos = ((uint8_t*)randomx_program_aarch64_update_spMix1) - ((uint8_t*)randomx_program_aarch64);
	emit32(ARMV8A::EOR | 10 | (IntRegMap[config.readReg0] << 5) | (IntRegMap[config.readReg1] << 16), code, codePos);

	// ubfx x19, x10, #6, #width (width = Log2(RANDOMX_SCRATCHPAD_L3) - 6)
	emit32(0xD3400000 | 19 | (10 << 5) | (6 << 16) | ((ScratchpadL3Log2 - 1) << 10), code, codePos);

	// ubfx x20, x10, #38, #width
	emit32(0xD3400000 | 20 | (10 << 5) | (38 << 16) | ((32 + ScratchpadL3Log2 - 1) << 10), code, codePos);

	codePos = ((uint8_t*)randomx_program_aarch64_v2_FE_mix) - ((uint8_t*)randomx_program_aarch64);
	emitV2AesTweak(*this, flags, codePos);
}

void JitCompilerA64::generateProgram(Program& program, ProgramConfiguration& config)
{
	uint32_t codePos;
	emitPrologueMix(program, codePos);

	// Update spMix2
	// eor w20, config.readReg2, config.readReg3
	emit32(ARMV8A::EOR32 | 20 | (IntRegMap[config.readReg2] << 5) | (IntRegMap[config.readReg3] << 16), code, codePos);

	// Jump back to the main loop
	const uint32_t offset = (((uint8_t*)randomx_program_aarch64_vm_instructions_end) - ((uint8_t*)randomx_program_aarch64)) - codePos;
	emit32(ARMV8A::B | (offset / 4), code, codePos);

	// and w20, w20, CacheLineAlignMask
	codePos = (((uint8_t*)randomx_program_aarch64_cacheline_align_mask1) - ((uint8_t*)randomx_program_aarch64));
	emit32(0x121A0000 | 20 | (20 << 5) | ((Log2(RANDOMX_DATASET_BASE_SIZE) - 7) << 10), code, codePos);

	// and w10, w10, CacheLineAlignMask
	codePos = (((uint8_t*)randomx_program_aarch64_cacheline_align_mask2) - ((uint8_t*)randomx_program_aarch64));
	emit32(0x121A0000 | 10 | (10 << 5) | ((Log2(RANDOMX_DATASET_BASE_SIZE) - 7) << 10), code, codePos);

	emitSpMix2(config, codePos);

	// Apply v2 prefetch tweak
	if (flags & RANDOMX_FLAG_V2) {
		uint32_t dst = (((uint8_t*)randomx_program_aarch64_vm_instructions_end) - ((uint8_t*)randomx_program_aarch64));
		uint32_t src = (((uint8_t*)randomx_program_aarch64_vm_instructions_end_v2) - ((uint8_t*)randomx_program_aarch64));
		memcpy(code + dst, code + src, 16);
	}
	else {
		uint32_t dst = (((uint8_t*)randomx_program_aarch64_vm_instructions_end) - ((uint8_t*)randomx_program_aarch64));
		uint32_t src = (((uint8_t*)randomx_program_aarch64_vm_instructions_end_v1) - ((uint8_t*)randomx_program_aarch64));
		memcpy(code + dst, code + src, 16);
	}

#ifdef __GNUC__
	__builtin___clear_cache(reinterpret_cast<char*>(code + MainLoopBegin), reinterpret_cast<char*>(code + codePos));
#endif
}

void JitCompilerA64::generateProgramLight(Program& program, ProgramConfiguration& config, uint32_t datasetOffset)
{
	uint32_t codePos;
	emitPrologueMix(program, codePos);

	// Update spMix2
	// eor w20, config.readReg2, config.readReg3
	emit32(ARMV8A::EOR32 | 20 | (IntRegMap[config.readReg2] << 5) | (IntRegMap[config.readReg3] << 16), code, codePos);

	// Apply v2 prefetch tweak
	if (flags & RANDOMX_FLAG_V2) {
		uint32_t dst = (((uint8_t*)randomx_program_aarch64_vm_instructions_end_light_tweak) - ((uint8_t*)randomx_program_aarch64));
		uint32_t src = (((uint8_t*)randomx_program_aarch64_vm_instructions_end_light_v2) - ((uint8_t*)randomx_program_aarch64));
		memcpy(code + dst, code + src, 8);
	}
	else {
		uint32_t dst = (((uint8_t*)randomx_program_aarch64_vm_instructions_end_light_tweak) - ((uint8_t*)randomx_program_aarch64));
		uint32_t src = (((uint8_t*)randomx_program_aarch64_vm_instructions_end_light_v1) - ((uint8_t*)randomx_program_aarch64));
		memcpy(code + dst, code + src, 8);
	}

	// Jump back to the main loop
	const uint32_t offset = (((uint8_t*)randomx_program_aarch64_vm_instructions_end_light) - ((uint8_t*)randomx_program_aarch64)) - codePos;
	emit32(ARMV8A::B | (offset / 4), code, codePos);

	// and w2, w2, CacheLineAlignMask
	codePos = (((uint8_t*)randomx_program_aarch64_light_cacheline_align_mask) - ((uint8_t*)randomx_program_aarch64));
	emit32(0x121A0000 | 2 | (2 << 5) | ((Log2(RANDOMX_DATASET_BASE_SIZE) - 7) << 10), code, codePos);

	emitSpMix2(config, codePos);

	// Apply dataset offset
	codePos = ((uint8_t*)randomx_program_aarch64_light_dataset_offset) - ((uint8_t*)randomx_program_aarch64);

	datasetOffset /= CacheLineSize;
	const uint32_t imm_lo = datasetOffset & ((1 << 12) - 1);
	const uint32_t imm_hi = datasetOffset >> 12;

	emit32(ARMV8A::ADD_IMM_LO | 2 | (2 << 5) | (imm_lo << 10), code, codePos);
	emit32(ARMV8A::ADD_IMM_HI | 2 | (2 << 5) | (imm_hi << 10), code, codePos);

#ifdef __GNUC__
	__builtin___clear_cache(reinterpret_cast<char*>(code + MainLoopBegin), reinterpret_cast<char*>(code + codePos));
#endif
}

void JitCompilerA64::dumpJitCode() const {
	// Build opcode-to-name mapping from frequency weights.
	// engine[256] maps raw opcode bytes to handlers; the weight table
	// determines which slot range each handler occupies.
	static constexpr const char* kHandlerNames[] = {
		"IADD_RS",   "IADD_M",   "ISUB_R",   "ISUB_M",
		"IMUL_R",    "IMUL_M",   "IMULH_R",  "IMULH_M",
		"ISMULH_R",  "ISMULH_M", "IMUL_RCP", "INEG_R",
		"IXOR_R",    "IXOR_M",   "IROR_R",   "IROL_R",
		"ISWAP_R",   "FSWAP_R",  "FADD_R",   "FADD_M",
		"FSUB_R",    "FSUB_M",   "FSCAL_R",  "FMUL_R",
		"FDIV_M",    "FSQRT_R",  "CBRANCH",  "CFROUND",
		"ISTORE",    "NOP",
	};
	static constexpr uint32_t kWeights[] = {
		WT(IADD_RS),  WT(IADD_M),   WT(ISUB_R),   WT(ISUB_M),
		WT(IMUL_R),   WT(IMUL_M),   WT(IMULH_R),  WT(IMULH_M),
		WT(ISMULH_R), WT(ISMULH_M), WT(IMUL_RCP), WT(INEG_R),
		WT(IXOR_R),   WT(IXOR_M),   WT(IROR_R),   WT(IROL_R),
		WT(ISWAP_R),  WT(FSWAP_R),  WT(FADD_R),   WT(FADD_M),
		WT(FSUB_R),   WT(FSUB_M),   WT(FSCAL_R),  WT(FMUL_R),
		WT(FDIV_M),   WT(FSQRT_R),  WT(CBRANCH),  WT(CFROUND),
		WT(ISTORE),   WT(NOP),
	};
	static constexpr auto kNumHandlers = sizeof(kHandlerNames) / sizeof(kHandlerNames[0]);

	// Precompute: for each raw opcode byte (0..255), which handler name?
	static const char* sOpcodeName[256] = {};
	if (!sOpcodeName[0]) {
		uint32_t slot = 0;
		for (uint32_t h = 0; h < kNumHandlers; ++h) {
			for (uint32_t w = 0; w < kWeights[h]; ++w) {
				if (slot < 256) sOpcodeName[slot++] = kHandlerNames[h];
			}
		}
		// Fill remaining slots (should only be NOP weight 0, so all 256 filled)
		while (slot < 256) sOpcodeName[slot++] = "NOP";
	}

	std::cout << "--- JIT code dump ---\n";
	std::cout << "Total code size: " << jit_dump_.back().offset + jit_dump_.back().size << " bytes\n";
	// PLAN.md Phase 6 item 14 (2026-07-24): the actual mmap'd/RWX-protected buffer
	// is CodeSize + CalcDatasetItemSize bytes -- bigger than the main table's own
	// "Total code size" above, which only covers the fixed template + per-hash
	// program region. Printed here as ground truth for matching this buffer
	// against a live process's /proc/<pid>/maps (sizes there can otherwise be
	// ambiguous if THP merges adjacent same-permission allocations).
	std::cout << "Total allocated buffer size (CodeSize + CalcDatasetItemSize): "
	          << (CodeSize + CalcDatasetItemSize) << " bytes\n";
	std::cout << "CodeSize (fixed template + per-hash program region, offsets below this "
	          << "are NOT superscalar): " << CodeSize << " bytes\n";

	// Print raw hex, 16 bytes per line
	const uint32_t total_bytes = jit_dump_.back().offset + jit_dump_.back().size;
	std::cout << "\n--- Raw bytes ---\n";
	for (uint32_t i = 0; i < total_bytes; i += 16) {
		std::cout << std::hex << std::setw(6) << std::setfill('0') << i << ": ";
		for (uint32_t j = i; j < i + 16 && j < total_bytes; ++j) {
			std::cout << std::hex << std::setw(2) << std::setfill('0')
			          << static_cast<int>(code[j]) << ' ';
		}
		std::cout << std::dec << '\n';
	}

	// Print boundary table
	std::cout << "\n--- Opcode boundary table ---\n";
	std::cout << "  #  | opcode_id | name        | offset  | size\n";
	std::cout << "-----|-----------|-------------|---------|------\n";
	for (size_t i = 0; i < jit_dump_.size(); ++i) {
		const auto& e = jit_dump_[i];
		const char* name = sOpcodeName[e.opcode];
		std::cout << std::dec << std::setw(4) << i << " | "
		          << std::setw(9) << e.opcode << " | "
		          << std::setw(11) << name << " | "
		          << std::hex << std::setw(6) << std::setfill('0') << e.offset << " | "
		          << std::dec << std::setw(4) << std::setfill(' ') << e.size << '\n';
	}

	// PLAN.md Phase 6 item 13 (2026-07-24): region-scoped instrumentation for the
	// superscalar/dataset-derivation path. Unlike the main table above (a fixed,
	// small, once-per-hash program), this region is what `bl rx_calc_dataset_item`
	// calls once per main-loop iteration in light mode -- 2048 iterations x 8
	// programs = 16,384 calls/hash -- so it dominates real per-hash instruction
	// volume even though it's compiled only once per seed rotation, not per hash.
	if (!superscalar_jit_dump_.empty()) {
		static constexpr const char* kSuperscalarNames[] = {
			"ISUB_R", "IXOR_R", "IADD_RS", "IMUL_R", "IROR_C",
			"IADD_C7", "IXOR_C7", "IADD_C8", "IXOR_C8", "IADD_C9", "IXOR_C9",
			"IMULH_R", "ISMULH_R", "IMUL_RCP",
		};
		constexpr uint32_t kNumSuperscalarNames =
			sizeof(kSuperscalarNames) / sizeof(kSuperscalarNames[0]);

		struct Agg { uint64_t count = 0; uint64_t bytes = 0; };
		std::array<Agg, kNumSuperscalarNames> agg{};
		uint64_t ss_total_instr = 0, ss_total_bytes = 0;
		for (const auto& e : superscalar_jit_dump_) {
			++ss_total_instr;
			ss_total_bytes += e.size;
			if (e.opcode < kNumSuperscalarNames) {
				agg[e.opcode].count += 1;
				agg[e.opcode].bytes += e.size;
			}
		}

		std::cout << "\n--- Superscalar/dataset-derivation dump "
		          << "(one generateSuperscalarHash() call = one rx_calc_dataset_item "
		          << "compile; in light mode this executes once per main-loop "
		          << "iteration, 16,384x/hash) ---\n";
		std::cout << "Total instructions: " << ss_total_instr
		          << "  Total bytes: " << ss_total_bytes << "\n\n";
		std::cout << "  opcode     | count | bytes | avg_size | % of bytes\n";
		std::cout << "-------------|-------|-------|----------|------------\n";
		for (uint32_t i = 0; i < kNumSuperscalarNames; ++i) {
			if (agg[i].count == 0) continue;
			const double avg = static_cast<double>(agg[i].bytes) / static_cast<double>(agg[i].count);
			const double pct = ss_total_bytes > 0
				? (static_cast<double>(agg[i].bytes) / static_cast<double>(ss_total_bytes) * 100.0)
				: 0.0;
			std::cout << std::left << std::setw(12) << kSuperscalarNames[i] << std::right
			          << " | " << std::setw(5) << agg[i].count
			          << " | " << std::setw(5) << agg[i].bytes
			          << " | " << std::fixed << std::setprecision(2) << std::setw(8) << avg
			          << " | " << std::setw(9) << pct << "%\n";
		}

		// PLAN.md Phase 6 item 14 (2026-07-24): per-entry boundary table for this
		// region, matching the main program's table above -- needed to correlate
		// `perf record` sample addresses (offset from this buffer's runtime base)
		// against individual opcodes, not just aggregate opcode totals. This
		// buffer is compiled once per seed rotation and reused unmodified across
		// every main-loop iteration for that seed's whole duration, so a single
		// snapshot of this table validly describes every sample taken while the
		// seed doesn't change -- unlike the fixed per-hash program above, which
		// is regenerated every hash and can't be correlated the same way.
		std::cout << "\n--- Superscalar opcode boundary table ---\n";
		std::cout << "  #  | opcode_id | name        | offset  | size\n";
		std::cout << "-----|-----------|-------------|---------|------\n";
		for (size_t i = 0; i < superscalar_jit_dump_.size(); ++i) {
			const auto& e = superscalar_jit_dump_[i];
			const char* name = e.opcode < kNumSuperscalarNames ? kSuperscalarNames[e.opcode] : "?";
			std::cout << std::dec << std::setw(4) << i << " | "
			          << std::setw(9) << e.opcode << " | "
			          << std::setw(11) << name << " | "
			          << std::hex << std::setw(6) << std::setfill('0') << e.offset << " | "
			          << std::dec << std::setw(4) << std::setfill(' ') << e.size << '\n';
		}
	}
	std::cout << std::flush;
}

void JitCompilerA64::generateSuperscalarHash(const SuperscalarProgramList& programs, const std::vector<uint64_t>& reciprocalCache)
{
	uint32_t codePos = CodeSize;

	uint8_t* p1 = (uint8_t*)randomx_calc_dataset_item_aarch64;
	uint8_t* p2 = (uint8_t*)randomx_calc_dataset_item_aarch64_prefetch;
	memcpy(code + codePos, p1, p2 - p1);
	codePos += p2 - p1;

	// Pinned at the cap (not 0, like emitPrologueMix's reset) so
	// emitMovImmediate's `num32bitLiterals < 64` branch (the shared NEON
	// literal-pool path) is never taken here -- IXOR_C7..9/IADD_C7..9's
	// large-immediate loads always fall through to the self-contained
	// MOVZ/MOVK path instead, which is what makes scheduleSuperscalarProgram()
	// safe under reordering without needing to reason about pool-slot
	// ordering for this opcode class (flagged by independent review,
	// 2026-07-25, docs/audits/scheduler-review-2026-07-25.md #4). If this
	// ever regresses to 0, superscalar immediate loads would silently
	// start using the shared pool and become order-sensitive under
	// scheduling, the same class of bug as the main-path IMUL_RCP hazard.
	num32bitLiterals = 64;
	constexpr uint32_t tmp_reg = 12;

	for (size_t i = 0; i < programs.size(); ++i)
	{
		// and x11, x10, CacheSize / CacheLineSize - 1
		emit32(0x92400000 | 11 | (10 << 5) | ((Log2(CacheSize / CacheLineSize) - 1) << 10), code, codePos);

		p1 = ((uint8_t*)randomx_calc_dataset_item_aarch64_prefetch) + 4;
		p2 = (uint8_t*)randomx_calc_dataset_item_aarch64_mix;
		memcpy(code + codePos, p1, p2 - p1);
		codePos += p2 - p1;

		const SuperscalarProgram& prog = programs[i];
		const size_t progSize = prog.size();

		uint32_t jmp_pos = codePos;
		codePos += 4;

		// Fill in literal pool
		for (size_t j = 0; j < progSize; ++j)
		{
			const Instruction& instr = prog(j);
			if (static_cast<SuperscalarInstructionType>(instr.opcode) == SuperscalarInstructionType::IMUL_RCP)
				emit64(reciprocalCache[instr.getImm32()], code, codePos);
		}

		// Jump over literal pool
		uint32_t literal_pos = jmp_pos;
		emit32(ARMV8A::B | ((codePos - jmp_pos) / 4), code, literal_pos);

		const std::vector<uint32_t> emit_order = scheduleSuperscalarProgram(prog);
		for (size_t idx = 0; idx < emit_order.size(); ++idx)
		{
			const size_t j = emit_order[idx];
			const Instruction& instr = prog(j);
			const uint32_t src = instr.src;
			const uint32_t dst = instr.dst;
			const uint32_t pos_before_superscalar = codePos;

			switch (static_cast<SuperscalarInstructionType>(instr.opcode))
			{
			case SuperscalarInstructionType::ISUB_R:
				emit32(ARMV8A::SUB | dst | (dst << 5) | (src << 16), code, codePos);
				break;
			case SuperscalarInstructionType::IXOR_R:
				emit32(ARMV8A::EOR | dst | (dst << 5) | (src << 16), code, codePos);
				break;
			case SuperscalarInstructionType::IADD_RS:
				emit32(ARMV8A::ADD | dst | (dst << 5) | (instr.getModShift() << 10) | (src << 16), code, codePos);
				break;
			case SuperscalarInstructionType::IMUL_R:
				emit32(ARMV8A::MUL | dst | (dst << 5) | (src << 16), code, codePos);
				break;
			case SuperscalarInstructionType::IROR_C:
				emit32(ARMV8A::ROR_IMM | dst | (dst << 5) | ((instr.getImm32() & 63) << 10) | (dst << 16), code, codePos);
				break;
			case SuperscalarInstructionType::IADD_C7:
			case SuperscalarInstructionType::IADD_C8:
			case SuperscalarInstructionType::IADD_C9:
				emitAddImmediate(dst, dst, instr.getImm32(), code, codePos);
				break;
			case SuperscalarInstructionType::IXOR_C7:
			case SuperscalarInstructionType::IXOR_C8:
			case SuperscalarInstructionType::IXOR_C9:
				emitMovImmediate(tmp_reg, instr.getImm32(), code, codePos);
				emit32(ARMV8A::EOR | dst | (dst << 5) | (tmp_reg << 16), code, codePos);
				break;
			case SuperscalarInstructionType::IMULH_R:
				emit32(ARMV8A::UMULH | dst | (dst << 5) | (src << 16), code, codePos);
				break;
			case SuperscalarInstructionType::ISMULH_R:
				emit32(ARMV8A::SMULH | dst | (dst << 5) | (src << 16), code, codePos);
				break;
			case SuperscalarInstructionType::IMUL_RCP:
				{
					int32_t offset = (literal_pos - codePos) / 4;
					offset &= (1 << 19) - 1;
					literal_pos += 8;

					// ldr tmp_reg, reciprocal
					emit32(ARMV8A::LDR_LITERAL | tmp_reg | (offset << 5), code, codePos);

					// mul dst, dst, tmp_reg
					emit32(ARMV8A::MUL | dst | (dst << 5) | (tmp_reg << 16), code, codePos);
				}
				break;
			default:
				break;
			}
			if (jit_dump_enabled_) {
				superscalar_jit_dump_.push_back({static_cast<uint32_t>(instr.opcode), pos_before_superscalar, codePos - pos_before_superscalar});
			}
		}

		p1 = (uint8_t*)randomx_calc_dataset_item_aarch64_mix;
		p2 = (uint8_t*)randomx_calc_dataset_item_aarch64_store_result;
		memcpy(code + codePos, p1, p2 - p1);
		codePos += p2 - p1;

		// Update registerValue
		emit32(ARMV8A::MOV_REG | 10 | (prog.address_register() << 16), code, codePos);
	}

	p1 = (uint8_t*)randomx_calc_dataset_item_aarch64_store_result;
	p2 = (uint8_t*)randomx_calc_dataset_item_aarch64_end;
	memcpy(code + codePos, p1, p2 - p1);
	codePos += p2 - p1;

#ifdef __GNUC__
	__builtin___clear_cache(reinterpret_cast<char*>(code + CodeSize), reinterpret_cast<char*>(code + codePos));
#endif
}

DatasetInitFunc* JitCompilerA64::getDatasetInitFunc()
{
	return (DatasetInitFunc*)(code + (((uint8_t*)randomx_init_dataset_aarch64) - ((uint8_t*)randomx_program_aarch64)));
}

size_t JitCompilerA64::getCodeSize() const
{
	return CodeSize;
}

void JitCompilerA64::emitMovImmediate(uint32_t dst, uint32_t imm, uint8_t* code, uint32_t& codePos)
{
	uint32_t k = codePos;

	if (imm < (1 << 16))
	{
		// movz tmp_reg, imm32 (16 low bits)
		emit32(ARMV8A::MOVZ | dst | (imm << 5), code, k);
	}
	else
	{
		if (num32bitLiterals < 64)
		{
			if (static_cast<int32_t>(imm) < 0)
			{
				// smov dst, vN.s[M]
				emit32(0x4E042C00 | dst | ((num32bitLiterals / 4) << 5) | ((num32bitLiterals % 4) << 19), code, k);
			}
			else
			{
				// umov dst, vN.s[M]
				emit32(0x0E043C00 | dst | ((num32bitLiterals / 4) << 5) | ((num32bitLiterals % 4) << 19), code, k);
			}

			((uint32_t*)(code + ImulRcpLiteralsEnd))[num32bitLiterals] = imm;
			++num32bitLiterals;
		}
		else
		{
			if (static_cast<int32_t>(imm) < 0)
			{
				// movn tmp_reg, ~imm32 (16 high bits)
				emit32(ARMV8A::MOVN | dst | (1 << 21) | ((~imm >> 16) << 5), code, k);
			}
			else
			{
				// movz tmp_reg, imm32 (16 high bits)
				emit32(ARMV8A::MOVZ | dst | (1 << 21) | ((imm >> 16) << 5), code, k);
			}

			// movk tmp_reg, imm32 (16 low bits)
			emit32(ARMV8A::MOVK | dst | ((imm & 0xFFFF) << 5), code, k);
		}
	}

	codePos = k;
}

void JitCompilerA64::emitAddImmediate(uint32_t dst, uint32_t src, uint32_t imm, uint8_t* code, uint32_t& codePos)
{
	uint32_t k = codePos;

	if (imm < (1 << 24))
	{
		const uint32_t imm_lo = imm & ((1 << 12) - 1);
		const uint32_t imm_hi = imm >> 12;

		if (imm_lo && imm_hi)
		{
			emit32(ARMV8A::ADD_IMM_LO | dst | (src << 5) | (imm_lo << 10), code, k);
			emit32(ARMV8A::ADD_IMM_HI | dst | (dst << 5) | (imm_hi << 10), code, k);
		}
		else if (imm_lo)
		{
			emit32(ARMV8A::ADD_IMM_LO | dst | (src << 5) | (imm_lo << 10), code, k);
		}
		else
		{
			emit32(ARMV8A::ADD_IMM_HI | dst | (src << 5) | (imm_hi << 10), code, k);
		}
	}
	else
	{
		constexpr uint32_t tmp_reg = 20;
		emitMovImmediate(tmp_reg, imm, code, k);

		// add dst, src, tmp_reg
		emit32(ARMV8A::ADD | dst | (src << 5) | (tmp_reg << 16), code, k);
	}

	codePos = k;
}

template<uint32_t tmp_reg>
void JitCompilerA64::emitMemLoad(uint32_t dst, uint32_t src, Instruction& instr, uint8_t* code, uint32_t& codePos)
{
	uint32_t k = codePos;

	uint32_t imm = instr.getImm32();

	if (src != dst)
	{
		imm &= instr.getModMem() ? (RANDOMX_SCRATCHPAD_L1 - 1) : (RANDOMX_SCRATCHPAD_L2 - 1);
		emitAddImmediate(tmp_reg, src, imm, code, k);

		constexpr uint32_t t = 0x927d0000 | tmp_reg | (tmp_reg << 5);
		constexpr uint32_t andInstrL1 = t | ((Log2(RANDOMX_SCRATCHPAD_L1) - 4) << 10);
		constexpr uint32_t andInstrL2 = t | ((Log2(RANDOMX_SCRATCHPAD_L2) - 4) << 10);

		emit32(instr.getModMem() ? andInstrL1 : andInstrL2, code, k);

		// ldr tmp_reg, [x2, tmp_reg]
		emit32(0xf8606840 | tmp_reg | (tmp_reg << 16), code, k);
	}
	else
	{
		imm = (imm & ScratchpadL3Mask) >> 3;
		emitMovImmediate(tmp_reg, imm, code, k);

		// ldr tmp_reg, [x2, tmp_reg, lsl 3]
		emit32(0xf8607840 | tmp_reg | (tmp_reg << 16), code, k);
	}

	codePos = k;
}

template<uint32_t tmp_reg_fp>
void JitCompilerA64::emitMemLoadFP(uint32_t src, Instruction& instr, uint8_t* code, uint32_t& codePos)
{
	uint32_t k = codePos;

	uint32_t imm = instr.getImm32();
	constexpr uint32_t tmp_reg = 19;

	imm &= instr.getModMem() ? (RANDOMX_SCRATCHPAD_L1 - 1) : (RANDOMX_SCRATCHPAD_L2 - 1);
	emitAddImmediate(tmp_reg, src, imm, code, k);

	constexpr uint32_t t = 0x927d0000 | tmp_reg | (tmp_reg << 5);
	constexpr uint32_t andInstrL1 = t | ((Log2(RANDOMX_SCRATCHPAD_L1) - 4) << 10);
	constexpr uint32_t andInstrL2 = t | ((Log2(RANDOMX_SCRATCHPAD_L2) - 4) << 10);

	emit32(instr.getModMem() ? andInstrL1 : andInstrL2, code, k);

	// ldr d<tmp_reg_fp>, [x2, tmp_reg]
	emit32(0xfc606800 | (tmp_reg << 16) | (2 << 5) | tmp_reg_fp, code, k);

	// sxtl tmp_reg_fp.2d, tmp_reg_fp.2s
	emit32(0x0F20A400 | (tmp_reg_fp << 5) | tmp_reg_fp, code, k);

	// scvtf tmp_reg_fp.2d, tmp_reg_fp.2d
	emit32(0x4E61D800 | tmp_reg_fp | (tmp_reg_fp << 5), code, k);

	codePos = k;
}

void JitCompilerA64::h_IADD_RS(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];
	const uint32_t shift = instr.getModShift();

	// add dst, src << shift
	emit32(ARMV8A::ADD | dst | (dst << 5) | (shift << 10) | (src << 16), code, k);

	if (instr.dst == RegisterNeedsDisplacement)
		emitAddImmediate(dst, dst, instr.getImm32(), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_IADD_M(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	constexpr uint32_t tmp_reg = 20;
	emitMemLoad<tmp_reg>(dst, src, instr, code, k);

	// add dst, dst, tmp_reg
	emit32(ARMV8A::ADD | dst | (dst << 5) | (tmp_reg << 16), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_ISUB_R(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	if (src != dst)
	{
		// sub dst, dst, src
		emit32(ARMV8A::SUB | dst | (dst << 5) | (src << 16), code, k);
	}
	else
	{
		const uint32_t imm = instr.getImm32();

		if (imm == 0x80000000ul) {
			constexpr uint32_t tmp_reg = 20;
			emit32(ARMV8A::MOVZ | tmp_reg | (1u << 21) | (0x8000u << 5), code, k);
			emit32(ARMV8A::ADD | dst | (dst << 5) | (tmp_reg << 16), code, k);
		}
		else {
			emitAddImmediate(dst, dst, -instr.getImm32(), code, k);
		}
	}

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_ISUB_M(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	constexpr uint32_t tmp_reg = 20;
	emitMemLoad<tmp_reg>(dst, src, instr, code, k);

	// sub dst, dst, tmp_reg
	emit32(ARMV8A::SUB | dst | (dst << 5) | (tmp_reg << 16), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_IMUL_R(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	if (src == dst)
	{
		src = 20;
		emitMovImmediate(src, instr.getImm32(), code, k);
	}

	// mul dst, dst, src
	emit32(ARMV8A::MUL | dst | (dst << 5) | (src << 16), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_IMUL_M(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	constexpr uint32_t tmp_reg = 20;
	emitMemLoad<tmp_reg>(dst, src, instr, code, k);

	// mul dst, dst, tmp_reg
	emit32(ARMV8A::MUL | dst | (dst << 5) | (tmp_reg << 16), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_IMULH_R(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	// umulh dst, dst, src
	emit32(ARMV8A::UMULH | dst | (dst << 5) | (src << 16), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_IMULH_M(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	constexpr uint32_t tmp_reg = 20;
	emitMemLoad<tmp_reg>(dst, src, instr, code, k);

	// umulh dst, dst, tmp_reg
	emit32(ARMV8A::UMULH | dst | (dst << 5) | (tmp_reg << 16), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_ISMULH_R(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	// smulh dst, dst, src
	emit32(ARMV8A::SMULH | dst | (dst << 5) | (src << 16), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_ISMULH_M(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	constexpr uint32_t tmp_reg = 20;
	emitMemLoad<tmp_reg>(dst, src, instr, code, k);

	// smulh dst, dst, tmp_reg
	emit32(ARMV8A::SMULH | dst | (dst << 5) | (tmp_reg << 16), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_IMUL_RCP(Instruction& instr, uint32_t& codePos)
{
	const uint32_t divisor = instr.getImm32();
	// isZeroOrPowerOf2 check: skip trivial divisors (power-of-two)
	if ((divisor & (divisor - 1)) == 0)
		return;

	uint32_t k = codePos;

	constexpr uint32_t tmp_reg = 20;
	const uint32_t dst = IntRegMap[instr.dst];

	const uint32_t literal_id = (ImulRcpLiteralsEnd - literalPos) / sizeof(uint64_t);
	literalPos -= sizeof(uint64_t);

	const uint64_t reciprocal = randomx_reciprocal(divisor);
	memcpy(code + literalPos, &reciprocal, sizeof(reciprocal));

	if (literal_id < 12)
	{
		static constexpr uint32_t literal_regs[12] = { 30 << 16, 29 << 16, 28 << 16, 27 << 16, 26 << 16, 25 << 16, 24 << 16, 23 << 16, 22 << 16, 21 << 16, 11 << 16, 0 };

		// mul dst, dst, literal_reg
		emit32(ARMV8A::MUL | dst | (dst << 5) | literal_regs[literal_id], code, k);
	}
	else
	{
		// ldr tmp_reg, reciprocal
		// Mask to the 19-bit imm19 field like the superscalar path does —
		// defensive parity: keeps a (hypothetical) negative offset from
		// spilling into opcode bits [31:24] via the <<5 shift.
		const uint32_t offset = ((literalPos - k) / 4) & ((1 << 19) - 1);
		emit32(ARMV8A::LDR_LITERAL | tmp_reg | (offset << 5), code, k);

		// mul dst, dst, tmp_reg
		emit32(ARMV8A::MUL | dst | (dst << 5) | (tmp_reg << 16), code, k);
	}

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_INEG_R(Instruction& instr, uint32_t& codePos)
{
	const uint32_t dst = IntRegMap[instr.dst];

	// sub dst, xzr, dst
	emit32(ARMV8A::SUB | dst | (31 << 5) | (dst << 16), code, codePos);

	reg_changed_offset[instr.dst] = codePos;
}

void JitCompilerA64::h_IXOR_R(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	if (src == dst)
	{
		src = 20;
		emitMovImmediate(src, instr.getImm32(), code, k);
	}

	// eor dst, dst, src
	emit32(ARMV8A::EOR | dst | (dst << 5) | (src << 16), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_IXOR_M(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	constexpr uint32_t tmp_reg = 20;
	emitMemLoad<tmp_reg>(dst, src, instr, code, k);

	// eor dst, dst, tmp_reg
	emit32(ARMV8A::EOR | dst | (dst << 5) | (tmp_reg << 16), code, k);

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_IROR_R(Instruction& instr, uint32_t& codePos)
{
	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	if (src != dst)
	{
		// ror dst, dst, src
		emit32(ARMV8A::ROR | dst | (dst << 5) | (src << 16), code, codePos);
	}
	else
	{
		// ror dst, dst, imm
		emit32(ARMV8A::ROR_IMM | dst | (dst << 5) | ((instr.getImm32() & 63) << 10) | (dst << 16), code, codePos);
	}

	reg_changed_offset[instr.dst] = codePos;
}

void JitCompilerA64::h_IROL_R(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	if (src != dst)
	{
		constexpr uint32_t tmp_reg = 20;

		// sub tmp_reg, xzr, src
		emit32(ARMV8A::SUB | tmp_reg | (31 << 5) | (src << 16), code, k);

		// ror dst, dst, tmp_reg
		emit32(ARMV8A::ROR | dst | (dst << 5) | (tmp_reg << 16), code, k);
	}
	else
	{
		// ror dst, dst, imm
		emit32(ARMV8A::ROR_IMM | dst | (dst << 5) | ((-instr.getImm32() & 63) << 10) | (dst << 16), code, k);
	}

	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_ISWAP_R(Instruction& instr, uint32_t& codePos)
{
	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];

	if (src == dst)
		return;

	uint32_t k = codePos;

	constexpr uint32_t tmp_reg = 20;
	emit32(ARMV8A::MOV_REG | tmp_reg | (dst << 16), code, k);
	emit32(ARMV8A::MOV_REG | dst | (src << 16), code, k);
	emit32(ARMV8A::MOV_REG | src | (tmp_reg << 16), code, k);

	reg_changed_offset[instr.src] = k;
	reg_changed_offset[instr.dst] = k;
	codePos = k;
}

void JitCompilerA64::h_FSWAP_R(Instruction& instr, uint32_t& codePos)
{
	const uint32_t dst = instr.dst + 16;
	emit32(0x6E004000 | dst | (dst << 5) | (dst << 16), code, codePos);
}

void JitCompilerA64::h_FADD_R(Instruction& instr, uint32_t& codePos)
{
	const uint32_t src = (instr.src % 4) + 24;
	const uint32_t dst = (instr.dst % 4) + 16;

	emit32(ARMV8A::FADD | dst | (dst << 5) | (src << 16), code, codePos);
}

void JitCompilerA64::h_FADD_M(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = (instr.dst % 4) + 16;

	constexpr uint32_t tmp_reg_fp = 28;
	emitMemLoadFP<tmp_reg_fp>(src, instr, code, k);

	emit32(ARMV8A::FADD | dst | (dst << 5) | (tmp_reg_fp << 16), code, k);

	codePos = k;
}

void JitCompilerA64::h_FSUB_R(Instruction& instr, uint32_t& codePos)
{
	const uint32_t src = (instr.src % 4) + 24;
	const uint32_t dst = (instr.dst % 4) + 16;

	emit32(ARMV8A::FSUB | dst | (dst << 5) | (src << 16), code, codePos);
}

void JitCompilerA64::h_FSUB_M(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = (instr.dst % 4) + 16;

	constexpr uint32_t tmp_reg_fp = 28;
	emitMemLoadFP<tmp_reg_fp>(src, instr, code, k);

	emit32(ARMV8A::FSUB | dst | (dst << 5) | (tmp_reg_fp << 16), code, k);

	codePos = k;
}

void JitCompilerA64::h_FSCAL_R(Instruction& instr, uint32_t& codePos)
{
	const uint32_t dst = (instr.dst % 4) + 16;

	emit32(ARMV8A::FEOR | dst | (dst << 5) | (31 << 16), code, codePos);
}

void JitCompilerA64::h_FMUL_R(Instruction& instr, uint32_t& codePos)
{
	const uint32_t src = (instr.src % 4) + 24;
	const uint32_t dst = (instr.dst % 4) + 20;

	emit32(ARMV8A::FMUL | dst | (dst << 5) | (src << 16), code, codePos);
}

void JitCompilerA64::h_FDIV_M(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = (instr.dst % 4) + 20;

	constexpr uint32_t tmp_reg_fp = 28;
	emitMemLoadFP<tmp_reg_fp>(src, instr, code, k);

	// bif tmp_reg_fp, or_mask_reg, and_mask_reg
	emit32(0x6EE01C00 | tmp_reg_fp | (30 << 5) | (29 << 16), code, k);

#ifdef ARMRX_JIT_FAST_DIV_SQRT
	// Newton-Raphson SIMD 2D double precision division: v_dst / v28
	// Scratch registers: v0-v2 (safe — not used by other instruction handlers)

	// frecpe v0.2d, v28.2d  — reciprocal estimate of divisor
	emit32(0x4EE1D800 | 0 | (tmp_reg_fp << 5), code, k);

	// Iteration 1: frecps + fmul
	emit32(0x4E60FC00 | 1 | (tmp_reg_fp << 5) | (0 << 16), code, k); // frecps v1.2d, v28.2d, v0.2d
	emit32(ARMV8A::FMUL | 0 | (0 << 5) | (1 << 16), code, k);        // fmul v0.2d, v0.2d, v1.2d

	// Iteration 2: frecps + fmul
	emit32(0x4E60FC00 | 1 | (tmp_reg_fp << 5) | (0 << 16), code, k);
	emit32(ARMV8A::FMUL | 0 | (0 << 5) | (1 << 16), code, k);

	// Iteration 3: frecps + fmul
	emit32(0x4E60FC00 | 1 | (tmp_reg_fp << 5) | (0 << 16), code, k);
	emit32(ARMV8A::FMUL | 0 | (0 << 5) | (1 << 16), code, k);

	// Markstein correction for correctly-rounded division:
	// q = a * y
	emit32(ARMV8A::FMUL | 1 | (dst << 5) | (0 << 16), code, k); // fmul v1.2d, v_dst.2d, v0.2d

	// r = a - b * q  (v2 = v_dst; fmls v2.2d, v28.2d, v1.2d)
	emit32(0x4EA01C00 | 2 | (dst << 5) | (dst << 16), code, k);       // mov v2.16b, v_dst.16b
	emit32(0x4EE0CC00 | 2 | (tmp_reg_fp << 5) | (1 << 16), code, k);  // fmls v2.2d, v28.2d, v1.2d

	// q' = q + r * y  (fmla v1.2d, v2.2d, v0.2d)
	emit32(0x4E60CC00 | 1 | (2 << 5) | (0 << 16), code, k);  // fmla v1.2d, v2.2d, v0.2d

	// mov v_dst.16b, v1.16b
	emit32(0x4EA01C00 | dst | (1 << 5) | (1 << 16), code, k);
#else
	emit32(ARMV8A::FDIV | dst | (dst << 5) | (tmp_reg_fp << 16), code, k);
#endif

	codePos = k;
}

void JitCompilerA64::h_FSQRT_R(Instruction& instr, uint32_t& codePos)
{
	const uint32_t dst = (instr.dst % 4) + 20;

#ifdef ARMRX_JIT_FAST_DIV_SQRT
	uint32_t k = codePos;
	// Newton-Raphson SIMD 2D double precision square root: sqrt(v_dst)
	// Scratch registers: v0, v1, v2, v3

	// frsqrte v0.2d, v_dst.2d  — reciprocal sqrt estimate
	emit32(0x6EE1D800 | 0 | (dst << 5), code, k);

	// Iteration 1: y_sq = y*y; step = frsqrts(a, y_sq); y = y*step
	emit32(ARMV8A::FMUL | 1 | (0 << 5) | (0 << 16), code, k);        // fmul v1.2d, v0.2d, v0.2d
	emit32(0x4EE0FC00 | 1 | (dst << 5) | (1 << 16), code, k);         // frsqrts v1.2d, v_dst.2d, v1.2d
	emit32(ARMV8A::FMUL | 0 | (0 << 5) | (1 << 16), code, k);        // fmul v0.2d, v0.2d, v1.2d

	// Iteration 2
	emit32(ARMV8A::FMUL | 1 | (0 << 5) | (0 << 16), code, k);
	emit32(0x4EE0FC00 | 1 | (dst << 5) | (1 << 16), code, k);
	emit32(ARMV8A::FMUL | 0 | (0 << 5) | (1 << 16), code, k);

	// Iteration 3
	emit32(ARMV8A::FMUL | 1 | (0 << 5) | (0 << 16), code, k);
	emit32(0x4EE0FC00 | 1 | (dst << 5) | (1 << 16), code, k);
	emit32(ARMV8A::FMUL | 0 | (0 << 5) | (1 << 16), code, k);

	// Markstein correction for correctly-rounded sqrt:
	// g = a * y  (initial sqrt estimate)
	emit32(ARMV8A::FMUL | 1 | (dst << 5) | (0 << 16), code, k);  // fmul v1.2d, v_dst.2d, v0.2d

	// r = a - g^2  (v2 = v_dst; fmls v2.2d, v1.2d, v1.2d)
	emit32(0x4EA01C00 | 2 | (dst << 5) | (dst << 16), code, k);   // mov v2.16b, v_dst.16b
	emit32(0x4EE0CC00 | 2 | (1 << 5) | (1 << 16), code, k);       // fmls v2.2d, v1.2d, v1.2d

	// h = 0.5 * y
	emit32(0x6F03F403, code, k);                                   // fmov v3.2d, #0.5
	emit32(ARMV8A::FMUL | 3 | (3 << 5) | (0 << 16), code, k);    // fmul v3.2d, v3.2d, v0.2d

	// g' = g + r * h  (fmla v1.2d, v2.2d, v3.2d)
	emit32(0x4E60CC00 | 1 | (2 << 5) | (3 << 16), code, k);       // fmla v1.2d, v2.2d, v3.2d

	// Handle zero input: if a[i] == 0.0, result should be 0.0 not NaN
	// fcmeq v2.2d, v_dst.2d, #0.0  — lane mask: all-ones where a==0
	emit32(0x4EE0D800 | 2 | (dst << 5), code, k);
	// bic v1.16b, v1.16b, v2.16b  — zero out lanes where input was 0
	emit32(0x4E601C00 | 1 | (1 << 5) | (2 << 16), code, k);

	// mov v_dst.16b, v1.16b
	emit32(0x4EA01C00 | dst | (1 << 5) | (1 << 16), code, k);

	codePos = k;
#else
	emit32(ARMV8A::FSQRT | dst | (dst << 5), code, codePos);
#endif
}

void JitCompilerA64::h_CBRANCH(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t dst = IntRegMap[instr.dst];
	const uint32_t modCond = instr.getModCond();
	const uint32_t shift = modCond + ConditionOffset;
	const uint32_t imm = (instr.getImm32() | (1U << shift)) & ~(1U << (shift - 1));

	emitAddImmediate(dst, dst, imm, code, k);

	// tst dst, mask
	static_assert((ConditionMask == 0xFF) && (ConditionOffset == 8), "Update tst encoding for different mask and offset");
	emit32((0xF2781C1F - (modCond << 16)) | (dst << 5), code, k);

	int32_t offset = reg_changed_offset[instr.dst];

	// Branchless CBRANCH: instead of a single backward `beq target` (AArch64
	// predicts backward cond branches TAKEN, but CBRANCH is only taken ~0.4% of
	// the time → 99.6% misprediction rate), emit:
	//   bne .Lskip          -- forward, predicted NOT-taken (correct 99.6%)
	//   b target            -- unconditional backward (always taken)
	//
	// imm19=2 because B.cond PC-relative offset = PC + imm19*4.
	// To skip the 4-byte `b` instruction ahead, the target is PC+8 → imm19=2.
	emit32(0x54000000 | (2 << 5) | 1, code, k); // bne +8 (skip next instr)

	// Unconditional backward branch to target (26-bit signed offset in words).
	// Offset computed after bne emission since k advanced by 4.
	int32_t branch_off = ((offset - static_cast<int32_t>(k)) >> 2);
	emit32(0x14000000 | (branch_off & 0x03FFFFFF), code, k);

	for (uint32_t i = 0; i < RegistersCount; ++i)
		reg_changed_offset[i] = k;

	codePos = k;
}

void JitCompilerA64::h_CFROUND(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];

	constexpr uint32_t tmp_reg = 20;
	constexpr uint32_t fpcr_tmp_reg = 8;

	// ror tmp_reg, src, imm
	emit32(ARMV8A::ROR_IMM | tmp_reg | (src << 5) | ((instr.getImm32() & 63) << 10) | (src << 16), code, k);

	if (flags & RANDOMX_FLAG_V2) {
		// tst tmp_reg, 60
		emit32(0xF27E0E9F, code, k);

		// bne next
		emit32(0x54000081, code, k);
	}

	// bfi fpcr_tmp_reg, tmp_reg, 40, 2
	emit32(0xB3580400 | fpcr_tmp_reg | (tmp_reg << 5), code, k);

	// rbit tmp_reg, fpcr_tmp_reg
	emit32(0xDAC00000 | tmp_reg | (fpcr_tmp_reg << 5), code, k);

	// msr fpcr, tmp_reg
	emit32(0xD51B4400 | tmp_reg, code, k);

	codePos = k;
}

void JitCompilerA64::h_ISTORE(Instruction& instr, uint32_t& codePos)
{
	uint32_t k = codePos;

	const uint32_t src = IntRegMap[instr.src];
	const uint32_t dst = IntRegMap[instr.dst];
	constexpr uint32_t tmp_reg = 20;

	uint32_t imm = instr.getImm32();

	if (instr.getModCond() < StoreL3Condition)
		imm &= instr.getModMem() ? (RANDOMX_SCRATCHPAD_L1 - 1) : (RANDOMX_SCRATCHPAD_L2 - 1);
	else
		imm &= RANDOMX_SCRATCHPAD_L3 - 1;

	emitAddImmediate(tmp_reg, dst, imm, code, k);

	constexpr uint32_t t = 0x927d0000 | tmp_reg | (tmp_reg << 5);
	constexpr uint32_t andInstrL1 = t | ((Log2(RANDOMX_SCRATCHPAD_L1) - 4) << 10);
	constexpr uint32_t andInstrL2 = t | ((Log2(RANDOMX_SCRATCHPAD_L2) - 4) << 10);
	constexpr uint32_t andInstrL3 = t | ((ScratchpadL3Log2 - 4) << 10);

	emit32((instr.getModCond() < StoreL3Condition) ? (instr.getModMem() ? andInstrL1 : andInstrL2) : andInstrL3, code, k);

	// str src, [x2, tmp_reg]
	emit32(0xF8206840 | src | (tmp_reg << 16), code, k);

	codePos = k;
}

void JitCompilerA64::h_NOP(Instruction& instr, uint32_t& codePos)
{
}

#include "instruction_weights.hpp"
#define INST_HANDLE(x) REPN(&JitCompilerA64::h_##x, WT(x))

	InstructionGeneratorA64 JitCompilerA64::engine[256] = {
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
}
