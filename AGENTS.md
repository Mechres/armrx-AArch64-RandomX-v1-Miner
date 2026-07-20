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
```

CMake requires `LANGUAGES C CXX ASM`. On AArch64: JIT + hardware AES/NEON auto-enabled. On x86_64: JIT excluded, interpreted VM only.

## Architecture
- **AArch64**: `-march=armv8-a+crypto`, `ARMRX_HAVE_JIT=1`. NEON AES paths were removed (AESE/AESD instruction ordering incompatible with RandomX round spec — AddRoundKey at start vs end). All AES uses software T-table path.
- **x86_64**: Interpreted VM only (JIT sources silently excluded)
- **Memory modes**: Auto-selected via `MemAvailable` check with 256 MiB OS reserve. Light: 256 MiB cache, Fast: 2080 MiB dataset

## Gotchas
- AES T-table bugs were fixed (encrypt column permutation + decrypt column permutation) — see `docs/aes-ttable-bug-postmortem.md`. NEON AES paths are **disabled** (AESE/AESD incompatible ordering).
- `scratch_vm_study/` — standalone, own CMakeLists.txt, embedded upstream RandomX reference. **Do not modify** — changes don't affect main build.
- TLS pool connections require OpenSSL at build time (`find_package(OpenSSL QUIET)`). Disabled silently if not found.
- Multiple `--pool=host:port` flags enable automatic failover after 5 retries with 2s cooldown.
- No lint/format tools configured.
- **Monero pools** (including herominers.com) use the CryptoNote Stratum protocol (`login` + `job`), not Bitcoin-style Stratum V1 (`mining.subscribe` + `mining.authorize`). The AUTO mode tries CryptoNote first, then falls back to Stratum V1.
- **Next steps plan** at `NEXT_STEPS.md` — current prioritized todo list. Check before starting new work.
- **CTest executable paths** — bench_armrx, bench_opcodes, test_jit_encodings, test_jit_determinism are built but "Not Run" by CTest (wrong binary search path). Run them directly from `build/` to verify.

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
