# armrx — Master Update and Improvement Plan

This document serves as the master plan and improvement roadmap for the `armrx` RandomX AArch64 miner. It details structural architecture upgrades, microarchitectural performance optimizations, quality assurance steps, developer experience (DX) refinements, and a phased execution schedule.

---

## 1. Code Architecture & Structural Integrity

### 1.1 Unsafe Thread Detachment in `MetricsExporter`
*   **Bottleneck:** In [metrics.hpp](file:///home/mechres/Projeler/aarch64-randomx/include/armrx/metrics.hpp#L78), the Prometheus server spawns a background thread and immediately calls `thread_.detach()`. The metrics provider callback lambda captures local stack instances from `main()` (such as `engine` and `pool_mgr`) by reference. If the miner terminates or `MetricsExporter` is destroyed, the detached background thread can execute the callback on dangling references, causing a use-after-free segmentation fault on exit.
*   **Refactoring:** Convert the socket thread to a joinable lifecycle. 
    1. Re-architect the class to hold a joinable `std::thread`.
    2. In the destructor, set an atomic `running_` flag to `false`.
    3. Invoke `::shutdown(server_fd_, SHUT_RDWR)` to force the blocking `::accept` socket call to immediately return with `EINVAL` or `EBADF`.
    4. Call `thread_.join()` to guarantee the thread has terminated before references in `main()` are destroyed.

### 1.2 Configuration Hardcoding of Stratum Nonces
*   **Bottleneck:** Nonce parameters (`nonce_offset = 39` and `nonce_size = 4`) are hardcoded directly inside the notification handlers in [stratum_client.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/stratum_client.cpp#L644-L645). While correct for Monero, this hardcoding limits modularity and compatibility with custom stratum configurations or alternative RandomX-based blockchains.
*   **Refactoring:** Abstract nonce parameters.
    1. Define a `NonceMetadata` struct in `mining_common.hpp` containing `offset` and `size` fields.
    2. Propagate these configuration fields from `PoolConfig` down through `StratumClient` and into the generated `Job` instances.

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

### 2.3 JIT Memory Page Recycling
*   **Bottleneck:** `allocMemoryPages` is executed on every JIT compilation run, requesting virtual memory allocations from the OS kernel.
*   **Optimization:** Implement a pre-allocated pool of JIT RX pages during miner initialization. Recirculate these buffers across compile cycles, using `mprotect` calls only to toggle permissions, reducing memory allocation transitions.

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

### 3.3 Windows Privilege Least-Privilege Alignment
*   **Vulnerability:** In [virtual_memory.c](file:///home/mechres/Projeler/aarch64-randomx/src/virtual_memory.c#L212), Windows builds query `SeLockMemoryPrivilege`. Requesting high-level privileges creates security flags on host platforms.
*   **Mitigation:** Enforce strict error handling. If privilege request is denied, print a warning stating that large pages are disabled, and degrade gracefully to standard virtual allocation instead of failing.

---

## 4. Developer Experience (DX) & CI/CD Pipeline

### 4.1 Cross-Compile Containerization
*   **Bottleneck:** Local development relies on manual host synchronization (`devbox_sync`) to a remote target, complicating CI automation.
*   **Strategy:** Build a Dockerfile wrapping a QEMU AArch64 environment with a postmarketOS toolchain. Set up a GitHub Actions workflow that executes this container, compiling the source and running the CTest suite on virtualized AArch64 runners.

### 4.2 Unified Compilation Flag Invariants
*   **Bottleneck:** Flag scopes (`ARMRX_ENABLE_JIT_FAST_DIV_SQRT`, `ARMRX_FAST_MATH`, `ARMRX_JIT_PROFILE`) are configured across different files, risking compilation conflicts (e.g. public definition leakage clobbering LTO/PGO optimizations).
*   **Strategy:** Move compile definition bindings to a centralized `cmake/CompilerFlags.cmake` file. Enforce static checks to throw a build error if conflicting configurations are requested.

---

## 5. Future-Proof Roadmap

```
Phase 1 (Short-term) ──────────────► Phase 2 (Medium-term) ─────────────► Phase 3 (Long-term)
• Fix Exporter data race             • Reuse workers for dataset        • QEMU Docker & GHA CI
• Centralize CMake flags             • Mock stratum socket tests        • Stratum V2 integration
• Private NR math validations       • Fuzz stratum JSON parser         • JIT cache line tuning
```

### Phase 1: Short-term / Immediate (Security & Thread Safety)
*   **Tasks:**
    1. Replace `MetricsExporter` detached thread with a joinable thread; fix the destructor socket shutdown block.
    2. Audit compile definitions and restrict flag scopes to `PRIVATE` in `CMakeLists.txt`.
    3. Ensure Windows privilege check degradations are warning-only.
*   **Expected Outcomes:** Elimination of shutdown segmentation faults when Prometheus is enabled. Safe build flag separation under LTO compiler optimizations.
*   **Rationale:** Resolving thread safety issues and compile-time flag clobbering prevents unstable execution and unblocks developer profiling.

### Phase 2: Medium-term (Microarchitectural Performance & Test Coverage)
*   **Tasks:**
    1. Re-engineer `MiningEngine` to reuse existing worker threads for dataset initialization via a synchronization barrier.
    2. Write a mock TCP server to test Stratum V1 / CryptoNote pool handshakes, timeouts, and failovers under CTest.
    3. Write fuzzing targets to validate `armrx::json` against malformed payloads.
    4. Benchmark NEON Argon2 `permute_block_neon` on the Cortex-A53 and clean or enable it.
*   **Expected Outcomes:** Elimination of thread-spawning latency during seed key changes. Hardened network parsing. Automated validation of pool failover states.
*   **Rationale:** Eliminates resource overhead and guarantees that the client can survive hostile or malformed network payloads during pool operations.

### Phase 3: Long-term (CI/CD Automation & Stratum V2)
*   **Tasks:**
    1. Deploy QEMU AArch64 container environments on GitHub Actions CI.
    2. Implement native Stratum V2 protocol support to minimize data payload transfers.
    3. Benchmark JIT compiler instruction alignments to minimize CPU cache eviction rates.
*   **Expected Outcomes:** Zero-manual-setup build automation. Highly optimized network performance on latency-constrained pool setups.
*   **Rationale:** Scalable build infrastructure enables future community contributions, and Stratum V2 ensures optimal network throughput for low-bandwidth environments.
