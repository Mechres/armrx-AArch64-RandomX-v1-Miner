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

#pragma once

#include <cstdint>
#include <vector>
#include <stdexcept>
#include <array>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <span>
#include "armrx/randomx_config.hpp"
#include "armrx/program.hpp"
#include "armrx/superscalar.hpp"
#include "armrx/jit_compiler_a64_static.hpp"

namespace armrx {

	struct ProgramConfiguration;
	using randomx_flags = uint32_t;
	using SuperscalarProgramList = std::array<SuperscalarProgram, 8>;

	class JitCompilerA64;
	typedef void(JitCompilerA64::*InstructionGeneratorA64)(Instruction&, uint32_t&);

	class JitCompilerA64 {
	public:
		JitCompilerA64();
		~JitCompilerA64();

		void generateProgram(Program&, ProgramConfiguration&);
		void generateProgramLight(Program&, ProgramConfiguration&, uint32_t, bool useHybrid = false);

		void generateSuperscalarHash(const SuperscalarProgramList& programs, const std::vector<uint64_t>&);

		void generateDatasetInitCode() {}

		ProgramFunc* getProgramFunc() { return reinterpret_cast<ProgramFunc*>(code); }
		DatasetInitFunc* getDatasetInitFunc();
		size_t getCodeSize() const;

		// Track D1: read-only accessor for the single-stream dataset-item
		// derivation function generateSuperscalarHash() just built at
		// `code + CodeSize` -- needed so a benchmark can call the existing,
		// proven single-stream path directly (same calling convention as
		// randomx_calc_dataset_item_aarch64: cache, output, itemNumber) for
		// a fair 1-way-vs-2-way comparison against JitDataset2Way. Purely
		// additive: exposes a region generateSuperscalarHash() already
		// wrote and JIT-executes via `bl rx_calc_dataset_item` in
		// production; this just hands back the same address as a callable.
		using CalcDatasetItemFunc = void (*)(const void* cache, void* output, std::uint64_t itemNumber);
		[[nodiscard]] CalcDatasetItemFunc getCalcDatasetItemFunc() const;

		// Read-only view of the emitted code buffer, for test/introspection use only
		// (e.g. decoding a specific instruction's bytes via a JitDumpEntry's offset).
		// Deliberately const-qualified and read-only, unlike the deleted getCode()
		// accessor this project removed for handing out a raw mutable pointer to
		// executable memory (docs/audits/WX_Alignment_and_LITTLE_Core_Profiling.md) — a
		// const view adds no new write-to-executable-memory capability.
		[[nodiscard]] std::span<const uint8_t> getCodeBytes() const { return {code, getCodeSize()}; }

		bool enableWriting();
		bool enableExecution();

		void setFlags(randomx_flags f) { flags = f; }

		// Track D1 (docs/plans/track-d1-superscalar-interleave-plan-20260728.md):
		// read-only passthrough to the private, already-verified superscalar
		// scheduler, so the new standalone 2-way dataset-item derivation
		// module (jit_dataset_2way.cpp) can reuse the exact same emission
		// order both streams must share, without duplicating the
		// hazard-detection logic (computeSuperscalarFootprint/hasHazard)
		// -- re-deriving that independently would risk exactly the kind of
		// silent divergence this project's postmortems warn about. Purely
		// additive; does not change scheduleSuperscalarProgram() itself or
		// any existing call site's behavior.
		std::vector<uint32_t> computeSuperscalarEmitOrder(const SuperscalarProgram& program) const {
			return scheduleSuperscalarProgram(program);
		}
	private:
		bool rwx_ = false;
		void emitPrologueMix(Program& program, uint32_t& codePos);
		void emitSpMix2(ProgramConfiguration& config, uint32_t& codePos);

		// PLAN.md Phase 6 item 12/L1 (2026-07-25): conservative emitter
		// lookahead scheduler -- reorders VM-instruction *emission* order
		// (never their semantics) to fill the stall after a long-latency
		// multiply with independent work, targeting IMUL_R/IMUL_RCP/
		// IMULH_R/ISMULH_R specifically (item 14's own `perf`-measured
		// dominant cost, not a guess). Full hazard/correctness design
		// (including the CBRANCH loop-body "anchor" argument -- CBRANCH
		// jumps back to the code position of the last writer of its
		// target register, and that instruction's *position* must never
		// move relative to what follows it, or the JIT's generated loop
		// would re-execute a different instruction set than the
		// interpreter's index-based loop) is in jit_compiler_a64.cpp
		// above scheduleProgram()'s definition.
		InstructionType resolveInstructionType(uint8_t opcode) const;
		std::vector<uint32_t> scheduleProgram(Program& program, uint32_t size) const;

