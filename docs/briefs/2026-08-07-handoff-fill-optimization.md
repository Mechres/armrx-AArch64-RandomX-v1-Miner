# 2026-08-07 (late) — Handoff: dataset-fill optimization + open bugs

## Session summary (what shipped)
- **Tier 1 `wait_for_fill` dead-start** — SHIPPED (`5e63942`). Workers block until
  the background partial-dataset fill completes, then mine at full speed. No more
  hashing on an empty dataset / misleading ramp. Regression KATs PASS.
- **Rolling-window live `Speed:`** — SHIPPED (`7047cfc`). The live pool `Speed:`
  line now uses a 10 s rolling window from `engine.snapshot()` deltas (XMRig-parity)
  instead of the whole-run cumulative `hash_rate()`. Verified on `lenovo`
  (aarch64, non-isolated): `Speed:` holds 0.00 during the ~164 s dead-start, then
  jumps to ~29 H/s within ~10 s of fill completion and stays flat (no ramp).
- **Tier 2(a) drop-NEON fill interpreter — REVERTED (negative A/B).** Delegated to
  external agent (Luna). The scalar-GPR fill regressed 164 s → 207 s (~27% slower);
  the NEON lane-extract was NOT the bottleneck (the scalar path lost the genuinely
  parallel vector ISUB/IXOR/IADD). `execute_superscalar_neon` in `dataset.cpp`
  restored. `time_partial_fill 512` back to 163.3 s.

## OPEN: light-mode seed rotation does NOT rebuild the partial dataset (BUG)
**Severity:** correctness (produces wrong hashes / invalid shares after a pool job
rotation). Latent — does NOT affect `--pool-test` with a single stable job.
**Location:** `src/mining_engine.cpp` `MiningEngine::set_job()` (lines 219–245).
**Root cause:** the partial dataset is filled exactly once, gated by the one-shot
`partial_dataset_fill_started_` `atomic_flag` (line 242). The fill uses
`shared_cache_` built for the *first* job's seed. On a later seed rotation
(pool `New job`), `shared_cache_` is reinitialized (line 226) but `start_fill` is
NOT re-triggered (flag already set). Result: partial dataset keeps the OLD seed's
contents while the cache has the NEW seed → workers mine on a seed-mismatched
partial dataset → wrong hashes.
**Fast mode is fine** — its reinit path (`if (mode_ == RandomXMode::fast)`, line 247)
rebuilds via the persistent workers handshake on every generation bump.
**Fix sketch (NOT implemented):** on seed-key change in light mode, either (a) reset
`partial_dataset_fill_started_` and re-run `start_fill` (but then workers must
`wait_for_fill()` again — acceptable on rare job rotation, not per-hash), or (b)
make the partial dataset hold BOTH seed datasets / keyed by seed and select at hash
time. (a) is simplest and matches the dead-start model; (b) is more complex.
**Verify before fixing:** confirm with a multi-job pool run (or a unit test that
rotates the seed and checks `test_mining`-style hash KAT against the new seed).
**Note:** `test_mining` single-job KATs PASS — they don't exercise rotation.

## Observation (NOT a bug): `htop` shows "8 → 4 → 7" armrx threads at startup
User saw armrx thread count go 8 → 4 → 7 in `htop` when starting `--pool-test
--workers=7`. This is EXPECTED: `htop` counts OS threads, not logical workers.
- `start_fill` spawns **8 fill threads** (log: `started fill with 8 workers`).
- `engine.start()` spawns **7 mining worker threads** (num_threads_=7).
- During the fill: 8 fill (running, then exiting as chunks finish) + 7 miners
  (blocked in `wait_for_fill()`) + main ≈ transient counts (8 early, 4 mid-fill as
  fill threads finish, 7 once fill threads all exit and miners persist).
- After fill completes (~164 s): fill threads gone → steady state = 7 miners + main.
No action needed; documented so it isn't re-flagged as a bug.

## Next session — dataset-fill optimization (the real remaining gap)
**Problem:** the ~164 s dead-start fill is ~16× XMRig's ~10 s. Tier 1 made the *ramp*
go away (clean dead-start), but the fill itself is still slow. Closing this is the
only thing that matches XMRig's instant start.
**Why 164 s:** `initialize_dataset` runs `execute_superscalar_neon` 8 rounds/item ×
8.4M items = ~67M sequential superscalar-program executions on the in-order A53.
**Retraction (from earlier brief):** "AES missing from item gen" was WRONG — spec-correct.
**Tier 2 scope (agreed, NOT started):**
- **(a) drop NEON from fill interpreter — DONE, NEGATIVE.** Reverted. Do not repeat.
- **(b) vectorize ACROSS items (next real attempt):** process 2–4 independent dataset
  items simultaneously, each in a NEON lane (items have no inter-dependency). This is
  real SIMD parallelism vs XMRig. MUST verify on the in-order A53 that NEON helps for
  interleaved independent items (some ops like IMUL_R/IROR_C may still need per-lane
  extraction even here) — measure against the (a) scalar baseline (which we now know
  is ~207 s, i.e. scalar is WORSE; so baseline to beat is the NEON 164 s).
- **(c) alternative:** different fill algorithm / cache-blocking / precompute, or
  accept the dead-start as the cost of Track-B hybrid light mode.
**Explicit non-goal:** do NOT vectorize WITHIN a single item's superscalar program
(sequential dependency chain — cannot be parallelized).

## Device / tooling notes for next session
- Device `lenovo` = 192.168.10.156 (aarch64, 8× Cortex-A53, two clusters 0-3/4-7,
  non-isolcpus currently). `isolcpus` is OFF — all "26-30 H/s" numbers this session
  were non-isolated; user's 28.4 H/s target was UNDER isolcpus. To compare fairly,
  enable isolcpus or note the condition.
- **Hermes `terminal()` SSH limitation (IMPORTANT):** launching `armrx` via the tool
  fails — backgrounding (`&`/`setsid`) kills the SSH session (server drops when a
  child holds the pty), and a long foreground run drops on the ~164 s SILENT fill
  phase (idle channel). Workaround that worked: `setsid sh -c "..."` detached + poll
  the log in a SEPARATE short SSH call; OR just have the user run it interactively
  (user's SSH holds fine). Also: the cross `cmake --build` reported "Built target
  armrx" WITHOUT actually recompiling `miner_app.cpp.o` (stale dep tracking) — had to
  `find build-cross -name miner_app.cpp.o -delete` to force a real rebuild. ALWAYS
  check the binary `mtime` after a build before trusting on-device verification.
- Build: `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake &&
  cmake --build build-cross -j$(nproc)`. Ship to `/tmp/cross-dag/` (512M tmpfs,
  NOEXEC, clears on reboot). One test/session per TESTING.md §8.
- `--pool-test` now prints instantaneous `INST agg=` (per-5s snapshot delta) AND the
  live `Speed:` (10s rolling window). Both are the honest rate; `Speed:` no longer ramps.
