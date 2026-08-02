# Baseline: armrx post-W4 (a1ea83c) — gated perf, Cortex-A53 @ 765 MHz

**Date:** 2026-08-03
**Commit:** a1ea83c (W4 phase-2 literal-pool committed)
**Purpose:** Item 0 of the residual-gap roadmap — clean gated re-baseline replacing the
corrupted "132.7M vs 104.8M" ungated comparison. Establishing the reference point for the
hardware-AES Item 1 A/B.

## Protocol (W11 / T1-2 gated)
- Device: Cortex-A53 @ 765 MHz fixed (no cpufreq OPP table), postmarketOS.
- `perf_event_paranoid=2` → per-process `perf stat -e cycles,instructions` works; `perf stat -a` blocked.
- **NOT isolated** — kernel cmdline had NO `isolcpus` (confirmed empty). Label: *non-isolated*.
- `taskset -c 3` single-thread. Binary md5 pinned + echoed per run.
- Gated window: `bench_armrx --full-hash-only --perf-ready`, 500 hashes, start captured via
  a SIGUSR1/FIFO "PERF_READY" handshake (only steady-state hashing counted — excludes the
  ~13.7M-instr/hash Argon2 fill + JIT-compile init contamination that corrupted earlier numbers).
- B-M-B-M repeated; run 1 used as baseline below.

## Result (run 1, gated)
| Metric | Value |
|---|---:|
| Instructions / hash | **107,360,000** (53,679,096,566 / 500) |
| Cycles / hash | **171,190,000** (85,594,335,154 / 500) |
| IPC | **0.627** |
| Median hash time | 220.59 ms |
| Window duration | ~111.1 s |
| Binary md5 | `1a5bb464ed7f263904fe927e98670ee3` |

## Comparison / interpretation
- **vs W11 census (pre-W4, gated, ISOLATED):** 118.96M instr → 107.36M instr = **−9.75%**
  (W4 literal-pool banked ~11.7M instr/hash; Kimi's dump-derived estimate of 107.3M confirmed
  to three digits).
- **Cycles +6.7% and IPC −15.5% vs W11** are **CONFOUNDED** — W11 was measured isolated, this
  run is non-isolated. The +6.9% median-hash increase is in the range isolation alone explains.
  The true W4 cycle cost is NOT separable without a same-state A/B (pre-W4 vs post-W4 both
  non-isolated). Deferred — it does not gate Item 1.
- **Kimi audit prediction for post-AES:** ~97-98M instr/hash, ~163M cycles/hash, ~210 ms — to
  be tested by the Item 1 A/B against THIS baseline (171.19M / 107.36M / 0.627).

## Caveats
- Non-isolated: absolute cycle/median numbers carry background-OS noise on cores 4-7; the
  instruction count is deterministic (same workload) and the AES delta should be read on
  instructions + relative cycles, not raw H/s.
- The W4 cycle/IPC regression cannot be attributed to W4 vs isolation from this single run.
