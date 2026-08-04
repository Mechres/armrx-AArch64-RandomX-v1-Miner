# Changelog

Dated, per-change-group entries. Each entry lists the files touched and the
verification evidence. Device-safety discipline (TESTING.md §8) applies to all
on-device claims.

---

## 2026-08-07 — Fix TUI garbage control bytes / overlapping lines (Bug 2)

**Files:** `src/tui.cpp`, `src/miner_app.cpp`, `docs/STRATEGY.md` (Known bugs)

- Root cause (twofold): (1) `armrx::log::set_tui_mode(true)` was **never called**
  anywhere (audit: dead ring-buffer), so worker-thread `ARMRX_LOG_*` wrote to
  `std::cout` under `sink_mutex`; (2) `Tui::render()` / `Tui::shutdown()` wrote
  their ANSI escape sequences to `std::cout` **without** `sink_mutex`. The two
  writers interleaved on the fd, splitting escape sequences with log text →
  broken control bytes + overlapping lines ("armrx" + raw control bytes).
- Fix: call `set_tui_mode(true)` when the TUI is constructed (worker logs → ring
  buffer, off stdout) and `set_tui_mode(false)` after the main loop; `render()`
  and `shutdown()` now lock `armrx::log::sink_mutex()` around their whole stdout
  write so the escape sequence is atomic. Also restored the cursor on the
  `--pool-test --tui` `_Exit` path (it skipped `tui->shutdown()` → hidden cursor
  + dashboard left on screen).

**Verification (on-device, MSM8929, cross-built `armrx`):**

- `--tui --pool-test --seconds=15 --pool=dummy.invalid:1111` (connection fails,
  but `render()` runs every second regardless of state) → captured log shows
  **clean, complete TUI frames with no interleaved log text**, the one worker-log
  line (`[ERROR] DNS resolution failed`) appears standalone before the frames
  (routed to ring buffer, off stdout). `DONE_EXIT=0`, no SIGSEGV/abort.
- The redirected-file capture cannot show visual TTY rendering (escape sequences
  only render on a terminal), but the structural absence of broken/interrupted
  sequences + clean self-terminating exit indicates the interleave is gone.
  User to confirm visually with a real `--tui --pool=` run.

---

## 2026-08-07 — Lever 3: `bench_armrx --workers=N` multi-worker throughput (measurement enabler)

**Files:** `tests/bench_armrx.cpp`, `docs/TESTING.md` (§1), `docs/STRATEGY.md` (Phase 1.5)

- Added `--workers=N` to `bench_armrx --full-hash-only`. Spawns N worker threads,
  each with its **own `VirtualMachine`** (scratchpad + JIT code) but a **SHARED
  `Argon2dCache`** (RandomX threading model: the cache/dataset is read-only after
  init and is meant to be shared — only the VM is per-thread). Reports **aggregate
  hash/s = total hashes across all workers / real wall time**.
- This was the designed-but-unwired flag referenced in TESTING.md §1 / STRATEGY.md
  (previously "`bench_armrx --full-hash-only` is single-threaded (ignores --workers)").
  Now genuinely implemented, unblocking the Lever-2 8w instruction-mix measurement.
- Correctness/robustness: sharing one cache drops memory from N×256 MiB to ~256 MiB
  total (the first attempt — per-worker caches — OOM-killed at 8w on the 1.35 GiB-
  free MSM8929). Per-worker `block_template`/`hash_out` copies avoid the nonce-byte
  data race. `--workers` requires `--full-hash-only` (guarded, errors otherwise).
- Single-worker (`--workers=1`, the default) path is byte-for-byte unchanged: still
  reports the 500-sample percentile table at 5.12 hash/s @1w (matches pre-change
  baseline — no regression).

**Verification (on-device, MSM8929, cross-built):**

- KAT gate `test_jit_equivalence` 16/16 byte-identical (DONE_EXIT=0) — JIT path
  unaffected by the bench change.
- `--workers=1 --full-hash-only`: `5.12 hash/s` median, identical report format.
- `--workers=8 --full-hash-only` (pinned 0-3): **aggregate 12.33 hash/s** over 500
  hashes / 40.5 s wall (DONE_EXIT=0). The 8w≪8×1w gap under 0-3 pinning is the
  expected weak-cluster (cores 4-7) penalty and is exactly the Lever-2 signal.
- Guard: `--workers=4` without `--full-hash-only` → `error: --workers requires
  --full-hash-only`.

---

**Files:** `src/miner_app.cpp`, `src/pool_manager.cpp`, `src/stratum_client.cpp`

**Symptom:** `^C` during `--pool` mining stopped hashing but the process never
returned to the prompt ("printed the final line but didn't return"). Previously
recorded as an OPEN bug (STRATEGY.md) and mis-attributed to a stalled-peer
`send()`.

**Root cause (two stacked defects in the teardown path):**

1. **Self-deadlock in `PoolManager::disconnect()`** (the actual hang). The old
   code held `stratum_mutex_` while calling `StratumClient::disconnect()`, which
   `join()`s the reader thread. The reader thread's teardown path invokes
   `error_callback_` → `PoolManager::current_pool_name()` →
   `std::lock_guard(stratum_mutex_)`. So the main thread waited on the reader to
   finish while the reader blocked on the mutex the main thread held. Every clean
   teardown deadlocked.
2. **Unbounded blocking `send()`** in `StratumClient::write_all()` (a secondary
   stall). A worker submitting a share could block forever in `send()` against a
   full/stalled peer, holding `stratum_mutex_` and preventing any teardown.

**Fixes:**

- `src/pool_manager.cpp` — `disconnect()` now drops `stratum_mutex_` before
  tearing down the `StratumClient` (and thus before joining the reader thread),
  then resets `stratum_` under the lock. Matches the existing pattern in
  `connect_to_current()` (which already releases the lock before the blocking
  `connect()`).
- `src/miner_app.cpp` — teardown order in `run_pool_mining()` changed to
  `engine.stop()` **before** `pool_mgr->disconnect()`, so no worker can enter
  `submit_share()` (which takes `stratum_mutex_`) during teardown. Makes teardown
  deterministic regardless of peer state.
- `src/stratum_client.cpp` — set `SO_SNDTIMEO` (2s) on the socket after connect,
  so a stalled `send()` returns (dropping the share) and releases the mutex
  promptly instead of blocking indefinitely. `send_line` already treats a
  non-`EINTR` send failure as a clean drop.

**Verification:**

- Cross-compile (`aarch64-linux-musl-g++`, GCC 16.1.0) clean: 3 TUs + link →
  `build-cross/armrx` (AArch64/musl PIE).
- Ad-hoc host harness: the "hold-mutex-across-join" pattern deadlocks (killed by
  SIGALRM after 3s); the "drop-mutex-then-join" pattern returns in 0.000s. Same
  harness confirmed `SO_SNDTIMEO` bounds a would-be-infinite `send()` (returns in
  ~6s).
- **On-device (`kill -INT` from a 2nd SSH session, 2026-08-07):** process printed
  `Pool mining stopped.` + totals and **returned to the prompt within ~2s**. Bug
  closed.
