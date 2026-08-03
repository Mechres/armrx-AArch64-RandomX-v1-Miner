# Audit-agent prompt — full independent audit of the armrx `*_M` / 8w-gap effort

> Copy everything below the line into your audit agent. Replace `AGENTNAME` with the
> agent's handle and `AUDITNAME` with a short slug (e.g. `full-audit`, `replay-rootcause`,
> `perf-independent`) before sending. The agent MUST save its report to the file named in
> §Final deliverable.

---

You are performing an **independent full audit** (NOT coding, NOT planning a new change)
of a performance-optimization effort on a clean-room AArch64 RandomX mining JIT called
**armrx** (C++, CMake, target: Cortex-A53 @ 765 MHz, MSM8929, two 4-core clusters). The
repo is at `/home/mechres/Projeler/aarch64-randomx`. The effort's goal was to close the
remaining gap vs XMRig at 8 workers (armrx 26.65 vs XMRig 28 H/s = 95.2% parity).

## What you must read (do not skip — the answer is in here, not in the code alone)

1. `docs/briefs/what-breaks-star-m-path.md` — consolidated failure analysis. The two prior
   attempts (W3-2 scheduler reorder → JIT≠interpreter divergence; E26 byte-hoist → SEGFAULT
   in JIT buffer) both failed on the SAME gate (`test_jit_scheduler_stress`, 450 pairs) and
   share a root-cause class: **CBRANCH replay-domain equivalence** (JIT replay is
   code-offset based via `reg_changed_offset[]`; interpreter replay is instruction-index
   based). Read fully.
2. `docs/briefs/2026-08-04-star-m-fix.md` — original brief + E26 outcome (§8).
3. `docs/experiments/perf-tracking.md` — the standing facts: parity numbers, the confirmed
   1w→8w `ld_dep_stall` +63% measurement, E24/E25 shipped, E26 record + a correction to a
   bad stall-arithmetic claim.
4. `docs/audits/opencode_20260803_audit.md` and
   `docs/audits/github_copilot_2026-08-03_audit.md` — the two post-E24 audits.
5. `RETROSPECTIVE.md`, `changelogs.md`, `README.md`, `ROADMAP.md` at repo root — project
   history and what shipped.
6. Source (for verification, not editing): `src/jit_compiler_a64.cpp` — `emitMemLoad`
   (1386), `h_CBRANCH` (1976, replay anchor `reg_changed_offset` at 1991), `emitPrologueMix`
   anchor bookkeeping (~822, 841), `scheduleProgram` (640–744), `hasHazard` (~471), `*_M`
   handlers (`h_IADD_M`/`h_ISUB_M` 1465–1527, `h_IMUL_M`/`h_IMULH_M`/`h_ISMULH_M` 1549–1625,
   `h_IXOR_M` 1700–1715), `emitCpoolImmediate` (~1341, the E24 change).

## Audit scope (be exhaustive on these)

### A. Correctness of the E24/E25 shipping changes
- E24: padded superscalar C* immediates to 3-instr `MOVZ/MOVN+MOVK`. Verify the encoding is
  byte-equivalent to the reference form and that the KAT (16/16) + both stress suites
  actually pass in the committed tree (do not trust the log; check `git log` + the test
  sources if needed).
- E25: skip imm==0 `ADD` in `emitMemLoad`. Verify the `AND tmp_reg, src, #mask` encoding in
  the imm==0 branch is correct (Rd=tmp_reg, Rn=src) and that it cannot change the address
  vs the imm!=0 path.

### B. The `ld_dep_stall` attribution — is it real?
- Re-derive the 1w→8w PMU numbers from `perf-tracking.md`. NOTE: the file's current stall
  fraction is ~11% of core cycles (an earlier "0.88 / 88%" figure was an 8× arithmetic error
  and has been corrected). Re-derive it yourself: 25.9M/hash × 26.65 H/s ≈ 690M/s aggregate
  across 8 workers; divide by 8 × 765 MHz = 6,120M cycles/s → ~0.11. Is the remaining claim
  "this is the 3-cycle L1 load-use bubble, not DRAM" defensible given the corrected fraction?
  Flag if the measurement window, core pinning, or
  per-hash normalization is unsound (note: the original 8w number was first mis-measured at
  23.6 H/s due to a short window + wrong core pin — confirm the corrected 26.65 is the one
  used).
