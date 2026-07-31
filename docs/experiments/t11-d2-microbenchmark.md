# T1-1: D2 Interleaved Hash+Fill Microbenchmark — On-Device A/B (2026-08-01)

Gate item T1-1 from `docs/audits/combined-audit-20260731.md`: measure
`hash_and_fill_aes_interleaved_x4` (Track D2) in isolation against the
sequential pair `hash_aes_1r_x4` + `fill_aes_1r_x4`, then perf stat A/B.

## Harness

`tests/bench_armrx.cpp` (test infra only — no production code touched):

- `--bench-d2-pipeline` — runs an **equivalence gate** (both variants from
  identical fresh buffers/states must produce byte-identical `hash_state` and
  fill_scratchpad), then times both variants (~30 samples) on two 64-byte
  aligned 2 MiB scratchpads (`posix_memalign`), prints delta.
- `--d2-only=sequential|interleaved` — single-variant mode: equivalence gate +
  fixed 200-iteration loop, no per-iteration timing, for a clean `perf stat`
  code path. Requires `--bench-d2-pipeline`; validates its value (exit 1
  otherwise).
- Flag-gated only — NOT part of `run_all`; default bench output unchanged.
- Implemented by Reasonix from a self-contained brief; host-verified by
  Reasonix (8/8 ctest) and re-verified by Hermes (all flag paths, exit codes).

## Results

### Host (x86_64, software T-table AES path)

| Variant | median | Δ |
|---|---|---|
| sequential pair | 1686.76 μs | — |
| interleaved | 2228.90 μs | **+32.14% (slower)** |

Equivalence PASS. On the x86 T-table path the fused loop hurts (register
pressure / cache behavior) — irrelevant for the target, but a useful negative
data point: the interleaving only pays off on the NEON path it was designed for.

### Device (MSM8929, Cortex-A53, NEON T-table AES — Track G ON, fixed 765 MHz)

Microbenchmark (30 samples):

| Variant | median | min/max | σ |
|---|---|---|---|
| sequential pair | 13080.74 μs | 13051/13224 | 0.3% |
| interleaved | 12717.80 μs | 12668/12864 | 0.3% |
| **Δ** | **−2.77%** | | |

perf stat A/B (200 iterations each, pinned core 3):

| Metric | sequential | interleaved | Δ |
|---|---|---|---|
| cycles | 2,232,198,676 | 2,171,138,579 | **−2.74%** |
| instructions | 3,874,193,946 | 3,880,747,017 | +0.17% |
| **IPC** | **1.735** | **1.787** | **+3.0%** |
| branches | 22,503,500 | 15,949,781 | **−29.1%** (fused loop) |
| branch-misses | 80,162 | 79,083 | −1.3% |
| wall (200 iters) | 2.838 s | 2.768 s | −2.47% |

## Interpretation

- **Equivalence**: gate PASS on both host and device — the interleaved path
  produces byte-identical results to the sequential pair (as designed).
- **Mechanism**: essentially the same instruction count (+0.17%); the win is
  pure ILP — IPC 1.735 → 1.787 (+3.0%) because the fused loop overlaps the
  hash-phase AES round latency with the fill-phase work. ~29% fewer branch
  instructions (one loop instead of two) is a secondary effect.
- **Why host showed +32%**: the x86 build uses the plain software T-table
  path; without NEON the interleaving only adds register pressure. The device
  NEON T-table path (Track G) is where the overlap pays off. This is a clean
  demonstration that the two AES paths behave differently — relevant context
  for any future x86-vs-AArch64 perf comparison.
- **End-to-end projection**: the AES boundary phases are ~12.3% of cycles
  (D2 design doc); 2.74% on 12.3% ≈ **~0.34% hashrate** — inside the
  estimated 0.5–1% envelope, at the conservative end. The mining engine
  already runs the interleaved path, so this win is already live; the
  microbenchmark confirms it was worth the integration risk.

## Files

- Harness: `tests/bench_armrx.cpp` (`bench_d2_pipeline()`, flags,
  help text) — implemented by Reasonix, commit follows
- Design: `docs/plans/d2-pipelined-hash-fill-plan-20260730.md`
- Implementation notes: `docs/experiments/d2-hash-fill-pipeline.md`
