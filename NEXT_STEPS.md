# Next Steps Task List

**Updated:** 2026-07-21  
**HEAD:** a3a7244  
**Devbox:** 192.168.10.156

---

## Current Telemetry Invariants (Cortex-A53, Light Mode, JIT)

| Metric | Software AES Baseline (SW) | PGO + SW AES Optimized | Δ |
|--------|:-------------------------:|:----------------------:|:-:|
| **Single-thread hashrate** | 4.34 H/s | **5.18 H/s** | **+19.3%** |
| **8-thread pool hashrate (pinned)** | ~22.0 H/s | **25.28 H/s** | **+14.9%** |
| **Init scratchpad** | 30,817 μs (12.35%) | 30,817 μs (12.35%) | — |
| **Get final result** | 23,207 μs (9.30%) | 23,207 μs (9.30%) | — |
| **Chain execution (VM)** | 194,692 μs (84.6%) | 170,860 μs (68.4%) | **−12.2%** |
| **Branch miss rate** | 31.6% | **31.6%** | — |

---

## Prioritized Next Steps

According to the master plan ([PLAN.md](PLAN.md)), the short-term and medium-term action list is defined below:

### 1. Phase 1 — Immediate (Security & Thread Safety)
*   [ ] **Fix detached thread data race** in `MetricsExporter` ([metrics.hpp](file:///home/mechres/Projeler/aarch64-randomx/include/armrx/metrics.hpp#L78)):
    *   Change thread loops to joinable; set termination flags and close socket descriptors in the destructor to unblock blocking `accept` calls.
*   [ ] **Centralize CMake configuration definitions:**
    *   Move flags (`ARMRX_ENABLE_JIT_FAST_DIV_SQRT`, etc.) to a separate compiler options file and enforce compatibility asserts.

### 2. Phase 2 — Medium-term (Microarchitectural Performance & Test Coverage)
*   [ ] **Worker Thread Reuse for Dataset Initialization:**
    *   Modify `MiningEngine::set_job` to reuse long-lived affinity-pinned mining threads instead of constructing and discarding temporary `init_threads` vectors on every seed transition.
*   [ ] **Mock Stratum Socket Integration Tests:**
    *   Implement local test harness simulating stratum handshakes, reconnect backoffs, pool timeouts, and automatic failovers under CTest.
*   [ ] **Fuzzing the JSON Parser:**
    *   Integrate a basic fuzzing framework (e.g. LibFuzzer) to test `armrx::json` against mutated/hostile payloads.
*   [ ] **Benchmarking NEON `permute_block_neon`:**
    *   Compare SIMD block permutations against the scalar version on the Cortex-A53 and permanently enable or clean them from `src/argon2.cpp`.

### 3. Phase 3 — Long-term (CI/CD Automation & Stratum V2)
*   [ ] **Cross-Compile GHA CI Integration:**
    *   Construct a QEMU-based Docker container to build and run the CTest suite on virtualized AArch64 runners.
*   [ ] **Stratum V2 Protocol support:**
    *   Deploy native Stratum V2 communication support.

---

## Resolved Blockers & Items
*   [x] **JIT Buffer Overflow Safety:** Root-caused segfaults to PUBLIC flag leaks clobbering register configurations under GCC 15 LTO. Audited sizes (average program size is 2,380 bytes, well below the 16 KB threshold). Retained the doubled 32,768-byte buffer size as a defense-in-depth practice.
*   [x] **PGO Linker Errors:** Unblocked compiler profile linkage across Alpine/musl and GCC 15.2.0.
*   [x] **CTest Mismatches:** Corrected CTest test target path resolving. All 6 tests now pass on virtualized and target platforms.
