# armrx — Beyond-Parity Optimization & Scaling Plan (v2)

This plan outlines the roadmap to optimize `armrx` beyond XMRig performance parity on AArch64 systems. Having achieved parity (~28.9 H/s on 8× Cortex-A53 against Monero mainnet), our focus transitions from basic JIT correctness and thread stabilization to mitigating hardware ceilings and CPU microarchitectural bottlenecks.

---

## 1. Hardware Context & The 30% Scaling Drop

### The Bottleneck
*   **Single-Threaded Baseline:** **5.16–5.18 H/s per thread** (measured in [bench_armrx.cpp](file:///home/mechres/Projeler/aarch64-randomx/tests/bench_armrx.cpp)).
*   **8-Thread Load Performance:** **3.61 H/s per thread** (28.92 H/s total, measured under Monero pool mining with mainnet blob).
*   **Analysis:** This represents a **~30% per-thread throughput drop** under full core load. On Cortex-A53 cores (which share cache and memory buses via a single-channel memory controller), having all 8 threads simultaneously hit the 256 MiB Argon2 cache for light-mode lookups creates severe memory bus contention.
*   **Caveat — MAP_HUGETLB not yet deployed at time of measurement:** The memory tier upgrades (MAP_HUGETLB for dataset/cache/scratchpad, committed in `8f7d273`) were deployed after this baseline was recorded. Huge pages reduce TLB miss rates significantly, which may have already changed the scaling characteristic. **Re-measure before optimizing.**

---

## 2. Core Pillars of the Beyond-Parity Strategy

### Pillar A: Worker Phase Staggering (Bandwidth Mitigation)
To recover the 30% performance penalty under load, we must desynchronize the worker threads' memory-intensive phases.
1.  **Diagnostic Harness:**
    *   Build a script or tool to run local benchmarks from 1 to 8 threads and measure scaling curves.
    *   **Prerequisite: re-measure with MAP_HUGETLB** to establish an up-to-date baseline.
2.  **Phase Staggering Implementation:**
    *   Modify [mining_engine.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/mining_engine.cpp) to introduce a staggered startup delay or an offset logic for worker loops.
    *   Startup staggering alone is unlikely to be sufficient — the RandomX workload is memory-bandwidth-bound *throughout* every hash (scratchpad mixing touches all 2 MiB each iteration). A more effective approach is **intra-loop staggering**: different threads running their hash iterations at phase offsets within the worker loop itself, so they don't hit the Argon2 cache in lockstep on every hash.

### Pillar B: Newton-Raphson FDIV/FSQRT JIT Unblocking (Highest Single JIT Win)

Native `fdiv` (~23 cycles) and `fsqrt` (~29 cycles) instructions are expensive on in-order Cortex-A53 pipelines. A Newton-Raphson approximation with fewer iterations can beat hardware latency while producing results within RandomX's tolerance (which does not require IEEE-754 correctly-rounded results).

**Current NR path instruction counts (gated behind `ARMRX_ENABLE_JIT_FAST_DIV_SQRT`):**

| Handler | Default (hardware) | NR path (3 iterations + Markstein) |
|---------|-------------------|------------------------------------|
| `h_FDIV_M` | 1 instruction (`FDIV`) | **12 instructions** |
| `h_FSQRT_R` | 1 instruction (`FSQRT`) | **19 instructions** |

(Previous versions of this plan cited "17 instructions" for FDIV_M — corrected to 12 per source audit.)

1.  **Debug the `x29` Corruption:**
    *   GPR register `x29` (Frame Pointer) is used by the JIT compiler to hold the IMUL_RCP literal pool base pointer during JIT execution, saved/restored in the prologue/epilogue (`static.S:131,494`). On Cortex-A53 the NR path crashes with x29 corruption; root cause is undiagnosed.
    *   **Important: GPR x29 is NOT clobbered by NEON register operations.** AArch64 GPRs (x0–x30) and SIMD/FP registers (v0–v31) are physically separate register banks. The `bif v28.2d, v30.2d, v29.2d` instruction at line 1092 writes to **v29** (NEON), not x29 (GPR). The earlier hypothesis that "a vector register mask clobbers GPR state" is architecturally impossible.
    *   The actual suspects for x29 corruption on Cortex-A53 in-order (from post-mortem analysis):
        1. **Static template conflict:** The static prologue in `jit_compiler_a64_static.S` uses x29 as a literal base pointer; the NR sequence writes to the code buffer at positions that may overlap the static template's x29 literal load.
        2. **Literal-pool load displacement:** The `LDR_LITERAL` instruction emitted for IMUL_RCP (when `literal_id >= 12`) computes a PC-relative offset. If the NR path shifts code positions, this offset could become misaligned.
        3. **ADR/ADRP misalignment:** Similar to the CBRANCH `imm19=1` vs `imm19=2` bug — an off-by-one in a branch or literal displacement caused by the NR path's larger instruction footprint.
2.  **Newton-Raphson Simplification (realistic target):**
    *   RandomX does not require IEEE-754 correctly-rounded results, but NR precision *does* matter: floating-point errors accumulate across 2048 program iterations, and the final hash must match pool expectations.
    *   Dropping to 0 iterations (raw `frecpe` only) gives ~2.5 bits of precision — insufficient.
    *   Dropping to **1 iteration without Markstein correction** gives ~2^-12 relative error. **Unlikely to pass KAT** — pool hash mismatch expected.
    *   Dropping to **1 iteration + simplified Markstein correction** gives ~2^-24 relative error. This is the realistic minimum — approximately **8 instructions** per FDIV_M (down from 12), not 4.
    *   The Markstein correction itself can be simplified: `fmla` replaces the separate `fmul`+`fmls` sequence by leveraging the FMA unit directly, saving 2 instructions.

**Recommendation:** Debug the x29 crash on the existing NR path first. The simplified path will share the same code-layout properties — if the current (correctly-encoded) NR code crashes, the simplified version is likely to crash identically.

### Pillar C: SuperscalarHash JIT Scheduling
*   **Concept:** While a full program-wide DAG list-scheduling pass is blocked by CBRANCH boundaries (which break the instruction stream into small regions), we can apply target-specific instruction scheduling to **SuperscalarHash** (`RANDOMX_SUPERSCALAR_LATENCY = 170`). Each program is ~17 instructions with no control flow — ideal for an in-order scheduling pass.
*   **Method — Corrected implementation target:**
    *   The Superscalar compiler (`superscalar.cpp`) emits virtual x86-like instructions. The x86-to-AArch64 mapping is not 1:1 — the same x86 opcode can map to different AArch64 sequences depending on operand types.
    *   Therefore, scheduling should target the **JIT output** (the AArch64 instruction stream in the code buffer), not the Superscalar DAG. This requires:
        1. A disassembly pass over the emitted JIT buffer for the 8 SuperscalarHash programs.
        2. Hazard detection (write-after-read, pipeline interlock) on the AArch64 instruction stream.
        3. Reordering within boundaries where control flow doesn't force serialization.
    *   The current NEON-vectorized path (`execute_superscalar_neon` in `superscalar.cpp`) already processes 2 items in parallel, which may already saturate the A53's dual-issue pipeline for this workload. **Measure before optimizing** — if NEON + scalar dispatch already hits 2 IPC, scheduling gains are minimal.

### Pillar D: Testing & Observability Expansion
*   **Observability — unblocked by structured logger:** The structured logger (`include/armrx/log.hpp`, committed in `2f009f5`) is now deployed, providing the thread-safe logging sink needed for a metrics exporter. Implement a lightweight HTTP server exporting Prometheus-compatible metrics (hashrate per thread, accepted shares, connection uptime, memory usage).
    *   **Security requirements:** The HTTP server must:
        - Listen on **localhost only** (not 0.0.0.0).
        - Be **opt-in** via `--metrics-port=` flag (not default-on).
        - Serve a **read-only** metrics endpoint (`GET /metrics`).
        - Have **no write or control endpoints** (no `/reload`, no `/config`, no POST handlers).
*   **Handshake/TLS Testing:** Create integration tests [test_stratum_handshake.cpp](file:///home/mechres/Projeler/aarch64-randomx/tests/test_stratum_handshake.cpp) and [test_tls_verification.cpp](file:///home/mechres/Projeler/aarch64-randomx/tests/test_tls_verification.cpp) to verify pool connectivity under ThreadSanitizer (TSan).

---

## 3. Implementation Phasing

### Priority Re-ranking (from post-parity analysis)

Based on the measured 30% scaling drop and current codebase state, the levers ranked by realistic impact:

| # | Lever | Realistic gain | Risk | Why |
|---|---|---|---|---|
| **1** | **Newton-Raphson FDIV/FSQRT postmortem + simplification** | **+5–8%** | Medium | A53 `fdiv` is ~23 cycles; 1-iteration NR is ~12 cycles. Already written (gated behind `ARMRX_ENABLE_JIT_FAST_DIV_SQRT`). Debug the x29 crash first. |
| **2** | **Worker phase staggering (bandwidth contention)** | **+10–15% pool-side** | Low | Closing the 3.6→5.16 H/s/thread gap. Re-measure with MAP_HUGETLB first. Intra-loop staggering > startup staggering. |
| **3** | **Peephole JIT coalescing (existing `peephole-jit-plan.md`)** | **+5–10%** | Medium | Phase 1 tooling already delivered (--jit-dump, bench_opcodes). Requires disassembly comparison with XMRig. |
| **4** | **SuperscalarHash JIT output scheduling** | **+3–5%** | Medium | Requires AArch64-level hazard analysis, not Superscalar DAG changes. Measure NEON saturation first. |
| **5** | **DVFS / thermal pinning** | **+0–5%** | Low | Check `/sys/class/thermal/thermal_zone*/temp` during long pool runs. May already be throttled. |

### Detailed Phase Plan

| Phase | Description | Files Affected | Estimated Gain | Risk | Prerequisites |
|---|---|---|---|---|---|
| **Phase 0** | Re-measure scaling with MAP_HUGETLB (1–8 threads) | — | — | Low | Memory tier upgrades deployed |
| **Phase A** | Worker Scaling Harness & Phase Staggering | [mining_engine.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/mining_engine.cpp) | +10–15% pool-side | Low | Phase 0 (re-measured baseline) |
| **Phase B** | Debug `x29` and implement simplified Newton-Raphson | [jit_compiler_a64.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/jit_compiler_a64.cpp) | +5–8% | Medium | Device debugging session |
| **Phase C** | Superscalar JIT Output Scheduling | [superscalar.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/superscalar.cpp), [jit_compiler_a64.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/jit_compiler_a64.cpp) | +3–5% | Medium | Phase 1.2 (opcode frequency data from bench_opcodes) |
| **Phase D** | Handshake/TLS Tests & Prometheus Exporter | [mining_engine.cpp](file:///home/mechres/Projeler/aarch64-randomx/src/mining_engine.cpp), `tests/` | Observability | Low | Structured logger deployed (already done) |
