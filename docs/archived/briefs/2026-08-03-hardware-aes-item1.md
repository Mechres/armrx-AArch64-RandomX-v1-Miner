# Brief: Hardware AES for scratchpad fill/hash (Item 1) — armrx aarch64

**Date:** 2026-08-03
**For:** Reasonix (execute). Author: Hermes (diagnostic/instrumentation mode — user waiver 2026-08-01 for DIAGNOSTIC only; this is a perf change routed through Reasonix per normal rule).
**Baseline (locked, Item 0 done):** post-W4 `a1ea83c`, non-isolated, gated W11/T1-2 protocol, core 3:
`107.36M instr/hash · 171.19M cycles/hash · IPC 0.627 · median 220.59 ms`.

> **Correction note vs the roadmap audit (docs/audits/residual-gap-optimization-roadmap.md §3):** that plan described a
> non-existent single `transform` helper and a wrong guard macro (`__ARM_FEATURE_CRYPTO`), and estimated ~200 lines. The
> real mechanism (verified by reading the code) is below — it is ~25 lines and touches exactly one file functionally.

---

## 0. Root cause (why AES is hand-rolled in T-tables instead of hardware)

`include/armrx/aes.hpp:88-96` carries a **misdiagnosis**: *"AESE fixes AddRoundKey's position in the round, so it cannot be
used to implement the RandomX AES."* The RandomX round order is **SubBytes → ShiftRows → MixColumns → AddRoundKey** (last).
AESE performs **SubBytes → ShiftRows → AddRoundKey → MixColumns** (AddRoundKey *first*). Feeding AESE a **zero key** makes its
leading AddRoundKey a no-op, then `vaesmcq_u8` does MixColumns, and the existing trailing `EOR key` applies the real key —
reproducing the RandomX order **exactly**. Same for AESD + AESIMC + EOR. This is the "compensating transformation" the
postmortem named but didn't apply.

**Proof it is correct on this silicon:** `src/jit_compiler_a64_static.S:425-489` already uses this exact construction for the
v2 FE_mix block (`aese vN, v28` with `movi v28.4s,0` zero key + `aesmc` + `eor vN,key`), and upstream ran `--verify` on this
device in W1-4 with `RANDOMX_FLAG_HARD_AES` (default) through `intrin_portable.h:476-484` — the identical decomposition.

---

## 1. The real mechanism (verified call graph)

`aes_encrypt_round` / `aes_decrypt_round` (aes.hpp:305, :316) pick the primitive:
- `#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_AES)` (Track G tower-field, **default OFF**)
  → `encrypt_transform_neon` / `decrypt_transform_neon`.
- `#else` → `encrypt_transform` / `decrypt_transform` (the **scalar T-table functions**, aes.hpp:21 / :53).

The **default build** (`ARMRX_ENABLE_NEON_TTABLE_AES=ON`, `ARMRX_ENABLE_NEON_AES=OFF`) does NOT go through
`aes_encrypt_round` at all for the hot loops. Instead, the 5 x4 functions call `encrypt_transform` / `decrypt_transform`
**directly** inside the `ARMRX_ENABLE_NEON_TTABLE_AES` block:
- `src/aes_hash.cpp`: `fill_aes_1r_x4` (:53-56), `fill_aes_4r_x4` (:103-118), `hash_aes_1r_x4` (:164-167, :192-195, :208-211),
  `hash_and_fill_aes_1r_x4` (:261-264, :280-283, :323-326, :339-342), `hash_and_fill_aes_interleaved_x4` (:400-403, :419-422, :462-465).
- `encrypt_round_x4_neon` / `decrypt_round_x4_neon` (aes.hpp:251, :279) also call `encrypt_transform` / `decrypt_transform`
  internally, then do a NEON batch AddRoundKey.

`aes_generator.cpp:25,27,50,54` calls `aes_decrypt_round` / `aes_encrypt_round`, whose else-branch (NEON_AES OFF) is the same
`decrypt_transform` / `encrypt_transform`.

**Conclusion: `encrypt_transform` / `decrypt_transform` (aes.hpp:21 / :53) are the SINGLE FUNNEL for every AES path on
aarch64** (both Track-G-TTABLE ON via the x4 fns, and Track-G OFF via `aes_encrypt_round`, and `aes_generator`). Overriding
those two functions with the zero-key hardware form propagates hardware AES to ALL call sites with **zero changes** to
`aes_hash.cpp`, `aes_generator.cpp`, or `CMakeLists.txt`. The change is ~25 lines in `aes.hpp` only.

---

## 2. Exact change — `include/armrx/aes.hpp`

### 2a. Replace the two scalar T-table function definitions (lines 21-83) with: T-table kept under a new
###     always-available name + a hardware funnel selected at compile time.

