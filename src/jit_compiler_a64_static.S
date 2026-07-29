# Copyright (c) 2018-2019, tevador <tevador@gmail.com>
# Copyright (c) 2019, SChernykh    <https://github.com/SChernykh>
#
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 	* Redistributions of source code must retain the above copyright
# 	  notice, this list of conditions and the following disclaimer.
# 	* Redistributions in binary form must reproduce the above copyright
# 	  notice, this list of conditions and the following disclaimer in the
# 	  documentation and/or other materials provided with the distribution.
# 	* Neither the name of the copyright holder nor the
# 	  names of its contributors may be used to endorse or promote products
# 	  derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#if defined(__APPLE__)
#define DECL(x) _##x
#else
#define DECL(x) x
#endif

	.arch armv8-a+crypto
	.text
	.global DECL(randomx_program_aarch64)
	.global DECL(randomx_program_aarch64_main_loop)
	.global DECL(randomx_program_aarch64_vm_instructions)
	.global DECL(randomx_program_aarch64_imul_rcp_literals_end)
	.global DECL(randomx_program_aarch64_vm_instructions_end)
	.global DECL(randomx_program_aarch64_cacheline_align_mask1)
	.global DECL(randomx_program_aarch64_cacheline_align_mask2)
	.global DECL(randomx_program_aarch64_update_spMix1)
	.global DECL(randomx_program_aarch64_v2_FE_mix)
	.global DECL(randomx_program_aarch64_v1_FE_mix)
	.global DECL(randomx_program_aarch64_v2_FE_mix_soft_aes)
	#ifndef __APPLE__
	.hidden DECL(randomx_program_aarch64_aes_lut_pointers)
	#endif
	.global DECL(randomx_program_aarch64_aes_lut_pointers)
	.global DECL(randomx_program_aarch64_vm_instructions_end_light)
	.global DECL(randomx_program_aarch64_vm_instructions_end_hybrid)
	.global DECL(randomx_program_aarch64_hybrid_cacheline_align_mask)
	.global DECL(randomx_program_aarch64_vm_instructions_end_hybrid_tweak)
	.global DECL(randomx_program_aarch64_hybrid_dataset_offset)
	.global DECL(randomx_program_aarch64_vm_instructions_end_light_tweak)
	.global DECL(randomx_program_aarch64_light_cacheline_align_mask)
	.global DECL(randomx_program_aarch64_light_dataset_offset)
	.global DECL(randomx_program_aarch64_vm_instructions_end_v1)
	.global DECL(randomx_program_aarch64_vm_instructions_end_v2)
	.global DECL(randomx_program_aarch64_vm_instructions_end_light_v1)
	.global DECL(randomx_program_aarch64_vm_instructions_end_light_v2)
	.global DECL(randomx_init_dataset_aarch64)
	.global DECL(randomx_init_dataset_aarch64_end)
	.global DECL(randomx_calc_dataset_item_aarch64)
	.global DECL(randomx_calc_dataset_item_aarch64_prefetch)
	.global DECL(randomx_calc_dataset_item_aarch64_mix)
	.global DECL(randomx_calc_dataset_item_aarch64_store_result)
	.global DECL(randomx_calc_dataset_item_aarch64_end)
	# Track D1: 2-way interleaved dataset-item derivation (new, separate
	# entry point -- see the register-contract comment at its definition).
	.global DECL(randomx_calc_dataset_item_aarch64_2way)
	.global DECL(randomx_calc_dataset_item_aarch64_2way_prefetch)
	.global DECL(randomx_calc_dataset_item_aarch64_2way_mix)
	.global DECL(randomx_calc_dataset_item_aarch64_2way_store_result)
	.global DECL(randomx_calc_dataset_item_aarch64_2way_end)

#include "configuration.h"

# Register allocation

# x0  -> pointer to reg buffer and then literal for IMUL_RCP
# x1  -> pointer to mem buffer and then to dataset
# x2  -> pointer to scratchpad
# x3  -> loop counter
# x4  -> "r0"
# x5  -> "r1"
# x6  -> "r2"
# x7  -> "r3"
# x8  -> fpcr (reversed bits)
# x9  -> mx, ma
# x10 -> spMix1
# x11 -> literal for IMUL_RCP
# x12 -> "r4"
# x13 -> "r5"
# x14 -> "r6"
# x15 -> "r7"
# x16 -> spAddr0
# x17 -> spAddr1
# x18 -> unused (platform register, don't touch it)
# x19 -> temporary
# x20 -> temporary
# x21 -> literal for IMUL_RCP
# x22 -> literal for IMUL_RCP
# x23 -> literal for IMUL_RCP
# x24 -> literal for IMUL_RCP
# x25 -> literal for IMUL_RCP
# x26 -> literal for IMUL_RCP
# x27 -> literal for IMUL_RCP
# x28 -> literal for IMUL_RCP
# x29 -> literal for IMUL_RCP
# x30 -> literal for IMUL_RCP

# v0-v15 -> store 32-bit literals
# v16 -> "f0"
# v17 -> "f1"
# v18 -> "f2"
# v19 -> "f3"
# v20 -> "e0"
# v21 -> "e1"
# v22 -> "e2"
# v23 -> "e3"
# v24 -> "a0"
# v25 -> "a1"
# v26 -> "a2"
# v27 -> "a3"
# v28 -> temporary
# v29 -> E 'and' mask = 0x00ffffffffc0000000ffffffffc00000
# v30 -> E 'or' mask  = 0x3*00000000******3*00000000******
# v31 -> scale mask   = 0x81f000000000000081f0000000000000

	.balign 4
