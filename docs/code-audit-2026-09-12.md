# General code audit — 2026-09-12

Scope: current working tree at `60b49b4`; mining and dataset lifecycle, pool
transport/reconnection, TLS configuration, JSON parsing, memory configuration,
and selected VM/JIT integration. This is a source review with host checks, not
an exhaustive security or AArch64 instruction audit. No implementation fixes
were made. Existing untracked `bigg/` was outside scope.

## Findings

### P1 — Seed rotation overwrites the prefix while old hashes can read it

`src/mining_engine.cpp:254` restarts fill on the existing PartialDataset before
incrementing the fill generation. Workers check that generation only between
hashes (`:490`), with no acknowledgement/barrier before the writes start.
`src/vm.cpp:832` snapshots the prefix pointer and bound for JIT execution;
resetting the atomic bound cannot invalidate a bound already loaded by a hash.
An old-seed computation can therefore read new-seed bytes during refill,
producing a mixed-seed hash. Shared ownership prevents freeing the buffer, not
concurrent mutation. This is a source-established race; no A53 reproduction was
run. The existing rotation test checks the resulting prefix, not every hash
that overlaps rotation (`tests/test_mining.cpp:434`).

Fix direction: stop and acknowledge all readers before reusing the allocation,
or publish an immutable replacement generation while retaining old ownership.
On memory-constrained A53 devices the reader barrier avoids doubling capacity.
Add a deterministic mid-hash rotation test covering both generations.

### P1 — Shutdown hangs before the first partial-dataset fill

`MiningEngine::worker_loop` immediately waits for fill (`src/mining_engine.cpp:389`).
`stop()` only clears running and notifies the unrelated full-dataset condition
variable (`:201`); `PartialDataset::wait_for_fill` checks its own completion/stop
flags (`src/partial_dataset.cpp:281`). No fill is ever started if the pool never
delivers a job. The engine joins a worker that cannot wake. The actual pool app
starts workers before connecting (`src/miner_app.cpp:395`). During an active
fill, shutdown also waits for the whole fill rather than cancelling promptly.

Reproduced with a one-item PartialDataset, engine.start(), and engine.stop()
without a job: a three-second timeout exits 124 after printing the stop call;
it never prints the return. Harness: `/tmp/armrx-audit-stop.cpp`.
Fix direction: make waits engine-cancellable, notify on stop, and make fill
cancellation observable between bounded chunks. Do not rely on destruction
to cancel work while the engine is still joining readers.

### P1 — Pool replacement can deadlock on an error callback

`src/pool_manager.cpp:66` replaces the owning unique_ptr while holding
stratum_mutex_. Destruction calls disconnect(), which joins reader/reconnect
threads. Their error callback calls current_pool_name(), taking that same
mutex (`src/miner_app.cpp:379`). If replacement overlaps a callback (for example
retry exhaustion), the callback waits for the lock while replacement waits for
the callback thread. The dedicated disconnect path already avoids this lock
ordering, but connect_to_current does not. Source finding, not reproduced.

Fix direction: move the old client out under the mutex, destroy it unlocked,
then publish/configure the replacement with explicit lifecycle serialization.
Exercise retry exhaustion/failover with a callback querying manager state.

### P1 — Builds without OpenSSL silently ignore requested TLS

`--tls` is accepted unconditionally (`src/cli_parser.cpp:266`), but the TLS
connection block exists only under ARMRX_HAVE_TLS
(`src/stratum_client.cpp:196`). Without OpenSSL the client continues through
plain TCP and sends the login, despite the user's encryption request.
A TLS-only endpoint may reject it, but a plaintext endpoint can receive it.
Source finding; no credentials were transmitted during this audit.

Fix direction: reject requested TLS before opening a socket when unavailable,
and test a build configured with CMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE.

### P2 — Partial capacity bypasses both dataset and RAM limits

`src/cli_parser.cpp:324` permits up to 1,048,576 MiB, while the RandomX dataset
is only about 2,080 MiB. `src/miner_app.cpp:651` allocates/prefaults that request
without adding it to required_bytes or checking available RAM. Requests above
the dataset extent can also reach initialize_dataset's range exception inside
an uncaught fill thread (`src/partial_dataset.cpp:211`), terminating the process
if allocation succeeds. Smaller requests can exhaust the A53's RAM reserve.
Source finding; intentionally did not attempt an OOM reproduction.

Fix direction: check algorithmic capacity and total memory budget before mmap,
then report errors on the calling thread. Include all scratchpad buffers.

### P2 — Empty fill affinity list divides by zero

`PartialDataset::start_fill(cache, {})` falls back to the same empty core list,
computes at least one worker, then evaluates i % avail_cores.size()
(`src/partial_dataset.cpp:155`). The public API documents no nonempty-list
precondition. Normal engine topology paths generally supply a list, limiting
this finding primarily to library callers.

Fix direction: reject an empty list explicitly or support unpinned fill workers;
also validate the cache argument before launching threads.

## Validation

- Native x86_64 Release build with native tuning and LTO disabled: passed.
- Five host CTest targets passed: armrx_tests, test_config, test_cli_parser,
  test_aes_hash, and test_partial_dataset.
- test_pool_protocol initially failed because sandbox policy blocked socket
  creation; rerun with permitted loopback access passed all protocol tests.
- test_mining remained running without a final result; the audit run was
  interrupted rather than waiting for its 900-second ceiling. It includes
  multiple full-size dataset builds. Its result is inconclusive, and the full
  suite is not claimed green.
- Deterministic no-first-job shutdown harness: reproduced timeout (exit 124).
- No device deployment, JIT equivalence run, performance benchmark, or fixes.

The highest-priority work is the partial-dataset generation/shutdown lifecycle;
it affects the project's largest practical light-mode optimization directly.

## Resolution — 2026-09-12 (same-day follow-up)

All six findings were independently re-verified against source before fixing
(none were found inaccurate) and are now fixed, tested, and validated on host
(both with and without OpenSSL) plus AArch64/musl cross-compilation. On-device
(A53) runtime execution was attempted but the device went offline
(`ssh: No route to host`) partway through deployment; see "Validation" below
for exactly what is and is not covered.

### P1 — Seed rotation overwrites the prefix while old hashes can read it — FIXED

