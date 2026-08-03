# Strategy — armrx Performance Program

## Era I — "The Stall Mirage" (closed)

*Why the name:* for ~2 weeks we chased `ld_dep_stall` / the `*_M` load-use bubble across the
scratchpad path on an in-order Cortex-A53. It looked like the gap (E19: armrx had +63%
`ld_dep_stall` growth 1w→8w; the E26 hoist was built to hide it). Then **M1 measured XMRig's
8w side and found XMRig pays the SAME stall** (30.1M vs 27.0M/hash, within noise). The oasis
was heat-shimmer. We had been optimizing a metric that was never the differentiator.

### What Era I established (the real wins, keep these)
- **E24** (+7.1% 1w): pad C* immediates to break the A53 4-cycle MAC interlock. *Real 1w win.*
- **E25**: skip redundant `add` on zero-offset `*_M` (equivalence-safe code-size win).
- **isolcpus +14%** and **GCC 16.1.0 cross-build +7.9%** — the two biggest wins were
  operational/toolchain, not code.
- Hardware AESE/AESD AES funnel; NEON-TTABLE AES (Track G) default ON.
- A **discipline**: every claim gated by KAT + 450/200 stress + PMU, every dead lead archived.

### What Era I exhausted (do NOT reopen — evidence in `docs/archived/`)
- `*_M` scheduler extension / E26 hoist — diverges JIT/interpreter (W3-2, SEGFAULTs). Dead.
- CBRANCH replay-map redesign — structurally unsafe without an emit-time byte-offset map;
  M1 weakened its ROI (the stall it would hide is not the competitive gap). Deferred.
- PRFM scratchpad hints, hugepages, PGO, dual-issue alignment, Track C, CSEL CBRANCH,
  FDIV/FSQRT, `-mtune=cortex-a53` — all measured null/regression. Closed with evidence.

### The contradiction — RESOLVED (Phase 0, `p0-instruction-count-resolution.md`)
Our own numbers disagreed on whether armrx emits *more* or *fewer* instructions than XMRig:

| source | armrx instr/hash | trust |
|---|---:|---|
| 1w census (w11) | 89.5M | **WRONG** (mis-divided/non-500 window) |
| 8w M1 (pool `Total`) | 103.5M | artifact (pool hash-count undercount) |
| **Phase 0 gated `--perf-ready` (1w AND 8w)** | **113.8M** | **authoritative** |

Phase 0 measured armrx with the self-counting 500-hash gated window at both 1w and 8w:
**113.8M instr/hash, flat across worker count (0% Δ)**. IPC 0.662, flat. This overturns BOTH
prior numbers. Compared to XMRig's M1 98.9M, **armrx is +15% HEAVIER per hash** — not leaner.

**Resolved direction:** the lever is **instruction count / codegen density** (armrx heavier),
NOT IPC. Per-worker IPC/stalls are identical 1w↔8w, so the 95.2% gap is cluster-contention
throughput loss (armrx scales to 65% of linear vs XMRig 73%) — which the +15% instruction count
likely *causes* (more instr/hash = more interconnect traffic). Fixing density (E3b) is the path.
See `docs/plans/era2-plan.md` §Phase 0 for the exact verdict and the corrected decision tree.

---

## The Era II principle (learned from the Mirage)

> **Lock the metric before you optimize it.**

We spent E24→E26→2 audits optimizing `ld_dep_stall` — a metric M1 proved isn't the gap. The
trap: optimizing a metric you haven't *confirmed* is the differentiator. In Era II every
experiment targets a metric that Phase 0 has locked as real, and is gated by the TESTING.md
sequence. No more phantom-chasing.

We also stop measuring *against XMRig* as the primary method. XMRig comparison gave us three
essential facts (stalls equal → not a stall gap; armrx leaner at 1w → not a density gap) and
now has diminishing returns (different build, hash-count accounting confounds). Era II optimizes
**our own IPC / instr-per-hash**, validated by our own 8w H/s (26.65 → 28) + PMU. XMRig becomes
a non-essential reference.

---

## Phases (Era II)

### Phase 0 — Resolve the 1w/8w contradiction (measurement only, ~1–2h)
Re-measure armrx 8w instr/hash + IPC with the **exact `--perf-ready` 500-hash gated window**
(TESTING.md §1), NOT the pool `Total` estimate. Expected ≈ 89.5M (matches 1w) → confirms lever
= **IPC**. If it really is ≈103.5M → lever = **instruction count**. One clean measurement picks
the direction. No code change. See `docs/plans/era2-plan.md` §Phase 0 for the command.

### Phase 1 — Experiment against the locked metric (TARGET = instruction count / density)
Phase 0 resolved: armrx = 113.8M instr/hash vs XMRig 98.9M = **+15% heavier**. The gap is codegen
density, NOT IPC (per-worker IPC is flat 1w↔8w). Primary experiment: **E3b per-opcode emission
diff vs XMRig** (GLM audit) — find the opcode(s) where armrx emits ~15% more. Candidates: `*_M`
consumer (`add→and→ldr→op` in emitMemLoad), CBRANCH form, ISWAP_R (3-MOV), INEG_R. Plus **Clang
cross-build A/B** (GLM Tier 1-B, unexplored, zero-risk, ~1hr) as the cheapest first probe. Every
candidate uses the reversible A/B flag pattern (TESTING.md §6) and the full gate sequence.

### Phase 2 — Gate, keep, or revert
Each candidate: KAT 16/16 → 450 stress → 200 stress → 1w+8w H/s + PMU. Keep only if it reduces
instr/hash without regression. Archive the dead attempt with its evidence (per Era I discipline).
The goal is still 95.2% → parity, but **earned by measurement, not assumed** (k3's rule).

---

## Known bugs to fix alongside (not perf, but real)
- **SIGINT/Ctrl-C ignored during pool mining** (user-confirmed). Worker loop checks
  `running_.load()` only at the top of each ~210ms hash; likely the signal isn't delivered to
  the blocked stratum reader thread. Fix: `sigaction` with `SA_RESTART=0` + ensure reader thread
  has SIGINT unblocked. (GLM §5.7-C.)
- **MetricsExporter data race** (`server_fd_` read in dtor without fence, written by bg thread) —
  one-line `std::atomic` fix, TSAN-catchable. (GLM §5.7-E.)

## Entry points for the next agent / session
1. `docs/TESTING.md` — how to measure (the only valid commands).
2. `docs/plans/era2-plan.md` — the concrete Phase 0/1/2 steps.
3. `docs/experiments/m1-miner-to-miner-pmu-diff.md`, `ab-e24-8w.md`, `perf-tracking.md` — the
   evidence that closed Era I.
4. `docs/audits/` — GLM, opencode, reasonix, gemini audits (context, some stale — check dates).
