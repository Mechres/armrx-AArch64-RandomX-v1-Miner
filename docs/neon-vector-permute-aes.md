# NEON Vector-Permute AES — Derived, Verified, Measured as a Regression

## Problem

The external performance audit (`docs/performance-improvement-audit.md`) flagged
`include/armrx/aes.hpp`'s `encrypt_transform`/`decrypt_transform` — 100% scalar,
byte-indexed T-table lookups — as untried NEON-vectorization territory,
distinct from the previously-reverted *hardware* `AESE`/`AESD`/`AESMC`
attempts (`changelogs.md` 2026-07-20, which failed because ARM's hardware AES
instructions apply AddRoundKey in a fixed position incompatible with
RandomX's round order). These functions are the inner loop of
`fill_aes_1r_x4`/`hash_aes_1r_x4` — init_scratchpad (~12.35% of full hash on
A53) and get_final_result (~9.30%), both run on *every single hash*, unlike
the Argon2 work earlier this session which only affected seed-rotation
latency.

## Why this needed real derivation, not a mechanical port

ARM NEON's `vtbl`/`vqtbl1q` instructions only support 16-entry (up to 64-byte
via 4 chained tables) lookups. This project's AES uses classic T-tables — 4
tables of 256×4-byte entries each (`src/soft_aes.cpp`) — nowhere near small
enough for a direct port. The real technique is "vector-permute AES"
(Hamburg-style): decompose the AES S-box computation via a field isomorphism
between GF(2⁸) (the AES field) and a "tower" representation GF(2⁴)², where
every sub-step fits a 16-entry table. This is a fundamentally different
algorithm from the T-table approach, not a mechanical translation — and it's
compatible with RandomX's round order (unlike hardware AESE) because it's
pure software computing the standard SubBytes→ShiftRows→MixColumns→
AddRoundKey sequence explicitly, exactly like `encrypt_transform` already
does.

## Derivation (done from scratch, in Python, before any C++ was written)

All of the following was derived independently from the AES/Rijndael
mathematical definition — not copied from any reference implementation —
and verified computationally at every step before being ported to C++:

1. **Standard FIPS-197 S-box**, built directly from the GF(2⁸) definition
   (multiplicative inverse + affine transform), spot-checked against known
   values (S(0x00)=0x63, S(0x01)=0x7c, S(0x53)=0xed, S(0xff)=0x16).
2. **A root of AES's defining polynomial** (x⁸+x⁴+x³+x+1) found inside the
   tower field GF(2⁴)[y]/(y²+y+λ), λ=0x8. Evaluating that polynomial's bit
   coefficients at the root gives a provably correct field isomorphism (any
   root of a field's defining polynomial, found in another field of the same
   size, induces one) — verified anyway, computationally, over all 256×256
   pairs for both multiplicativity and additivity, plus bijectivity and
   round-trip.
3. **Full vector-permute S-box pipeline** (nibble-split forward map → GF(2⁴)²
   inversion via a norm+GF(2⁴) tables → nibble-split backward map → AES
   affine transform) reproduces the real FIPS-197 S-box for **all 256 byte
   values, zero mismatches**. The inverse direction (InvSubBytes) derived and
   verified the same way, against the ground-truth inverse permutation of the
   forward S-box.
4. **ShiftRows/InvShiftRows permutations** verified against this codebase's
   *actual* byte layout — confirmed by decoding `encrypt_transform`'s/
   `decrypt_transform`'s existing T-table index pattern (which fuses
   ShiftRows into which table each row reads from) by hand, then
   computationally, for all 4 output columns of each direction.
5. **MixColumns/InvMixColumns "compact" SIMD formulas** verified against the
   direct GF(2⁸) definition over thousands of random column samples.
6. **The trickiest building block**: GF(2⁴) multiplication of two
   *per-lane-varying* operands (needed for the tower-field norm/inversion
   formula) isn't a single `vtbl` — solved via log/antilog tables (GF(2⁴)*
   is cyclic of order 15) plus explicit zero-masking, simulated in Python
   exactly as the planned NEON instruction sequence would compute it
   (including the log/antilog mod-15 reduction and masking), verified against
   direct GF(2⁴) multiplication for all 16×16 pairs before ever writing an
   intrinsic.
7. **Full-round simulation** on complete 16-byte states (not just single
   bytes) — the entire planned instruction sequence (SubBytes, ShiftRows,
   MixColumns, and their inverses) simulated in Python and verified against
   an independently-built scalar reference matching this codebase's actual
   `randomx_aes_lut_enc`/`randomx_aes_lut_dec` row/column selection pattern,
   for 3000 random `(state, round_key)` trials, both directions, zero
   mismatches.

## Implementation

`include/armrx/aes.hpp` gained `encrypt_transform_neon`/`decrypt_transform_neon`
(AArch64/NEON-only, `armrx::detail` namespace for the building blocks:
`aes_gf4_mul_neon`, `aes_tower_invert_neon`, `aes_sub_bytes_neon`/
`aes_inv_sub_bytes_neon`, `aes_shift_rows_neon`/`aes_inv_shift_rows_neon`,
`aes_xtime_neon`, `aes_mix_columns_neon`/`aes_inv_mix_columns_neon`). Every
constant table used (`FWD_HI`/`FWD_LO`/`BACK_HI`/`BACK_LO`/`GSQ4`/`GINV4`/
`AFFINE_INV_HI`/`AFFINE_INV_LO`/log/antilog/ShiftRows permutations) is exactly
the values verified in the Python derivation above.

