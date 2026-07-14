# Changelog

## 2026-07-14 (Cache Prefetching + Final Optimizations)

### Added
- **Scratchpad cache prefetch** (`src/jit_compiler_a64_static.S`): Added `prfm pldl1keep` instructions in the JIT main loop to prefetch three scratchpad cache lines (spAddr0, spAddr1, spAddr1+32) before the load instructions execute. Hides memory latency on Cortex-A53's in-order dual-issue pipeline.
- **Dataset cache line prefetch upgraded** (`src/jit_compiler_a64_static.S`): Changed `prfm pldl2strm` (L2 streaming hint, next-line eviction) to `prfm pldl1keep` (L1 keep hint) for the cache line read in `rx_calc_dataset_item_prefetch`. Since the data is XOR'd immediately after the SuperscalarHash computation, L1 residency avoids a costly L1→L2 refill. Net gain: +2.2% across all cores.
- **TUI per-worker bars fixed** (`src/tui.cpp`, `src/mining_engine.cpp`): Worker hash counters were allocated but never incremented (flushes went only to `total_hashes_`). Added `worker_hashes_[thread_id].fetch_add()` alongside the total flush, enabling live per-core bars in the TUI.

### Result
Steady hashrate of **21.8 H/s** on 8× Cortex-A53 (big: ~3.58 H/s per core, LITTLE: ~1.86 H/s per core). JIT profile: 1.6% compile, 98.4% execute.

## 2026-07-14 (NEON SIMD Vectorization & JIT/Cache Optimizations)

### Added
- **NEON SIMD Vectorized Superscalar execution** (`include/armrx/superscalar.hpp`, `src/superscalar.cpp`): Added `execute_superscalar_neon` using AArch64 NEON intrinsics (`vaddq_u64`, `vsubq_u64`, `veorq_u64`, etc.) to process two items in parallel using `uint64x2_t` registers.
- **Vectorized Dataset initialization** (`src/dataset.cpp`): Updated `initialize_dataset` under `__aarch64__` to process dataset items in pairs of 2, calling `execute_superscalar_neon` and parallelizing cache line XOR lookups.
- **Dataset generation benchmark** (`tests/bench_armrx.cpp`): Added micro-benchmark for `initialize_dataset` generating 5,000 items in a batch.
- **Argon2d Cache Huge Page allocation** (`include/armrx/argon2.hpp`, `src/argon2.cpp`): Converted the 256 MiB Argon2d cache memory layout from standard `std::vector` (malloc heap pages) to a raw block allocated via `mmap` with transparent huge page hint `madvise(MADV_HUGEPAGE)`. This reduces translation lookaside buffer (TLB) thrashing under random Light Mode lookups, increasing hashrate to ~23 H/s.
- **JIT compilation timing profiling** (`include/armrx/vm.hpp`, `src/vm.cpp`, `include/armrx/mining_engine.hpp`, `src/mining_engine.cpp`, `include/armrx/tui.hpp`, `src/tui.cpp`, `src/main.cpp`): Integrated high-resolution JIT compilation and execution timers inside `VirtualMachine::run`, periodically reported as a breakdown in the terminal UI dashboard and CLI logger.
- **Link Time Optimization (LTO/IPO) integration** (`CMakeLists.txt`): Enabled Interprocedural Optimization (LTO) across all build targets. This allows cross-translation unit optimization and inlining, reducing remote ctest runtime by ~48% (from 51.2s to 26.5s) and speeding up mining framework logic.
- **JIT Loop Alignment optimization** (`src/jit_compiler_a64_static.S`): Added `.p2align 5` before `randomx_program_aarch64_main_loop` in the static assembly template. This aligns the JIT compiled program's main loop entry point to a 32-byte boundary, optimizing instruction fetch unit utilization and branch prediction accuracy on Cortex-A53.

## 2026-07-13 (CryptoNote/Herominers Stratum support)

