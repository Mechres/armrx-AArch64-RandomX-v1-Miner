# armrx — Reasonix project card

## Stack

- **Language** — C++20, AArch64 Linux primary target
- **Build** — CMake ≥3.20 (`cmake -S . -B build && cmake --build build -j`)
- **Test** — CTest via `ctest --test-dir build --output-on-failure`
- **Key deps** — pthreads (via `find_package(Threads)`); hardware AES/NEON on AArch64 (`-march=armv8-a+crypto`)
- **Lint/format** — `.clang-format` + `.clang-tidy` checked in (LLVM base, Allman braces, 120-col), no CI enforcement

## Layout

| Path | Content |
|------|---------|
| `include/armrx/` | Public headers (`.hpp`) |
| `src/` | Implementation files (`.cpp`, `.c`, `.S` for AArch64 JIT) |
| `tests/` | 17 test executables: correctness (blake2b, mining, config, cli_parser, aes_hash, aes_neon, pool_protocol, jit_equivalence, jit_encodings, jit_determinism, jit_scheduler_stress, jit_superscalar_scheduler_stress), benchmarks (bench_armrx, bench_opcodes), fuzzing (fuzz_json), introspection (jit_check, jit_compare) |
| `scratch_vm_study/` | Standalone RandomX reference study area (own CMakeLists.txt, NOT part of main build) |
| `build/` | Build output (gitignored) |

## Commands

| Action | Command |
|--------|---------|
| Configure | `cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON` |
| Build | `cmake --build build -j` |
| Test | `ctest --test-dir build --output-on-failure` |
| Cross-build (x86_64) | Same configure/build; JIT files excluded automatically |

## Conventions

- **Include style** — project headers with `""`, stdlib headers with `<>`, grouped separately
- **Header guard** — `#pragma once` everywhere
- **Namespace** — everything under `armrx`
- **Naming** (enforced by `.clang-tidy`) — `PascalCase` classes/structs, `camelBack` functions/methods,
  `lower_case` local variables/members (member fields get a trailing `_`), `UPPER_CASE` constants
- **File extensions** — `.hpp` for C++ headers, `.h` for C headers, `.cpp` for C++ sources, `.c` for C sources, `.S` for AArch64 assembly
- **Test files** — `test_` prefix in filename, linked against `armrx_core` library

## Watch out for

- **`scratch_vm_study/` is standalone** — it has its own `CMakeLists.txt` and an embedded `upstream_rx/` subtree (third-party RandomX reference). Edits there don't affect the main build.
- **JIT is AArch64-only** — `jit_compiler_a64.cpp`, `jit_compiler_a64_static.S`, and `virtual_memory.c` are only compiled when targeting AArch64. On x86_64 they're silently excluded; the VM falls back to interpreted mode.
- **`build/` is gitignored** — all build output lives there. `cmake -S . -B build` is required before any build commands.
- **`-frounding-math` IS set in CMakeLists** (added 2026-07-25) for correctness-by-construction — the RandomX VM depends on runtime `fesetround()` changes.
- **Two-cluster topology** — target device (MSM8929/Snapdragon 415) has two 4-core L2 clusters. Cores 4-7 lose ~50% throughput under full contention (interconnect arbitration). Cluster-normalized: armrx at ~90% of XMRig, gap is instruction-count (~33.5% more instructions/hash), not stalls.
- **Emitter scheduler** — active in both main-VM and superscalar JIT paths. Reorders emission to hide long-latency stalls; adopted after the main-VM-only version measured null (wrong target region) and extending to superscalar (real IMUL cost) made it a real, small, measured win (+0.233% IPC). Stress-tested, reviewed by three independent code reviews. Do not extend to memory-load (`*_M`) opcodes without reading `docs/experiments/memory-op-scheduler-attempt.md` first — tried once, caused a real correctness regression, reverted.
- **Experimental flags** — `ARMRX_PGO`, `ARMRX_ENABLE_NEON_AES`, `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` (all default OFF, measured not beneficial on current hardware).
- **Peephole JIT coalescing is closed on evidence** (2026-07-25) — the main VM program region has a ~2.2× IPC penalty from memory-op stalls, not an instruction-count problem. See `docs/plans/performance-plan-20260725.md` (gated steps) and `docs/plans/experimental-performance-ideas-20260725.md` (speculative backlog) if code-level performance work resumes.
- **`isolcpus`/`rcu_nocbs` deployment tuning — real ~14% hashrate win, biggest in project history** (2026-07-25) — `isolcpus=1-7 rcu_nocbs=1-7` on the kernel boot cmdline (core 0 left for housekeeping) gives ~28.4 vs ~24.9 H/s at 8 workers by stopping background OS work from stealing cycles from weak-cluster (cores 4-7) workers. Operational change, not shippable in `armrx` itself; `nohz_full=1-7` no-ops on this kernel. See `docs/experiments/isolcpus-rt-priority-win.md`.