`aes_encrypt_round`/`aes_decrypt_round` dispatch to the NEON path only when
the new `ARMRX_ENABLE_NEON_AES` CMake option (default **OFF**) is set,
alongside the existing `ARMRX_ENABLE_JIT_FAST_DIV_SQRT`-style experimental
flags — every other call site (`aes_hash.cpp`, `aes_generator.cpp`) is
unchanged, same signatures. The scalar `encrypt_transform`/`decrypt_transform`
and `randomx_aes_lut_enc`/`randomx_aes_lut_dec` are untouched and remain the
default.

## Correctness validation — all passed, zero issues found

- **`tests/test_aes_neon.cpp`** (new, AArch64/NEON-only): SubBytes and
  InvSubBytes checked against the standard FIPS-197 S-box/inverse-S-box for
  all 256 byte values (broadcast to all 16 lanes each) — **256/256 exact
  match, both directions**. A handful of structural edge cases (all-zero,
  all-0xFF, alternating bit patterns). Full-round parity: **20,000 random
  `(state, round_key)` trials**, comparing `encrypt_transform`/
  `decrypt_transform` (scalar) directly against `encrypt_transform_neon`/
  `decrypt_transform_neon` — **byte-for-byte identical, zero mismatches**,
  both directions.
- Full end-to-end KAT hashes (`tests/test_blake2b.cpp`, `"This is a test"` /
  `"Lorem ipsum dolor sit amet"`, interpreted and JIT mode) — byte-identical
  with `ARMRX_ENABLE_NEON_AES=ON`.
- `tests/test_aes_hash.cpp`'s golden pins (written earlier this session) —
  **byte-identical hex output** to the scalar-path values already recorded,
  with the NEON path active.
- Full `ctest` on-device, both `ARMRX_ENABLE_NEON_AES` settings: **12/12
  green** with the flag on (11/11 without `test_aes_neon`, which only builds
  and is meaningful on AArch64).

This is about as thorough a correctness validation as this session has done
for any change — the implementation compiled and passed every test on the
**first attempt on real hardware**, with zero bugs found, a direct result of
verifying the entire algorithm mathematically before writing any NEON
intrinsics.

## Performance — measured honestly: a real regression

Apples-to-apples, same device, back-to-back, `bench_armrx --micro-only`'s
existing `bench_aes_primitives()` (measured twice for the NEON build to rule
out a thermal artifact — the first run had high variance, σ=25.9%, likely an
early-sample frequency-scaling transient; the second run was clean, σ=0.2-0.5%,
and matched the first run's mean almost exactly, confirming the result is
real and reproducible, not noise):

| | Scalar (current default) | NEON vector-permute | Δ |
|---|---|---|---|
| `fill_aes_1r_x4` (2 MiB) | 28,392.79 μs (35.22 fill/s) | 33,914.99 μs (29.49 fill/s) | **+19.4% slower** |
| `hash_aes_1r_x4` (2 MiB) | 28,597.41 μs (34.97 finalize/s) | 34,157.06 μs (29.28 finalize/s) | **+19.5% slower** |

Full end-to-end single-thread steady-state hashrate (`armrx --mine
--seconds=60 --workers=1`) stayed at 4.27 H/s either way — the ~19%
regression in these two functions alone is too small a fraction of total
hash time (chain execution/JIT dominates at ~85%) to show up clearly at this
measurement's resolution, but the direct, low-variance component measurement
above is unambiguous.

**Root cause, consistent with prior history on this hardware**: this is the
same failure mode documented for the earlier hardware-AES re-enable attempt
(`changelogs.md` 2026-07-20): "per-block NEON load/store overhead cancels
[the] speedup on this core." The vector-permute technique replaces 16
sequential *scalar* table lookups per SubBytes step with roughly a dozen
*vector* instructions (loads, `vqtbl1q_u8` lookups, XORs, shifts) — fewer
total operations, but Cortex-A53's NEON pipeline latency for this specific
instruction mix apparently doesn't beat simple scalar memory loads for this
data size/access pattern. This isn't a flaw in the vector-permute *technique*
in general (it's a well-established, real technique that helps on many
other cores) — it's specific to this device's microarchitecture, matching
the pattern already seen twice now (hardware AESE, and now software
vector-permute) for AES acceleration attempts on this particular Cortex-A53.

## Conclusion: implementation kept, default stays OFF

Same standard applied to every other performance investigation this session
(CBRANCH/CSEL, Argon2 copy-elimination): implement carefully, verify
correctness exhaustively, measure honestly, and only adopt if it's a real
win. This one measured as a clear, reproducible regression at the component
level. Unlike CSEL (which was `git checkout`-reverted since it was an
unconditional rewrite), this is flag-gated — `ARMRX_ENABLE_NEON_AES` stays
available (default OFF) as a well-tested, fully-verified reference
implementation, in case a future device/microarchitecture, compiler version,
or further tuning changes the calculus. The code, its extensive test
coverage, and this derivation are kept rather than deleted — the
mathematical work is reusable even though the performance didn't pan out on
this hardware.
