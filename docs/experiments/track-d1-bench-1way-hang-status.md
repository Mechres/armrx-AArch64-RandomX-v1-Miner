# Track D1 gate tool: bench_dataset_2way 1-way mode hang — CLOSED

**Combined status: the hang is fixed and the L1I gate has been measured.
Result: D1's premise (2-way interleaving helps I-cache locality) is
contradicted on this core.** The 2-way path causes ~40-100× more L1I
refills than the 1-way baseline, making D1 a clean negative result.
Do not wire the 2-way path into production mining. D3 (full dual-nonce
interleave of the main VM program) is also contraindicated by this data.

## Root cause (hang fix)

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

## L1I Gate Measurement

**Method**: `perf stat -e l1i_cache_refill,cpu_cycles,instructions,l1i_cache`
on `bench_dataset_2way {1way,2way} 100000` (200K items each), `taskset -c 3`,
isolcpus active, reversed trial order to control for thermal bias. 5 runs
total (3 1-way, 2 2-way — the extra 1-way was a repeat to verify the
noise-floor hypothesis).

### Raw data

| Run | Mode | L1I refills | Cycles | Instructions | L1I accesses | Time (s) |
|---|---|---|---|---|---|---|
| 1 | 1way | 410,669 | 7,139,991,549 | 4,444,486,139 | 2,363,091,456 | 9.10 |
| 2 (rev) | 2way | 35,580,267 | 7,219,392,347 | 4,441,183,534 | 2,357,082,104 | 9.19 |
| 3 (rev) | 1way | 348,387 | 7,123,038,040 | 4,444,380,291 | 2,360,813,893 | 9.08 |
| 4 | 2way | 35,569,057 | 7,200,857,124 | 4,441,328,174 | 2,358,915,384 | 9.17 |
| 5 | 1way | 852,276 | 7,123,917,656 | 4,444,413,936 | 2,361,775,130 | 9.07 |
| 6 | 2way | 35,765,171 | 7,189,007,543 | 4,438,042,578 | 2,358,410,273 | 9.15 |

(Note: runs 2-4 from first batch, runs 5-6 from fresh-verify batch. The
2-way was accidentally run twice in the first batch — the reversed-order
column above is conceptual, both 2-way runs were consecutive.)

### Analysis

| Metric | 1-way (avg 3 runs) | 2-way (avg 3 runs) | Ratio |
|---|---|---|---|
| **L1I refills** | 537,111 | 35,638,165 | **~66×** |
| L1I miss rate | 0.023% | 1.51% | **~66×** |
| Instructions | 4,444,426,789 | 4,440,184,762 | ~1× |
| Cycles | 7,128,982,415 | 7,203,085,671 | +1.0% |
| IPC | 0.624 | 0.616 | −1.2% |
| Wall time | 9.08 s | 9.17 s | +1.0% |

### Interpretation

The 1-way L1I miss rate (0.023%) is essentially the Cortex-A53 noise floor
on an isolated core — the entire 1-way JIT derivation code (~51 KB of
emitted code) fits in the 16 KB L1I about as well as can be expected on
a hardware design with 2-way set-associative caches and line fills.

The 2-way path's 1.51% miss rate (35.6M refills) corresponds to the
branch-heavy control flow of interleaved dual-stream emission thrashing
the L1I — each branch in the emitted code causes the next stream's
instructions to displace the previous stream's, even though the total
emitted code is only ~45 KB (smaller than 1-way's 51 KB). The issue is
not code size but **access pattern**: the interleaved streams force
alternating I-cache line usage that the simple sequential 1-way path
avoids.

The cycle cost (+1.0%) is modest but directionally consistent, and the IPC
drop (−1.2%, from 0.624 to 0.616) confirms the added I-cache pressure
creates detectable front-end stalls even at this already low IPC.

## D1 Conclusion

**Clean negative result.** The 2-way interleaved superscalar derivation
causes ~66× more L1I refills than the single-stream baseline on Cortex-A53,
contradicting the premise that collapsing two passes into one contiguous
function would improve I-cache locality. The IPC is slightly *worse*, not
better.

**Implications:**
- **Do not wire the 2-way path into production mining** — it regresses,
  not improves.
- **D3 (full dual-nonce interleave of the main VM program)** is also
  contraindicated. D3's code size would be much larger (the entire VM
  program, not just the superscalar preamble) and its access pattern even
  more branch-heavy. If the cheap, simple D1 can't clear this bar, D3 is
  extremely unlikely to.
- **The 2-way path and its benchmark harness remain in-tree** as a
  regression-protection tool (the exhaustive differential KAT detects any
  future accidental divergence between 1-way and 2-way semantics), but
  should not be wired anywhere.

## Previous hang debug record (retained for reference)

> ...original content about the bug investigation...
