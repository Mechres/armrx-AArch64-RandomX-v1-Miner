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

Full progress, metrics comparisons, and future tasks are tracked in [ROADMAP.md](ROADMAP.md) and [PLAN.md](PLAN.md).

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
*   **NEON T-table AES AddRoundKey (Track G):** 🧪 **+28.8% AES primitive throughput** (microbenchmark, σ ≤ 0.2%), projected ~3.6% full-workload gain. Gated behind `-DARMRX_ENABLE_NEON_TTABLE_AES=ON` (default OFF), experimental. See `docs/experiments/neon-ttable-aes.md`.
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
| [`PLAN.md`](PLAN.md) | Live master plan — genuinely open work only. Start here for "what's next." |
| [`ROADMAP.md`](ROADMAP.md) | Status tracker — completed/remaining item tables, hardware baseline. |
| [`NEXT_STEPS.md`](NEXT_STEPS.md) | Short-list actionable view of the current phase. |
| [`changelogs.md`](changelogs.md) | Chronological, dated record of every change and why it was made. |
| [`docs/audits/`](docs/audits/) | Correctness, security, and performance audits (internal and third-party). |
| [`docs/experiments/`](docs/experiments/) | Measured performance attempts — both adopted wins and honest, documented reverts. |
| [`docs/plans/`](docs/plans/) | Forward-looking performance plans: [gated plan](docs/plans/performance-plan-20260725.md) and [speculative backlog](docs/plans/experimental-performance-ideas-20260725.md), if performance work resumes. |
| [`docs/postmortems/`](docs/postmortems/) | Root-cause writeups for past critical bugs (dataset corruption, pool-failover deadlock, AES T-table). |
| [`docs/archived/`](docs/archived/) | Superseded material — completed-phase narratives ([Phases 1–5](docs/archived/plan_completed_phases_1-5.md), [Phase 6](docs/archived/plan_phase6_completed.md), [Phase 7](docs/archived/plan_phase7_completed.md)) and old plans. |

`PLAN.md` used to carry the full narrative for every completed phase inline; once a phase's
narrative grows past a few hundred lines it gets split into `docs/archived/`, keeping `PLAN.md`
itself focused on what's actually still open. Read `PLAN.md` for what's open, the archives for
the full why-and-how behind everything already shipped.
