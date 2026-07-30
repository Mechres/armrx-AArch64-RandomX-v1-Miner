# armrx — Agent Instructions

## Build
```sh
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Devbox (AArch64 device)
```sh
devbox_status        # verify connectivity
devbox_sync          # rsync local source tree
devbox_build         # native AArch64 build
devbox_test          # run KATs
devbox_bench         # performance benchmark
devbox_perf_stat     # PMU counters (branch misses, IPC)
devbox_full          # all-in-one: sync → build → test → bench
devbox_logs          # read last build/test/bench log
devbox_pgo_build     # full PGO: generate → train → use in one call
```

CMake requires `LANGUAGES C CXX ASM`. On AArch64: JIT + hardware AES/NEON auto-enabled. On x86_64: JIT excluded, interpreted VM only.

Experimental flags (all default OFF, all verified but not adopted):
- `ARMRX_PGO=GENERATE|USE` — profile-guided optimization (GCC only, LTO must be off; measured null on current code)
- `ARMRX_ENABLE_NEON_AES=ON` — NEON vector-permute AES (~19.4% slower on Cortex-A53)
- `ARMRX_ENABLE_JIT_FAST_DIV_SQRT=ON` — Newton-Raphson FDIV/FSQRT (−1.1% hashrate)
- `ARMRX_FAST_MATH=ON` — `-Ofast` / `-ffast-math`
- `ARMRX_DISABLE_LTO=ON` — workaround for GCC 15 + musl LTO crashes

## Architecture
- **AArch64**: `-march=armv8-a+crypto`, `ARMRX_HAVE_JIT=1`. NEON AES paths were removed (AESE/AESD instruction ordering incompatible with RandomX round spec — AddRoundKey at start vs end). All AES uses software T-table path.
- **x86_64**: Interpreted VM only (JIT sources silently excluded)
- **Memory modes**: Auto-selected via `MemAvailable` check with 256 MiB OS reserve. Light: 256 MiB cache, Fast: 2080 MiB dataset

## Gotchas
- AES T-table bugs were fixed (encrypt column permutation + decrypt column permutation) — see `docs/postmortems/aes-ttable-bug-postmortem.md`. NEON AES paths are **disabled** (AESE/AESD incompatible ordering).
- `scratch_vm_study/` — standalone, own CMakeLists.txt, embedded upstream RandomX reference. **Do not modify** — changes don't affect main build.
- TLS pool connections require OpenSSL at build time (`find_package(OpenSSL QUIET)`). Disabled silently if not found.
- Multiple `--pool=host:port` flags enable automatic failover after 5 retries with 2s cooldown.
- `.clang-format` and `.clang-tidy` exist (LLVM base, Allman braces, 120-col). No CI enforcement, but match their style when editing.
- **Monero pools** (including herominers.com) use the CryptoNote Stratum protocol (`login` + `job`), not Bitcoin-style Stratum V1 (`mining.subscribe` + `mining.authorize`). The AUTO mode tries CryptoNote first, then falls back to Stratum V1.
- **RETROSPECTIVE.md** at the repo root — full project retrospective. Start here for context.
- **CTest BAD_COMMAND flake** — intermittent CTest path-resolution issue on-device. Treat BAD_COMMAND "failures" as a flake, not real regressions; re-run named tests or binaries directly from `build/`.
- **Two-cluster topology**: device is MSM8929/Snapdragon 415 (not MSM8916), two 4-core L2 clusters. Cores 4-7 lose ~50% throughput to cores 0-3 under full contention (interconnect arbitration, not a frequency difference).
- **Instruction-count gap**: armrx is ~90% of XMRig per-cluster (cluster-normalized). Remaining ~10-12% is entirely an instruction-count gap (~33.5% more instructions/hash), not stalls/scheduling/branch-prediction. IPC is actually *better* than XMRig (0.731 vs 0.612).
- **Emitter scheduler**: reorders JIT emission order to hide long-latency stalls. Active in both main-VM and superscalar paths — the main-VM-only version measured null at first (wrong target region), extending to superscalar (where the dominant IMUL cost lives) made it a real, small, measured win (+0.233% IPC / -0.036% cycles). Stress-tested (450 + 200 differential pairs). Reviewed independently three times (Deepseek, Gemini, Hermes — `docs/archived/audits/emitter-scheduler-review.md` + 2 more). **Do not extend to memory-load (`*_M`) opcodes** — tried, caused a real JIT/interpreter divergence, reverted, mechanism not identified; see `docs/experiments/memory-op-scheduler-attempt.md`.
- **`-frounding-math` IS set in CMakeLists** (added 2026-07-25 for correctness-by-construction — the RandomX VM depends on runtime `fesetround()` changes).
- **Peephole JIT coalescing is closed, on evidence** — `tools/jit_correlate.py` found the main VM program region has a ~2.2× IPC penalty (memory-op stalls, ~9% of instructions but ~20% of cycles), not an instruction-count problem, so code-density reduction can't fix it. See `docs/archived/plans/performance-plan-20260725.md` (gated steps) / `docs/archived/plans/experimental-performance-ideas-20260725.md` (speculative backlog) if code-level performance work resumes.
- **`isolcpus`/`rcu_nocbs` — real ~14% hashrate win, biggest in project history (2026-07-25)**: adding `isolcpus=1-7 rcu_nocbs=1-7` to the kernel boot cmdline (core 0 left for housekeeping) gives ~28.4 vs ~24.9 H/s at 8 workers. Mechanism: without isolation, 2-3 of the 4 weak-cluster (cores 4-7) workers randomly lose half their throughput to background OS work each run; isolation stops it. Operational/deployment change only, not shippable in `armrx` itself. `nohz_full=1-7` silently no-ops on this kernel (`CONFIG_NO_HZ_FULL` unset). `--rt-priority`'s incremental contribution on top of isolation is unconfirmed. See `docs/experiments/isolcpus-rt-priority-win.md`.

## Entry Points
- Main executable: `src/main.cpp`
- Core library: `armrx_core` (CMake target)
- Tests: `tests/test_blake2b.cpp`, `tests/test_mining.cpp`

## Changelog & docs discipline

After every code change that is deployed and verified (KATs green, build passes):

1. **`changelogs.md`** — add a brief dated entry summarizing what changed, with filenames. One entry per logical change group (multiple related fixes can share one entry).
2. **`README.md`** — update the Status table if the change adds or removes a feature. Add a row or mark an existing one as ✅.
3. **`ROADMAP.md`** — move completed items from "Remaining" to the appropriate "Completed" section. Close resolved items.
4. **Reference docs in `docs/`** — if a change updates a design decision, bug postmortem, or plan, update the corresponding doc. Keep cross-references consistent.
5. **Skip if** the change is purely cosmetic (comment fix, whitespace) or a revert of an unshipped change.
