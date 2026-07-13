# armrx

`armrx` is a clean-room, CPU-only Monero RandomX v1 miner for AArch64 Linux. It
is being implemented against the public specification; it does not incorporate
miner source code. RandomX v2 is a planned Monero network upgrade, not current
mainnet consensus, so it is deliberately not the initial target.

## Status

The interpreted RandomX Virtual Machine and hash pipeline is fully implemented and passes the official end-to-end RandomX validation test suite. This includes:
- Blake2b-512 program seeding and final hashing.
- Exact Argon2d cache initialization.
- Exact on-demand dataset-item generation (light-mode).
- Exact interpreted Virtual Machine execution (integer registers, floating-point math registers, scratchpad reads/writes, compiler thresholds).

All reference test vectors (`Input1` and `Input2`) pass successfully.

## Interpreted Virtual Machine Implementation Details

Our clean-room AArch64 interpreted VM matches the reference RandomX implementation by resolving several subtle design decisions:
1. **AES Inverse Round Equivalence**: Hardware instruction round math (`aesd`) matches equivalent AES decryption inverse round execution sequence (`InvShiftRows -> InvSubBytes -> InvMixColumns -> AddRoundKey`). In software execution, the round key XOR must happen *after* the inverse transformations rather than before.
2. **Word Mapping in AES Block Builder**: Elements in `build_aes_block` follow a strict little-endian layout matching the reference `rx_set_int_vec_i128` macro (the lowest address maps to the lowest byte of the last parameter).
3. **In-place Scratchpad Seeding**: The scratchpad initialization pipeline (`init_scratchpad`) modifies the hashing pipeline's seed (`tempHash`) in-place. The VM's first program runs with this state-modified seed.
4. **Cumulative Frequency Thresholds**: Cumulative opcode frequencies for floating-point operations (`FSUB_R`, `FSUB_M`, `FSCAL_R`, `FMUL_R`, `FDIV_M`, `FSQRT_R`, `CBRANCH`, `CFROUND`, `ISTORE`) match the standard configuration frequencies exactly.
5. **Zero-Initialization**: Integer registers `reg_.r` are zero-initialized on VM state setup to prevent garbage residues from leaking across program runs.

## Build

On an AArch64 Linux host:

```sh
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For cross compilation, provide an AArch64 CMake toolchain file and leave
`ARMRX_ENABLE_NATIVE` disabled.

To exercise the full shared light-mode cache initialization without mining or
network access:

```sh
./build/armrx --init-cache 'test key 000'
```

This allocates 256 MiB and runs the three Argon2d passes. It is a validation
tool only; a successful run does not yet make the program a miner.

The probe also accepts `--mode=auto|light|fast` and `--workers=N` to report the
selected memory mode for a specific worker count.

The current code can also initialize contiguous dataset output with the
light-mode cache scaffold, which is useful for validating the next dataset
construction step.

## Implementation order

1. BLAKE2b, including Argon2-compatible variable output/H' and its 1 KiB compression function, plus deterministic byte/word helpers (complete).
2. AES round primitive, AesGenerator1R/AesGenerator4R, Argon2d cache initialization, exact SuperscalarHash generation/execution, and exact on-demand dataset-item generation (complete).
3. Interpreted RandomX VM: register files, bytecode compiler, interpreted execution loop, scratchpad state, and final hashing (complete).
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
