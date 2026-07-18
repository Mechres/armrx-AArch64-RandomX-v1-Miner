# armrx — Master Plan

> **Single source of truth for what we're building, why, and in what order.**
> Supersedes `docs/next_phase.md` (v1, archived). Detailed reference documents
> for each area are linked — read those for implementation specs.
>
> Current status: **Post-parity on 8× Cortex-A53 (~28.9 H/s Monero mainnet).**
> Focus shifts from JIT correctness to hardware ceiling mitigation and
> operator-facing polish.

---

## Where we are

- **Single-thread baseline:** 5.16–5.18 H/s (light mode JIT, `bench_armrx`)
- **8-thread pool performance:** 28.92 H/s (3.61 H/s/thread — 30% per-thread drop due to memory bandwidth contention)
- **CPI:** 1.22 (armrx) vs 1.56 (XMRig) — armrx emits simpler instructions that individually CPI better, but +33% more of them
- **All KATs pass in JIT + interpreted mode**

### What's been delivered

| Area | Items | Status |
|------|-------|--------|
| **Security** | TLS hostname verification, JSON injection protection, read_buf cap, W^X compliance, CLI validation, SIGTERM | ✅ |
| **Concurrency** | Stratum mutex, atomic session members, TSAN CMake option, per-instance rounding mode | ✅ |
| **Memory** | MAP_HUGETLB for dataset/cache/scratchpad, MADV_POPULATE_WRITE warmup | ✅ |
| **JIT tooling** | `--jit-dump`, bench_opcodes frequency analyzer, determinism/encoding tests, per-opcode audit | ✅ |
| **Structured logger** | `include/armrx/log.hpp`, all cross-thread log sites migrated, `--log-level` flag | ✅ |
| **Hot-path** | Template copy moved to job-change path, superscalar heap churn eliminated | ✅ |
| **Parser** | `get_array_first` dead-code fix, `find_key` scope fix, `json::escape` all control chars | ✅ |
| **Pool** | CryptoNote + Stratum V1 dual protocol, auto-reconnect with backoff, multi-pool failover | ✅ |

See [`ROADMAP.md`](ROADMAP.md) for the detailed completed/remaining checklist.

---

## Active priorities (ranked by impact)

