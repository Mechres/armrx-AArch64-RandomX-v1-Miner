# Unattended Code/Architecture Audit — 2026-08-04

Scope: `src/jit_compiler_a64.cpp`, `include/armrx/jit_compiler_a64.hpp`, TUI render/teardown,
pool-mining lifecycle, `run_pool_mining`, and signal handling. Read-only source audit; no device
workloads were launched.

## Evidence labels

- **FACT:** directly verified in current source on this host.
- **INFERENCE:** plausible mechanism derived from source, not runtime-reproduced.
- **UNVERIFIED:** historical or device-specific claim accepted from project docs but not rerun.

## Prioritized defects

1. **FACT — TUI snapshot contains a dangling `std::string_view`.**
   - `run_pool_mining()` assigns `snap.pool_name = pool_mgr->current_pool_name()` at
     `src/miner_app.cpp:443`.
   - `PoolManager::current_pool_name()` returns a temporary `std::string` at
     `src/pool_manager.cpp:34-38`.
   - `TuiSnapshot::pool_name` is a `std::string_view` at `include/armrx/tui.hpp:21`.
   - The temporary string is destroyed before `tui->render(snap)` at `src/miner_app.cpp:463`.
   - Impact: scheduler-independent garbage bytes, invalid reads, or a segfault depending on timing
     and heap reuse.

2. **INFERENCE — Pool teardown has a lock-inversion deadlock path.**
   - `PoolManager::disconnect()` holds `stratum_mutex_` while calling
     `stratum_->disconnect()` (`src/pool_manager.cpp:96-101`).
   - `StratumClient::disconnect()` closes the socket and joins reader/keepalive/reconnect threads
     (`src/stratum_client.cpp:59-75,228-234`).
   - The reader thread can invoke `error_callback_("connection closed")` while being joined
     (`src/stratum_client.cpp:416-430`).
   - The registered callback calls `pool_mgr->current_pool_name()` (`src/miner_app.cpp:379-381`),
     which attempts the same mutex.
   - The joining thread therefore waits for a callback blocked on the mutex it owns. This is a
     concrete source-visible explanation for `--pool` failing to return after SIGINT.

3. **FACT — `StratumClient` shared socket/reconnect state is inconsistently synchronized.**
   - `sockfd_` is a plain `int` (`include/armrx/stratum_client.hpp:176`) read by send/read paths
     and closed by teardown without a shared mutex or atomic.
   - `reconnect_attempts_` is not atomic but is modified in `reconnect_loop()` and read from other
     threads (`include/armrx/stratum_client.hpp:113-125,211-214`; `src/stratum_client.cpp:732`;
     `src/pool_manager.cpp:123-147`).
   - `reconnect_thread_` is assigned/joined from reader, connect, reconnect, and teardown paths
     without shared synchronization (`src/stratum_client.cpp:97-106,418-426,228-234`).
   - Impact: C++ data-race/undefined-behavior risk independent of TUI behavior.

4. **FACT — `--pool-test --tui` bypasses TUI teardown.**
   - `std::_Exit(0)` at `src/miner_app.cpp:520-525` skips destructors.
   - The cursor hide sequence is emitted in `Tui::Tui()` (`src/tui.cpp:40-46`) and normally restored
     by `Tui::~Tui()`/`shutdown()` (`src/tui.cpp:48-60`).
   - `atexit` restoration is registered, but `_Exit()` does not run `atexit` handlers.
   - Impact: `--pool-test --tui` can leave the terminal with the cursor hidden and skips socket and
     worker cleanup by design.

5. **FACT — `JitCompilerA64::flags` is default-uninitialized for non-VM construction.**
   - `flags` has no initializer at `include/armrx/jit_compiler_a64.hpp:169`.
   - Production `VirtualMachine` sets it immediately (`src/vm.cpp:152-156`), so the shipped mining
     path is safe.
   - Impact: a direct/test user that forgets `setFlags()` reads indeterminate state in
     `emitV2AesTweak()` and `h_CFROUND()`.

6. **FACT — `MiningEngine::snapshot()` reports an uninitialized member on the empty path.**
   - `HashSnapshot::total` has no initializer (`include/armrx/mining_engine.hpp:113-116`).
   - `snapshot()` leaves it untouched when `worker_hashes_` is empty or `num_workers_ == 0`
     (`src/mining_engine.cpp:338-353`).
   - Impact: callers of `snapshot()` before `start()` can read an indeterminate total.

