# armrx AArch64 RandomX Audit (post-E24)
Date: 2026-08-03
Agent: GitHub Copilot
Scope: Read-only audit, no code edits. Goal is to localize the remaining 8-worker gap and root-cause the prior W3-2 `*_M` scheduler divergence.

## TL;DR
E24 solved the superscalar interlock issue. The remaining real software delta is most likely in the main-VM `*_M` path structure (not generation), specifically the strict `add -> and -> ldr -> consume` chain and a few small emission-shape differences vs XMRig that amplify load-use serialization on A53 under 8 workers.

The prior W3-2 divergence is most likely not "memory aliasing". The stronger explanation is a scheduler-equivalence modeling gap around CBRANCH replay-domain semantics once memory ops were promoted to long-latency anchors (`P`), not raw register hazard mistakes.

## Evidence Anchors
- Main facts and dead leads: `docs/experiments/perf-tracking.md`
- W3-2 postmortem: `docs/experiments/memory-op-scheduler-attempt.md`
- Bisection instrumentation: `docs/experiments/w3-2-swap-budget-bisect.md`
- Main scheduler/hazard model: `src/jit_compiler_a64.cpp` (comments + `computeFootprint`, `hasHazard`, `scheduleProgram`)
- `*_M` emission: `src/jit_compiler_a64.cpp:1385-1416`, `src/jit_compiler_a64.cpp:1465-1715`
- CBRANCH JIT targeting: `src/jit_compiler_a64.cpp:1959-1995`
- Interpreter CBRANCH/register_usage model: `src/vm.cpp:490-511`, execution loop `src/vm.cpp:715-721`

## Ranked Hypotheses (remaining gap)

### 1) Main-VM `*_M` load-use chain is still too serial under contention (highest confidence)
Confidence: High

Why:
- `emitMemLoad` emits a hard dependency chain (`add/imm materialization -> and -> ldr`) and the value is consumed immediately by `ADD/SUB/MUL/UMULH/SMULH/EOR` in the same handler.
- This shape is visible in all integer `*_M` handlers (`h_IADD_M`, `h_IMUL_M`, `h_IMULH_M`, `h_ISMULH_M`, `h_IXOR_M`).
- On A53, this is exactly the pattern that generates load-to-use bubbles (`ld_dep_stall`) when independent work is scarce.
- E24 removed superscalar interlock excess; remaining gap naturally shifts to main-VM memory path.

Where:
- `src/jit_compiler_a64.cpp:1385-1416`
- `src/jit_compiler_a64.cpp:1465-1715`

What is new/structural:
- Compared technique-wise with XMRig A64 emission: XMRig’s `emitMemLoad` includes small zero-immediate fast paths (avoiding unnecessary address materialization in some cases). armrx currently always emits address materialization in these spots. Even if each case is tiny, it lengthens the critical chain in the dominant serialized region.

---

### 2) W3-2 divergence root cause is scheduler-equivalence modeling around CBRANCH replay domains, not memory aliasing
Confidence: Medium-high

Why:
- The reverted W3-2 change marked memory ops long-latency to allow `P,R,Q`/`P,R2,Q,R1` reordering.
- Memory-memory swaps were still forbidden (`hasHazard`: any two memory ops hazard), so pure aliasing between memory ops is unlikely as root cause.
- Divergence was immediate and deterministic in stress, which matches semantic replay-boundary issues more than rare cache/data races.
- The scheduler comments already document one subtle equivalence class (CBRANCH anchor/replay-domain coupling) and one unresolved class (`src==dst` exclusion rationale). Promoting `*_M` to eligible anchor `P` likely exposed another unmodeled replay-domain condition.

Where:
- Model assumptions: `src/jit_compiler_a64.cpp:364-476`, `src/jit_compiler_a64.cpp:638-744`
- CBRANCH codegen/replay target: `src/jit_compiler_a64.cpp:1959-1995`
- Interpreter target semantics: `src/vm.cpp:490-511`, `src/vm.cpp:715-721`
- Failure history: `docs/experiments/memory-op-scheduler-attempt.md`

