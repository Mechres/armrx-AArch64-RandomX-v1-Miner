# Changelog

Dated, per-change-group entries. Each entry lists the files touched and the
verification evidence. Device-safety discipline (TESTING.md §8) applies to all
on-device claims.

---

## 2026-08-07 — Fix SIGINT-on-`--pool` teardown deadlock (exit now returns to prompt)

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