		// Extension (2026-07-25): same idea applied to the superscalar
		// dataset-derivation region, where item 14's dominant IMUL cost
		// actually lives (the main-program scheduler above measured as a
		// null result precisely because it doesn't touch this region --
		// see changelogs.md). Structurally simpler (no CBRANCH/CFROUND, no
		// memory ops, single flat 8-register file) but has its own hazard
		// with no main-program analogue: IMUL_RCP's literal pool is
		// consumed via a sequential pointer that assumes emission order
		// never reorders IMUL_RCP instructions relative to each other.
		// Full design comment above computeSuperscalarFootprint() in
		// jit_compiler_a64.cpp.
		std::vector<uint32_t> scheduleSuperscalarProgram(const SuperscalarProgram& program) const;

		static InstructionGeneratorA64 engine[256];
		uint32_t reg_changed_offset[8];
		uint8_t* code;
		uint32_t literalPos;
		uint32_t num32bitLiterals;

		randomx_flags flags;

		static void emit32(uint32_t val, uint8_t* code, uint32_t& codePos)
		{
			memcpy(code + codePos, &val, sizeof(val));
			codePos += sizeof(val);
		}

		static void emit64(uint64_t val, uint8_t* code, uint32_t& codePos)
		{
			memcpy(code + codePos, &val, sizeof(val));
			codePos += sizeof(val);
		}

		void emitMovImmediate(uint32_t dst, uint32_t imm, uint8_t* code, uint32_t& codePos);
		void emitAddImmediate(uint32_t dst, uint32_t src, uint32_t imm, uint8_t* code, uint32_t& codePos);

		template<uint32_t tmp_reg>
		void emitMemLoad(uint32_t dst, uint32_t src, Instruction& instr, uint8_t* code, uint32_t& codePos);

		template<uint32_t tmp_reg_fp>
		void emitMemLoadFP(uint32_t src, Instruction& instr, uint8_t* code, uint32_t& codePos);

		void h_IADD_RS(Instruction&, uint32_t&);
		void h_IADD_M(Instruction&, uint32_t&);
		void h_ISUB_R(Instruction&, uint32_t&);
		void h_ISUB_M(Instruction&, uint32_t&);
		void h_IMUL_R(Instruction&, uint32_t&);
		void h_IMUL_M(Instruction&, uint32_t&);
		void h_IMULH_R(Instruction&, uint32_t&);
		void h_IMULH_M(Instruction&, uint32_t&);
		void h_ISMULH_R(Instruction&, uint32_t&);
		void h_ISMULH_M(Instruction&, uint32_t&);
		void h_IMUL_RCP(Instruction&, uint32_t&);
		void h_INEG_R(Instruction&, uint32_t&);
		void h_IXOR_R(Instruction&, uint32_t&);
		void h_IXOR_M(Instruction&, uint32_t&);
		void h_IROR_R(Instruction&, uint32_t&);
		void h_IROL_R(Instruction&, uint32_t&);
		void h_ISWAP_R(Instruction&, uint32_t&);
		void h_FSWAP_R(Instruction&, uint32_t&);
		void h_FADD_R(Instruction&, uint32_t&);
		void h_FADD_M(Instruction&, uint32_t&);
		void h_FSUB_R(Instruction&, uint32_t&);
		void h_FSUB_M(Instruction&, uint32_t&);
		void h_FSCAL_R(Instruction&, uint32_t&);
		void h_FMUL_R(Instruction&, uint32_t&);
		void h_FDIV_M(Instruction&, uint32_t&);
		void h_FSQRT_R(Instruction&, uint32_t&);
		void h_CBRANCH(Instruction&, uint32_t&);
		void h_CFROUND(Instruction&, uint32_t&);
		void h_ISTORE(Instruction&, uint32_t&);
		void h_NOP(Instruction&, uint32_t&);

		static void emitV2AesTweak(JitCompilerA64& jit, uint32_t flags, uint32_t codePos);

		// ── JIT dump infrastructure (--jit-dump) ─────────────────────
	public:
		struct JitDumpEntry {
			uint32_t opcode;
			uint32_t offset;   // byte offset in code buffer
			uint32_t size;     // bytes emitted by this instruction's handler
		};

		void enableJitDump() {
			jit_dump_enabled_ = true;
			jit_dump_.clear();
			superscalar_jit_dump_.clear();
		}
		void dumpJitCode() const;
		const std::vector<JitDumpEntry>& getJitDump() const { return jit_dump_; }

		// PLAN.md Phase 6 item 13 (2026-07-24): region-scoped instrumentation for the
		// superscalar/dataset-derivation path -- `--jit-dump`'s main table only ever
		// covered the fixed, 2047-instruction per-hash VM program; this covers
		// generateSuperscalarHash()'s emitted code instead, which in light mode is
		// what `bl rx_calc_dataset_item` actually calls once per main-loop iteration
		// (2048 iterations x 8 programs = 16,384 calls/hash) -- the region that
		// dominates real instruction volume, not the small fixed program dumped above.
		const std::vector<JitDumpEntry>& getSuperscalarJitDump() const { return superscalar_jit_dump_; }
	private:
		bool jit_dump_enabled_ = false;
		std::vector<JitDumpEntry> jit_dump_;
		std::vector<JitDumpEntry> superscalar_jit_dump_;
	};
}