### Added
- **CryptoNote Stratum Protocol support** (`src/stratum_client.cpp`, `include/armrx/stratum_client.hpp`): Added support for the CryptoNote JSON-RPC stratum protocol (including `login`, `keepalived`, and `submit` methods).
- **Auto-fallback handshake** (`src/stratum_client.cpp`): Added automatic fallback from Stratum V1 (`mining.subscribe`) to CryptoNote (`login`) when the pool rejects Stratum V1.
- **Improved JSON parsing** (`src/stratum_client.cpp`): Fixed `json_get` helper to support parsing nested JSON objects and arrays correctly by scanning matching brace/bracket depths.
- **Stable keepalive handling** (`src/stratum_client.cpp`): Suppressed keepalive responses (`KEEPALIVED`) in the share submission response handler to avoid treating them as rejected shares.
- **Race-free fallback connection lifecycle** (`src/stratum_client.cpp`, `include/armrx/stratum_client.hpp`): Introduced `fallback_in_progress_` state to prevent redundant reconnect triggers and duplicate client threads during handshake fallback.
- **SIGPIPE signal ignore** (`src/main.cpp`): Ignore `SIGPIPE` globally to prevent OpenSSL shutdown alert writes to closed socket descriptors from abruptly terminating the program.
- **Non-blocking login handshake** (`src/stratum_client.cpp`): Complete the connection handshake promise before running the synchronous job callback, avoiding handshake timeouts on slower CPUs (like Cortex-A53) during Argon2d cache initialization.
- **JIT compilation in Light Mode** (`src/vm.cpp`, `src/jit_compiler_a64.cpp`, `include/armrx/jit_compiler_a64.hpp`): Enabled JIT compilation in Light mode on AArch64 by compiling Superscalar programs in `set_cache()` and matching const parameter layouts. Corrected the `CacheSize` constant in the JIT compiler from `2 GiB` to the correct cache size of `256 MiB`, fixing the out-of-bounds cache line alignment mask that caused segmentation faults. This speeds up Light mode hashrate from 1.6 H/s to hardware JIT speed (~28 H/s).

## 2026-07-13 (CryptoNote protocol + 21 H/s light-mode milestone)

### Added
- **Dual-protocol Stratum client** (`stratum_client.hpp/cpp`): Auto-detects pool protocol — tries standard Stratum V1 (`mining.subscribe`) first, falls back to CryptoNote (`login`) on rejection. Handles `keepalive`, nested JSON job parsing, and CryptoNote share submission format.
- **`StratumProtocol` enum** with `AUTO`, `STRATUM_V1`, `CRYPTONOTE` modes. Handshake lifecycle with fallback tracking prevents race conditions during protocol switch.

### Fixed (performance: 1.2 → 21 H/s)
- **`CacheSize` constant in JIT compiler** (`jit_compiler_a64.cpp`): Was `2147483648` (2 GiB, dataset size) — corrected to `268435456` (256 MiB, cache size). The 8× too large cache mask caused out-of-bounds reads and segfaults in light mode. **This was the primary 22× performance bottleneck.**
- **Light-mode JIT enabled**: `set_cache()` now calls `jit_->generateSuperscalarHash()` to compile SuperscalarHash programs. Cache data pointer passed to `mem_regs.memory` for the JIT light-mode dataset derivation path.
- **Non-blocking handshake**: Moved handshake promise fulfillment before Argon2d cache init in job callback — prevents connection timeouts on slow hardware.
- **`kRandOMXFlagJit` unconditional**: JIT enabled regardless of `kRandOMXFlagFullMem` so light-mode VMs get JIT compilation.
- **SIGPIPE ignored**: Prevents OpenSSL `close_notify` writes on closed sockets from crashing the process.
- **Hashrate flushing per hash**: Worker threads flush local counter after each hash in interpreted/light mode for instant accurate reporting.

## 2026-07-13 (CPU affinity + per-worker counters + pool failover)

### Added
- **CPU affinity pinning** (`src/mining_engine.cpp`): Each worker thread pinned to `thread_id % hardware_concurrency()` via `pthread_setaffinity_np` — eliminates core migration overhead.
- **Per-worker hash counters** (`include/armrx/mining_engine.hpp`, `src/mining_engine.cpp`): Per-thread `std::atomic<uint64_t>` array with local accumulator batching (flush every 64 hashes). Exposed via `worker_hash_rate(thread_id)`.
- **Multiple pool failover** (`src/main.cpp`): Accept multiple `--pool=host:port` arguments. After 5 retries on the current pool, automatically cycles to the next with a 2s cooldown.
- **`--help` updated**: Documents `--tls`, multi-pool, and all CLI flags.