Important clarification:
- Notes that attribute the W3-2 failure to superscalar scheduler are likely conflated. The actual `*_M is_long_latency` experiment was in main `computeFootprint`, not `computeSuperscalarFootprint`.
- `test_jit_superscalar_scheduler_stress` still executes full VM hashes, so a main-program scheduling bug can fail it.

---

### 3) Zero-immediate `*_M` address path inefficiency compounds at 8 workers
Confidence: Medium

Why:
- In armrx, `emitMemLoad` always materializes address via `emitAddImmediate` even when effective immediate is zero in `src!=dst` path; in `src==dst`, it still materializes immediate then loads.
- This is cheap in isolation, but it extends dependency depth ahead of the load in the exact region already known to be bottleneck-prone.
- Under 8 workers (single LSU per core + shared memory pressure), extra chain depth can cost more than at 1 worker.

Where:
- `src/jit_compiler_a64.cpp:1392-1413`

---

### 4) Residual gap is mostly not superscalar anymore
Confidence: Medium

Why:
- E24 fixed the superscalar interlock pathology and flipped 1-worker parity in armrx’s favor.
- Remaining 8-worker loss while 1-worker is ahead points to contention-sensitive region(s), which aligns better with main-VM scratchpad path than with superscalar body now.

Where:
- `docs/experiments/perf-tracking.md` (E24 section + post-E24 standings)

## W3-2 Divergence: Most Likely Root Cause
Most likely cause: an equivalence bug in scheduling constraints relative to CBRANCH replay semantics after enabling memory-op `P` reorders, not a simple register hazard omission and not plain memory aliasing.

Reasoning chain:
1. Memory-memory reordering remained blocked by model, so classic address alias bugs are less likely.
2. Divergence is deterministic and appears in differential stress, which is exactly the signature of semantic replay-domain mismatch.
3. CBRANCH semantics are unusually sensitive: interpreter replay boundary is instruction-index based; JIT replay boundary is code-offset based (`reg_changed_offset`).
4. Existing anchor exclusions were built around already-observed long-latency ALU cases. Allowing `*_M` as scheduling anchor introduces a new class where "safe by register hazard" may still be unsafe by replay-domain equivalence.

This directly explains why W3-2 failed despite conservative hazard checks.

## Cheapest Single Confirm/Kill Measurement
Run a 1w vs 8w PMU growth-delta test focused on main-VM memory stalls, miner-to-miner:

- Measure both miners at 1 worker and 8 workers on same thermal state, no isolcpus changes.
- Events: `cycles`, `instructions`, `ld_dep_stall`, `agu_dep_stall`, `other_interlock_stall`.
- Compare per-hash growth from 1w -> 8w for `ld_dep_stall`.

Decision rule:
- If armrx `ld_dep_stall/hash` inflates materially more than XMRig while `other_interlock_stall` stays post-E24-normalized, top hypothesis is confirmed (main-VM `*_M` structure under contention).
- If not, kill Hypothesis #1 and move to CBRANCH-equivalence instrumentation first.

Why this is cheapest:
- No code changes.
- Uses existing perf workflow already used in this repo.
- Directly discriminates memory-serialization vs multiply-interlock as the remaining bottleneck class.

## What Is Not Worth Trying (now)
- More superscalar scheduler/peephole window tweaks (distance-4/5, etc.): exhausted, tiny hit-rate history, not the post-E24 bottleneck.
- Reopening E22-style "disable scheduler" loops: already inconclusive/no upside and does not target current dominant suspect.
- Instruction-count-only optimizations detached from stall-class evidence: this repo already has multiple regressions from that pattern.
- Hugepages/PGO/mtune/LTO toggles as gap-localization tools: already characterized in current docs.

## Practical Next Step Order
1. Do the single PMU growth-delta measurement above (confirm/kill Hypothesis #1).
2. If confirmed: focus only on non-reordering structural `emitMemLoad` chain shortening opportunities.
3. If killed: instrument first bad swap for W3-2-style memory-op scheduling to isolate exact replay-domain hazard before any new scheduler attempt.
