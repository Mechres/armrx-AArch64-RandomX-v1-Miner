# 2026-08-07 — Hybrid dead-stop: 3 untried variants (branched attempts)

**Status:** CLOSED — all 3 variants REVERTED (honest negative, 2026-08-07). Part 1
(contiguous publish, `9881878`) is the safety net; Part 2+3 (`785ee69`) and all 3
variants below were tried and failed the kill criterion for the SAME root cause:
on an 8-core A53 with no free cores, you cannot start hashing early without
lowering the hit rate → lower steady H/s. The 172s `wait_for_fill` dead-stop is
the OPTIMAL steady-state choice; it is intended behavior, not a bug. See "Final
verdict" below. Branches kept (not merged, not deleted) as evidence of what was
tried, per the user's branch-discipline request.

## The physics (why every variant failed)
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

## Results (2026-08-07, agent-implemented, Hermes inspected the diffs)
- **Variant 1** (`5a2ce72`, tiered threshold): correct code (threshold polling on the
  contiguous prefix; plus a `local_partial_fill_gen` init fix). BUT on-device at
  threshold 0.5: first hash ~95s, steady 23.28 H/s, `fill_items` stalled at 4,194,304
  (exactly half) for most of the run. The fill is still core-starved (variant 1 did
  NOT change fill scheduling, only the start threshold) → fill never completes behind
  the workers → 50% hit rate the whole run. **REVERT** (same root cause as Part 2+3).
- **Variant 2** (`b4e2006`, co-located): the diff is 2 added COMMENT lines only. The
  `start_fill` call already passes `exclude_cores={}` on main (the stale comment above
  it claimed otherwise). So variant 2 == main; its "pass" (178s fill, 28.53 H/s) is the
  baseline. **CLOSE** (no-op, nothing to adopt).
- **Variant 3** (`044cd27`, smaller default): `dataset_mb = 0` → `256`. Real effect:
  dead-start ~83s (256 MiB) vs 172s (512 MiB). But agent's fill times were ~83s for
  64/128/256 MiB ALIKE (fill is core-starved, size-independent), and steady was
  26/27/27.4 H/s (lower hit rate than 512's 29.8). **REVERT for mining** (see below).

## Final verdict — H/s > start time, so the dead-stop is optimal (user decision)
The user correctly prioritized: **in a miner, steady-state H/s matters more than
start time.** The 172s dead-start is a one-time fixed cost; 29.8 H/s is earned every
second after. Every variant traded H/s for a shorter start and LOST:
- 172s + 29.8 H/s (main, `wait_for_fill`) = optimal for persistent mining.
- 83s + 27 H/s (variant 3) = 9% permanent hashrate loss to save 89s once. A net loss
  for any session longer than ~15 min. Variant 3 only "wins" inside a `--pool-test
  --seconds=200` window (cumulative H/s favors the shorter start) — that is a TEST
  ARTIFACT, not a mining win.
- Therefore: **do NOT adopt any variant. Keep main as-is** (`wait_for_fill`, hybrid
  OFF by default `dataset_mb=0`). The 172s dead-stop is INTENDED BEHAVIOR, not a bug;
  it maximizes steady-state H/s on an 8-core A53 with no free cores. The only way to
  cut it without losing H/s is a device with spare cores (2nd Unisoc target) or the
  clock/OPP unlock (~+44%).
- Branches `try/variant1-tiered-threshold`, `try/variant2-colocated-fill`,
  `try/variant3-smaller-dataset` are KEPT (not merged, not deleted) as evidence of
  what was tried, per the user's branch-discipline request. No change to main.
- If a shorter-looking start is still desired for `--pool-test` UX only (zero H/s
  impact), that is a display change (show "warming up, fill N%") — out of scope here.
