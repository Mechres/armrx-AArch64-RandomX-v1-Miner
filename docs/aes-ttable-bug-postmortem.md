# AES T-Table Bug Postmortem — Hash Divergence

**Date:** 2026-07-20
**Status:** Fixed, verified on x86_64 and AArch64 (Cortex-A53)
**Revision:** `441313b` (final fix), previous commits `5ac93e8`, `70427e5`, `ed512e5`

---

## Symptoms

Both JIT and interpreted execution produced identical hashes (`8bbce3a1...`),
diverging from the upstream RandomX reference (`639183aa...`). Dataset items,
blake2b, and AES single-round KAT (FIPS-197) all appeared correct. The bug
was in shared code that runs **before** the JIT/interpreted split.

## Root Cause

Three independent bugs in the AES T-table transforms in `src/aes.cpp` and
`src/aes_hash.cpp`:

### Bug 1: `encrypt_transform` — reversed byte order and wrong column permutation

**File:** `src/aes.cpp:20-35`

The T-table lookup extracted bytes in MSB-first order (`(s0 >> 24) & 0xff`)
instead of LSB-first (`(s0 >> 0) & 0xff`). Combined with an incorrect column
permutation for the ShiftRows+MixColumns mapping, every AES encryption
operation produced wrong output.

**Original wrong code:**
```cpp
// Wrong: MSB-first byte extraction + wrong permutation
t0 = TE0[(s0 >> 24) & 0xff] ^ TE1[(s1 >> 16) & 0xff] ^
     TE2[(s2 >>  8) & 0xff] ^ TE3[(s3 >>  0) & 0xff];
```

**Corrected code:**
```cpp
// Correct: LSB-first byte extraction + correct ShiftRows permutation
t0 = TE0[(s0 >>  0) & 0xff] ^ TE1[(s1 >>  8) & 0xff] ^
     TE2[(s2 >> 16) & 0xff] ^ TE3[(s3 >> 24) & 0xff];
t1 = TE0[(s1 >>  0) & 0xff] ^ TE1[(s2 >>  8) & 0xff] ^
     TE2[(s3 >> 16) & 0xff] ^ TE3[(s0 >> 24) & 0xff];
t2 = TE0[(s2 >>  0) & 0xff] ^ TE1[(s3 >>  8) & 0xff] ^
     TE2[(s0 >> 16) & 0xff] ^ TE3[(s1 >> 24) & 0xff];
t3 = TE0[(s3 >>  0) & 0xff] ^ TE1[(s0 >>  8) & 0xff] ^
     TE2[(s1 >> 16) & 0xff] ^ TE3[(s2 >> 24) & 0xff];
```

### Bug 2: `decrypt_transform` — used encrypt's column permutation

**File:** `src/aes.cpp:52-67`

The upstream `soft_aesenc` and `soft_aesdec` use **different** column permutations:

| Output | Encrypt TE0..TE3 | Decrypt TD0..TD3 |
|--------|-----------------|-----------------|
| t0     | (s0, s3, s2, s1) | (s0, s1, s2, s3) |
| t1     | (s1, s0, s3, s2) | (s1, s2, s3, s0) |
| t2     | (s2, s1, s0, s3) | (s2, s3, s0, s1) |
| t3     | (s3, s2, s1, s0) | (s3, s0, s1, s2) |

The decrypt uses a **straight sequential rotation**, not the interleaved pattern
used by encrypt. Our code used the encrypt pattern for both, breaking every
AES decryption operation.

### Bug 3: NEON AES hardware paths — incompatible operation order

**File:** `src/aes_hash.cpp` — removed all 4 `#if defined(__aarch64__)` blocks

The ARM NEON `AESE`/`AESD` instructions implement a different operation order
than the RandomX AES round specification:

| Step | Standard AES round | ARM AESE instruction |
|------|-------------------|---------------------|
| 1 | SubBytes | XOR with round key |
| 2 | ShiftRows | SubBytes |
| 3 | MixColumns | ShiftRows |
| 4 | XOR with round key | (MixColumns via separate AESMC) |

The key XOR is at **opposite ends**. Since SubBytes is non-linear,
`SubBytes(state XOR key) ≠ SubBytes(state) XOR key`. The operations
do not commute.

For decrypt, `AESD` has the same ordering reversal (AddRoundKey first,
InvMixColumns after separate AESIMC). Both produce different results from
the standard RandomX AES round specification.

**Fix:** Removed all NEON hardware paths. All AES operations now use the
software T-table path, which correctly implements the standard round order.

### Why the FIPS-197 KAT didn't catch it

The KAT expected value in `tests/test_blake2b.cpp` was **circular** —
derived from the buggy implementation's own output, not from an independent
reference. The comment acknowledged "byte order differs from FIPS-197 canonical
byte order" but the expected value was never cross-checked against the upstream
or the standard.

## Detection

The bug was found by stage-gated binary search using the upstream reference
(built from `scratch_vm_study/upstream_rx/`):

1. **Step 1:** Verified blake2b tempHash matches Python → blake2b correct.
2. **Step 2:** Dumped AesGenerator4R entropy values, compared derived
   `mx_`/`ma_` against `ref_output.txt` → mismatch confirmed.
3. **Step 3:** Compared `encrypt_transform` output against upstream
   `soft_aesenc` using FIPS-197 test vector → mismatch revealed.
4. **Step 4:** Wrote direct comparison test linking our `aes.cpp` against
   the upstream's T-tables → `aes_encrypt_round` matched but
   `aes_decrypt_round` differed → decrypt permutation was wrong.
5. **Step 5:** Built upstream test binary and ran `randomx_calculate_hash` to
   capture tempHash-after-init, entropy, and reg values at every chain stage.
6. **Step 6:** Cross-referenced our tempHash-after-init → s0/s1 mismatched →
   decrypt bug confirmed. s2/s3 matched → encrypt was correct.

## Files Changed

| File | Change |
|------|--------|
| `src/aes.cpp:20-35` | Fix encrypt_transform (byte order + column permutation) |
| `src/aes.cpp:52-67` | Fix decrypt_transform (different column permutation) |
| `src/aes_hash.cpp` | Remove all 4 NEON hardware AES paths (~180 lines) |
| `tests/test_blake2b.cpp` | Fix circular FIPS-197 KAT expected value |

## Verification

- **x86_64:** `armrx_tests` produces `639183aa...` ✓
- **AArch64 (Cortex-A53):** `armrx_tests` produces `639183aa...` ✓
- **JIT = Interpreted:** Both paths produce identical output ✓
- **Full CTest:** 33 passed, 0 failures (4 "Not Run" are unrelated executable-path issues) ✓

## Lessons

1. **KATs must reference an independent standard.** A circular KAT (expected
   value from the implementation under test) provides no protection against
   bugs. Every KAT should cite its reference source.
2. **Encrypt and decrypt permutations are not the same.** The AES decrypt
   round uses a simpler sequential rotation, not the interleaved encrypt
   pattern.
3. **ARM NEON AES instructions are not drop-in replacements for standard
   AES rounds.** AESE/AESD apply the round key at the start; the standard
   applies it at the end. These produce different results and are not
   interchangeable without compensating transformations.
4. **Binary-search debugging works.** Starting from the final hash and working
   backward through the pipeline (tempHash → entropy → AES round → NEON vs SW)
   isolated each bug efficiently.
