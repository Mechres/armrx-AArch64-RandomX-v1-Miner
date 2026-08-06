# 2026-08-07 — Superscalar generator timing-model A53 re-tune (E19→E22)

**Status:** OPEN — brief ready for a delegated agent. This is the top untried CODE
lever (`docs/audits/2026-08-07-improvement-headroot.md` §"THE real unmeasured lever").
The dataset-fill / hybrid / multiply levers are all CLOSED (per-family). The remaining
gap is STALLS, not instruction count or scaling: E19 measured `other_interlock_stall`
2.12× XMRig (= +12.30M cycles/hash, 154% of the 1w gap), localized to **A53 integer-
multiplier interlocks** on the dependency-dense superscalar body (~35% multiplies).

## The bug in the model (precise)
`src/superscalar.cpp` models **x86 ports**: `MacroOp::Imul_r`/`Mul_r` are latency 4,
ports `P1|P5` (line 88-89); `Imul_rr` latency 3, port `P1` (line 93). The generator
(`scheduleUop`, ~line 497) emits superscalar programs optimizing for x86's **TWO
multiply ports** (P1 + P5). But the **Cortex-A53 has ONE integer MUL pipeline** (3–4
cycle latency, non-pipelined for dependent chains). So on A53, every multiply contends
for the single MUL port and a dependent multiply chain stalls the whole body — exactly
the `other_interlock_stall` E19 found. The generated program order is optimal for x86
and ANTI-optimal for A53. RandomX's official `superscalar.cpp` is x86-tuned; armrx
inherited it unchanged.

## Why this lever is principled (not a guess)
- E19 isolated the stall class (multiplier interlocks) precisely via PMU.
- The diff vs upstream (`scratch_vm_study/upstream_rx/src/superscalar.cpp`) shows armrx
  matches upstream's x86 port model — so the delta is the *cycle/latency model*, not the
  program semantics. Changing the model changes WHICH program the generator emits (the
  generated program is *data*, not executable) — so JIT correctness is preserved by
  construction (the program still computes the same RandomX function), only ordering
  changes. Low risk to the JIT/VM; the existing JIT KATs (`armrx_tests` 16/16,
  `test_mining`, `test_partial_dataset`) still gate correctness.
- This attacks the EXACT stall class, not a proxy. Higher expected value than any
  fill/vectorization attempt (those are all CLOSED).

## Distinct designs (per-family gate — try all, close only if ALL fail)
### Design A — A53-accurate MUL model  [branch: try/superscalar-a53-mul-model]
Rewrite `MacroOp` MUL latencies/ports to A53: `Imul_r`/`Mul_r` = latency 4, SINGLE port
(e.g. a new `ExecutionPort::A53_MUL` used by all multiply MacroOps, so the scheduler sees
them as mutually exclusive on one port). `Imul_rr` latency 3 single MUL port. Add/Sub/Xor
stay P015 (A53 dual-issues those with the MUL). Re-run the generator's `scheduleUop`
optimization under the new single-MUL-port constraint → it should emit programs that
spread multiplies and interleave them with add/xor to avoid MUL-port contention.
- Gate: 1w H/s + `other_interlock_stall` A/B via gated `--perf-ready` harness; adopt if
  stall drops AND H/s ≥ parity (no regression). Byte-identical program OUTPUT still valid
  (KATs pass).

### Design B — Dependent-MUL-chain penalty (lighter)  [branch: try/superscalar-mul-chain-penalty]
Keep the x86 port model but add an A53-specific **penalty for back-to-back dependent
multiplies on the same register** in `scheduleUop` (so the generator avoids MUL chains
even if it doesn't fully model single-port). Smaller model change; tests whether the
chain-avoidance alone moves the stall.
- Gate: same as Design A (stall + H/s).

### Design C — Multiply-op decomposition re-tune  [branch: try/superscalar-mul-decomp]
The high-multiply ops are decomposed as `IMULH_R_ops_array = {Mov_rr, Mul_r, Mov_rr}`
(line 108) — 3 MacroOps, 2 through MUL. An A53 model might prefer a decomposition that
minimizes MUL-port occupancy or breaks the dependency (e.g. scheduling the `Mov_rr`
around other ops). Re-tune the `*_ops_array` decompositions under the A53 MUL model.
- Gate: same as Design A.

## Branch discipline (project rule)
From main @ `115806f`, create THREE branches:
- `try/superscalar-a53-mul-model`
- `try/superscalar-mul-chain-penalty`
- `try/superscalar-mul-decomp`
One design per branch, committed, NOT merged to main, NOT deleted on reject (evidence).
Hermes inspects each diff + on-device numbers.

## Correctness gate (all designs)
- Host: build + `test_partial_dataset` + `test_mining` + `armrx_tests` (JIT 16/16) PASS.
  The generated program must still hash byte-identically (the model change only reorders
  ops within the same RandomX semantics — if a design accidentally changes program
  SEMANTICS, the KATs will catch it; REVERT that design).
- On-device (lenovo, cross-built, one session per branch): 1w + 8w `bench_armrx` (or
  `--pool-test`) for H/s, plus `other_interlock_stall` via `perf stat` (the gated
  `--perf-ready` harness from `docs/experiments/`). Compare to the E19 baseline
  (2.12× XMRig).
- Adopt if `other_interlock_stall` drops toward XMRig AND H/s no regression.

## Kill criterion (PER FAMILY)
- Adopt any design that reduces the stall AND keeps H/s ≥ parity.
- CLOSE the lever ONLY IF all 3 designs fail (stall stays ~2× XMRig and/or H/s regresses).
- Report honest numbers per design.

## Discipline
- One device test/session per branch; separate scp/ssh; never qemu; kill stale procs.
- Do NOT modify the Part 1 contiguous publish or the JIT `_end_hybrid` hit/miss.
- Do NOT commit the audit docs in docs/audits/.
- Do NOT commit the verbatim agent prompt (one-time use; leave untracked / .gitignore).
- Report per-branch: diff scope, host KAT results, on-device H/s + `other_interlock_stall`.
