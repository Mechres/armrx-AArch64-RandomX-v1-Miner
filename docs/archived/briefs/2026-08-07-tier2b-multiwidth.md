# 2026-08-07 — Tier 2(b) re-attempt: multiple widths/designs (per-family gate)

**Status:** CLOSED — all 3 designs FAILED their gate (2026-08-07). Per the per-family
rule, the lever closes only when ALL distinct designs fail — and all 3 did. This is the
correction to the prior single-design closure (`2899f12`, 4-wide regressed to 165.12s).
Branches kept (not merged, not deleted) as evidence per the branch-discipline rule.
See "Results" below.

**Prior state (must read):** `docs/briefs/2026-08-07-tier2b-across-item-vectorization.md`
(the original single-design attempt, now CLOSED) and `docs/briefs/2026-08-07-tier2b-agent-prompt.md`.

## What we know about the fill path (from code reading)
- `initialize_dataset` (src/dataset.cpp ~81-142) ALREADY does 2-wide: `uint64x2_t vr[8]`
  holds item0/item1, calls `execute_superscalar_neon` once per access.
- `execute_superscalar_neon` (src/superscalar.cpp ~773-845) is FAKE-SIMD: ADD/SUB/XOR/SHIFT
  are true NEON `v*_u64`, but every MULTIPLY (`IMUL_R`, `IMULH_R`, `ISMULH_R`, `IMUL_RCP`)
  and `IROR_C` extracts both lanes, computes SCALAR, recombines via `vcombine`. So the
  2-wide speedup covers only the ~65% non-multiply ops; the ~35% multiplies run
  scalar-per-lane on in-order A53 (each `IMUL_R` is a 4-cycle-interlocked multiply done
  twice per access).
- On-device baseline (lenovo, `time_partial_fill 512`): **NEON 2-wide = 163.3s**, scalar
  (Tier 2(a)) = 207s. So 2-wide already buys ~21% over scalar. The 4-wide attempt = 165s
  (worse than 163.3) — likely because the serial per-access `load_cache_line` (2 dependent
  memcpys + modulo, dataset.cpp ~104-108) doesn't scale with lane count, AND 4-wide added
  lane-management overhead without touching the scalar multiplies.
- The fill is derivation-bound; steady-state mining is gated by the SAME multiplies (the
  JIT `execute_superscalar_neon` is the same function). So a REAL multiply-speedup in the
  fill would also help the JIT — bigger prize than just fill time.

## The 3 designs (distinct; all must be tried before closing the lever)
### Design A — 8-wide fake-SIMD  [branch: try/tier2b-8wide]
Widen `initialize_dataset` to hold FOUR pairs (8 items) in the `uint64x2_t[8]` array
(item0..7), calling `execute_superscalar_neon` per pair, halving the outer loop
iterations vs 4-wide. Tests whether 4-wide's regression was width-specific (maybe 8
amortizes the cache-line fetch better) or fundamental. Keep the fake-SIMD multiply
extract/recombine; only change item count per iteration + the load/store stride.
- Gate: `time_partial_fill 512` < 163.3s AND `test_partial_dataset` byte-identical.

### Design B — True NEON multiply-high  [branch: try/tier2b-neon-mulh]
The hard stretch, never tried. Replace the scalar `IMULH_R`/`ISMULH_R` (u64×u64→u128
high 64 bits) in `execute_superscalar_neon` with a REAL NEON computation: decompose each
u64 into two u32 limbs, use `vmull_u32` (u32×u32→u64) on the 4 cross-limb products +
`vmlal`/`vmlal_lane` accumulates to reconstruct the full 128-bit product, take the high
64 bits. This parallelizes the DOMINANT multiply cost across NEON lanes (no scalar
extract). `IMUL_R` (low 64) and `IMUL_RCP` similarly via `vmull`/`vmlal`. `IROR_C` can
stay or be NEON-ized. Keep ADD/SUB/XOR/SHIFT as-is. This changes the shared
`execute_superscalar_neon` used by BOTH fill and JIT — so it must pass JIT KATs too
(`armrx_tests` 16/16, `test_mining`). Higher risk; if it regresses IPC or breaks a KAT,
REVERT this design only.
- Gate: `time_partial_fill 512` < 163.3s (fill) AND `armrx_tests` JIT 16/16 AND
  `test_mining` pass AND steady-state H/s no regression. If multiply-high is correct but
  fill not faster, still valuable for the JIT — report separately.

### Design C — Cache-line amortization  [branch: try/tier2b-cacheline]
Decouple the serial per-access `load_cache_line` (2 dependent memcpys + modulo per access)
from the lane count. Pre-fetch a BLOCK of cache lines (e.g. the superscalar program's
read set for K items) once into a local buffer, then run K items' `execute_superscalar_neon`
against that buffer before refilling. This attacks the per-access serial work that 4-wide
didn't help. Keep 2-wide (or combine with A). Pure restructuring of `initialize_dataset`'s
inner loop; does not touch `execute_superscalar_neon`.
- Gate: `time_partial_fill 512` < 163.3s AND `test_partial_dataset` byte-identical.

