# T3-3: IMUL_R Operand-Magnitude Gate Check — FAIL (2026-08-01)

Gate item T3-3 from `docs/audits/combined-audit-20260731.md` (row 10): determine
whether RandomX IMUL_R (64×64→64) operands are narrow often enough to justify a
NEON lane-parallel transform. Gate criterion (from the audit): **proceed only if
≥30% of executed IMUL_R instances have one operand ≤ 2^32** (unsigned).

Result: **FAIL — 0.003% of 74,079,016 genuine IMUL_R executions have one operand
≤ 2^32.** The NEON lane-pack premise is falsified; F3's follow-on is closed on
evidence. (The earlier "✅ gate check done" mark was an error — see
`changelogs.md` 2026-08-01 / commit c29dd89; this document is the real gate.)

## Why static sampling cannot answer this gate

IMUL_R has **no immediate — both operands are runtime register values**. The
opcode-frequency tool `bench_opcodes` samples *program generation* (JIT dump) and
is JIT-gated (does not even build on x86_64). The gate therefore required
**instrumented execution**: hooking the interpreter's IMUL_R case to record the
actual pre-multiply src/dst values. The interpreter is semantically byte-identical
to the JIT (that is what `test_jit_equivalence` verifies), so the gate runs on the
host — the result is a property of RandomX's runtime data distribution.

## Implementation (by Reasonix from a Hermes brief; verified by Hermes)

- `include/armrx/vm.hpp` — `setImulSampleCallback(fn, ctx)`: public setter, private
  null-default pair (zero-cost when unset).
- `src/vm.cpp` `execute_bytecode()` IMUL_R case — captures pre-multiply src/dst,
  fires the callback with `ibc.isrc == &ibc.imm` as the RCP discriminator, multiply
  semantics unchanged.
- **RCP segregation (critical):** `h_IMUL_RCP` lowers `IMUL_RCP` to
  `InstructionType::IMUL_R` with `ibc.isrc = &ibc.imm` (constant reciprocal,
  ~full-width — known useless for lane packing). Pointer-identity distinguishes
  RCP-lowered from genuine IMUL_R; RCP is counted separately and excluded.
- `tests/bench_imul_magnitudes.cpp` — self-checking sampler: 40 seeds × N hashes,
  tiered one/both-operand fractions (unsigned 2³²/2¹⁶/2⁸, signed 31/15), per-seed
  variance, `GATE: PASS/FAIL` line. Deterministic inputs, interpreter-only.
- `CMakeLists.txt` — registered outside the JIT guard; ctest entry `8 20`.

## Self-check (hook is observation-only)

Seed 0, hash 0 run once with the callback disabled and once enabled → the two
32-byte outputs are **byte-identical** (reported PASS by the tool). The hook
cannot change VM semantics; it is also null-defaulted in production paths.

## Results (host, 40 seeds × 8 hashes = 320 hashes, 56.3 s)

| Quantity | Value |
|---|---|
| Genuine IMUL_R executions | 74,079,016 |
| RCP-lowered executions (excluded) | 52,129,903 |
| one operand ≤ 2^32 (unsigned) | 2,222 — **0.003%** |
| both operands ≤ 2^32 | 217 — 0.0003% |
| one operand ≤ 2^16 | 0.0015% |
| one operand ≤ 2^8 | 0.0014% |
| one operand \|v\| ≤ 2^31 (signed) | 2,352 — 0.003% |
| per-seed one≤2³² fraction | min 0.00% / max 0.03% / mean 0.00% |

Independent confirmation: Reasonix's own run (1,600 hashes, 367.8M genuine
IMUL_R) measured **0.003%** — identical. Cross-run and cross-seed stable.

## Analysis

- Runtime register values are effectively full-width random 64-bit: 0.003% is
  ~65,000× above uniform-random expectation (2^-32 ≈ 2.3×10^-8 × 2 operands)
  but ~10⁴× below the 30% gate.
- The RCP tail (which produces small quotients) was hypothesized as the main
  narrow-operand source — it is real (52.1M executions, weight 8/256) but
  *excluded by design*: its constant reciprocal src is full-width, and F3 already
  ruled the schoolbook `umull` route out.
- Per-seed max 0.03% vs mean 0.00% → no seed dependence; the FAIL is structural
  (RandomX's 64-bit arithmetic produces full-width results by construction).

## Decision

**GATE FAIL — close T3-3 and the F3 lane-pack follow-on permanently.** A NEON
`mul v.4s` lane-parallel IMUL_R transform requires narrow operands it will almost
never see; the packing/masking overhead would additionally violate the project's
standing "added instructions cost real cycles on A53" law (T2-1, T2-2). No
design work (audit W3-4) proceeds.

The sampler tool + VM hook remain in place: `./build/bench_imul_magnitudes [seeds]
[hashes]` — reusable for any future operand-distribution question (e.g. W2-2
scratchpad adjacency uses a different hook, but the pattern generalizes).

## Cross-references

- `docs/experiments/f3-neon-mul-latency-test.md` — premise (NEON mul ≈1 CPI,
  schoolbook ruled out); its open design question is now closed by this FAIL
- `docs/audits/combined-audit-20260731.md` row 10 / T3-3 section
- `docs/audits/performance-audit-work-ideas-20260801.md` W1-2 (gate spec), W3-4
  (blocked → now closed), D14 (status correction history)
- Files: `src/vm.cpp`, `include/armrx/vm.hpp`, `tests/bench_imul_magnitudes.cpp`,
  `CMakeLists.txt`