DECL(randomx_program_aarch64):
	# Save callee-saved registers
	sub	sp, sp, 192
	stp	x16, x17, [sp]
	str	x19, [sp, 16]
	stp	x20, x21, [sp, 32]
	stp	x22, x23, [sp, 48]
	stp	x24, x25, [sp, 64]
	stp	x26, x27, [sp, 80]
	stp	x28, x29, [sp, 96]
	stp	x8, x30, [sp, 112]
	stp	d8, d9, [sp, 128]
	stp	d10, d11, [sp, 144]
	stp	d12, d13, [sp, 160]
	stp	d14, d15, [sp, 176]

	# Zero integer registers
	mov	x4, xzr
	mov	x5, xzr
	mov	x6, xzr
	mov	x7, xzr
	mov	x12, xzr
	mov	x13, xzr
	mov	x14, xzr
	mov	x15, xzr

	# Save original x1 (MemoryRegisters*) before ldp clobbers it
	str	x1, [sp, 24]

	# Load ma, mx and dataset pointer
	ldp	x9, x1, [x1]

	# Load initial spMix value
	mov	x10, x9

	# Load group A registers
	ldp	q24, q25, [x0, 192]
	ldp	q26, q27, [x0, 224]

	# Load E 'and' mask
	mov	x16, 0x00FFFFFFFFC00000
	ins	v29.d[0], x16
	ins	v29.d[1], x16

	# Load E 'or' mask (stored in reg.f[0])
	ldr	q30, [x0, 64]

	# Load scale mask
	mov	x16, 0x80f0000000000000
	ins	v31.d[0], x16
	ins	v31.d[1], x16

	# Read fpcr
	mrs	x8, fpcr
	rbit	x8, x8

	# Save x0
	str	x0, [sp, -16]!

	# Read literals
	ldr	x0, literal_x0
	ldr	x11, literal_x11
	ldr	x21, literal_x21
	ldr	x22, literal_x22
	ldr	x23, literal_x23
	ldr	x24, literal_x24
	ldr	x25, literal_x25
	ldr	x26, literal_x26
	ldr	x27, literal_x27
	ldr	x28, literal_x28
	ldr	x29, literal_x29
	ldr	x30, literal_x30

	ldr	q0, literal_v0
	ldr	q1, literal_v1
	ldr	q2, literal_v2
	ldr	q3, literal_v3
	ldr	q4, literal_v4
	ldr	q5, literal_v5
	ldr	q6, literal_v6
	ldr	q7, literal_v7
	ldr	q8, literal_v8
	ldr	q9, literal_v9
	ldr	q10, literal_v10
	ldr	q11, literal_v11
	ldr	q12, literal_v12
	ldr	q13, literal_v13
	ldr	q14, literal_v14
	ldr	q15, literal_v15

	# Compute first iteration's spAddr0 and spAddr1
	ubfx	x16, x10, #6, #15
	ubfx	x17, x10, #38, #15
	add	x16, x2, x16, lsl #6
	add	x17, x2, x17, lsl #6
	prfm	pldl1keep, [x16]
	prfm	pldl1keep, [x17]
	prfm	pldl1keep, [x17, #32]

	# 64-byte alignment matches the Cortex-A53's I-cache line size exactly
	# (docs/plans/experimental-performance-ideas-20260725.md #11); was
	# 32-byte, a weaker guarantee.
	.p2align 6
DECL(randomx_program_aarch64_main_loop):
.Lmain_loop:

	# Interleaved loading of FP registers (d16-d23) and integer registers (F0-F3)
	# to hide load latencies and FP execution delays.
	
	# Load first batch of FP registers to build distance before sshll/scvtf
	ldr	d16, [x17]
	ldr	d17, [x17, #8]
	ldr	d18, [x17, #16]
	ldr	d19, [x17, #24]
	ldr	d20, [x17, #32]

	# Start loading F0 int registers
	ldp	x20, x19, [x16]
	# Hide the 3-cycle load-use latency of the ldp by completing FP register loads
	ldr	d21, [x17, #40]
	ldr	d22, [x17, #48]
	ldr	d23, [x17, #56]
	# XOR F0 int registers
	eor	x4, x4, x20
	eor	x5, x5, x19

	# Start loading F1 int registers
	ldp	x20, x19, [x16, 16]
	# Hide ldp latency with first three FP shifts
	sshll	v16.2d, v16.2s, #0
	sshll	v17.2d, v17.2s, #0
	sshll	v18.2d, v18.2s, #0
	# XOR F1 int registers
	eor	x6, x6, x20
	eor	x7, x7, x19

	# Start loading F2 int registers
	ldp	x20, x19, [x16, 32]
	# Hide ldp latency with next three FP shifts
	sshll	v19.2d, v19.2s, #0
	sshll	v20.2d, v20.2s, #0
	sshll	v21.2d, v21.2s, #0
	# XOR F2 int registers
	eor	x12, x12, x20
	eor	x13, x13, x19

	# Start loading F3 int registers
	ldp	x20, x19, [x16, 48]
	# Hide ldp latency with remaining FP shifts & first convert (F0)
	sshll	v22.2d, v22.2s, #0
	sshll	v23.2d, v23.2s, #0
	scvtf	v16.2d, v16.2d
	# XOR F3 int registers
	eor	x14, x14, x20
	eor	x15, x15, x19

	# FP converts (scvtf) for remaining registers
	scvtf	v17.2d, v17.2d
	scvtf	v18.2d, v18.2d
	scvtf	v19.2d, v19.2d
	scvtf	v20.2d, v20.2d
	scvtf	v21.2d, v21.2d
	scvtf	v22.2d, v22.2d
	scvtf	v23.2d, v23.2d

	# E register logic mask (bif) - v20-v23 depend on scvtf v20-v23.
	# Constant 4-instruction gap ensures minimal pipeline stalls.
	bif	v20.16b, v30.16b, v29.16b
	bif	v21.16b, v30.16b, v29.16b
	bif	v22.16b, v30.16b, v29.16b
	bif	v23.16b, v30.16b, v29.16b

	# Execute VM instructions
DECL(randomx_program_aarch64_vm_instructions):

	.fill RANDOMX_PROGRAM_MAX_SIZE*32,4,0

literal_x0:  .fill 1,8,0
literal_x11: .fill 1,8,0
literal_x21: .fill 1,8,0
literal_x22: .fill 1,8,0
literal_x23: .fill 1,8,0
literal_x24: .fill 1,8,0
literal_x25: .fill 1,8,0
literal_x26: .fill 1,8,0
literal_x27: .fill 1,8,0
literal_x28: .fill 1,8,0
literal_x29: .fill 1,8,0
literal_x30: .fill 1,8,0
DECL(randomx_program_aarch64_imul_rcp_literals_end):

literal_v0:  .fill 2,8,0
literal_v1:  .fill 2,8,0
literal_v2:  .fill 2,8,0
literal_v3:  .fill 2,8,0
literal_v4:  .fill 2,8,0
literal_v5:  .fill 2,8,0
literal_v6:  .fill 2,8,0
literal_v7:  .fill 2,8,0
literal_v8:  .fill 2,8,0
literal_v9:  .fill 2,8,0
literal_v10: .fill 2,8,0
literal_v11: .fill 2,8,0
literal_v12: .fill 2,8,0
literal_v13: .fill 2,8,0
literal_v14: .fill 2,8,0
literal_v15: .fill 2,8,0

DECL(randomx_program_aarch64_vm_instructions_end):
	# Calculate dataset pointer for dataset read
	# Do it here to break false dependency from readReg2 and readReg3 (see next line)
	lsr	x10, x9, 32

	# mx ^= r[readReg2] ^ r[readReg3];
	eor	x9, x9, x20

	# Calculate dataset pointer for dataset prefetch
	mov	w20, w9

	# mx <-> ma
	ror	x9, x9, 32

DECL(randomx_program_aarch64_cacheline_align_mask1):
	# Actual mask will be inserted by JIT compiler
	and	x20, x20, 1
	add	x20, x20, x1

	# Prefetch dataset data (pldl1keep for immediate use; pldl2keep may be better
	# on systems where L1 scratchpad eviction is measurable in fast mode)
	prfm	pldl1keep, [x20]

DECL(randomx_program_aarch64_cacheline_align_mask2):
	# Actual mask will be inserted by JIT compiler
	and	x10, x10, 1
	add	x10, x10, x1

DECL(randomx_program_aarch64_xor_with_dataset_line):
rx_program_xor_with_dataset_line:
	# xor integer registers with dataset data
	ldp	x20, x19, [x10]
	eor	x4, x4, x20
	eor	x5, x5, x19
	ldp	x20, x19, [x10, 16]
	eor	x6, x6, x20
	eor	x7, x7, x19
	ldp	x20, x19, [x10, 32]
	eor	x12, x12, x20
	eor	x13, x13, x19
	ldp	x20, x19, [x10, 48]
	eor	x14, x14, x20
	eor	x15, x15, x19

DECL(randomx_program_aarch64_update_spMix1):
	# JIT compiler will replace it with "eor x10, config.readReg0, config.readReg1"
	eor	x10, x0, x0

	# Actual instructions will be inserted by JIT compiler
	and	w19, w10, 1
	and	w20, w10, 1

	# Compute next iteration's scratchpad addresses
	add	x19, x2, x19, lsl #6
	add	x20, x2, x20, lsl #6

	# Prefetch next iteration's scratchpad data -- removed 2026-07-24 (PLAN.md Phase 6
	# item 10): measured a small but real *regression* from these three prfm hints on
	# this Cortex-A53 (perf stat, interleaved A/B, ~0.885% fewer instructions/wall-time
	# with them present, confirmed low-noise; wall-clock hashrate agreed at ~0.4%).
	# Do not re-add without a fresh measurement -- see changelogs.md for the full account.
	# prfm	pldl1keep, [x19]
	# prfm	pldl1strm, [x20]
	# prfm	pldl1keep, [x20, 32]

	# Store integer registers to scratchpad (spAddr1)
	stp	x4, x5, [x17, 0]
	stp	x6, x7, [x17, #16]
	stp	x12, x13, [x17, #32]
	stp	x14, x15, [x17, #48]

	# RandomX v2 AES tweak (mix group F and group E registers using AES)
DECL(randomx_program_aarch64_v2_FE_mix):

	# Jump to v1 FE mix code if we're running RandomX v1
	# JIT compiler will write a "movi v28.4s, 0" (set v28 to all 0) here if we're running RandomX v2
	# Or, JIT compiler will write a "b randomx_program_aarch64_v2_FE_mix_soft_aes" if we're running RandomX v2 with soft AES
	b DECL(randomx_program_aarch64_v1_FE_mix)

	# f0 = aesenc(f0, e0), f1 = aesdec(f1, e0), f2 = aesenc(f2, e0), f3 = aesdec(f3, e0)

	aese	v16.16b, v28.16b
	aesd	v17.16b, v28.16b
	aese	v18.16b, v28.16b
	aesd	v19.16b, v28.16b

	aesmc	v16.16b, v16.16b
	aesimc	v17.16b, v17.16b
	aesmc	v18.16b, v18.16b
	aesimc	v19.16b, v19.16b

	eor	v16.16b, v16.16b, v20.16b
	eor	v17.16b, v17.16b, v20.16b
	eor	v18.16b, v18.16b, v20.16b
	eor	v19.16b, v19.16b, v20.16b

	# f0 = aesenc(f0, e1), f1 = aesdec(f1, e1), f2 = aesenc(f2, e1), f3 = aesdec(f3, e1)

	aese	v16.16b, v28.16b
	aesd	v17.16b, v28.16b
	aese	v18.16b, v28.16b
	aesd	v19.16b, v28.16b

	aesmc	v16.16b, v16.16b
	aesimc	v17.16b, v17.16b
	aesmc	v18.16b, v18.16b
	aesimc	v19.16b, v19.16b

	eor	v16.16b, v16.16b, v21.16b
	eor	v17.16b, v17.16b, v21.16b
	eor	v18.16b, v18.16b, v21.16b
	eor	v19.16b, v19.16b, v21.16b

	# f0 = aesenc(f0, e2), f1 = aesdec(f1, e2), f2 = aesenc(f2, e2), f3 = aesdec(f3, e2)

	aese	v16.16b, v28.16b
	aesd	v17.16b, v28.16b
	aese	v18.16b, v28.16b
	aesd	v19.16b, v28.16b

	aesmc	v16.16b, v16.16b
	aesimc	v17.16b, v17.16b
	aesmc	v18.16b, v18.16b
	aesimc	v19.16b, v19.16b

	eor	v16.16b, v16.16b, v22.16b
	eor	v17.16b, v17.16b, v22.16b
	eor	v18.16b, v18.16b, v22.16b
	eor	v19.16b, v19.16b, v22.16b

	# f0 = aesenc(f0, e3), f1 = aesdec(f1, e3), f2 = aesenc(f2, e3), f3 = aesdec(f3, e3)

	aese	v16.16b, v28.16b
	aesd	v17.16b, v28.16b
	aese	v18.16b, v28.16b
	aesd	v19.16b, v28.16b

	aesmc	v16.16b, v16.16b
	aesimc	v17.16b, v17.16b
	aesmc	v18.16b, v18.16b
	aesimc	v19.16b, v19.16b

	eor	v16.16b, v16.16b, v23.16b
	eor	v17.16b, v17.16b, v23.16b
	eor	v18.16b, v18.16b, v23.16b
	eor	v19.16b, v19.16b, v23.16b

	# Skip v1 FE mix code because we already did v2 FE mix
	b randomx_program_aarch64_FE_store

DECL(randomx_program_aarch64_v1_FE_mix):
	eor	v16.16b, v16.16b, v20.16b
	eor	v17.16b, v17.16b, v21.16b
	eor	v18.16b, v18.16b, v22.16b
	eor	v19.16b, v19.16b, v23.16b

randomx_program_aarch64_FE_store:

	# Store FP registers to scratchpad (spAddr0)
	stp	q16, q17, [x16, 0]
	stp	q18, q19, [x16, 32]

	# Move next-iteration scratchpad addresses into active registers
	mov	x16, x19
	mov	x17, x20

	subs	x3, x3, 1
	bne	.Lmain_loop
	
	# Restore x0
	ldr	x0, [sp], 16

	# Store integer registers
	stp	x4, x5, [x0, 0]
	stp	x6, x7, [x0, 16]
	stp	x12, x13, [x0, 32]
	stp	x14, x15, [x0, 48]

	# Store FP registers
	stp	q16, q17, [x0, 64]
	stp	q18, q19, [x0, 96]
	stp	q20, q21, [x0, 128]
	stp	q22, q23, [x0, 160]

	# Restore callee-saved registers
	ldp	x16, x17, [sp]
	ldr	x19, [sp, 16]
	ldp	x20, x21, [sp, 32]
	ldp	x22, x23, [sp, 48]
	ldp	x24, x25, [sp, 64]
	ldp	x26, x27, [sp, 80]
	ldp	x28, x29, [sp, 96]
	ldp	x8, x30, [sp, 112]
	ldp	d8, d9, [sp, 128]
	ldp	d10, d11, [sp, 144]
	ldp	d12, d13, [sp, 160]
	ldp	d14, d15, [sp, 176]
	add	sp, sp, 192

	ret

DECL(randomx_program_aarch64_vm_instructions_end_light):
	sub	sp, sp, 96
	stp	x0, x1, [sp, 64]
	stp	x2, x30, [sp, 80]

	lsr	x2, x9, 32

DECL(randomx_program_aarch64_light_cacheline_align_mask):
	# Actual mask will be inserted by JIT compiler
	and	w2, w2, 1

DECL(randomx_program_aarch64_vm_instructions_end_light_tweak):
	# mx ^= r[readReg2] ^ r[readReg3];
	eor	x9, x9, x20

	# mx <-> ma
	ror	x9, x9, 32

	# x0 -> pointer to cache memory
	mov	x0, x1

	# x1 -> pointer to output
	mov	x1, sp

	# x2 -> item number
	lsr	x2, x2, 6

DECL(randomx_program_aarch64_light_dataset_offset):
	# Apply dataset offset (filled in by JIT compiler)
	add	x2, x2, 0
	add	x2, x2, 0

	bl	rx_calc_dataset_item

	mov	x10, sp
	ldp	x0, x1, [sp, 64]
	ldp	x2, x30, [sp, 80]
	add	sp, sp, 96

	b	rx_program_xor_with_dataset_line

# ── Hybrid partial-dataset entry point ──────────────────────────────────
# Same calling convention as _end_light but adds a bound check before the
# derivation call: if item_number < partial_dataset_items_, load the 64-byte
# item directly from partial_dataset_ (the mmap'd prefix buffer) instead of
# calling rx_calc_dataset_item. MemoryRegisters layout:
#   offset  0: mx  (uint32_t)
#   offset  4: ma  (uint32_t)
#   offset  8: memory (uint8_t*)
#   offset 16: partial_dataset_ (uint8_t*)
#   offset 24: partial_dataset_items_ (size_t)
#
# x1 at entry = MemoryRegisters pointer (saved at [sp, 72]).
# Uses x16-x17 as temporaries (scratch per AArch64 ABI, not callee-saved).
#
# When partial_dataset_items_ is 0, the cmp always branches to the miss path
# (item >= 0 always), so this entry point is safe to use unconditionally — it
# reverts to the pure derivation path at zero cost whenever no partial dataset
# is configured.

DECL(randomx_program_aarch64_vm_instructions_end_hybrid):
	sub	sp, sp, 112
	stp	x0, x1, [sp, 64]
	stp	x2, x30, [sp, 80]
	# Save x16 (scratchpad address used by caller) — corrupted by bound check below
	str	x16, [sp, 96]
	# Save x17 (scratchpad address used by caller) — corrupted by hit path ldp loads
	str	x17, [sp, 104]
	# MemoryRegisters* was saved at prologue's [sp, 24]; sub sp,sp,192 then
	# str x0,[sp,-16]! gives a 208-byte base frame, and the 112-byte frame
	# puts it at [sp + 152] relative to this stack pointer.

	lsr	x2, x9, 32

DECL(randomx_program_aarch64_hybrid_cacheline_align_mask):
	and	w2, w2, 1

DECL(randomx_program_aarch64_vm_instructions_end_hybrid_tweak):
	eor	x9, x9, x20
	ror	x9, x9, 32

	# x2 -> raw item number (before dataset offset)
	lsr	x2, x2, 6

	# Bound check using saved MemoryRegisters* at [sp, 152]
	ldr	x16, [sp, 152]
	ldr	w16, [x16, 24]
	cbz	w16, 1f
	cmp	x2, x16
	b.hs	1f

	# ── HIT path: load from partial_dataset_ directly ──
	# Reload MemoryRegisters* from stack — x16 was clobbered by bound check
	ldr	x16, [sp, 152]
	ldr	x16, [x16, 16]
	add	x16, x16, x2, lsl 6
	ldp	x17, x0, [x16]
	stp	x17, x0, [sp]
	ldp	x17, x0, [x16, 16]
	stp	x17, x0, [sp, 16]
	ldp	x17, x0, [x16, 32]
	stp	x17, x0, [sp, 32]
	ldp	x17, x0, [x16, 48]
	stp	x17, x0, [sp, 48]
	b	2f

1:	# ── MISS path: apply dataset offset and derive ──
DECL(randomx_program_aarch64_hybrid_dataset_offset):
	add	x2, x2, 0
	add	x2, x2, 0
	mov	x0, x1
	mov	x1, sp
	bl	rx_calc_dataset_item

2:	# ── Common exit ──
	# Restore x16, x17 (scratchpad addresses) saved at _end_hybrid entry
	ldp	x16, x17, [sp, 96]
	mov	x10, sp
	ldp	x0, x1, [sp, 64]
	ldp	x2, x30, [sp, 80]
	add	sp, sp, 112
	b	rx_program_xor_with_dataset_line

DECL(randomx_program_aarch64_vm_instructions_end_v1):
	lsr	x10, x9, 32
	eor	x9, x9, x20
	mov	w20, w9
	ror	x9, x9, 32

DECL(randomx_program_aarch64_vm_instructions_end_v2):
	lsr	x10, x9, 32
	ror	x9, x9, 32
	eor	x9, x9, x20
	mov	w20, w9

DECL(randomx_program_aarch64_vm_instructions_end_light_v1):
	eor	x9, x9, x20
	ror	x9, x9, 32

DECL(randomx_program_aarch64_vm_instructions_end_light_v2):
	ror	x9, x9, 32
	eor	x9, x9, x20

DECL(randomx_program_aarch64_v2_FE_mix_soft_aes):
	sub	sp, sp, 176

	stp	x0, x1, [sp]
	stp	x2, x3, [sp, 16]
	stp	x4, x5, [sp, 32]
	stp	x6, x7, [sp, 48]
	stp	x8, x9, [sp, 64]
	stp	x10, x11, [sp, 80]
	stp	x12, x13, [sp, 96]
	stp	x14, x15, [sp, 112]
	stp	x16, x30, [sp, 128]
	stp	q0, q1, [sp, 144]

	adr	x19, DECL(randomx_program_aarch64_aes_lut_pointers)
	ldp	x19, x20, [x19]

	# f0 = aesenc(f0, e0), f0 = aesenc(f0, e1), f0 = aesenc(f0, e2), f0 = aesenc(f0, e3)
	mov	v0.16b, v16.16b
	mov	v1.16b, v20.16b
	bl	randomx_soft_aesenc
	mov	v1.16b, v21.16b
	bl	randomx_soft_aesenc
	mov	v1.16b, v22.16b
	bl	randomx_soft_aesenc
	mov	v1.16b, v23.16b
	bl	randomx_soft_aesenc
	mov	v16.16b, v0.16b

	# f1 = aesdec(f1, e0), f1 = aesdec(f1, e1), f1 = aesdec(f1, e2), f1 = aesdec(f1, e3)
	mov	v0.16b, v17.16b
	mov	v1.16b, v20.16b
	bl	randomx_soft_aesdec
	mov	v1.16b, v21.16b
	bl	randomx_soft_aesdec
	mov	v1.16b, v22.16b
	bl	randomx_soft_aesdec
	mov	v1.16b, v23.16b
	bl	randomx_soft_aesdec
	mov	v17.16b, v0.16b

	# f2 = aesenc(f2, e0), f2 = aesenc(f2, e1), f2 = aesenc(f2, e2), f2 = aesenc(f2, e3)
	mov	v0.16b, v18.16b
	mov	v1.16b, v20.16b
	bl	randomx_soft_aesenc
	mov	v1.16b, v21.16b
	bl	randomx_soft_aesenc
	mov	v1.16b, v22.16b
	bl	randomx_soft_aesenc
	mov	v1.16b, v23.16b
	bl	randomx_soft_aesenc
	mov	v18.16b, v0.16b

	# f3 = aesdec(f3, e0), f3 = aesdec(f3, e1), f3 = aesdec(f3, e2), f3 = aesdec(f3, e3)
	mov	v0.16b, v19.16b
	mov	v1.16b, v20.16b
	bl	randomx_soft_aesdec
	mov	v1.16b, v21.16b
	bl	randomx_soft_aesdec
	mov	v1.16b, v22.16b
	bl	randomx_soft_aesdec
	mov	v1.16b, v23.16b
	bl	randomx_soft_aesdec
	mov	v19.16b, v0.16b

	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, 16]
	ldp	x4, x5, [sp, 32]
	ldp	x6, x7, [sp, 48]
	ldp	x8, x9, [sp, 64]
	ldp	x10, x11, [sp, 80]
	ldp	x12, x13, [sp, 96]
	ldp	x14, x15, [sp, 112]
	ldp	x16, x30, [sp, 128]
	ldp	q0, q1, [sp, 144]

	add	sp, sp, 176

	b randomx_program_aarch64_FE_store
	

randomx_soft_aesenc:
	umov	w4, v0.b[5]
	umov	w1, v0.b[10]
	umov	w12, v0.b[15]
	umov	w9, v0.b[9]
	umov	w2, v0.b[14]
	umov	w11, v0.b[3]
	umov	w5, v0.b[0]
	umov	w16, v0.b[4]
	add	x4, x4, 256
	add	x1, x1, 512
	add	x12, x12, 768
	umov	w3, v0.b[13]
	umov	w8, v0.b[2]
	umov	w7, v0.b[7]
	add	x9, x9, 256
	add	x2, x2, 512
	add	x11, x11, 768
	ldr	w10, [x19, x4, lsl 2]
	ldr	w15, [x19, x5, lsl 2]
	umov	w13, v0.b[8]
	ldr	w14, [x19, x12, lsl 2]
	umov	w6, v0.b[1]
	ldr	w1, [x19, x1, lsl 2]
	eor	w10, w10, w15
	ldr	w2, [x19, x2, lsl 2]
	umov	w5, v0.b[6]
	ldr	w9, [x19, x9, lsl 2]
	umov	w4, v0.b[11]
	ldr	w12, [x19, x16, lsl 2]
	eor	w1, w1, w14
	ldr	w11, [x19, x11, lsl 2]
	eor	w1, w1, w10
	add	x8, x8, 512
	add	x3, x3, 256
	add	x7, x7, 768
	eor	w9, w9, w12
	fmov	s28, w1
	eor	w1, w2, w11
	umov	w10, v0.b[12]
	eor	w1, w1, w9
	ldr	w3, [x19, x3, lsl 2]
	add	x6, x6, 256
	ldr	w9, [x19, x13, lsl 2]
	ins	v28.s[1], w1
	ldr	w2, [x19, x8, lsl 2]
	add	x5, x5, 512
	ldr	w7, [x19, x7, lsl 2]
	add	x4, x4, 768
	eor	w1, w3, w9
	ldr	w3, [x19, x6, lsl 2]
	eor	w2, w2, w7
	ldr	w6, [x19, x10, lsl 2]
	eor	w2, w2, w1
	ldr	w1, [x19, x5, lsl 2]
	ldr	w0, [x19, x4, lsl 2]
	eor	w3, w3, w6
	ins	v28.s[2], w2
	eor	w0, w1, w0
	eor	w0, w0, w3
	ins	v28.s[3], w0
	eor	v0.16b, v1.16b, v28.16b
	ret

randomx_soft_aesdec:
	umov	w1, v0.b[10]
	umov	w3, v0.b[7]
	umov	w12, v0.b[13]
	umov	w2, v0.b[14]
	umov	w9, v0.b[11]
	umov	w11, v0.b[1]
	umov	w4, v0.b[0]
	umov	w16, v0.b[4]
	add	x3, x3, 768
	add	x1, x1, 512
	add	x12, x12, 256
	umov	w8, v0.b[5]
	umov	w6, v0.b[2]
	umov	w7, v0.b[15]
	add	x9, x9, 768
	add	x2, x2, 512
	add	x11, x11, 256
	ldr	w15, [x20, x3, lsl 2]
	ldr	w10, [x20, x4, lsl 2]
	umov	w13, v0.b[8]
	ldr	w14, [x20, x12, lsl 2]
	umov	w5, v0.b[9]
	ldr	w1, [x20, x1, lsl 2]
	umov	w3, v0.b[6]
	ldr	w12, [x20, x9, lsl 2]
	umov	w4, v0.b[3]
	ldr	w9, [x20, x16, lsl 2]
	eor	w1, w1, w15
	ldr	w2, [x20, x2, lsl 2]
	eor	w10, w10, w14
	ldr	w11, [x20, x11, lsl 2]
	eor	w1, w1, w10
	add	x8, x8, 256
	add	x6, x6, 512
	add	x7, x7, 768
	eor	w2, w2, w12
	fmov	s28, w1
	eor	w1, w9, w11
	eor	w1, w2, w1
	umov	w9, v0.b[12]
	ldr	w2, [x20, x13, lsl 2]
	add	x5, x5, 256
	ldr	w8, [x20, x8, lsl 2]
	ins	v28.s[1], w1
	ldr	w6, [x20, x6, lsl 2]
	add	x3, x3, 512
	ldr	w7, [x20, x7, lsl 2]
	add	x4, x4, 768
	eor	w2, w2, w8
	ldr	w1, [x20, x9, lsl 2]
	eor	w6, w6, w7
	ldr	w3, [x20, x3, lsl 2]
	eor	w2, w2, w6
	ldr	w4, [x20, x4, lsl 2]
	ldr	w5, [x20, x5, lsl 2]
	ins	v28.s[2], w2
	eor	w0, w1, w5
	eor	w1, w3, w4
	eor	w0, w0, w1
	ins	v28.s[3], w0
	eor	v0.16b, v1.16b, v28.16b
	ret

DECL(randomx_program_aarch64_aes_lut_pointers):
	.fill 2, 8, 0


# Input parameters
#
# x0 -> pointer to cache
# x1 -> pointer to dataset memory at startItem
# x2 -> start item
# x3 -> end item

DECL(randomx_init_dataset_aarch64):
	# Save x20 (used as temporary, but must be saved to not break ABI) and x30 (return address)
	stp	x20, x30, [sp, -16]!

	# Load pointer to cache memory
	ldr	x0, [x0]

DECL(randomx_init_dataset_aarch64_main_loop):
	bl	rx_calc_dataset_item
	add	x1, x1, 64
	add	x2, x2, 1
	cmp	x2, x3
	bne	DECL(randomx_init_dataset_aarch64_main_loop)

	# Restore x20 and x30
	ldp	x20, x30, [sp], 16

	ret

DECL(randomx_init_dataset_aarch64_end):

# Input parameters
#
# x0 -> pointer to cache memory
# x1 -> pointer to output
# x2 -> item number
#
# Register allocation
#
# x0-x7 -> output value (calculated dataset item)
# x8 -> pointer to cache memory
# x9 -> pointer to output
# x10 -> registerValue
# x11 -> mixBlock
# x12 -> temporary
# x13 -> temporary

DECL(randomx_calc_dataset_item_aarch64):
rx_calc_dataset_item:
	sub	sp, sp, 112
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, 16]
	stp	x4, x5, [sp, 32]
	stp	x6, x7, [sp, 48]
	stp	x8, x9, [sp, 64]
	stp	x10, x11, [sp, 80]
	stp	x12, x13, [sp, 96]

	ldr	x12, superscalarMul0

	mov	x8, x0
	mov	x9, x1
	mov	x10, x2

	# rl[0] = (itemNumber + 1) * superscalarMul0;
	madd	x0, x2, x12, x12

	# rl[1] = rl[0] ^ superscalarAdd1;
	ldr	x12, superscalarAdd1
	eor	x1, x0, x12

	# rl[2] = rl[0] ^ superscalarAdd2;
	ldr	x12, superscalarAdd2
	eor	x2, x0, x12

	# rl[3] = rl[0] ^ superscalarAdd3;
	ldr	x12, superscalarAdd3
	eor	x3, x0, x12

	# rl[4] = rl[0] ^ superscalarAdd4;
	ldr	x12, superscalarAdd4
	eor	x4, x0, x12

	# rl[5] = rl[0] ^ superscalarAdd5;
	ldr	x12, superscalarAdd5
	eor	x5, x0, x12

	# rl[6] = rl[0] ^ superscalarAdd6;
	ldr	x12, superscalarAdd6
	eor	x6, x0, x12

	# rl[7] = rl[0] ^ superscalarAdd7;
	ldr	x12, superscalarAdd7
	eor	x7, x0, x12

	b	rx_calc_dataset_item_prefetch

superscalarMul0: .quad 6364136223846793005
superscalarAdd1: .quad 9298411001130361340
superscalarAdd2: .quad 12065312585734608966
superscalarAdd3: .quad 9306329213124626780
superscalarAdd4: .quad 5281919268842080866
superscalarAdd5: .quad 10536153434571861004
superscalarAdd6: .quad 3398623926847679864
superscalarAdd7: .quad 9549104520008361294

# Prefetch -> SuperScalar hash -> Mix will be repeated N times

DECL(randomx_calc_dataset_item_aarch64_prefetch):
rx_calc_dataset_item_prefetch:
	# Actual mask will be inserted by JIT compiler
	and	x11, x10, 1
	add	x11, x8, x11, lsl 6
	prfm	pldl1keep, [x11]

	# Generated SuperScalar hash program goes here

DECL(randomx_calc_dataset_item_aarch64_mix):
	ldp	x12, x13, [x11]
	eor	x0, x0, x12
	eor	x1, x1, x13
	ldp	x12, x13, [x11, 16]
	eor	x2, x2, x12
	eor	x3, x3, x13
	ldp	x12, x13, [x11, 32]
	eor	x4, x4, x12
	eor	x5, x5, x13
	ldp	x12, x13, [x11, 48]
	eor	x6, x6, x12
	eor	x7, x7, x13

DECL(randomx_calc_dataset_item_aarch64_store_result):
	stp	x0, x1, [x9]
	stp	x2, x3, [x9, 16]
	stp	x4, x5, [x9, 32]
	stp	x6, x7, [x9, 48]

	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, 16]
	ldp	x4, x5, [sp, 32]
	ldp	x6, x7, [sp, 48]
	ldp	x8, x9, [sp, 64]
	ldp	x10, x11, [sp, 80]
	ldp	x12, x13, [sp, 96]
	add	sp, sp, 112

	ret

DECL(randomx_calc_dataset_item_aarch64_end):

# ══════════════════════════════════════════════════════════════════════
# Track D1 (docs/plans/track-d1-superscalar-interleave-plan-20260728.md):
# 2-way interleaved dataset-item derivation.
#
# NEW, SEPARATE entry point -- parallel to, and does NOT replace, modify,
# or share any register with, randomx_calc_dataset_item_aarch64 above.
# That function, rx_calc_dataset_item_prefetch/_mix/_store_result, and
# every existing caller (randomx_program_aarch64_vm_instructions_end_light,
# _end_hybrid, and the dead-code randomx_init_dataset_aarch64_main_loop)
# are completely untouched by this block.
#
# This entry is ALSO not reached by generateSuperscalarHash()'s existing
# copy-and-splice mechanism, is NOT wired into the JIT-emitted main VM
# program template (randomx_program_aarch64_main_loop) or its _end_light/
# _end_hybrid exits, and is NOT called from anywhere in the live mining
# path. It is only ever reached by the new, standalone
# src/jit_dataset_2way.cpp module's own memcpy-and-splice logic (a
# SEPARATE JIT buffer from JitCompilerA64::code), which builds a per-seed
# interleaved-derivation function for testing/benchmarking two dataset
# items at once. Track D1's plan explicitly scopes wiring this into the
# live per-hash call sites as a distinct, not-yet-attempted follow-on
# (see the plan's Step 5) -- so this cannot yet suffer the specific
# failure class documented in
# docs/experiments/track-b-inline-hit-path-attempt.md (a hidden
# `ldr x21, <literal>` inside the MAIN LOOP template clobbering a
# register a new template entry assumed was live): there is no caller
# template here to clobber anything, because there is no caller yet.
#
# extern "C" void randomx_calc_dataset_item_aarch64_2way(
#     const void* cache,          // x0
#     void* outA, uint64_t itemA, // x1, x2
#     void* outB, uint64_t itemB) // x3, x4
#
# ── FULL REGISTER CONTRACT (read this before extending or calling this
#    function from anywhere new) ──────────────────────────────────────
#
# Inputs (standard AAPCS64 argument registers x0-x4; each is relocated to
# a persistent register -- see below -- in the first few instructions,
# then dead/reused):
#   x0 = cache pointer            -> copied to x8  (shared by both
#                                     streams; cache access is read-only,
#                                     so sharing one pointer register
#                                     between streams introduces no
#                                     aliasing hazard)
#   x1 = output pointer, stream A -> copied to x9 before being clobbered
#                                     as rl_A[1]
#   x2 = item number,   stream A  -> consumed directly by the rl_A[0]
#                                     `madd`, then reused in place as
#                                     rl_A[2] (identical technique to the
#                                     existing single-stream function
#                                     above: the input register becomes
#                                     a working register once its input
#                                     value has been consumed)
#   x3 = output pointer, stream B -> copied to x27 before being
#                                     clobbered as rl_A[3] (stream A's
#                                     setup runs first and physically
#                                     overwrites x3)
#   x4 = item number,   stream B  -> copied to x16 before being
#                                     clobbered as rl_A[4]; x16's copy is
#                                     then consumed by the rl_B[0] `madd`
#                                     and left dead (rl_B[2] is NOT
#                                     aliased to x16 the way rl_A[2]/x2
#                                     is -- x16 has a second job as
#                                     stream B's temp/tmp_reg register
#                                     from the very next round onward,
#                                     so its item-number value is
#                                     deliberately not preserved further)
#
# Stream A -- IDENTICAL physical registers to the existing single-stream
# randomx_calc_dataset_item_aarch64 above (deliberate: stream A's
# arithmetic is byte-for-byte the same sequence as the proven
# single-stream path, just missing that function's own x0-x13
# save/restore, which this function does not need -- see "ABI" below):
#   x0-x7  = rl_A[0..7]   (working/output registers)
#   x8     = cache pointer            (SHARED with stream B, read-only)
#   x9     = output pointer, stream A
#   x10    = registerValue, stream A
#   x11    = mixBlock pointer, stream A
#   x12    = temp A / tmp_reg for instruction emission
#   x13    = temp A (mix-step second XOR operand only)
#
# Stream B -- new registers. Working set extends into the callee-saved
# range x19-x26; the four "helper" roles use x27/x28 (callee-saved) plus
# x16/x17/x14 (caller-saved scratch -- safe to use with no save/restore
# because this is a leaf function containing no internal `bl`, so
# nothing downstream can observe them as clobbered mid-call the way a
# real nested call could). x15 and x18 are deliberately left completely
# unused: x15 as a spare in case a future revision needs one more temp,
# x18 because it is the reserved platform register on some AArch64 ABIs
# (notably Apple/Darwin, which this file's DECL() macro already
# special-cases) even though armrx only targets AArch64 Linux -- costs
# nothing to avoid it here.
#   x19-x26 = rl_B[0..7]   (working/output registers)
#   x27     = output pointer, stream B
#   x28     = registerValue, stream B
#   x17     = mixBlock pointer, stream B
#   x16     = temp B / tmp_reg for instruction emission
#   x14     = temp B (mix-step second XOR operand only)
#
# ABI: this is a plain, AAPCS64-conforming leaf function, callable via an
# ordinary `bl`/`blr` from arbitrary C++ (specifically: the differential
# test and any future benchmark harness in jit_dataset_2way.cpp) with NO
# special caller-side bookkeeping required, unlike
# randomx_calc_dataset_item_aarch64 above (which additionally preserves
# x0-x13 as a defensive convention specific to its own JIT-splice caller
# site's needs). This function saves/restores every callee-saved
# register it touches (x19-x28, 5 stp/ldp pairs) and nothing else --
# x29/x30 are untouched (no frame record needed: leaf function, no
# internal calls). Every other register (x0-x18) is caller-saved by
# standard AAPCS64 semantics and this function makes no extra promise
# about them, exactly matching how a normal call already works.
#
# This prologue ends with an explicit `b rx_calc_dataset_item_2way_prefetch`
# (see the comment right before it, below) that jumps over the 8 literal
# constants both streams' rl[0] computations read -- unlike the
# single-stream prologue's own trailing branch (a same-address, effectively
# no-op fallthrough there, since nothing sits between it and _prefetch),
# this one is load-bearing: omitting it was tried first and produced a
# real on-device SIGSEGV (PC landed inside the literal pool, decoded as
# garbage instructions) during this feature's own differential testing.
# After that branch lands at _2way_prefetch, the REST of the transition
# into round 0 (the compile-time AND-mask instructions) is genuinely
# sequential C++ emission with no branch involved -- jit_dataset_2way.cpp
# emits them at the current codePos immediately after memcpy'ing this
# whole prologue chunk, then memcpy's [_2way_prefetch, _2way_mix) right
# after that.
DECL(randomx_calc_dataset_item_aarch64_2way):
rx_calc_dataset_item_2way:
	sub	sp, sp, 80
	stp	x19, x20, [sp]
	stp	x21, x22, [sp, 16]
	stp	x23, x24, [sp, 32]
	stp	x25, x26, [sp, 48]
	stp	x27, x28, [sp, 64]

	ldr	x12, superscalarMul0_2way
	mov	x8, x0
	mov	x9, x1
	mov	x10, x2
	mov	x27, x3
	mov	x16, x4

	# rl_A[0] = (itemA + 1) * superscalarMul0
	madd	x0, x2, x12, x12
	ldr	x12, superscalarAdd1_2way
	eor	x1, x0, x12               // rl_A[1]
	ldr	x12, superscalarAdd2_2way
	eor	x2, x0, x12               // rl_A[2]
	ldr	x12, superscalarAdd3_2way
	eor	x3, x0, x12               // rl_A[3]
	ldr	x12, superscalarAdd4_2way
	eor	x4, x0, x12               // rl_A[4]
	ldr	x12, superscalarAdd5_2way
	eor	x5, x0, x12               // rl_A[5]
	ldr	x12, superscalarAdd6_2way
	eor	x6, x0, x12               // rl_A[6]
	ldr	x12, superscalarAdd7_2way
	eor	x7, x0, x12               // rl_A[7]

	# rl_B[0] = (itemB + 1) * superscalarMul0   (itemB copy is in x16)
	ldr	x17, superscalarMul0_2way
	madd	x19, x16, x17, x17
	mov	x28, x16                  // registerValue_B = itemB (matches
	                                  // stream A's x10 = itemA convention)
	ldr	x17, superscalarAdd1_2way
	eor	x20, x19, x17             // rl_B[1]
	ldr	x17, superscalarAdd2_2way
	eor	x21, x19, x17             // rl_B[2]
	ldr	x17, superscalarAdd3_2way
	eor	x22, x19, x17             // rl_B[3]
	ldr	x17, superscalarAdd4_2way
	eor	x23, x19, x17             // rl_B[4]
	ldr	x17, superscalarAdd5_2way
	eor	x24, x19, x17             // rl_B[5]
	ldr	x17, superscalarAdd6_2way
	eor	x25, x19, x17             // rl_B[6]
	ldr	x17, superscalarAdd7_2way
	eor	x26, x19, x17             // rl_B[7]

	# Unlike the single-stream prologue's trailing branch (which really is
	# a same-address fallthrough, since nothing sits between it and
	# _prefetch there), THIS branch is load-bearing: the 8 literal
	# constants immediately below are 64 bytes of DATA sitting between
	# here and _2way_prefetch, and without an explicit jump execution
	# would fall through into them as if they were instructions. (Found
	# by exactly that crash -- PC landing inside this literal pool --
	# during on-device differential testing; see
	# docs/plans/track-d1-superscalar-interleave-plan-20260728.md's
	# implementation notes.) Both this branch and its target are inside
	# the SAME contiguous memcpy'd prologue chunk (jit_dataset_2way.cpp's
	# first copy, [randomx_calc_dataset_item_aarch64_2way,
	# randomx_calc_dataset_item_aarch64_2way_prefetch)), so its
	# PC-relative offset is trivially preserved under relocation -- no
	# textual-adjacency reasoning required, unlike a branch whose target
	# lives in a separately-copied chunk.
	b	rx_calc_dataset_item_2way_prefetch

superscalarMul0_2way: .quad 6364136223846793005
superscalarAdd1_2way: .quad 9298411001130361340
superscalarAdd2_2way: .quad 12065312585734608966
superscalarAdd3_2way: .quad 9306329213124626780
superscalarAdd4_2way: .quad 5281919268842080866
superscalarAdd5_2way: .quad 10536153434571861004
superscalarAdd6_2way: .quad 3398623926847679864
superscalarAdd7_2way: .quad 9549104520008361294

# Per-round prefetch wrapper (doubled). The compile-time AND-mask
# instructions (mask depends on RANDOMX_CACHE_ACCESSES config, same as
# the single-stream path) are emitted by the C++ generator immediately
# before this copied chunk, one per stream:
#   and x11, x10, CacheSize/CacheLineSize-1     (stream A, emitted by C++)
#   and x17, x28, CacheSize/CacheLineSize-1     (stream B, emitted by C++)
# then this chunk computes both prefetch addresses and issues both
# prefetches:
DECL(randomx_calc_dataset_item_aarch64_2way_prefetch):
rx_calc_dataset_item_2way_prefetch:
	add	x11, x8, x11, lsl 6
	prfm	pldl1keep, [x11]
	add	x17, x8, x17, lsl 6
	prfm	pldl1keep, [x17]

	# Generated 2-way interleaved SuperScalar hash program goes here
	# (jit_dataset_2way.cpp emits each scheduled instruction twice, once
	# per stream's disjoint register set, in the SAME order
	# JitCompilerA64::computeSuperscalarEmitOrder() already produces for
	# the single-stream path -- no new hazard analysis, per the plan.)

DECL(randomx_calc_dataset_item_aarch64_2way_mix):
rx_calc_dataset_item_2way_mix:
	ldp	x12, x13, [x11]
	eor	x0, x0, x12
	eor	x1, x1, x13
	ldp	x12, x13, [x11, 16]
	eor	x2, x2, x12
	eor	x3, x3, x13
	ldp	x12, x13, [x11, 32]
	eor	x4, x4, x12
	eor	x5, x5, x13
	ldp	x12, x13, [x11, 48]
	eor	x6, x6, x12
	eor	x7, x7, x13

	ldp	x16, x14, [x17]
	eor	x19, x19, x16
	eor	x20, x20, x14
	ldp	x16, x14, [x17, 16]
	eor	x21, x21, x16
	eor	x22, x22, x14
	ldp	x16, x14, [x17, 32]
	eor	x23, x23, x16
	eor	x24, x24, x14
	ldp	x16, x14, [x17, 48]
	eor	x25, x25, x16
	eor	x26, x26, x14

# After this label, the C++ generator emits two register-move
# instructions per round (registerValue update, one per stream):
#   mov x10, x{A-stream physical register for prog.address_register()}
#   mov x28, x{B-stream physical register for prog.address_register()}
# before either falling through to the next round's compile-time
# AND-mask pair (see above) or, after the final round, falling through
# to the epilogue immediately below.

DECL(randomx_calc_dataset_item_aarch64_2way_store_result):
rx_calc_dataset_item_2way_store_result:
	stp	x0, x1, [x9]
	stp	x2, x3, [x9, 16]
	stp	x4, x5, [x9, 32]
	stp	x6, x7, [x9, 48]

	stp	x19, x20, [x27]
	stp	x21, x22, [x27, 16]
	stp	x23, x24, [x27, 32]
	stp	x25, x26, [x27, 48]

	ldp	x19, x20, [sp]
	ldp	x21, x22, [sp, 16]
	ldp	x23, x24, [sp, 32]
	ldp	x25, x26, [sp, 48]
	ldp	x27, x28, [sp, 64]
	add	sp, sp, 80

	ret

DECL(randomx_calc_dataset_item_aarch64_2way_end):
