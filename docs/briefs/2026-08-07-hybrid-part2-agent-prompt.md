# VERBATIM AGENT PROMPT — Hybrid fill: hash during fill (Part 2+3)

You are implementing a performance/UX fix in the `armrx` AArch64 RandomX miner
(repo: /home/mechres/Projeler/aarch64-randomx). This is a C++ / CMake project
cross-compiled for aarch64 (Cortex-A53, 8 cores, two clusters). Part 1 of this
work is ALREADY DONE AND COMMITTED (`9881878`) — do NOT redo it. You implement
Part 2 + Part 3 only. A senior engineer (Hermes) wrote the brief and will
independently re-verify your changes on-device before they are adopted.

## Context you must read first (do not skip)
- `docs/briefs/2026-08-07-hybrid-hash-during-fill.md` — the full brief. It
  documents Part 1 (DONE), Part 2, Part 3, the correctness gate, and the kill
  criterion. Read it fully before writing any code.
- `include/armrx/partial_dataset.hpp` and `src/partial_dataset.cpp` — Part 1
  already added `contiguous_done_`, `chunk_done_`, `items_per_chunk_`, and a
  `chunk_delays_ms` test hook to `start_fill`. The contiguous-publish logic in
  `fill_worker` is done and KAT-verified. DO NOT modify the contiguous-publish
  logic unless the KAT fails (it shouldn't).
- `src/mining_engine.cpp` — `worker_loop()` (around lines 380-391) calls
  `partial_dataset_->wait_for_fill()` unconditionally when `partial_dataset_` is
  set. `set_job()` (around line 255) calls `start_fill` with `exclude_cores={}`.
- `src/vm.hpp` — `set_partial_dataset` (line ~92) wires the JIT to the atomic
  `item_count_ptr`; the JIT hit/miss reads it every hash. DO NOT touch the JIT
  `_end_hybrid` hit/miss logic (fixed 2026-08-07, correct).

## The problem
On `--dataset-mb=N --workers=7`, the miner shows `Speed: 0.00 H/s` for ~172
seconds while the partial dataset fills, because `wait_for_fill()` blocks every
mining worker until the fill is 100% complete. The hybrid design is MEANT to let
workers hash during fill (read whatever prefix is filled, derive the rest
on-the-fly). Part 1 made that safe (contiguous publish, C1 closed). Your job:
relax the block so workers hash during fill (Part 2), and fix the fill-worker
core starvation that made `fill_items` stall at exactly half for ~75s (Part 3).

## Part 2 — hybrid hashes during fill (the main fix)
In `MiningEngine::worker_loop()` (src/mining_engine.cpp, the `if
(partial_dataset_) { partial_dataset_->wait_for_fill(); }` block near line 389),
do NOT call `wait_for_fill()` when running in HYBRID mode — i.e. when
`partial_dataset_` is non-null AND the mode is light with a cached prefix
(`--dataset-mb>0`). Let the worker proceed to hash immediately; the JIT hit/miss
(already wired via `set_partial_dataset`, called at the end of worker_loop setup
around line 560) reads the live `item_count_` and derives misses on-the-fly.
- KEEP the seed-rotation re-wait (the block at lines ~476-485: on
  `partial_dataset_fill_generation_` change, call `wait_for_fill()` then fall
  through to re-setup the VM with the new seed). The new fill's prefix restarts
  at 0, so re-waiting on rotation is still correct. DO NOT remove it.
- For `partial_dataset_ == nullptr` (pure light, `--dataset-mb=0`) or non-hybrid
  paths, KEEP `wait_for_fill()` as-is. Only the hybrid-branch skip is changed.
- `miner_app.cpp` line ~246 `wait_for_fill()` (full-memory / non-hybrid) is
  UNCHANGED.

## Part 3 — fill-worker starvation (the half-stall)
`start_fill` is called from `set_job` (mining_engine.cpp ~255) with
`exclude_cores={}` and `core_order_` = all cores. On the in-order A53, the fill
threads and the (now hashing) miner threads contend for the same cores, and the
fill stalled at exactly HALF (4,194,304 of 8,388,608 items) for ~75s. Fix: in
`set_job`, compute the set of cores the miner workers will actually occupy under
the active AffinityMode (the same `core_order_[thread_id % ...]` logic
worker_loop uses for BigOnly / All), and pass that set as `exclude_cores` to
`start_fill` so fill workers use the *other* cores. If there are fewer remaining
cores than fill chunks, that's fine — `start_fill` already caps fill-worker count
to `min(chunks, avail_cores.size())`. Verify the fill no longer stalls at half.

## Correctness gate (you MUST satisfy these)
1. Host: build and run `./build/test_partial_dataset` — the existing KAT
   `test_contiguous_publish_no_uninitialized_read` MUST still pass (it proves no
   uninitialized read is ever published). Also `test_mining` and `armrx_tests`
   (JIT 16/16) MUST pass.
2. On-device (lenovo, mechres@192.168.10.156, aarch64): cross-build with
   `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake
   && cmake --build build-cross -j$(nproc)`, scp the binaries to
   `/tmp/cross-dag/` on the device (NOEXEC tmpfs — run from there or `sh` them),
   then run `./armrx --pool-test --dataset-mb=512 --workers=7 --seconds=200`
   and confirm: H/s is > 0 within the FIRST ~10s window (not after 172s), and
   steady-state H/s matches the prior post-fill baseline (~29-30 H/s at 7
   workers). Also `time_partial_fill 512` and confirm `fill_items` no longer
   stalls at half for ~75s.
3. The JIT hit/miss must produce CORRECT hashes during fill. The contiguous
   publish (Part 1) is the safety net: if it holds, no wrong hashes. If you see
   wrong hashes or H/s that stays 0 for the whole fill (not a stall), REVERT your
   Part 2 change and report it — do not ship.

## Kill criterion (decision rule)
- ADOPT only if: (a) host `test_partial_dataset` (incl. contiguous KAT) +
  `test_mining` + `armrx_tests` JIT 16/16 PASS, (b) on-device
  `--dataset-mb=512 --workers=7` shows H/s > 0 within first 10s AND
  steady-state H/s matches the ~29-30 baseline (no regression), (c) `fill_items`
  no longer stalls at half for ~75s.
- OTHERWISE REVERT (git checkout the changed files) and report exactly which gate
  failed and the numbers you measured. Report the honest result either way — no
  fake win.
- Do NOT modify the JIT `_end_hybrid` hit/miss path. Do NOT modify the Part 1
  contiguous-publish logic. Keep the seed-rotation re-wait.

## Discipline
- ONE test per session on-device. Separate scp and ssh. Never use qemu (no A53
  model). Kill any stale `armrx`/`cmake`/`cc1plus` processes on the device
  before building/running.
- Commit your change with a clear message referencing Part 2+3 and the brief. Do
  NOT commit the two audit docs in docs/audits/ (they are handled separately).
- When done, report: the diff scope, host test results, and the on-device numbers
  (first-window H/s, steady-state H/s, fill_items stall behavior, fill time).
