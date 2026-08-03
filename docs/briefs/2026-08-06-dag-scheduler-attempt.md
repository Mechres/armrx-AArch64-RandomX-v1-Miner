# Brief: JIT DAG (dependence-graph) list scheduler — attempt after W3-2/E26

**Date:** 2026-08-06 · **Author:** Hermes (skeptical gate) · **Planner:** external agent (user-chosen) · **Status:** brief + prompt, NOT yet coded

---

## 1. Why this attempt, and why it is a *different shape*

The residual competitive gap is **instruction-mix only** (armrx 101.10M vs XMRig 94.5M @1w; H/s already at parity: 5.11 vs 5.04 1w, 26.65 vs 28 = 95.2% 8w). Every stall/padding/peephole lever is closed or reverted. The only lever that *could* beat XMRig is re-scheduling the JIT emission to better hide the in-order Cortex-A53's multiply/interlock latency.

Two prior attempts hit the **same trap twice** and must NOT be repeated in shape:
- **W3-2 (memory-op scheduler):** marked `*_M` opcodes `is_long_latency = true` so they became eligible as the **swap anchor (P)** in the existing greedy adjacent-swap scheduler. Result: `test_jit_equivalence` failed on seed 0 — silent wrong hashes. Reverted; mechanism never identified. See `docs/experiments/memory-op-scheduler-attempt.md`.
- **E26:** same class of `*_M` load-latency hoist → device segfault after clock-jump stale objects; rejected.

**New shape (different from both):** replace the greedy **adjacent-pair swap** (`scheduleProgram`, `src/jit_compiler_a64.cpp:638`) with a real **dependence-graph list scheduler** that:
1. Anchors (priority nodes whose latency it schedules around) are **multiply / long-latency ops ONLY** — `IMUL_R`, `IMULH_R`, `ISMULH_R`, `IMUL_RCP`. **Never any `*_M` opcode.**
2. Reuses the existing `hasHazard()` (line 465) and `computeFootprint()` (line 364) **verbatim** as the hazard oracle — does not weaken or reinvent them.
3. May *move* any non-anchor instruction (including `*_M`) as long as every pair in the moved sequence passes `hasHazard()` — but a `*_M` op is **never** the anchor of a reorder.
4. Preserves the **src==dst exclusion** (constraint 4) for moved instructions, **CBRANCH domain anchoring**, and the **IMUL_RCP literal-pool ordering rule** (superscalar `IMUL_RCP` never Q/R; main `h_IMUL_RCP` is self-contained per existing code).

This is a different *algorithm* (topological/DAG priority scheduling vs single adjacent swap), which is exactly what the post-double-trap rule requires.

## 2. Honest expected ROI

**Small / uncertain — possibly null.** `docs/audits/residual-gap-optimization-roadmap.md` §2 already concludes instruction *ordering* is not the dominant gap; the existing scheduler already hides part of the multiply latency (+0.233% IPC measured). A DAG scheduler *might* do better by finding non-adjacent independent work to fill multiply bubbles. Realistic ceiling: ≤1% cycles. **If the result is null, REVERT** — do not ship a dead change (project rule: tiered keep / clearly-negative = revert+docs).

## 3. Files & exact code anchors (read these first)

`src/jit_compiler_a64.cpp`:
- `struct InstrFootprint` — line 353 (int/f/e read/write bitmaps, `is_memory_op`, `is_barrier`, `is_cbranch`, `is_long_latency`, `is_imul_rcp`)
- `computeFootprint()` — line 364 (main-VM footprints; `*_M` cases at 385-394 set `is_memory_op=true`, **do NOT** set `is_long_latency`)
- `computeSuperscalarFootprint()` — line 529
- `hasHazard()` — line 465 (**THE oracle — reuse as-is**)
- `scheduleProgram()` — line 638 (current greedy adjacent-swap; the function to supersede with a DAG variant)
- `scheduleSuperscalarProgram()` — line 746 (second stage only)
- `JitCompilerA64::emitPrologueMix()` — line 777 (calls `scheduleProgram`)
- `g_swap_budget` / `ARMRX_MAX_SWAPS` — line 627 (bisection harness; keep working)

`include/armrx/jit_compiler_a64.hpp` — declarations of `scheduleProgram` / `scheduleSuperscalarProgram`.

**Docs to read before coding:** the scheduler doc-comments at `src/jit_compiler_a64.cpp:230-349` (constraints 1-4 + the src==dst story), `docs/experiments/memory-op-scheduler-attempt.md`, `docs/audits/residual-gap-optimization-roadmap.md`.

## 4. Hard constraints (any violation = reject)

