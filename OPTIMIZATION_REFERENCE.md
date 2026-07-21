# armrx Optimization Reference

## Hardware Target

**Device:** Lenovo MSM8916 (Snapdragon 410), postmarketOS, Linux 6.12.1
**CPU:** 8× Cortex-A53 @ ~1.2 GHz (big: 3.58 H/s per core, LITTLE: 1.86 H/s per core)
**RAM:** 2 GiB total (~1.3 GiB available)
**Compiler:** GCC 15.2.0 (Alpine/musl)

## Final Hashrate: ~22 H/s (vs XMRig ~27 H/s)

---

## Optimizations Tried

### ✅ WORKED — Applied and kept

| Optimization | Gain | Source | Files changed |
|-------------|------|--------|---------------|
| **JIT cache size fix** (2 GiB → 256 MiB) | **15×** (1.2→21 H/s) | Bug fix | `jit_compiler_a64.cpp` |
| **Light-mode JIT enabled** | Core fix | Our analysis | `vm.cpp` |
| **CryptoNote protocol** | Pool compatibility | Pool requirement | `stratum_client.cpp/hpp` |
| **TLS/SSL** | Pool compatibility | User requirement | `tls_client.cpp/hpp`, `CMakeLists.txt` |
| **NEON SIMD SuperscalarHash** | +56% dataset thpt | Gemini | `superscalar.cpp/hpp`, `dataset.cpp` |
| **Argon2d cache huge pages** (mmap) | +8.5% (21.2→23.0) | Gemini | `argon2.cpp/hpp` |
| **LTO (Link-Time Optimization)** | -48% test time | Standard practice | `CMakeLists.txt` |
| **Buffer size 4608→6144 slots** | Stability | XMRig source | `jit_compiler_a64_static.S` |
| **Dataset prefetch pldl1keep→pldl1strm** | Marginal | XMRig source | `jit_compiler_a64_static.S` |
| **Scratchpad prefetch (start + end of loop)** | Marginal | jit_plan.md | `jit_compiler_a64_static.S` |
| **Loop alignment (.p2align 5)** | Stability | Gemini | `jit_compiler_a64_static.S` |
| **ubfx for spAddr** | Marginal | jit_plan.md | `jit_compiler_a64_static.S` |
| **FSWAP_R ext instruction** | Marginal | jit_plan.md | `jit_compiler_a64.cpp` |
| **NEON ld1+sxtl for FP loads (emitMemLoadFP)** | Marginal | jit_plan.md | `jit_compiler_a64.cpp` |
| **mmap scratchpad** (std::vector→mmap) | Marginal | Analysis | `vm.hpp`, `vm.cpp` |
| **Lock-free job counter** | Marginal | Analysis | `mining_engine.hpp/cpp` |
| **CPU affinity pinning** | Marginal | Analysis | `mining_engine.cpp` |
| **Per-worker hash counters + TUI bars** | Observability | User request | `mining_engine.hpp/cpp`, `tui.cpp/hpp` |
| **Auto-reconnect + pool failover** | Reliability | User request | `stratum_client.cpp/hpp`, `main.cpp` |
| **Config file** | UX | User request | `config.cpp/hpp`, `main.cpp` |
| **TUI dashboard** (--tui) | UX | User request | `tui.cpp/hpp`, `main.cpp` |
| **JIT prologue instruction scheduling (O12)** | +0.33% hashrate, −439M cycles | Gemini | `jit_compiler_a64_static.S` |
| **JIT register-offset FP loads (O13)** | −56M instructions | Gemini | `jit_compiler_a64.cpp` |
| **Profile-Guided Optimization (PGO)** | +0.8% hashrate, −6.4B instructions, −10.7B cycles | CMake options | `CMakeLists.txt` |

### ❌ FAILED — Did not work

