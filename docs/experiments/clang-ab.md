# Clang cross-build A/B (Phase 1.2 / GLM Tier 1-B)

**Date:** 2026-08-05 (session clock)
**Author:** Hermes
**Status:** DONE — NULL (Clang does not reduce instruction count vs GCC)
**Purpose:** Era II Phase 1b cheapest probe. GCC 16.1.0 cross-build gave +7.9% over device
GCC 15.2.0 with zero source change; test whether Clang's AArch64 codegen reduces the +15%
instruction-count gap vs XMRig (Phase 0: armrx 113.8M vs XMRig 98.9M).

## Method
- New toolchain file `cmake/toolchain-aarch64-musl-clang.cmake`: host `clang`/`clang++` as
  drivers, `--target=aarch64-linux-musl --sysroot=/usr/aarch64-linux-musl -mcpu=cortex-a53
  -fuse-ld=lld`. Mirrors the GCC toolchain (host-ar pins for the AUR cross-ar hang, LTO off,
  pkg-config disabled). Built `build-clang`.
- KAT gate: `test_jit_equivalence` 16/16 byte-identical (equivalence-safe). ✅
- Measured with `bench_armrx --full-hash-only --perf-ready` (gated 500-hash window) at 1w and
  8w under `perf stat` (7 events). Compared instr/hash to the GCC-16 baseline (Phase 0).

## Results (instr/hash, ÷500)

| metric | GCC-16 (Phase 0) | **Clang 22** | Δ |
|---|---:|---:|---|
| 1w instr/hash | 113.8 M | **113.2 M** | −0.5% |
| 8w instr/hash | 113.8 M | **113.2 M** | −0.5% |
| 1w H/s | 5.11 | 5.11 | 0% |
| 8w H/s (per-worker-equiv) | 2.56 | 2.56 | 0% |
| IPC | 0.662 | ~0.664 | 0% |
| `ld_dep_stall` | 17.1M | 16.9M | ~0% |

## Verdict: NULL
Clang produces **bit-identical throughput AND essentially identical instruction count** (113.2M
vs 113.8M, within noise). A different compiler backend does NOT close the +15% gap vs XMRig.
The instruction-count excess is in armrx's **source-level emission logic** (which opcode
sequences it chooses), not the compiler's codegen. This confirms E3b is the real lever — we must
change *what* armrx emits per VM opcode (see `e3b-opcode-emission-diff.md`), not just swap the
compiler.

Clang build is kept as a working alternative toolchain (`cmake/toolchain-aarch64-musl-clang.cmake`)
for future A/B; it is not adopted as the shipping build (GCC-16 cross remains the shippable
artifact per AGENTS.md).
