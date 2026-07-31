# W1-3 — Blake2b instruction-share close

**Date:** 2026-07-31
**Status:** Closed — no action required (NEON path active, share ≈ 0.5%)

## Question
N3 (Blake2b NEON check) was "mostly closed as missing NEON" — the NEON path exists.
W1-3 closes the residual: (a) what share of the 119 M instr/hash does Blake2b occupy,
and (b) does the scalar fallback ever run on the target device?

## Findings
1. **NEON path is compiled and active on device.** `src/blake2b.cpp:9,71` gates the
   NEON compress on `__aarch64__ && __ARM_NEON`; the device build uses
   `-march=armv8-a+crypto` (NEON always present on AArch64), so the scalar fallback
   at `blake2b.cpp:185` is **dead on this target** (compiled out / never taken).
2. **Share of total is negligible.** From the W1-1 census, the "named C++" region is
   9.5% (11.33 M instr/hash) of which AES ≈ 10.7 M (NEON T-table). The remainder —
   Blake2b finalization + VM glue — is **≈ 0.6 M instr/hash ≈ 0.5%** of the 119 M total.
3. **No win available.** Even a hypothetical 2× speedup of the 0.5% slice would yield
   ~0.25% hashrate — below noise. The N3 "missing NEON" hypothesis is disproven:
   the NEON path is present and is what runs.

## Conclusion
Blake2b is **not** a meaningful lever. The instruction-gap story is dominated by the
superscalar body (80.5% / 95.77 M) and, secondarily, the main-VM JIT (9.9% / 11.79 M,
IPC 0.405 — the memory-stall region). W1-3 is closed with no code change.

## Evidence
- `src/blake2b.cpp` (NEON guard at lines 9/71, scalar fallback at 185 — unreachable on AArch64 build)
- `docs/experiments/w11-instruction-census.md` (region attribution: named C++ 9.5%, AES ≈10.7M)
- AGENTS.md (AArch64 build: `-march=armv8-a+crypto`, JIT + NEON auto-enabled)