- ❌ **Never** set `is_long_latency = true` for any `*_M` opcode. (W3-2 trap.)
- ❌ **Never** make a `*_M` opcode the anchor (P) of any reorder.
- ❌ Do **not** weaken `hasHazard()` or `computeFootprint()`.
- ✅ Reuse `hasHazard()` as the sole hazard gate for every pair in any emitted sequence.
- ✅ Keep src==dst exclusion (constraint 4) for moved (Q/R) instructions.
- ✅ Keep CBRANCH domain anchoring; keep IMUL_RCP literal-pool ordering rule.
- ✅ Keep `ARMRX_MAX_SWAPS` bisection harness functional.
- ✅ Be conservative: when ambiguity, do not reorder (fall back to current adjacent-swap or original order).

## 5. Scope

- **Stage 1 (this attempt):** main-VM program path (`scheduleProgram`). Add a DAG variant, gated behind an env var (e.g. `ARMRX_DAG_SCHED=1`) so the existing adjacent-swap remains the default until Stage 1 proves a win. `emitPrologueMix` selects the variant from the env var.
- **Stage 2 (ONLY if Stage 1 passes + shows a perf signal):** superscalar path (`scheduleSuperscalarProgram`), same hazard contract.
- **Out of scope:** memory-op anchoring, changing `*_M` emission, PGO, literal-pool changes, Blake2b, anything touching AES (already shipped, do not regress).

## 6. Verification gate (Hermes re-runs these on-device — all must pass)

Build: cross `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DARMRX_DISABLE_LTO=ON`, ship the cross-built binary.

Correctness (on-device, authoritative):
- `test_jit_equivalence` **16/16** (KAT differential — the gate that caught W3-2)
- `test_jit_scheduler_stress` **450 + 200 pairs** byte-identical (the differential harness that found constraint 4)
- `test_jit_determinism`, `test_jit_dataset_2way`, `test_mining` (real shares)

Perf A/B (device, `taskset -c 3`, B-M-B-M, 500-hash gated window, `--perf-ready`):
- `bench_armrx --full-hash-only --perf-ready` → `perf stat -e cycles,instructions`
- adopt only if cycles improve ≥0.5% in BOTH modified runs; record instr/hash + IPC + md5 of the executed binary.

If `test_jit_equivalence` or the stress test diverges: use `ARMRX_MAX_SWAPS=<k>` to binary-search the first divergent swap (the technique from `memory-op-scheduler-attempt.md` "Where this leaves future work"), root-cause, and either fix narrowly or revert. **Do not ship a change that fails any correctness gate.**

## 7. The external-agent prompt (copy-paste block below)

The prompt is self-contained for a planner with no chat context. Hand it to a *different* external planner than the one used for the W3-2/E26 attempts (post-double-trap rule: different shape + different planner).

---

## 8. External-agent prompt (self-contained — give this to the planner verbatim)

