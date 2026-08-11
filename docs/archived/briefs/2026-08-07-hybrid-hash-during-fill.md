# 2026-08-07 — Hybrid path: hash during fill (kill the 172s dead-stop)

**Status:** PART 1 DONE (committed `9881878`, host 9/9 PASS, C1 closed). Part 2+3
CLOSED — REVERTED (honest negative, 2026-08-07). Attempted by external agent, gated
by the brief's kill-criterion. The `wait_for_fill` dead-stop is CORRECT behavior on
this device, not a bug (see "Part 2+3 result" below). Delegated per the project's
Hermes-authored-brief / user-runs-agent / Hermes-gates-on-device discipline. Verbatim
prompt: `2026-08-07-hybrid-part2-agent-prompt.md`.

**Severity:** correctness-adjacent + UX (the 172s `Speed: 0.00 H/s` dead-stop on
`--dataset-mb=N`). Root cause of the dead-stop: Tier 1 `wait_for_fill()` blocks ALL
mining workers until the partial dataset is 100% filled, defeating the hybrid design's
whole point (the JIT hit/miss is supposed to read whatever prefix is filled and derive
the rest on-the-fly).

**Observed (user, lenovo, 2026-08-07):** `./armrx --pool-test --dataset-mb=512 --workers=7`
→ `Speed: 0.00 H/s` for **172 s** (fill ran 7s-start but completed at ~172s; `fill_items`
stuck at exactly 4,194,304 = half for ~75 s — a fill-worker/core-contention stall). Only
after fill_complete did H/s jump to ~29.8. So the hybrid feature gave a 3-minute dead-start
instead of instant start.

## Why wait_for_fill is wrong for the hybrid path
- The JIT reads `partial_dataset_item_count_ptr_` **fresh every hash** (vm.hpp:90-91,
  `memory_order_acquire`). Hit = `item < item_count_` → read cached (filled) bytes; miss →
  derive on-the-fly. So per-hash, hit/miss is self-consistent with the *current* prefix.
- Therefore workers CAN hash during fill safely — IF `item_count_` only ever advertises a
  **fully-filled contiguous prefix** [0, item_count_).
- **This invariant NOW HOLDS** (fixed by Part 1, commit `9881878`): `fill_worker` advances
  `item_count_` only over the contiguous filled prefix via a `contiguous_done_` cursor +
  per-chunk `chunk_done_` flags. The old max-end-bound publish (audit C1) is eliminated and
  verified by `test_contiguous_publish_no_uninitialized_read` (4 chunks, middle chunk
  lagged 500ms → `item_count_` held at 1 chunk during the lag). So the safety precondition
  for hashing during fill is satisfied. `wait_for_fill()` still blocks today, so the
  dead-stop persists until Part 2 relaxes it.

## The fix (Part 1 done; Part 2+3 remaining)
### Part 1 — Contiguous publish (DONE, committed `9881878`)
`item_count_` now advances only over the contiguous filled prefix, never the max-end-bound
(C1 closed). `wait_for_fill()` still blocks (unchanged) so current miner behavior is
identical. Host `ctest` 9/9 PASS. No on-device run needed (behavior-neutral).

### Part 2 — Hybrid hashes during fill (removes the dead-stop) — AGENT TASK
- In `worker_loop` (mining_engine.cpp:389-391), do NOT call `wait_for_fill()` when a partial
  dataset is active in HYBRID mode (i.e. `partial_dataset_` is non-null AND mode is light with
  `--dataset-mb>0`). The VM already has `set_partial_dataset` (line ~560), so the JIT hit/miss
  handles correctness as the prefix grows. Workers hash immediately; early hashes mostly miss
  (derive on-the-fly, slightly slower), then ramp to mostly-hits as the prefix fills. The
  rolling-window `Speed:` shows this honestly (low→steady), unlike the old wrong empty-dataset
  ramp.
- The seed-rotation re-wait (worker_loop:476-485) is UNCHANGED: on rotation, `set_job` bumps
  `partial_dataset_fill_generation_` and restarts fill; workers re-wait (the new fill's prefix
  starts at 0, so reading the old seed's stale prefix would be wrong — re-wait is still correct
  for rotation). KEEP THIS.
- `miner_app.cpp:246` `wait_for_fill()` (full-memory / non-hybrid path) is UNCHANGED. Only the
  per-worker `worker_loop` hybrid branch should skip the block.
