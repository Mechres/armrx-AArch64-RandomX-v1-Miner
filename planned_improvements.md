## Performance Optimizations

### Profile: Current bottleneck analysis

XMRig on same hardware (8× Cortex-A53, light/slow mode) achieves **27 H/s**.
Our miner achieves **~1.2-1.9 H/s** — a **14-22× gap**. Root causes identified:

| Factor | Est. contribution | Evidence |
|--------|-------------------|----------|
| `generateProgramLight()` never called (fixed) | ~2× | 0.83 → 1.9 H/s |
| Cache pointer null in light-mode JIT (fixed) | TBD | memory was nullptr |
| JIT recompilation per hash | TBD | Needs profiling |
| ASM dataset stubs not resolving | TBD | Needs verification |
| Remaining after fixes | TBD | Test on device |

### 1. Scratchpad — Huge Pages (2 MiB THP) ✅
**Complete.** `madvise(MADV_HUGEPAGE)` applied after each 2 MiB scratchpad allocation in `VirtualMachine` constructor. The kernel promotes the pages to 2 MiB transparent huge pages, reducing TLB pressure on AArch64.

### 2. Parallel Dataset Generation (Fast Mode) ✅
**Already implemented.** `MiningEngine::set_job()` in `src/mining_engine.cpp` splits the 32M-item range across `hardware_concurrency()` threads, each calling `initialize_dataset()` on a disjoint sub-range. All cache/program access is read-only, so no synchronization needed.

