# armrx — Beyond-Parity Optimization & Scaling Plan (v1)

This plan outlines the roadmap to optimize `armrx` beyond XMRig performance parity on AArch64 systems. Having achieved parity (~28.9 H/s on 8× Cortex-A53 against Monero mainnet), our focus transitions from basic JIT correctness and thread stabilization to mitigating hardware ceilings and CPU microarchitectural bottlenecks.

---

## 1. Hardware Context & The 30% Scaling Drop

### The Bottleneck
*   **Single-Threaded Baseline:** **5.16 H/s per thread** (measured in [bench_armrx.cpp](file:///home/mechres/Projeler/aarch64-randomx/tests/bench_armrx.cpp)).
*   **8-Thread Load Performance:** **3.61 H/s per thread** (28.92 H/s total, measured under Monero pool mining).
*   **Analysis:** This represents a **~30% per-thread throughput drop** under full core load. On Cortex-A53 cores (which share cache and memory buses via a single-channel memory controller), having all 8 threads execute high-bandwidth Argon2 cache lookups and scratchpad mixes simultaneously saturates L2/L3 cache and RAM interfaces, creating severe memory bus stalls.

---

## 2. Core Pillars of the Beyond-Parity Strategy

### Pillar A: Worker Phase Staggering (Bandwidth Mitigation)
To recover the 30% performance penalty under load, we must desynchronize the worker threads' memory-intensive phases.
1.  **Diagnostic Harness:**
    *   Build a script or tool to run local benchmarks from 1 to 8 threads and measure scaling curves.
2.  **Phase Staggering Implementation:**
    *   Modify [mining_engine.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/mining_engine.cpp) to introduce a staggered startup delay or an offset logic for worker loops. 
    *   By desynchronizing the start times, we ensure that the threads do not execute high-bandwidth Argon2 G-functions or memory-hard Superscalar loops in lockstep, smoothing out spike memory requests and reducing L2/RAM cache thrashing.

### Pillar B: Newton-Raphson FDIV/FSQRT JIT Unblocking
Native `fdiv` (23 cycles) and `fsqrt` (29 cycles) instructions are extremely expensive on in-order Cortex-A53 pipelines. 
1.  **Debug the `x29` Corruption:**
    *   GPR register `x29` (Frame Pointer) is currently used by the JIT compiler to load integer literals for `IMUL_RCP` (see [jit_compiler_a64.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/jit_compiler_a64.cpp#L882) and [jit_compiler_a64_static.S](file:///home/mechres/Projeler/aarch64-randomx/src/jit_compiler_a64_static.S#L286)). 
    *   We will investigate and debug the Newton-Raphson `h_FDIV_M` and `h_FSQRT_R` handlers under `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` to isolate why GPR `x29` gets corrupted at runtime (e.g., verifying if the literal pool loading overlaps or if a vector register mask clobbers GPR state).
2.  **Newton-Raphson Simplification:**
    *   RandomX does not require IEEE-754 correctly-rounded results for floating-point divisions.
    *   Simplify the math by **dropping iterations 2–3 and the Markstein correction entirely**, emitting only a single-iteration reciprocal estimate sequence. This cuts the instruction footprint per `fdiv`/`fsqrt` from 17 to 4 instructions, saving significant execution cycles.

### Pillar C: SuperscalarHash JIT Scheduling
*   **Concept:** While a full program-wide DAG list-scheduling pass is blocked by high JIT compilation costs and branch boundaries, we can apply target-specific instruction scheduling directly to **SuperscalarHash** (`RANDOMX_SUPERSCALAR_LATENCY = 170`).
*   **Method:**
    *   Audit the instruction sequences emitted by the Superscalar compiler in [superscalar.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/superscalar.cpp).
    *   Reschedule emitted instructions to maximize dual-issue slots (pairing integer ALU operations with loads/stores) and prevent register write-after-read hazards on in-order pipelines.

### Pillar D: Testing & Observability Expansion
*   **Observability:** Implement a lightweight HTTP server on top of [mining_engine.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/mining_engine.cpp) to export Prometheus-compatible metrics (hashrate per thread, accepted shares, connection uptime, memory usage).
*   **Handshake/TLS Testing:** Create integration tests [test_stratum_handshake.cpp](file:///home/mechres/Projeler/aarch64-randomx/tests/test_stratum_handshake.cpp) and [test_tls_verification.cpp](file:///home/mechres/Projeler/aarch64-randomx/tests/test_tls_verification.cpp) to verify pool connectivity under ThreadSanitizer (TSan).

---

## 3. Implementation Phasing

| Phase | Description | Files Affected | Estimated Gain | Risk |
|---|---|---|---|---|
| **Phase A** | Worker Scaling Harness & Phase Staggering | [mining_engine.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/mining_engine.cpp) | +10–15% | Low |
| **Phase B** | Debug `x29` and implement simplified Newton-Raphson | [jit_compiler_a64.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/jit_compiler_a64.cpp) | +5–8% | Medium |
| **Phase C** | Superscalar JIT Scheduling | [superscalar.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/superscalar.cpp) | +3–5% | Medium |
| **Phase D** | Handshake/TLS Tests & Prometheus Exporter | [mining_engine.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/mining_engine.cpp), `tests/` | Observability | Low |