Replace this:

```cpp
[[nodiscard]] inline AesBlock encrypt_transform(AesBlock input) {
    ... existing T-table body (lines 22-50) ...
}

[[nodiscard]] inline AesBlock decrypt_transform(AesBlock input) {
    ... existing T-table body (lines 54-82) ...
}
```

with:

```cpp
// Scalar T-table transforms — ALWAYS available (non-crypto fallback + KAT oracle).
[[nodiscard]] inline AesBlock encrypt_transform_ttable(AesBlock input) {
    ... existing encrypt_transform T-table body ...
}

[[nodiscard]] inline AesBlock decrypt_transform_ttable(AesBlock input) {
    ... existing decrypt_transform T-table body ...
}

// Hardware-AES funnel for aarch64 + crypto. RandomX round order is
// SubBytes->ShiftRows->MixColumns->AddRoundKey(last). AESE does AddRoundKey FIRST,
// so feed it a ZERO key (no-op), let AESMC do MixColumns, then the caller applies
// the real key as a trailing XOR (in aes_encrypt_round / the x4 helpers). This is
// byte-identical to encrypt_transform_ttable and matches armrx's JIT v2 FE_mix
// (jit_compiler_a64_static.S:425-489). Proven on this silicon in W1-4 (upstream
// intrin_portable.h:476-484).
#if defined(__aarch64__) && defined(__ARM_FEATURE_AES)
[[nodiscard]] inline AesBlock encrypt_transform(AesBlock input) {
    const uint8x16_t z = vdupq_n_u8(0);
    const uint8x16_t s = vld1q_u8(reinterpret_cast<const uint8_t*>(input.data()));
    const uint8x16_t r = vaesmcq_u8(vaeseq_u8(s, z));
    AesBlock output{};
    vst1q_u8(reinterpret_cast<uint8_t*>(output.data()), r);
    return output;
}
[[nodiscard]] inline AesBlock decrypt_transform(AesBlock input) {
    const uint8x16_t z = vdupq_n_u8(0);
    const uint8x16_t s = vld1q_u8(reinterpret_cast<const uint8_t*>(input.data()));
    const uint8x16_t r = vaesimcq_u8(vaesdq_u8(s, z));
    AesBlock output{};
    vst1q_u8(reinterpret_cast<uint8_t*>(output.data()), r);
    return output;
}
#else
[[nodiscard]] inline AesBlock encrypt_transform(AesBlock input) {
    return encrypt_transform_ttable(input);
}
[[nodiscard]] inline AesBlock decrypt_transform(AesBlock input) {
    return decrypt_transform_ttable(input);
}
#endif
```