- **Guard:** only skip the block on the hybrid path. For `partial_dataset_ == nullptr` (pure
  light, `--dataset-mb=0`) or non-hybrid, keep `wait_for_fill()` as-is.

### Part 3 — Fill-worker starvation (the 75s half-stall) — AGENT TASK
- `start_fill` is called with `exclude_cores={}` (mining_engine.cpp:255) → fill workers can
  land on the same cores as the (now hashing) miners, starving the fill on in-order A53.
- With Part 2 miners HASH (not park), so the parked-miner stall largely resolves, but to be
  safe: pass `exclude_cores` = the set of cores the miner workers will occupy (so fill workers
  use the remaining cores), OR cap fill-worker count and pin them to distinct cores. Concrete
  approach: in `set_job`, compute `miner_cores` (the cores `worker_loop` will pin to under the
  active AffinityMode) and pass them as `exclude_cores` to `start_fill`. Re-evaluate after
  on-device A/B. The 172s dead-stop is primarily Part 2; the half-stall is secondary but the
  log showed `fill_items` stuck at exactly HALF (4,194,304) for ~75s, so it is real.

## Correctness gate (MUST pass before Part 2 ships)
- **Part 1 KAT `test_contiguous_publish_no_uninitialized_read`** already PASSES host-side
  (C1 closed). The agent must NOT regress it.
- Host: `test_partial_dataset` + `test_mining` + `armrx_tests` (JIT 16/16) PASS.
- On-device (lenovo, cross-built): `time_partial_fill 512` (fill time) +
  `--pool-test --dataset-mb=512 --workers=7` should now show H/s > 0 IMMEDIATELY (within the
  first ~10s window), not after 172s. Compare steady-state H/s vs the post-fill 29.8 baseline
  (should match; hybrid perf gate −31%@8w still applies, but 7w should be ~29-30). Also confirm
  `fill_items` no longer stalls at half for 75s (Part 3).

## Kill criterion
- Adopt Part 2 + Part 3 only if: (a) host `test_partial_dataset` (incl. contiguous-publish KAT)
  + `test_mining` + `armrx_tests` JIT 16/16 PASS, (b) on-device `--dataset-mb=512 --workers=7`
  shows H/s > 0 within first 10s AND steady-state H/s matches the post-fill baseline (no
  regression), (c) `fill_items` no longer stalls at half for ~75s.
- If the on-device run shows H/s still 0 for the full fill, or wrong hashes, REVERT (the
  contiguous publish is the safety net; if it holds, no wrong hashes — so a 0 H/s that isn't a
  stall is a different bug, report it).

## Discipline
- Part 1 is DONE (behavior-neutral, host-verified). Do NOT re-touch contiguous publish unless
  the KAT fails.
- Part 2+3 are the behavior change → ONE device session, gated by the KAT + on-device A/B.
- Do NOT touch the JIT `_end_hybrid` hit/miss logic (fixed 2026-08-07; correct, verified).
- One test per session on-device; separate scp and ssh; never qemu.

## Part 2+3 result (2026-08-07 — REVERTED, honest negative)
Attempted by external agent, gated by the kill-criterion. Host gates passed
(test_partial_dataset incl. contiguous KAT, test_mining, armrx_tests JIT 16/16).
On-device (lenovo, cross-built):
- Fill time **163.88s** (Part 3's `exclude_cores` fixed the 75s half-stall — fill
  now completes cleanly instead of stalling at half).
- Hashing began at **~12s** (Part 2 worked — instant start achieved).
- **BUT steady-state was 23.16 H/s, below the 29–30 required baseline → REVERT.**
- Root cause: excluding the 7 miner cores left **only 1 core** for the fill → a
  single fill thread deriving the whole 512 MiB, starved by 7 hashing workers. The
  cache stayed near-empty, so most hashes MISSED and derived on-the-fly (the slow
  path) — hybrid effectively became pure light-mode derivation, ~22% slower than
  waiting for a 100%-populated cache (29.8 H/s).
- **Conclusion:** on an 8-core A53 with all cores already committed to hashing, the
  172s `wait_for_fill` dead-stop is the *optimal* strategy, not a bug. Deriving the
  dataset is more expensive than the idle time saved by not waiting; there are no
  free cores to fill in parallel without starving either the fill or the hashers.
  Part 2+3 is CLOSED. Do NOT re-attempt on ≤8-core devices. The only way it could
  win is a device with spare cores (the 2nd Unisoc target, or a big.LITTLE with idle
  big cores) — out of scope here.
- No code retained; the two audit docs in docs/audits/ were left untouched.
