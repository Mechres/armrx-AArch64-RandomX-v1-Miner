# armrx — Master Plan Archive: Phases 1–5 (Completed)

> **Archived 2026-07-24.** This document is the completed-work half of `PLAN.md`, split out
> once every item in Phases 1–5 was done, so the live plan could stay focused on open work
> (currently Phase 6 — see `PLAN.md`). Nothing here is still open; it's kept verbatim as the
> historical record of *why* each change was made, not just *that* it was made. For the
> chronological, dated version of the same history, see `changelogs.md`; for the short-list
> actionable view at each point in time, see `NEXT_STEPS.md`.

> **Verification pass (2026-07-21):** every item below was re-checked against current HEAD (`61899d7`) before this revision. Two items turned out to rest on stale premises and were corrected in place rather than silently dropped — see §2.3 and §4.2. See also `docs/audits/audit-20260721-cross-reference.md` for a broader doc-vs-code cross-reference conducted the same day; check it before assuming any *other* project doc (`ROADMAP.md`, `STATUS_REPORT.md`, `NEXT_STEPS.md`) is current.
>
> **Critical fix landed mid-Phase-2 (2026-07-21):** implementing §2.1 surfaced a pre-existing, severe fast-mode dataset-corruption bug (silently zero-filling most of any multi-threaded dataset build) plus a project-wide `assert()`-silently-disabled-under-`-DNDEBUG` issue in the test suite. Both fixed; see `docs/postmortems/fast-mode-dataset-corruption-postmortem.md` for the full account and §2.1 below for the summary.

---

## 1. Code Architecture & Structural Integrity

