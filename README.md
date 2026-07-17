# armrx — AArch64 RandomX v1 Miner

[![AArch64](https://img.shields.io/badge/arch-AArch64_ARMv8--A-blue)]()
[![C++20](https://img.shields.io/badge/c%2B%2B-20-00599C)]()
[![test vectors](https://img.shields.io/badge/test%20vectors-passing-brightgreen)]()

Clean-room, CPU-only Monero RandomX v1 miner for AArch64 Linux, implemented
against the [public RandomX specification](https://github.com/tevador/RandomX).
Zero borrowed miner source code.

---

## Quick Start

```sh
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

On AArch64 the build automatically enables hardware AES/NEON
(`-march=armv8-a+crypto`), compiles the JIT backend, and sets
`ARMRX_HAVE_JIT=1`. On x86_64 the JIT is excluded and the VM falls back to
the interpreted loop — no code changes needed.

### Cross-compilation

Provide an AArch64 CMake toolchain file and leave `ARMRX_ENABLE_NATIVE` off.

---

## Usage

### 1. Cache Benchmark
```sh
./build/armrx --init-cache 'test key 000'
```

### 2. Local Mining Benchmark
```sh
./build/armrx --mine --mode=auto|light|fast --workers=N --difficulty=D --seconds=S
```

| Flag | Default | Description |
|------|---------|-------------|
| `--mine` | — | Run local benchmark |
| `--mode` | `auto` | Memory strategy: `auto`, `light`, or `fast` |
| `--workers` | all cores | Thread count |
| `--difficulty` | `100` | Target share difficulty |
| `--seconds` | `10` | Duration (`0` = indefinite) |

### 3. Pool Mining (Stratum V1 / CryptoNote)
```sh
./build/armrx --pool=pool.example.com:3333 --wallet=<YOUR_WALLET> [--password=x] [--mode=auto] [--workers=N]
```

Supports **standard Stratum V1** and automatically falls back to the **CryptoNote protocol** (required by pools like `herominers.com`). Most pools use TLS — enable with `--tls`.
Disconnects are **automatically retried** with exponential backoff (1s → 2s → … → 30s max, 5 retries then failover).
Multiple pools can be specified for automatic failover: `--pool=A:1111 --pool=B:1111`.

---

## Architecture

### Interpreted VM

The clean-room AArch64 interpreted VM matches the reference implementation
by resolving five subtle design decisions:

| # | Decision | Detail |
|---|----------|--------|
| 1 | **AES inverse round** | `aesd` matches `InvShiftRows → InvSubBytes → InvMixColumns → AddRoundKey`; round-key XOR must happen *after* the inverse transforms |
| 2 | **Word mapping** | `build_aes_block` follows strict little-endian layout matching `rx_set_int_vec_i128` |
| 3 | **Scratchpad seeding** | `init_scratchpad` modifies `tempHash` in-place; first program runs with the modified seed |
| 4 | **Frequency thresholds** | Cumulative opcode ceilings match standard RandomX v1 frequencies exactly |
| 5 | **Zero-init registers** | Integer registers zeroed on VM setup to prevent cross-run residue |

### JIT Backend (AArch64 only)

Hardware AES/NEON intrinsics + a runtime code generator that emits AArch64
machine code directly. Verified on real hardware (postmarketOS, GCC 15.2.0,
ARMv8-A + crypto). Falls back to the interpreted loop on other architectures.

### Memory Modes

| Mode | Shared Memory | Per Worker | Use Case |
|------|--------------|------------|----------|
| **Light** | 256 MiB cache | 2 MiB scratchpad | Fits on 2 GiB devices; derives dataset on-demand |
| **Fast** | 2080 MiB dataset | 2 MiB scratchpad | Full speed; needs ≥3 GiB available RAM |

The startup probe uses Linux `MemAvailable` (cgroup-aware), reserves 256 MiB
for the OS, and selects fast mode only when everything fits.

### Stratum V1 Client

TCP connection to any Monero-compatible pool with:
- `mining.subscribe` / `mining.authorize` handshake
- `mining.notify` job dispatch (blob parsing, target extraction)
- `mining.set_target` / `mining.set_difficulty` updates
- `mining.submit` share submission
- **Auto-reconnect** with exponential backoff on disconnect

---

## Status

| Component | Status |
|-----------|--------|
| BLAKE2b + Argon2-compatible H' | ✅ |
| AES primitives, AesGenerator1R/4R | ✅ |
| Argon2d cache init + dataset generation | ✅ |
| SuperscalarHash generation/execution | ✅ |
| Interpreted VM (register file, bytecode, scratchpad, final hash) | ✅ |
| Multi-threaded worker pool + target comparison + mode selection | ✅ |
| AArch64 JIT backend (ASM + JIT compiler + virtual memory) | ✅ verified on hardware |
| Stratum V1 / CryptoNote client (subscribe, authorize, notify, submit) | ✅ |
| Auto-reconnect with backoff | ✅ |
| TLS/SSL pool connections | ✅ |
| Multi-pool failover | ✅ |
| Config file (`~/.config/armrx/config.json`) | ✅ |
| TUI dashboard (`--tui`) | ✅ |
| CPU affinity + per-worker H/s counters | ✅ |
| NEON SIMD SuperscalarHash | ✅ |
| JIT loop alignment + prefetch + NEON loads | ✅ |
| **Security hardening (S1–S5, S8)** | ✅ |
| **Always-on assertions (`ARMRX_ASSERT`)** | ✅ |
| **JSON injection protection** | ✅ |
| **Dataset OOB read guard** | ✅ |
| **JIT W^X compliance** | ✅ |
| **KATs in both JIT + interpreted mode** | ✅ |
| **T-table AES fallback** | ✅ |
| **Rounding mode cache** | ✅ |
| **`alignas(16)` RegisterFile** | ✅ |
| **ASan/UBSan CMake options** | ✅ |
| **`bench_armrx` in CTest** | ✅ |
| **VM refactor: `is_fast_mode()`** | ✅ |
| **VM refactor: `run()` split** | ✅ |
| **VM refactor: dispatch table** | ✅ |
| **`armrx::json` module** | ✅ |

### Performance

Tested on **8× Cortex-A53 @ ~1.2 GHz** (Lenovo MSM8916, postmarketOS):

| Mode | Hashrate |
|------|----------|
| **Light, 8 workers** | **~22 H/s** (big: 3.58/core, LITTLE: 1.86/core) |
| Light, 4 workers (big only) | ~14 H/s |
| Fast mode | Requires ≥2.3 GiB available RAM |

Reference XMRig on same hardware: ~27 H/s. See [OPTIMIZATION_REFERENCE.md](OPTIMIZATION_REFERENCE.md) for the full optimization history.

### Reference Test Vectors
```
Input1: 639183aae1bf4c9a35884cb46b09cad9175f04efd7684e7262a0ac1c2f0b4e3f
Input2: 300a0adb47603dedb42228ccb2b211104f4da45af709cd7547cd049e9489c969
```

---

## 2 GiB Devices

Fast mode needs a 2080 MiB shared dataset plus per-worker scratchpads plus OS
overhead — it cannot fit on 2 GiB. Use **light mode** (`--mode=light` or let
`--mode=auto` choose automatically). The auto-probe checks `MemAvailable`,
respects cgroup limits, and selects light mode when the 2080 MiB dataset + 2 MiB
per worker + 256 MiB OS reserve doesn't fit.

