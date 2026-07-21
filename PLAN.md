# armrx — Master Update and Improvement Plan

This document serves as the master plan and improvement roadmap for the `armrx` RandomX AArch64 miner. It details structural architecture upgrades, microarchitectural performance optimizations, quality assurance steps, developer experience (DX) refinements, and a phased execution schedule.

> **Verification pass (2026-07-21):** every item below was re-checked against current HEAD (`61899d7`) before this revision. Two items turned out to rest on stale premises and were corrected in place rather than silently dropped — see §2.3 and §4.2. See also `docs/audit-20260721-cross-reference.md` for a broader doc-vs-code cross-reference conducted the same day; check it before assuming any *other* project doc (`ROADMAP.md`, `STATUS_REPORT.md`, `NEXT_STEPS.md`) is current.

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

### 1.3 Monolithic Main Coordination
*   **Bottleneck:** [main.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/main.cpp) spans 800+ lines, mixing CLI argument parsing, JSON configuration deserialization, signal handlers, interactive TUI hooks, metrics endpoints, and thread pooling setup.
*   **Refactoring:**
    1. Extract argument parsing into a dedicated `CommandLineParser` class.
    2. Move miner state coordination, signals, and worker-pool lifecycles into a single `MinerApp` runner module.

---

## 2. Performance & Resource Optimization