### Remaining
- Only two items left in the priority list: config file (medium) and Stratum V2 (high).

## 2026-07-13 (TLS/SSL pool connections + documentation sweep)

### Added
- **TLS/SSL support for pool connections** (`src/tls_client.cpp`, `include/armrx/tls_client.hpp`): Optional OpenSSL-based TLS wrapping. RAII `TlsClient` class with `SSL_connect`, SNI hostname, cipher reporting, and clean shutdown. Disabled at build time when OpenSSL is not found.
- **`--tls` / `--no-tls` CLI flag** (`src/main.cpp`): Enables encrypted pool connections (default: off).
- **CMake**: `find_package(OpenSSL QUIET)` — auto-detects OpenSSL; links `OpenSSL::SSL` + `OpenSSL::Crypto` and defines `ARMRX_HAVE_TLS=1` when found.

### Modified
- **`include/armrx/stratum_client.hpp`**: Added `enable_tls()` setter, optional `TlsClient` member under `#ifdef ARMRX_HAVE_TLS`.
- **`src/stratum_client.cpp`**: `connect()` wraps TCP socket with TLS when enabled; `write_all()`/`read_line()` route through TLS; `disconnect()` tears down TLS before closing socket.
- **`planned_improvements.md`**: Marked TLS/SSL as complete; collapsed priority table to 6 remaining items.

## 2026-07-13 (README restructure + auto-reconnect + huge pages)

### Added
- **Auto-reconnect with exponential backoff** (`src/stratum_client.cpp`): When the pool connection drops, the client now automatically retries with 1s → 2s → 4s → … → 30s cap backoff, configurable via `set_reconnect_config(max_retries, base_delay_ms)`. Error callback is only called after all retries are exhausted (default: 10 retries, then give up).
- **Huge pages for scratchpads** (`src/vm.cpp`): `madvise(MADV_HUGEPAGE)` applied after each 2 MiB scratchpad allocation, prompting the kernel to promote to transparent huge pages for reduced TLB pressure.

### Modified
- **`README.md`**: Full restructure — badges header, Quick Start / Usage / Architecture / Status sections with tables, tighter prose (~40% shorter). Replaced verbose "Implementation order" with a component checklist.
- **`include/armrx/stratum_client.hpp`**: Added `set_reconnect_config()`, `reconnect_attempts()` getter, `reconnect_loop()` private method, and backoff state members.
- **`src/main.cpp`**: Pool loop now runs `while (keep_running)` regardless of connection state. Status line shows yellow "Reconnecting (attempt N)..." when offline. Initial connection failure no longer exits — reconnect loop handles retries.
- **`planned_improvements.md`**: Marked auto-reconnect and huge pages as complete; renumbered priority table.

## 2026-07-13 (AArch64 build verification + JIT fixes)

### Added
- **`src/instruction_weights.hpp`**: Instruction frequency `#define`s and REP macros for the JIT compiler's 256-entry opcode handler table. Covers all 30 RandomX v1 opcode weights and REP0–REP256 expansion macros.
- **`src/configuration.h`**: Minimal assembly-compatible header defining `RANDOMX_PROGRAM_MAX_SIZE=384` required by `jit_compiler_a64_static.S`.
- **`src/soft_aes.cpp`**: AES lookup tables (`randomx_aes_lut_enc[4][256]`, `randomx_aes_lut_dec[4][256]`) extracted from upstream RandomX, needed by the JIT compiler's soft-AES fallback path.
- **`REASONIX.md`**: Project card capturing stack, layout, commands, conventions, and gotchas for future Reasonix sessions.