Confirmed exactly as described: `vm.cpp`'s `run_jit()` snapshots
`item_count_atomic()` once per `VirtualMachine::run()` call and the JIT-compiled
code then makes many internal accesses against that one snapshot, so resetting
the atomic bound cannot stop a hash already using the old one, and the old
`start_fill()` began overwriting `data_` immediately after the reset with no
handshake at all.

Fix: `PartialDataset` gained a reader-quiescence barrier —
`PartialDataset::ReadGuard` (a hand-rolled, **writer-preferring** reader-writer
lock, `include/armrx/partial_dataset.hpp`'s `RotationLock`) held by
`MiningEngine::worker_loop` for the full duration of every
`randomx_calculate_hash`/`randomx_calculate_hash_pipelined` call that touches
the partial dataset (`src/mining_engine.cpp`). `PartialDataset::start_fill()`
takes the exclusive side only around the moment it resets
`item_count_`/`contiguous_done_`/`fill_complete_` to "not ready"
(`src/partial_dataset.cpp`), which cannot proceed while any hash holds the
guard, and any hash that starts after the reset sees `item_count() == 0`
(forcing the always-safe derive path) until the existing atomic
contiguous-publish protocol republishes real bytes. This closes the race
**without a second large allocation** (a plain `std::shared_mutex` was tried
first and rejected — see the "regression caught during fix" note below).

Regression caught during fix: the first implementation used
`std::shared_mutex`, which makes no fairness guarantee. Two continuously-
hashing workers starved `start_fill()`'s writer lock indefinitely — reproduced
directly as a 900+ second hang in `test_light_mode_seed_rotation_rebuilds_partial_dataset`.
Replaced with a hand-rolled writer-preferring lock (blocks *new* readers as
soon as a writer is waiting, bounding the writer's wait to however long the
currently-active readers take, not however long the reader stream continues).
Deliberately not `pthread_rwlock` +
`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`: that attribute is a glibc
extension the musl cross-compile target does not implement.

Tests: `test_partial_dataset.cpp`'s
`test_readguard_blocks_rotation_until_released` (deterministic: proves
`start_fill()` cannot proceed while a guard is held, and proceeds immediately
after release) and `test_concurrent_rotation_no_mixed_generation` (stress:
6 rotations racing a continuous reader, asserts no read ever matches neither
seed A's nor seed B's reference — the actual failure mode this bug would
produce). `test_mining.cpp`'s existing
`test_light_mode_seed_rotation_rebuilds_partial_dataset` now also validates
the fix doesn't reintroduce the starvation regression (it must complete in
seconds, not hang).

### P1 — Shutdown hangs before the first partial-dataset fill — FIXED

Confirmed exactly as described, and reproduced with the audit's own harness
(`/tmp/armrx-audit-stop.cpp`, still present) before fixing.

Fix: `PartialDataset::cancel()` (new) wakes every current/future
`wait_for_fill()`/`wait_until_published()` waiter regardless of whether
`start_fill()` was ever called, by signalling the (now safely-synchronized,
see below) `stop_` flag and notifying `fill_cv_`.
`MiningEngine::stop()` calls it right after `running_.store(false)`. Separately,
`PartialDataset::fill_worker()` now runs its assigned item range as a loop of
internal sub-chunks (`kFillChunkItems` = 64 MiB each) and checks `stop` between
them instead of one uninterruptible `initialize_dataset()` call over the whole
assigned range — a single-core fill of a large `--dataset-mb` request could
previously run for minutes before shutdown could observe cancellation at all
(matches AGENTS.md's "~164s on 8 A53 cores" note for full fills; a single-core
fallback is proportionally longer). Threads are still always joined, never
detached (`PartialDataset`'s destructor and `MiningEngine::stop()` both
unchanged in that respect).

Incidental fix required for correctness: `stop_` (a `shared_ptr<atomic<bool>>`)
is reassigned by `start_fill()` and read by `wait_for_fill()`/
`wait_until_published()`/`cancel()` from different threads; this was a latent
unsynchronized-shared_ptr race (distinct from the atomic `bool` it points to).
Both sides now go through `fill_cv_mutex_`.

Tests: `test_mining.cpp`'s `test_stop_before_first_job_returns_promptly`
(direct port of the audit harness, bounded via `std::async`+`wait_for` so a
regression fails the assertion instead of hanging the suite) and
`test_stop_during_active_fill_returns_promptly` (engine-level, stop() called
seconds into a live fill). `test_partial_dataset.cpp`'s
`test_cancel_before_any_fill_unblocks_waiter` (no `start_fill()` ever called)
and `test_cancel_bounds_active_fill_to_one_subchunk` (forces a single-core,
6-sub-chunk fill, cancels ~0.3s in, and asserts completion within a bound
derived from a **self-calibrated** measurement of one sub-chunk's fill time on
whatever host/device is actually running the test — not a hard-coded
wall-clock number; this is the test that actually proves "bounded by one
sub-chunk," not just "eventually returns." A first version used a fixed 20s
bound tuned to an x86_64 dev workstation (~6.9s/sub-chunk there) and flaked
(timed out) on first on-device execution: the Cortex-A53 target took ~82.6s
for the same 64 MiB sub-chunk, ~12x slower. Self-calibrating fixed it; see
"Validation" below for the actual on-device numbers).

### P1 — Pool replacement can deadlock on an error callback — FIXED

