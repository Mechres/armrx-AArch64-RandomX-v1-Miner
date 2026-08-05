# 2026-08-07 — Hybrid path: hash during fill (kill the 172s dead-stop)

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
- TODAY that invariant does NOT hold: `fill_worker` publishes `item_count_ = max(completed)`
  = the END bound of its chunk (partial_dataset.cpp:196-203). Cross-chunk, a faster chunk B
  finishing [1M,2M) before chunk A [0,1M) advertises [0,2M) ready while [0,1M) is
  uninitialized → wrong hashes. This is audit C1. It is masked ONLY because `wait_for_fill`
  blocks all workers until `fill_complete_`.

## The fix (two coupled parts — neither alone is safe)
### Part 1 — Contiguous publish (eliminates C1; no behavior change by itself)
`item_count_` must advance only over the contiguous filled prefix, never the max-end-bound.
- Add `std::atomic<std::uint64_t> contiguous_done_{0}` and a `std::vector<std::atomic<bool>>
  chunk_done_` (one per fill chunk, sized at `start_fill`).
- In `fill_worker`, after `initialize_dataset` completes its chunk, set
  `chunk_done_[my_chunk] = true` (release), then a small CAS loop: while
  `chunk_done_[contiguous_done_]` is true, advance `contiguous_done_` (and publish
  `item_count_ = contiguous_done_` with release) until the first not-yet-done chunk.
- This makes `item_count_` ALWAYS = a fully-filled [0, item_count_) prefix → hash-during-fill
  is safe. `wait_for_fill()` still blocks (unchanged) so existing tests are unaffected.

### Part 2 — Hybrid hashes during fill (removes the dead-stop)
- In `worker_loop` (mining_engine.cpp:389-391), do NOT call `wait_for_fill()` when a partial
  dataset is active in HYBRID mode. The VM already has `set_partial_dataset` (line 560), so
  the JIT hit/miss handles correctness as the prefix grows. Workers hash immediately; early
  hashes mostly miss (derive on-the-fly, slightly slower), then ramp to mostly-hits as the
  prefix fills. The rolling-window `Speed:` shows this honestly (low→steady), unlike the old
  wrong empty-dataset ramp.
- The seed-rotation re-wait (worker_loop:476-485) is unchanged: on rotation, `set_job` bumps
  `partial_dataset_fill_generation_` and restarts fill; workers re-wait (the new fill's
  prefix starts at 0, so reading the old seed's stale prefix would be wrong — re-wait is
  still correct for rotation).
- `miner_app.cpp:246` `wait_for_fill()` (full-memory path) is unchanged (no partial dataset
  there in light mode items=0; for hybrid it's the same PartialDataset, so it will hash
  during fill too — fine).

### Part 3 — Fill-worker starvation (the 75s half-stall)
- `start_fill` is called with `exclude_cores={}` (mining_engine.cpp:255) → fill workers can
  land on the same cores as the (now hashing) miners, starving the fill on in-order A53.
- With Part 2 miners HASH (not park), so the parked-miner stall largely resolves, but to be
  safe: pin fill workers to a distinct core subset when `core_order_.size() > num_workers`,
  and cap fill-worker count to `min(avail_cores, chunks)` (already done). Leave pinning as-is
  for now; re-evaluate after on-device A/B. The 172s dead-stop is primarily Part 2; the
  half-stall is secondary.

## Correctness gate (MUST pass before Part 2 ships)
- **New KAT `test_partial_dataset_contiguous_publish`**: force one chunk to finish LATE (e.g.
  sleep in that chunk's `fill_worker` via a test hook, or run chunks with artificial delays),
  and while the fill is in progress, have a reader thread call the JIT hit/miss path (or
  directly read `item_count_` + `data_`) asserting that EVERY item `< item_count_` equals
  `generate_dataset_item(cache, i)` — i.e. no uninitialized byte is ever observable. This is
  the C1 regression guard.
- Host: `test_partial_dataset` + `test_mining` + `armrx_tests` (JIT 16/16) PASS.
- On-device: `time_partial_fill 512` (fill time) + `--pool-test --dataset-mb=512 --workers=7`
  should now show H/s > 0 IMMEDIATELY (within the first ~10s window), not after 172s. Compare
  steady-state H/s vs the post-fill 29.8 baseline (should match; hybrid perf gate −31%@8w
  still applies, but 7w should be ~29-30).

## Kill criterion
- Adopt Part 1 + Part 2 only if: (a) contiguous-publish KAT passes (no uninitialized read
  possible), (b) host ctest 9/9 + JIT 16/16 pass, (c) on-device `--dataset-mb=512` shows H/s
  > 0 within first 10s AND steady-state H/s matches the post-fill baseline (no regression).
- If the contiguous-publish KAT reveals a hole, fix Part 1 before Part 2.

## Discipline
- Part 1 (contiguous publish) is behavior-neutral (wait_for_fill still blocks) → can verify
  host-only first.
- Part 2 (relax wait_for_fill) is the behavior change → ONE device session, gated by the KAT.
- Do NOT touch the JIT `_end_hybrid` hit/miss logic (just fixed 2026-08-07; it's correct).
