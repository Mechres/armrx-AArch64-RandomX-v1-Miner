# Emitter Scheduler Extension to Memory-Load Opcodes — Tried, Reverted, Cause Not Identified

## Context

`PLAN.md` Phase 6 item 12 added a conservative emitter lookahead scheduler that reorders JIT
emission (never computed results) to hide long-latency-op stalls on the in-order Cortex-A53. It
originally targeted `IMUL_R`/`IMUL_RCP`/`IMULH_R`/`ISMULH_R` in both the main VM program and the
superscalar/dataset-derivation region, and was measured and adopted (+0.233% IPC / -0.036%
cycles, `taskset`-pinned).

Closing out peephole JIT coalescing (Phase 7 item 2 — see `PLAN.md` for the full account)
required extending `tools/jit_correlate.py` to split its old "~22% of cycles unattributed"
bucket by the `code_size` boundary it already parsed but never used. That split found:

| Region | % of instructions | % of cycles | relative IPC |
|---|---|---|---|
| Main per-hash VM program (offset < CodeSize) | 9.23% | 20.04% | **0.461×** avg |
| Superscalar, opcode-attributed | 72.71% | 63.52% | 1.145× avg |
| Superscalar, unattributed (fixed wrapper chunks) | 2.49% | 2.38% | 1.046× avg |
| Outside any JIT buffer (named C++) | 15.56% | 14.05% | 1.107× avg |

The main VM program region carries ~9% of dynamic instructions but ~20% of cycles — a ~2.2× IPC
penalty relative to the rest of the pipeline. This region is where the memory-operand opcodes
(`*_M`, `ISTORE`) live — roughly 48% of this region's code bytes per the original static
breakdown — doing genuinely random 64-byte reads/writes into the 2 MiB scratchpad. Branch
misprediction was already ruled out separately (2.4% hot-path miss rate, ~0.1-0.16% of cycles).
This reads as a stall signature, not an instruction-count signature — exactly the kind of thing
the emitter scheduler's existing mechanism was built to hide, just never previously pointed at
these opcodes.

## What was tried