### 3. NEON SIMD for SuperscalarHash
The SuperscalarHash inner loop in [`src/superscalar.cpp`](file:///home/mechres/Projeler/aarch64-randomx/src/superscalar.cpp) executes a simulated 4-issue superscalar pipeline. AArch64 NEON can vectorize many of the multiply/add operations across multiple items simultaneously.

### 4. CPU Affinity Pinning ✅
**Complete.** Each worker thread is pinned to `thread_id % hardware_concurrency()` via `pthread_setaffinity_np` in `MiningEngine::worker_loop()`. Eliminates core migration overhead.

### 5. NUMA-Aware Allocation
On multi-socket AArch64 servers, allocating the cache and dataset from NUMA-local memory (via `mbind`/`numa_alloc_onnode`) avoids cross-socket memory latency.

---

## 🚀 Light-Mode Speed (22× gap to XMRig)

### 6. Profile: JIT Compilation Overhead per Hash
The JIT generates 8 new programs per hash (one per VM chain), each compiling ~256 RandomX instructions to AArch64 machine code *plus* the `calc_dataset_item` inline assembly stubs. **Hypothesis:** this compilation takes significant CPU time. **Fix:** add micro-timing around `jit_->generateProgramLight()` in `run()` to measure overhead per hash. If ≥10ms, caching programs across hashes (when seed is stable, ~2 min on pool) would help.

### 7. Profile: Dataset Item Derivation Rate
In light mode, each hash needs 16,384 dataset items (8 programs × 2048 iterations). Measure how many items/sec the JIT's ASM stubs actually produce vs the C++ `generate_dataset_item()` path. If the JIT path is slower, the ASM stubs may have a bug or misaligned code layout.

### 8. Verify ASM Stub Execution
The `randomx_calc_dataset_item_aarch64` ASM routines in `jit_compiler_a64_static.S` are called from JIT-generated code. Verify they execute and produce correct items. Add a counter in `run()` or compare hash output between JIT light mode and interpreted light mode on a known input.

### 9. JIT Program Caching
Currently, each hash generates 8 new programs. If the RandomX seed key hasn't changed (typically stable for ~120s on pool), the SuperscalarHash programs are identical across hashes. **Caching** them in an LRU map keyed by (seed_height, block_template_hash) would eliminate 99% of JIT recompilation. This is the highest-impact single optimization.

### 10. Scratchpad: `mmap` instead of `std::vector`
`std::vector<std::byte>` allocates via `new` → `malloc`. Using `mmap(MAP_ANONYMOUS | MAP_PRIVATE)` with `MAP_HUGETLB` or `MADV_HUGEPAGE` gives direct control over page size and alignment. XMRig uses this approach. May reduce TLB misses beyond `MADV_HUGEPAGE` alone.

### 11. Worker Loop: Reduce Lock Contention
Each hash acquires `job_mutex_` to check for job updates and copy the active job. For a pool with stable jobs, this lock is almost never contended. Switching to a `std::atomic<uint64_t>` job generation counter + lock-free job pointer swap would eliminate the mutex entirely.

### 12. Multi-Issue Superscalar Execution
`execute_superscalar()` in `src/superscalar.cpp` simulates a 4-issue pipeline in software. AArch64 NEON can vectorize these operations for parallel item derivation.

### 13. `CalcDatasetItemSize` Code Layout Optimization
The `randomx_calc_dataset_item_aarch64` assembly blocks are embedded in the JIT executable code region. Their size (`CalcDatasetItemSize`) may be misaligned or larger than optimal, wasting I-cache. Profile with `perf stat` to check I-cache miss rate.

### 14. Argon2 Cache Init Parallelism
Cache initialization uses a single-threaded Argon2d hash. Splitting across threads (Argon2 supports up to 4 lanes) could reduce startup time.

### 15. Benchmark Each Component in Isolation
Create micro-benchmarks for:
- Cache line read bandwidth (MiB/s)
- SuperscalarHash item derivation (items/s)
- Scratchpad fill/hash (MiB/s)
- JIT compilation time (μs/program)
- Interpreted loop throughput (instructions/s)
Compare against XMRig's perf numbers to identify remaining gaps.

---

## 🔧 JIT Compiler

### 6. Verify JIT Correctness on Real Hardware ✅
**Complete.** Built and tested on real AArch64 hardware (Lenovo/MSM8916, postmarketOS, GCC 15.2.0). All RandomX reference test vectors pass with `ARMRX_HAVE_JIT=1`. Build required adding three new source files (`instruction_weights.hpp`, `configuration.h`, `soft_aes.cpp`) and fixing upstream constant references — see changelog for details.

### 7. JIT Dataset Item Generation
`JitCompilerA64::generateDatasetInitProgram` (in the upstream ASM stubs) can JIT-compile the dataset item loop too — this is what gives RandomX miners their full fast-mode speed advantage.

### 8. Literal Pool Tuning
The JIT uses a 32-bit literal pool for `IMUL_RCP` constants. On very long programs this can fill up; adding a second literal page or switching to PC-relative `ADRP+ADD` addressing eliminates the overflow risk.

---

## 🌐 Stratum & Network

### 9. TLS/SSL Support ✅
**Complete.** Optional OpenSSL-based TLS wrapping in `src/tls_client.cpp`. Enabled via `--tls` flag (off by default). `find_package(OpenSSL)` in CMake — silently disabled when OpenSSL is not installed. RAII `TlsClient` wrapper handles `SSL_connect`, SNI, `SSL_read`/`SSL_write`, and clean shutdown.

### 10. Auto-Reconnect with Backoff
The current client disconnects permanently on socket error. An exponential backoff reconnect loop (1 s → 2 s → 4 s → 30 s max) with automatic re-subscription and re-authorization makes the miner production-grade.

### 11. Multiple Pool Failover ✅
**Complete.** Multiple `--pool=host:port` flags accepted. On permanent disconnect (5 retries exhausted), main.cpp cycles to the next pool in the list with a 2s cooldown.

### 12. Stratum V2 (Binary Protocol)
The Stratum V2 protocol (used by p2pool and newer pools) provides encrypted channels, individual job assignment per worker, and reduced bandwidth. It's a larger undertaking but gives miners more privacy and pool operators better load balancing.

---

## 📊 Observability & Operations

### 13. Per-Worker Hash Rate ✅
**Complete.** Per-worker `std::atomic<uint64_t>` counters with periodic flush from local accumulators. Exposed via `MiningEngine::worker_hash_rate(thread_id)`.

### 14. Accepted / Rejected Share Counters
Tracking pool-accepted vs rejected shares with timestamps allows computing your real effective difficulty and catching share submission bugs.

### 15. HTTP Stats Endpoint
A minimal `SO_REUSEPORT` HTTP server (one extra thread, ~100 lines) serving a JSON metrics page makes the miner easy to integrate with Prometheus / Grafana dashboards.

---

## 🏗️ Build & Deployment

### 16. Config File Support
CLI-only configuration is fine for testing but a TOML or JSON config file (`~/.armrx.conf`) is far more convenient for production deployments and makes secrets (wallet address) easier to manage.

### 17. systemd Service Unit
A templated `armrx@.service` file with `Restart=always` and CPU affinity directives makes the miner trivially deployable as a system service.

### 18. GitHub Actions CI for AArch64
QEMU-based cross-compilation + test runs (`runs-on: ubuntu-latest` + `qemu-user-static`) would give you automated test coverage on every commit without needing physical hardware.

---

## Priority order (if I were to rank them):

| Priority | Item | Impact | Effort |
|---|---|---|---|
| ~~✅ 🔴 **1**~~ | ~~Verify JIT on real AArch64 hardware (done)~~ | | |
| ~~🔴 **2**~~ | ~~Auto-reconnect with backoff (done)~~ | | |
| ~~🟡 **4**~~ | ~~Huge pages for scratchpads (done)~~ | | |
| ~~🟡 **2**~~ | ~~Parallel dataset generation (already implemented)~~ | | |
| ~~🔴 **1**~~ | ~~TLS/SSL pool connections (done)~~ | | |
| ~~🟡 **2**~~ | ~~CPU affinity pinning (done)~~ | | |
| ~~🟢 **3**~~ | ~~Per-worker H/s counters (done)~~ | | |
| ~~🟢 **4**~~ | ~~Multiple pool failover (done)~~ | | |
| 🔴 **1** | JIT program caching (seed-stable, eliminates 99% of recompilation) | Speed | Medium |
| 🔴 **2** | Profile JIT compilation overhead per hash | Diagnosis | Low |
| ~~✅ 🔴 **3**~~ | ~~Verify ASM dataset stubs execute correctly (done)~~ | | |
| 🟡 **4** | Scratchpad via mmap instead of vector | Speed | Low |
| 🟡 **5** | Worker loop: lock-free job pointer | Speed | Low |
| 🟡 **6** | Benchmark each component in isolation | Diagnosis | Medium |
| 🟢 **7** | Config file | UX | Medium |
| ⚪ **8** | Stratum V2 | Future-proofing | High |