Confirmed exactly as described (source finding, not reproduced by the
original audit; still not reproduced deterministically here either — see
below). Fix: `PoolManager::connect_to_current()` (`src/pool_manager.cpp`) now
moves the old `StratumClient` out of `stratum_` under `stratum_mutex_`,
releases the lock, and only then lets it destruct (mirroring
`disconnect()`'s existing pattern) before constructing and installing the
replacement under a fresh lock acquisition. `~StratumClient()`'s thread joins
(and therefore its error callback) never run while `stratum_mutex_` is held.

Test: `test_pool_protocol.cpp`'s
`test_pool_replacement_no_deadlock_during_failover` wires the error callback
exactly as `miner_app.cpp` does (calls back into `current_pool_name()`) and
hammers `current_pool_name()`/`is_connected()`/`reconnect_attempts()` from a
separate thread throughout a real ~31s retry-exhaustion-triggered failover
(the same production 5-retry/1s-base backoff `test_pool_failover` already
exercises), bounded by a 60s `wait_until`. This is a stress/soak test, not a
single deterministic interleaving — the exact race requires the callback to
land while replacement holds the lock, which is not directly controllable
without adding test-only instrumentation to production code (out of scope per
this task's "do not perform unrelated refactoring"). It passed on both the
old and new code in practice (the window is narrow), so its main value is as
a **regression guard against the class of bug**, verified correct by
inspection: the fix's own critical sections no longer overlap the join at
all, which is a structural (not probabilistic) guarantee.

### P1 — Builds without OpenSSL silently ignore requested TLS — FIXED

Confirmed exactly as described. Fixed at two layers per the fix direction's
"cover CLI/config and direct client usage":
1. `CommandLineParser::parse()` (`src/cli_parser.cpp`) rejects a resolved
   `pool_tls == true` (from `--tls` or a config file) with exit code 64 when
   `ARMRX_HAVE_TLS` is not defined, before `MinerApp` is even constructed.
2. `StratumClient::connect()` (`src/stratum_client.cpp`) throws
   `std::runtime_error` if `tls_enabled_` is set and `ARMRX_HAVE_TLS` is not
   defined, before DNS resolution or socket creation — covers direct
   `StratumClient`/`PoolManager` API usage that bypasses the CLI, and every
   `reconnect_loop()` retry (both funnel through `connect()`).

Tests: `test_cli_parser.cpp`'s `test_tls_rejected_without_openssl_support`
(and `test_wallet_password_tls` updated to branch on `ARMRX_HAVE_TLS`) and
`test_pool_protocol.cpp`'s `test_tls_unavailable_rejected`
(`#ifndef ARMRX_HAVE_TLS`-guarded). Both were run under **both** configurations
as required: the normal build (OpenSSL found) and a second build directory
configured with `-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE` ("OpenSSL not
found — TLS pool connections disabled" confirmed in the CMake configure log),
where the rejection paths actually execute and pass.

### P2 — Partial capacity bypasses both dataset and RAM limits — FIXED

Confirmed exactly as described. Fix, at the point of use
(`MinerApp::run()`, `src/miner_app.cpp`) and defense-in-depth in the class
itself:
- New `armrx::validate_partial_dataset_request(dataset_mb, workers,
  available_bytes)` (`include/armrx/memory.hpp` / `src/memory.cpp`, pure
  function over integers) checks, in order: MiB->bytes overflow, the request
  against the real dataset extent (`randomx_dataset_item_count() *
  kRandomXDatasetItemBytes`, ~2080 MiB), and the total memory budget (256 MiB
  Argon2 cache + `workers * 2 MiB` scratchpads + the partial dataset itself +
  the existing `kAutoModeSafetyReserve`) against `available_memory()`.
  `MinerApp::run()` calls it before constructing `PartialDataset` and exits
  with code 64 and a specific message on failure — the calling thread, not a
  background fill thread.
- `PartialDataset`'s constructor (`src/partial_dataset.cpp`) independently
  rejects `item_count > randomx_dataset_item_count()` (and byte-conversion
  overflow) with `std::invalid_argument` before any `mmap`, so any caller
  (not just `MinerApp`) is protected, not only the CLI path.
- `PartialDataset::fill_worker()` now catches exceptions from
  `initialize_dataset()` (`std::invalid_argument`/`std::runtime_error` for an
  out-of-range span or an unusable cache) instead of letting them escape a
  background thread and call `std::terminate()`; it marks a new
  `fill_failed()` flag, signals `stop_`, and wakes waiters — a last-resort
  backstop, not the primary defense, since the two checks above should make
  it unreachable via the application.

Tests: `test_partial_dataset.cpp`'s `test_validate_partial_dataset_request`
(the primary regression test — a pure function, so every boundary is checked
**without allocating anything**: disabled/fits/exceeds-dataset-extent/
exceeds-memory-budget/MiB-overflow) and
`test_constructor_rejects_oversized_item_count` (also allocation-free: the
constructor throws before `mmap`). The fill-thread exception backstop itself
is not exercised by a dedicated test — doing so cheaply would require either
an oversized (~2 GiB+) allocation or bypassing `Argon2dCache`'s own
construction validation, out of proportion to a path the two synchronous
checks above already make practically unreachable; it is verified by code
inspection (correct exception types caught, correct scope, no leaked
`std::terminate` path) instead.

### P2 — Empty fill affinity list divides by zero — FIXED

Confirmed exactly as described (verified the exact `i % avail_cores.size()`
with `avail_cores` empty via `core_order` empty and the "fallback: allow
sharing" line ALSO reproducing empty when `core_order` itself is empty).
Chose "reject invalid input" (documented in `PartialDataset::start_fill()`'s
header doc comment) over "support unpinned workers", since the engine's own
caller always supplies a real, non-empty core list and there is no unpinned
fill mode elsewhere in the codebase to be consistent with.

Fix: `start_fill()` now throws `std::invalid_argument` up front, before any
thread starts, for an empty `core_order` or a null `cache_holder` (the
"validate the cache argument" half of the fix direction — a null cache would
otherwise null-dereference inside a fill thread).

Test: `test_partial_dataset.cpp`'s `test_start_fill_rejects_invalid_input`
covers both cases and confirms the object remains usable afterward with valid
input (rejecting bad input must not corrupt internal state).

## Validation

**Host, x86_64, OpenSSL present (`build/`):** full `ctest` (excluding the
pre-existing, unrelated, multi-minute JIT-scheduler stress tests) — **7/7
passed**, including `test_mining` (previously timed out at 900s under the
first, buggy `std::shared_mutex` fix attempt; now ~3-4 minutes) and
`test_pool_protocol` (~35s, includes the new deadlock regression test).

**Host, x86_64, `-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE` (`build-notls/`):**
CMake confirms "OpenSSL not found — TLS pool connections disabled".
`armrx_tests`, `test_config`, `test_cli_parser`, `test_aes_hash`,
`test_partial_dataset`, `test_mining` (208s) — **6/6 passed**;
`test_pool_protocol` run separately — **passed** (includes
`test_tls_unavailable_rejected`, which only exists in this configuration).

**AArch64/musl cross-compile (`build-cross/`, `cmake/toolchain-aarch64-musl.cmake`):**
configures and builds clean (LTO auto-disabled per the toolchain's known GCC+musl
history; JIT and hardware AES enabled; OpenSSL absent as expected for this
target) for `armrx`, `test_mining`, `test_partial_dataset`, `test_pool_protocol`,
`test_cli_parser`, `test_aes_hash`, `test_config`, `armrx_tests`.

**On-device (A53, `mechres@192.168.10.156`, Cortex-A53, `~1.3 GiB` available):**
the device went offline once mid-session (`ssh: connect to host ... No route
to host`) partway through deployment and came back a few minutes later;
noted here since it's an honest part of the record, not because it affected
the result — everything below ran to completion after it reconnected.
Cross-compiled binaries were `scp`'d to `/tmp/cross/` and run directly (not
through the on-device native `devbox_build`/`devbox_test` path, to use the
~10x-faster cross-compile iteration AGENTS.md recommends):
- `armrx_tests`, `test_cli_parser`, `test_config`, `test_aes_hash`: **passed**
  (includes the JIT-vs-interpreter KAT in `armrx_tests` — byte-identical on
  real AArch64+crypto hardware).
- `test_partial_dataset`: **passed in full**, including every new test for
  findings P1/P2/P5/P6 above. This is the direct evidence for the
  seed-rotation and shutdown/cancellation fixes on the actual target CPU, not
  just cross-compiled-but-unexecuted code. Concretely measured on this device:
  one 64 MiB single-core fill sub-chunk takes **~82.6s** (~12x slower than the
  ~6.9s measured on the x86_64 dev host) — confirming the self-calibration fix
  above was not optional, and independently corroborating AGENTS.md's existing
  "~164s on 8 A53 cores" full-dataset-fill note (single-core-equivalent scales
  up consistently). `test_cancel_bounds_active_fill_to_one_subchunk`'s
  self-calibrated bound correctly adapted (baseline 82.62s, bound 247.86s,
  actual cancellation 82.65s) and passed.
- `test_pool_protocol`: **passed in full**, including
  `test_pool_replacement_no_deadlock_during_failover` and
  `test_tls_unavailable_rejected` — direct evidence for the pool-replacement
  and TLS-rejection fixes on-device. Network/backoff-bound tests ran at
  essentially host-equivalent wall-clock time (real timer delays, not
  CPU-bound), unlike the compute-bound partial-dataset fill work above.
- `test_mining`: **passed in full**, including
  `test_light_mode_seed_rotation_rebuilds_partial_dataset` (the original,
  pre-existing regression test for the seed-rotation bug — now exercised
  end-to-end through the real `MiningEngine`/JIT hash pipeline on the actual
  target CPU, not just at the `PartialDataset` unit level) and both new
  `test_stop_before_first_job_returns_promptly` /
  `test_stop_during_active_fill_returns_promptly` tests. This device has only
  `~1.3 GiB` available, below what fast mode needs, so
  `test_fast_mode_dataset_reinit_via_workers` and
  `test_stop_races_dataset_reinit` (fast-mode-only, unrelated to any of the
  six findings) correctly SKIPPED via their existing
  `fast_mode_fits_on_this_host()` guard rather than running the ~164s+
  full-dataset builds AGENTS.md documents for this device.

**Every one of the six findings is now confirmed fixed by tests that actually
executed on the AArch64/musl cross-compiled binaries running on the real
Cortex-A53 target** (`test_partial_dataset`, `test_pool_protocol`, and
`test_mining` all green), in addition to the deterministic/bounded host
coverage above. Partial-dataset hashes across live seed rotation, shutdown
during startup/refill, pool replacement, and TLS rejection were all
specifically re-verified against the request's own validation checklist on
real hardware, not just cross-compiled-but-unexecuted code.

## Round 2 — independent review, 2026-09-12 (same day)

An independent review of the round-1 fixes above found four remaining gaps.
All four were re-verified against source (confirmed accurate, none disputed)
and are now fixed.

### P1 — Seed rotation still permits old-cache/new-prefix hashes (round 1's gap)

Confirmed exactly as described. The round-1 `ReadGuard` barrier stops
`start_fill()` from tearing down the buffer *while* a guard is held, but a
worker's own generation checks (top of `worker_loop`, `src/mining_engine.cpp`)
happen *before* the guard is acquired for the actual hash call. A rotation
can complete entirely inside that window: the worker then acquires the guard
holding a stale `active_cache_`/VM setup from the OLD seed while the buffer
already reflects the NEW seed — an internally inconsistent hash (VM/superscalar
state from one seed, some dataset bytes from another), matching neither
seed's reference. The round-1 rotation test still only checked the final
buffer contents, exactly as its own comment (`tests/test_mining.cpp`) admitted.

Fix: `worker_loop` now snapshots `partial_dataset_fill_generation_` at the
exact moment `active_cache_` is refreshed (`local_partial_fill_gen_for_cache`,
read inside the same `job_mutex_` critical section as `shared_cache_`, so the
two are mutually consistent), then re-compares against the *current*
generation immediately after acquiring the `ReadGuard`, before every hash
call (both the prime and pipelined paths). Because the guard blocks any new
rotation from completing while held, a match at that point guarantees
`active_cache_` and the prefix belong to the same generation for the entire
hash; a mismatch skips the hash for that iteration instead of computing a
silently wrong one, and the top-of-loop checks resync on the next pass. This
is the "generation validation and reader admission as one synchronized
operation" the review asked for.

Test: this is a narrow timing race between a worker's checks and its later
guard acquisition, so — as with the pool-replacement deadlock in round 1 —
it cannot be forced into one exact deterministic interleaving without adding
test-only instrumentation hooks to production code. `test_mining.cpp`'s new
`test_rapid_seed_rotation_hashes_never_mixed` instead hammers repeated
back-to-back rotations (no settling wait between them, unlike the existing
sibling test) across two workers while continuously hashing, and
cross-validates *every* observed hash against an independently-built
reference for its own claimed seed — a hash computed from a stale cache but
a rotated prefix would, in general, match neither seed's reference (VM
execution depends on the cache far beyond the few dataset bytes a small
prefix can shortcut), so a mismatch here is exactly the bug's signature.

### P1 — Cancellation can expose a concurrent thread-vector mutation (new)

Confirmed exactly as described. `start_fill()`'s thread-spawning loop
(`fill_threads_.emplace_back(...)`) mutated `fill_threads_` without holding
`fill_join_mutex_`, the same mutex `wait_for_fill()` uses to join and clear
that vector. Before `cancel()` existed, `wait_for_fill()` could only reach
its join+clear section once `fill_complete_` became true *naturally* —
which could only happen long after the spawn loop had already finished — so
the two could never actually overlap. `cancel()` (round 1's own fix) broke
that invariant: it can wake a worker's `wait_for_fill()` early (via the
`stop_` token or, now, `shutdown_requested_`) at any time, including while a
concurrent `start_fill()` (a live rotation, on a different thread) is still
inside its spawn loop — a genuine, unsynchronized concurrent read+write of
the vector, not just a logical race.

Fix: the spawn loop (`src/partial_dataset.cpp`) is now wrapped in the same
`fill_join_mutex_` used by the initial join+clear and by
`wait_for_fill()`'s final join+clear, so all three `fill_threads_` accesses
are mutually exclusive.

Verification: this is a pure data race requiring the spawn loop and a
cancel-triggered join to genuinely overlap in time — thread creation is
fast (microseconds per thread), so reproducing the overlap deterministically
without test-only hooks is impractical, and a tool built for this (TSan) is
not part of this project's current build configuration. Fixed and verified
by inspection (the fix is a direct, minimal application of the mutex already
used for the other two accesses to the same vector) and by confirming the
existing concurrent-stop tests (`test_stop_during_active_fill_returns_promptly`,
`test_cancel_bounds_active_fill_to_one_subchunk`) still pass on host, the
non-TLS host build, and on-device.

### P2 — A concurrent refill can erase shutdown cancellation (new)

Confirmed exactly as described. `cancel()` reads the *current* `stop_`
token and signals it, but `start_fill()` can replace `stop_` with a fresh
(`false`) token for a new rotation immediately after `cancel()` read the old
one — the mutex fix from round 1 makes that reassignment itself race-free,
but does not stop the *logical* race: the signal lands on a token nobody is
looking at anymore, and the new generation's fill workers/waiters would wait
for the entire refill with no way to know shutdown was requested.

Fix: added a persistent, never-cleared `shutdown_requested_` flag, set first
(and unconditionally) by `cancel()`, independent of whichever `stop_` token
happens to be current. Every wait/cancellation check in `partial_dataset.cpp`
now checks it alongside the per-generation token (`wait_for_fill()`,
`wait_until_published()`, `fill_worker()`'s two checks, and the
fill-complete-marking guard); `start_fill()` also refuses to start once it is
set, as a cheap (but not load-bearing) optimization. This is a one-way,
terminal flag — correct because `cancel()`'s only caller
(`MiningEngine::stop()` and the destructor) is itself terminal; nothing in
this codebase restarts a `PartialDataset` after cancelling it.

Test: `test_partial_dataset.cpp`'s new `test_cancel_persists_across_racing_rotation`
forces the exact race deterministically (not probabilistically) by holding a
`ReadGuard` to block a second `start_fill()` from completing its reset — so
it is provably still looking at generation 1's token when `cancel()` runs —
then releasing the guard so the blocked call proceeds and swaps in a new
token, exactly the "cancel signalled the old token; a new one is now
current" sequence the bug required. Self-calibrated (measures this
host/device's natural, uncancelled fill time first, then asserts the
cancelled run completes in a small fraction of it) rather than a hard-coded
bound, learning from the round-1 timing-portability mistake below.

### P2 — Memory validation still undercounts scratchpads (new)

Confirmed exactly as described. `worker_loop()` (`src/mining_engine.cpp`)
allocates `kScratchpadSize * 2` (two double-buffered 2 MiB scratchpads,
Track D2's pipelined hash+fill design) per worker, unconditionally — 4 MiB,
not the 2 MiB `randomx_worker_memory()` returns. The round-1
`validate_partial_dataset_request()` used that single-buffer figure
(deliberately mirroring `choose_randomx_mode()`'s existing accounting, which
has the same undercounting — a separate, pre-existing characteristic of
`randomx_worker_memory()`'s callers left unchanged here, since correcting it
would alter `choose_randomx_mode()`'s fast/light threshold decision, a
functional behavior change outside this audit's scope and this project's
"measure before changing a perf-relevant threshold" discipline), understating
the real budget by 2 MiB/worker (16 MiB at eight workers).

Fix: added `mining_worker_actual_scratchpad_bytes()` (`memory.hpp`,
`= 2 * randomx_worker_memory()`, documented as specifically reflecting the
engine's actual per-worker allocation) and switched
`validate_partial_dataset_request()` to use it instead.

Test: `test_partial_dataset.cpp`'s existing `test_validate_partial_dataset_request`
asserted `required_bytes` against `4 * randomx_worker_memory()` — the exact
same undercounting assumption the review called out ("the new test repeats
the same accounting assumption"). Updated to assert against
`4 * mining_worker_actual_scratchpad_bytes()`.

### Round 2 validation

Same three-tier validation as round 1, repeated for all four fixes:

- **Host x86_64 (`build/`, OpenSSL present):** `test_partial_dataset` and
  `test_mining` full suites green, including all new round-2 tests.
- **Host x86_64 (`build-notls/`, `-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE`):**
  same, green.
- **AArch64/musl cross-compile:** clean.
- **On-device (Cortex-A53):** `test_partial_dataset` — **passed in full**,
  including both new round-2 tests. Concretely:
  `test_cancel_bounds_active_fill_to_one_subchunk` (round 1, self-calibrated)
  measured 83.05s for one 64 MiB sub-chunk on this device and confirmed
  cancellation still resolves in ~83.03s (one sub-chunk), not the ~498s six
  sub-chunks would take. `test_cancel_persists_across_racing_rotation`
  (round 2, new) measured a 20.72s natural fill for its 16 MiB test size on
  this device, and confirmed the cancel-during-a-racing-token-swap scenario
  resolves in **0.001 seconds** — the token-swap race, if actually lost as
  the review described, would have made this wait approximately the full
  20.72s instead; a four-order-of-magnitude difference leaves no ambiguity
  about which behavior occurred. `test_mining` — **passed in full**,
  including `test_rapid_seed_rotation_hashes_never_mixed` (3 seed-A hashes
  collected across 6 back-to-back rotations, all cross-validated against
  their claimed seed's independently-built reference, none mixed-generation)
  and the original `test_light_mode_seed_rotation_rebuilds_partial_dataset`
  (still green end-to-end through the real JIT hash pipeline with the
  round-2 generation-revalidation fix in place) plus both round-1 shutdown
  tests.

**All four round-2 findings are confirmed fixed by tests that actually
executed on the real Cortex-A53 target**, on top of the six round-1 findings
already confirmed there. Every fix in both rounds now has on-device evidence,
not just cross-compiled-but-unexecuted code — the one exception remains the
pool-replacement deadlock and the round-2 thread-vector-mutation fix, both
of which are narrow-window data races verified by inspection and by the
absence of regressions in the concurrency-heavy tests that would be most
likely to surface them, rather than by a reproduction forcing the exact
interleaving (neither this project's current build nor this task's scope
includes TSan or test-only production instrumentation hooks, which are what
forcing those two specific races deterministically would require).

## Round 3 — reviewer follow-up on round 2, 2026-09-12 (same day)

The reviewer flagged one detail worth checking in round 2's persistent
`shutdown_requested_` flag, and asked for a steady-state performance check
of the `ReadGuard`'s overhead. Both addressed.

### Confirmed: permanent cancellation required fixing a real `PartialDataset` reuse site

The reviewer's concern was precise and correct: making `shutdown_requested_`
permanent (round 2) means a `PartialDataset` can never fill again once
`cancel()` has been called on it (via `MiningEngine::stop()` or its
destructor) — so anything that reuses one instance across two logical
mining sessions would silently and permanently lose the optimization for
the second session, with no warning.

Checking this project's actual usage found exactly that reuse: `MinerApp`
(`src/miner_app.cpp`) held a single `std::shared_ptr<PartialDataset>` member
constructed once in `run()`, and handed the *same* instance to both
`run_local_benchmark()` and `run_pool_mining()` — two independent
`MiningEngine` instances, each calling `engine.stop()` on its own way out.
`--mine` and `--pool` are independent CLI flags (`run()` calls
`run_local_benchmark()` then `run_pool_mining()` when both are set, not
either/or), so `armrx --mine --seconds=N --pool=... --dataset-mb=N` is a
real, directly reachable command line that would have silently disabled the
partial-dataset optimization for the pool-mining phase after the local
benchmark phase's `engine.stop()` ran.

Fix: `MinerApp` no longer owns a shared `PartialDataset`. It stores only the
*validated item count* (`partial_dataset_items_`, `include/armrx/miner_app.hpp`)
from `validate_partial_dataset_request()`, and each of `run_local_benchmark()`
and `run_pool_mining()` constructs its **own fresh** `PartialDataset` instance
from that count at the start of its own session. Each session's `PartialDataset`
still gets cancelled (permanently) when its own engine stops, exactly as
intended — but the next session, if any, starts clean rather than inheriting
a dead one. No other code path in this project reuses a `PartialDataset`
instance across an `engine.stop()`/restart boundary (checked: `main.cpp`
constructs one `MinerApp` and calls `run()` exactly once; nothing else holds
a `PartialDataset` across a `MiningEngine::stop()` call), so this is the only
site that needed it.

Verified functionally (not just by inspection): built and ran
`armrx --mode=light --mine --seconds=2 --dataset-mb=4 --workers=2 --pool=127.0.0.1:1 --wallet=test`
against an unreachable pool — the log shows **two separate**
`"PartialDataset: allocated 65536 items (4 MiB)"` lines, one for the mine
phase (`14:01:51`) and a second, independent one for the pool phase
(`14:01:54`), confirming the pool session gets a genuinely fresh instance
rather than the mine session's already-cancelled one.

### Steady-state A/B: the `ReadGuard`'s overhead on real hardware

Methodology: cross-compiled two binaries from the exact same toolchain/flags
— `armrx_before` from the pre-audit commit (`60b49b4`, via a `git worktree`,
so the comparison isolates only the code change) and `armrx_after` from the
current tree (all three rounds of fixes applied) — and ran both on-device
with identical arguments (`--mode=light --dataset-mb=64 --workers=4
--affinity-mode=big-only --mine --seconds=150 --warmup=60`, reporting
steady-state H/s over the post-warmup window the app already computes from
snapshot deltas). Run in reversed order (**after, before, after**) to guard
against thermal drift biasing the comparison in either direction, per this
project's existing A/B discipline (`AGENTS.md`).

Results (Cortex-A53, 4 workers pinned to the big cluster, 64 MiB partial
dataset, 90s post-warmup measurement window):

| Run | Binary | Steady-state H/s | Per-worker H/s |
|---|---|---|---|
| 1 | after  | 13.61 | 3.37 / 3.43 / 3.43 / 3.37 |
| 2 | before | 13.82 | 3.43 / 3.43 / 3.43 / 3.54 |
| 3 | after  | 13.66 | 3.37 / 3.43 / 3.48 / 3.37 |

**Correction (this section originally mischaracterized the direction of this
first result — flagged by reviewer, verified):** mean(after) =
(13.61 + 13.66) / 2 = 13.635 H/s vs. before = 13.82 H/s. After was
~1.34% lower, the direction a regression would produce, not the opposite as
first written. One `before` run cannot establish that baseline's own
run-to-run variability, so "within noise" was an unsupported claim from 3
runs alone — extended to 9 runs (6 more, alternating before/after/before/
after/before/after) below for a firmer read, per the reviewer's suggestion.

**Full 9-run result** (same device, same flags, same reversed-order
discipline continued):

| Run | Binary | Steady-state H/s |
|---|---|---|
| 1 | after  | 13.61 |
| 2 | before | 13.82 |
| 3 | after  | 13.66 |
| 4 | before | 13.72 |
| 5 | after  | 13.66 |
| 6 | before | 13.77 |
| 7 | after  | 13.66 |
| 8 | before | 13.72 |
| 9 | after  | 13.72 |

after (n=5): mean 13.662, stdev 0.039, range [13.61, 13.72].
before (n=4): mean 13.7575, stdev 0.048, range [13.72, 13.82].
Mean difference: before − after = 0.0955 H/s = **0.70%** — smaller than the
first 3-run snapshot's 1.34% and, critically, the ranges now *touch*
(after's max equals before's min at 13.72), where they didn't overlap at
all after 7 runs. A Welch t-statistic on this data is ≈3.2, which would
ordinarily suggest a real difference, but the observed values cluster on
only five distinct levels 0.05–0.06 H/s apart (13.61/13.66/13.72/13.77/13.82)
— consistent with the app's own H/s figure being total-hash-count/90s and
therefore quantized to a handful of achievable integer-hash-count outcomes
over that window, not a continuous measurement. A textbook t-test assumes
continuous, independent-noise data; it is not reliable evidence here given
that quantization, this device's already-documented thermal variance, and
n in the single digits.

**Honest conclusion (superseded below by a cleaner, isolated experiment): a
small (~0.7–1.3%, shrinking as more runs were added) throughput difference
in the regression direction is consistently observed, but this A/B design —
single device, n=9, non-normal quantized data — can narrow the question
without closing it.** It is not strong enough evidence to confirm a real
regression, and it is no longer strong enough to claim "no regression"
either (the original 3-run framing that said so was wrong to). Two
candidate explanations for a *real* small effect, neither verified: (1)
`RotationLock` serializes every worker's `lock_shared()`/`unlock_shared()`
(hash entry/exit) through one `std::mutex` — at 4 concurrent workers this
is genuine cross-core contention on a single cache line, not the
"uncontended" case the earlier nanoseconds estimate described, and nothing
here measured how that scales at the 8-worker recommended config, where
twice the contention on the same single mutex could plausibly cost more,
proportionally, than measured at 4 (**but this is only a hypothesis about
mechanism, not a measured scaling law — doubling worker count does not
necessarily double contention cost, and the follow-up below tests the
actual 8-worker config directly rather than assuming**); (2) ordinary
device variance this design (one A53 board, no repeated-session averaging)
cannot separate from (1). The follow-up below narrows (1) specifically —
whether isolating the guard reproduces this gap — without identifying what,
if anything, actually caused it.

### `RotationLock` isolated at 8 workers

The comparison above has a confound the reviewer identified directly:
`60b49b4` vs. the current tree differs by the *entire* fix set (six findings,
three review rounds), not by the guard alone. General PMU counters
(`devbox_perf_stat`) also would not attribute time to `RotationLock`
specifically — they report aggregate cycles/IPC/branch-misses, not
per-lock contention. Fixing both: built two binaries from the **identical**
current (all-fixes-applied) source tree, differing in exactly one place —
`PartialDataset::ReadGuard`'s construction is present (`armrx_guard`) or
removed (`armrx_noguard`) at both hash-call sites in `worker_loop`
(`src/mining_engine.cpp`), with the post-guard generation re-check left in
both (it is one atomic load, not what "`RotationLock` overhead" refers to).
This isolates the lock's marginal cost from every other change in this
audit, and from any compiler-codegen drift a wider source diff could
introduce. (The `noguard` binary is unsafe under real seed rotation — it
was only ever run against `--mine`'s single, non-rotating benchmark seed,
never against a pool.)

Run at **8 workers** (the actually-recommended config, not 4), with **240s
post-warmup windows** (vs. 90s before, to reduce the quantization step),
4 alternating pairs (guard/noguard × 4 = 8 runs total), retaining raw
`(hash_count, elapsed_seconds)` pairs at full `double` precision rather than
the app's rounded 2-decimal H/s print:

| Pair | Guard: hashes / seconds → H/s | No-guard: hashes / seconds → H/s |
|---|---|---|
| 1 | 5917 / 240.75845 → 24.5765 | 5850 / 240.68820 → 24.3053 |
| 2 | 5964 / 240.62123 → 24.7858 | 5867 / 240.66489 → 24.3783 |
| 3 | 5840 / 240.61118 → 24.2715 | 5875 / 240.65887 → 24.4121 |
| 4 | 5849 / 240.61655 → 24.3084 | 5903 / 240.70879 → 24.5234 |

guard (n=4): mean 24.4856, stdev 0.2420.
no-guard (n=4): mean 24.4048, stdev 0.0908.
Unpaired: mean diff (guard − no-guard) = **+0.0808 H/s = +0.33%** (guard
*higher*), Welch t ≈ 0.625. Paired by run order (pair 1 guard vs. pair 1
no-guard, etc., matching this project's own alternating-order discipline):
per-pair diffs +0.2712, +0.4075, −0.1406, −0.2150; mean +0.0808, t ≈ 0.529.

Both t-statistics are far below any threshold that would indicate a real
effect (would need roughly 3+ with this n), and the point estimate itself
goes the opposite direction from an overhead hypothesis.

**Conclusion, precisely scoped: no `RotationLock` throughput penalty was
detected in this 8-worker experiment, and retaining the correctness guard
is the right choice.** Two limits on that, stated plainly rather than
smoothed over:

1. **Four pairs do not establish zero overhead.** Absence of a detected
   effect at this n and precision is not proof the true effect is zero —
   only that if a penalty exists, it is too small for this experiment to
   resolve. That is a sufficient basis for the engineering decision (keep
   the guard; it fixes a real, confirmed correctness bug, and no throughput
   cost has been detected at the recommended configuration), but not a
   claim that no cost exists at any scale or configuration.
2. **This result does not retrospectively identify the cause of the
   earlier ~0.7–1.3% whole-change (`60b49b4` vs. current) slowdown.** It
   shows that isolating the guard specifically does not reproduce that gap
   here, which weighs against a guard-contention explanation for it, but it
   does not identify what *did* cause it (an earlier draft of this section
   claimed the isolated result "explains" the whole-change gap — that
   overstated what one experiment against a different comparison can show;
   corrected here). That earlier result's cause remains unidentified.

Per the reviewer's assessment: this evidence is sufficient to accept the
guard as shipped, and the performance investigation is closed here unless a
reproducible regression appears in real use — no further profiling
(`perf record`+annotate or otherwise) is being pursued on this basis.

## Round 4 — reviewer follow-up ("codex"), 2026-09-12 (same day)

### P1 — Generation counter published *after*, not atomically with, the buffer reset (round 2's gap)

Confirmed exactly as described. Round 2's fix compared a worker's cached
generation against `MiningEngine::partial_dataset_fill_generation_` while
holding `ReadGuard`, which correctly stops a rotation's exclusive reset from
running *concurrently* with a held guard — but that counter lived in
`MiningEngine`, not `PartialDataset`, and `set_job()` only incremented it
*after* `PartialDataset::start_fill()` had already returned. `start_fill()`
resets the buffer state and releases its exclusive `rotation_mutex_` section
before returning (so fill threads can start publishing), which left a real
window: a worker could acquire a *fresh* `ReadGuard` after the reset but
before `set_job()`'s later increment, see the old generation value still
match, and pass validation while hashing against a buffer already reset (and
possibly partially refilled with new-seed data) under the old seed's cache.
Holding the guard does not close this — it only prevents a *second*
concurrent reset, not a stale comparison against a counter that hadn't moved
yet.

Fix: removed `partial_dataset_fill_generation_` from `MiningEngine` entirely
and added `PartialDataset::generation()`, an atomic counter that lives
*inside* `PartialDataset` and is incremented inside the *same* exclusive
`rotation_mutex_` section that resets `item_count_`/`contiguous_done_`/
`fill_complete_` (`start_fill()`, `src/partial_dataset.cpp`), immediately
after `fill_failed_` is cleared and before the section releases. Because both
the reset and the generation bump happen under the same lock, before any
reader can observe either, they can never be seen out of step: any
`ReadGuard` acquired after `start_fill()` returns is guaranteed to see the
*new* generation together with the *new* (reset, possibly still filling)
buffer state — never the old generation paired with a reset buffer, which
was the exact gap. `worker_loop()`'s existing re-validation logic
(`src/mining_engine.cpp`) is otherwise unchanged: it still snapshots the
generation when refreshing `active_cache_` under `job_mutex_`, and still
re-compares immediately after acquiring `ReadGuard`, before every hash call —
only the counter it reads from moved, from a second, independently-timed
counter to the one `PartialDataset` itself publishes atomically with its own
state change. The reviewer's caution that "simply moving the increment
before `start_fill()` would create a different ordering problem" was heeded
by not doing that — the bump is inside `start_fill()`'s own critical
section, not moved to either caller side of it.

Test, two parts as requested ("a deterministic barrier around this
transition" plus "validation of hashes exercising it"):

1. `test_partial_dataset.cpp`'s new `test_generation_published_atomically_with_reset`
   is the deterministic part. It fills a single-core, multi-sub-chunk
   dataset (large enough that `start_fill()` returning does *not* imply the
   refill has finished), records the generation after a first complete fill,
   then calls `start_fill()` again for a second seed and — with no sleep,
   immediately upon that call returning — asserts three things together:
   `generation()` has already incremented, `item_count()` has already been
   reset (not still showing the old seed's full count), and a `ReadGuard`
   acquired at that exact moment sees the *same* new generation as whatever
   partial prefix is published under it, spot-checked against the *new*
   seed's reference (never the old seed's). This directly exercises the
   reviewer's described interleaving — a reader arriving right as
   `start_fill()` returns — deterministically, rather than by timing luck,
   because it is driven by program order against `PartialDataset`'s own
   synchronous contract (`start_fill()` does not return until the reset and
   generation bump are both visible), not by hoping two threads race a
   particular way.
2. `test_mining.cpp`'s `test_rapid_seed_rotation_hashes_never_mixed` had its
   final assertion strengthened from `!samples_a.empty() ||
   !samples_b.empty()` (true even if every rotation happened to land on one
   seed, exercising no real transition) to requiring *both*
   `!samples_a.empty()` and `!samples_b.empty()` — guaranteed deterministically
   by explicitly settling on each seed in turn after the rapid-fire
   no-settling-wait rotation loop and polling (bounded, not a fixed sleep
   guess) until at least one sample lands for each, rather than hoping the
   rapid loop happens to leave samples of both behind. Every collected
   sample, from both seeds, is still cross-validated against an
   independently-built reference hash for its claimed seed — a
   mixed-generation hash would, in general, match neither reference. Rerun
   on host: passed with 7 seed-A + 3 seed-B hashes verified, none mismatched.

### Round 4 validation

Host, TLS build (`build/`): full `ctest` — 7/7 passed (`armrx_tests`,
`test_mining`, `test_config`, `test_cli_parser`, `test_aes_hash`,
`test_partial_dataset`, `test_pool_protocol`), including both new/strengthened
tests above.

Host, non-TLS build (`build-notls/`): full `ctest` — 7/7 passed, same set.

Cross-compile (`build-cross/`, `cmake/toolchain-aarch64-musl.cmake`): clean
rebuild; `file` confirms `test_mining`, `test_partial_dataset`, and `armrx`
are genuine `ELF 64-bit LSB pie executable, ARM aarch64 ... ld-musl-aarch64.so.1`
binaries, not a stale x86_64 artifact.

Device (Cortex-A53, confirmed reachable): source synced, on-device native
build clean. `ctest -j1` (all 7 targets, run serially): **7/7 passed**,
including `test_mining` (336.84s, real JIT hash pipeline, both new/
strengthened round-4 tests) and `test_partial_dataset` (817.57s, including
`test_generation_published_atomically_with_reset` with real device timing —
observed generation 1→2 across a genuine ~91s single-core in-progress
refill).

One pre-existing, unmodified test (`test_contiguous_publish_no_uninitialized_read`,
dating to 2026-08-07's contiguous-publish work, unrelated to this round's
generation/reset fix) was observed to fail intermittently on this device
when run with CTest's default parallelism (`-j2`, `test_mining` and
`test_partial_dataset` running concurrently): 2 passes, 3 failures across 5
attempts, always the same assertion (`item_count() == kChunkItems`
immediately after `wait_until_published(kChunkItems)`), never the later
`violation`/uninitialized-read assertion. Re-run **serially** (`-j1`) and in
isolation, it passed consistently (3/3). Root-cause hypothesis, not fully
confirmed: the test hard-codes a 5000 ms artificial lag on one chunk,
reasoned (in its own comment) to be "far longer than 1M-item compute on any
AArch64 target" — under `-j2` contention this device's per-chunk compute
time apparently comes close enough to that margin that the contiguous
merge can legitimately advance past the intended observation point before
the test's very next line checks it (which would be a test-timing-margin
issue, not a data race — no run reached the uninitialized-read check, only
the earlier exact-equality timing assertion). 8/8 consecutive host runs
were clean. This is unrelated to the round-4 fix (the code path is not
touched by it, host is unaffected, and the failure mode disappears once
device CPU contention with `test_mining` is removed) and is not being
fixed as part of this change — flagged here rather than silently
worked around, per this audit's standing honesty requirement.
