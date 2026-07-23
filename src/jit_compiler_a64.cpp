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
// RANDOMX_PROGRAM_MAX_SIZE * 16 * 4 bytes (6144 AArch64 instruction slots).
// Each RandomX instruction must compile to at most 16 AArch64 words.
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

void JitCompilerA64::emitPrologueMix(Program& program, uint32_t& codePos) {
	codePos = PrologueSize;
	literalPos = ImulRcpLiteralsEnd;
	num32bitLiterals = 0;

	for (uint32_t i = 0; i < RegistersCount; ++i)
		reg_changed_offset[i] = codePos;

	for (uint32_t i = 0; i < program.getSize(flags); ++i)
	{
		Instruction& instr = program(i);
		instr.src %= RegistersCount;
		instr.dst %= RegistersCount;
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
	std::cout << std::flush;
}

void JitCompilerA64::generateSuperscalarHash(const SuperscalarProgramList& programs, const std::vector<uint64_t>& reciprocalCache)
{
	uint32_t codePos = CodeSize;

	uint8_t* p1 = (uint8_t*)randomx_calc_dataset_item_aarch64;
	uint8_t* p2 = (uint8_t*)randomx_calc_dataset_item_aarch64_prefetch;
	memcpy(code + codePos, p1, p2 - p1);
	codePos += p2 - p1;

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

		for (size_t j = 0; j < progSize; ++j)
		{
			const Instruction& instr = prog(j);
			const uint32_t src = instr.src;
			const uint32_t dst = instr.dst;

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

	// sub dst, dst, tmp_reg
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
		const uint32_t offset = (literalPos - k) / 4;
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
