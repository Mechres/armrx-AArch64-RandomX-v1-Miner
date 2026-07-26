# Main VM program scheduler: widened swap window — adopted (2026-07-26)

**Status: adopted.** This is `docs/plans/mid-high-risk-performance-ideas-20260726.md` Tier 1
item 2. Real, small, reproducible win — validated against the most rigorous differential test
this scheduler has (450 pairs), then measured via `perf stat`.

## Background

The existing emitter lookahead scheduler (`scheduleProgram()`, `src/jit_compiler_a64.cpp`,
adopted 2026-07-25) hides the stall after a long-latency multiply (`IMUL_R`/`IMULH_R`/
`ISMULH_R`/`IMUL_RCP`) by looking exactly one swap ahead: at long-latency instruction P, if the
very next instruction Q would stall on P anyway, and the instruction after that (R, at `i+2`) is
independent of both P and Q, emit `P, R, Q` instead of `P, Q, R`. Deliberately narrow in scope
(single adjacent-pair swap, fixed 3-instruction window) to keep the correctness argument
tractable — see the file's own doc comment above `scheduleProgram()` for the full hazard model
(register RAW/WAR/WAW across int/f/e register files, CBRANCH anchors, memory-op aliasing,
src==dst exclusion).

Given `docs/experiments/scratchpad-locality-bound-20260726.md` (Step 1 of
`docs/plans/performance-plan-20260725.md`) confirmed the main VM program's ~2.2× IPC penalty is
architectural — dependency chains and in-order pipeline depth, not memory latency — a wider
scheduling window is exactly the class of technique that addresses that specific penalty (unlike
`PRFM` prefetching or the memory-op scheduler extension, both of which target latency and were
closed by that same finding).

## The change

If the existing `i+2` candidate (R) doesn't qualify (hazards with P or with the instruction at
`i+1`), try a second candidate at `i+3` (R2) before giving up on P:

```
P, Q, R1, R2  →  P, R2, Q, R1
```

Moving R2 to fill P's stall slot changes the relative order of three pairs, not one: R2 must not
hazard with P (the reason for moving it), must not hazard with Q (which now follows it), and must
not hazard with R1 (which also now follows it). Q and R1 keep their original relative order, so no
new check is needed between them. The same anchor/barrier/`src==dst` exclusions that already apply
to Q and R1 (CBRANCH domain anchors, CFROUND/CBRANCH barriers, the `src==dst` exclusion from the
original scheduler's own bisected hazard) apply to R2 too — this widens how far the search looks,
it doesn't loosen any existing constraint.

Only `scheduleProgram()` (the main VM program's scheduler) was touched.
`scheduleSuperscalarProgram()` (the dataset-derivation path) is untouched — kept as a separate,
smaller-scope change to limit what needed re-verification in one pass.

## Validation

In order, each gating the next:

1. `test_jit_equivalence` (16 pairs, fast signal) — pass.
2. `test_jit_determinism`, `test_jit_encodings` — pass.
3. **`test_jit_scheduler_stress` (450 pairs, the differential test built specifically for this
   scheduler's hazard model)** — **450/450 pairs, all byte-identical.** This is the test that
   originally caught the `src==dst` hazard via bisection when the scheduler was first built; a
   clean pass here is the strongest available correctness signal short of a full audit.
4. `armrx_tests` (KAT hashes) — pass, byte-identical to the reference hashes.
5. `test_mining` (full mining engine lifecycle, including a bad-nonce-job recovery scenario) —
   pass.

## Performance

`perf stat -e cycles,instructions`, `bench_armrx --full-hash-only`, `taskset -c 0`, two
independent on-device samples, compared against the pre-change baseline (cycles 97,566,036,355 /
instructions 73,738,435,428, IPC 0.7558):

| Sample | Cycles | Instructions | IPC | Cycles Δ |
|---|---|---|---|---|
| Baseline | 97,566,036,355 | 73,738,435,428 | 0.7558 | — |
| Run 1 | 97,453,618,188 | 73,744,577,449 | 0.7567 | −0.115% |
| Run 2 | 97,390,075,589 | 73,744,586,086 | 0.7572 | −0.180% |
| **Average** | | | **0.7570** | **−0.148%** |

Instruction count is unchanged (+0.008%, noise-level, as expected — this only reorders emission,
it doesn't eliminate any instruction). **Average IPC improvement: +0.156%.** Both samples agree in
direction and are of similar magnitude to the original scheduler's own adopted win (+0.233% IPC,
2026-07-25) — small, but real and consistent, not a single noisy sample.

## Why this one worked when the memory-op extension and superscalar `IMUL_RCP` attempt didn't

Both of those attempts introduced a genuinely new hazard class the existing model didn't cover:
the memory-op extension needed to reason about dynamic scratchpad aliasing (unprovable at compile
time), and the superscalar `IMUL_RCP` attempt crossed a call boundary into another JIT program's
live register state that wasn't part of any documented contract. This change stays entirely within
the register-hazard model the scheduler already has — it doesn't add a new *kind* of reasoning,
it applies the same reasoning to one more candidate position. That's why the correctness argument
stayed tractable enough to validate in one pass.

## If revisited further

The same technique could in principle extend to `i+4`, `i+5`, etc. — each additional position adds
one more pairwise hazard check (the new candidate against everything it now precedes) but no new
*kind* of reasoning. Diminishing returns are likely: each successive candidate is less probable to
both qualify (independent of P) and be needed (P's stall window is only so long), and the
per-position combinatorial cost of hazard-checking grows. Not attempted here — this change stopped
at one additional position deliberately, to keep the increment small and reviewable, consistent
with how the original scheduler was built up in verified small steps rather than one large change.
