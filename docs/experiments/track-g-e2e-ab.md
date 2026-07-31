# W1-5: Track G (NEON T-table AES) Full-Workload E2E A/B — CONFIRMED WIN (2026-08-01)

The first full-workload A/B of Track G. The microbenchmark projected ~3.6%
(12.3% AES cycle share × +28.8% primitive throughput, `docs/experiments/neon-ttable-aes.md`)
but the full-workload measurement was never done before the feature went default-ON
(commit 4888ba1). This A/B closes that gap: **ON is a real, measured win — +1.68% H/s,
−2.07% cycles, −4.93% instructions vs OFF** — though smaller than the projection.

## Method

- Device: MSM8929, 8× Cortex-A53 @ fixed 765 MHz, `isolcpus=1-7`, light mode.
- Binaries: cross-built `bench_armrx` — ON = Track G default ON (md5
  `257e6d14…`), OFF = `-DARMRX_ENABLE_NEON_TTABLE_AES=OFF` (md5 `5e76629c…`),
  same tree, separate build dir (`build-cross-off`).
- Protocol: ON-OFF-ON-OFF, `taskset -c 3`, `--full-hash-only --perf-ready`
  (T1-2 steady-state hook; 500 hashes ≈ 105 s per run), `perf stat` attached
  after PERF_READY, per-run md5 of the executed binary logged (T2-2 v3
  discipline). Device idle of other load (bit-identical instruction counts
  across same-variant runs prove no contention).
- Full log: device `/tmp/w15-ab.log` (this doc quotes the summary table).

## Results

| run | variant | md5 | cycles | instructions | IPC | μs/hash | H/s |
|---|---|---|---|---|---|---|---|
| ON1 | ON | `257e6d14` | 80,406,212,488 | 59,478,963,979 | 0.740 | 206,465 | 4.84 |
| OFF1 | OFF | `5e76629c` | 82,053,623,565 | 62,411,546,302 | 0.761 | 210,209 | 4.76 |
| ON2 | ON | `257e6d14` | 80,612,182,554 | 59,478,956,954 | 0.738 | 206,429 | 4.84 |
| OFF2 | OFF | `5e76629c` | 82,291,561,515 | 62,411,562,755 | 0.759 | 210,243 | 4.76 |

Mean ON: 80.509B cycles / 59.479B instr / 206,447 μs/hash (4.84 H/s)
Mean OFF: 82.173B cycles / 62.412B instr / 210,226 μs/hash (4.76 H/s)

**Deltas (ON vs OFF):**

| metric | delta |
|---|---|
| cycles | **−2.07%** |
| instructions | **−4.93%** |
| wall μs/hash | −1.83% |
| hashrate | **+1.68%** |
| IPC | −2.7% (0.740 vs 0.760 — OFF's higher IPC is the scalar path doing more, simpler work) |

Run-to-run: instructions bit-identical within variant (0.00002%), cycles ±0.25%.

## Analysis

1. **Track G is confirmed as a real E2E win** (+1.68% H/s, −2.07% cycles) — the
   default-ON decision (T0-1) was correct. Not reverted.

2. **Measured < projected (~3.6%).** The projection used a 12.3% AES cycle share
   measured before D2 pipelining (and before Track G itself shrank AES cost).
   Back-solving: 2.07% ÷ 28.8% ≈ **7.2% current AES cycle share** — the D2
   interleave and other post-measurement changes roughly halved AES's share.
   Consistent with W1-5's caution in `performance-audit-work-ideas-20260801.md`.

3. **Secondary finding — instruction census input:** ON executes **118.96M
   instr/hash** vs OFF **124.8M** (−4.93%): Track G's vectorized AddRoundKey
   (vld1q/veorq/vst1q) is denser than the scalar byte-loop. This directly
   validates the Cursor audit's §1 hypothesis that Track G contributes to the
   132.93M → 119.0M instruction-total drop (measured contribution here:
   −5.8M instr/hash). The W1-1 census (in flight) should attribute this.

## Cross-references

- `docs/experiments/neon-ttable-aes.md` — microbenchmark (+28.8% primitive) and the
  ~3.6% projection this A/B supersedes with a measured E2E number
- `docs/audits/performance-audit-work-ideas-20260801.md` W1-5 (this item) / §1
- `docs/experiments/t12-perf-ready-first-run.md` — the 119.0M / 0.740 baseline
  this A/B re-confirms for the ON variant (118.96M, 0.740)
- `docs/experiments/t11-d2-microbenchmark.md` — D2 interleave (the ~halving of
  AES's cycle share)
- `docs/experiments/t22-dual-issue-alignment.md` — the md5-per-run A/B discipline
  reused here
