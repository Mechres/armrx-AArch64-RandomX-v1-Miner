# T2-2: Dual-Issue-Aware Instruction Alignment — Measured Regression (2026-08-01)

Gate item T2-2 from `docs/audits/combined-audit-20260731.md` (row 7, status "Ready",
projected ~0.1–0.5%): pad long-latency op emissions to 8-byte boundaries so the
following instruction can dual-issue with them on the in-order 2-way superscalar
Cortex-A53. Result: **measured regression — reverted**. The mechanism *works* (IPC
rose), but the NOP overhead exceeds the alignment gain.

## What was tried

`emitPadTo8()` — a file-local helper emitting the canonical NOP (`0xD503201F`) when
`(k - PrologueSize) & 4 != 0` — inserted immediately before the long-latency
instruction at 8 sites in `src/jit_compiler_a64.cpp` (main-VM path only):

- `h_IMUL_R` (both normal and `src==dst`/x20 paths converge on one emission site)
- `h_IMULH_R`, `h_ISMULH_R`
- `h_IMUL_RCP` (both literal and `LDR_LITERAL` paths)
- `emitMemLoad<>` (both `src != dst` and `src == dst` branches, before the final LDR)
- `emitMemLoadFP<>` (before the FP LDR)

Parity base: `PrologueSize` (0x1E0 = 480 bytes, ≡ 0 mod 8), verified equal for both
`generateProgram()` and `generateProgramLight()`, so the relative test coincides with
the absolute `k & 4` test; the relative form was kept per the measurement mandate.
The superscalar emitter, dataset-item code, and all other paths were left untouched.

Implementation (+61 lines) by Reasonix from a Hermes brief; host sanity gate
`test_mining` passed. The change was **reverted** (`git checkout`) after the A/B.

## Device A/B — protocol

4 runs, B-P-B-P, `taskset -c 3 perf stat -e cycles,instructions,branches,branch-misses,l1d_cache_refill`
on `bench_armrx --full-hash-only`, per-run md5 verification (baseline
`257e6d14…`, padded `0c784a72…`). Two earlier A/B attempts were invalidated by a
harness bug (missing dir + clobbered stash caused all runs to execute the baseline —
see "Harness lesson" below); v3 with in-loop md5 proof produced the data below.

## Results

| run | md5 | cycles | instructions | IPC | wall µs/hash |
|---|---|---|---|---|---|
| B1 | baseline `257e6d14` | 90,782,258,173 | 66,346,635,796 | 0.7308 | 206,475 |
| P1 | padded `0c784a72` | 90,895,545,340 | 66,659,911,265 | 0.7334 | 206,623 |
| B2 | baseline `257e6d14` | 90,763,007,933 | 66,349,828,144 | 0.7310 | 206,319 |
| P2 | padded `0c784a72` | 90,941,556,527 | 66,659,901,550 | 0.7330 | 206,650 |

Deltas (padded vs adjacent baseline):

| metric | P1 vs B1 | P2 vs B2 |
|---|---|---|
| cycles | **+0.125%** | **+0.197%** |
| instructions | +0.472% | +0.467% |
| IPC | +0.36% | +0.27% |
| wall µs/hash | +0.072% | +0.160% |
| l1d_cache_refill | −1.7% | +0.1% (noise) |
| branch-misses | +1.9% | +2.1% (NOP-shifted branch alignment) |

## Mechanism

The padding **worked as designed**: IPC rose +0.27–0.36% in both padded runs —
the aligned [long-latency op, follower] pairs genuinely dual-issued more often.
But the NOP overhead (+0.47% instructions, reproducible to 0.005% across seeds)
exceeds the IPC recovery, netting **+0.13–0.20% more cycles** (slower). This is
the same verdict as T2-1 (PRFM hints) and the audit's 94%-architectural-stall
conclusion: on in-order A53 the main-VM stall is dependency-chain-bound, and
*any* added instruction costs real cycles — even one designed to dual-issue away
for free, because its residual cost (issue slot + I-cache footprint) lands inside
the already-bound pipeline.

## Decision

**REVERT.** Gate rule from the brief: adopt only if cycles improve ≥0.3% in both
padded runs (failed — cycles worsened in both) AND instruction overhead ≤0.5%
(passed, moot). No variant-hopping (no `AND xzr` substitute, no wider padding):
one measurement, one decision.

Reverted cleanly: `git checkout -- src/jit_compiler_a64.cpp`; cross rebuild
bit-reproduced the baseline binary (md5 `257e6d14…` identical to the pre-change
snapshot), device `/tmp/cross/bench_armrx` restored, tree clean at `05f4c70`.
JIT correctness gates were not re-run for the rejected build (no adoption ⇒ no
need to validate discarded code; the restored baseline is already KAT-verified).

## Forward pointer (why not to retry this casually)

The +0.3% IPC gain proves alignment *can* improve dual-issue — the lost
opportunity is not alignment per se but the NOP's cost. A zero-instruction-cost
way to exploit it (filling the pad slot with a *useful* instruction hoisted from
later in the program) is exactly the T3-2 load-hoisting / scheduler territory,
already gated HIGH-risk (shared live register x2, historical unexplained
divergence). Do not retry without the T3-2 gates.

## Harness lesson (measurement discipline)

Two A/B attempts produced 8 worthless baseline runs before the bug was found:
(1) the stash dir `/tmp/cross-padded/` was never created, so the "preserve" cp
silently failed; (2) the preserve step copied from `/tmp/cross/` — which still
held baseline residue from the first attempt's cp — re-clobbering the uploaded
padded binary. **A measurement harness must log the md5 of the exact binary
`perf stat` executes, per run, and abort on source mismatch** — v3 did this and
is the only trustworthy run. Applies to all future device A/Bs.

## Cross-references

- `docs/experiments/t21-prfm-hints.md` — sibling dead end (PRFM), same verdict
- `docs/audits/combined-audit-20260731.md` row 7 — gate item, now closed
- `RETROSPECTIVE.md` — prefetch-removal +0.885% IPC (largest code-level win),
  consistent with "added instructions cost cycles" theme