- Is the residual gap really `*_M` and not something else (e.g. the main-VM JIT memory-op
  stalls the perf-tracking doc itself calls out at ~9% of instructions / ~20% of cycles)?

### C. The CBRANCH replay mechanism — root-cause, not hand-wave
- Trace exactly what `reg_changed_offset[]` requires for JIT replay to match interpreter
  replay. Identify the PRECISE invariant a code relocation must preserve.
- Explain specifically WHY E26's hoist (with its anchor-stop guard) still segfaulted. Is it
  (a) a sub-boundary liveness issue (hoist reads `x20`/scratchpad temp before its in-window
  writer), (b) a `writesX20()`/conservative-check false negative, or (c) a multi-anchor /
  CFROUND domain mix? Give a concrete, evidence-backed verdict — this is the crux the prior
  attempts never root-caused.
- State whether the replay invariant is even *preservable* by any byte-relocation, or
  whether the only safe `*_M` improvement is one that changes NO emitted byte positions
  across any CBRANCH boundary.

### D. Independent search for the lever (fresh eyes)
- Given (B) and (C), what is the highest-payoff *safe* change? Evaluate: (i) root-causing
  the replay invariant then designing a fix that preserves it; (ii) reducing the stall
  WITHOUT moving bytes (VM-level op fusion / local unrolling, zero emission-order change);
  (iii) a NEON-load lever (load scratchpad via NEON LSU + `ins` into scalar — different
  pipe, may hide the integer load-use bubble); (iv) the honest "this is a hard in-order-A53
  limit, 95.2% is the parity point" conclusion.
- For each, give expected payoff, replay-risk, and the minimal gate sequence to validate.

### E. Process / discipline audit
- Was the "revert on first gate failure" rule actually followed? (E26 was reverted — verify
  the tree is clean at the stated HEAD.)
- Were the dead-ends documented so they aren't re-attempted? (Check `perf-tracking.md` and
  the briefs.)
- Any evidence gaps, unverified claims, or measurements that should be re-run?

## Hard rules for the audit
- **Read the docs; do not re-derive from code alone.** The prior failures are documented,
  not visible in the current (reverted) source.
- **No source edits.** Audit only. If you find a bug, describe it; do not patch.
- **Clean-room:** if you reference XMRig, technique only — no XMRig source in findings.
- Distinguish **evidence** (measured/committed) from **assertion** (plausible, unverified).
  The effort has a history of plausible-but-wrong claims (e.g. the "30cyc/op DRAM" stall
  rationalization that was later corrected) — call those out where you see them.

## Final deliverable

Produce a full audit report. **You MUST save the report to a file named
`AGENTNAME_AUDITNAME_DATE.md`** (e.g. `gemini_full-audit_2026-08-05.md`) in the repo's
`docs/audits/` directory, and report the file path back. The report must contain, at
minimum:
1. A verdict on each of A–E above, with evidence cited.
2. A concrete root-cause for the E26 segfault (not "likely", but the mechanism).
3. A ranked list of safe next attempts with expected payoff + replay-risk + gate plan.
4. An explicit statement: is the residual 4.8% closable without breaking CBRANCH replay
   equivalence, or is 95.2% the evidence-backed parity point?

---

## Notes for the human (not part of the agent prompt)

- Replace `AGENTNAME` / `AUDITNAME` before sending.
- This audit is INDEPENDENT of Kimi k3's planning run — run it in parallel; they should not
  share context (the audit verifies the past effort; k3 plans the future attempt).
- When the audit returns its `AGENTNAME_AUDITNAME_DATE.md`, route it to Hermes for a
  gate-review alongside k3's plan. Hermes gates before any code lands.
