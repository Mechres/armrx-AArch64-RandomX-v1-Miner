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

### The contradiction we left unresolved (the hinge of Era II)
Our own numbers disagree on whether armrx emits *more* or *fewer* instructions than XMRig:

| source | armrx instr/hash | XMRig | who's leaner |
|---|---:|---:|---|
| 1w census (w11, `--perf-ready` 500-hash, authoritative) | **89.5M** | 101.4M | **armrx −12%** |
| 8w M1 (pool `Total`, estimate) | **103.5M** | 98.9M | armrx **+4.6%** |

Instruction count per hash cannot jump +15% with worker count. The 1w census is the trustworthy
one (exact gated window); the 8w M1 number is likely a measurement artifact (pool hash-count
undercount, or window includes re-init). **This contradiction must be resolved before any
optimization**, because it decides the entire direction:
- If armrx is genuinely *leaner* (1w census right) → the gap is **IPC** (0.547 vs 0.654) →
  lever = stall-hiding / scheduling, broadened beyond C* immediates.
- If armrx is *heavier* at 8w (M1 right) → the gap is **instruction count** → lever = codegen
  density.

The E24 A/B (removing C* padding changed 8w instr by +0.3%, stalls 0%) already hints the
lever is IPC, not density — but we won't bet on a hint.

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

### Phase 1 — Experiment against the locked metric (trial-and-error, targeted)
- *If lever = IPC:* stall-hiding / scheduling experiments across the **whole program** (the
  superscalar body is 80.5% of instructions per the region census; main-VM JIT is the worst
  region at IPC 0.405). Build on E24's proven technique, broaden it. Candidates: Clang
  cross-build A/B (GLM Tier 1-B, unexplored, zero-risk, ~1hr), per-opcode emission diff vs
  XMRig (GLM E3b: `*_M` consumer, CBRANCH, ISWAP_R, INEG_R), interleave-of-independent-ops
  in the main-VM body.
- *If lever = instruction count:* codegen-density experiments (instruction selection, constant
  materialization, register allocation) on the same candidate opcodes.
Every candidate uses the reversible A/B flag pattern (TESTING.md §6) and the full gate sequence.

### Phase 2 — Gate, keep, or revert
Each candidate: KAT 16/16 → 450 stress → 200 stress → 1w+8w H/s + PMU. Keep only if it improves
the *target* metric without regression elsewhere. Archive the dead attempt with its evidence
(per Era I discipline). The goal is still 95.2% → parity, but **earned by measurement, not
assumed** (k3's rule).

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
