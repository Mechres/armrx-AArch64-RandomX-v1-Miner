# Track D1 gate tool: bench_dataset_2way 1-way mode hang — CLOSED

**Status: closed (2026-07-29).** Root cause found and fixed. The 1-way
benchmark now works at -O3. The D1 gate measurement (L1I refill rate
comparison) is unblocked.

## Root cause

`JitCompilerA64::emitAddImmediate()` hardcoded x20 as its scratch register
for the large-immediate (> ~16M) fallback path (line 1282 in the original).
This is correct for the *main VM program*, whose JIT prologue/epilogue
save/restore callee-saved registers x19-x28. However, the superscalar
dataset-derivation path (`generateSuperscalarHash()`) emits code that runs
inside `rx_calc_dataset_item`, a leaf function whose prologue only saves
caller-saved registers (x0-x13). x20 is used there as scratch during
the `IADD_C7`/`IADD_C8`/`IADD_C9` cases, silently corrupting the caller's
x20.

At -O2, the compiler either didn't keep anything important in x20 across
the call, or the larger -O2 code layout meant the IADD large-immediate path
was never taken. At -O3, x20 was used as the loop counter, causing the
benchmark's main loop to spin for 2^64 iterations whenever a superscalar
program happened to contain an `IADD_C7-9` with a large immediate.

The 2-way path (`JitDataset2Way`) was unaffected: it has its own
`emitAddImmediate2Way()` that parameterizes the scratch register per-stream
(x12/x16), both caller-saved.

## Fix

EmitAddImmediate now has a 6-arg overload that accepts an explicit scratch
register. The original 4-arg signature delegates to the 6-arg with
tmp_reg=20 (preserving existing behavior for the main VM program). The
superscalar path's IADD_C7..9 case passes x13 (caller-saved) instead.

Files changed:
- `include/armrx/jit_compiler_a64.hpp` — declare 6-arg emitAddImmediate
- `src/jit_compiler_a64.cpp` — add 4-arg wrapper, 6-arg implementation,
  change superscalar IADD_C7-9 to use x13

## Verification

- `bench_dataset_2way 1way 10`: completes immediately at -O3, checksum=17499
- `bench_dataset_2way 2way 10`: same checksum, still works

## Next steps

Run the actual L1I decision gate:
```
perf stat -e l1i_cache_refill taskset -c 3 ./bench_dataset_2way 1way N
perf stat -e l1i_cache_refill taskset -c 3 ./bench_dataset_2way 2way N
```
with `isolcpus=1-7` active, reversed trial order, N large enough for
stable `perf stat` (100K-1M).

## Previous contents (for reference)

> **Status: open.** The D1 implementation is unaffected (ctest 16/16 green...
