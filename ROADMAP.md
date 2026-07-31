# armrx — Status Tracker

> **Post-alpha archive:** For the full retrospective covering all 210 commits (2026-07-13 to 2026-07-30) and 10 performance tracks A–J, see [`RETROSPECTIVE.md`](RETROSPECTIVE.md).
>
> For the still-active strategic master plan, see
> [`docs/plans/20260727/master-plan-20260727.md`](docs/plans/20260727/master-plan-20260727.md).
>
> The complete alpha-phase status tracker is preserved at
> [`docs/archived/alpha-roadmap.md`](docs/archived/alpha-roadmap.md).
>
> New entries go below this line.

## Current Status (2026-08-01)

**Release readiness: all zero/low-risk audit tiers complete or closed on evidence.** T0-1 (Track G default ON), T0-2 (hardware_concurrency fix), T1-1 (D2 benchmark), T1-2 (perf stat harness), T2-3 (isolcpus-aware worker pinning) all ✅ done and device-verified; T2-1 (PRFM hints) ❌ and T2-2 (dual-issue alignment) ❌ closed as measured regressions. Remaining: T3 items (gated).

### In Progress
(none — all implementable audit items shipped or closed; next items need the JIT-compiler files, which are gated to comments-only until lifted)

### Completed (post-alpha)
- **W1-1 — region instruction census** (2026-08-01) — 118.96M attributed: superscalar 80.5% / main-VM 9.9% (IPC 0.405) / AES+glue 9.5%; static 58.4M superseded (live body 5,224 A64/call); sample-bias dead, under-count primary, 8% call-count residual — `docs/experiments/w11-instruction-census.md`
- **T0-1 — Track G (NEON T-table AES) default ON** (2026-08-01) — commit 4888ba1; E2E A/B same day confirms +1.68% H/s, −2.07% cycles, −4.93% instr — `docs/experiments/track-g-e2e-ab.md`
- **T0-2 — hardware_concurrency() footgun** (2026-07-31) — online_cpu_count() reads `/sys/devices/system/cpu/online`
- **T2-3 — isolcpus-aware worker pinning** (2026-08-01) — workers pin to isolated cores on all `detect_core_order()` paths; default worker count capped to isolated count; on-device verified (workers 1-7, main 0)
- **T1-2 — clean perf stat path** (2026-08-01) — `--perf-ready` hook + wrapper, device-verified: first clean steady-state run (IPC 0.740, 119M instr/hash @ fixed 765 MHz) — `docs/experiments/t12-perf-ready-first-run.md`
- **T1-1 — D2 on-device benchmark** (2026-08-01) — `--bench-d2-pipeline` harness; device A/B: interleaved −2.77% (IPC 1.735→1.787), ~0.34% E2E — `docs/experiments/t11-d2-microbenchmark.md`
- **T2-1 — PRFM hints (closed, regression)** (2026-08-01) — inline main-VM `PRFM PLDL1KEEP` per scratchpad LDR: +0.26–0.50% cycles, +0.47% instructions, l1d unchanged. In-order A53 issues the hint in the same window as the dependent LDR — no stall hidden, pure overhead. Corroborates the 2026-07-24 fill-loop removal. Fresh measurement mandated by `jit_compiler_a64_static.S:397` now in hand — `docs/experiments/t21-prfm-hints.md`
- **T2-2 — dual-issue alignment (closed, regression)** (2026-08-01) — NOP-pad long-latency ops (IMUL-family, scratchpad LDRs) to 8-byte group starts in the main-VM JIT: cycles +0.13/+0.20% (worse), instructions +0.47% (NOP overhead), but **IPC +0.3% — the alignment mechanism worked**; the NOP cost just exceeds the dual-issue recovery. Third independent confirmation that added instructions cost real cycles on in-order A53 (after T2-1 and the 2026-07-24 fill-loop hints). Reverted bit-clean; device restored. See `docs/experiments/t22-dual-issue-alignment.md`
- **T3-3 — NEON mul analysis (closed, gate FAIL)** (2026-08-01) — real IMUL_R operand-magnitude gate run via instrumented interpreter sampling (`bench_imul_magnitudes`): **0.003% of 74.1M genuine IMUL_R have one operand ≤ 2^32** (gate ≥30%). Lane-pack premise falsified; F3 follow-on closed on evidence. See `docs/experiments/t33-imul-magnitude-gate.md`

### Next Up
1. T3 items — design-first, gated (Track C retry blocked on diagnostic)
