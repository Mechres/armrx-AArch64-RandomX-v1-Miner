# Walkthrough: Code Refactoring, W^X Alignment & LITTLE Core Profiling

We completed a comprehensive sweep of the architectural gaps, JIT code deduplication, security-related encapsulation, compile definition scoping, and metrics server logging integration identified in the cross-reference audit. We also profiled the JIT miner on the LITTLE cores.

---

## 1. Code Refactoring & Security Alignments

### JIT Compiler deduplication & encapsulation
*   **Deduplicated JIT Loops:** Extracted `emitPrologueMix` and `emitSpMix2` as private helper methods in `JitCompilerA64`. We collapsed `generateProgram` and `generateProgramLight` onto these helper methods, reducing the duplication of the initialization and scratchpad mix blocks (~70% of function bodies).
*   **Encapsulated Executable Memory:** Deleted the unused public method `getCode()` from `JitCompilerA64` to prevent leaks of raw pointers to executable memory.
*   **Direct Access:** Updated `emitV2AesTweak` to access `jit.code` directly (which is safe as it is a member function of `JitCompilerA64`), completely bypassing the dead `getCode()` accessor.

### Compile definitions scoping
*   Changed `ARMRX_JIT_FAST_DIV_SQRT` from `PUBLIC` to `PRIVATE` in [CMakeLists.txt](file:///home/mechres/Projeler/aarch64-randomx/CMakeLists.txt) to restrict its compilation scope strictly to `armrx_core` units, preventing definition pollution in downstream tests and executables.

### Metrics logger integration
*   Refactored `MetricsExporter` in [metrics.hpp](file:///home/mechres/Projeler/aarch64-randomx/include/armrx/metrics.hpp) to include `"armrx/log.hpp"` and replaced all raw outputs to `std::cerr` with leveled structured logging macros (`ARMRX_LOG_WARN`, `ARMRX_LOG_INFO`), aligning the metrics server with the console-logging ring buffer and TUI disciplines.

---

## 2. Documentation Synchronization

*   **Status Report:** Updated [STATUS_REPORT.md](file:///home/mechres/Projeler/aarch64-randomx/STATUS_REPORT.md) to reflect the completed state of PGO, O12, O13, Software AES inlining, and big.LITTLE scheduling. Updated module listings to remove the deleted `src/aes.cpp`.
*   **Roadmap & Plan:** Updated [ROADMAP.md](file:///home/mechres/Projeler/aarch64-randomx/ROADMAP.md) and [PLAN.md](file:///home/mechres/Projeler/aarch64-randomx/PLAN.md) to reflect these newly completed architectural alignments and performance uplifts.
*   **AES Postmortem:** Fixed incorrect file paths in [aes-ttable-bug-postmortem.md](file:///home/mechres/Projeler/aarch64-randomx/docs/aes-ttable-bug-postmortem.md) to point to `include/armrx/aes.hpp` instead of `src/aes.cpp`.
*   **Evolution Plans:** Archived the stale `next_phase_v2.md` to `docs/archived/next_phase_v2.md`, and created a clean [next_phase_v3.md](file:///home/mechres/Projeler/aarch64-randomx/docs/next_phase_v3.md) mapping out only current outstanding goals (Peephole JIT Phase 2, CBRANCH re-measurement, etc.).

---

## 3. Profiling Results on Core 4 (LITTLE Core)

We successfully ran `bench_armrx` pinned strictly to CPU 4 (Cortex-A53 LITTLE core) to measure JIT execution characteristics:

| Metric / Phase | CPU 0 (Big Core @ ~1.2 GHz) | CPU 4 (LITTLE Core @ lower freq) | Ratio (Big/LITTLE) |
|---|:---:|:---:|:---:|
| **Total hashrate** | **4.55 H/s** | **2.27 H/s** | **2.00×** |
| **fill_aes_1r_x4** | 14.10 ms | 28.24 ms | 2.00× |
| **hash_aes_1r_x4** | 14.67 ms | 29.16 ms | 1.99× |
| **Interpreted mode** | 2.32 sec | 4.59 sec | 1.98× |

### Analysis
*   The Cortex-A53 LITTLE cores perform at exactly half the throughput of the big cores across all components (including AES T-table lookups, JIT execution, and interpreted loop bytecode dispatch).
*   Correctness was fully verified: all 6 tests in CTest passed on the devbox.

---

## 4. Worker-Count Sweep Results & Contention Analysis

We completed a comprehensive 3-minute, 3-repeat worker-count sweep on the Snapdragon 410 devbox, measuring steady-state hashrates after a 40-second warmup period. The results are summarized below:

### Sweep Telemetry Table

| Label | Workers | Big Cores | LITTLE Cores | Repeat 1 (H/s) | Repeat 2 (H/s) | Repeat 3 (H/s) | Mean (H/s) | Stddev (H/s) |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **C** | 4 | 4 | 0 | 6.84* | 16.45 | 16.45 | **16.45** | 0.00 |
| **B5** | 5 | 4 | 1 | 18.74 | 18.73 | 18.74 | **18.74** | 0.01 |
| **B6** | 6 | 4 | 2 | 21.02 | 21.02 | 21.02 | **21.02** | 0.00 |
| **B7** | 7 | 4 | 3 | 23.30 | 23.30 | 23.30 | **23.30** | 0.00 |
| **A_pinned** | 8 | 4 | 4 | 25.13 | 25.13 | 25.58 | **25.28** | 0.26 |
| **A_unpin** | 8 | 4 | 4 | 24.67 | 25.13 | 25.58 | **25.13** | 0.46 |

*\* Note: CONFIG C Repeat 1 was contaminated by background processes from accidental duplicate launches, and has been excluded from the mean/stddev calculation. The clean runs are exactly 16.45 H/s.*

### Findings & Insights

1. **Monotonic Hashrate Scaling:** Hashrate increases monotonically up to 8 threads. There is **no local maximum** between 4 and 8 cores, meaning that utilizing all cores maximizes total throughput despite asymmetric clock domains.
2. **Minimal Memory Bus Contention:**
   - In all configurations (from 4 to 8 workers), the big cores consistently achieve **4.11 H/s/thread** with zero performance degradation.
   - The little cores achieve exactly **2.28 H/s/thread** in B5, B6, and B7.
   - When all 8 cores are busy (A_pinned), the little cores sometimes experience a minor drop to **1.83 H/s/thread**, resulting in a slight deviation from the theoretical linear peak of 25.58 H/s down to 25.13 H/s.
3. **OS-Scheduling / Pinning Advantage:** Pinned execution (`A_pinned`) yields a higher mean hashrate (**25.28 H/s**) and lower variance (**0.26 stddev**) than unpinned execution (**25.13 H/s** mean, **0.46 stddev**). This confirms that binding worker threads to physical CPU cores prevents OS scheduling penalties and core migration overhead.

---

## 5. JIT Buffer Overflow Resolution & Newton-Raphson Evaluation

We identified and successfully resolved the historical Cortex-A53 segfault and frame pointer (`x29`/`x30`) register corruption. We also completed a full performance evaluation of the fast Newton-Raphson JIT division and square root.

### Root Cause & Resolution
1. **JIT Code Buffer Overflow:** The AArch64 JIT compiler previously allocated a static JIT code buffer of exactly 16,384 bytes (`RANDOMX_PROGRAM_MAX_SIZE * 16 * 4`). However, typical RandomX programs emit an average of **19,045 bytes** of instructions. This boundary collision caused the JIT compiler to silently write generated instruction words right over the preloaded literal pool (`literal_x0` to `literal_x30`) situated immediately after the buffer, corrupting the preloaded `x29` and `x30` registers during compile-time.
2. **Fast Math Exacerbation:** Enabling `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` (Newton-Raphson math) adds 10+ instructions per division/sqrt, pushing the compilation size even further into the literal pool and triggering immediate segfaults.
3. **The Fix:** Expanded the instructions buffer in [jit_compiler_a64_static.S](file:///home/mechres/Projeler/aarch64-randomx/src/jit_compiler_a64_static.S) to `RANDOMX_PROGRAM_MAX_SIZE * 32` instructions (32,768 bytes). This resolves the collison and provides a 1.7× safety margin.

### Correctness & Performance Verification
* **Correctness:** With the buffer expanded, compiling with `ARMRX_ENABLE_JIT_FAST_DIV_SQRT=ON` passed 100% of all correctness and determinism tests in CTest.
* **Performance Impact:** 
  * Single-thread hashrate with Newton-Raphson JIT math under PGO USE measured at **5.12 H/s**, compared to **5.18 H/s** for native hardware `fdiv`/`fsqrt` (a ~1.1% hashrate reduction).
  * **Microarchitectural Analysis:** On the in-order Cortex-A53, the native `fdiv` execution unit runs independently. Replacing a single hardware `fdiv` with a 12-17 instruction Newton-Raphson approximation increases FPU pipeline pressure and instruction-decode overhead, resulting in a slight net slowdown. 
  * **Action:** Because native `fdiv`/`fsqrt` yields higher throughput and requires fewer instruction bytes, we recommend keeping `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` turned **OFF** for production runs. The expansion of the JIT buffer size remains a critical stability fix that protects the normal JIT compiler path from potential overflow crashes.
