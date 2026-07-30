# Track G: NEON T-table AES Lookup Vectorization

## Context

`hash_aes_1r_x4` / `fill_aes_1r_x4` / `hash_and_fill_aes_1r_x4` in
`src/aes_hash.cpp` cost ~12.3% of all cycles — the single largest named C++
cost in the project's profiling. Currently they process 4 independent 16-byte
blocks sequentially per loop iteration, each block using scalar T-table
lookups in `encrypt_transform`/`decrypt_transform` (`include/armrx/aes.hpp`).

**Read these documents before starting:**
- `docs/plans/track-g-neon-ttable-aes-plan-20260728.md` — the handoff plan
- `docs/experiments/neon-vector-permute-aes.md` — prior (different) NEON attempt, -19.4% regression
- `docs/postmortems/aes-ttable-bug-postmortem.md` — AES T-table bug history

**CRITICAL: The existing ARMRX_ENABLE_NEON_AES flag gates a DIFFERENT mechanism**
(bit-sliced NEON AES round, already reverted with -19.4% measured). Do NOT reuse
that flag or touch that code path. Create a NEW flag: `ARMRX_ENABLE_NEON_TTABLE_AES`.

## What to do

### Step 1 — Read the current code

Read these files:
- `src/aes_hash.cpp` (lines 43-95: `fill_aes_1r_x4`, lines 97-130: `hash_aes_1r_x4`, lines 132-187: `hash_and_fill_aes_1r_x4`)
- `include/armrx/aes.hpp` (lines 244-266: `aes_encrypt_round`/`aes_decrypt_round`, switch between scalar T-table and NEON)
- `include/armrx/aes.hpp` (lines 40-95: `encrypt_transform`/`decrypt_transform` — the scalar T-table implementation)
- `include/armrx/aes.hpp` (lines 100-240: existing NEON AES code under `ARMRX_ENABLE_NEON_AES` — read but DO NOT modify)
- `tests/test_aes_hash.cpp` — existing golden-pin tests
- `CMakeLists.txt` — find how `ARMRX_ENABLE_NEON_AES` is wired (for the pattern to copy)
- `src/cpu_features.cpp` — find how NEON capabilities are detected at runtime

### Step 2 — Add the flag

Add `ARMRX_ENABLE_NEON_TTABLE_AES` to `CMakeLists.txt` (default OFF), following
the exact same structural pattern as `ARMRX_ENABLE_NEON_AES` (same section, same
comment style). Do NOT reuse or modify the existing flag.

### Step 3 — Implement the NEON T-table gather

The core idea: `encrypt_transform` and `decrypt_transform` each do 4 byte-granular
T-table lookups per round (Te0..Te3 or Td0..Td3), 256-entry × 4-byte tables.
Each 32-bit table entry is assembled from 4 byte loads. The `hash_aes_1r_x4`
functions process 4 independent blocks — NEON `tbl`/`tbx` can gather bytes from
the T-tables across all 4 lanes simultaneously instead of doing 16 scalar loads.

**Implementation approach:**
1. Create new functions `encrypt_transform_neon_ttable` and
   `decrypt_transform_neon_ttable` that operate on 4 `AesBlock`s at once
   (process all 4 lanes in one call using NEON).
2. These sit behind `#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_TTABLE_AES)`.
3. Add new overloads `aes_encrypt_round_x4`/`aes_decrypt_round_x4` that take
   4 states and 4 round keys and process them in NEON registers.
4. Create new `fill_aes_1r_x4_neon`, `hash_aes_1r_x4_neon`, and
   `hash_and_fill_aes_1r_x4_neon` variants that call the new x4 round functions.
5. Wire them into `src/aes_hash.cpp` with a runtime dispatch similar to how
   the existing NEON path is dispatched (check `src/cpu_features.cpp` for the
   pattern).

**Correctness constraint:** bit-exact equality with the scalar version is
MANDATORY. Every byte of every test hash must match exactly, not approximately.

### Step 4 — Tests

1. Verify existing `tests/test_aes_hash.cpp` passes with the flag OFF (no
   regression).
2. Build with flag ON and run:
   - `test_aes_hash` golden-pin tests
   - `ctest --test-dir build --output-on-failure` (full suite)
   - `test_jit_equivalence` (JIT/interpreter byte-identity)
   - `test_mining --kat-only` (KATs)

### Step 5 — Verify on hardware

Built with -DARMRX_ENABLE_NEON_TTABLE_AES=ON and -DARMRX_DISABLE_LTO=ON on the
AArch64 device (taskset -c 0, -j1 for build). Run benchmark:
`taskset -c 0-7 ./armrx --mine --seconds=150 --warmup=30`

## DO NOT

1. Modify the existing `ARMRX_ENABLE_NEON_AES` path or its `encrypt_transform_neon`.
2. Touch the hardware AESE/AESD instructions (they're spec-incompatible with RandomX).
3. Change the T-table contents, round count, or AddRoundKey ordering.
4. Expect this to automatically win — the sibling `vtbl` attempt measured -19.4%.
   This is a different mechanism but must be measured, not assumed.

## Files to modify

- `CMakeLists.txt` — add ARMRX_ENABLE_NEON_TTABLE_AES flag (default OFF)
- `src/aes_hash.cpp` — new NEON T-table variants of fill/hash/hash_and_fill
- `include/armrx/aes.hpp` — new x4 round functions and NEON T-table transforms
- Possibly: `src/cpu_features.cpp` — if runtime dispatch is needed

## Verification commands (on-device)

```sh
# Build
cd ~/armrx/build && taskset -c 0 cmake -S .. -DARMRX_ENABLE_NATIVE=ON -DARMRX_DISABLE_LTO=ON -DARMRX_ENABLE_NEON_TTABLE_AES=ON
taskset -c 0 cmake --build . -j1

# Test
taskset -c 3 ./test_aes_hash
ctest --test-dir build --output-on-failure

# Benchmark
taskset -c 0-7 ./armrx --mine --seconds=150 --warmup=30
```