| # | Priority | Est. gain | Phase | Detail doc |
|---|----------|-----------|-------|------------|
| **1** | **Newton-Raphson FDIV/FSQRT postmortem** — debug x29 crash on existing NR code, then simplify | **+5–8% (BLOCKED)** | Beyond-parity B | [`beyond-parity.md`](docs/beyond-parity.md#pillar-b-newton-raphson-fdivfsqrt-jit-unblocking-highest-single-jit-win) |
| **2** | **Worker phase staggering** — tested, no benefit on Cortex-A53. Hardware bandwidth ceiling. | **+0%** | Beyond-parity A | [`beyond-parity.md`](docs/beyond-parity.md#priority-re-ranking-from-post-parity-analysis) |
| **3** | **Peephole JIT coalescing** — disassembly comparison with XMRig on identical seed programs | **+5–10%** | JIT plan Phase 2 | [`peephole-jit-plan.md`](docs/peephole-jit-plan.md) |
| **4** | **TUI redesign** — TuiSnapshot, terminal-width, NO_COLOR, EMA bars, atexit cursor | ✅ Done | TUI U1 | [`tui_usability_plan.md`](docs/tui_usability_plan.md#phase-u1--tui-foundations) |
| **5** | **SuperscalarHash JIT output scheduling** — AArch64-level hazard analysis on emitted JIT buffer | **+3–5%** | Beyond-parity C | [`beyond-parity.md`](docs/beyond-parity.md#pillar-c-superscalarhash-jit-scheduling) |
| **6** | **CLI/config consolidation** — `--version`, dead code deleted | ✅ Done | TUI U3 | [`tui_usability_plan.md`](docs/tui_usability_plan.md#phase-u3--telemetry-and-cli) |
| **7** | **Prometheus metrics endpoint** — HTTP `/metrics` on localhost | ✅ Done | Beyond-parity D | [`beyond-parity.md`](docs/beyond-parity.md#pillar-d-testing--observability-expansion) |
| **8** | **PGO unblock** — try `-fprofile-use -fno-lto` path, static libgcov link, resolve GCC 15 + musl `__gcov_*` crash | **+5–10%** (blocked) | next_phase_v2 §2.6 | [`OPTIMIZATION_REFERENCE.md`](OPTIMIZATION_REFERENCE.md#L47) |
| **9** | **Stratum handshake / TLS tests** — integration tests for pool protocol under TSAN | Robustness | next_phase_v2 §4.4 | [`next_phase_v2.md`](docs/next_phase_v2.md#44-testing-strategy-for-next-phase-features) |
| **10** | **DVFS / thermal pinning check** — measure throttle under sustained load | **+0–5%** | Beyond-parity | [`beyond-parity.md`](docs/beyond-parity.md#priority-re-ranking-from-post-parity-analysis) |

---

## Phase roadmap

```
Now ─────────────────────────────────────────────────────────────────►

NR postmortem ──► Simplify NR ──► Measure 1-8 thr scaling ──► Stagger
                    │                    │
                    └── KAT veto ────────┘
                                               └──► Peephole JIT diff vs XMRig
                                               └──► TUI redesign
                                               └──► CLI/config polish
                                               └──► Prometheus endpoint
                                               └──► Superscalar scheduling
                                               └──► PGO unblock
                                               └──► Handshake/TLS tests
```

1. **Debug the x29 crash** on the existing NR code (`ARMRX_ENABLE_JIT_FAST_DIV_SQRT`). This is the highest single JIT win and unblocks all other NR work. If the root cause is found, the simplified NR (1 iteration + lightweight Markstein) is 30 minutes of code.
2. **Re-measure scaling** with MAP_HUGETLB (1–8 threads). The 30% scaling drop may have changed. This tells us whether to chase JIT work or bandwidth work.
3. **Implement intra-loop staggering** if scaling is still 30% down.
4. **TUI redesign** (snapshot struct, width awareness, NO_COLOR, share tracking) — this is the largest operator-facing gap.
5. **Peephole JIT disassembly comparison** with XMRig — the only remaining path to close the instruction-count gap.
6. **CLI polish, Prometheus, Superscalar scheduling, PGO** in any order — these are independent.

---

## Reference documents

| Doc | Scope | Status |
|-----|-------|--------|
| [`ROADMAP.md`](ROADMAP.md) | Completed/remaining checklist | Active |
| [`docs/next_phase_v2.md`](docs/next_phase_v2.md) | Comprehensive Phase 1/2/3 plan (post-review) | Active — detailed reference |
| [`docs/next_phase.md`](docs/archived/next_phase.md) | v1 of above | **Archived** — superseded by v2 |
| [`docs/peephole-jit-plan.md`](docs/peephole-jit-plan.md) | JIT instruction-count gap closure | Active reference |
| [`docs/beyond-parity.md`](docs/beyond-parity.md) | Post-parity scaling optimization | Active reference (v2) |
| [`docs/tui_usability_plan.md`](docs/tui_usability_plan.md) | TUI redesign, CLI ergonomics, Prometheus surface | Active reference |
| [`docs/branchless-cbranch.md`](docs/branchless-cbranch.md) | CBRANCH misprediction postmortem | Reference |
| [`OPTIMIZATION_REFERENCE.md`](OPTIMIZATION_REFERENCE.md) | Historical log of every optimization tried | Reference |

---

## How to use this plan

1. **Starting work?** Read the active priority list above. Pick the highest-ranked item that isn't blocked.
2. **Need details?** Follow the "Detail doc" link to the specialized plan.
3. **Updating status?** Edit `ROADMAP.md` (completed/remaining checklist) and the active priority table in this file. Keep specialized docs consistent.
4. **Archiving a doc?** Move it to `docs/archived/` and update the reference table above.
