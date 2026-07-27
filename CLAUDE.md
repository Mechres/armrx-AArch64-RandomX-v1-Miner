# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`armrx` is a clean-room CPU-only Monero RandomX v1 miner for AArch64 Linux, built independently against the
[RandomX Specification](https://github.com/tevador/RandomX) (not derived from an existing mining client).
It has a native AArch64 JIT compiler as the primary execution path and a portable bytecode interpreter as a
fallback (used automatically on non-AArch64 hosts, e.g. for local x86_64 dev builds).

## Build & test

```sh
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

- CMake requires `LANGUAGES C CXX ASM` (the `.S` file is AArch64 JIT glue).
- On AArch64: JIT + hardware AES/NEON build automatically (`-march=armv8-a+crypto`, `ARMRX_HAVE_JIT=1`).
- On x86_64: JIT sources (`src/jit_compiler_a64.cpp`, `src/jit_compiler_a64_static.S`, `src/virtual_memory.c`)
  are silently excluded; only the interpreted VM builds. This is the normal way to dev/compile-check locally.
- **CTest path caveat — intermittent, reproduced again 2026-07-24**: `bench_armrx`,
  `bench_opcodes`, `test_jit_encodings`, `test_jit_determinism` (and sometimes `armrx_tests`/
  `test_mining`) can show as "Not Run (BAD_COMMAND / permission denied)" under plain `ctest`
  on-device. A 2026-07-22 session couldn't reproduce it, but a 2026-07-24 `devbox_full` run
  hit it again (7 of 12 "failed", all BAD_COMMAND) while an immediate filtered re-run
  (`devbox_test` with explicit names) passed 5/5 cleanly. Treat BAD_COMMAND "failures" as
  the flake, not real regressions: re-run the named tests or the binaries directly from
  `build/` to confirm.
- Useful CMake options: `ARMRX_ENABLE_NATIVE` (tune for host CPU), `ARMRX_PGO=GENERATE|USE` (profile-guided
  optimization, GCC only — LTO must be off while using PGO), `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` (Newton-Raphson
  FDIV/FSQRT), `ARMRX_ENABLE_NEON_AES` (NEON vector-permute AES, experimental), `ARMRX_FAST_MATH` (`-Ofast`),
  `ARMRX_ENABLE_ASAN`/`ARMRX_ENABLE_UBSAN`/`ARMRX_ENABLE_TSAN`,
  `ARMRX_DISABLE_LTO` (workaround for GCC 15 + musl LTO crashes).
- **`-frounding-math` is set** in CMakeLists (added 2026-07-25 per a third-party audit finding) for
  correctness-by-construction; the RandomX VM depends on IEEE 754 rounding-mode changes via runtime `fesetround()`.
- No lint/format CI step exists, but `.clang-format` and `.clang-tidy` are checked in — match their style
  when editing (see Conventions below).

### Devbox (real AArch64 hardware validation)

Perf numbers and the JIT itself only mean something on real AArch64 hardware; the local dev sandbox here is
x86_64. `tools/devbox/` is an MCP bridge (`devbox_mcp.py`) that syncs, builds, and benchmarks on a remote
AArch64 device over SSH. If those MCP tools (`devbox_status`, `devbox_sync`, `devbox_build`, `devbox_test`,
`devbox_bench`, `devbox_perf_stat`, `devbox_full`, `devbox_logs`, `devbox_pgo_build`) are available in a
session, `devbox_status` is the right first call — it reports reachability and how far the device's deployed
revision is from local HEAD. `devbox_full` does sync → build → test → bench in one call.
`devbox_pgo_build` orchestrates the full generate → train → use PGO cycle. Device connection config lives in
`tools/devbox/devbox.json` (gitignored, never commit it).

## Architecture

### Execution pipeline

`MiningEngine` (`src/mining_engine.cpp`, `include/armrx/mining_engine.hpp`) owns worker threads, dataset/cache
memory, and job/seed transitions. Workers use generation-counter-based lock-free job distribution and per-worker
partitioned nonces. Affinity pinning uses sysfs-detected core ordering with frequency-based cluster detection
(corrected from a prior hardcoded "cores 0-3 = big" assumption — the real device is MSM8929/Snapdragon 415 with
two 4-core L2 clusters, not MSM8916/Snapdragon 410 as previously assumed).

Each worker runs the RandomX VM (`src/vm.cpp`) which either:
- **JIT path (AArch64 default)**: `src/jit_compiler_a64.cpp` translates a RandomX `Program`
  (`include/armrx/program.hpp`, `instruction.hpp`) into native AArch64 machine code. A
  **conservative emitter lookahead scheduler** (lines ~220–670 of `jit_compiler_a64.cpp`, adopted
  2026-07-25) reorders emission order within 3-instruction windows to hide long-latency multiply
  stalls on the in-order Cortex-A53. Applied to both the main VM program (`scheduleProgram`) and the
  superscalar dataset-derivation path (`scheduleSuperscalarProgram`) — the main-program-only version
  measured as a null at first specifically because it targeted the wrong region; extending it to the
  superscalar path (where the dominant `IMUL_R`/`IMUL_RCP` cost actually lives) turned it into a real,
  small, `taskset`-pinned-measured win (+0.233% IPC / -0.036% cycles). Full hazard model: register
  RAW/WAR/WAW across int/f/e register files, CBRANCH anchors, memory-op aliasing, and src==dst/x20
  physical-scratch-register exclusion. **Do not extend this scheduler to the memory-load (`*_M`)
  opcodes** — tried 2026-07-25, caused a real JIT/interpreter divergence whose exact hazard mechanism
  was never identified, reverted; see `docs/experiments/memory-op-scheduler-attempt.md` before
  attempting this again. Dedicated stress tests at `tests/test_jit_scheduler_stress.cpp` (450 pairs)
  and `tests/test_jit_superscalar_scheduler_stress.cpp` (200 pairs). Independently reviewed three
  times (Deepseek, Gemini, Hermes — `docs/audits/emitter-scheduler-review.md`,
  `jit_scheduler_code_review_gemini.md`, `scheduler-review-2026-07-25.md`). JIT code executes from a
  W^X-mapped buffer (`src/virtual_memory.c` / `jit_compiler_a64_static.S`).
- **Interpreter fallback**: a portable C++ dispatch loop over the same bytecode, used whenever
  `ARMRX_HAVE_JIT` is not defined (non-AArch64 targets).

Superscalar program generation (`src/superscalar.cpp`) and the AES-based generators (`src/aes_generator.cpp`,
`src/aes_hash.cpp`, `src/blake2_generator.cpp`) implement the RandomX dataset/cache initialization spec.
`src/argon2.cpp` builds the Argon2d cache. Memory mode (`auto`/`light`/`fast`) is chosen via a `MemAvailable`
check with a 256 MiB OS reserve — light mode uses a 256 MiB cache, fast mode a ~2080 MiB dataset
(`src/memory.cpp`, `src/dataset.cpp`).

**AES note**: NEON hardware AES paths (AESE/AESD) were tried and removed — their fixed instruction ordering
(AddRoundKey position) is incompatible with the RandomX round spec. All AES hashing uses the software T-table
path (`src/soft_aes.cpp`, `src/aes_hash.cpp`); see `docs/postmortems/aes-ttable-bug-postmortem.md` for the bug history.

### Networking

`src/stratum_client.cpp` / `src/pool_manager.cpp` implement a dual-protocol Stratum client: standard Bitcoin-
style Stratum V1 (`mining.subscribe`/`mining.authorize`) and the CryptoNote variant (`login`/`job`) used by
Monero pools (including herominers.com). Auto mode tries CryptoNote first, then falls back to Stratum V1.
Multiple `--pool=host:port` flags give automatic failover after 5 retries with a 2s cooldown. TLS
(`src/tls_client.cpp`) is compiled in only if OpenSSL is found at configure time (`ARMRX_HAVE_TLS`); otherwise
disabled silently.

### Other subsystems

- `src/config.cpp` / `src/json.cpp` — CLI/config parsing and a hand-rolled JSON parser. The parser itself
  (`json.cpp`) is fuzz-tested (`tests/fuzz_json.cpp`, 2.5M+ executions, zero findings) and hardened against
  injection. `config.cpp`'s numeric config-file fields (`workers`/`difficulty`/`seconds`/pool port) are
  exception-guarded with try/catch, matching `cli_parser.cpp`'s CLI-flag treatment (fixed; regression test
  in `tests/test_config.cpp`).
- `src/tui.cpp` (`include/armrx/tui.hpp`) — optional interactive terminal dashboard (`--tui`). Untested.
- `include/armrx/metrics.hpp` — Prometheus metrics endpoint (`--metrics-port`). Its background thread joins
  cleanly on destroy (fixed), and `server_fd_` is a `std::atomic<int>` (data race fixed).
- `src/cpu_features.cpp` — `/proc/self/auxv` AT_HWCAP detection for AES/CRC32 on AArch64 Linux.
- `src/memory.cpp` — available-memory detection for auto mode selection (host `MemAvailable` + cgroup v1/v2).
- `src/main.cpp` / `src/cli_parser.cpp` / `src/miner_app.cpp` — the `armrx` executable entry point, CLI flag
  parsing, and `MinerApp` orchestration tying `PoolManager` + `MiningEngine` + `MetricsExporter` together.
- `scratch_vm_study/` is a **standalone** area with its own `CMakeLists.txt` and an embedded upstream RandomX
  reference implementation, used for cross-checking VM behavior during development. It is not part of the
  main build — do not expect changes there to affect `armrx_core`.

## Conventions

- Public headers live in `include/armrx/` (`.hpp`); implementations in `src/` (`.cpp`/`.c`, `.S` for AArch64
  asm). `#pragma once` everywhere; everything under the `armrx` namespace.
- Naming (enforced by `.clang-tidy`): classes/structs `PascalCase`, functions/methods `camelBack`, local
  variables/members `lower_case` (member fields get a trailing `_`), constants `UPPER_CASE`.
- Formatting: LLVM base style, 4-space indent, Allman braces, 120-column limit — see `.clang-format`.
- Test files use a `test_` filename prefix and link against the `armrx_core` library target.

## Project docs & maintenance discipline

- `NEXT_STEPS.md` — current prioritized todo list; check before starting new work.
- `PLAN.md` / `ROADMAP.md` — longer-range plan and status tracking. Phases 1 through 8 are all
  fully resolved (2026-07-25; Phases 1–5, 6, and 7 archived into `docs/archived/`, Phase 8's
  result kept in `docs/experiments/isolcpus-rt-priority-win.md`). No genuinely open item remains.
- **Performance context** — the device runs light mode (2 GiB RAM). armrx is at ~90% of XMRig
  per-cluster. `tools/jit_correlate.py`'s opcode-level cycle correlation (2026-07-25) found the main
  per-hash VM program region carries ~9% of dynamic instructions but ~20% of cycles — a ~2.2× IPC
  penalty, a memory-op (scratchpad) stall signature, not an instruction-count one. This closed
  **peephole JIT coalescing** on evidence (its whole premise is code-density reduction, which can't
  fix a memory-latency stall) and is the reason every instruction-count-reduction attempt this
  project has tried has failed or regressed (CSEL, Newton-Raphson, NEON AES ×3, superscalar
  literal-pool relayout, `IMUL_RCP` literal-load elimination). The natural follow-on — extending the
  emitter scheduler to hide this region's memory-op stalls — was tried and reverted (see the
  scheduler note above). See `docs/plans/performance-plan-20260725.md` (gated steps) and
  `docs/plans/experimental-performance-ideas-20260725.md` (speculative backlog) for where the
  evidence points if code-level performance work resumes.