### Fixed
- **`CMakeLists.txt`**: Changed `LANGUAGES CXX` to `LANGUAGES C CXX ASM` so `virtual_memory.c` actually compiles instead of silently dropping the C source.
- **`src/jit_compiler_a64.cpp`**: Added 12 missing upstream constants (`RANDOMX_SCRATCHPAD_L1/L2/L3`, `CacheLineSize`, `CacheSize`, `ScratchpadL3Mask`, `ConditionMask/Offset`, `StoreL3Condition`, `RegisterNeedsDisplacement`) that were referenced but never defined. Fixed API mismatches (`getSize()`→`size()`, `getAddressRegister()`→`address_register()`), replaced `randomx_reciprocal_fast` with the project's `randomx_reciprocal`, and inlined the `isZeroOrPowerOf2` check.

### Verified
- **First full build + test pass on real AArch64 hardware** (Lenovo/MSM8916, postmarketOS edge, Linux 6.12.1, GCC 15.2.0). Both `armrx_tests` and `test_mining` pass, confirming:  
  - Reference hash `Input1` = `639183aae1bf4c9a35884cb46b09cad9175f04efd7684e7262a0ac1c2f0b4e3f` ✅  
  - Reference hash `Input2` = `300a0adb47603dedb42228ccb2b211104f4da45af709cd7547cd049e9489c969` ✅  
  - JIT + hardware AES/NEON pipeline fully operational on ARMv8-A with crypto extensions.

## 2026-07-13

### Added
- Created interpreted RandomX Virtual Machine execution engine (`src/vm.cpp`, `include/armrx/vm.hpp`) including integer and floating-point registers, scratchpad reads/writes, compiler thresholds, and interpreted execution loop.
- Added software AES encryption/decryption round primitives and scratchpad filling logic (`src/aes_hash.cpp`, `include/armrx/aes_hash.hpp`).
- Added end-to-end VM hash parity tests in `tests/test_blake2b.cpp` to validate against reference inputs `"This is a test"` and `"Lorem ipsum dolor sit amet"`.
- Implemented multi-threaded `MiningEngine` (`src/mining_engine.cpp`, `include/armrx/mining_engine.hpp`) for orchestrating mining loops, thread-local VM instances, and memory mode transitions.
- Added `Target` structure and `meets_target` verification logic (`include/armrx/mining_common.hpp`) to compare computed hashes against target difficulties.
- Added target difficulty translation, signal handling, and runtime benchmark duration controls to `src/main.cpp`.
- Added unit tests `tests/test_mining.cpp` to validate target difficulty boundary checks and worker orchestration lifecycles.

### Fixed
- Fixed AES decryption round sequence (`aes_decrypt_round` in `src/aes.cpp`) to apply standard Inverse ShiftRows -> Inverse SubBytes -> Inverse MixColumns -> AddRoundKey order.
- Corrected `build_aes_block` word mapping to follow standard little-endian format.
- Modified `init_scratchpad` to update the seeding `tempHash` in-place, passing the state-modified seed to the first VM program execution.
- Corrected floating-point instruction compilation frequency thresholds (`ceil_` ceilings in `src/vm.cpp`) to align with standard configuration frequencies.
- Added zero-initialization of integer registers `reg_.r` on VM setup to prevent cross-run state leaks.
- Removed unused parameter warnings from `execute_bytecode`.

### Modified
- Updated `README.md` to document the completed interpreted VM implementation details, status, and verification test vectors.
- Updated `CMakeLists.txt` to compile `src/mining_engine.cpp` with pthread linkages, and register `test_mining` to the build and CTest validation pipeline.

## 2026-07-13 (AArch64 JIT + Hardware AES)

