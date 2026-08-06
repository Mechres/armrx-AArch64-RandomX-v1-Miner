# VERBATIM AGENT PROMPT — Hybrid dead-stop: 3 variants on separate branches

You are implementing 3 variants of a fix in the `armrx` AArch64 RandomX miner
(repo: /home/mechres/Projeler/aarch64-randomx, C++ / CMake, cross-compiled for
aarch64 Cortex-A53, 8 cores). Part 1 (contiguous publish) is ALREADY DONE AND
COMMITTED (`9881878`) — it is the safety net; do NOT modify it. Part 2+3 was
REVERTED (instant-start + exclude-all-miner-cores left 1 fill core → 23.16 H/s).
The *lever* "cut the 172s dead-stop's cost" is NOT exhausted — 3 distinct designs
remain untried. Your job: implement each on its OWN branch, verify host + on-device,
and leave each branch non-reverted so the senior engineer (Hermes) can inspect.

## Read first (do not skip)
- `docs/briefs/2026-08-07-hybrid-deadstop-variants.md` — full brief (variants,
  gates, branch discipline).
- `docs/briefs/2026-08-07-hybrid-hash-during-fill.md` — prior Part 1/2/3 history
  (why Part 2+3 reverted; the 1-core starvation root cause).
- `src/mining_engine.cpp` — `worker_loop()` ~389 calls `wait_for_fill()` when
  `partial_dataset_` set; `set_job()` ~255 calls `start_fill(..., exclude_cores={})`.
- `include/armrx/partial_dataset.hpp` + `src/partial_dataset.cpp` — Part 1's
  `contiguous_done_`, `chunk_done_`, `item_count()` (contiguous published prefix).
  `wait_for_fill()` blocks until `fill_complete_`. DO NOT change contiguous publish.

## Branch discipline (CRITICAL — so code is never lost)
From current HEAD (main @ `785ee69`), create THREE branches:
- `try/variant1-tiered-threshold`
- `try/variant2-colocated-fill`
- `try/variant3-smaller-dataset`
Implement ONE variant per branch. Commit on the branch. Do NOT merge to main. Do NOT
revert/delete the branch work — leave it committed so Hermes can `git diff main...branch`.
Tag your final commit on each branch with a clear message (variant + result).

## Variant 1 — Tiered `wait_for_fill(threshold)`  [branch: try/variant1-tiered-threshold]
Reduce dead-time WITHOUT the all-miss regression. Block workers until the contiguous
prefix reaches `threshold × N`, then start; the rest fills behind them.
- Add `double fill_start_threshold = 1.0` to the engine (default 1.0 = current
  `wait_for_fill` behavior, no regression). Add CLI `--fill-start-threshold=N`
  (0.0–1.0; clamp; 1.0 = wait for full fill).
- In `worker_loop`, when `partial_dataset_` is set: if threshold == 1.0, call
  `wait_for_fill()` as today; else poll
  `partial_dataset_->item_count() >= static_cast<std::size_t>(threshold * allocated_items_)`
  (with a small sleep, like the existing spin) before hashing. Seed-rotation re-wait
  (~476-485) UNCHANGED.
- Correctness is GUARANTEED by Part 1: at threshold T, items `[0, T×N)` are fully
  filled (safe hits); ≥ T×N miss and derive on-the-fly. No new safety risk.
- Predicted: 0.5 → ~86s dead-time, steady ramps 26→29.8.
- Host gate: `test_partial_dataset` (incl. contiguous KAT) + `test_mining` +
  `armrx_tests` JIT 16/16 PASS. On-device: `--dataset-mb=512 --workers=7` dead-time
  (first H/s>0) < ~120s AND steady ≥ 28 H/s → PASS. Else REVERT on the branch only.

## Variant 2 — Co-located fill (no exclude_cores)  [branch: try/variant2-colocated-fill]
Minimal change: in `set_job`, call `start_fill` with `exclude_cores={}` (all cores)
and do NOT pin fill threads to a subset — let the OS scheduler share fill threads
across all 8 cores alongside the miners. Keep `wait_for_fill()` as the block (so
workers still wait for full fill) — this variant only changes HOW the fill is
scheduled, not when workers start.
- Rationale (untested hypothesis): giving the fill a thin slice of every core may
  fill faster in aggregate than pinning to 1 starved core, and the scheduler may
  balance better than the model predicts. Risk: fill may crawl (688s+) → REVERT.
- Host gate same as Variant 1. On-device: fill must complete in a reasonable time
  (say < 300s) AND steady ≥ 28 H/s. If fill is still crawling, REVERT on branch.

## Variant 3 — Smaller default `--dataset-mb`  [branch: try/variant3-smaller-dataset]
The 172s scales with dataset size. Lower the effective default so the dead-stop is
short. Two sub-options (pick the cleaner): (a) change the auto-selected default when
`--dataset-mb` is not given, or (b) just measure and document 128 MiB as recommended.
Implement (a): set the implicit default to 128 MiB (or add `--dataset-mb` default in
cli_parser). Keep `wait_for_fill` (full fill, then max rate).
- Measure on-device fill time + steady H/s at 64 / 128 / 256 MiB (one session each,
  or as the device discipline allows). Predicted: 128 MiB ≈ 43s dead, ~27-28 H/s;
  64 MiB ≈ 20s, ~26-27. For a 200s `--pool-test`, 43s hashes far more of the window
  than 172s.
- Adopt the size with best window-utilization (dead-time + steady H/s trade-off).
  This variant is the safest (config-only); if it alone satisfies the user's "why
  172s of 0.00" complaint, it may be the pragmatic answer.

## Correctness gate (ALL variants)
1. Host: build + `./build/test_partial_dataset` (contiguous KAT MUST pass) +
   `test_mining` + `armrx_tests` (JIT 16/16).
2. On-device (lenovo, mechres@192.168.10.156, aarch64): cross-build
   `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake
   && cmake --build build-cross -j$(nproc)`; scp binaries to `/tmp/cross-dag/`
   (NOEXEC tmpfs — run from there); run `--pool-test --dataset-mb=N --workers=7
   --seconds=200`. One test/session per branch; separate scp/ssh; never qemu; kill
   stale `armrx`/`cmake`/`cc1plus` first.
3. Wrong hashes or H/s stuck at 0 for the whole fill (not a stall) → REVERT on that
   branch and report. The Part 1 contiguous publish is the safety net; if it holds,
   no wrong hashes.

## Kill criterion (per branch)
- Variant 1: dead-time < ~120s AND steady ≥ 28 H/s → PASS (leave on branch).
- Variant 2: fill completes < ~300s AND steady ≥ 28 → PASS; else REVERT on branch.
- Variant 3: report fill-time + steady at each size; adopt best trade-off (leave on
  branch).
- Report honest numbers either way — no fake win.

## Discipline
- Do NOT touch JIT `_end_hybrid` hit/miss or Part 1 contiguous publish.
- Do NOT commit the audit docs in docs/audits/.
- For each branch, report: diff scope, host results, on-device dead-time / steady H/s
  / fill_items behavior. Leave the branch non-reverted so Hermes can inspect.
