# 2026-08-07 — Hybrid dead-stop: 3 untried variants (branched attempts)

**Status:** OPEN — brief + verbatim agent prompt ready. Part 1 (contiguous publish,
`9881878`) is the safety net; Part 2+3 (instant-start + exclude-all-miner-cores) was
REVERTED because excluding 7 miner cores left 1 fill core → cache starved → 23.16 H/s
(honest negative, `785ee69`). The *lever* ("cut the 172s dead-stop's cost") is NOT
exhausted — only one design of it was tried. This brief specifies 3 distinct variants,
each on its own branch, so the code is never deleted on revert and Hermes can inspect it.

## The physics (why Part 2+3 failed)
On an 8-core A53 with all cores needed for 29.8 H/s, any core given to the fill is a
core taken from hashing. The fill is derivation-bound and slow (~164s for 512 MiB).
- Block until 100% (`wait_for_fill`, current): 172s dead, then 29.8. Optimal for
  *persistent* mining.
- Block until 0% + exclude miners (Part 2+3): instant start, but 1 fill core →
  all-miss → 23.16. Worse.
- The untried middle/cheaper designs below avoid both extremes.

## Variant 1 — Tiered `wait_for_fill(threshold)` [PRIMARY]
Block workers until the contiguous prefix reaches `threshold × N` (e.g. 50%), then
start. Workers hash immediately with a `threshold` hit-rate; the rest fills behind them.
- Correctness: GUARANTEED by Part 1 contiguous publish — at threshold T, items
  `[0, T×N)` are fully filled (safe hits); items ≥ T×N miss and derive on-the-fly.
  No new safety risk.
- Dead-time: ~86s at 50% (half of 172s). Steady-state ramps 26→29.8 (not flat 23).
- Implementation: add `double fill_start_threshold = 0.5` to engine; `worker_loop`
  polls `partial_dataset_->item_count() >= threshold*allocated` instead of
  `wait_for_fill()` (or a `wait_for_fill(threshold)` method). Seed-rotation re-wait
  UNCHANGED. Add `--fill-start-threshold=N` CLI (default 1.0 = current behavior).
- Predicted: dead-time ~86s, steady ~28-29.8. Should PASS kill criterion (start <
  ~150s AND steady ≥ 28).

## Variant 2 — Co-located fill (no exclude_cores) [FALLBACK]
Call `start_fill` with `exclude_cores={}` (all cores) and let the OS scheduler share
fill threads across all 8 cores alongside the miners (no pinning of fill to a subset).
- Risk: fill gets a thin slice of every core → may take much longer (688s+) and the
  cache fills slowly → still mostly misses. But UNTESTED; on a non-isolated device the
  scheduler may balance better than the model assumes. Low code cost.
- Keep Part 1 safety. If fill is still crawling at, say, 300s, REVERT.

## Variant 3 — Smaller default `--dataset-mb` [COMPANION, trivial]
The 172s scales with dataset size. At 128 MiB ≈ 43s, 64 MiB ≈ 20s. Trade hit-rate
(128/2080 ≈ 6% vs 512/2080 ≈ 25%) for a short dead-stop. No new code beyond a default
change; could pair with Variant 1.
- For a `--pool-test` that self-terminates at 200s, 43s dead-start hashes ~4× more of
  the window than 172s.
- Implementation: lower the implicit default when `--dataset-mb` is auto-selected, or
  document 128 as the recommended size. Measure fill time + steady H/s at 128/64/256.

## Branch discipline (so code is never lost)
Agent creates THREE branches off current HEAD (main @ `785ee69`):
- `try/variant1-tiered-threshold`
- `try/variant2-colocated-fill`
- `try/variant3-smaller-dataset`
Implements ONE variant per branch. Does NOT merge to main. Leaves each branch
non-reverted (committed on the branch) so Hermes can `git diff main...branch` and
inspect. Reports host + on-device numbers per branch.

## Correctness gate (all variants)
- Host: `test_partial_dataset` (incl. contiguous-publish KAT) + `test_mining` +
  `armrx_tests` JIT 16/16 PASS.
- On-device (lenovo, cross-built, one session per branch): `time_partial_fill 512`
  (or the variant's size) + `--pool-test --dataset-mb=N --workers=7`.
- Variant 1 kill criterion: dead-time (first H/s > 0) < ~120s AND steady-state ≥ 28
  H/s. Variant 2: fill completes in reasonable time AND steady ≥ 28 (else REVERT).
  Variant 3: fill time + steady H/s at each size; adopt the size with best
  window-utilization.

## Discipline
- One device test/session per branch; separate scp/ssh; never qemu; kill stale procs.
- Do NOT touch JIT `_end_hybrid` or Part 1 contiguous publish.
- Do NOT commit the audit docs in docs/audits/.
- Report per-branch: diff scope, host results, on-device dead-time / steady H/s /
  fill_items behavior. Hermes gates adopt/revert per branch after inspecting.
