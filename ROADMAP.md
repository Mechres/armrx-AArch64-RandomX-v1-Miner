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

**Release readiness: all zero/low-risk audit tiers complete.** T0-1 (Track G default ON), T0-2 (hardware_concurrency fix), T1-1 (D2 benchmark), T1-2 (perf stat harness), T2-3 (isolcpus-aware worker pinning) all ✅ done and device-verified. Remaining: T2-1 (PRFM), T2-2 (dual-issue alignment), T3 items (gated).

### In Progress
(none — all implementable audit items shipped; next items need the JIT-compiler files, which are gated to comments-only until lifted)

### Completed (post-alpha)
- **T0-1 — Track G (NEON T-table AES) default ON** (2026-08-01) — commit 4888ba1
- **T0-2 — hardware_concurrency() footgun** (2026-07-31) — online_cpu_count() reads `/sys/devices/system/cpu/online`
- **T2-3 — isolcpus-aware worker pinning** (2026-08-01) — workers pin to isolated cores on all `detect_core_order()` paths; default worker count capped to isolated count; on-device verified (workers 1-7, main 0)
- **T1-2 — clean perf stat path** (2026-08-01) — `--perf-ready` hook + wrapper, device-verified: first clean steady-state run (IPC 0.740, 119M instr/hash @ fixed 765 MHz) — `docs/experiments/t12-perf-ready-first-run.md`
- **T1-1 — D2 on-device benchmark** (2026-08-01) — `--bench-d2-pipeline` harness; device A/B: interleaved −2.77% (IPC 1.735→1.787), ~0.34% E2E — `docs/experiments/t11-d2-microbenchmark.md`

### Next Up
1. **T2-1** — PRFM hints (needs JIT-file gate lifted)
2. **T2-2** — dual-issue alignment (needs JIT-file gate lifted)
3. T3 items — design-first, gated (Track C retry blocked on diagnostic)
