# Changelog

Dated, per-change-group entries. Each entry lists the files touched and the
verification evidence. Device-safety discipline (TESTING.md §8) applies to all
on-device claims.

---

## 2026-08-07 — FIX hybrid partial-dataset consumption produced wrong end-to-end hashes (pre-existing, separate from the seed-rotation fix)

**Files:** `src/jit_compiler_a64_static.S`, `src/jit_compiler_a64.cpp`,
`include/armrx/jit_compiler_a64_static.hpp`, `tests/test_mining.cpp`,
`tools/verify_seed_rotation.cpp`

- **Bug:** in hybrid light mode (`--dataset-mb=N`), when the RandomX main loop
  read a cached prefix item, the AArch64 `_end_hybrid` path bound-checked and
  indexed the partial buffer using the **pre-offset** dataset item number, while
  the light *derivation* (miss) path applied `randomx_dataset_item_count()`
  offset first. So the hit path read the wrong (un-offset-shifted) item, and the
  end-to-end hash diverged from the light reference for **any** `--dataset-mb>0`
  job (single job and rotation alike). `test_partial_dataset` still passed
  because it only checks `pd.data()[i] == generate_dataset_item(cache,i)` — the
  *buffer fill* was always byte-correct; the defect was purely in the JIT read.
  This is why README already flagged the partial dataset "⚠️ not adopted for
  production".
- **Fix:** move the `dataset_offset` application to **before** the hit/miss
  selection in `randomx_program_aarch64_hybrid_tweak` (the partial buffer is
  indexed by the same offset-adjusted item the reference derives), and extend the
  JIT I-cache flush to cover the complete hybrid template (through a new
  `randomx_program_aarch64_hybrid_end` label) so the patched offset range is
  actually invalidated, not just the early label.
- **Regression test:** `test_light_mode_partial_dataset_matches_reference` in
  `tests/test_mining.cpp` drives the engine through a seed-B job with
  `--dataset-mb` (partial) set and asserts the engine's end-to-end hash equals a
  fresh-process light-mode reference for the same nonce — the test that was
  impossible while the path was broken. (The earlier `…_rebuilds_partial_dataset`
  test only byte-checks the buffer rebuild; this one checks the hash.)
- **Harness:** `tools/verify_seed_rotation.cpp` `engine-norotate` now uses seed B
  (it was seed A) so the single-job comparison is against the correct reference.

**Verification (host x86_64 + on-device aarch64, 2026-08-07):**

- Host: `armrx_tests` (JIT 16/16), `test_mining` (incl. both partial-dataset
  tests), `test_partial_dataset` — all PASS.
- **On-device (lenovo, aarch64, cross-built, fresh `.o`):** with `--dataset-mb`
  (items=65536), the engine hash now equals the reference for single job
  (`8c7c5d6169128438618971d99e43bdb28456cdba85ffee2509cc0f42a9ef2ef2`) AND for
  rotate mode, where previously both were wrong (`241b3376…` / `cd5cc806…`).
  `test_mining` passes including `test_light_mode_partial_dataset_matches_reference`;
  `armrx_tests` EXIT=0. Independently re-gated by Hermes (not just the agent's
  report).
