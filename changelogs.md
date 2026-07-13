# Changelog

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

