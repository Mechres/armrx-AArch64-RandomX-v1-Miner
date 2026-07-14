## Performance Optimizations

### Profile: Current bottleneck analysis

XMRig on same hardware (8× Cortex-A53, light/slow mode) achieves **27 H/s**.
Our miner achieves **~23 H/s** (after NEON vectorization, Cache Huge Pages, and LTO) — a **15% remaining gap**.

### Profiling Breakdown (from timing instrumentation)
* **JIT compilation overhead**: **~1.5%** of the VM loop time.
* **JIT program execution**: **~98.5%** of the VM loop time.
* **Conclusion**: We must focus on JIT execution code efficiency, memory latency hiding, and pipeline scheduling to close the remaining 15% gap.

---

## 🚀 Next-Level Performance Optimizations (Unimplemented)

### 1. JIT Register Allocation & Memory Operation Coalescing
Optimizing the JIT's register usage to minimize load/store dependencies.
* **Tweak**: Audit `jit_compiler_a64.cpp` to ensure memory load operations are coalesced where possible and register forwarding is maximized.
* **Implementation**: Ensure registers that hold immediate offsets or scratchpad values are loaded using optimal LDR/STR sequences, avoiding pipeline interlocks on write-after-read hazards.

### 2. Floating Point / Vector Register Save-Restore Optimization
Before executing the JIT program, `VirtualMachine::run` saves host floating-point/vector registers (`v8`-`v15` as per the AArch64 ABI) and restores them afterward.
* **Tweak**: Optimize the save/restore assembly routine using vector pair load/store instructions (`ldp`/`stp`).
* **Implementation**: Inspect the context-switch stubs in `jit_compiler_a64_static.S` to verify `stp` and `ldp` are utilized for all vector registers to minimize stack traffic.

### 3. Fast Mode Dataset Item JIT Compilation
If the miner is run on devices with >2.3 GiB memory, Fast Mode is activated. Currently, dataset initialization is done via vectorized NEON loops.
* **Tweak**: Compile a specialized JIT program for dataset item generation rather than calling static ASM loops.
* **Implementation**: Implement `JitCompilerA64::generateDatasetInitProgram` in the JIT body to generate custom machine code for `initialize_dataset`.

---

## 🌐 Observability & Network Roadmap

### 4. HTTP JSON Stats Endpoint
* **Tweak**: Expose hashrate, uptime, shares, and JIT timing statistics via a minimal embedded HTTP server.
* **Implementation**: Use a lightweight socket thread to serve a JSON status payload for Grafana / Prometheus scraping.

### 5. Accepted / Rejected Share Tracker
* **Tweak**: Track submission status and display pool-accepted shares vs rejected shares with percentages in the TUI / console logs.

### 6. Stratum V2 Protocol
* **Tweak**: Support binary Stratum V2 for security, privacy, and lower bandwidth.

---

## ✅ Completed Improvements Log

| Improvement | Category | Measured Impact / Evidence |
|-------------|----------|---------------------------|
| **Scratchpad Cache Prefetch** | JIT | Added `prfm pldl1keep` for scratchpad lines loaded per iteration (spAddr0, spAddr1, spAddr1+32) in the JIT main loop. Hides memory latency on Cortex-A53 in-order pipeline. |
| **Dataset Cache Prefetch** | JIT | Changed dataset line prefetch from `pldl2strm` (L2 streaming) to `pldl1keep` (L1 keep) — data is used immediately after SuperscalarHash, so L1 avoids a refill. |
| **JIT Loop Alignment** | JIT | Aligned main JIT compiled program execution loop to a 32-byte boundary using `.p2align 5` before entry label, optimizing instruction fetch/caching. |
| **Scratchpad Huge Pages** | Memory | Added `madvise(MADV_HUGEPAGE)` on 2 MiB scratchpad; reduced TLB misses. |
| **Argon2d Cache Huge Pages** | Memory | Converted `std::vector` to `mmap` + `MADV_HUGEPAGE` for the 256 MiB cache. Boosted hashrate from 21.2 H/s to 23.0 H/s (+8.5%). |
| **Link Time Optimization (LTO)** | Build | Enabled `INTERPROCEDURAL_OPTIMIZATION` in CMake. Reduced remote `ctest` execution time by **48%** (from 51.2s to 26.5s). |
| **NEON SIMD Superscalar** | SIMD | Vectorized superscalar item generation using `uint64x2_t` NEON registers. Sped up dataset benchmark from 3721 items/s to 5806 items/s (+56%). |
| **Lock-Free Job Counter** | Concurrency | Replaced recursive mutex locks with an atomic generation check, eliminating lock contention. |
| **CPU Affinity Pinning** | System | Pinned mining threads to specific CPU cores, eliminating context migration overhead. |
| **TLS/SSL Pool Connections** | Network | Integrated OpenSSL wrapping for encrypted stratum communication. |
| **Pool Failover** | Network | Implemented failover queueing with 5-retry limit and 2s cooldown. |
| **CryptoNote Stratum Protocol** | Network | Added automatic login/subscribe fallback to CryptoNote protocol for compatibility with pools (e.g. Herominers). |
| **Config File Support** | UX | Added `--config=<path>` flag, `~/.config/armrx/config.json`, and `./armrx.conf` auto-detection. CLI args override config values. |
| **Auto-Reconnect & Pool Failover** | Network | Exponential backoff reconnection (1s→2s→...→30s cap, 5 retries) with automatic multi-pool cycling on permanent failure. |
| **Terminal UI Dashboard** | UX | Zero-dependency ANSI TUI (`--tui`) with per-worker hash bars, JIT compile/execute breakdown, pool status, and uptime. |
| **Per-Worker Hashrate Counters** | Observability | Added atomic hashrate accumulators and exposed them in the terminal UI. |
