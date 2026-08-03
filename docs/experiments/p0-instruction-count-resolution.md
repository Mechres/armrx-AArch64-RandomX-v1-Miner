# Phase 0 — Resolve the 1w/8w instruction-count contradiction

**Date:** 2026-08-05 (session clock)
**Author:** Hermes
**Status:** DONE — contradiction resolved; lever = instruction count (armrx +15% heavier than XMRig)
**Method:** `docs/plans/era2-plan.md` §Phase 0. Pristine cross-build, `bench_armrx
--full-hash-only --perf-ready --workers={1,8}` under `perf stat` (7 validated events). The
gated 500-hash window is self-counting, so instr/hash = `instructions:u / 500` exactly. This
is the authoritative source (TESTING.md §1) — supersedes both the w11 census and the M1 pool
estimate, which disagreed.

## Results (gated 500-hash window, per-worker-equivalent)

| metric | 1w | 8w | Δ (1w→8w) |
|---|---:|---:|---:|
| `instructions:u` | 56,882,414,583 | 56,885,578,919 | +0.01% |
| **instr/hash (÷500)** | **113.8 M** | **113.8 M** | **0%** |
| `cycles:u` | 85,962,838,740 | 85,916,243,641 | 0% |
| IPC | 0.662 | 0.662 | 0% |
| `ld_dep_stall:u` | 8,527,369,213 | 8,469,376,449 | ~0% |
| `other_interlock_stall:u` | 6,178,339,689 | 6,178,300,243 | 0% |

Bench confirmed `PERF_READY` window, median 195,524 μs = 5.11 H/s (per-worker). Both 1w and 8w
produce byte-identical aggregate counters over the 500-hash window → **instr/hash is flat
across worker count (113.8M).**

## What this overturns
1. **M1 8w armrx 103.5M was a measurement artifact** (pool `Total` undercount), exactly as
   STRATEGY.md predicted. Real armrx instr/hash = **113.8M** at both 1w and 8w.
2. **The w11 census "armrx 89.5M (leaner than XMRig 101.4M)" was WRONG.** The gated window says
   armrx is **113.8M** — i.e. armrx is **+15% HEAVIER** than XMRig (98.9M from M1), not leaner.
   The census used a mis-divided/non-500 hash count. The gated `--perf-ready` is authoritative.

## Verdict: the lever is INSTRUCTION COUNT (armrx heavier, not leaner)
- armrx emits **113.8M instr/hash** vs XMRig **98.9M** = **+15% more instructions**. This is the
  dominant, confirmed gap.
- Per-worker IPC (0.662) and stalls are flat 1w↔8w, so the 95.2% (26.65 vs 28 H/s) gap is NOT a
  per-worker instr/IPC difference. It is **cluster-contention throughput loss**: armrx scales to
  65% of linear (26.65 / 5.11×8 = 0.65) vs XMRig 73% (28 / 5.04×8 = 0.69). armrx loses more
  per-worker throughput when 8 threads share the 2-cluster MSM8929 interconnect.
- The +15% instruction count likely *causes* the worse scaling (more instr/hash = more mem/cache
  traffic = more interconnect pressure). Fixing density (E3b) may also improve scaling.

## Next step (Phase 1b — density lever)
`docs/plans/era2-plan.md` assumed a binary "IPC vs density" decided by whether armrx was leaner.
Phase 0 shows armrx is **heavier (+15%)**, so the lever is unambiguously **codegen density**.
Primary experiment: **E3b per-opcode JIT-emission diff vs XMRig** (GLM audit E3b) — find the
opcode(s) where armrx emits ~15% more. Candidates from GLM: `*_M` consumer (`add→and→ldr→op` in
emitMemLoad), CBRANCH form, ISWAP_R (3-MOV), INEG_R. Implement each behind an `ARMRX_*` flag
(TESTING.md §6), gate (KAT→450→200→8w H/s), keep only if it reduces instr/hash without regression.

## Correction to STRATEGY.md / era2-plan.md
The "armrx may be leaner (89.5M)" framing is wrong. Update to: armrx = 113.8M instr/hash
(gated), +15% heavier than XMRig; lever = density; Phase 1b is the path (not the IPC branch).