Notes:
- `__ARM_FEATURE_AES` is defined (=1) under the existing `-march=armv8-a+crypto` (`armrx_core`, CMakeLists.txt:67).
  **Do NOT** gate on `__ARM_FEATURE_CRYPTO` — it is NOT reliably defined by this toolchain/flag combo; `__ARM_FEATURE_AES`
  is the correct, verified macro. (Confirmed via `aarch64-linux-musl-g++ -march=armv8-a+crypto -dM -E`: both are 1, but
  use AES to match upstream's own guard style.)
- `<arm_neon.h>` is already included at aes.hpp:8-10 for aarch64, so the AES intrinsics are in scope.
- `AesBlock = std::array<std::byte,16>`; `reinterpret_cast<uint8_t*>(output.data())` mirrors the existing
  `encrypt_transform_neon` (aes.hpp:228), which proves the cast/layout is correct.
- The `vaese`/`vaesmc` hardware path expects the 16-byte block laid out as the AES state (bytes 0-3 = column 0), which is
  exactly how `encrypt_transform_ttable` reads `input[0..3]` as `s0`. So byte layout matches — confirmed by `.S:425-489`.

### 2b. Optional (consistency, NOT required for correctness): prefer hardware over Track-G when both present.

If `ARMRX_ENABLE_NEON_AES` (Track G) is ever turned ON, `aes_encrypt_round` (aes.hpp:305-314) routes to the tower-field
`encrypt_transform_neon`, while the x4 loops route to hardware — two different AES implementations running simultaneously.
Both are correct, but for a single consistent fast path, change the two `#if` guards at aes.hpp:306 and :317 from

```cpp
#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_AES)
```

to

```cpp
#if defined(__aarch64__) && defined(__ARM_NEON) && defined(ARMRX_ENABLE_NEON_AES) && !defined(__ARM_FEATURE_AES)
```

so hardware wins when available. **Skip this unless the human wants Track-G disabled automatically on crypto silicon.**
Track-G is OFF by default, so leaving it alone is fine.

### 2c. Files confirmed UNCHANGED
- `src/aes_hash.cpp` — the 5 functions keep calling `encrypt_transform`/`decrypt_transform` directly; the override makes them hardware.
- `src/aes_generator.cpp` — keeps calling `aes_encrypt_round`/`aes_decrypt_round`; the else-branch routes through the overridden funnel.
- `CMakeLists.txt` — `-march=armv8-a+crypto` already on `armrx_core` (line 67); no flag change needed.

---

## 3. Oracle / KAT tool — `tools/aes_kat_check.cpp`

The existing tool only *prints* `aes_encrypt_round(initial, rk)` (no assertion). Extend it to be a real oracle:

1. **Known-answer check (FIPS-197 single-round vector):** assert `aes_encrypt_round(initial, rk)` equals the published
   value for the standard AES test vector (or, simpler and sufficient, the exact 32 hex chars currently printed by the
   tool on a KNOWN seed — capture them once from the current T-table build as the golden pin, since the hardware path
   MUST be byte-identical).
2. **hw-vs-T-table cross-check over random blocks:** for N ≥ 10000 random `(state, key)` pairs, assert
   `aes_encrypt_round(state, key)` == `encrypt_transform_ttable(state) ^ key` and
   `aes_decrypt_round(state, key)` == `decrypt_transform_ttable(state) ^ key`, byte-for-byte. This catches any
   AESD/AESIMC lane-pairing error deterministically, with no spec reasoning — it just compares the two implementations.
3. (The `test_aes_hash` golden pins and `test_mining` known-hash KATs are the end-to-end oracles; see gate below.)

---

## 4. Verification gate (host → device) — DO NOT SKIP ORDER

1. **Host x86_64:** `cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON && cmake --build build -j && ctest --test-dir build --output-on-failure` — must be **100% unchanged** (hardware funnel compiled out; T-table path is the fallback).
2. **Cross-build → device, correctness FIRST (fast gates):**
   - `tools/aes_kat_check` (hw == T-table over ≥10k random blocks, plus golden pin) — the decisive oracle.
   - `test_aes_hash` (**golden pins UNCHANGED — do not edit expected values**).
   - `test_blake2b` (spec KATs).
   - `test_mining` (end-to-end known-hash KATs).
   - `test_jit_equivalence` 16/16, `test_jit_determinism`, `test_jit_dataset_2way`.
3. **⚠ CRITICAL CAVEAT:** the differential tests (JIT vs interpreter) will **NOT** catch a broken AES — both sides share
   `aes_hash.cpp` and would be wrong *together*. The real oracles are `aes_kat_check` (hw-vs-T-table), `test_aes_hash`
   golden pins, `test_mining`, and one **cross-implementation KAT**: mine one share on-device (share validation = ultimate
   oracle; already exercised in W1-4).
4. **Perf A/B (device, T1-2 discipline):** B-M-B-M, `taskset -c 3 perf stat -e cycles,instructions` on
   `bench_armrx --full-hash-only --perf-ready`, per-run md5 of the executed binary. Adopt if instructions drop to ~97-98M
   and cycles improve (expect ~163M). Slow scheduler stress tests are orthogonal (no scheduler contact) — human re-runs separately.

---

## 5. Risks & doc follow-ups (for after adoption — NOT part of this code PR)

- **R4 (institutional):** `include/armrx/aes.hpp:88-96`, `AGENTS.md` ("NEON AES paths are disabled — AESE/AESD incompatible
  ordering"), and `docs/postmortems/aes-ttable-bug-postmortem.md` currently assert AESE incompatibility as fact. After the
  change is verified, amend these in a SEPARATE doc commit: the postmortem's own "not interchangeable *without compensating
  transformations*" sentence is the hook — the compensation is the zero key, and `jit_compiler_a64_static.S:425-489` already
  embodies it. Do NOT edit these docs as part of the correctness gate.
- **R1 (correctness, LOW):** lane pairing wrong → caught by `aes_kat_check` hw-vs-T-table + `test_aes_hash` golden pins deterministically, before any mining.
- **R2 (perf, LOW):** if hardware somehow not faster, the `#else` T-table fallback remains; Track-G retained.
- **R3 (host drift, LOW):** guarded by `__aarch64__` + `__ARM_FEATURE_AES`; x86_64 build untouched.

---

## 6. Prediction to beat (vs locked baseline)

| Metric | Baseline (W4) | Post-AES (predicted) |
|---|---:|---:|
| Instructions/hash | 107.36M | ~97-98M |
| Cycles/hash | 171.19M | ~163M |
| Median hash | 220.59 ms | ~210 ms |

Adopt if instructions drop ≥ ~8M and cycles improve ≥ ~2% in both modified runs.
