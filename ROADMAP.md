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

**Release readiness: all audit tiers complete except T1/T2 performance work.** T0-1 (Track G default ON), T0-2 (hardware_concurrency fix), T2-3 (isolcpus-aware worker pinning) all ✅ done and on-device verified. Remaining: T1-1 (D2 benchmark), T1-2 (perf stat harness), T2-1 (PRFM), T2-2 (dual-issue alignment), T3 items.

### In Progress
- **T1-1 — Benchmark D2 pipelined hash+fill on-device** (needs light-mode harness; estimated 0.5–1% gain, unmeasured)

### Completed (post-alpha)
- **T0-1 — Track G (NEON T-table AES) default ON** (2026-08-01) — commit 4888ba1
- **T0-2 — hardware_concurrency() footgun** (2026-07-31) — online_cpu_count() reads `/sys/devices/system/cpu/online`
- **T2-3 — isolcpus-aware worker pinning** (2026-08-01) — workers pin to isolated cores on all `detect_core_order()` paths; default worker count capped to isolated count; on-device verified (workers 1-7, main 0)

### Next Up
1. **T1-2** — clean perf stat path (prerequisite for all measurement)
2. **T1-1** — D2 on-device benchmark (needs T1-2)
3. **T2-1** — PRFM hints
4. **T2-2** — dual-issue alignment
5. T3 items — design-first, gated (Track C retry blocked on diagnostic)
