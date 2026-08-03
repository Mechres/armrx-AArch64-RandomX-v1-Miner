# Planning-agent prompt — `*_M` scratchpad stall, next attempt

> Copy everything below the line into your planning agent. Replace `AGENTNAME` with the
> agent's handle and `PLANNAME` with a short slug (e.g. `replay-invariant`,
> `neon-ldr`, `vm-fusion`) before sending. The agent MUST save its plan to the file
> named in §Final deliverable.

---

You are planning (NOT coding) the next attempt to close a performance gap in a
clean-room AArch64 RandomX mining JIT called **armrx** (C++, CMake, targets
Cortex-A53 @ 765 MHz, MSM8929, two 4-core clusters). The repo is at
`/home/mechres/Projeler/aarch64-randomx`.

## Context you must read first (do not skip)

1. `docs/briefs/what-breaks-star-m-path.md` — the consolidated failure analysis. This is
   the single most important file. It explains why two prior attempts (W3-2 scheduler
   reorder, E26 byte-hoist) BOTH failed on the same on-device gate, and names the real
   root-cause class: **CBRANCH replay-domain equivalence** (JIT replay is code-offset
   based via `reg_changed_offset[]`; interpreter replay is instruction-index based). Read
   it in full before proposing anything.
2. `docs/briefs/2026-08-04-star-m-fix.md` — original brief + E26 outcome (§8).
3. `docs/experiments/perf-tracking.md` — parity numbers (1w 5.12 vs 5.04; 8w 26.65 vs 28
   = 95.2%), the confirmed `ld_dep_stall` +63% at 8w measurement, E24/E25 shipped, E26
   record + stall-arithmetic correction.
4. `docs/audits/opencode_20260803_audit.md` and
   `docs/audits/github_copilot_2026-08-03_audit.md` — the two post-E24 audits.
5. Source: `src/jit_compiler_a64.cpp` — focus on `emitMemLoad` (1386), `h_CBRANCH`
   (1976, replay anchor at 1991), `emitPrologueMix` anchor bookkeeping (~822, 841),
   `scheduleProgram` (640–744), `hasHazard` (~471), and the `*_M` handlers
   (`h_IADD_M`/`h_ISUB_M` 1465–1527, `h_IMUL_M`/`h_IMULH_M`/`h_ISMULH_M` 1549–1625,
   `h_IXOR_M` 1700–1715).

## The problem

armrx is at 95.2% of XMRig at 8 workers (26.65 vs 28 H/s). The residual ~4.8% is a
single PMU stall class: `ld_dep_stall` on the main-VM `*_M` scratchpad path
(`ldr → op` 3-cycle A53 load-use bubble; `ld_dep_stall` is a MODEST ~11% of core cycles at
8w — the earlier "saturates 88%" figure was an 8× arithmetic error and has been corrected in
perf-tracking.md; verify the current number there before quoting). The
stall is real and large — this is NOT a measurement artifact and NOT a DRAM latency.

## Hard constraints (violating any of these repeats a known failure — do not)

1. **Do NOT change the `[anchor_offset, cbranch_offset]` CBRANCH replay window
   semantics.** If you relocate emitted bytes, the set of ops + their order inside every
   replay window must remain byte-for-byte equivalent to the interpreter's replay-index
   slice. Safest: never move bytes across CBRANCH/CFROUND boundaries.
2. **Do NOT mark `*_M` as `is_long_latency` and do NOT extend scheduler distance-N
   reordering to memory ops** (W3-2 diverged on this).
3. **The `ldr` (memory op) never moves relative to its consumer** — memory order exact.
4. **Clean-room:** technique only from XMRig. Re-implement in armrx conventions. No
   XMRig source in the tree.
5. Any proposal must specify its **on-device gate plan**: `test_jit_equivalence` (16/16)
   + `test_jit_scheduler_stress` (450 pairs) + `test_jit_superscalar_scheduler_stress`
   (200 pairs). The 450-pair suite is the real oracle — KAT alone is insufficient (both
   prior failures passed KAT). Budget ~20–35 min per stress suite on device.
6. If a proposed change fails any gate: REVERT, record failing seed/input + first
   differing hash stage, do not iterate blindly.

## What you must produce (a PLAN, not code)

Evaluate the candidate angles in `what-breaks-star-m-path.md` §6 (A: root-cause the
replay invariant first; B: reduce stall without moving bytes; C: NEON-load lever;
D: accept-and-document). You may also propose a genuinely different shape — but it must
respect §Hard constraints. For your recommended approach(es):

- State precisely what changes and what does NOT change in emitted code.
- Prove (or argue rigorously) that the CBRANCH replay window is preserved — this is the
  crux; a hand-wave here is the exact failure mode of the two prior attempts.
- Identify the exact invariant `reg_changed_offset[]` requires and show your change
  satisfies it.
- Specify the minimal safe experiment + the gate sequence to validate it.
- Rank approaches by expected payoff vs risk; explicitly call out which are "likely safe
  but small" vs "high-payoff but replay-risky."
- If your conclusion is that this is a hard in-order-A53 limit, say so with the evidence
  chain — that is an acceptable outcome (parity = 95.2%).

## Final deliverable

Produce a detailed plan. **You MUST save the full plan to a file named
`AGENTNAME_PLANNAME_DATE.md`** (e.g. `claude_replay-invariant_2026-08-05.md`) in the
repo's `docs/briefs/` directory, and report the file path back. The plan file must be
self-contained: it should include the chosen approach, the replay-invariant proof, the
exact code regions touched, the gate sequence, and the expected/measured-risk verdict.
Do NOT modify any source files — planning only.

---

## Notes for the human (not part of the agent prompt)

- Replace `AGENTNAME` / `PLANNAME` before sending so the saved filename is meaningful.
- After the agent returns the plan file, route it back to Hermes (this assistant) for a
  gate-review against the W3-2/E26 failure mode BEFORE any code lands. Hermes will not
  design the attempt; it gates it.
- The 4 stale untracked agent outputs (`20260803/*`, the sun_aug_02 desktop-attachment)
  have been deleted; working tree is clean.
