# Re-baseline: armrx HEAD (`ed466a2`) — clean gated 1w, non-isolated (2026-08-06)

**Date:** 2026-08-06
**Commit:** `ed466a2` (Era II reframe; contains shipped E24 C* padding `acc7735`, E25 `a2685ad`,
hardware-AES funnel `2687a2e`).
**Purpose:** Item 0 of the residual-gap roadmap — re-lock authoritative numbers on the shipped tree.
**Harness:** `bench_armrx --full-hash-only --perf-ready` (T1-2 gated 500-hash steady-state window,
attach `perf stat -p` at `PERF_READY`, release via FIFO newline). Cross build: GCC 16.1.0 musl,
`-march=armv8-a+crypto`, LTO off. Binary md5 `b52d024289237159418a6be316d4f71c`.

## CRITICAL CAVEAT — non-isolated
The device kernel cmdline has **NO `isolcpus`** (`/proc/cmdline` = plain pmos, no isolcpus/rcu_nocbs).
So this is a **non-isolated** run — background OS work on cores 4-7 can perturb the window. The
**instruction count is deterministic** (same workload) and the two runs agree to 0.00006%, so the
instr/hash figure is reliable; absolute cycle/H/s carry the usual non-isolation noise. A *clean
isolated* 8w run would require a device reboot with `isolcpus=1-7 rcu_nocbs=1-7` (operational
change — NOT done unilaterally).

## Result — 1w, core 3, non-isolated, two repeats
| Metric | Run 1 | Run 2 | notes |
|---|---:|---:|---|
| Instructions | 50,550,148,705 | 50,550,179,491 | **identical to 6 digits** |
| Cycles | 75,609,340,426 | 75,837,109,896 | |
| Instructions / hash | **101.10 M** | **101.10 M** | 500-hash divisor |
| Cycles / hash | 151.22 M | 151.67 M | |
| **IPC** | **0.668** | **0.666** | |
| Median hash | 195.48 ms | 196.05 ms | |
| Window duration | 99.11 s | 99.11 s | |
| Clock check (cycles/elapsed) | 763 MHz ✓ | 766 MHz ✓ | valid (≈765 MHz) |
| cache-misses | 326.3 M | 324.1 M | |
| l2d_cache_refill | 139.2 M | 139.3 M | |
| bus_access | 570.6 M | 571.3 M | |
| branch-misses | 12.56 M | 12.58 M | |

(`bus_cycles` = `cycles` on this PMU — redundant, ignore.)

## Reconciliation vs prior numbers — the changelog "−16.7%" is a measurement error
| source | instr/hash | status |
|---|---:|---|
| W4 baseline (pre-AES, non-isolated, `2026-08-03-post-w4-baseline.md`) | 107.36 M | valid (gated) |
| **HEAD this re-baseline (non-isolated, 2× repeat)** | **101.10 M** | **valid, reproducible** |
| changelog Item 1 claim (post-AES) | 89.47 M (−16.7%) | **NOT REPRODUCED — outlier** |

The binary **does** contain the hardware-AES funnel (234 `aese`/`aesmc`/`aesd`/`aesimc` instructions;
`__ARM_FEATURE_AES` defined under `-march=armv8-a+crypto`), so AES *is* active. But the apples-to-apples
AES delta on this device is **107.36M → 101.10M = −5.8%**, **not** the claimed −16.7% (89.47M). The
89.47M figure is a contaminated-divisor artifact (the exact class of error the project's own
discipline flags: dividing perf instruction counts inside an ungated/non-500 window). The brief
(`2026-08-03-hardware-aes-item1.md`) itself predicted ~97-98M post-AES — even *that* is below the
measured 101.10M, so the AES instruction saving is smaller than predicted.

## Consequence for the "gap closed" narrative
- **Instruction count:** armrx 101.10M vs XMRig 94.5M = **armrx is ~7% HEAVIER at 1w** (non-isolated).
  The changelog's "armrx now sits BELOW XMRig (94.5M)" and "largest code-level win in project history"
  claims are **wrong** — AES is a real but modest (~6%) density win, not a 16.7% one.
- **H/s (still valid):** the 1w H/s parity/win (armrx 5.11 vs XMRig 5.04, E24 real-pool long-run) is
  unaffected — armrx wins H/s *despite* being ~7% heavier on instructions because its IPC is better
  (0.667 vs XMRig ~0.654 in the census). The user's actual goal (parity) holds at 1w; the *mechanism*
  described in the changelog (instruction-count leadership) does not.
- **8w:** NOT measured here — `bench_armrx --full-hash-only` is single-threaded (ignores `--workers`;
  confirmed in `bench_armrx.cpp:872-927`). The only authoritative 8w figure remains the real-pool
  long-run (armrx 26.65 vs XMRig 28 = 95.2%, `perf-tracking.md` §0). A clean isolated 8w instr/cycle
  re-baseline is **BLOCKED on `isolcpus`** (device reboot).

## Stray `reb_8w` artifact — NOT an 8w measurement
A `--workers=8` invocation of `bench_armrx --full-hash-only` was attempted, but the code path
(`bench_armrx.cpp:872-927`) is **single-threaded** and ignores `--workers`. The captured run took
~197s wall (2× the 1w ~99s) with median hash 391.6 ms (exactly 2× 195.5 ms) and the **same 50.55B
instructions** as the 1w runs — i.e. it is a 1w-equivalent capture under heavier background load,
mislabeled "8w". **No 8w instruction/cycle data exists from this tool.** A real 8w number requires
the mining engine (`--mine --workers=8`) under `isolcpus` (see Action item 3).
1. **Correct `changelogs.md` Item 1 entry** (2026-08-03): the −16.7% / 89.47M instr figure is an
   artifact; replace with the reproducible −5.8% (107.36M → 101.10M) and strike "armrx now below
   XMRig on instruction count." Keep the correctness-KAT evidence (hw==T-table, golden pins) — that
   part is solid; only the perf attribution is wrong.
2. **Reconcile `ROADMAP.md` / `docs/archived/strategy.md`** "Item 1 closed the residual gap / below XMRig" wording
   with the corrected ~7%-heavier-1w reality (H/s parity still holds).
3. **8w number:** `bench_armrx --full-hash-only` is single-threaded (ignores `--workers`), so a
   clean 8w instr/cycle re-baseline from this tool is **not possible**. The authoritative 8w figure
   is the real-pool long-run (armrx 26.65 vs XMRig 28 = 95.2%, `perf-tracking.md` §0). `isolcpus` is
   an operational deployment knob, NOT an armrx feature, and is **not** part of the baseline
   methodology (real devices won't have it enabled). No device reboot is warranted for measurement.