```
TASK: Implement a JIT dependence-graph (DAG) list scheduler for the armrx RandomX miner
(AArch64/Cortex-A53), as a gated alternative to the existing greedy adjacent-swap scheduler.
Read the repo FIRST; do not code from assumptions. Repo root: /home/mechres/Projeler/aarch64-randomx

== CONTEXT (why this shape, not another) ==
armrx is a clean-room RandomX miner. It is at H/s parity with XMRig (1w 5.11 vs 5.04; 8w 95.2%)
but ~7% heavier in instruction count. Every stall/padding/peephole lever is closed or reverted.
The one remaining lever that could beat XMRig is better hiding of the in-order A53's multiply /
interlock latency via emission reordering.

TWO PRIOR ATTEMPTS HIT THE SAME TRAP and were reverted — DO NOT repeat their shape:
- W3-2: marked scratchpad memory-load opcodes (*_M) as is_long_latency so they became the swap
  ANCHOR (P) in the greedy swap scheduler. Result: silent wrong hashes (test_jit_equivalence
  failed on seed 0). Reverted, mechanism never identified.
- E26: same class of *_M load-latency hoist -> device segfault. Rejected.
Project rule after a double trap: switch to a DIFFERENT approach shape + DIFFERENT planner.

== WHAT TO BUILD ==
A real dependence-graph list scheduler that supersedes the greedy adjacent-swap in
src/jit_compiler_a64.cpp::scheduleProgram (line 638), gated behind env var ARMRX_DAG_SCHED=1 so the
existing swap remains default until this proves a win. emitPrologueMix (line 777) selects the variant.

Algorithm shape (DIFFERENT from adjacent-swap):
- Build a per-instruction dependence graph from InstrFootprint (int_read/int_write/f_read/f_write,
  is_memory_op, is_barrier, is_cbranch, is_long_latency, is_imul_rcp).
- Hazard oracle = the EXISTING hasHazard() at line 465. REUSE IT VERBATIM. Never weaken it.
- Priority/anchor nodes (the long-latency ops you schedule around) = IMUL_R, IMULH_R, ISMULH_R,
  IMUL_RCP ONLY. **NEVER** any *_M opcode.
- You MAY move any non-anchor instruction (including *_M) to fill a multiply bubble, BUT every
  pair in the emitted sequence must pass hasHazard(). A *_M opcode is NEVER the anchor of a reorder.
- Preserve ALL existing safety rules:
  * src==dst exclusion (constraint 4): do not move a Q/R instruction whose src==dst.
  * CBRANCH domain anchoring (no reorder across a CBRANCH-to-CBRANCH domain boundary).
  * Superscalar IMUL_RCP literal-pool ordering: IMUL_RCP is never Q/R of a swap (its reciprocal
    literal is consumed in program order by a sequential pointer — swapping perturbs it).
  * Keep ARMRX_MAX_SWAPS (line 627) bisection harness functional.
- When in doubt, do NOT reorder — fall back to original program order for that instruction.

== FILES TO READ BEFORE CODING == (all in src/jit_compiler_a64.cpp)
- Lines 230-349: scheduler doc-comments (constraints 1-4, src==dst story) — mandatory reading.
- Line 353 InstrFootprint struct; 364 computeFootprint(); 529 computeSuperscalarFootprint();
  465 hasHazard(); 638 scheduleProgram() (the function to supersede); 746
  scheduleSuperscalarProgram(); 777 emitPrologueMix(); 627 g_swap_budget/ARMRX_MAX_SWAPS.
- include/armrx/jit_compiler_a64.hpp (declarations).
- docs/experiments/memory-op-scheduler-attempt.md (the W3-2 trap — read to understand what broke).
- docs/audits/residual-gap-optimization-roadmap.md (why ordering is a small/uncertain lever).

== SCOPE ==
Stage 1 (this task): main-VM program path ONLY, behind ARMRX_DAG_SCHED=1.
Stage 2 (ONLY later, if Stage 1 shows a perf signal): superscalar path — do NOT do it now.
Out of scope: memory-op anchoring, changing *_M emission, PGO, literal-pool changes, Blake2b, AES.

== HARD CONSTRAINTS (any violation => reject) ==
- NEVER set is_long_latency=true for any *_M opcode.
- NEVER make a *_M opcode the anchor (P) of any reorder.
- Do NOT weaken hasHazard() or computeFootprint(). hasHazard() is the sole gate.
- Reuse hasHazard() for every pair in every emitted sequence.

== VERIFICATION (you must run before claiming success) ==
CRITICAL DEVICE-SAFETY RULE (cost the user a device reboot last time):
- NEVER run the full test suite in one shot. The on-device JIT stress suites
  (test_jit_scheduler_stress = 450 pairs ~20min + 200 pairs ~35min; test_mining
  KAT ~7min; plus --perf-ready 500-hash windows) will peg the weak Cortex-A53 and
  make the device unresponsive — it had to be hard-rebooted. Run ONE test at a time,
  with a cooldown, and NEVER all together.
- qemu-aarch64 is FORBIDDEN for verification. qemu does NOT model the A53's in-order
  pipeline / memory-latency wall, so perf numbers are meaningless AND the slow tests
  just cook the host. Use qemu ONLY for a fast host-x86_64 compile sanity check if you
  must, never for the AArch64 target or for any perf claim.
- Keep the device alive: cap concurrent miners, leave headroom, one long test per session.

Staged gate (do in THIS order, one step at a time, stop if anything fails):
1. Cross build (host): cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake
   -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DARMRX_DISABLE_LTO=ON && cmake --build build-cross -j
2. FAST correctness first (device, ONE test, never the stress suite): test_jit_equivalence (16/16).
   This is the gate that caught W3-2 — run it ALONE first. Then, separately: test_jit_determinism,
   test_jit_dataset_2way. Each as its own invocation.
3. Stress test ONLY on demand / last: test_jit_scheduler_stress (450+200 pairs) is SLOW (~20-35 min
   each) — run it as a SEPARATE session, alone, only after steps 1-2 pass. Do NOT bundle it with
   anything else. test_mining (real shares) is also slow (~7 min) — run alone, separately.
4. Perf A/B (device, taskset -c 3, B-M-B-M, 500-hash gated, bench_armrx --full-hash-only --perf-ready,
   perf stat -e cycles,instructions): adopt only if cycles improve >=0.5% in BOTH modified runs;
   record instr/hash, IPC, binary md5.
If test_jit_equivalence or the stress test diverge: use ARMRX_MAX_SWAPS=<k> to binary-search the
first divergent swap, root-cause, fix narrowly OR revert. DO NOT SHIP a change that fails a
correctness gate. If the win is null, REVERT — do not ship a dead change.

== DELIVERABLE ==
A git branch with the gated change, a precise diff of scheduleProgram + emitPrologueMix + the new
DAG function, and a short report: what it does, the verification results (correctness gates +
perf A/B numbers), and whether to adopt or revert. Keep commits small.
```

