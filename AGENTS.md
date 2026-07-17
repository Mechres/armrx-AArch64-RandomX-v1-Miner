# armrx — Agent Instructions

## Build
```sh
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CMake requires `LANGUAGES C CXX ASM`. On AArch64: JIT + hardware AES/NEON auto-enabled. On x86_64: JIT excluded, interpreted VM only.

## Architecture
- **AArch64**: `-march=armv8-a+crypto`, `ARMRX_HAVE_JIT=1`, hardware AES/NEON intrinsics
- **x86_64**: Interpreted VM only (JIT sources silently excluded)
- **Memory modes**: Auto-selected via `MemAvailable` check with 256 MiB OS reserve. Light: 256 MiB cache, Fast: 2080 MiB dataset

## Gotchas
- `scratch_vm_study/` — standalone, own CMakeLists.txt, embedded upstream RandomX reference. **Do not modify** — changes don't affect main build.
- TLS pool connections require OpenSSL at build time (`find_package(OpenSSL QUIET)`). Disabled silently if not found.
- Multiple `--pool=host:port` flags enable automatic failover after 5 retries with 2s cooldown.
- No lint/format tools configured.
- **Monero pools** (including herominers.com) use the CryptoNote Stratum protocol (`login` + `job`), not Bitcoin-style Stratum V1 (`mining.subscribe` + `mining.authorize`). The AUTO mode tries CryptoNote first, then falls back to Stratum V1.

## Entry Points
- Main executable: `src/main.cpp`
- Core library: `armrx_core` (CMake target)
- Tests: `tests/test_blake2b.cpp`, `tests/test_mining.cpp`