In `src/jit_compiler_a64.cpp`'s `computeFootprint()`, flagged the memory-load opcodes
(`IADD_M`/`ISUB_M`/`IMUL_M`/`IMULH_M`/`ISMULH_M`/`IXOR_M`/`FADD_M`/`FSUB_M`/`FDIV_M` — every
`*_M` opcode that reads from the scratchpad **and** writes a register) as `is_long_latency =
true`, making them eligible as swap triggers (`P`) the same way the long-latency ALU ops already
were. `ISTORE` was deliberately excluded: it writes scratchpad memory, not a register, so no
later instruction can have a register-level RAW dependency on it — the scheduler's mechanism
(defer `Q`, which needs `P`'s result, and fill the gap with independent `R`) has no analogue for
a pure store.

The reasoning for believing this needed no new hazard-model work: the existing
memory-memory-always-hazard rule in `hasHazard()` (`if (a.is_memory_op && b.is_memory_op) return
true;`) already prevents any `*_M` instruction from being swapped past another `*_M` instruction,
since scratchpad addresses are dynamic and non-aliasing can't be proven at compile time. Marking
`*_M` opcodes `is_long_latency` should therefore only ever fill a stall with an independent
*register* op (`R` must not be a memory op if `P` is), never another memory op — the same
argument that already applies safely to `IMUL_R` et al.

## Result

`test_jit_equivalence` (the standard 16-seed JIT-vs-interpreter differential suite) failed
immediately, on its first, most basic (seed, input) pair (`jit_equiv_seed_0` /
`equivalence input 0_0`) — the first failure of this specific test in the project's history.
Reverting the memory-op change alone, with everything else unchanged, made it pass again,
confirming the memory-op extension as the sole cause of the divergence.

## Investigation

Two hypotheses were checked and ruled out before the attempt was abandoned:

1. **`emitMemLoad`'s own `src==dst` shared-physical-scratch-register special case.**
   `emitMemLoad<tmp_reg>` (used by all the integer `*_M` handlers) branches on `src != dst`
   vs. `src == dst`, materializing the address computation through a shared physical scratch
   register (x20) in the `src == dst` path — structurally identical in shape to the *original*
   `src==dst` hazard that required an explicit Q/R exclusion for the ALU opcodes (`h_ISUB_R`,
   `h_IMUL_R`, `h_IXOR_R`, `h_IROL_R`). This looked like the most promising lead. It doesn't hold
   up: this exact pattern (`emitMemLoad`'s x20 usage) was already reviewed by all three
   independent code reviews of the scheduler (Deepseek, Gemini, Hermes — see
   `docs/audits/emitter-scheduler-review.md`, `jit_scheduler_code_review_gemini.md`,
   `scheduler-review-2026-07-25.md`) and confirmed self-contained (write-then-immediately-read
   within one handler's own emission, regardless of swap position).

2. **A CBRANCH-with-unwritten-target-register assert firing during the investigation.** A
   defensive `ARMRX_ASSERT` (added the same day per an external audit's "theoretical only" claim)
   fired repeatedly during test runs. This turned out to be an unrelated, separate, real bug in
   the assert's own premise (see below) — not the cause of the hash divergence. The interpreter's
   `-1 → pc=0` handling for this case is correct and already matches the JIT's own
   `reg_changed_offset[]` reset semantics (reset to `PrologueSize` before every compile in
   `emitPrologueMix`); this was a dead end for explaining the memory-op divergence specifically.

**The actual mechanism was not conclusively identified.** Given the failure mode is silent wrong
hashes — not a crash — and this was an explicitly speculative, "may be a null result" experiment
from the outset (not a required feature), the change was fully reverted rather than ship a
targeted exclusion without being able to verify it. This differs from the two earlier scheduler
hazard discoveries (`src==dst`, superscalar `IMUL_RCP` literal-pool ordering), both of which had
an empirically-validated fix even without a fully-traced mechanism — here there is no fix at all,
just a clean revert back to the known-good state.

## Side finding: a real bug in the CBRANCH assert

While investigating, the defensive `ARMRX_ASSERT(register_usage_[creg] >= 0, ...)` added earlier
the same day fired repeatedly on a completely normal `test_jit_equivalence` run — disproving its
"theoretical only, no known real-world trigger" premise directly. Traced why the existing
behavior is actually correct and intentional: `register_usage_[creg] == -1` (a CBRANCH's target
register genuinely never written earlier in the program) wraps `pc` to `0` via the interpreter's
execute loop's `++pc`, i.e. "restart from VM instruction 0" — which exactly matches the JIT's own
`reg_changed_offset[]`, reset to `PrologueSize` (VM instruction 0's own code offset) before every
compile. Both paths already handled this correctly; the assert was flagging normal, spec-
legitimate program behavior as if it were exceptional. Removed (`4da77f0`).

## Where this leaves future work

- **If this idea is revisited**, budget real bisection time using the same technique that solved
  the original `src==dst` hazard: a global swap-count budget (`ARMRX_MAX_SWAPS`-style env var)
  to binary-search which specific swap first causes a divergence, across a minimal reproducer.
  Don't assume "no new hazard needed" just because the existing rules look sufficient on paper —
  something about a memory op specifically occupying the `P` (trigger/anchor) position, not just
  `Q`/`R`, broke something not yet understood. A narrower version (e.g. integer `*_M` only, or
  only when the swap window has zero register overlap with any nearby CBRANCH domain) might avoid
  whatever the hazard is, but this wasn't tested.
- **This region's ~2.2× IPC penalty remains real and unaddressed.** It's the most concrete,
  quantified performance lead this project currently has on record — just not one with a
  currently-known safe fix. See `docs/plans/future-performance-ideas-20260725.md` for other
  angles that don't require extending this specific scheduler mechanism.
- `tools/jit_correlate.py`'s region-split extension (splitting by `code_size`) is kept as reusable
  diagnostic infrastructure regardless of this outcome — it's how the 2.2× IPC finding was made
  in the first place.

## Verification

Full on-device suite (`armrx_tests`, `test_mining`, `test_jit_encodings`, `test_jit_determinism`,
`test_jit_equivalence`) green after the revert, 5/5. No behavior change from the pre-attempt
state other than the CBRANCH assert removal (see above), which is independently verified correct.