- **`isolcpus`/`rcu_nocbs` deployment win (2026-07-25)** — a real ~14% aggregate hashrate win
  (~28.4 vs. ~24.9 H/s, 8 workers), the biggest measured in this project's history, from adding
  `isolcpus=1-7 rcu_nocbs=1-7` to the kernel boot cmdline (core 0 left for housekeeping). This is
  an **operational/deployment** change, not shippable in `armrx` itself. Mechanism: without
  isolation, 2-3 of the 4 weak-cluster (cores 4-7) workers randomly lose half their throughput to
  background OS work each run; isolation stops it, fast-cluster workers unaffected either way.
  `nohz_full=1-7` silently no-ops on this kernel (`CONFIG_NO_HZ_FULL` unset). `--rt-priority`'s
  independent contribution on top of isolation is unconfirmed. Full account, including two
  corrected false starts (a stale historical baseline mis-comparison and an overconfident
  thermal-throttling attribution), in `docs/experiments/isolcpus-rt-priority-win.md`.
- `docs/` — organized into subdirectories:
  - `docs/audits/` — correctness, security, performance audits (including this project's audit reports)
  - `docs/experiments/` — measured performance attempts, both wins and honest negative results
  - `docs/plans/` — performance master plans and forward-looking proposals
  - `docs/postmortems/` — detailed root-cause analyses for past bugs
  - `docs/archived/` — superseded or historical material
- After a code change that's deployed and verified (KATs green, build passes), keep the docs in sync:
  1. Add a dated entry to `changelogs.md` (filenames + what changed; one entry per logical change group).
  2. Update `README.md`'s status table if a feature was added/removed.
  3. Move completed items in `ROADMAP.md` from "Remaining" to "Completed".
  4. Update the relevant `docs/` reference if a design decision or plan changed.
  5. Skip this for purely cosmetic changes or reverts of unshipped work.
