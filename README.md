# armrx 🚀 — High-Performance AArch64 RandomX v1 Miner

<p align="center">
  <img src="https://img.shields.io/badge/Architecture-AArch64%20%7C%20ARMv8--A%20%2B%20Crypto-blue.svg" alt="Architecture">
  <img src="https://img.shields.io/badge/Language-C%2B%2B20%20%2F%20Assembly-00599C.svg" alt="Language">
  <img src="https://img.shields.io/badge/Correctness%20Tests-100%25%20Passing-brightgreen.svg" alt="Tests">
  <img src="https://img.shields.io/badge/License-MIT-orange.svg" alt="License">
</p>

`armrx` is a **clean-room, highly optimized CPU-only Monero RandomX v1 miner** specifically engineered for AArch64 Linux platforms. Built from the ground up against the official [RandomX Specification](https://github.com/tevador/RandomX), it is entirely independent of any existing mining client codebases.

---

## ⚡ Core Features

*   **Dual-Path VM Execution Engine:**
    *   🚀 **AArch64 JIT compiler (Default):** Generates native machine instructions on-the-fly, leveraging NEON registers and hardware-accelerated instructions.
    *   ⚙️ **Bytecode Interpreter (Fallback):** A highly portable C++ dispatch loop. Used on non-AArch64 platforms (e.g. x86_64 host builds).
*   **Microarchitectural Tuning (Cortex-A53 focus):**
    *   **Emitter Lookahead Scheduler:** Conservative JIT instruction reordering to hide long-latency
        multiply stalls on this in-order core — measured +0.23% IPC / -0.04% cycles, `taskset`-pinned
        (see [`docs/archived/plan_phase6_completed.md`](docs/archived/plan_phase6_completed.md) item 12).
    *   **PGO tooling available, measured null on current code:** the `devbox_pgo_build` pipeline
        (GENERATE→train→USE) works end-to-end, but profile-guided optimization has not produced a
        measurable win on this codebase in either of two independent, pinned A/B measurements —
        kept for future re-evaluation, not claimed as a current performance feature.
    *   **Memory Tiering:** Automated huge-pages mapping (`MAP_HUGETLB` + `MADV_HUGEPAGE`) to eliminate TLB miss penalties under heavy cache pressure.
*   **Robust Network Layer:**
    *   Dual-protocol client supporting standard **Stratum V1** and **CryptoNote** stratum variants.
    *   Secure **TLS/SSL encryption** with peer verification for protected pool connections.
    *   Automated pool failover logic and exponential reconnect backoff.
*   **Security Hardening:**
    *   Fully compliant **W^X (Write XOR Execute)** memory policies.
    *   JSON injection mitigations and strict boundary bounds-checking on dataset access.

---

## 🏎️ Performance Baseline

Measurements conducted on an **8× Cortex-A53 CPU (4×1.1 GHz + 4×1.4 GHz)** (Lenovo, MSM8929 /
Snapdragon 415, running postmarketOS — a genuine two-cluster big.LITTLE-shaped part; see
`ROADMAP.md`'s Baseline section for the cache-topology finding this corrected),
re-baselined 2026-07-24 (`PLAN.md` Phase 6 item 2 — see there for the full worker-count sweep,
two passes, thermal-settled between runs). Earlier figures in this table (5.18 H/s / 25.28 H/s,
"linear scaling") were stale and did not reproduce; see `docs/archived/plan_completed_phases_1-5.md`
Phase 5 for how that was found (PGO's claimed +19.3% didn't reproduce either — both PGO and non-PGO
measure identically on current code).

| Mode | Workers | Hashrate | Per-Core Efficiency | Notes |
|:---|:---:|:---:|:---:|:---|
| **Light mode JIT** | 1 | **4.27 H/s** | 4.27 H/s | Native hardware division & fast memory paths |
| **Light mode JIT (Pinned)** | 4 | **16.82 H/s** | 4.20 H/s | 98.5% scaling efficiency |
| **Light mode JIT (Pinned)** | 6 | **21.13 H/s** | 3.52 H/s | 82.5% scaling efficiency |
| **Light mode JIT (Pinned)** | 8 | **24.95 H/s** | 3.12 H/s | 73.0% scaling efficiency — **not linear**; efficiency declines smoothly with no plateau across 4→8, so 8 workers is still the highest-throughput choice on this device |
| **Interpreted Fallback** | 1 | 0.44 H/s | 0.44 H/s | Portable bytecode fallback — not re-measured this pass, ratio to JIT is approximate |

> [!TIP]
> Pinned execution (`--workers=8` on physical cores) avoids OS scheduling overhead, yielding higher throughput and lower variance than unpinned runs. There is no lower-worker-count "free lunch" on this device — going from 8 down to fewer workers trades real throughput for lower heat/power, it does not recover the same hashrate at a lower core count.

> [!TIP]
> **Deployment tuning, not shown in the table above**: on asymmetric multi-cluster ARM SoCs like this one, adding `isolcpus=<N>-<N> rcu_nocbs=<N>-<N>` (covering every core except core 0) to the kernel boot cmdline measured a reproducible **~28.4 H/s (+14%)** on this device — the biggest win this project has found, bigger than any code change. It's a root-only, reboot-required OS setting, so it can't be baked into `armrx` itself; see [`docs/experiments/isolcpus-rt-priority-win.md`](docs/experiments/isolcpus-rt-priority-win.md) for the full measurement and mechanism (background OS work was stealing cycles from pinned workers on the weaker cluster; isolation stops it).

> [!CAUTION]
> **If you set `isolcpus`, always pass `--workers=<N>` explicitly.** The default worker-count auto-detection (`std::thread::hardware_concurrency()`) reads the *calling process's own CPU affinity mask*, which `isolcpus` restricts new processes to (core 0 only) — so an un-flagged run silently mines on 1 core instead of all of them, losing far more than the 14% gained above. Confirmed live on this device; not yet fixed in code.
>
> **Even with `--workers=N` set correctly, the measured +14% mostly doesn't show up in real pool mining.** The isolcpus win above was measured with the built-in local benchmark (`--seconds=N`, no `--pool`) — no stratum/network overhead. In real pool mining, one worker still lands on core 0 (the only core `isolcpus` leaves for the OS/main thread), where it now competes with the stratum reader thread, JSON handling, and the once-a-second console print — overhead the benchmark never has. A full overnight pool run with `isolcpus` active and `--workers=8` sustained only ~24.76 H/s, matching the *pre-isolcpus* baseline, not the benchmarked 28.4 H/s. Not yet fixed in code; see [`docs/experiments/isolcpus-rt-priority-win.md`](docs/experiments/isolcpus-rt-priority-win.md).

---

## 🚀 Quick Start

### 1. Build from Source
Ensure CMake and a compatible C++20 compiler are installed.
```sh
# Generate build configuration and compile
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j

# Execute unit and integration tests
ctest --test-dir build --output-on-failure
```

### 2. Run Local Benchmarks
```sh
# Perform cache initialization benchmarks
./build/armrx --init-cache 'test key 000'

# Run indefinite local mining with steady-state telemetry
./build/armrx --mine --mode=light --workers=8 --seconds=0 --warmup=30
```

### 3. Connect to a Mining Pool
```sh
./build/armrx --pool=pool.example.com:3333 --wallet=<YOUR_MONERO_ADDRESS> --tls --workers=8
```

---

## 🛠️ CLI Reference

| Flag | Default | Description |
|:---|:---:|:---|
| `--mine` | *None* | Activates local benchmark mode. |
| `--mode` | `auto` | Memory allocation mode (`auto`, `light`, or `fast`). |
| `--workers` | *All Cores* | Number of execution threads. |
| `--difficulty` | `100` | Target difficulty for share generation. |
| `--seconds` | `10` | Run duration in seconds (`0` for infinite). |
| `--warmup` | `30` | Startup warmup delay in seconds before taking hashrate snapshots. |
| `--pool` | *None* | Stratum pool hostname and port. Can be specified multiple times for failover. |
| `--wallet` | *None* | Wallet address for pool submissions. |
| `--tls` | `false` | Enable secure TLS wrapper on pool connections. |
| `--metrics-port` | *Disabled* | Launch local Prometheus metrics endpoint on `127.0.0.1:{port}/metrics`. |
| `--tui` | `false` | Enable interactive terminal dashboard. |

---

## 🏛️ Codebase Status

Full progress and metrics are in [`RETROSPECTIVE.md`](RETROSPECTIVE.md).

*   **Blake2b/Argon2 Core Primitives:** ✅ Production-ready.
*   **NEON Direct Vector Mapping:** ✅ Loaded directly to pipeline registers.
*   **AArch64 JIT Engine:** ✅ Fully verified on physical target platforms.
*   **Security hardeners (S1–S8):** ✅ Fully active.
*   **Prometheus Metrics Server:** ✅ Operational.
*   **Emitter lookahead scheduler:** ✅ Adopted — small, measured, reproducible IPC win.
*   **PGO compiler profiles:** ⚠️ Tooling integrated into CMake and works end-to-end, but measured
    as a null on current code (re-confirmed twice, most recently 2026-07-25 after the scheduler
    landed) — not currently a performance win, kept for future re-evaluation.
*   **Stratum client state machine:** ✅ Stable with CryptoNote failover.
*   **NEON T-table AES AddRoundKey (Track G):** ✅ **+28.8% AES primitive throughput** (microbenchmark, σ ≤ 0.2%); E2E A/B (2026-08-01): **+1.68% H/s, −2.07% cycles, −4.93% instructions**. Enabled by default since 2026-08-01. See `docs/experiments/neon-ttable-aes.md`, `docs/experiments/track-g-e2e-ab.md`.
*   **Hardware AESE/AESD AES funnel (Item 1, 2026-08-03):** ✅ **Adopted as the default aarch64+crypto AES path.** `encrypt_transform`/`decrypt_transform` now use the **zero-key** `vaesmcq_u8(vaeseq_u8(s, zero))` / `vaesimcq_u8(vaesdq_u8(s, zero))` form (byte-identical to the T-table path; gated on `__ARM_FEATURE_AES`), replacing ~10.7M T-table instructions/hash with 3 NEON ops. Gated device A/B: **−16.7% instructions/hash (107.36M → 89.47M), −5.4% cycles/hash, −5.3% median hash time** — the largest code-level win in project history; armrx now below both the BSD reference (104.8M) and XMRig (94.5M) on instruction count. The 2026-07-20 "AESE incompatible" revert was a *direct* `aese(state,key)` form (AddRoundKey-first = wrong order); the zero-key compensation fixes it. See `docs/briefs/2026-08-03-hardware-aes-item1.md`.
*   **W4 phase-2 — superscalar C* literal pool (dedicated PC-relative region):** ✅ **Correct, full gate set PASS (2026-08-02).** `IADD_C*`/`IXOR_C*` load their immediate from a dedicated per-program PC-relative literal pool (1 `LDR` vs 2–3 `MOVZ/MOVN+MOVK+ALU`), closing phase-1's shared-region collision. Root-cause of the 15+ prior failure iterations: the offset formula's spurious `-8` (A64 `LDR (literal)` targets `k + off*4`, no `+8`) made every C* load hit the previous slot — masked by a circular self-check. Fixed + Luna's sign-extend fix → all 703 pooled ops resolve correctly. Hashrate latency-neutral on in-order A53 (pooled 4.32 vs baseline 4.27 H/s, 1 worker): instruction-count win with no throughput regression. See `changelogs.md` (2026-08-02) and `docs/briefs/w4-phase2-investigation-notes.md`.
*   **Hugepages verified on device (E9, 2026-08-03):** ✅ armrx's 256 MiB light-mode cache was confirmed running on **4 KiB pages** (`MAP_HUGETLB` failed — `HugePages_Total: 0`), falling back to anonymous + THP (which collapsed nothing). Reserved 256×2 MiB hugepages as root (`echo 256 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`); `/proc/<pid>/smaps` now shows `KernelPageSize: 2048 kB`. Live bench **+0.8% H/s (4.75 → 4.79)** — much smaller than XMRig's "up to 50%" headline because on this in-order A53 the bottleneck is the memory-latency wall (L1D 3cyc / L2 17cyc / DRAM 129ns, single port), not TLB misses. Free and correct; stays on. Plain `echo 256 > /sys/...` fails for non-root (shell opens the file as the user before sudo) — `sudo tee` is required.
*   **Dataset-derivation load-batching (E12, 2026-08-03):** 🔻 **NULL — precisely understood.** Tried to pipeline the superscalar-constant loads in `rx_calc_dataset_item` (XMRig batches them via `ldp`; armrx was serial). Rewrote to overlap loads via x12/x13; gates stay green (test_jit_equivalence 16/16, test_mining real shares) but **H/s unchanged (4.75, median 210.3 ms) and IPC 0.552 vs 0.554 baseline**. Root cause: the load stream is **dependency-bound, not load-port-bound** — each `eor` waits 3 cycles for its constant regardless of overlap; deeper overlap (x14–x17) is forbidden because those regs are live across the call in JIT code (prologue only saves x0–x13). The "simple load-batching" hypothesis is exhausted; the 0.551→0.648 IPC gap lives elsewhere (main-VM program emission — E13). Two implementation bugs found and fixed along the way: `adr`+`ldp` to the `.quad` literals segfaults (they sit outside the JIT `CodeSize` copy window; pooled `ldr` is required), and x14–x17 clobbering corrupts caller-live regs. See `docs/experiments/next-iteration-plan.md` (E11/E12/E13).
*   **IPC-gap localization (E13, 2026-08-03):** 🔻 **Gap found in the `*_M` scratchpad path; `*_M` is a solved-unknown, not a wall.** Captured the live JIT buffer from a real-mining run, disassembled, and classified every `ldr`/`ldp` by base register + immediate-consumer rate: **scratchpad (x2) loads are 64.3% serial** (`ldr x2 → immediate consume`), while dataset (x1/x20) and NEON/AES paths are **already pipelined**. The 0.551→0.648 IPC gap to XMRig therefore lives in the **scratchpad memory-op emission** — exactly the **`*_M` memory-op scheduler region** AGENTS.md records as having **diverged and been reverted (mechanism not identified)**. That makes `*_M` a *solved-unknown*: RandomX scratchpad ops are `read → transform → write back` with the transform depending on the loaded value, so a naive reorder likely tripped an address-aliasing / PC-coupling / AES-state constraint that was never root-caused. **Ground truth:** a compiled `xmrig-dev` on this device gives **1-thread IPC 0.628** (perf, core 3) vs armrx 0.551 — the ~14% per-cycle gap is real. **But the real-pool like-for-like test (2026-08-03) shows the IPC gap is a *phantom real-world deficit*:** 8-worker non-isolated, armrx = **21-23 H/s** vs XMRig = **27.77 H/s** (~18-23% behind); armrx WITH isolcpus = ~28.4 H/s (== XMRig). E15 investigated the two obvious non-isolated causes (hugepages, affinity) and **ruled both out** (armrx already pins correctly and already acquires hugepages as root, yet stays ~18% behind) — so the residual gap's cause is **open** (likely real-program hash-loop efficiency or 8-worker threading overhead, not yet measured). **E14** (root-cause `*_M`) remains de-prioritized. The `jit_compiler_a64_static.S` E12 edit is retained as a documented dead-end reference. See `docs/experiments/next-iteration-plan.md` (E13/E14/E15).
*   **Real-pool non-isolated gap (E15, 2026-08-03):** 🔻 **~18-23% gap found, BOTH hugepage/affinity hypotheses ruled out — localized to MULTI-WORKER SCALING.** Like-for-like (8w, no isolcpus): armrx **21-23 H/s** vs XMRig **27.77 H/s**; armrx isolcpus = **~28.4 H/s** (= XMRig). Ruled out: (1) **hugepages** — as root armrx gets 128×2 MiB via `MAP_HUGETLB` (HugePages_Free 256→128) yet only 22.9 H/s (+1.5); armrx's `virtual_memory.c` already uses XMRig's exact technique; (2) **affinity** — default `AffinityMode::All` pins 1:1 to `core_order_` (0..7 fallback on no-cpufreq, coincidentally correct). **Decisive per-core test:** 1 worker core 3 = **4.08 H/s** vs XMRig 4.53 (**~90%** — per-core hash loop is FINE, the known IPC phantom). The real gap is **multi-worker scaling**: armrx 1w→8w scales only **5.4×** (8×4.08=32.6 ideal, actual 22 = 67% of linear) vs XMRig **6.1×**. So ~33% of armrx's per-core potential is lost to 8-worker contention/placement on the two-cluster interconnect. **E16** = attack multi-worker scaling (find which workers under-perform + whether fill-thread/handshake contention or missing fast/weak awareness is the cause). No code change for E15 (localization only). Earlier "2× per-core" claim retracted (4w run read un-settled).

*   **isolcpus-aware worker pinning (audit T2-3):** ✅ Workers pin exclusively to isolated cores when `isolcpus=` is active — on all `detect_core_order()` paths including the cpufreq-less fallback. Default worker count caps to isolated-core count. On-device verified: 7 workers on cores 1-7, main/housekeeping thread on core 0, no contention. Recovers the ~14% isolcpus win for pool mining. Since 2026-08-01.
*   **Cross-hash boundary pipelining (Track D2):** ✅ Overlaps AES finalization of hash N with AES fill of hash N+1 via interleaved read/write function. Verified correct (mining KATs) and live in the mining path; on-device A/B (2026-08-01): interleaved −2.77% AES time (IPC 1.735→1.787), ~0.34% E2E. See `docs/experiments/t11-d2-microbenchmark.md`.
*   **Hybrid partial dataset (Track B):** ⚠️ Landed but **not adopted for production** — `--dataset-mb=N`
    caches a prefix of the fast-mode dataset for direct JIT loads instead of on-the-fly derivation.
    JIT `_end_hybrid` entry point with incremental background fill. Verified differential-correct
    (100% byte-identical) across 20 seeds. Gate B (memory contention) measured **−31% at 8 workers**
    on this device (baseline 24.68 → hybrid 17.04 H/s) — the extra DRAM traffic saturates the
    two-cluster interconnect. Code stays in the tree gated behind `--dataset-mb=N` (default 0, zero
    cost when off) as reference for future targets with better memory bandwidth.

---

## 📚 Documentation

| Doc | What it's for |
|---|---|
| [`RETROSPECTIVE.md`](RETROSPECTIVE.md) | Full project retrospective — 210 commits, 18 days, 10 performance tracks. Start here. |
| [`ROADMAP.md`](ROADMAP.md) | Post-alpha status — points to RETROSPECTIVE.md for the full story. |
| [`changelogs.md`](changelogs.md) | Post-alpha changelog — alpha record preserved at `docs/archived/alpha-changelogs.md`. |
| [`docs/audits/combined-audit-20260731.md`](docs/audits/combined-audit-20260731.md) | Consolidated next-steps audit from AGY (Gemini) + Reasonix (DeepSeek). Prioritized T0–T3. |
| [`docs/archived/audits/`](docs/archived/audits/) | Archived alpha-phase audits — correctness, security, performance reviews. |
| [`docs/experiments/`](docs/experiments/) | Measured performance attempts — both adopted wins and honest, documented reverts. |
| [`docs/archived/plans/`](docs/archived/plans/) | Archived performance plans — all tracks A–J completed or closed. |
| [`docs/plans/20260727/master-plan-20260727.md`](docs/plans/20260727/master-plan-20260727.md) | Still-active strategic master plan with ranked priorities. |
| [`docs/postmortems/`](docs/postmortems/) | Root-cause writeups for past critical bugs (dataset corruption, pool-failover deadlock, AES T-table). |
| [`docs/archived/`](docs/archived/) | Superseded material — completed-phase narratives and alpha archives. |

`RETROSPECTIVE.md` carries the full narrative for every phase. The master plan at
`docs/plans/20260727/master-plan-20260727.md` is the still-active reference for any future restart.
Archived phase narratives are in `docs/archived/plan_completed_phases_1-5.md` (Phases 1–5),
the full why-and-how behind everything already shipped.
