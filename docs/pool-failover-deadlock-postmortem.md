# Pool Failover Self-Deadlock Postmortem

**Date:** 2026-07-22
**Status:** Fixed, verified on both x86_64 and AArch64 (on-device)
**Discovered while:** writing PLAN.md §3.2's mock Stratum protocol tests (`tests/test_pool_protocol.cpp`)

---

## Symptoms

`PoolManager::tick()` — called once per second from the miner's main pool-mining loop (`MinerApp::run_pool_mining()`, `src/miner_app.cpp`) — permanently hangs the calling thread the first time automatic multi-pool failover actually completes its cooldown and attempts to reconnect to the next configured pool. This is the exact feature `README.md` advertises: "Multiple `--pool=host:port` flags give automatic failover after 5 retries with a 2s cooldown."

Practical impact: any real deployment using more than one `--pool=` flag where the first pool goes down would freeze after the first failover attempt. The mining worker threads keep running independently (they don't depend on this lock), so hashing continues, but all pool communication, status display, share submission bookkeeping, and any *further* failover attempts stop forever — the miner would appear to keep "mining" while silently never submitting another share or trying another pool again.

## Root Cause

`PoolManager::tick()` (`src/pool_manager.cpp`) took `std::lock_guard<std::mutex> lock(stratum_mutex_)` scoped to its entire body. When the failover cooldown counter reached zero, it called `connect_to_current()` — from *inside* that same lock scope:

```cpp
void PoolManager::tick() {
    std::lock_guard<std::mutex> lock(stratum_mutex_);   // locked here...
    ...
    if (failover_cooldown_ > 0) {
        --failover_cooldown_;
        if (failover_cooldown_ == 0) {
            connect_to_current();   // ...and connect_to_current() locks it again
        }
    }
}

void PoolManager::connect_to_current() {
    ...
    {
        std::lock_guard<std::mutex> lock(stratum_mutex_);  // same mutex, same thread
        stratum_ = std::make_unique<StratumClient>(...);
    }
    ...
}
```

`std::mutex` is non-recursive. A thread relocking a `std::mutex` it already owns is undefined behavior, and in practice on Linux (glibc/pthread `PTHREAD_MUTEX_NORMAL`, the default `std::mutex` implementation on both the x86_64 dev sandbox and the Alpine musl on-device target) this deadlocks the thread against itself permanently — it blocks forever waiting on a lock it is itself holding.

`connect_to_current()`'s internal lock only wraps the `stratum_` pointer reassignment; the slow, blocking `stratum_->connect()` call (full TCP handshake, up to a 10-second protocol handshake timeout) deliberately runs *outside* that lock, presumably so other threads calling `is_connected()`/`shares_accepted()`/`submit_share()` aren't blocked for the duration of a reconnect attempt. `tick()` calling it while already holding the lock defeated that design and introduced the self-deadlock.

### Why existing tests didn't catch it

There were no automated tests exercising `PoolManager::tick()`'s failover path at all before `tests/test_pool_protocol.cpp` — this is precisely the coverage gap PLAN.md §3.2 was written to close. The bug only manifests when the failover cooldown actually reaches zero and a reconnect is attempted, which requires a full, realistic multi-pool failover scenario (one pool connects successfully, drops, exhausts 5 reconnect retries with real exponential backoff, and rotates to the next pool) — not something exercised by manual testing against a single always-up pool.

## Fix

`src/pool_manager.cpp`: `tick()` now reads and mutates the failover-cooldown state under the lock as before, but defers the actual `connect_to_current()` call until after the `lock_guard`'s scope ends:

```cpp
void PoolManager::tick() {
    bool should_reconnect_now = false;
    {
        std::lock_guard<std::mutex> lock(stratum_mutex_);
        if (!stratum_) return;
        ...
        if (failover_cooldown_ > 0) {
            --failover_cooldown_;
            if (failover_cooldown_ == 0) {
                should_reconnect_now = true;
            }
        }
    }
    if (should_reconnect_now) {
        connect_to_current();
    }
}
```

No behavioral change to the ordering of state reads/writes — only the point at which `connect_to_current()` is invoked moves outside the lock, matching how it already manages its own locking internally. `tick()` has a single caller (the sequential, one-thread-per-second pool-mining main loop), so releasing the lock before the deferred call introduces no new race.

## Detection

1. Wrote `tests/test_pool_protocol.cpp::test_pool_failover()`: a first mock pool that accepts a connection, completes a handshake, then drops mid-session (arming `StratumClient`'s real `reconnect_loop()`), and a second "good" pool that should receive the failed-over connection.
2. First run hung indefinitely past a 120-second test timeout with no crash, no error — a hard, silent hang, immediately after the log line `"Failing over to <host>:<port>"` printed.
3. Traced the exact call chain (`tick()` → `connect_to_current()`, both taking `stratum_mutex_`) and confirmed the non-recursive self-lock via direct code reading — no debugger needed, the mutex nesting is visible directly in the two adjacent functions once you're looking for it.
4. Applied the fix; re-ran the same test — `test_pool_failover` now completes in ~64s total (dominated by the real 5-attempt exponential backoff timing: 1+2+4+8+16 = 31s, plus handshake overhead), matching expected production timing with no hang.

## Also found while building the same test (documented, not fixed — out of scope)

1. **AUTO-fallback gap:** `PoolManager::tick()`'s failover only triggers via `StratumClient::reconnect_attempts()` reaching 5, and that counter is only ever incremented inside `reconnect_loop()`, which is only *started* by `reader_thread_fn()` noticing a connection that was previously up go down. If the very *first* pool in a configured list is unreachable from the start (connection refused immediately, not merely dropped after connecting), `StratumClient::connect()` throws synchronously before ever starting a reader thread — `reconnect_loop()` never starts, `reconnect_attempts()` stays 0 forever, and `tick()`'s failover condition is never met. A first-configured pool that is simply down from process startup will never trigger failover to the next pool in the list; only a pool that connects successfully and later drops does.

2. **Stale-reconnect-thread join latency:** even after the deadlock fix, `connect_to_current()`'s `stratum_ = std::make_unique<StratumClient>(...)` reassignment destroys the OLD `StratumClient` first, and `~StratumClient()` joins its `reconnect_thread_` — which, at the moment failover triggers, is very likely mid-`sleep_for()` for what would have been a doomed 6th retry, sleeping up to `kMaxBackoffMs` (30s). `reconnect_enabled_.store(false)` in the destructor doesn't wake a thread already inside `sleep_for()`; the join blocks until that sleep elapses naturally. So the *actual* reconnect to the next pool can be delayed by up to ~30s beyond the already-real ~31s backoff before it even starts. This was invisible on the fast x86_64 sandbox (total scenario time ~64s, comfortably under a 60s test budget by luck of the timing) but became directly visible on-device: the first on-device test run failed with `"handshake timed out"` after the mock server's own accept-timeout budget (60s) was exceeded by the combined ~31s backoff + up to ~30s stale-join delay. Fixed *the test* by widening its timeout budget (150s wait, 300s ctest `TIMEOUT`) to accommodate this real, if suboptimal, production latency — not by changing production code. A tighter production fix would need `reconnect_loop()`'s sleep to be interruptible (e.g. a `condition_variable` instead of `sleep_for()`), which is a real design change beyond this test-writing task's scope.

Both are real behaviors worth knowing about but weren't part of this test-writing task's scope to fix — flagged here for future prioritization.

## Files Changed

| File | Change |
|------|--------|
| `src/pool_manager.cpp` | Fixed the `tick()`/`connect_to_current()` self-deadlock by releasing `stratum_mutex_` before the deferred reconnect call |
| `tests/test_pool_protocol.cpp` | New file (PLAN.md §3.2): 5 mock-Stratum scenarios, including the one that caught this bug; timeouts sized to absorb the stale-join latency described above |
| `CMakeLists.txt` | `test_pool_protocol`'s ctest `TIMEOUT` raised to 300s for the same reason |

## Verification

- **x86_64 (local):** full `ctest` 4/4 passing, including `test_pool_protocol`'s 5 scenarios. Positive confirmation: the exact same test hung indefinitely before the deadlock fix and passes cleanly after it.
- **AArch64 (on-device):** full `ctest` 7/7 passing, including `test_pool_protocol`'s 5 scenarios with the widened timeout budget. First on-device attempt (before widening the timeout) surfaced the stale-join latency finding above via a real, informative failure — not a hang — confirming the deadlock fix itself was solid even when the test's own timeout was too tight.

## Lessons

1. **A mutex that's locked internally by a "leaf" function must never be held by a caller invoking that function.** `connect_to_current()`'s self-managed locking was a reasonable design in isolation; the bug was entirely in a caller not respecting that contract.
2. **Untested failover/retry paths are exactly where deadlocks hide**, because they only execute under a specific, less-common runtime condition (here: a full retry-exhaustion-then-rotate cycle) that's easy to never hit in manual testing against a single healthy pool.
3. **A test that hangs instead of crashing is still a finding, not a timeout to raise.** The instinct when a new test hangs should be "is my test wrong, or is the code wrong" — in this case it was the code, and tracing the exact lock scope directly (no debugger needed) confirmed it in minutes once suspected.
4. **Fixing a hang can unmask a second, smaller timing issue underneath it.** The deadlock was masking how slow real failover actually is (backoff + stale-thread-join); once the hang was gone, the on-device run's real timing became visible for the first time and needed its own (test-side) accommodation.
5. **Real hardware timing assumptions don't transfer from a fast x86_64 sandbox.** The original 60s test budget happened to be just enough on x86_64 but not on the slower, more loaded on-device target — the same lesson as this session's earlier fast-mode dataset-corruption and Argon2 NEON work: always confirm timing-sensitive tests on the actual target hardware, not just wherever development happens to be fast.