7. **FACT — TUI mode does not enable the logger's intended TUI sink.**
   - `armrx::log` has a `g_tui_mode` switch and a mutex-protected ring buffer to avoid racing on
     `std::cout` (`include/armrx/log.hpp:37-117`).
   - No call site enables it: `set_tui_mode` has no production callers (`grep
     "set_tui_mode" .`).
   - TUI frames write `std::cout` while job, share, pool, and metrics logs continue through the
     normal logger sink (`src/tui.cpp:157-160`; `src/miner_app.cpp:375-390`).
   - Impact: interleaved lines/control sequences are expected even after fixing the dangling
     pool-name view. The existing ring buffer is unused infrastructure for this bug.

8. **FACT — `PartialDataset` publishes out-of-order chunks as fully initialized.**
   - Fill workers are assigned disjoint contiguous item ranges (`src/partial_dataset.cpp:100-115`).
   - Each worker advances `item_count_` to `start_item + total_items` on completion
     (`src/partial_dataset.cpp:150-157`).
   - Because ranges run in parallel, a later chunk can finish first and publish a boundary over
     an earlier unfinished chunk.
   - The JIT treats every item below that boundary as initialized
     (`src/vm.cpp:824-831`).
   - Impact: potential reads of uninitialized dataset items and nondeterministic hashes when a
     partial dataset is in progress. This is a correctness bug, not merely a reporting issue.

9. **FACT — `PartialDataset::fill_complete_` is never set by fill workers.**
   - The only completion store is in `wait_for_fill()` (`src/partial_dataset.cpp:163-176`);
     `fill_worker()` does not set it (`src/partial_dataset.cpp:121-161`).
   - `fill_complete()` can therefore remain false after all items are complete unless
     `wait_for_fill()` is called.
   - The destructor checks `fill_finished` via `item_count_`, so this does not by itself cause
     teardown corruption, but it makes the public completion accessor stale.

10. **FACT — TUI worker-rate display can report substantially wrong values during sparse flush windows.**
    - `worker_hash_rate()` divides the worker's current flushed hash count by elapsed time since
      engine start (`src/mining_engine.cpp:316-324`).
    - Worker counters flush only every 64 hashes or one second
      (`src/mining_engine.cpp:614-622`).
    - On the target device each hash is roughly 100-200 ms, so short-lived idle/active transitions
      can produce visibly misleading bars. This is measurement/display-only, not consensus risk.

11. **FACT — `count_top_frequency_cores()` cannot detect the MSM8929 cluster split.**
   - It reads `cpuinfo_max_freq` and counts entries equal to the maximum
     (`src/mining_engine.cpp:144-169`).
   - On this device both clusters are A53 at one firmware frequency, so all cores count as "big".
   - `BigOnly` therefore degenerates to all-core placement (`src/mining_engine.cpp:356-374`).
   - Impact: the documented worker-cluster-placement lever needs a different topology probe.

12. **FACT — `MetricsExporter` shutdown can block indefinitely behind one idle HTTP client.**
    - The destructor flips `running_` and joins the worker (`include/armrx/metrics.hpp:86-89`).
    - The worker handles exactly one blocking `read()` per accepted connection
      (`include/armrx/metrics.hpp:57-79`).
    - If a client connects and sends nothing, `read()` can block; the destructor cannot unblock it
      because the listening fd is intentionally worker-owned.
    - Impact: SIGINT teardown can hang only when `--metrics-port` is enabled and a connection is
      idle. This is narrower than the unconditional pool SIGINT report.

13. **FACT — IPv6 pool addresses are parsed incorrectly.**
    - The parser uses `addr.rfind(':')` and treats the suffix as a port
      (`src/cli_parser.cpp:181-202`).
    - A bracketed IPv6 address such as `[2001:db8::1]:3333` is handled, but a bare IPv6 address
      such as `2001:db8::1` is interpreted as host `2001:db8:` with port `1`, or fails parsing.
    - Impact: bare IPv6 pools are unsupported despite `getaddrinfo(AF_UNSPEC)` supporting IPv6
      downstream (`src/stratum_client.cpp:108-131`).

## Assessment of the three OPEN bugs

### TUI garbage/control-byte artifacts

**Likely root cause:** the dangling `TuiSnapshot::pool_name` described above. Concurrent writes also
exist: worker share callbacks write `std::cout` (`src/miner_app.cpp:384-390`) while the TUI writes
the same stream (`src/tui.cpp:157-160`). That can interleave frames, but it does not explain the
lifetime violation. Fix should start with owning/copying the pool name, then serialize all TUI/stdout
writes if interleaving remains.

