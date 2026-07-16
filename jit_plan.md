
JIT Compiler Improvement Plan for aarch64-randomx
Background
Current state: 22.2 H/s total on 8× Cortex-A53 (big: 3.66 H/s, LITTLE: 1.88 H/s). JIT execution is 98.4% of per-hash time. The hot loop is the 2048-iteration randomx_program_aarch64_main_loop in jit_compiler_a64_static.S, plus the JIT-emitted random-program body (~256 ops) per iteration.
Cortex-A53 is in-order, dual-issue with ~4-cycle L1 load-use latency. The biggest wins will come from:
1. Reducing load-use stalls (between ldp and dependent eor)
2. Reducing instruction count (replacing multi-instruction sequences with single instructions)
3. Hiding long-latency FP ops (FDIV, FSQRT) behind other work
Opportunities, ranked by impact-to-risk ratio
Tier A — High impact, low risk (recommended)
A1. ubfx for scratchpad address computation
Location: src/jit_compiler_a64_static.S:213-217
Today: 3 instructions to compute both spAddr0 and spAddr1:
lsr  x20, x10, 32      ; (1) extract high half
and  w16, w10, MASK    ; (2) mask low half
and  w17, w20, MASK    ; (3) depends on (1)
The high half has a 3-instruction serial dependency chain before add x17, x17, x2 at line 222 can issue, blocking the prefetch at line 226.
Proposed: Replace with 2 ubfx instructions:
ubfx x16, x10, #0,  #21    ; spAddr0 = (spMix1 >>  0) & ((1<<21)-1)
ubfx x17, x10, #32, #21    ; spAddr1 = (spMix1 >> 32) & ((1<<21)-1)
- ubfx (unsigned bitfield extract) combines lsr + and into one instruction. Both ubfx depend only on x10, so they dual-issue.
- Saves 1 instruction and ~1 cycle on the critical path to the spAddr1 prefetch + load.
- Requires patching the JIT compiler's generateProgram site at jit_compiler_a64.cpp:165-168 to emit ubfx immediates (lsb, width) rather than the and immediate encoding. The 21-bit width comes from the L3 mask width (Log2(RANDOMX_SCRATCHPAD_L3) - 3 = 21).
- Risk: Low. ubfx is baseline A64. Functional equivalence is exact.
- Expected impact: +0.5-1% h/s
A2. Fold h_FSWAP_R from 3 instructions to 1
Location: src/jit_compiler_a64.cpp:959-974
Today: 3 MOV_VREG_EL instructions (mov lane from src→tmp, then src→src with index 1, then tmp→src):
mov  v28.16b[1], vN.16b[1]   ; (1) tmp.hi = dst.hi
mov  vN.16b[1],  vN.16b[0]   ; (2) dst.hi = dst.lo
mov  vN.16b[0],  v28.16b[0]  ; (3) dst.lo = old dst.hi
Net effect: swap two 64-bit lanes of vN.
Proposed: Single ext instruction:
ext  vN.16b, vN.16b, vN.16b, #8
- ext extracts the lane at byte offset 8 from the concatenation of (vN, vN), producing a lane-swap.
- Alternative equivalent: rev64 vN.2d, vN.2d (also single instruction).
- Saves 2 instructions per FSWAP_R. Frequency = 4/256 = 1.6%.
- Risk: Very low. Drop-in replacement. C++ codegen needs a single emit32(0x6E60FC00 | dst | (dst << 5) | (dst << 16) | (8 << 10)) (the ext encoding).
- Expected impact: +0.5% h/s
A3. NEON load for emitMemLoadFP (FP memory operand loads)
Location: src/jit_compiler_a64.cpp:591-624
Today: 7-9 instructions per FP memory load (used by FADD_M, FSUB_M, FDIV_M):
ldr/ldpsw tmp_reg, tmp_reg+1, [addr]   ; (1) load 2× 32-bit
ins  vN.d[0], tmp_reg                   ; (2) move to FP vector low
ins  vN.d[1], tmp_reg+1                 ; (3) move to FP vector high
scvtf vN.2d, vN.2d                      ; (4) int→fp convert
Plus the add+and chain for address computation. Total: ~8-10 instructions per FP load.
Proposed: Use NEON directly:
ld1  {v28.2s}, [addr]                  ; (1) direct load into FP register, two 32-bit lanes
sxtl v28.2d, v28.2s                     ; (2) sign-extend 32→64-bit (new 128-bit lanes)
scvtf v28.2d, v28.2d                    ; (3) int→fp convert
- Saves the 2 ins instructions and uses ld1 (no ldpsw).
- sxtl is the "sign extend long" NEON op — single instruction, faster than 2× ins.
- Frequency: FADD_M + FSUB_M + FDIV_M = 5 + 5 + 4 = 14/256 = 5.5%.
- Saves 2 instructions × 14 = ~28 instructions per program iteration.
- Risk: Low. NEON ld1 {vN.2s} is baseline A64. Need to verify sign-extension matches RandomX spec (RandomX treats spAddr1 scratchpad reads as signed 32-bit integers converted to FP64).
- Expected impact: +1-2% h/s
A4. Software pipeline: prefetch next iteration's scratchpad earlier
Location: src/jit_compiler_a64_static.S:225-227 (current prefetches), plus new prefetches near end of iteration (around line 460)
Today: All 3 prefetches fire at iteration start (lines 225-227), only ~5-25 instructions before the dependent loads. This hides L1 latency but not L2/memory latency.
Proposed: Issue next iteration's scratchpad prefetches at the end of the current iteration:
;jit-compiled program body executes
;... near end of iteration, after spMix1 update:
; (with offsets known from following iteration's x10)
prfm pldl1keep, [next_x16]   ; spAddr0 for next iter
prfm pldl1keep, [next_x17]   ; spAddr1 for next iter
prfm pldl1keep, [next_x17, 32]
But next iteration's x10 (= spMix1) depends on the JIT-emitted eor x10, reg0, reg1 at line 366. The JIT compiler already knows readReg0 and readReg1 at compile time, so we can emit the and x16, x10, MASK ; add x16, x16, x2; prfm ... right after the spMix1 update.
- Saves ~10-30 cycles of latency per L1 miss (the prefetch has the entire program body + tail + branch-back to hide latency — about 600 instructions).
- Risk: Medium. Requires touching both static assembly and JIT codegen; need to recompute the next-iteration addresses between iterations and store them in registers not needed by the body. Currently, all caller-saved registers are committed, so this needs either:
- Hoisting to free x16/x17 (recomputing them at top of iteration rather than at end of previous), or
- Using x18 (platform reg — NOT safe on Linux).
- Suggested approach: compute next iter's spAddr registers at the end of the iteration into x16/x17 themselves, then the loop top jumps past the lsr/and setup. So the loop entry becomes "post-prologue" — branch directly to the prefetches.
- Expected impact: +2-4% h/s if done cleanly
A5. Reorder ldp/eor pairs to overlap load-use latency
Location: src/jit_compiler_a64_static.S:230-241
Today: Each ldp is immediately followed by the eor that consumes it, causing 4-cycle stalls:
ldp  x20, x19, [x16]      ; stall
eor  x4, x4, x20          ; uses x20
eor  x5, x5, x19          ; uses x19
ldp  x20, x19, [x16, 16]  ; stall (waits for previous eors? actually no — ldp can issue in parallel with eors?)
Actually, on A53 the load pipe and ALU pipe are separate, so ldp at line 233 can issue while eor at line 231-232 are still in ALU. The true stall is only the first ldp→eor pair. The bigger issue is the 8-cycle serial chain of:
230: ldp → 231: eor (wait for x20 from L1)
233: ldp → 234: eor (can issue while 231 eors complete)
236: ldp → 237: eor
239: ldp → 240: eor
Assuming L1 hits, each ldp has 4 cycles latency. The 4 ldp pairs issue back-to-back, and the dependent eors track through. Effective: ~16 cycles for the 4 loads + 8 eors in parallel.
Proposed: Interleave loads of the next group earlier. Reorder to issue ldp x16, 32 before the eor of ldp x16, 0:
ldp  x20, x19, [x16]       ; load 0
ldp  x20', x19', [x16, 16] ; load 1 — can issue during load 0 latency (load pipe)
eor  x4, x4, x20'          ; wait for x20' (this is the dependent operand)
eor  x5, x5, x19'          ; ... but we still need separate temp regs
Hmm, this needs extra registers or restructuring. Better approach: swap F/E FP load group with the integer XOR group so FP loads (which have longer latency) start before integer XORs occupy the ALU.
Actually, the simplest win: insert the FP ldpsw at line 244 earlier (before the int ldp at line 236-241 completes):
ldp  x20, x19, [x16]       ; 230 — start int load 0
ldpsw x20', x19', [x17, 24]; 253 — start FP load 3 (load pipe overlaps)
eor  x4, x4, x20           ; 231 — ALU uses load 0
ldp  x20, x19, [x16, 16]   ; 233
eor  x6, x6, x20
...
This requires using a temp register that doesn't collide with the existing x19/x20 pairs. All available registers are committed, so this needs the prologue to be restructured with temporary use of x11-x30 (literals — partially avoidable by re-using immediates that are still live in memory rather than registers).
Risk: Medium-high. Requires careful register juggling. Restructuring load order may break the dependency chain.
- Expected impact: +2-3% h/s
Tier B — High impact, medium-high risk
B1. Reciprocal-estimate FDIV (Newton-Raphson refinement)
Location: src/jit_compiler_a64.cpp:1037-1053
Today: Native fdiv v_dst.2d, v_dst.2d, v28 — 14-23 cycle latency on A53.
Frequency: FDIV_M = 4/256 = 1.6% per program iteration.
Proposed: Use frecpe+frecps×2 to compute the reciprocal then multiply:
frecpe  v0.2d, v28.2d          ; (1) initial reciprocal estimate [5 cyc]
frecps  v1.2d, v0.2d, v28.2d   ; (2) refine [5 cyc]
fmul    v_dst, v_dst, v1.2d     ; (3) y = x * (1/d) [5 cyc]
- Total ~15 cycles, but all three are pipelined (different source dependencies), so back-to-back FDIVs from different program iterations can overlap. In contrast, native fdiv blocks the FP pipe for 14 cycles per instruction.
- Same for FSQRT_R using frsqrte+frsqrts×2 + fmul.
- Risk: HIGH. RandomX requires IEEE-compliant division. Newton-Raphson frecpe is only ~8-bit accurate; refinement steps bring it to ~12 bits — not full double-precision. The result will diverge from IEEE 754 division, likely breaking hash parity against the reference.
However, XMRig's AArch64 JIT reportedly uses this trick (need to verify with their source). If XMRig's output matches the RandomX reference, then there's a subtlety: RandomX v1 FDIV's "result" is exactly the result of convertible_to_sp_with_rounding(x / y) — the FDIV output is converted to FP32 with the current rounding mode! That means the result has only ~24-bit precision. Newton-Raphson with 1-2 refinements gives ~12+ bits, but adding one more Newton-Raphson step reaches the required precision.
But that's more research needed. This is the canonical XMRig optimization.
- Expected impact: +3-5% h/s if correctness can be verified, otherwise broken hashes.
- Status: MUST be deferred until we read XMRig's source and validate against test_mining KAT vectors.
B2. Conditional-replace (branchless) CBRANCH
Location: src/jit_compiler_a64.cpp:1062-1087
Today: add dst, dst, imm; tst dst, MASK; beq back_to_target. Frequency = 25/256 = 9.8%.
Conditional branches in the JIT'd body are mispredicted ~20% on A53's 2-bit predictor → ~5 cycles × 10 mispredictions/iteration = 50 cycles/iteration of penalty.
Proposed: When the branch is "almost certainly taken" (backward by a small offset), the A53 hardware predictor handles it well; no change needed. But for the small-offset forward form (target is slightly later in the JIT buffer), we can use cbnz with a predicted-not-taken path.
Alternative: leave it. The branch frequency is high but the predictor is decent.
- Risk: Touching CBRANCH breaks hash parity unless exact semantics preserved.
- Expected impact: +1-2% h/s only if branch prediction is actually a measurable bottleneck. Need profiling first.
- Status: Lower priority until we have AArch64 perf counters showing branch-mispredict stalls.
Tier C — Lower impact, low risk (free wins)
C1. Remove redundant stp x16, x17 save at function entry
Location: src/jit_compiler_a64_static.S:125 (and the matching ldp at exit)
Issue: x16 (IP0) and x17 (IP1) are caller-saved, so saving them at JIT entry is unnecessary.
Proposed: Remove the stp x16, x17, [sp] at entry and the matching ldp at exit.
- Risk: Very low — caller-saved registers never need preservation by a leaf function. Need to verify the JIT'd program doesn't make any calls (it doesn't — the body is inlined).
- Expected impact: Trivial — affects function entry/exit only, not per-iteration. Not worth doing alone, but free if combined with other static.S work.
C2. Re-evaluate dataset prefetch hint
Location: src/jit_compiler_a64_static.S:341
Issue: This prfm pldl1keep, [x20] prefetches the next iteration's dataset line into L1. But the next iteration's scratchpad loads (32 KB L1, 4-way) will likely evict the prefetched dataset line before it's used at the next iteration's ldp x20, x19, [x10] (line 351).
The earlier 2026-07-14 changelog changed pldl2strm → pldl1keep for "consistency" with the dataset-item prefetch at line 884. But the use cases are different — line 884 prefetches for use ~10 instructions later (truly L1-targeted), while line 341 prefetches for ~80 instructions later (likely L1-evicted before use).
Proposed: A/B test: change line 341 back to pldl2keep (keep in L2 until use). Then the next iter's ldp at line 351 will hit L2 (4 cycles) rather than evicting scratchpad from L1.
- Risk: Very low. May need empirical verification on AArch64 hardware.
- Expected impact: Unknown — could be +0.5% or -0.5%. Worth A/B benchmarking.
C3. Avoid the rbit+msr fpcr in CFROUND
Location: src/jit_compiler_a64.cpp:1110-1115
Today: bfi x8, x20, 40, 2; rbit x20, x8; msr fpcr, x20 — the rbit (bit-reverse) is needed because x8 stores FPCR in a bit-reversed layout (per jit_compiler_a64_static.S:80).
Proposed: Store FPCR in canonical (non-reversed) layout, eliminating the rbit. Requires touching jit_compiler_a64_static.S:80 and the JIT compiler's fpcr handling.
- Risk: Medium (could break FP rounding mode semantics).
- Expected impact: CFROUND freq = 1/256 — saves 1 cycle × 1/256 × 2048 iterations = ~8 cycles/hash, negligible (0.04%). Not worth the risk.
Tier D — Major architectural changes (out of scope for this pass)
- Cross-instruction register coalescing (e.g., merge consecutive IADD_RS dst, src1 and IXOR_R dst, src2 into a single eor + add chain with reduced register pressure): requires adding a peephole pass over the generated program before emission. Significant engineering effort.
- RandomX v2 enablement: v2 uses hardware AES for the FE-mix (jit_compiler_a64_static.S:382-448) and has different memory access patterns. Changing consensus rules — only relevant if the target network migrates.
- Software-pipelined main loop: unroll the 2048-iteration loop by 2 and pipeline iterations to fully overlap loads/ALU ops. Major static.S rewrite.
Recommended execution order
Suggested batching with verification between each batch:
Batch	Items	Why
1	A1 (ubfx), A2 (FSWAP_R ext)	Pure codegen simplifications; small, well-defined impact; easy to verify correctness with ctest
2	A3 (NEON ld1+sxtl for FP loads)	Bigger codegen fix, ~2% h/s potential
3	C2 (dataset prefetch A/B test)	Falls out of batch 2; quick measurement
4	A4 (software pipelining: prefetch next iter earlier)	Mild structural change in static.S + codegen
5	A5 (load interleaving for ALU overlap)	Requires deeper restructuring
6	B1 (Newton-Raphson FDIV/FSQRT)	Requires research — read XMRig's AArch64 source, verify hash parity, then implement only if KAT tests pass
Expected cumulative impact of Batches 1-3 (low-risk work alone): ~+3-4% h/s (22.2 → ~23 H/s).
With Batches 4-5 added: ~+5-7% h/s (~23.5 H/s).
With Batch 6 (if it verifies): ~+8-12% h/s (~24-25 H/s)