| Optimization | Attempt | Why it failed |
|-------------|---------|---------------|
| **Newton-Raphson FDIV/FSQRT** | `-DARMRX_ENABLE_JIT_FAST_DIV_SQRT=ON` | Segfault — instruction encodings verified correct but x29 register gets corrupted at runtime. Root cause unclear (possibly pipeline interaction on Cortex-A53 in-order). Upstream RandomX also uses native `fdiv`/`fsqrt`. |

| **-Ofast / -ffast-math** | `-DARMRX_FAST_MATH=ON` | No measurable change. RandomX FP operations are already efficient. |

### ⏸️ DEFERRED — Could work with more effort

| Optimization | Est. gain | Why deferred |
|-------------|-----------|-------------|
| **A5: Load interleaving** (reorder ldp/eor) | +2-3% | Blocked by register pressure — all temp registers in use, can't issue ldps back-to-back without overwriting. Would need major register reallocation. |
| **C2: Dataset prefetch A/B test** | ~±0.5% | Quick to test but impact uncertain. |
| **hwloc topology detection** | +2-5% | Not the bottleneck — per-core gap to XMRig is same across all core types. |
| **Per-component profiling** | Diagnosis | Would identify exact gap locations but requires iterative cycle-by-cycle analysis. |
| **Cross-instruction register coalescing** | Unknown | Major engineering effort — requires adding a peephole pass over the JIT program. |

---

## Performance Analysis (perf)

### armrx vs XMRig on same hardware (light/slow mode)

| Metric | armrx | XMRig | Gap |
|--------|-------|-------|-----|
| **Hashrate** | ~22 H/s | ~27 H/s | -18% |
| **Instructions** | 64.3B | 48.2B | +33% |
| **Cycles** | 78.5B | 75.0B | +5% |
| **IPC** | 0.819 | 0.642 | Higher (more work) |
| **L1-dcache misses** | 227M | 280M | -19% |
| **Branch misses** | 152M | 11M | +13× |

**Key insight:** armrx executes 33% more instructions per benchmark than XMRig. This is the primary performance gap. The extra instructions are distributed across the entire pipeline (JIT body, main loop, Blake2b, AES) — not concentrated in one handler. Closing this gap would require an iterative cycle-by-cycle profile comparison.

---

## What XMRig Does Differently (from source analysis)

We matched XMRig's `xmrig-dev` branch on all measurable parameters:

| Parameter | Ours | XMRig's dev |
|-----------|------|-------------|
| JIT buffer size | 6144 slots | 6144 slots ✅ |
| Dataset prefetch hint | `pldl1strm` | `pldl1strm` ✅ |
| FMUL_R, FADD_R, FSUB_R handlers | 1 inst | 1 inst ✅ |
| Static ASM instruction count | 343 | 325 (mostly our additions) ✅ |

The remaining gap is from distributed codegen differences — years of iterative profiling that XMRig's developers have accumulated. Not a single fix, but many small improvements across the entire pipeline.

---

## Build Flags Reference

```sh
# Standard build (recommended)
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON

# With experimental NR FDIV/FSQRT (may crash)
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON -DARMRX_ENABLE_JIT_FAST_DIV_SQRT=ON

# With extra profiling (compile vs execute breakdown)
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON -DARMRX_JIT_PROFILE=ON
```

## CLI Reference

```sh
./build/armrx --pool=host:port --wallet=ADDRESS    # Pool mining
./build/armrx --pool=host:port --wallet=ADDR --tls  # TLS pool
./build/armrx --pool=A:3333 --pool=B:4444          # Multi-pool failover
./build/armrx --mine --mode=light --workers=4       # Local benchmark
./build/armrx --tui                                  # Terminal UI dashboard
./build/armrx --init-cache 'key'                    # Cache benchmark
./build/armrx --help                                 # Full options
```

## Performance on Reference Hardware

| Config | Hashrate | Notes |
|--------|----------|-------|
| Light mode, 8 workers (all cores) | ~22 H/s | 4 big × 3.58 + 4 LITTLE × 1.86 |
| Light mode, 4 workers (big cores) | ~14 H/s | Better efficiency |
| Fast mode | N/A | Requires ≥2.3 GiB RAM |