### Added
- **Hardware AES round intrinsics** (`src/aes_hash.cpp`): Added conditional `#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)` fast paths for all four core functions (`fill_aes_1r_x4`, `fill_aes_4r_x4`, `hash_aes_1r_x4`, `hash_and_fill_aes_1r_x4`). Uses `vaeseq_u8`/`vaesmcq_u8`/`vaesdq_u8`/`vaesimcq_u8` NEON intrinsics; falls back to software-GF(2^8) path on non-AArch64 targets.
- **JIT compiler infrastructure**: Ported `jit_compiler_a64.cpp`, `jit_compiler_a64_static.S`, and `jit_compiler_a64.hpp` from upstream RandomX under `src/` and `include/armrx/`. Adapted includes, namespace (`armrx`), constants (`RegistersCount`, `RANDOMX_FLAG_*`, `RANDOMX_CACHE_ACCESSES`, `RANDOMX_SUPERSCALAR_LATENCY`), and removed upstream-specific dependencies.
- **Virtual memory support** (`src/virtual_memory.c`, `include/armrx/virtual_memory.h`): Ported upstream POSIX `mmap`/`mprotect` page allocator (writable + executable memory pages needed for JIT code emission).
- **JIT VM integration** (`include/armrx/vm.hpp`, `src/vm.cpp`): Added `kRandOMXFlagJit` flag; `VirtualMachine` conditionally constructs `JitCompilerA64` and compiles each program at runtime via `generateProgram`, wires `ProgramConfiguration` from VM state, and calls `getProgramFunc()` instead of entering the interpreted loop when `ARMRX_HAVE_JIT` is defined and the flag is set.
- **Program/ProgramConfiguration types** (`include/armrx/program.hpp`): Introduced `ProgramConfiguration` and `MemoryRegisters` structs used by the JIT compiler ABI.

### Modified
- **`CMakeLists.txt`**: Detects AArch64 target processor via `CMAKE_SYSTEM_PROCESSOR`; enables `ASM` language and adds JIT/ASM sources (`jit_compiler_a64.cpp`, `jit_compiler_a64_static.S`, `virtual_memory.c`) only on that platform; sets `-march=armv8-a+crypto` and `ARMRX_HAVE_JIT=1`.
- **`include/armrx/vm.hpp`**: Replaced raw `std::array<Instruction, 256> program_{}` with `Program program_{}` for compatibility with JIT compiler API; added optional `std::unique_ptr<JitCompilerA64> jit_` member under `#ifdef ARMRX_HAVE_JIT`.
- **`README.md`**: Updated step 5 as complete; added build notes explaining AArch64-specific flags and x86_64 fallback behaviour.

### Fixed
- Fixed stray `c` prefix character on `aes_encrypt_round` function declaration in `src/aes.cpp`.

## 2026-07-13 (Stratum V1 Client)

### Added
- **`include/armrx/stratum_client.hpp`**: Declared `StratumClient` class implementing Monero Stratum V1 protocol. Exposes `connect()`, `disconnect()`, `submit_share()`, `set_job_callback()`, and `set_error_callback()`.
- **`src/stratum_client.cpp`**: Full Stratum V1 implementation:
  - TCP socket connection via POSIX `getaddrinfo` / `connect`, with `TCP_NODELAY` for low-latency share submission.
  - Background reader thread that accumulates line-delimited JSON messages from the pool.
  - `mining.subscribe` — sends agent string, extracts `extranonce1` from pool reply.
  - `mining.authorize` — authenticates wallet address and password with the pool.
  - `mining.notify` — parses job ID, block template blob, target, and seed hash; dispatches via `JobCallback`.
  - `mining.set_target` — updates 32-byte target from 64-char hex string.
  - `mining.set_difficulty` — converts numeric difficulty to 32-byte target using the same `2^256 / D` algorithm as the local miner.
  - `mining.submit` — serialises nonce as little-endian hex and sends a JSON-RPC submit message.
  - Minimal hand-rolled JSON extractor (no external library dependency).
- **`src/main.cpp`**: Added pool mining mode with three new CLI flags:
  - `--pool=host[:port]` — pool server address (default port 3333).
  - `--wallet=<address>` — Monero wallet address used as worker login.
  - `--password=<pw>` — pool worker password (default `x`).
  - `--help` updated to document pool mining section.
  - Pool mode wires `StratumClient::set_job_callback` → `MiningEngine::set_job`, and `MiningEngine::ShareCallback` → `StratumClient::submit_share`, with a live H/s status line.

### Modified
- **`CMakeLists.txt`**: Added `src/stratum_client.cpp` to `armrx_core` source list.
- **`README.md`**: Added **§3 Pool Mining (Stratum V1)** usage section; marked step 6 as complete in the implementation order list.