- **Residual:** the new regression test cannot be *negated* on host (x86_64
  doesn't run the AArch64 hybrid JIT), so its failure-without-fix was reasoned
  from the pre-fix device hashes rather than re-run on-device. Device time was
  spent on the positive gate.

---

## 2026-08-07 — Revert `PartialDataset` serialization fix; 512 MiB fill-stall is the real bug

**Files:** `src/mining_engine.cpp`, `docs/STRATEGY.md` (Known bugs)

- Earlier today a serialization fix added `partial_dataset_->wait_for_fill()`
  before hashing (in `set_job`, then moved to `worker_loop`) to kill the
  out-of-order-publish wrong-hash window. On-device testing showed it turned
  into a **hard hang**: `--dataset-mb=512` produced 0.00 H/s for 60s+ with no
  `fill complete` log. Measurement: the 512 MiB background fill's 8 threads
  **park at ~13% busy CPU** — they never complete. `wait_for_fill()` joins those
  never-completing threads, so any hashing (or the pool thread) blocks forever.
- **Root cause is the fill stall itself, not the publish race.** `initialize_dataset`
  is pure NEON (no locks/threads), so the stall is in fill-thread
  scheduling/completion or a thrown exception that strands `wait_for_fill`. Because
  the fill never completes, the cached-prefix fast-path (`item_number < item_count_`)
  never engages and every hash takes the slow derivation path — `--dataset-mb`
  gives no speedup on MSM8929 (correct, just not faster).
- **Reverted** to the original "hash during fill" behavior: `set_job` only calls
  `start_fill()`, no `wait_for_fill()` in the mining path (`wait_for_fill()` remains
  only at teardown, `miner_app.cpp:246`). This restores mining (`--dataset-mb=512`
  → ~12 H/s immediately, verified on-device) and leaves the masked out-of-order
  publish race latent (pool rejects wrong shares; fill is stalled anyway).
- **Proper fix (not yet done):** contiguous publish of `item_count_` (advance only
  over the completed prefix) so hashing can safely overlap the fill AND the
  fast-path engages once the fill actually completes — but that requires first
  fixing the fill-stall. Tracked as OPEN bug in STRATEGY.md.

**Verification (on-device, MSM8929, cross-built):**

- `--dataset-mb=512 --mine`: Speed ramps to ~12 H/s within 1-2s (was 0.00 H/s
  hang with the serialization fix). Fill runs in background; no hang.
- `test_partial_dataset` ALL PASSED, `test_mining` ALL PASSED (no regression).

---

## 2026-08-07 — Fix `PartialDataset` out-of-order publish (wrong-hash window during background fill)

**Files:** `src/mining_engine.cpp`, `docs/STRATEGY.md` (Known bugs)

- Fill workers publish `item_count_` as the **max** of completed chunk bounds
  across workers (`partial_dataset.cpp:150-155`). A lagging chunk stays
  uninitialized while `item_count_` has jumped past it, so the JIT's
  `item_number < item_count` check could read an uninitialized item in the hole
  and emit a **wrong hash**. The audit called this "latent" assuming
  `wait_for_fill()` ran before mining — but the code did NOT (only teardown +
  unit test called it); mining read the partial dataset *while fill ran in the
  background*, so the race was reachable (masked: pool rejects wrong shares, fill
  is fast).
- Fix: `partial_dataset_->wait_for_fill()` at the end of the one-shot
  `start_fill()` path inside `MiningEngine::set_job()`. Workers only hash once
  `set_job` sets `has_job_`, which now returns only after fill completes — no
  hash reads a partially-filled dataset. One-time startup cost only
  (`start_fill` gated by the one-shot `partial_dataset_fill_started_`
  `atomic_flag` at the time), not per seed rotation.

**Verification (on-device, MSM8929, cross-built):**

- `test_partial_dataset` — ALL PASSED (cached items match reference, large
  dataset spot-checked, incremental fill consistent).
- `test_mining` — ALL PASSED (engine lifecycle produces valid shares via the
  `set_job`+wait path; bad-nonce recovery OK). No deadlock from the new wait.
- `armrx` cross-builds clean.

---

## 2026-08-07 — FIX light-mode seed rotation now rebuilds the partial dataset (was a silent wrong-share bug)

**Files:** `src/mining_engine.cpp`, `src/partial_dataset.cpp`,
`include/armrx/mining_engine.hpp`, `tests/test_partial_dataset.cpp`,
`tests/test_mining.cpp`, `tools/verify_seed_rotation.cpp`

- **Bug (OPEN in the 2026-08-07 handoff brief):** in hybrid light mode the
  partial dataset was filled exactly **once**, gated by the one-shot
  `partial_dataset_fill_started_` `atomic_flag`. On a live pool seed-rotation
  (`set_job` with a new `seed_key`), `shared_cache_` was rebuilt but
  `start_fill` was never re-triggered (flag already set), so workers kept mining
  on the OLD seed's partial dataset → silent wrong hashes / invalid shares.
  Single-job KATs (`test_mining`, `--pool-test`) never exercised rotation, so
  it stayed latent.
- **Fix (matches the brief's option (a)):** replace the one-shot flag with a
  monotonic `partial_dataset_fill_generation_` counter. `set_job()` now
  re-runs `start_fill()` with the new cache and bumps the generation on **any**
  seed-key change while running (still one fill before `start()`, since the
  first job is also a rotation from no-cache). Each mining worker compares its
  local generation in `worker_loop()` and **re-waits** on `wait_for_fill()`
  before hashing again, so it can never read a seed-mismatched partial dataset.
  The re-wait sits *before* the job-gen re-arm, guaranteeing the VM is
  re-pointed at the new cache only after the fresh fill is complete.
- **`PartialDataset` made re-fillable:** `start_fill()` now resets
  `item_count_`/`fill_complete_` and **joins any still-running prior fill**
  threads before spawning new ones (so a second fill can't race the first on the
  same buffer, and the thread vector doesn't grow unbounded across rotations);
  `wait_for_fill()` clears its joined thread handles. This is what makes
  `start_fill()` safe to call repeatedly.
- **New regression tests:** `test_refill_with_new_seed` in
  `tests/test_partial_dataset.cpp` (re-fill with a different seed, byte-verify);
  `test_light_mode_seed_rotation_rebuilds_partial_dataset` in `tests/test_mining.cpp`
  (drives `MiningEngine` through a live seed-A→seed-B rotation while workers run,
  then byte-verifies the partial-dataset buffer holds seed-B's items, not stale
  seed-A). Plus a standalone `tools/verify_seed_rotation.cpp` driver that runs the
  engine and a fresh-process light reference in **separate processes** so no JIT
  program-cache is shared.

**Verification (host x86_64 + on-device aarch64, 2026-08-07):**

- Host: `test_partial_dataset` (incl. `test_refill_with_new_seed`),
  `test_mining` (incl. the new rotation test), `armrx_tests` (JIT 16/16),
  `test_aes_hash`, `test_config` all PASS. `test_cli_parser` fails at
  `test_pool_flags` line 177 — **pre-existing, parser/test inconsistency,
  unrelated** (confirmed on a clean stash tree).
- **On-device (lenovo, aarch64, cross-built):** `test_mining` passes including
  the rotation test — log shows "seed rotation — restarted background fill,
  workers will re-wait" and the buffer is byte-verified rebuilt with seed-B
  (256 items, none equal to stale seed-A). The re-fill + worker re-wait
  handshake works on the real AArch64 NEON path.
- **Separate pre-existing finding (NOT this fix):** the RandomX *hybrid
  partial-dataset consumption path* (the JIT reading cached prefix items)
  produces WRONG end-to-end hashes on-device even for a SINGLE non-rotated job
  with `--dataset-mb>0` (verified via the verify_seed_rotation driver:
  items=65536 hashes don't match a same-nonce light reference; items=0 matches
  correctly). This is independent of the rotation fix (the single-job fill path
  is observationally identical before/after this change) and is why README
  already flags the partial dataset "⚠️ not adopted for production". The rotation
  test asserts the *buffer rebuild* directly (via `generate_dataset_item`), not
  the engine's end-to-end hash, to avoid conflating the two issues. Fixing the
  hybrid consumption path is a separate task.

---

## 2026-08-07 — Fix IPv6 bare-address `--pool=` parse

**Files:** `src/cli_parser.cpp`, `docs/STRATEGY.md` (Known bugs)

- `--pool=` split on the **last** `:` (`addr.rfind(':')`), so a bare IPv6 like
  `2001:db8::1` mis-parsed to host `2001:db8:`, port `1`. `[v6]:port` worked only
  because the bracketed host has no `:` outside `]:`.
- Fix: detect the bracket form `[host]:port` (host may contain `:`); otherwise a
  single `:` with a numeric port and no other `:` before it is `host:port`, and a
  multi-`:` address (bare IPv6) or non-numeric trailing segment is host/IPv6 with
  the default port (3333).

**Verification (host, linked cli_parser.cpp + crafted argv):**

- `host:port` → (pool.example.com, 3333); bare host → (pool.example.com, 3333);
  `2001:db8::1` → (**2001:db8::1**, 3333) [was 2001:db8: / 1]; `[2001:db8::1]:3333`
  → (2001:db8::1, 3333); `[2001:db8::1]` → (2001:db8::1, 3333). All PASS.
  `armrx` cross-builds clean with the change.

---

## 2026-08-07 — Fix `MetricsExporter` shutdown blocks behind idle HTTP client

**Files:** `include/armrx/metrics.hpp`, `docs/STRATEGY.md` (Known bugs)

- The worker thread did a blocking `accept()` then a blocking `read()` per
  connection. An idle client (connected, sent nothing) pinned `read()`
  forever; even *no* incoming connection left the worker stuck in `accept()`.
  So the destructor's `join()` hung teardown whenever `--metrics-port` was set.
- Fix: `poll()` the listen socket with a 250 ms timeout (shutdown unblocks even
  with no connection) + `SO_RCVTIMEO` (2 s) on the accepted client fd (idle
  `read()` returns, worker loops). The listen fd stays worker-owned, preserving
  the prior `server_fd_` data-race fix.

**Verification (host, header-only regression test — portable C++, no device):**

- Start exporter, open an **idle** connection, destroy it → teardown completed
  in **2.024 s** (the SO_RCVTIMEO read timeout) and exited cleanly. Before the
  fix this hung forever. `/metrics` still serves the correct Prometheus body
  (HTTP 200 + body). `armrx` cross-builds clean with the header change.

---

## 2026-08-07 — Fix TUI dangling `string_view pool_name` (UAF → empty header + DAG segfault root cause)

**Files:** `include/armrx/tui.hpp`, `docs/STRATEGY.md` (Known bugs)

- `TuiSnapshot::pool_name` was `std::string_view` bound to a **temporary**
  `std::string` returned by `PoolManager::current_pool_name()` (miner_app.cpp:448).
  It dangled by the time `render()` read it seconds later — a use-after-free
  active in BOTH TUI modes. This is the shared root cause the 2026-08-07 audit
  attributed to: (a) the non-DAG garbage/empty TUI header, and (b) the
  `ARMRX_DAG_SCHED=1` `--tui` segfault (DAG's different heap-reuse timing
  merely exposed the bad read as a crash).
- Fix: change `TuiSnapshot::pool_name` to an **owned `std::string`**. (`mode`
  stays `string_view` — it binds to a lifetime-safe `mode_name()` string literal.)
  This also closes the deferred `--tui` segfault (no bad read remains to crash;
  `ARMRX_DAG_SCHED` is gated OFF, so the segfault path is gone structurally).

**Verification (on-device, MSM8929, cross-built `armrx`):**

- `--tui --pool-test --seconds=15 --pool=dummy.invalid:1111` → header now renders
  `armrx  dummy.invalid:1111  [light]` with **0 NUL bytes** (was NUL-padded
  garbage before the fix). Frames remain clean (no interleave from the Bug-2
  fix). `DONE_EXIT=0`, no SIGSEGV/abort. The empty-header UAF is gone.

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
