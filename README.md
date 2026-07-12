# armrx

`armrx` is a clean-room, CPU-only Monero RandomX v1 miner for AArch64 Linux. It
is being implemented against the public specification; it does not incorporate
miner source code. RandomX v2 is a planned Monero network upgrade, not current
mainnet consensus, so it is deliberately not the initial target.

## Status

The current milestone provides the build system, Linux AArch64 feature probe,
and a tested BLAKE2b-512 primitive. It is **not yet a functional miner**:
RandomX cache/dataset generation, VM execution, nonce search, and pool
protocol support are still to be implemented.

## Build

On an AArch64 Linux host:

```sh
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For cross compilation, provide an AArch64 CMake toolchain file and leave
`ARMRX_ENABLE_NATIVE` disabled.

## Implementation order

1. BLAKE2b, including Argon2-compatible variable output/H' and its 1 KiB compression function, plus deterministic byte/word helpers (complete).
2. AES round primitive and AesGenerator1R/AesGenerator4R generators (complete); RandomX cache initialization.
3. Dataset initialization plus the interpreter VM and official test vectors.
4. AArch64 JIT backend, guarded by runtime AES feature detection.
5. Stratum client, work scheduling, nonce partitioning, and share submission.

The implementation must pass the official RandomX test vectors before any pool
networking is enabled.

## 2 GB devices

Fast mode needs a 2080 MiB shared dataset, before the operating system or the
per-worker 2 MiB scratchpad are considered. It cannot fit on a 2 GB device.

Use RandomX **light mode** instead. It keeps a 256 MiB shared cache and derives
dataset items on demand, producing exactly the same hashes but at a much lower
hash rate. Reserve at least 256 MiB plus 2 MiB per worker and normal OS memory;
start with one worker on a 2 GB device. The miner will expose this as an
explicit `--mode=light` setting once the hash engine is added, and will refuse
fast mode when available memory is insufficient.

The startup probe already reports an automatic recommendation. It uses Linux
`MemAvailable` rather than installed RAM, takes the lower value when a cgroup
memory limit applies, and reserves 256 MiB for the operating system. Fast mode
is selected only if the 2080 MiB dataset, 2 MiB per worker, and that reserve
all fit. `auto` will therefore select light mode on a 2 GB device.