### 1.1 Unsafe Thread Detachment in `MetricsExporter` — ✅ Done (2026-07-21)
*   **Bottleneck:** In [metrics.hpp](file:///home/mechres/Projeler/aarch64-randomx/include/armrx/metrics.hpp#L78), the Prometheus server spawns a background thread and immediately calls `thread_.detach()`. The metrics provider callback lambda captures local stack instances from `main()` (such as `engine` and `pool_mgr`) by reference. If the miner terminates or `MetricsExporter` is destroyed, the detached background thread can execute the callback on dangling references, causing a use-after-free segmentation fault on exit.
*   **Current state (verified):** the destructor (`metrics.hpp:81-84`) already sets `running_ = false` and calls `::shutdown(server_fd_, SHUT_RDWR)` — that half of the fix landed already. The gap is narrower than it looks: `thread_` is `.detach()`ed at construction (`metrics.hpp:78`), so it is no longer joinable by the time the destructor runs, and `thread_.join()` cannot simply be added to it as-is (a detached thread throws `std::system_error` on `.join()`). The use-after-free window is real but small — it is the time between `shutdown()` unblocking `::accept`/`::read` and the detached thread finishing its cleanup (`::close(fd); server_fd_ = -1;` at `metrics.hpp:75-76`) and returning, during which it may still touch `this` or the captured `prov` after `MetricsExporter`/`main()` locals have started tearing down.
*   **Refactoring:**
    1. Remove `thread_.detach()` at `metrics.hpp:78`; keep `thread_` as a normal joinable member (already declared as `std::thread thread_` — no type change needed).
    2. In the destructor, after `shutdown()`, call `thread_.join()` so `MetricsExporter`'s destructor doesn't return until the socket thread has actually exited.
    3. No change needed to the `running_` flag or the `shutdown()` call — both are already correct.
*   **Implemented:** exactly as above — `thread_.detach()` removed, `thread_.join()` added after `shutdown()` in the destructor. Build + `ctest` (3/3) verified clean.

### 1.2 Configuration Hardcoding of Stratum Nonces — ✅ Done (2026-07-21)
*   **Bottleneck:** Nonce parameters (`nonce_offset = 39` and `nonce_size = 4`) are hardcoded in **two** places: the Stratum V1 `mining.notify` handler ([stratum_client.cpp:472-473](file:///home/mechres/Projeler/aarch64-randomx/src/stratum_client.cpp#L472-L473)) and the CryptoNote `process_cryptonote_job` handler ([stratum_client.cpp:644-645](file:///home/mechres/Projeler/aarch64-randomx/src/stratum_client.cpp#L644-L645)). While correct for Monero, this hardcoding limits modularity and compatibility with custom stratum configurations or alternative RandomX-based blockchains.
*   **Refactoring:** Abstract nonce parameters.
    1. Define a `NonceMetadata` struct in `mining_common.hpp` containing `offset` and `size` fields.
    2. Propagate these configuration fields from `PoolConfig` down through `StratumClient` and into the generated `Job` instances, updating **both** call sites above so they stay in sync.
*   **Implemented (deviated from the letter of the plan, not the intent):** `Job` already carried `nonce_offset`/`nonce_size` fields, so a separate `NonceMetadata` struct would have been a redundant wrapper around two `std::size_t`s. Instead, added `nonce_offset_`/`nonce_size_` members directly to `StratumClient` (Monero defaults 39/4) plus a `set_nonce_config()` setter; both call sites now read from that single source of truth. No `PoolConfig`/CLI plumbing was added since nothing downstream needs to override it yet — can be wired up later if an alternative chain actually needs it.

### 1.3 Monolithic Main Coordination — ✅ Done (2026-07-21)
*   **Bottleneck:** [main.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/main.cpp) spans 800+ lines, mixing CLI argument parsing, JSON configuration deserialization, signal handlers, interactive TUI hooks, metrics endpoints, and thread pooling setup.
*   **Refactoring:**
    1. Extract argument parsing into a dedicated `CommandLineParser` class.
    2. Move miner state coordination, signals, and worker-pool lifecycles into a single `MinerApp` runner module.
*   **Implemented:** `CommandLineParser` (`include/armrx/cli_parser.hpp`, `src/cli_parser.cpp`) parses argv into a `MinerOptions` struct after applying config-file defaults, in the same precedence order as before; handles `--help`/`--version`/invalid-arg by printing and returning a `ParsedArgs{should_exit, exit_code}`. `MinerApp` (`include/armrx/miner_app.hpp`, `src/miner_app.cpp`) owns signal handling and the four run modes (init-cache, JIT dump, local benchmark, pool mining) as private methods called from `run()`. `main.cpp` is now 10 lines: parse, construct `MinerApp`, run.
*   **Verified:** local build clean, `--help`/`--version`/invalid-arg output byte-identical to the pre-refactor binary (diffed directly), exit codes preserved for every early-return path (including the `--pool`/`--wallet` validation, which needed a fix mid-refactor — see below). 3/3 local ctest passing. On-device: clean build, `--version` confirms `AArch64 JIT: enabled`, full 6-test ctest run in progress.
*   **Caught during self-review:** the first draft of `run_pool_mining()` swallowed the wallet/pool-list validation's `return 64;` into a plain `return;`, which would have silently changed that failure's exit code from 64 to the fallthrough `cpu.aarch64 ? 0 : 2`. Fixed by moving the validation back into `run()` before calling `run_pool_mining()`, mirroring the existing fast-mode-memory-check pattern in the same function.

---

## 2. Performance & Resource Optimization

### 2.1 CPU Core Reuse for Dataset Initialization — ✅ Done (2026-07-21)
*   **Bottleneck:** During seed change shifts, [mining_engine.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/mining_engine.cpp#L171-L185) creates a temporary vector of threads `init_threads` to initialize the 2080 MiB dataset in parallel, joins them, and discards them. Spawning new OS threads under CPU contention incurs significant scheduling latency and invalidates cache states.
*   **Optimization:** Reuse the existing long-lived mining worker threads. Integrate a synchronization barrier (using `std::barrier` or condition variables) inside `MiningEngine` to desynchronize mining loops during a job transition, partition the dataset ranges, and utilize the existing CPU-affinity-pinned threads to populate the dataset.
*   **Implemented:** used a manual mutex/counter/`condition_variable` handshake instead of `std::barrier` — `std::barrier` has no clean cancellation path, and `stop()` racing a live fast-mode seed rotation (SIGINT mid-rebuild) would otherwise leave `set_job()` waiting forever for workers that already exited via `while(running_)`. `set_job()`'s wait predicate includes `!running_`, and `stop()` notifies the CV after flipping `running_` false, so that race resolves cleanly (verified — see below). A generation counter (`dataset_init_generation_`, same idiom as the existing `job_generation_`/`local_gen`) drives worker participation instead of a boolean pending flag, so each worker participates exactly once per rebuild with no reset-window race. Falls back to the original temp-thread behavior when no persistent workers exist yet (the first job, set before `start()`).
*   **Bug found and fixed during implementation — not a regression, but a pre-existing correctness bug surfaced by this work:** writing the correctness test for this feature uncovered that `MiningEngine`'s multi-threaded dataset build (both this new path and the original temp-thread code it's alongside) has been calling `initialize_dataset()` with the wrong output span — passing the full dataset buffer instead of each thread's own sub-span — silently corrupting most of any fast-mode dataset built with more than one thread. See `docs/postmortems/fast-mode-dataset-corruption-postmortem.md` for the full writeup; this also surfaced that `assert()` was silently compiled out under the project's default Release build (`-DNDEBUG`), so the test suite hadn't actually been checking its assertions.
*   **Verified:** x86_64 (31 GiB RAM) — full local `ctest` 3/3, both new fast-mode tests fully exercised (not skipped). AArch64 on-device (Cortex-A53, ~1.8 GiB RAM, built with `-DARMRX_DISABLE_LTO=ON` — see the postmortem for why) — full `ctest` 6/6, with the two fast-mode tests correctly skipped via a memory-availability guard (the device can't fit fast mode at all, confirmed via the miner's own `--mode=fast` check).

### 2.2 Argon2d Cache SIMD Evaluation — ✅ Done, NEON enabled (2026-07-21)
*   **Bottleneck:** The NEON implementation of the Argon2 `gb` permutation function in [argon2.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/argon2.cpp#L199-L200) is currently disabled using `#if 0`.
*   **Optimization:** Conduct a comparative micro-benchmark on the Cortex-A53 to measure if vector load/store instructions (`vld1q_u64`/`vst1q_u64`) suffer from memory gather penalties during diagonal permutations. If a net performance gain is verified, permanently enable `permute_block_neon`; otherwise, clean the codebase by removing the dead code.
*   **Implemented:** added a permanent `argon2_compress` micro-benchmark to `tests/bench_armrx.cpp` (§"3b. Argon2 compression") since the suite had no benchmark for the cache-init hot path at all. Measured on-device (Cortex-A53, pinned to core 0), reproduced twice: **scalar 11.87 μs/compress (84,218 compress/s) vs NEON 9.95 μs/compress (100,533 compress/s) — a consistent ~16% latency reduction, ~19% throughput gain.** No gather penalty observed on the diagonal step (which already falls back to scalar `gb()` there — see the code comment on why). Flipped the guard from `#if 0 // defined(__aarch64__) && defined(__ARM_NEON)` to the real `#if defined(__aarch64__) && defined(__ARM_NEON)`, permanently enabling `permute_block_neon` on AArch64+NEON builds; x86_64 and non-NEON ARM builds still take the scalar path via the existing `#else` branch.
*   **Verified:** correctness is unusually well-covered here — `Argon2dCache::initialize()` (which calls `permute_block` ~786k times for the default 262144-block/3-pass config) directly determines every RandomX hash output, so the existing KAT suite (`armrx_tests`) is itself the regression test for this change, not just a smoke test. Passed 6/6 on-device ctest (`armrx_tests`, `test_mining`, `bench_armrx`, `bench_opcodes`, `test_jit_encodings`, `test_jit_determinism`) with NEON enabled.

### 2.3 JIT Memory Page Recycling — Investigated, Not an Issue
*   **Original claim:** `allocMemoryPages` is executed on every JIT compilation run, requesting virtual memory allocations from the OS kernel.
*   **Finding (verified against code):** this premise is false. `allocMemoryPages` is called exactly once, in the `JitCompilerA64` constructor (`jit_compiler_a64.cpp:133`). `JitCompilerA64` is itself constructed exactly once per `VirtualMachine` (`vm.cpp:158`), and `VirtualMachine` is constructed exactly once per worker thread for the lifetime of that thread (`mining_engine.cpp:283` — `VirtualMachine vm(flags);` sits above the mining loop, not inside it). Each subsequent `generateProgram`/`generateProgramLight` call (once per RandomX program, 8 per hash) only resets `codePos` and rewrites the same pre-allocated `code` buffer — no new page allocation, no `mprotect` toggling, per compile. There is nothing to recycle; the buffer is already reused for the process/thread lifetime.
*   **Action:** no code change needed. Left in the plan (rather than deleted) so a future pass doesn't re-investigate the same non-issue — see the audit note at the top of this file about stale claims propagating across docs.

---

## 3. Code Quality, Testing & Security

### 3.1 Hardened Network Parsing & Fuzzing — ✅ Fuzz target done (2026-07-22); SAX rewrite not pursued
*   **Vulnerability:** The stratum JSON parser in `armrx::json` relies on custom string search procedures (`find`, `get_string`). A compromised mining pool could exploit this by sending nested arrays, malformed unicode control characters, or oversized string payloads to crash the worker thread or overflow stack variables.
*   **Mitigation:**
    1. Establish a fuzzing target using LibFuzzer to stream mutated payloads to `StratumClient::handle_line`.
    2. Re-engineer the JSON module to use a hardened, non-allocating SAX parser with strict input length checks.
*   **Implemented (item 1, deviated from the letter per an explicit scope decision):** `tests/fuzz_json.cpp` fuzzes `armrx::json`'s full public API surface (`get_string`/`get_raw`/`get_array_first`/`get_str_array`/`get_object`/`get_array_element`/`escape`) directly, rather than through the private `StratumClient::handle_line` — the mock Stratum tests (§3.2) already exercise `handle_line` with valid protocol messages, so this harness's scope is specifically "hostile bytes crash the parser module," matching the actual threat (a compromised pool's replies flow through `armrx::json` regardless of which caller invokes it). Gated behind a new `ARMRX_BUILD_FUZZERS` CMake option (default `OFF`, Clang-only — `-fsanitize=fuzzer` isn't supported by GCC, which the rest of this project builds with; a clear `FATAL_ERROR` fires at configure time if enabled with the wrong compiler). Compiles `src/json.cpp` directly into the fuzz binary rather than linking `armrx_core`, sidestepping any GCC/Clang object-compatibility question entirely, since that module has no other dependencies.
*   **Item 2 (SAX rewrite) not pursued:** over 2.5M fuzz executions (two runs, 61s + 121s) found zero crashes and zero ASAN findings against the current hand-rolled parser. Given that result, rewriting the parser is not justified right now — revisit only if the fuzzer (run for longer, or with a larger corpus) ever finds something.
*   **Verified:** confirmed the `ARMRX_BUILD_FUZZERS=ON` + default (GCC) compiler combination fails cleanly at configure time with a clear message. Built and ran with `-DCMAKE_CXX_COMPILER=clang++`: two clean fuzzing passes (942K + 1.57M executions), zero findings. Confirmed the default `ARMRX_BUILD_FUZZERS=OFF` GCC build path is entirely unaffected — local `ctest` 4/4 unchanged.

### 3.2 Automated Stratum Protocol Testing — ✅ Done (2026-07-22)
*   **Coverage Gap:** The test suite has no coverage for networking protocols, socket reconnect loops, or pool failovers.
*   **Strategy:** Build a local test harness `test_pool_protocol.cpp` using a mock TCP socket. It must simulate:
    *   Stratum V1 / CryptoNote subscribe handshakes.
    *   Simulated connection drops to verify that `StratumClient` executes exponential backoff.
    *   Pool timeout failures to verify that `PoolManager` seamlessly migrates workers to the next configured fallback pool.
*   **Implemented:** `tests/test_pool_protocol.cpp` — a loopback POSIX-socket mock server scripts 5 scenarios: Stratum V1 full flow (reached via AUTO's real CryptoNote-first-then-fallback negotiation), CryptoNote full flow, reconnect-backoff exhaustion (via `StratumClient::set_reconnect_config()`, fast/deterministic timing), multi-pool failover (real `PoolManager` production backoff timing, ~31s), and malformed-input robustness.
*   **Critical bug found and fixed while building this exact test:** `PoolManager::tick()` self-deadlocked (locked `stratum_mutex_` for its whole body, then called `connect_to_current()` — which locks the same non-recursive mutex again — from inside that scope) the first time real multi-pool failover actually completed its cooldown and tried to reconnect. This is a documented core feature (README's "automatic failover after 5 retries with a 2s cooldown") that would have permanently frozen the miner's pool-management loop on first use in any real multi-`--pool=` deployment where the first pool went down. See `docs/postmortems/pool-failover-deadlock-postmortem.md` for the full account, including a second (non-blocking, documented-not-fixed) finding about stale-reconnect-thread join latency during failover, and a documented (also not fixed) gap where a pool unreachable from process startup never triggers failover at all.
*   **Verified:** x86_64 local `ctest` 4/4. AArch64 on-device full `ctest` 7/7 (`test_pool_protocol` 64.6s) — the on-device run's tighter timing (slower CPU, more scheduling jitter) initially surfaced the stale-join latency finding via a real, informative test failure (not a hang), accommodated by widening the test's own timeout budget before this final confirming run.

### 3.3 Windows Code Path — Dead Weight, Not a Live Vulnerability — ✅ Annotated (2026-07-21)
*   **Original claim:** In [virtual_memory.c](file:///home/mechres/Projeler/aarch64-randomx/src/virtual_memory.c#L212), Windows builds query `SeLockMemoryPrivilege`, and denial should degrade gracefully instead of failing.
*   **Finding (verified against code):** the graceful-degradation half already exists at the *call site* — `MappedMemory` (`mining_engine.hpp:27-43`) and `vm.cpp:136` already treat a `NULL` return from `allocLargePagesMemory` as "no huge pages" and fall back to plain `mmap` + `MADV_HUGEPAGE`. More importantly, `_WIN32`/`_MSC_VER`/`__CYGWIN__` appear **nowhere else** in the codebase — no CMake Windows target, no MSVC toolchain support, and the fallback path in `MappedMemory` itself calls POSIX-only `::mmap`/`::madvise` unconditionally, which wouldn't compile on Windows regardless. The Windows branch in `virtual_memory.c` is inherited verbatim from upstream RandomX and is unreachable dead code in this project, which is Linux-only by design (per `README.md`).
*   **Action:** downgrade from "security fix" to a cleanup task. Either delete the `_WIN32`/`__CYGWIN__` branches from `virtual_memory.c` entirely (consistent with the project's zero-dead-code discipline elsewhere), or leave a one-line comment noting they're vestigial upstream code, never compiled. Do not spend effort hardening a path that can't build.
*   **Implemented:** chose the annotate-not-delete option — the Windows branches are interleaved with the Apple/BSD paths this project *does* build in the same functions, so surgical deletion across ~8 sites risked collateral damage in a memory-protection-critical file for a Phase-1 "quick win." Added a header comment in `virtual_memory.c` explaining the branches are dead in this Linux-only project. Revisit full deletion later if the file gets touched for other reasons.

---

## 4. Developer Experience (DX) & CI/CD Pipeline

### 4.1 Cross-Compile Containerization — open, backlog (not a Phase 1-5 item, tracked for completeness)
*   **Bottleneck:** Local development relies on manual host synchronization (`devbox_sync`) to a remote target, complicating CI automation.
*   **Strategy:** Build a Dockerfile wrapping a QEMU AArch64 environment with a postmarketOS toolchain. Set up a GitHub Actions workflow that executes this container, compiling the source and running the CTest suite on virtualized AArch64 runners.
*   **Status:** deprioritized to backlog per explicit direction (see Phase 3 note below) — not started. Still tracked in `PLAN.md`'s live backlog section and `NEXT_STEPS.md`.

### 4.2 Unified Compilation Flag Invariants
*   **Original bottleneck:** Flag scopes (`ARMRX_ENABLE_JIT_FAST_DIV_SQRT`, `ARMRX_FAST_MATH`, `ARMRX_JIT_PROFILE`) are configured across different files, risking compilation conflicts (e.g. public definition leakage clobbering LTO/PGO optimizations).
*   **Update (verified against code):** the specific crash this was chasing is already fixed. `ARMRX_JIT_FAST_DIV_SQRT` was `PUBLIC` (propagating into unrelated translation units like `superscalar.cpp`/`dataset.cpp` and corrupting `x29` under GCC 15 + LTO), but commit `b814c17e` (2026-07-21) changed it to `PRIVATE` (`CMakeLists.txt:135`) — root-caused and written up in `docs/audits/jit-buffer-size-audit.md` §3. `ARMRX_JIT_FAST_DIV_SQRT` was the only flag actually implicated in a crash; `ARMRX_HAVE_JIT`/`ARMRX_HAVE_TLS`/`ARMRX_HAVE_HWLOC` are `PUBLIC` deliberately (consumers like `main.cpp` and the tests need to see them via `#ifdef`), and `ARMRX_JIT_PROFILE` (still `PUBLIC`, `CMakeLists.txt:130`) has no reported issue.
*   **Strategy (downgraded from "safety fix" to DX nice-to-have):** if flag sprawl in `CMakeLists.txt` becomes hard to track, move compile definition bindings to a centralized `cmake/CompilerFlags.cmake` file with a comment on each definition explaining why its scope (`PUBLIC` vs `PRIVATE`) was chosen. No longer urgent — do this opportunistically, not as a Phase 1 item. **Status: not started, low priority, still in backlog.**

---

## 5. Future-Proof Roadmap (Phases 1–5, all completed)

```
Phase 1 (Short-term) ──────────────► Phase 2 (Medium-term) ─────────────► Phase 3 (superseded — see below)
✅ Fix Exporter join-on-destroy      ✅ main.cpp → CommandLineParser +   C. Perf re-baseline + branch-miss
✅ Abstract Stratum nonce metadata      MinerApp split                     re-measurement (not started)
✅ Annotate dead Windows alloc path  ✅ Reuse workers for dataset        D. AES key / scratchpad-mask
   (all three done 2026-07-21)      ✅ Mock stratum socket tests            constant consolidation (not
                                     ✅ Fuzz stratum JSON parser            started)
                                     ✅ Resolve Argon2 NEON permute       (QEMU CI / Stratum V2 deferred
   (all five done 2026-07-22 —         to backlog, deprioritized)
    2 critical prod bugs found
    & fixed along the way)
```

### Phase 1: Short-term / Immediate (Correctness — quick, low-risk fixes) — ✅ Done (2026-07-21)
*   **Tasks:**
    1. ✅ `MetricsExporter` (§1.1): removed `thread_.detach()`, joined the thread in the destructor after the existing `shutdown()` call.
    2. ✅ Abstracted Stratum nonce metadata (§1.2) out of the two hardcoded call sites into `StratumClient` member state + `set_nonce_config()`.
    3. ✅ Annotated the unreachable `_WIN32`/`__CYGWIN__` branch in `virtual_memory.c` (§3.3) as dead code (see §3.3 for why annotation was chosen over deletion).
*   **Verified:** `cmake --build build -j` clean; `ctest --test-dir build --output-on-failure` 3/3 passing (`armrx_tests`, `test_mining`, `bench_armrx`) on the x86_64 dev sandbox (interpreted-only, JIT excluded at build time).
*   **Verified on real AArch64 hardware (2026-07-21):** the devbox MCP tools weren't wired into this session, so validated over direct SSH instead (same steps `devbox_sync`/`devbox_build`/`devbox_test` would run) — `rsync` to the device, native `cmake --build`, then `ctest`. All 6/6 tests passed, including the JIT-only `bench_opcodes`, `test_jit_encodings`, and `test_jit_determinism` that don't even compile on x86_64: `armrx_tests` 25.5s, `test_mining` 10.8s, `bench_armrx` 296.0s, `bench_opcodes` 215.7s, `test_jit_encodings` 53.9s, `test_jit_determinism` 11.0s.
*   **Rationale:** All three were small, mechanical, and didn't require design decisions — good first tasks with no open questions.

### Phase 2: Medium-term (Structural Refactors & Test Coverage) — ✅ Done (2026-07-22)
*   **Tasks:**
    1. ✅ Split `main.cpp` (§1.3) into a `CommandLineParser` and a `MinerApp` runner.
    2. ✅ Re-engineer `MiningEngine` to reuse existing worker threads for dataset initialization via a mutex/counter/condition_variable handshake (§2.1) — also surfaced and fixed a pre-existing critical dataset-corruption bug along the way, see `docs/postmortems/fast-mode-dataset-corruption-postmortem.md`.
    3. ✅ Write a mock TCP server to test Stratum V1 / CryptoNote pool handshakes, timeouts, and failovers under CTest (§3.2) — also surfaced and fixed a critical `PoolManager` self-deadlock, see `docs/postmortems/pool-failover-deadlock-postmortem.md`.
    4. ✅ Write fuzzing targets to validate `armrx::json` against malformed payloads (§3.1) — 2.5M+ executions, zero findings.
    5. ✅ Benchmark NEON Argon2 `permute_block_neon` on the Cortex-A53 and either enable it or delete the dead `#if 0` block (§2.2) — benchmarked, NEON wins, enabled.
*   **Expected Outcomes:** Smaller, testable `main.cpp`. Elimination of thread-spawning latency during seed key changes. Hardened network parsing. Automated validation of pool failover states.
*   **Rationale:** These require real design/measurement work (barrier synchronization, mock socket harness, a genuine A/B benchmark) rather than mechanical fixes, so they follow the Phase 1 quick wins.
*   **Unplanned but consequential:** two of Phase 2's own correctness tests each caught a critical, pre-existing production bug unrelated to the feature/coverage they were written for — §2.1's dataset-reinit test found the fast-mode dataset corruption bug (`docs/postmortems/fast-mode-dataset-corruption-postmortem.md`), and §3.2's mock failover scenario found the `PoolManager::tick()` self-deadlock (`docs/postmortems/pool-failover-deadlock-postmortem.md`). Also surfaced, along the way: `assert()` was silently compiled out project-wide under the default Release build (`-DNDEBUG`) until fixed with `-UNDEBUG` on the affected test targets; the on-device Cortex-A53 devbox (~1.8 GiB RAM) cannot fit RandomX fast mode at all, so any future fast-mode test/benchmark work needs the same memory-availability guard used in `tests/test_mining.cpp` (`fast_mode_fits_on_this_host()`); and two documented-but-not-fixed pool-failover gaps (a pool dead from process startup never triggers failover; failover can be delayed up to ~30s more by a stale reconnect thread's blocking join) — see the failover postmortem for both.

### Phase 3 — superseded (2026-07-22): CI/CD and Stratum V2 deprioritized, replaced with performance re-baseline + targeted cleanup

The original Phase 3 (QEMU AArch64 CI, Stratum V2, generic "JIT tuning") is not a current priority. Replaced with a narrower, code-verified set of next steps — re-checked against current HEAD rather than restated from the (partly stale) `docs/audits/audit-20260721-cross-reference.md`, several of whose findings turned out to already be resolved: the `generateProgram`/`generateProgramLight` duplication the audit flagged is gone (`emitPrologueMix`/`emitSpMix2` are now shared, `jit_compiler_a64.cpp:192-228`), the dangerous `getCode()` raw-executable-pointer accessor is deleted (only `getCodeSize()` remains), and `MetricsExporter`'s raw `std::cerr` usage is already routed through `ARMRX_LOG_*`.

**C. Fresh performance re-baseline + branch-miss re-measurement — ✅ Data gathered (2026-07-22); CSEL/peephole JIT work NOT started, awaiting explicit go-ahead.** Every existing perf number in `NEXT_STEPS.md`/`ROADMAP.md`/`STATUS_REPORT.md` predated this session's changes (Argon2 NEON, worker-thread dataset reuse, the fast-mode dataset-corruption fix, the pool-failover deadlock fix). Ran `perf stat -e instructions,cycles,branches,branch-misses ./build/bench_armrx` on-device (Cortex-A53).
*   **Result:** branch-miss rate **31.08%** (169.0B instructions, 224.5B cycles, IPC 0.7528, 7.80B branches, 2.43B branch-misses) — essentially unchanged from the pre-PGO baseline in `NEXT_STEPS.md` (31.6%) despite everything landed since (PGO, O12/O13 JIT scheduling, the AES fix, Argon2 NEON, worker-thread dataset reuse, two critical bug fixes). Estimated cost at Cortex-A53's typical 8–11 cycle misprediction penalty: **~8.6–11.9% of total cycles** lost purely to misprediction, real and apparently untouched by any optimization work to date. >98% of measured wall-time is the actual light-mode JIT hash pipeline (micro-benchmarks are under 1.4% of total time), so this number is representative of real mining behavior, not diluted by unrelated code.
*   **On the decision gate:** the old audit's "94.85% of misses are in Superscalar dataset generation, not per-hash" claim doesn't straightforwardly transfer to this hardware — this devbox can only run light mode (see §2.1's finding that it can't fit fast mode's ~2.3 GiB requirement), and in light mode `execute_superscalar` runs *inside* the per-hash chain via on-demand `generate_dataset_item()`, not as a separable one-time per-job cost the way it is in fast mode. So the fresh data does not rule out CBRANCH work the way the old claim would have.
*   **Recommendation (not acted on — awaiting sign-off):** the numbers don't close the door on CBRANCH/peephole work, but `docs/experiments/branchless-cbranch.md`'s own prior attempt at a CSEL-based fix was inconclusive, and this is security-sensitive JIT hot-path surgery (wrong CBRANCH semantics → wrong hash, the same bug class as this session's two critical fixes) for an estimated 5–15% gain per the audit's own uncertain estimate. Given that risk/reward and coming right after two severe production bugs found this session, this should not be started without an explicit, separate go-ahead.
*   **Go-ahead given (2026-07-22); "profile first" prerequisite finally executed.** Ran `perf record -e branch-misses` (not just `perf stat`) with symbol attribution on-device — the exact step every prior session recommended but none had done. Confirmed via cross-check with `strace -f -c` (53 syscalls, 6.6ms total — ruling out a large `[k]`-tagged bucket as real kernel work; it's PMU sampling skid on this SPE-less Cortex-A53, a measurement artifact). Within the trustworthy ~54% of resolvable userspace samples: CBRANCH is confirmed the JIT compiler's *only* emitted data-dependent conditional branch (grepped every `0x54xxxxxx` B.cond site in `jit_compiler_a64.cpp`), so the `[JIT]` 10.54% share is cleanly attributable to it — real signal, not diluted by another opcode. Also confirmed Superscalar is *not* dominant here (`generate_superscalar` 0.56%, `execute_superscalar` not in the top ~30 symbols) — refuting the old audit's "94.85% in Superscalar" claim for this hardware/mode. Notably, `Argon2dCache::initialize` + its NEON permute helpers (18.62% combined) are actually a *larger* single contributor than CBRANCH — flagged for a future investigation but explicitly out of scope; this effort stays CBRANCH-only per direction given when scoping this work. Full methodology and numbers in `docs/experiments/branchless-cbranch.md`'s new "Precise attribution (2026-07-22)" section.
*   **Test-hardening done (2026-07-22), before touching `h_CBRANCH` itself.** `test_jit_encodings.cpp` previously only checked each CBRANCH's emitted size (≥4 bytes) despite its own file comment claiming to verify branch targets — it never actually did. Rewrote it to decode the real `bne`/`b` bytes (via a new, deliberately read-only `getCodeBytes()`/`getJitCodeBytes()` accessor — const-only, unlike the deleted mutable `getCode()`) and assert the computed branch target is real, backward, and in-bounds. Getting this right required discovering and working around a genuine subtlety: `randomx_calculate_hash()` runs several chained internal rounds reusing the same JIT buffer, so `JitDumpEntry`s from earlier rounds share offset numbers with — but point to memory since overwritten by — the final round; only the last contiguous run of entries is trustworthy. Also added `tests/test_jit_equivalence.cpp`: a JIT/interpreter equivalence sweep across 8 seeds × 2 inputs (vs. the existing KAT's 2 fixed inputs), bounded to ~226s via an explicit `TIMEOUT 600` (a prior CBRANCH bug caused a 120s hang — this test must fail fast, not hang, if a future change breaks it). Both new/rewritten tests pass cleanly against the current implementation. Verified: `ctest` 4/4 on x86_64 (JIT tests excluded there), 8/8 on-device. Full account in `docs/experiments/branchless-cbranch.md`'s "Unit test recommendation" section.
*   **CSEL implemented, measured, and reverted (2026-07-22) — a small net regression, not an improvement.** Implemented the CSEL-based rewrite `docs/experiments/branchless-cbranch.md` had sketched (compute both possible next-PC values, `csel` between them, single unconditional indirect `br` — no conditional branch at all). Caught and fixed one real bug in the process via the KAT test (the *reason* KATs must run before any benchmarking): the `csel`'s `Rn`/`Rm` register fields were transposed, silently inverting which address got selected. Once correct (all 8 tests pass, KATs byte-identical), a clean **apples-to-apples** `perf stat` comparison against the `bne`/`b` version — same tool, same `--full-hash-only` workload, old code rebuilt fresh in a separate directory for a fair baseline — showed CSEL is worse: +0.87% instructions, +0.32% cycles, **branch-misses up 46%** (14.1M → 20.7M, rate 2.4%→3.5%), hashrate flat (4.48→4.47 h/s, within noise). Consistent with the BTB-aliasing concern raised before starting: since the JIT buffer regenerates every hash, no encoding trick fixes the underlying predictor-history problem, and the extra indirect `br` just gives the predictor one more thing to mispredict. **Reverted to the `bne`/`b` version** (already correct and marginally better) — `git checkout` on `src/jit_compiler_a64.cpp` and `tests/test_jit_encodings.cpp` back to their `e563112` state, verified.
*   **Root cause of the original 31.08% figure found (2026-07-22) — it does not represent the mining hot path.** Getting a clean CSEL baseline required isolating `bench_armrx`'s `--full-hash-only` section, which showed a branch-miss rate of only **2.4%** — nothing like 31%. Reconciled this by running `perf stat` on all three of `bench_armrx`'s sections separately and summing: the reconstructed totals (169.35B instructions, 224.68B cycles, 7.80B branches, 2.43B misses, 31.12% rate) reproduce the historical 31.08% baseline almost exactly, confirming the math is sound. The breakdown: `--full-hash-only` (the actual mining hot path) contributes **0.58%** of all branch misses; `--micro-only` contributes 4.49%; **`--attribution-only` contributes 94.93%** — and that section includes a 30-sample *interpreted-mode* comparison run ("JIT speedup: 11.37×") that never executes during real JIT mining on AArch64, plus its own internal phase-timing sub-benchmarks. 94.93% is nearly identical to the old audit's much-cited "94.85% in Superscalar" claim — strong evidence that claim measured this same real phenomenon but misattributed it to Superscalar dataset generation rather than the actual cause (the attribution benchmark's non-representative interpreted-mode comparison).
    **Real-world impact recalculated**: 14.1M mispredictions on the actual hot path, at an 8–11 cycle penalty, costs only **~0.11–0.16% of total cycles** — not the "~8.6–11.9%" previously estimated by applying the diluted 31% rate uniformly to the whole workload. **CBRANCH misprediction was never a meaningful real-world performance lever** on this hardware. This closes the CBRANCH investigation: no further JIT branch-encoding work is justified by this data. If `bench_armrx`'s aggregate branch-miss number is used again for prioritization, use `--full-hash-only` in isolation, or clearly caveat that the default (no-flag) run's PMU counters are dominated by non-representative benchmark code, not the mining hot path.
*   **Argon2 diagonal-step vectorization — ✅ implemented and kept (2026-07-23), a real win.** The CBRANCH investigation flagged `Argon2dCache::initialize` + its NEON permute helpers (18.62% combined) as a comparably-sized, never-investigated contributor. Followed the same profile-first discipline: multi-event `perf stat` (IPC 0.65, branch-miss rate 2.0%, cache-miss rate 0.3% — ruling out both misprediction and memory-boundedness, contrary to the reasonable prior that Argon2's memory-hardness design would make it memory-bound) then `perf record -e cycles` (212K samples) attributing **38.09% of all cycles to the scalar `gb()` function alone**. Root cause: `permute_16_neon()`'s 4 "diagonal" mixing rounds fall back to sequential scalar `gb()` calls (long dependency chains, zero ILP) while the 4 "column" rounds already get 2x NEON parallelism via `gb_neon()` — because the diagonal register-pairs aren't memory-adjacent, so the naive `vld1q_u64` load doesn't work for them. Fixed by gathering the one non-adjacent operand pair per group via `vcombine_u64(vld1_u64(...), vld1_u64(...))` and reusing the *existing* `gb_neon()` unchanged — the same gather/scatter idea `permute_block_neon()`'s outer loop already uses for non-adjacent columns, applied one level deeper. Verified: KAT hashes and reference dataset-item values byte-identical, `ctest` 8/8 on-device + 4/4 on x86_64 (scalar path untouched). Apples-to-apples `perf stat` (old code rebuilt fresh, both runs back-to-back to minimize this device's real thermal/frequency variance between runs — wall-clock alone isn't trustworthy here, instruction/cycle counts are): **26.8% fewer instructions, 19.0% fewer cycles** (11,364 → 9,204 cycles per `argon2_compress` call). Scope honestly stated: this speeds up seed-key-rotation *latency* (cache init runs once per ~2048 blocks), not sustained steady-state hashrate — not the same claim as a hashrate win. Full account in `docs/experiments/argon2-neon-diagonal-vectorization.md`.
    **`memcpy` lead tried, measured, reverted (2026-07-23) — no net win.** The same 212K-sample `perf record` pass that found the diagonal-step fix also attributed 5.54% of cycles to `memcpy` (`argon2_compress`'s `auto permuted = result;` 1024-byte copy, needed because `permute_block` mutates in place). Gave `permute_block` an out-of-place `permute_block_into(src, dst)` sibling to eliminate it. Apples-to-apples `perf stat` (old code rebuilt fresh, on-device): -2.86% instructions but **+0.35% cycles** (flat-to-worse) — symbol attribution showed the `memcpy` cost didn't disappear, it relocated into the new function (6.77%→3.56% `memcpy`, but a new 12.74% `permute_block_into_neon`), netting out roughly even; glibc's `memcpy` was already about as fast as the hand-rolled replacement on this hardware. Reverted. Full account in `docs/experiments/argon2-compress-copy-elimination.md`.

    **`Argon2dCache::initialize`'s own driver-code cycle share — investigated and closed (2026-07-23), no action needed.** `perf annotate` (debug-symbol rebuild, instruction-level attribution) found this isn't separate driver overhead: 92% of sampled instructions in the function cost ≈0%, including the actual address/reference-computation arithmetic. Every hot instruction is a NEON `eor`/`ldr q`/`str q` — `argon2_compress()`'s own XOR-combine loops, auto-vectorized and inlined directly into `initialize`'s body by the compiler. Inherent, spec-required work, already well-optimized. **This closes the Argon2 performance backlog** — no further actionable lead from the original profiling pass. Full account in `docs/experiments/argon2-compress-copy-elimination.md`.
*   **Side finding while doing this work — real build regression, since root-caused and fixed:** the *standard documented* build command failed on-device — `armrx` hit a GCC15+musl+LTO `vsnprintf`/`always_inline` link error. The prior handoff attributed this to the §1.3 `main.cpp` split changing LTO partitioning; that theory was **disproved** by bisection — building the pre-split commit (`d7ca542`) reproduces the identical failure, and forcing `-flto-partition=one` (single WHOPR partition) does *not* fix it either, ruling out partitioning as the mechanism. Actual root cause: Alpine's `fortify-headers` package wraps libc calls (e.g. `vsnprintf`, reached via `std::to_string(double)` → libstdc++'s `__to_xstring`) in `extern`+`always_inline` functions that are fundamentally incompatible with GCC LTO — confirmed pre-existing since the on-device `gcc 15.2.0-r6 → r8` package upgrade on 2026-07-13 (well before this session), just never hit by a from-scratch LTO build of the `armrx` target specifically until now. Fix: scope `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` to just the `armrx` target when LTO is enabled (`CMakeLists.txt`), rather than disabling LTO project-wide. Verified: default build command (LTO on, no `-DARMRX_DISABLE_LTO=ON`) now links and passes `ctest` 7/7 on-device. `README.md`/`REASONIX.md` workaround notes removed as no longer needed.

**D. Both documented pool-failover gaps (§3.2's postmortem) — ✅ Fixed (2026-07-22).** The AUTO-fallback gap (a pool dead from process startup never triggered failover) is fixed via a new `StratumClient::reconnect_loop_active()` flag that lets `PoolManager::tick()` distinguish "no reconnect loop has ever run" from "one is running but hasn't incremented its counter yet" — the naive `reconnect_attempts()==0` check can't tell these apart, and an early draft of the fix that used it directly raced ahead of the real exponential backoff (caught because `test_pool_failover` finished in under a second instead of ~31s). The stale-reconnect-thread-join latency (up to ~30s extra delay before failover) is fixed by switching `reconnect_loop()`'s sleep to an interruptible `condition_variable::wait_for()`. Two new regression tests added to `tests/test_pool_protocol.cpp` (7 scenarios total). Verified: `ctest` 4/4 on x86_64, 7/7 on-device, `test_pool_failover` confirmed still exercising the real ~31s backoff (not short-circuited). See `docs/postmortems/pool-failover-deadlock-postmortem.md` for the full account.

**E. Small, low-risk maintainability fixes, same bug class as the two critical fixes found this session (duplicated logic silently drifting apart):**
1. ✅ **Consolidate AES round-key constants (2026-07-22).** `aes_generator.cpp`'s `key0..key3`/`key4r0..key4r7` and `aes_hash.cpp`'s `key1r_0..key1r_3`/`key4r_0..key4r_7` were confirmed byte-for-byte identical (verified numerically before touching code) — the same 12 RandomX-spec round-key blocks, just encoded as raw byte arrays in one file and as `build_aes_block()` from big-endian words in the other. Extracted to `include/armrx/aes_keys.hpp` (`kAesGen1RKey0..3`, `kAesGen4RKey0..7`); both source files now reference the shared constants. `aes_hash.cpp`'s `hash_state_*`/`hash_xkey_*` (unique to that file, not duplicated) reuse the header's `build_aes_key()` helper but stay local. Verified: KAT hashes byte-identical before/after on both x86_64 (interpreter) and on-device (JIT) — `ctest` 4/4 x86_64, 7/7 on-device.
2. ✅ **Unify scratchpad L3 mask constants (2026-07-22).** `vm.cpp`'s `kScratchpadL1Mask`/`kScratchpadL2Mask`/`kScratchpadL3Mask`/`kScratchpadL3Mask64` and `jit_compiler_a64.cpp`'s hardcoded `ScratchpadL3Mask` literal (with a comment repeating the derivation formula in prose) are now all derived from a single `armrx::scratchpad_mask()` constexpr helper in `include/armrx/randomx_config.hpp`, computed from `kRandomXScratchpadL1Bytes`/`L2Bytes`/`kRandomXScratchpadBytes` — confirmed numerically to reproduce the exact prior literal values before landing. Also collapsed `jit_compiler_a64.cpp`'s three independent `Log2(RANDOMX_SCRATCHPAD_L3)` call-site re-derivations into one named `ScratchpadL3Log2` constant (compile-time value unchanged, purely a single-source-of-truth cleanup). Verified: `armrx_tests`/JIT hashes byte-identical, `test_jit_encodings`/`test_jit_determinism` both pass on-device (7/7 full suite) — these are exactly the tests that would catch a JIT byte-code regression from this change.
3. ✅ **Derive `kCompileHandlers[256]` from `instruction_weights.hpp` (2026-07-22).** Confirmed the hand-written array's opcode ranges matched `RANDOMX_FREQ_*`'s frequencies exactly, in the same order, before touching it. `jit_compiler_a64.cpp` already builds its own 256-entry opcode table this way (`INST_HANDLE`/`REPN`/`WT`, used at line ~1277) — `vm.cpp`'s interpreter table now uses the identical macro pattern instead of an independently hand-maintained 73-line literal list, so the JIT and interpreter's opcode-to-instruction-type maps (a correctness-critical invariant — both must dispatch every opcode identically) can no longer silently drift apart from a typo in either copy. Verified: KAT hashes byte-identical before/after on x86_64 (interpreter) and on-device (JIT); full `ctest` 4/4 x86_64, 7/7 on-device including `test_jit_encodings`/`test_jit_determinism`.

This completes all 3 items in §5 item E — the full constant-dedup list from the handoff is now done.

**Explicitly deferred (backlog only):** QEMU AArch64 GitHub Actions CI (§4.1), Stratum V2 protocol support, `ARMRX_JIT_FAST_DIV_SQRT` CMake flag centralization (§4.2, already low priority), `STATUS_REPORT.md` regeneration.

### Phase 4 — proposed (2026-07-22): fresh codebase inspection after Phase 2/3 closeout

With every item from the prior handoff resolved (LTO regression, both pool-failover gaps, all 3 constant-dedup items), this phase is a fresh, from-scratch inspection of the current codebase to find what's next — not a restatement of old, partly-stale audits. Findings below were each independently verified by reading the actual code (and, for the top item, tracing the exact failure path) before being written down, not just asserted.

**A. `MiningEngine::worker_loop()` permanently kills a worker thread on a bad nonce offset/size — ✅ fixed (2026-07-23).**
`src/mining_engine.cpp:418-423`: when `update_nonce_in_template()` fails (i.e. `local_job.nonce_offset + local_job.nonce_size > block_input.size()`), the handler logs an error, sets `active = false`, then calls `return;` — which exits `worker_loop()` entirely, ending that thread for the rest of the process's life. Every *other* bad-state path in the same function (unknown job, dataset-size mismatch at line 398-402) instead falls through to `if (!active) { sleep; continue; }`, which keeps the worker alive to pick up the next job. This one path is inconsistent with its own neighbors for no apparent reason.

Concretely reachable: a pool sending a job whose `block_template` is shorter than `nonce_offset + nonce_size` (a malformed/truncated blob, or a misconfigured `--nonce-offset`/`set_nonce_config()` for a non-Monero RandomX chain) permanently loses one worker thread, silently. `num_workers_`/thread count reporting stays the same, hashrate quietly drops, no crash, easy to miss in a long-running session — the exact "silent degradation, not a crash" shape that made both of this session's earlier critical bugs (fast-mode dataset corruption, PoolManager self-deadlock) take a while to notice in the wild. Repeated bad jobs (e.g. a badly-behaving pool) could bleed workers down to zero over time while the miner still reports `MINING`.
*   **Fix (landed):** `return;` changed to `active = false; continue;`, matching the fall-through pattern every neighboring bad-state path already used.
*   **Test (landed):** `test_worker_survives_bad_nonce_job()` in `tests/test_mining.cpp` — feeds a job with `nonce_offset + nonce_size > block_template.size()`, confirms both workers log the error, idle (`total_hashes() == 0`) rather than dying, then pick up a subsequent valid job and mine normally. Verified locally (x86_64 interpreter, full `test_mining` suite green) and on-device.

**B. `config.cpp`'s numeric config-file fields aren't exception-guarded — ✅ fixed (2026-07-23).**
`src/config.cpp:18` (`parse_pool_str`'s port), `:63` (`workers`), `:66` (`difficulty`), `:69` (`seconds`) all call `std::stoul`/`std::stoull` directly on raw JSON-extracted strings with no `try`/`catch`. `cli_parser.cpp` already wraps the *identical* conversions for the equivalent CLI flags (`--workers=`, `--difficulty=`, `--seconds=`) in `try { ... } catch (...) { clean error; exit 64; }` — the established, correct pattern for exactly this failure mode already exists in this codebase, just wasn't applied to the config-file path. `load_config_with_fallback()` runs *unconditionally* on every invocation (even with no `--config=` flag — it auto-probes `$ARMRX_CONFIG`, `~/.config/armrx/config.json`, `./armrx.conf`), so a stray malformed default config (e.g. `"workers": "auto"` typo, or a pool string like `"host:"` with an empty port) crashes the whole miner via an unhandled `std::invalid_argument`/`std::out_of_range` before it ever reaches CLI parsing or logs anything useful.
*   **Fix (landed):** each conversion in `load_config()`/`parse_pool_str()` wrapped in the same try/catch-and-warn pattern `cli_parser.cpp` uses — a malformed field logs a warning and falls back to `AppConfig`'s default instead of aborting the process.
*   **Test (landed):** new `tests/test_config.cpp` — `test_malformed_numeric_fields_dont_crash` (bad pool port + bad workers/difficulty/seconds all in one config, asserts defaults kick in and nothing throws), `test_valid_numeric_fields_still_parse` (confirms the guards didn't break normal parsing), `test_missing_file_returns_defaults`. Verified locally and on-device.

**C. `MetricsExporter::server_fd_` is a plain `int` shared across threads without synchronization — ✅ fixed (2026-07-23).**
`include/armrx/metrics.hpp`: the background thread writes `server_fd_` (line 46, on successful bind/listen; line 76, on loop exit), and the destructor (a different thread) both reads it (`if (server_fd_ >= 0) ::shutdown(...)`) and lets it go out of scope — all without a memory fence or atomic. This is a data race under the C++ memory model (undefined behavior, `-fsanitize=thread`-catchable) even though `running_` itself is correctly atomic. Narrow window in practice (matters most if the destructor runs very shortly after construction — e.g. `--metrics-port` combined with an immediate SIGINT, or a bind failure racing the destructor), and unlikely to have caused a real observed failure yet, but it's a one-line, zero-risk fix.
*   **Fix (landed):** `int server_fd_ = -1;` → `std::atomic<int> server_fd_{-1};`. No new test added — one-line, zero-risk change; `-fsanitize=thread` remains the tool to re-verify with if this area is ever revisited.

**D. Stale project docs actively mislead — regenerate or correct.**
`NEXT_STEPS.md` (dated 2026-07-21, HEAD `a3a7244`) still lists the `MetricsExporter` thread-detach race, worker-thread dataset reuse, mock Stratum tests, JSON fuzzing, and NEON benchmarking as open `[ ]` items — every one of them has been done since. `STATUS_REPORT.md` similarly lists "Stratum protocol: manual testing only" (obsolete since `test_pool_protocol.cpp`) and frames NEON AES reintroduction as pending work the codebase has since decided against (`docs/postmortems/aes-ttable-bug-postmortem.md`, reflected correctly in `CLAUDE.md`). Separately, **`CLAUDE.md`'s own "CTest path caveat"** (claims `bench_armrx`/`bench_opcodes`/`test_jit_encodings`/`test_jit_determinism` show `Not Run` under plain `ctest`, requiring direct execution from `build/`) **did not reproduce even once this session** — every on-device `ctest` run this session (multiple, across three separate fixes) showed all 7 tests passing cleanly via plain `ctest`, no path workaround needed. This matters concretely: this session's own LTO investigation was initially misdirected by trusting a stale handoff attribution instead of testing it directly (see the LTO postmortem's own "lesson learned" in `changelogs.md`) — stale docs are not just clutter, they cost real investigation time.
*   **Fix:** regenerate `NEXT_STEPS.md`/`STATUS_REPORT.md`/`ROADMAP.md` from current state (this has been on the backlog since Phase 2 without being done), and re-verify + correct or remove the `CLAUDE.md` CTest caveat.

**E. Test coverage gaps worth closing (lower priority — but #1 found a real bug anyway):**
1. ✅ **`cli_parser.cpp` unit tests — done (2026-07-23).** New `tests/test_cli_parser.cpp` (15 cases: defaults, every flag family, malformed-value exit codes, `--version`/`--help`, unknown-argument handling, config-file/CLI-override precedence — matching this session's pattern of new tests for under-covered code repeatedly finding real bugs). **Found one**: `--config=<path>` was scanned in the pre-loop to extract its value but never explicitly recognized in the main flag-parsing loop, so it always fell through to `"Unknown argument"` and made the process exit with code 64 — the documented `--config=` flag was completely broken (confirmed against the built `armrx` binary before fixing). Fixed with an explicit `continue` on `--config=` in the main loop (`src/cli_parser.cpp`). `miner_app.cpp` remains untested (thin orchestration layer, lower value, not attempted).
2. ✅ **`fill_aes_1r_x4`/`fill_aes_4r_x4`/`hash_aes_1r_x4`/`hash_and_fill_aes_1r_x4` direct tests — done (2026-07-23).** New `tests/test_aes_hash.cpp`: a golden-output pin for `fill_aes_1r_x4` (captured from the current, KAT-verified-correct implementation), determinism + output-prefix-consistency checks for both fill functions, an input-sensitivity check for `hash_aes_1r_x4`, and a decomposition-equivalence check for `hash_and_fill_aes_1r_x4` — asserting its combined hash+fill pass produces byte-identical results to calling `hash_aes_1r_x4()`/`fill_aes_1r_x4()` separately on the same inputs, which is the actual contract that fused function exists to provide.
3. `tls_client.cpp` and `tui.cpp` remain fully untested (need a mock TLS server / a terminal-output capture harness respectively) — flagged for completeness, not urgent; not attempted this round, larger lift than 1/2 above.

**F. Hardening posture decision, not a bug — default kept, now disclosed at startup (2026-07-23).**
`src/virtual_memory.c`'s `setPagesRWX()` maps the JIT code buffer read+write+execute in one call, and the JIT compiler tries this path first, silently succeeding whenever the kernel allows it (stock Linux, no PaX/grsecurity) — meaning W^X enforcement (`enableWriting()`/`enableExecution()` toggling separately) only actually applies when `RANDOMX_FORCE_SECURE` is defined at build time, which is not the default. This is a known, deliberate perf/hardening tradeoff (RWX avoids a syscall pair per JIT recompile), not an oversight. **Decision (explicit, user-directed): keep the RWX-by-default behavior unchanged for now** ("we may or may not change it later") **but stop it being silent.** `JitCompilerA64`'s constructor (`src/jit_compiler_a64.cpp`) now logs which mode is active exactly once per process (guarded by a static `std::atomic<bool>`, since one `JitCompilerA64` exists per worker thread and they'd all report the identical result): `"JIT code buffer: RWX (...)"` or `"JIT code buffer: W^X enforced (...)"`. Verified on-device: the line fires once regardless of worker count, and reports `RWX` on this device's stock Linux kernel as expected. No default behavior changed — this is disclosure only, revisit the actual default later if wanted.

**Not re-litigated:** the CBRANCH/JIT branch-misprediction work (Phase 3 item C) remains the single largest known performance lever (31.08% branch-miss rate, ~8.6–11.9% of cycles) but stays gated on an explicit go-ahead per that section's own risk framing — nothing new this inspection changes that calculus. (This framing was later superseded — see Phase 6 in `PLAN.md`: the 31.08% figure was root-caused as non-representative of the hot path, real hot-path cost is ~0.1-0.16% of cycles.)

### Phase 5 — proposed (2026-07-23): external audit leads, verified and adopted

`docs/audits/performance-improvement-audit.md` (written by another agent, untracked)
proposed several performance leads. Each claim was independently fact-checked
against the actual codebase/history before adoption — this is standard practice
for any externally-sourced recommendation, not just an audit: verify before
acting on it. One inaccuracy was found and fixed (see below); the two
substantive recommendations checked out as accurate and non-redundant with
prior work, so they're adopted here as tracked next steps (`NEXT_STEPS.md` §5a).

**Verification summary:**
- PGO plumbing/devbox-default claim (`CMakeLists.txt:26,118-128`,
  `tools/devbox/devbox_mcp.py:56`) — confirmed accurate. The +19.3%/+14.9%
  numbers match this file's own telemetry exactly. One nuance the audit
  understated: `devbox_build`'s `extra_flags` already lets a caller pass
  `-DARMRX_PGO=USE` per-invocation, so this is a missing *default/automation*,
  not a missing *capability*.
- NEON software-AES `vtbl` vectorization claim — confirmed **not** redundant
  with the two previously-reverted hardware-AES attempts (`AESE`/`AESD`, wrong
  round order; then `AESE`+`AESMC` re-enable, zero benefit, both
  `changelogs.md` 2026-07-20). Current `include/armrx/aes.hpp` is 100% scalar,
  no NEON at all — this is a genuinely untried technique, correctly flagged
  with the same "must be hashrate-vetoed on-device, not assumed" risk framing
  this session has used throughout.
- `--stagger-ms` default claim — the underlying memory-bus-contention data point
  (`changelogs.md` 2026-07-21 worker sweep, 25% per-worker efficiency drop
  under 8-worker contention) is real, but the audit's *proposed fix* (a
  nonzero startup stagger might help) had already been tried and found
  ineffective in an earlier session — see item 3's outcome below. A second
  instance of "verify before trusting a claim," this time catching a doc the
  audit itself apparently didn't check (`docs/archived/beyond-parity_v2.md`).
- **One inaccuracy found and fixed**: the audit attributed the Newton-Raphson
  "−1.1% hashrate" figure to `ROADMAP.md` "Features", but that entry actually
  said "Failed once (segfault). Do not retry..." — describing an *earlier*,
  separate `x29`-register-corruption bug that was since root-caused and fixed
  (`docs/audits/WX_Alignment_and_LITTLE_Core_Profiling.md` §5), after which
  Newton-Raphson was cleanly re-evaluated (100% correctness pass, −1.1%
  hashrate, kept off for perf not safety reasons — the real source is
  `OPTIMIZATION_REFERENCE.md`/`changelogs.md` 2026-07-21). Fixed the stale
  `ROADMAP.md` entry to reflect this (2026-07-23).

**Adopted as next steps** (see `NEXT_STEPS.md` §5a for the actionable form):
1. Wire PGO into the devbox release flow (generate→train→use), or at minimum
   document the two-stage build and correct `README.md`'s framing. Zero code
   risk.
2. Prototype a `vtbl`/`vqtbl1q`-vectorized software T-table AES path, KAT-gated,
   verified on the interpreted path before any JIT integration, hashrate-vetoed
   on-device before adoption — same discipline as every perf change this
   session (CBRANCH/CSEL, Argon2 diagonal-step, Argon2 copy-elimination).
3. (Lower priority) a quick on-device `--stagger-ms` default experiment.

**Item 1 outcome (2026-07-23): tool shipped, payoff claim did NOT reproduce.**
Added `devbox_pgo_build` (`tools/devbox/devbox_mcp.py`) — a genuine, mechanically
verified GENERATE→train→USE orchestration. While validating it, found and fixed
a real, pre-existing bug affecting the *entire* devbox toolchain: `shlex.quote()`
was being applied to paths built from `cfg.remote_dir`, which single-quotes the
string and defeats tilde expansion — since this project's actual config uses
`remote_dir: "~/armrx"`, every `devbox_build`/`test`/`bench` log silently landed
in a disconnected literal `~` directory, and `devbox_status`'s deployed-revision
check permanently read from that same wrong location (confirmed stale logs from
unrelated *prior* sessions sitting there — this had been silently broken for a
while, not something introduced by this PGO work). Fixed by interpolating those
paths unquoted, matching the convention `tool_build`'s own commands already
used correctly.

**However**, measured honestly and apples-to-apples (two training durations
tried, 15s and 90s, non-PGO and PGO builds both freshly built, same device,
back-to-back): both measured **identical 4.27 H/s** single-thread steady-state
— not the historically-claimed 5.18 H/s. `.gcda` profile data was confirmed
real and non-empty, `-fprofile-use` confirmed present in `armrx`'s actual link
command, KATs passed on every build. This isn't a broken PGO flow — it's a
genuinely-reproduced null result on the *current* codebase. Most likely
explanation: substantial hot-path code has changed since the 2026-07-21
measurement that produced +19.3% (Argon2 diagonal-step vectorization, the JIT
startup log line, several correctness fixes), shifting the code shape PGO's
compile-time decisions were tuned against. **The tool is kept** for future
re-evaluation, but the audit's "+19.3%, single biggest lever" framing should
not be repeated without re-measuring against whatever the codebase looks like
at the time.

**Item 2 outcome (2026-07-23): implemented, exhaustively verified correct,
measured as a real regression.** Derived a full "vector-permute AES" S-box
from scratch in Python before writing any C++ — GF(2⁸)↔tower-field GF(2⁴)²
isomorphism via a root of AES's defining polynomial, verified 256/256 against
the standard FIPS-197 S-box/inverse-S-box, full round structure verified
against 3000 random trials matching this codebase's actual T-table
semantics. Implemented as `encrypt_transform_neon`/`decrypt_transform_neon`
in `include/armrx/aes.hpp`, gated behind a new `ARMRX_ENABLE_NEON_AES` CMake
option (default OFF). New `tests/test_aes_neon.cpp` — 256/256 SubBytes/
InvSubBytes exact match, 20,000 random full-round parity trials — **compiled
and passed on the first attempt on real hardware, zero bugs found**. Full
KATs and `test_aes_hash.cpp`'s golden pins byte-identical with the flag on;
full `ctest` 12/12 green.

Measured honestly, apples-to-apples (twice, ruling out a thermal artifact):
a real **~19.4% regression** on `fill_aes_1r_x4`/`hash_aes_1r_x4`. Same root
cause as the earlier hardware-AES re-enable attempt — per-block NEON
load/store overhead cancels the lookup savings on this Cortex-A53,
independent of *which* NEON AES technique is tried. Kept (flag-gated, default
OFF) rather than reverted — the implementation, its test coverage, and the
derivation are reusable even though the performance didn't pan out here.
Full account in `docs/experiments/neon-vector-permute-aes.md`.

**Item 3 outcome (2026-07-23): already investigated in an earlier session,
found ineffective — not re-tested.** Before spending on-device time,
checked whether this exact experiment had already been run: it had.
`docs/archived/beyond-parity_v2.md` documents startup stagger tried at 5,
20, 100, and 1000ms on this same device — "+0% (tested, ineffective)... a
hardware ceiling." RandomX touches the full 2 MiB scratchpad on *every*
hash iteration, so a one-time launch-time delay can't desync steady-state
memory-bus contention the way the audit's suggestion assumed; the 8-worker
efficiency drop is single-channel LPDDR3 bandwidth saturation. Current
default (`stagger_ms_ = 0`) left as-is, no change made. This finding predates
this session's other work and was not re-verified against the current
codebase — noted as a caveat, not re-run, given how mechanism-clear and
hardware-fundamental the prior result is.

### Phase 3 (original, superseded — kept for reference)
*   **Tasks:**
    1. Deploy QEMU AArch64 container environments on GitHub Actions CI (§4.1).
    2. Implement native Stratum V2 protocol support to minimize data payload transfers.
    3. Opportunistically centralize CMake compile-definition scopes into `cmake/CompilerFlags.cmake` (§4.2) — downgraded from Phase 1 now that the one flag that actually caused a crash (`ARMRX_JIT_FAST_DIV_SQRT`) is already `PRIVATE`. Do this only if flag sprawl becomes a real maintenance problem.
    4. JIT/CBRANCH instruction-scheduling and cache-alignment tuning — tracked in more detail in `ROADMAP.md`'s Performance table (P3/P4); re-measure branch-miss attribution post-AES-fix before investing further here, per `docs/audits/audit-20260721-cross-reference.md` §3.1.
*   **Expected Outcomes:** Zero-manual-setup build automation. Stratum V2 network throughput gains for low-bandwidth pool setups.
*   **Rationale:** Scalable build infrastructure enables future community contributions; Stratum V2 and JIT tuning are valuable but not blocking anything else in this plan.
