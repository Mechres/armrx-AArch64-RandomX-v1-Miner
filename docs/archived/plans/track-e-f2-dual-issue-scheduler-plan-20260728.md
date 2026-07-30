# Track E F2: A53 Dual-Issue-Slot Scheduler — Plan for the Next Agent

## Status this plan assumes

Track E's F2 item was gated on Track A item 2 (a PMU-based front-end/back-end
stall breakdown). **That gate is now cleared**: Track A item 2 completed
2026-07-27 with a decisive result — back-end-dominated stalls, roughly 26:1
against front-end. F2's precondition ("only pursue if Track A item 2 confirms
back-end/dependency attribution") is satisfied. Full background:
`docs/plans/20260727/master-plan-20260727.md`, `## Track E`.

This is unimplemented — you are extending the existing emitter scheduler, not
retrying a prior attempt. Read the existing scheduler thoroughly before touching
it: `src/jit_compiler_a64.cpp` lines ~220-670 (`scheduleProgram` /
`scheduleSuperscalarProgram`), plus its three independent prior audits
(`docs/audits/emitter-scheduler-review.md`, `jit_scheduler_code_review_gemini.md`,
`scheduler-review-2026-07-25.md`) and the two known hazard-model gaps already
found and fixed/documented (`src==dst` physical-scratch-register reuse via x20;
the `IMUL_RCP` literal-pool superscalar-path ordering issue). You are adding
capability to this scheduler, not replacing it — its existing hazard-safety net
stays in force.

## What this item actually changes

The current scheduler reorders emission based only on a register-hazard model —
it has no model of which specific AArch64 instruction-*type* pairs can actually
co-issue on the A53's two pipes (per the Cortex-A53 Software Optimization Guide:
roughly branch+simple-ALU on one pairing, ALU+MUL/DIV/NEON on another — confirm
the exact pairing rules against the current optimization guide before hardcoding
anything, don't work from memory of the general pattern). A hazard-clean reorder
can still be issue-slot-suboptimal: two instructions with no data dependency
between them can still be a bad *pairing* choice if neither can co-issue with what
sits next to it.

This is **strictly a scheduling change** — it operates on top of the same
hazard-safety net that already exists (RAW/WAR/WAW across int/f/e register files,
CBRANCH anchors, memory-op aliasing, src==dst exclusion). Correctness risk is low
by construction: the hazard model doesn't change, only the priority function that
picks which hazard-legal swap to make. The real risk is a *slower* schedule, not
a wrong one — getting the dual-issue pairing rules wrong just leaves performance
on the table, it doesn't produce a bad hash. Budget accordingly: this is a
1-2 week effort-risk item, not a correctness-risk item.

## Step-by-step plan

### Step 1 — Confirm the A53 dual-issue pairing rules for real

- Before writing any scheduling logic, pin down the actual A53 issue-slot pairing
  rules from the Cortex-A53 Software Optimization Guide (Arm's official document)
  for every instruction class this JIT emits: integer ALU, integer MUL/UMULH/SMULH,
  branches, loads/stores, NEON/FP moves. Write this down explicitly as a table
  (instruction-class-pair → co-issuable Y/N) before touching the scheduler code —
  this table is the actual deliverable of this step, and everything after depends
  on it being right.

### Step 2 — Extend the scheduler's model, don't replace it

- The existing scheduler already has a hazard model (`hasHazard()` and friends)
  deciding *whether* a swap is legal. Add a second, independent scoring function
  that, among the hazard-legal candidate swaps at a given emission point, prefers
  the one that produces a better dual-issue pairing with its neighbor(s) — this
  should be additive to the existing mechanism, not a rewrite of its hazard logic.
- Apply to both `scheduleProgram` (main VM program) and
  `scheduleSuperscalarProgram` (dataset-derivation path), mirroring how the
  original scheduler's adoption required extending to both regions before it
  measured as a real win (the main-program-only version of the *original*
  scheduler measured as a null at first — don't repeat that mistake by only
  wiring this into one region and concluding it doesn't help).

### Step 3 — Stress-test correctness before measuring performance

- Reuse and extend the existing stress-test infrastructure:
  `tests/test_jit_scheduler_stress.cpp` (450 pairs) and
  `tests/test_jit_superscalar_scheduler_stress.cpp` (200 pairs). These already
  exist specifically to catch scheduler-introduced divergence — run them
  unmodified first to confirm your changes don't regress the existing hazard
  guarantees, then consider whether the new pairing-aware scoring needs its own
  additional stress cases (it shouldn't introduce new hazard classes, but verify
  that claim rather than assuming it).
- Full `test_jit_equivalence` (JIT vs. interpreter differential) must stay green
  throughout.

### Step 4 — Measure, `taskset`-pinned, per the project's standing discipline

- `perf stat -e cycles,instructions` before/after, on real hardware, pinned per
  the master plan's §4 measurement discipline (this is the same protocol that
  measured the original scheduler's superscalar-path extension as a real
  +0.233% IPC / -0.036% cycles win — small, but real and reproducible under this
  protocol). Reverse trial order at least once for thermal drift. Use the long
  measurement window, not a burst one.

## If real remaining slack is found here

Two further escalations are queued behind F2 specifically (not just behind Track A
item 2) in the master plan, and should not be started before F2's result is in:

- **A full dependency-graph list scheduler** (throwing away the fixed-window
  heuristic entirely for genuine list scheduling over the main VM program's full
  per-program dependency graph) — high effort, high correctness risk, would need
  review rigor matching or exceeding the original scheduler's three independent
  audits.
- **Register-allocation restructuring** (reducing false WAW/WAR dependencies by
  allocating more physical registers per virtual register, so whichever scheduler
  is in place has fewer artificial serialization points to work around) —
  genuinely unexplored, speculative; park behind F2's result.

Only promote either of these if F2 demonstrably still leaves real slack that a
wider-but-still-windowed pairing-aware model can't capture — don't jump to them on
spec alone.

## Validation

1. `tests/test_jit_scheduler_stress.cpp` and
   `tests/test_jit_superscalar_scheduler_stress.cpp`, both unmodified and green.
2. `test_jit_equivalence` green.
3. Full `ctest` suite green.
4. `perf stat` A/B, pinned, reversed trial order, real numbers written up.

## Success criterion

A measured (not estimated) net reduction in cycles/hash or increase in IPC beyond
what the existing hazard-only scheduler already achieves, with all correctness
tests green and no new hazard class introduced. A clean negative result (pairing
awareness doesn't move the needle beyond what's already captured) is also a
complete, valid outcome — write it up in a new `docs/experiments/` doc either way.