### TUI segfault under `ARMRX_DAG_SCHED=1`

**Likely root cause:** the same dangling view, not DAG code. DAG selection is exact-match
`ARMRX_DAG_SCHED == "1"` (`src/jit_compiler_a64.cpp:946-950`); otherwise the legacy scheduler is
used. DAG changes JIT emission timing and heap reuse enough to expose the existing invalid read.
This is an inference until a debugger backtrace is captured, but the source defect is real and must
be fixed regardless.

### SIGINT not cleanly exiting `--pool`

The SIGINT delivery fix is present: `sigaction(..., sa_flags=0)` at `src/miner_app.cpp:64-79` and a
100 ms polling loop at `src/miner_app.cpp:414-420`. The likely remaining defect is the teardown
deadlock described above, not the sleep-loop issue already fixed.

## DAG scheduler assessment

- **FACT:** it is default-off and only selected when `ARMRX_DAG_SCHED` exactly equals `"1"`.
- **FACT:** when off, the DAG-specific graph and order construction is not executed.
- **FACT:** `hasHazard()` is reused (`src/jit_compiler_a64.cpp:465-472,788-797`).
- **INFERENCE:** the reported +13.6M instructions/hash is credible because handlers have
  order-sensitive emission state, including register-change offsets (`src/jit_compiler_a64.cpp:2157-2176`)
  and immediate/literal handling. The exact extra-instruction source is **unverified** without fresh
  JIT dumps.
- **Do not re-enable:** the documented B-M-B-M result is a consistent −14.3% H/s regression.

## Remaining performance levers

1. **Worker-cluster placement — highest expected ROI, moderate implementation risk.**
   Same-frequency clusters mean frequency probing is insufficient. A topology/cluster-ID or measured
   cluster probe is needed before pinning workers to cores 0-3.

2. **Make `bench_armrx` genuinely multi-worker — low risk, prerequisite for measuring lever 1.**
   Current code is unambiguously single-threaded and has no `--workers` parser
   (`tests/bench_armrx.cpp:782-835,872-927`).

3. **`-mtune=cortex-a53` — low risk, but effectively closed/stale as a lever.**
   The cross toolchain already sets it (`cmake/toolchain-aarch64-musl.cmake:66-67`), and prior device
   measurement reported null (`docs/experiments/perf-tracking.md:170`).

4. **Pipelined worker allocation elimination — new low-risk candidate.**
   `MiningEngine::worker_loop()` copies `block_input` into a fresh `std::vector<std::byte>
   next_block` on every pipelined hash (`src/mining_engine.cpp:583-585`). Reusing a worker-local
   buffer could remove a per-hash allocation/copy. Expected ROI is probably small and must be
   measured; it is not a consensus/JIT change.

## Documentation/code inconsistencies

- `docs/TESTING.md:20-29` describes `bench_armrx --workers=<N>` as supported; `tests/bench_armrx.cpp`
  has no such option and runs one thread.
- `docs/briefs/2026-08-07-handoff.md:31-34` says cross-builds lack `-mtune`;
  `cmake/toolchain-aarch64-musl.cmake:66-67` already sets it.
- The same handoff calls `-mtune` open, while `docs/STRATEGY.md:19-24` and
  `docs/experiments/perf-tracking.md:170` describe it as measured null/closed.
- `changelogs.md:9` says W4 literal-pool machinery was removed, but
  `src/jit_compiler_a64.cpp:1294-1316` still reserves the pool and retains `cpool*` state. It is
  effectively dead because `emitCpoolImmediate()` currently emits `MOVZ/MOVN + MOVK`.
- `docs/experiments/perf-tracking.md:168` describes `emitCpoolImmediate()` as `LDR+ALU`; current
  implementation is the E24 three-instruction form (`src/jit_compiler_a64.cpp:1502-1506`).
- `docs/STRATEGY.md` attributes TUI artifacts primarily to terminal/fd serialization, but the direct
  dangling-view defect is visible in current source.

## Not verified

- No device commands or tests were run, per unattended-review and one-test-per-session constraints.
- No existing `/tmp/cross-dag/*.log` files were present on the host during the audit.
- Historical DAG performance numbers and reported TUI/SIGINT reproductions were not rerun.
- The exact instruction-generation source of the DAG's additional 13.6M instructions/hash remains
  unproven without controlled JIT dumps or device measurement.