### 2.1 CPU Core Reuse for Dataset Initialization
*   **Bottleneck:** During seed change shifts, [mining_engine.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/mining_engine.cpp#L171-L185) creates a temporary vector of threads `init_threads` to initialize the 2080 MiB dataset in parallel, joins them, and discards them. Spawning new OS threads under CPU contention incurs significant scheduling latency and invalidates cache states.
*   **Optimization:** Reuse the existing long-lived mining worker threads. Integrate a synchronization barrier (using `std::barrier` or condition variables) inside `MiningEngine` to desynchronize mining loops during a job transition, partition the dataset ranges, and utilize the existing CPU-affinity-pinned threads to populate the dataset.

### 2.2 Argon2d Cache SIMD Evaluation
*   **Bottleneck:** The NEON implementation of the Argon2 `gb` permutation function in [argon2.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/argon2.cpp#L199-L200) is currently disabled using `#if 0`.
*   **Optimization:** Conduct a comparative micro-benchmark on the Cortex-A53 to measure if vector load/store instructions (`vld1q_u64`/`vst1q_u64`) suffer from memory gather penalties during diagonal permutations. If a net performance gain is verified, permanently enable `permute_block_neon`; otherwise, clean the codebase by removing the dead code.

### 2.3 JIT Memory Page Recycling — Investigated, Not an Issue
*   **Original claim:** `allocMemoryPages` is executed on every JIT compilation run, requesting virtual memory allocations from the OS kernel.
*   **Finding (verified against code):** this premise is false. `allocMemoryPages` is called exactly once, in the `JitCompilerA64` constructor (`jit_compiler_a64.cpp:133`). `JitCompilerA64` is itself constructed exactly once per `VirtualMachine` (`vm.cpp:158`), and `VirtualMachine` is constructed exactly once per worker thread for the lifetime of that thread (`mining_engine.cpp:283` — `VirtualMachine vm(flags);` sits above the mining loop, not inside it). Each subsequent `generateProgram`/`generateProgramLight` call (once per RandomX program, 8 per hash) only resets `codePos` and rewrites the same pre-allocated `code` buffer — no new page allocation, no `mprotect` toggling, per compile. There is nothing to recycle; the buffer is already reused for the process/thread lifetime.
*   **Action:** no code change needed. Left in the plan (rather than deleted) so a future pass doesn't re-investigate the same non-issue — see the audit note at the top of this file about stale claims propagating across docs.

---

## 3. Code Quality, Testing & Security

### 3.1 Hardened Network Parsing & Fuzzing
*   **Vulnerability:** The stratum JSON parser in `armrx::json` relies on custom string search procedures (`find`, `get_string`). A compromised mining pool could exploit this by sending nested arrays, malformed unicode control characters, or oversized string payloads to crash the worker thread or overflow stack variables.
*   **Mitigation:** 
    1. Establish a fuzzing target using LibFuzzer to stream mutated payloads to `StratumClient::handle_line`.
    2. Re-engineer the JSON module to use a hardened, non-allocating SAX parser with strict input length checks.

### 3.2 Automated Stratum Protocol Testing
*   **Coverage Gap:** The test suite has no coverage for networking protocols, socket reconnect loops, or pool failovers.
*   **Strategy:** Build a local test harness `test_pool_protocol.cpp` using a mock TCP socket. It must simulate:
    *   Stratum V1 / CryptoNote subscribe handshakes.
    *   Simulated connection drops to verify that `StratumClient` executes exponential backoff.
    *   Pool timeout failures to verify that `PoolManager` seamlessly migrates workers to the next configured fallback pool.

### 3.3 Windows Code Path — Dead Weight, Not a Live Vulnerability — ✅ Annotated (2026-07-21)
*   **Original claim:** In [virtual_memory.c](file:///home/mechres/Projeler/aarch64-randomx/src/virtual_memory.c#L212), Windows builds query `SeLockMemoryPrivilege`, and denial should degrade gracefully instead of failing.
*   **Finding (verified against code):** the graceful-degradation half already exists at the *call site* — `MappedMemory` (`mining_engine.hpp:27-43`) and `vm.cpp:136` already treat a `NULL` return from `allocLargePagesMemory` as "no huge pages" and fall back to plain `mmap` + `MADV_HUGEPAGE`. More importantly, `_WIN32`/`_MSC_VER`/`__CYGWIN__` appear **nowhere else** in the codebase — no CMake Windows target, no MSVC toolchain support, and the fallback path in `MappedMemory` itself calls POSIX-only `::mmap`/`::madvise` unconditionally, which wouldn't compile on Windows regardless. The Windows branch in `virtual_memory.c` is inherited verbatim from upstream RandomX and is unreachable dead code in this project, which is Linux-only by design (per `README.md`).
*   **Action:** downgrade from "security fix" to a cleanup task. Either delete the `_WIN32`/`__CYGWIN__` branches from `virtual_memory.c` entirely (consistent with the project's zero-dead-code discipline elsewhere), or leave a one-line comment noting they're vestigial upstream code, never compiled. Do not spend effort hardening a path that can't build.
*   **Implemented:** chose the annotate-not-delete option — the Windows branches are interleaved with the Apple/BSD paths this project *does* build in the same functions, so surgical deletion across ~8 sites risked collateral damage in a memory-protection-critical file for a Phase-1 "quick win." Added a header comment in `virtual_memory.c` explaining the branches are dead in this Linux-only project. Revisit full deletion later if the file gets touched for other reasons.

---

## 4. Developer Experience (DX) & CI/CD Pipeline

### 4.1 Cross-Compile Containerization
*   **Bottleneck:** Local development relies on manual host synchronization (`devbox_sync`) to a remote target, complicating CI automation.
*   **Strategy:** Build a Dockerfile wrapping a QEMU AArch64 environment with a postmarketOS toolchain. Set up a GitHub Actions workflow that executes this container, compiling the source and running the CTest suite on virtualized AArch64 runners.

### 4.2 Unified Compilation Flag Invariants
*   **Original bottleneck:** Flag scopes (`ARMRX_ENABLE_JIT_FAST_DIV_SQRT`, `ARMRX_FAST_MATH`, `ARMRX_JIT_PROFILE`) are configured across different files, risking compilation conflicts (e.g. public definition leakage clobbering LTO/PGO optimizations).
*   **Update (verified against code):** the specific crash this was chasing is already fixed. `ARMRX_JIT_FAST_DIV_SQRT` was `PUBLIC` (propagating into unrelated translation units like `superscalar.cpp`/`dataset.cpp` and corrupting `x29` under GCC 15 + LTO), but commit `b814c17e` (2026-07-21) changed it to `PRIVATE` (`CMakeLists.txt:135`) — root-caused and written up in `docs/jit-buffer-size-audit.md` §3. `ARMRX_JIT_FAST_DIV_SQRT` was the only flag actually implicated in a crash; `ARMRX_HAVE_JIT`/`ARMRX_HAVE_TLS`/`ARMRX_HAVE_HWLOC` are `PUBLIC` deliberately (consumers like `main.cpp` and the tests need to see them via `#ifdef`), and `ARMRX_JIT_PROFILE` (still `PUBLIC`, `CMakeLists.txt:130`) has no reported issue.
*   **Strategy (downgraded from "safety fix" to DX nice-to-have):** if flag sprawl in `CMakeLists.txt` becomes hard to track, move compile definition bindings to a centralized `cmake/CompilerFlags.cmake` file with a comment on each definition explaining why its scope (`PUBLIC` vs `PRIVATE`) was chosen. No longer urgent — do this opportunistically, not as a Phase 1 item.

---

## 5. Future-Proof Roadmap

```
Phase 1 (Short-term) ──────────────► Phase 2 (Medium-term) ─────────────► Phase 3 (Long-term)
✅ Fix Exporter join-on-destroy      • main.cpp → CommandLineParser +   • QEMU Docker & GHA CI
✅ Abstract Stratum nonce metadata     MinerApp split                  • Stratum V2 integration
✅ Annotate dead Windows alloc path  • Reuse workers for dataset       • Opportunistic CMake flag
   (all three done 2026-07-21)      • Mock stratum socket tests         centralization (no longer
                                      • Fuzz stratum JSON parser          crash-motivated, see §4.2)
                                      • Resolve Argon2 NEON permute
```

### Phase 1: Short-term / Immediate (Correctness — quick, low-risk fixes) — ✅ Done (2026-07-21)
*   **Tasks:**
    1. ✅ `MetricsExporter` (§1.1): removed `thread_.detach()`, joined the thread in the destructor after the existing `shutdown()` call.
    2. ✅ Abstracted Stratum nonce metadata (§1.2) out of the two hardcoded call sites into `StratumClient` member state + `set_nonce_config()`.
    3. ✅ Annotated the unreachable `_WIN32`/`__CYGWIN__` branch in `virtual_memory.c` (§3.3) as dead code (see §3.3 for why annotation was chosen over deletion).
*   **Verified:** `cmake --build build -j` clean; `ctest --test-dir build --output-on-failure` 3/3 passing (`armrx_tests`, `test_mining`, `bench_armrx`) on the x86_64 dev sandbox (interpreted-only, JIT excluded at build time).
*   **Verified on real AArch64 hardware (2026-07-21):** the devbox MCP tools weren't wired into this session, so validated over direct SSH instead (same steps `devbox_sync`/`devbox_build`/`devbox_test` would run) — `rsync` to the device, native `cmake --build`, then `ctest`. All 6/6 tests passed, including the JIT-only `bench_opcodes`, `test_jit_encodings`, and `test_jit_determinism` that don't even compile on x86_64: `armrx_tests` 25.5s, `test_mining` 10.8s, `bench_armrx` 296.0s, `bench_opcodes` 215.7s, `test_jit_encodings` 53.9s, `test_jit_determinism` 11.0s.
*   **Rationale:** All three were small, mechanical, and didn't require design decisions — good first tasks with no open questions.

### Phase 2: Medium-term (Structural Refactors & Test Coverage)
*   **Tasks:**
    1. Split `main.cpp` (§1.3) into a `CommandLineParser` and a `MinerApp` runner.
    2. Re-engineer `MiningEngine` to reuse existing worker threads for dataset initialization via a synchronization barrier (§2.1).
    3. Write a mock TCP server to test Stratum V1 / CryptoNote pool handshakes, timeouts, and failovers under CTest (§3.2).
    4. Write fuzzing targets to validate `armrx::json` against malformed payloads (§3.1).
    5. Benchmark NEON Argon2 `permute_block_neon` on the Cortex-A53 and either enable it or delete the dead `#if 0` block (§2.2).
*   **Expected Outcomes:** Smaller, testable `main.cpp`. Elimination of thread-spawning latency during seed key changes. Hardened network parsing. Automated validation of pool failover states.
*   **Rationale:** These require real design/measurement work (barrier synchronization, mock socket harness, a genuine A/B benchmark) rather than mechanical fixes, so they follow the Phase 1 quick wins.

### Phase 3: Long-term (CI/CD Automation, Protocol Work & Opportunistic Cleanup)
*   **Tasks:**
    1. Deploy QEMU AArch64 container environments on GitHub Actions CI (§4.1).
    2. Implement native Stratum V2 protocol support to minimize data payload transfers.
    3. Opportunistically centralize CMake compile-definition scopes into `cmake/CompilerFlags.cmake` (§4.2) — downgraded from Phase 1 now that the one flag that actually caused a crash (`ARMRX_JIT_FAST_DIV_SQRT`) is already `PRIVATE`. Do this only if flag sprawl becomes a real maintenance problem.
    4. JIT/CBRANCH instruction-scheduling and cache-alignment tuning — tracked in more detail in `ROADMAP.md`'s Performance table (P3/P4); re-measure branch-miss attribution post-AES-fix before investing further here, per `docs/audit-20260721-cross-reference.md` §3.1.
*   **Expected Outcomes:** Zero-manual-setup build automation. Stratum V2 network throughput gains for low-bandwidth pool setups.
*   **Rationale:** Scalable build infrastructure enables future community contributions; Stratum V2 and JIT tuning are valuable but not blocking anything else in this plan.
