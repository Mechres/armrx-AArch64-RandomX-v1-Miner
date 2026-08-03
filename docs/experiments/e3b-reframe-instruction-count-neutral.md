# E3b reframe — instruction-count reduction is 8w-NEUTRAL; the gap is contention IPC

**Date:** 2026-08-05 (session clock)
**Author:** Hermes
**Status:** DONE — redirects Era II Phase 1b away from instruction-count cuts

## What we traced (before editing anything)
The plan was to implement E3b's over-emitted opcodes behind `ARMRX_*` flags. Tracing the two
picked "safe" targets (`h_CBRANCH`, `h_CFROUND`) plus the dominant `*_M` family hit a wall:

### h_CBRANCH = 5 instr is INTENTIONAL (not over-emission)
`jit_compiler_a64.cpp:2006-2019` — the branchless `bne +8; b target` form exists because a
naive `beq target` (backward) would mispredict **99.6%** of the time (RandomX CBRANCH is taken
only ~0.4%). The 5-instr form trades instruction count for mispredict avoidance. Tightening it
→ reintroduces mispredicts → **IPC hit**. This is exactly the trap we must avoid: the real gap
is IPC, not instruction count. **CBRANCH is NOT a safe win.**

### h_CFROUND = 4 instr is NEGLIGIBLE
Rare opcode (~0.4% frequency per E3b occ counts). Even 4→2 saves <0.01% of instructions. Not worth a flag + gate.

### The `*_M` family and the superscalar `*_C*` constants are DELIBERATE PADDING
`emitCpoolImmediate` (`jit_compiler_a64.cpp:1327-1335`) documents that the 3-instr `MOVZ+MOVK`
form for C* immediates was chosen in **E24** because the denser 2-instr LDR-pool form exposed the
Cortex-A53's 4-cycle MAC interlock (consecutive multiplies 2 instructions apart → saturated
`other_interlock_stall`). E24 measured **+7.1% H/s (4.77→5.11)** from the padding.

**And the E24 A/B (`ab-e24-8w.md`, already run this session) proved removing that padding =
0% 8w H/s change:** instr 103.8 vs 103.5M, IPC 0.599 vs 0.598 — statistically identical.
So the "over-emission" E3b flagged on the `*_M`/C* paths is the *same deliberate, IPC-preserving
padding*, and cutting it does **not** move 8w H/s.

## Conclusion — E3b's premise is wrong for our metric
Phase 0 resolved the lever to "instruction count" because armrx (113.8M) is +15% heavier than
XMRig (98.9M). But **E24's A/B shows instruction-count cuts are 8w-NEUTRAL** — the excess is
padding that buys IPC. Meanwell, Phase 0's own finding (per-worker IPC flat 1w↔8w, armrx scales
to 65% of linear vs XMRig 73%) says the 8w gap is **cluster-contention throughput loss**, not
per-worker instruction count.

**Therefore: trimming instructions will not close the 26.65→28 H/s gap.** The lever is
**per-worker IPC under 8-worker shared-interconnect contention**. E3b's per-opcode map is still
useful as a *clean-room census* of where armrx spends instructions, but it is NOT an optimization
to-do list — most of the "excess" is proven IPC-preserving padding.

## What to do instead (next axis)
1. **Measure 8w per-worker IPC under contention directly**, vs XMRig, with the same gated
   `--perf-ready --workers=8` window + `perf` (Phase 0 used 1 aggregate window; now correlate
   per-worker IPC with `cache-misses`, `l2d_cache_refill`, `bus_access`, `bus_cycles` — the
   interconnect-pressure events). Hypothesis: armrx's 15% higher instruction count means 15%
   more cache/mem traffic per hash → 15% more interconnect contention → the 65%-vs-73% scaling gap.
   If confirmed, the *real* win is reducing **memory traffic per hash** (scratchpad access
   pattern, dataset cache locality), NOT instruction count.
2. Keep the `ARMRX_*` flag pattern ready, but only for changes that move **8w H/s + PMU**, not
   for instruction-count cosmetics (E24 A/B is the precedent: measure the flag's 8w effect, not
   its instr delta).

## Gate status (unchanged, still required for any real change)
`test_jit_scheduler_stress` (450 pairs, TIMEOUT 2400) + `test_jit_superscalar_scheduler_stress`
(200 pairs) are real `add_test` targets, cross-buildable. Any emission change must pass KAT 16/16
→ 450 → 200 → 8w H/s + PMU. (No change made this session — reframe only.)