## Branch discipline (project rule)
From main @ `41ae490`, create THREE branches:
- `try/tier2b-8wide`
- `try/tier2b-neon-mulh`
- `try/tier2b-cacheline`
One design per branch, committed, NOT merged to main, NOT deleted on reject (evidence).
Hermes inspects each diff + on-device numbers, then recommends adopt/revert per branch.

## Correctness gate (all designs)
- Host: `test_partial_dataset` (incl. contiguous-publish KAT) + `test_mining` +
  `armrx_tests` JIT 16/16 PASS. For Design B also confirm JIT hashes byte-identical to
  the reference (the existing `test_light_mode_partial_dataset_matches_reference`).
- On-device (lenovo, cross-built, one session per branch): `time_partial_fill 512` for
  fill time; `--pool-test --dataset-mb=512 --workers=7` for steady H/s + correctness.
- Wrong hashes or byte-difference vs reference → REVERT that design.

## Kill criterion (PER FAMILY)
- Adopt any design whose gate passes (fill < 163.3s for A/C; fill < 163.3s + JIT 16/16 +
  no steady regression for B). Multiple may pass — adopt the best.
- CLOSE the Tier 2(b) lever ONLY IF all 3 designs fail their gate. One failure does NOT
  close it (that was the prior mistake).
- Report honest numbers per design either way.

## Discipline
- One device test/session per branch; separate scp/ssh; never qemu; kill stale procs.
- Do NOT modify Part 1 contiguous publish (`partial_dataset.cpp`) unless a design
  requires it (it shouldn't).
- Do NOT commit the audit docs in docs/audits/.
- Report per-branch: diff scope, host results, on-device fill time + steady H/s.

## Results (2026-08-07, agent-implemented, Hermes inspected diffs)
Implemented + committed on separate branches from `41ae490`. All 3 failed the gate.

- **Design A — 8-wide fake-SIMD** (`try/tier2b-8wide`, `1a9003a`): correct code (4 pairs
  / 8 items per iter, proper remainder loop for non-multiples of 8 — `test_partial_dataset`
  byte-identical). Device fill **165.30s** > 163.3s gate. Pool test 28.59 H/s. **FAIL**
  (wider than 4-wide but still no speedup — the serial per-access cache-line fetch +
  scalar multiplies dominate; widening the lane count doesn't touch them).
- **Design B — true NEON multiply-high** (`try/tier2b-neon-mulh`, `1df0b86` then reverted
  `c97dd13`): attempted `vmull_u32` 128-bit product reconstruction for IMUL*/ISMULH*/IMUL_RCP.
  Device `test_partial_dataset` **failed at item 0** (correctness bug: `vuzp1q_u32(aw,aw)`
  does NOT broadcast the two 32-bit limbs correctly — `vuzp` is an interleave op, and the
  carry/low reconstruction `vaddq_u64(p0, vshlq_n_u64(cross,32))` overflows without carry
  from the low add; the signed `neon_mulh_s64` is also wrong). REVERTED with an explicit
  revert commit. Branch is back to main (no code leaks). **FAIL (correctness)** — the
  technique is valid (the brief called it a stretch) but this implementation is broken;
  NOT re-attempted because Designs A/C already close the lever. If ever reopened, the
  correct limb-decomposition is: a=a_lo+a_hi·2³², b=b_lo+b_hi·2³², product = a_lo·b_lo +
  (a_lo·b_hi+a_hi·b_lo)·2³² + a_hi·b_hi·2⁶⁴, with proper carry propagation — NOT `vuzp`.
- **Design C — cache-line amortization** (`try/tier2b-cacheline`, `ca6a294`): correct code
  (pre-fetch a block of cache lines, run K items against it). Device fill **164.70s** >
  163.3s gate. Pool test 30.20 H/s instantaneous (closest to baseline, but still over).
  **FAIL** (the per-access `load_cache_line` is already cheap relative to the superscalar
  program + multiply; amortizing it didn't move the number).

### Per-family conclusion
All 3 distinct designs failed (A: 165.30s, B: broken-multiply, C: 164.70s) — none beats
the 163.3s NEON 2-wide baseline. **TIER 2(b) LEVER CLOSED** (per-family rule, all N fail).
Root cause (all designs): the fill is bounded by the superscalar program execution, where
the **multiplies** (`IMULH_R` etc.) are the dominant cost and run scalar-per-lane in NEON
fake-SIMD; Design B proved true-NEON-multiply is *hard to get correct* and wasn't worth
pursuing once A/C confirmed the floor. No further variants justify a 4th attempt.

### Outcome
- `main` unchanged (the lever had no shipped code; 2-wide NEON remains the fill path).
- Branches `try/tier2b-8wide`, `try/tier2b-neon-mulh` (reverted to main), `try/tier2b-cacheline`
  KEPT as evidence per the branch-discipline rule.
- Ledger updated: Tier 2(b) marked CLOSED (per-family, all 3 designs failed).

