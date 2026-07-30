# Track G NEON T-table fix — x4 round functions fail bit-exact parity test

## Bug
`test_aes_neon` assertion fails on trial 0:
```
Assertion failed: s0 == n0 && s1 == n1 && s2 == n2 && s3 == n3
(/home/mechres/armrx/tests/test_aes_neon.cpp: test_neon_ttable_x4_round_parity: 183)
```

The x4 functions in `include/armrx/aes.hpp` (encrypt_round_x4_neon, decrypt_round_x4_neon)
don't produce bit-identical results to calling aes_encrypt_round/aes_decrypt_round 4 times.

## Current code (include/armrx/aes.hpp, lines 251-301)

Both x4 functions call scalar `encrypt_transform`/`decrypt_transform` (same as the single-block
round functions), then batch the AddRoundKey XOR using NEON vld1q_u8 + veorq_u8 + vst1q_u8
instead of the byte-by-byte loop. The logic is structurally equivalent.

## Debugging steps needed

1. Test if `encrypt_transform(b0)` alone matches when called from x4 vs single-block context
   (narrows issue to transform vs XOR step).
2. Test if the NEON XOR `veorq_u8(vld1q_u8(b0.data()), vk)` produces the same result as the
   scalar loop for the same inputs.
3. Check if the `#if` guards around the x4 functions in `aes.hpp` are correct — the x4
   functions must be INSIDE `#if defined(__aarch64__) && defined(__ARM_NEON)` (the existing
   guard that includes `<arm_neon.h>`).
4. Check if `AesBlock` (`std::array<std::byte, 16>`) has sufficient alignment for NEON
   `vld1q_u8` (requires at least 16-byte alignment; `std::array<std::byte, 16>` has alignment 1
   by default, which works on most ARM implementations but could cause issues with certain
   compiler flags). Consider using `alignas(16)` or a local aligned buffer.

## Fix approach

The most likely root cause is alignment: `std::array<std::byte, 16>` may not be 16-byte aligned,
and NEON loads/stores to unaligned addresses can fault or produce wrong results depending on
compiler flags and system configuration. Fix by using aligned local buffers:

```cpp
inline void encrypt_round_x4_neon(...) {
    b0 = encrypt_transform(b0);  // etc.
    alignas(16) uint8_t buf0[16], buf1[16], buf2[16], buf3[16];
    std::memcpy(buf0, b0.data(), 16);
    std::memcpy(buf1, b1.data(), 16);
    // ... etc.
    uint8x16_t vs0 = veorq_u8(vld1q_u8(buf0), vk0);
    // ...
    vst1q_u8(buf0, vs0);
    std::memcpy(b0.data(), buf0, 16);
    // ...
}
```

Alternatively, check whether `AesBlock` can be `alignas(16)` without breaking other code.

Another possibility: the `#if` guard placement is wrong and the x4 functions end up OUTSIDE
the `#if defined(__aarch64__) && defined(__ARM_NEON)` block, making the NEON intrinsics
unavailable or causing a compilation mismatch. Verify that the `#endif` at line 303 is the
closing guard for the section containing the x4 functions (lines 251-301).

## Files
- `include/armrx/aes.hpp` — fix encrypt_round_x4_neon and decrypt_round_x4_neon
- No other files need changes

## Verification
```sh
taskset -c 3 ./test_aes_neon
# Must pass all tests including test_neon_ttable_x4_round_parity
```
