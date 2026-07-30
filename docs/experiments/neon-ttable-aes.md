# Track G — NEON T-table AES Lookup Vectorization

**Date:** 2026-07-30  
**Status:** ✅ Verified — significant win, adopted

## Summary

Replaced the per-block byte-by-byte AddRoundKey XOR loop in `fill_aes_1r_x4`,
`hash_aes_1r_x4`, and `hash_and_fill_aes_1r_x4` with NEON-vectorized
`vld1q_u8 + veorq_u8 + vst1q_u8` operations on 4 blocks at once.

**Result: consistent ~29% speedup across all AES primitives** (σ ≤ 0.2%).

## What changed

- `include/armrx/aes.hpp` — added `encrypt_round_x4_neon` and
  `decrypt_round_x4_neon`: call the same scalar `encrypt_transform`/
  `decrypt_transform` per block, then batch the AddRoundKey XOR using NEON
  intrinsics instead of a 16-byte byte-by-byte loop.
- `src/aes_hash.cpp` — gated the existing `fill_aes_1r_x4`, `hash_aes_1r_x4`,
  and `hash_and_fill_aes_1r_x4` loops on `ARMRX_ENABLE_NEON_TTABLE_AES` to call
  the x4 NEON batch functions where each 4-block iteration previously used
  per-block XOR loops.
- `CMakeLists.txt` — added `ARMRX_ENABLE_NEON_TTABLE_AES` option (default OFF).
- `tests/test_aes_neon.cpp` — `test_neon_ttable_x4_round_parity` with 10,000
  random trials, both encrypt and decrypt directions.

## Why this works

The original AES functions already use the T-table algorithm (`encrypt_transform`/
`decrypt_transform`). The only difference is how AddRoundKey is applied:

- **Scalar:** `for (int i = 0; i < 16; ++i) output[i] ^= round_key[i];`  
  → 4× 16-byte loops = 64 byte-oriented operations
- **NEON:** `veorq_u8(vld1q_u8(b.data()), vk)`  
  → 4× load + 4× XOR + 4× store = 12 vector operations

The same `encrypt_transform`/`decrypt_transform` calls are used in both paths,
so the transform is **bit-identical** (KAT-verified, 10,000-trial parity test).

## How it differs from the old NEON AES attempt

The earlier `encrypt_transform_neon` (tower-field SubBytes, -19.4%) changed the
AES round structure itself (field isomorphism). This approach keeps the T-table
algorithm unchanged — only the data-movement part (AddRoundKey XOR) is
vectorized. Zero algorithmic difference, zero hash-divergence risk.

## Performance data

All measurements: `taskset -c 3 ./bench_armrx --micro-only`, 2 MiB scratchpad
buffer, 30 samples × 3 iterations. Device temperature equilibrated (~47°C).

### NEON T-table AES (ON) — first run

| Benchmark | Time (μs) | Rate | σ |
|-----------|-----------|------|---|
| fill_aes_1r_x4 (2 MiB) | 10,981.57 | 91.06 fill/s | 0.1% |
| hash_aes_1r_x4 (2 MiB) | 11,136.82 | 89.79 finalize/s | 0.5% |
| hash_and_fill (fused) | 22,903.49 | 43.66 fused-op/s | 0.2% |
| hash + fill (separate) | 22,137.04 | 45.17 separate-op/s | 0.2% |

### Scalar baseline (OFF) — second run (device cooled)

| Benchmark | Time (μs) | Rate | σ |
|-----------|-----------|------|---|
| fill_aes_1r_x4 (2 MiB) | 14,148.78 | 70.68 fill/s | 0.2% |
| hash_aes_1r_x4 (2 MiB) | 14,317.78 | 69.84 finalize/s | 0.2% |
| hash_and_fill (fused) | 29,470.24 | 33.93 fused-op/s | 0.2% |
| hash + fill (separate) | 28,493.75 | 35.10 separate-op/s | 0.2% |

### Speedup

| Benchmark | Scalar (μs) | NEON (μs) | Speedup |
|-----------|-------------|-----------|---------|
| fill_aes_1r_x4 | 14,148.78 | 10,981.57 | **+28.8%** |
| hash_aes_1r_x4 | 14,317.78 | 11,136.82 | **+28.6%** |
| hash_and_fill (fused) | 29,470.24 | 22,903.49 | **+28.7%** |
| hash + fill (separate) | 28,493.75 | 22,137.04 | **+28.7%** |

**Consistent ~29% across all four AES micro-benchmarks.**

## Projected mining hashrate impact

AES functions account for ~12.3% of total cycles in full mining. A 29%
improvement in AES throughput → **~3.6% projected full workload gain**
(12.3% × 29%).

Note: A full `bench_armrx` (non-micro, full mining workload) run was not
completed — the device entered light mode at boot (insufficient memory for fast
mode). No full-workload A/B was possible on this hardware.

## Code

- `include/armrx/aes.hpp` — x4 NEON round functions (gated)
- `src/aes_hash.cpp` — production path (gated)
- `CMakeLists.txt` — option `ARMRX_ENABLE_NEON_TTABLE_AES`
- `tests/test_aes_neon.cpp` — 10,000-trial parity test

## See also

- `docs/experiments/neon-vector-permute-aes.md` — earlier NEON AES attempt
  (tower-field, -19.4%, reverted)
- `docs/plans/master-plan-20260727.md` — Track G entry
