# Brainstorm: next levers after the SIGINT/metrics fixes + DAG attempt

**Date:** 2026-08-06 · **Context:** SIGINT + MetricsExporter fixed & committed (`1e5fc52`); DAG list-scheduler
attempt handed to external planner "luna" (brief `docs/briefs/2026-08-06-dag-scheduler-attempt.md`).
Hermes is skeptical gate. This file is brainstorming for the NEXT lever if DAG is null or once it lands.
**Not committed yet** — review before adding to changelog/ROADMAP.

---

## Status of the field (grounded, from this session's reads)
- **At H/s parity**: 1w 5.11 vs 5.04 (win); 8w 26.65 vs 28 = 95.2% (real-pool long-run, authoritative).
- **Residual gap is instruction-mix only** (101.10M vs 94.5M @1w). All stall/padding/peephole levers closed/reverted.
- **Closed (do NOT re-propose):** PRFM hints (T2-1, triple-closed: +0.26–0.50% cycles regression + 2026-07-24
  fill-loop removal + audit "do not re-add in any form" — `static.S:405-407`); hugepages (E9 +0.8%, E15 +1.5%,
  already XMRig's exact method); memory-op scheduler extension (W3-2 divergence); E26 load-hoist (segfault);
  PGO (null); LTO (musl crash); FDIV/FSQRT fast path (−1.1%); Track C (≤0.5%); CBRANCH CSEL; dual-issue padding (T2-2).
- **The W3-2 trap rule:** never make a `*_M` opcode the anchor/reorder-trigger of a JIT scheduler; never reorder
  `*_M` across another `*_M`. Any idea that touches `*_M` emission order is suspect — avoid.

---

## Idea 1 — `-mtune=cortex-a53` as the default device build flag  ⭐ strongest
- **What:** `CMakeLists.txt:67` sets `-march=armv8-a+crypto` only. The cross-build has **no `-mtune`** — GCC emits
  generic armv8-a scheduling, not Cortex-A53-tuned. Add `-mtune=cortex-a53` (or `-mcpu=cortex-a53+crypto`, which
  implies both arch + tune) to `armrx_core` for the aarch64/crypto target. `ARMRX_ENABLE_NATIVE` (`-mcpu=native`) is
  wrong for cross-to-device (native = build host, not A53) and is off in the device toolchain.
- **Why it's grounded/open:** `docs/archived/plan_phase6_completed.md:696` explicitly lists "`-mtune=cortex-a53`
  default tuning" as **"Not yet acted on, needs measurement not blind adoption"** (raised by Hermes 2.1 / Gemini).
  `docs/STRATEGY.md:23` and `docs/plans/era2-plan.md:153` also list `-mtune=` as open.
- **Risk:** LOW. Compiler flag, zero correctness surface (no JIT/scheduler touch). Worst case: no measurable win.
- **ROI:** Small but real (instruction selection/scheduling tuned for the exact in-order A53). Likely sub-1% to low
  single-digit % cycles. Different shape from W3-2 (it's a build flag, not an emission reorder).
- **Verify:** cross-build; B-M-B-M `perf stat -e cycles,instructions` on `bench_armrx --full-hash-only`; adopt if
  cycles improve ≥0.3% in both B runs. Keep `-march=armv8-a+crypto` semantics intact (x86_64 host build unaffected).
- **Caveat:** `cortex-a53` is the *only* core on this device, so `-mtune=cortex-a53` is exactly right (unlike native).

## Idea 2 — Fast-cluster-aware default worker placement (attacks the 8w gap head-on)  ⭐ high-value
- **What:** `mining_engine.cpp` splits cores by `cpuinfo_max_freq` (`detect_core_order` L49/106, `count_top_frequency_cores`).
  On MSM8929 **both clusters are A53 at one firmware freq (765 MHz)**, so `big_core_count_` = 8 → `AffinityMode::BigOnly`
  collapses to `All` → pins workers to **all 8, including the slow cluster (4-7)**. Default affinity (`cli_parser.cpp`)
  may be `Unpinned`, letting the OS scatter workers onto the slow cluster under contention.
- **Why it matters (the actual 8w parity deficit):** E15 = armrx non-isolated **21-23 H/s** vs XMRig **27.77**; armrx
  *isolcpus* = 28.4 = XMRig. Mechanism = slow cluster (4-7) loses ~50% under contention. XMRig tolerates the topology
  WITHOUT isolcpus — it binds workers to the fast cluster. armrx doesn't, by default.
- **Lever:** Make the default placement fast-cluster-aware: on topologies where the two clusters differ by L2/clock
  domain (not freq — detect via `cpuinfo` L2 or a latency probe, since MSM8929 reports one freq), pin mining workers
  to cores 0-3 and leave 4-7 idle. Or: detect the slow cluster via a short cross-core latency probe at startup and
  exclude it. This recovers the isolcpus parity **without requiring the user to reboot with isolcpus**.
- **Risk:** MEDIUM-LOW. Pure thread placement; no JIT/correctness surface. Must not break the `Unpinned` option or
  single-worker runs. Must handle the no-isolation and the "both clusters identical freq" cases gracefully.
- **ROI:** Potentially the **largest remaining lever** — directly targets the 8w gap (the only place armrx is <100%).
  Could close the 95.2% → ~100% on 8w without any user/kernel change. Different shape from W3-2 (placement, not reorder).
- **Verify:** real-pool 8w long-run (non-isolated) before/after; target ≥27 H/s to match XMRig. Also check 1w unchanged
  (pinning 1 worker to core 0-3 should match current).

## Idea 3 — Make `bench_armrx` multi-worker so the "8w instruction-mix diff" is measurable  ⭐ methodology
- **What:** `bench_armrx --full-hash-only` is **single-threaded** (`tests/bench_armrx.cpp:798,901` — ignores `--workers`;
  `--perf-ready` requires `--full-hash-only`). The roadmap's stated next lever ("armrx vs XMRig instruction-mix diff @8w")
  is therefore **currently unmeasurable** — we can only get 8w numbers from the real pool long-run.
- **Why:** To actually chase the 8w gap with `perf stat`, we need a gated multi-worker bench. Add a `--workers=N` path
  that hashes in parallel (reuse `MiningEngine`) and reports aggregate instr/cycle/H-s. This unblocks Idea 2's
  measurement and any future 8w analysis.
- **Risk:** LOW (test tooling only, no shipping path). Note: `bench_armrx` is a test target; the slow stress suites are
  separate.
- **ROI:** Enabling, not directly perf. Closes the measurement gap that's been blocking 8w analysis.

## Idea 4 (speculative, lower priority) — interpreted-path dataset prefetch
- `docs/archived/plan_phase6_completed.md:696` lists "interpreted-path dataset prefetch (Hermes 2.2)" as open. The JIT
  path already has `randomx_calc_dataset_item_aarch64_prefetch` (`static.S:1008`); the *interpreted* `vm.cpp` dataset
  item path may not. Low expected ROI (dataset access is already streaming/prefetchable by HW), but distinct from the
  closed JIT PRFM work. **Park** unless Ideas 1-3 exhausted.

---

## Ranking for action (if DAG is null or after it lands)
1. **Idea 2** (fast-cluster placement) — largest potential win, directly closes the only <100% gap, different shape.
2. **Idea 1** (`-mtune=cortex-a53`) — cheap, safe, open, likely small but real; do first as a quick win.
3. **Idea 3** (multi-worker bench) — do alongside Idea 2 to measure it properly.
4. **Idea 4** — speculative, park.

## What is explicitly NOT worth proposing
- PRFM (any form) — triple-closed, pure overhead on in-order A53.
- Hugepages / PGO / LTO / FDIV-FSQRT-fast / Track C / CBRANCH-CSEL / dual-issue padding — all closed on evidence.
- Any `*_M`-reordering scheduler extension — the W3-2 trap; the DAG attempt (luna) is the only sanctioned reorder shape.

## Verification discipline (MANDATORY for any agent running these)
- NEVER run the full on-device test suite in one shot. The JIT stress suites (test_jit_scheduler_stress
  450+200 pairs ~20-35 min each; test_mining KAT ~7 min; plus --perf-ready 500-hash windows) peg the
  weak Cortex-A53 and made the device unresponsive (hard reboot required, 2026-08-06).
- qemu-aarch64 is FORBIDDEN for verification — it doesn't model the A53 in-order/mem-latency wall, so
  perf numbers are meaningless and slow tests just cook the host.
- Run ONE test per session, fastest-first (test_jit_equivalence 16/16 ALONE first), cooldown between,
  stress suite last and alone. See the DAG brief §8 for the full staged gate.
