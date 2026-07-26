# armrx — Master Update and Improvement Plan

This document is the live master plan for the `armrx` RandomX AArch64 miner — the *open* work
only. It used to also carry the full narrative for every completed phase; that grew to 400+
lines of 100%-done history sitting in front of the actually-open work, so it was split out
2026-07-24 into **[`docs/archived/plan_completed_phases_1-5.md`](docs/archived/plan_completed_phases_1-5.md)**,
Phase 6 was split out the same way 2026-07-25 into
**[`docs/archived/plan_phase6_completed.md`](docs/archived/plan_phase6_completed.md)**, and
Phase 7's closed items followed the same pattern the same day into
**[`docs/archived/plan_phase7_completed.md`](docs/archived/plan_phase7_completed.md)** once only
one open item remained. Read this file for what's still open; read the archives for the full
why-and-how behind everything already shipped. The chronological, dated record of the same
history lives in `changelogs.md`; the short-list actionable view at any point in time lives in
`NEXT_STEPS.md`; completed/remaining status tables live in `ROADMAP.md`.

---

## Completed — Phases 1–5 (2026-07-21 through 2026-07-23)

All correctness fixes, structural refactors, test-coverage gaps, and performance
investigations identified through Phase 5 are done. Summary (full detail in the archive):

- **Phase 1–2** (2026-07-21/22): `MetricsExporter` thread-join fix, Stratum nonce
  abstraction, `main.cpp` → `CommandLineParser`/`MinerApp` split, worker-thread reuse for
  dataset init (surfaced and fixed a critical fast-mode dataset-corruption bug), mock Stratum
  protocol tests (surfaced and fixed a `PoolManager` self-deadlock), JSON fuzzing (2.5M+
  executions, zero findings), Argon2 NEON `permute_block` enabled (~16% faster).
- **Phase 3** (2026-07-22/23): on-device LTO/`fortify-headers` build regression root-caused
  and fixed, both pool-failover gaps fixed, 3 constant-dedup refactors, CBRANCH/CSEL
  investigated and reverted (net regression) — **and its own investigation root-caused the
  historical 31.08% branch-miss figure to a non-representative benchmark section**; the real
  hot-path rate is 2.4%, costing ~0.1–0.16% of cycles. Argon2 diagonal-step NEON
  vectorization landed (26.8% fewer instructions / 19.0% fewer cycles for cache init);
  `memcpy` copy-elimination tried and reverted (no net win).
- **Phase 4** (2026-07-23): fresh codebase inspection found and fixed 3 more bugs — a worker
  thread permanently killed by a malformed nonce job, unguarded config-file numeric parsing,
  and a `MetricsExporter::server_fd_` data race — plus new test coverage for `cli_parser.cpp`
  (found a real broken-`--config=` bug) and `aes_hash.cpp`. JIT buffer RWX default kept,
  now disclosed at startup.
- **Phase 5** (2026-07-23): external audit's leads fact-checked and adopted — PGO devbox
  wiring shipped (tool works, but the claimed +19.3% payoff did **not** reproduce on current
  code: measured identical 4.27 H/s PGO vs. non-PGO); NEON vector-permute AES derived from
  scratch and exhaustively verified correct, measured as a real ~19.4% regression (kept,
  flag-gated OFF); `--stagger-ms` confirmed already tested and ineffective in an earlier
  session, not re-run.

**Still open from this era, low priority, tracked in backlog (not started):** QEMU AArch64
GitHub Actions CI, Stratum V2 protocol support, `ARMRX_JIT_FAST_DIV_SQRT` CMake flag
centralization, `tls_client.cpp`/`tui.cpp` test coverage (need a mock TLS server / terminal-
capture harness respectively — bigger lift than the `cli_parser.cpp`/`aes_hash.cpp` work
already done). None of these block or relate to Phase 7 below.

---

## Completed — Phase 6 (2026-07-24 through 2026-07-25)

Two independent performance master plans (this assistant's and a "Hermes" agent's) were
synthesized into one adopted, phased plan rather than run in parallel. Full detail, including
every measurement table and the two-cluster interconnect discovery's complete evidence chain,
is in **[`docs/archived/plan_phase6_completed.md`](docs/archived/plan_phase6_completed.md)**.
Summary:

- **Verification-first, both foundational leads closed as no-ops**: huge-page residency
  (already ~97.6-100% THP-coalesced, no real hugetlbfs pool needed) and a worker-count sweep
  (4→8 workers: smooth monotonic decline, no plateau, 8 remains the right default).
- **Major, unplanned finding**: this device (real SoC: MSM8929/Snapdragon 415, not the
  previously-assumed MSM8916) has two separate 4-core L2 cache clusters with a *dynamic
  interconnect-arbitration* asymmetry under contention (not a static clock/thermal effect) —
  found via a real head-to-head XMRig comparison, fully explains the worker-sweep efficiency
  curve, and gives a real cluster-normalized gap to XMRig of ~10-12% (not the "far behind"
  impression raw aggregates gave).
- **Several medium-term JIT leads investigated; two were stale claims already implemented,
  one (superscalar literal-pool relayout) was implemented/measured/reverted as a small real
  regression, one (prefetch removal) was adopted as a real small win after three rounds of
  increasingly sensitive measurement, one (fused hash-and-fill) failed its own benchmark
  gate before any integration risk was taken.**
- **Instrumentation built for the previously-unmeasured superscalar/dataset-derivation
  region** (item 13/14): static counting, live `perf record` self-profiling, and a real
  opcode-level correlation script (`tools/jit_correlate.py`) together explain ~63% of all
  mining cycles — `IMUL_R`/`IMUL_RCP` alone account for over 35%. A follow-on literal-load
  elimination attempt targeting `IMUL_RCP` was implemented, measured, and reverted as a small
  net regression (stall savings cancelled by extra instructions).
- **Emitter lookahead scheduler (item 12) — implemented, extended, measured, adopted.** First
  version (main VM program only) measured as a clean null because it targeted the wrong JIT
  region; extended to the actual hot superscalar path after finding and fixing a second
  hazard class (`IMUL_RCP` literal-pool ordering). Two dedicated large-N differential stress
  tests plus the full existing suite all green. Final `taskset`-pinned `perf stat` comparison:
  **+0.233% IPC, −0.036% cycles** vs. baseline — small but real and reproducible.
  **Three independent code reviews** (Deepseek, Gemini, Hermes) of the scheduler followed,
  converging on "no constructible failure scenario" but catching one real doc-comment error
  (two of three independently caught it; Gemini missed it, restating the falsified claim).
- **PGO re-checked after the scheduler landed — still a confirmed null**, this time after
  catching (mid-session) a misleading unpinned ~2x gap that was purely a core-cluster
  scheduling artifact, not a real PGO effect — corrected by `taskset`-pinning both binaries to
  the same core.
- **Dual external audit (Gemini, Hermes) and a third full-codebase audit (Deepseek)** — every
  concrete claim independently verified against actual source/live device state before acting;
  4 real issues fixed (OOB dataset-read guard, `BigOnly` affinity hardcoding, `worker_hashes_`
  false sharing, a stale comment), several claims refuted with reasoning, one previously-open
  question (scratchpad huge-page residency) closed as a non-issue.
- **Two real MCP-server tooling bugs found and fixed** (`tools/devbox/devbox_mcp.py`): a wrong
  timeout bucket causing near-guaranteed `devbox_test` timeouts, and a single-threaded
  synchronous stdin loop that couldn't answer liveness pings during long calls, causing
  spurious forced reconnects. **One related limitation remains, not fixed, not a supported
  usage pattern**: this MCP host's client only tolerates one in-flight request per connection —
  firing a second tool call while an earlier long-running one is still active reliably kills
  the connection. Wait for a background task's completion notification rather than polling with
  a fresh call while it's still running.

---

## Completed — Phase 7 (2026-07-25)

Full detail in **[`docs/archived/plan_phase7_completed.md`](docs/archived/plan_phase7_completed.md)**.
Summary:

- **Peephole JIT coalescing — closed on evidence, not deferred.** Extended
  `tools/jit_correlate.py` to split the region-split's "unattributed" bucket by the `code_size`
  boundary, isolating a real, precise finding: the main per-hash VM program region carries ~9%
  of dynamic instructions but ~20% of cycles — a **~2.2× IPC penalty**, a memory-op (scratchpad)
  stall signature, not an instruction-count one. This is evidence *against* peephole's entire
  premise (code-density reduction), matching every prior instruction-count-reduction attempt's
  outcome (CSEL, Newton-Raphson, NEON AES ×3, superscalar literal-pool relayout, `IMUL_RCP`
  literal-load elimination — all reverted for the same reason).
- **`-frounding-math` added** to `armrx_core`'s compile options (Deepseek audit finding,
  confirmed missing, fixed). Committed `059b6fe`.
- **CBRANCH-unwritten-target-register assert — added, then removed.** A Deepseek audit claim
  that this case was "theoretical only" was disproved empirically (fired repeatedly on normal
  test runs); traced to confirm the existing behavior is correct and intentional, assert
  removed. Committed `4da77f0`.
- **Emitter scheduler extended to memory-load (`*_M`) opcodes — tried, caused a real
  JIT/interpreter divergence, reverted.** Root cause not conclusively identified; full
  writeup at `docs/experiments/memory-op-scheduler-attempt.md`. Performance work reached a
  natural stopping point this phase.
- **Same-day doc consolidation**: three overlapping future-performance planning docs were
  reconciled into two — see the pointer below.

## Completed — Phase 8 (2026-07-25): `isolcpus`/`rcu_nocbs` — a real ~14% win

Full detail in **[`docs/experiments/isolcpus-rt-priority-win.md`](docs/experiments/isolcpus-rt-priority-win.md)**.
The user installed `setcap` and granted device access (cmdline edit + reboot). Result: **the
biggest measured win in this project's history** — every prior adopted change has been sub-1%.

- **`isolcpus=1-7 rcu_nocbs=1-7`** (core 0 deliberately left for kernel housekeeping) gives a
  reproducible **~28.4 H/s aggregate 8-worker steady-state hashrate vs. ~24.9 H/s without it**
  (matches `README.md`'s independently-documented 24.95 H/s baseline closely) — **+14%**, measured
  across 2 no-isolation rounds and 4 isolated rounds (3 of 4 tightly reproducible, 1 anomaly).
- **Mechanism, not just a number**: the fast cluster (cores 0-3) is identical either way (4.26
  H/s/worker). The entire effect is on the slow cluster (cores 4-7) — without isolation, 2-3 of
  those 4 workers randomly get knocked to half rate (1.42 vs 2.84 H/s) each run, a different
  subset each time — the signature of background OS work/interrupts stealing cycles from pinned
  workers. With isolation, all four hit the full rate with zero variability. This is a
  *different*, software-fixable mechanism than the hardware interconnect-arbitration effect
  documented in Phase 6 (which still exists and still caps cluster 1's ceiling under contention).
- **`nohz_full=1-7` silently no-ops** on this kernel — confirmed `CONFIG_NO_HZ_FULL` is not set.
  Real, permanent finding for anyone deploying on this device/kernel combination.
- **`--rt-priority`'s independent contribution is unconfirmed** — functionally engaged (`setcap
  cap_sys_nice+ep`, no fallback warning), but every isolated round landed in the same ~28.4 H/s
  range regardless of the flag. A `perf stat` attempt to get a low-noise reading hit a real,
  unresolved tooling gotcha (near-zero cycle counts attaching to the full multi-threaded `armrx`
  binary — correctly measured a sanity-check busy loop, so the issue is specific to this binary's
  worker-thread attribution).
- **Three false starts caught and corrected during this investigation** (see the experiment doc's
  "false starts" section) — two stale historical baseline figures initially mis-compared against,
  and an overconfident thermal-throttling attribution walked back after the user pushed back with
  direct hardware knowledge. All three corrected before writing this up.
- **Not yet explored, not blocking**: IRQ affinity tuning (`/proc/irq/*/smp_affinity`) as a
  possible explanation for the one anomalous round; fixing the `perf stat` thread-attribution
  gotcha for a real low-noise `--rt-priority` measurement.

This is an **operational/deployment recommendation** (kernel boot cmdline), not a code change —
it can't be shipped in `armrx` itself, but should be recommended to anyone deploying on similar
asymmetric multi-cluster ARM hardware. **Scope caveat**: this is a general OS-scheduling fix, not
an armrx-specific one — it would very likely help XMRig by a similar margin too (not measured).
It does not close the ~10-12% cluster-normalized code-level gap to XMRig (Phase 6 item 9), which
remains open.

**Real bug found the same night, not yet fixed**: with `isolcpus` set, running `armrx` without
an explicit `--workers=N` silently picks **1 worker instead of 8** — confirmed live during a real
overnight pool-mining run. `src/cli_parser.cpp:35`'s default (`std::thread::hardware_concurrency()`)
is affinity-based on this device's musl toolchain (via `sched_getaffinity()`), and `isolcpus`
restricts new processes' default affinity to core 0 only. This makes the `isolcpus` win actively
dangerous without a companion warning or fix — naive deployment loses far more (7/8 of
throughput) than the 14% gained. **Not yet fixed**; always pass `--workers=<N>` explicitly on any
`isolcpus`-configured host in the meantime. See `docs/experiments/isolcpus-rt-priority-win.md`.

**Second, deeper bug found the next night, not yet fixed**: even with `--workers=8` passed
correctly, a full overnight real-pool run sustained only ~24.76 H/s — the *pre-isolcpus* baseline,
not the benchmarked 28.4 H/s win. Root cause: this device has no `cpufreq` sysfs at all, so
`detect_core_order()` (`src/mining_engine.cpp:82-109`) falls back to sequential core order
`[0..7]`, and `AffinityMode::All`'s `i % core_order_.size()` mapping puts worker 0 on core 0 — the
one core `isolcpus=1-7` leaves unisolated for the OS/main thread. In real pool mining (unlike the
local `--seconds=N` benchmark used to measure the 28.4 H/s figure), core 0 also hosts the stratum
reader thread, JSON/job handling, and the per-second console print, so worker 0 now eats the same
"pinned worker loses cycles to unrelated OS work" penalty `isolcpus` was adopted to fix on cores
4-7 — just relocated to core 0, invisible to the no-network benchmark. No existing flag works
around it (`--workers=7` still maps worker 0 to core 0 under the same modulo scheme).
**Correction (2026-07-26): the "real fix" originally proposed here (exclude non-isolated cores
from the worker pool, e.g. 7 workers instead of 8) was wrong and would make it worse, not
better** — core 0 is physically one of the fast cluster's cores (4.26 H/s isolated); excluding it
gives 3 fast + 4 slow = 24.14 H/s, *below* the measured real 24.76 H/s, meaning the contended
worker 0 is still contributing ~0.62 H/s that dropping it would lose for nothing. Caught by the
user before this was implemented. No corrected fix has been identified yet — see
`docs/experiments/isolcpus-rt-priority-win.md`'s "Second footgun" section for the full analysis.

## Completed — Phase 9 (2026-07-26): performance plan Step 1 run — gate closed, Steps 2-3 not needed

Full detail in **[`docs/plans/performance-plan-20260725.md`](docs/plans/performance-plan-20260725.md)**
(Step 1's "Result" callout) and **[`docs/experiments/scratchpad-locality-bound-20260726.md`](docs/experiments/scratchpad-locality-bound-20260726.md)**.

Phase 7's remaining lead — the main VM program region's ~2.2× IPC penalty
(`docs/archived/plan_phase7_completed.md`) — had one gated, evidence-first step defined but not
yet run: bound how much of that penalty is recoverable memory-latency stall (fixable) versus
architectural floor (not fixable), before spending any more implementation effort chasing it.

**Ran it.** New `bench_armrx --scratchpad-real`/`--scratchpad-l1` flags (`tests/bench_armrx.cpp`)
and `VirtualMachine::run_execute_only()` (`src/vm.cpp`) isolate the JIT-compiled main-VM-program
execute step from compile overhead, then re-run it against either the real 2 MiB scratchpad or a
16 KiB `memfd` tiled 128× across the same 2 MiB virtual range (so every address the JIT computes
lands on the same L1-sized physical backing, with zero change to the JIT's own address-masking
logic). `perf stat -e cycles,instructions`, 2000 iterations each, on-device:

| Condition | Cycles | Instructions | IPC |
|---|---|---|---|
| Real 2 MiB scratchpad | 44,694,130,884 | 29,373,692,608 | 0.6572 |
| L1-aliased (16 KiB) | 42,135,298,579 | 29,373,699,356 | 0.6971 |

Instruction counts match to 5 decimal places — a clean comparison. Forcing the scratchpad to be
effectively latency-free bought only **+6.07% IPC**. Per the plan's own gate, that's small
against the region's ~2.2× overall penalty: **the stall is mostly not a memory-latency problem**.
This closes Step 2 (`PRFM` prefetch) and Step 3 (bisecting the reverted memory-op scheduler)
without attempting either — both target latency, and there isn't enough latency-bound stall left
to justify either's cost (Step 3 specifically carries real correctness risk, silent wrong
hashes). The residual penalty reads as architectural (in-order pipeline / dependency-chain-bound
on this Cortex-A53), not something a further code change can chase. **No genuinely open
performance lead remains project-wide as of this writing.**

## Completed — Phase 10 (2026-07-26): experimental performance backlog worked to full closure

Full detail in **[`docs/plans/experimental-performance-ideas-20260725.md`](docs/plans/experimental-performance-ideas-20260725.md)**.
Per explicit user direction, stopped gating low-risk ideas on a diagnostic before implementing
and worked through every remaining item in this speculative backlog directly:

- **Adopted (4)**: `-fvisibility=hidden`/`-fno-semantic-interposition` + `-fomit-frame-pointer`
  (ideas #6+#12, `CMakeLists.txt`) — average +0.298% IPC across two on-device `perf stat` samples.
  Argon2 cache `MADV_POPULATE_WRITE` (idea #10, `src/argon2.cpp`) — mirrors the scratchpad's
  existing prefault pattern, confirmed to apply on every seed rotation (a fresh `Argon2dCache` is
  constructed each time), latency-only so not independently quantified. `.p2align 6` for the main
  loop's I-cache alignment (idea #11, `src/jit_compiler_a64_static.S`) — measured noise-level as
  predicted, kept for zero cost/risk.
- **Closed on evidence (5)**: scratchpad alignment (#9, already 2 MiB-aligned by construction),
  I-cache pressure (#8, 0.788% miss rate), BLAKE2b NEON (#5, doesn't register in the profile),
  superscalar `IXOR_C*` immediate materialization (#2 — built and validated a standalone AArch64
  logical-immediate encoder *before* touching any JIT code; 0 of 20,000 real, uniformly-random
  superscalar immediates turned out encodable, so the fast path would essentially never trigger),
  double-buffered JIT compile/execute overlap (#7 — read `randomx_calculate_hash()` first; its
  premise doesn't hold, run N+1's entropy genuinely depends on run N's post-execution output, no
  overlap window exists).
- **Done (1)**: profiled the C++ overhead slice (#4) — `hash_aes_1r_x4`+`fill_aes_1r_x4` is the
  single biggest named-C++ cost at ~12.3% of all cycles, bigger than Argon2 or NEON permute
  combined; both known optimization avenues for it already tried and failed, so not newly
  actionable but now precisely quantified.
- **Tried, reverted (1)**: superscalar `IMUL_RCP` register pre-assignment (idea #1) — implemented
  after correcting the backlog doc's wrong register-availability claim (x9 holds a live pointer,
  not free), but caused a real `test_jit_equivalence` failure on its first test case. Mechanism not
  identified; fully reverted rather than ship an undemonstrated fix. See
  `docs/experiments/superscalar-imul-rcp-preassignment-attempt.md`.

**Nothing remains unaddressed in either performance backlog document.** The two genuinely open
items project-wide are both the isolcpus deployment bugs documented in Phase 8 (worker-count
default, worker-to-core placement) — not performance-tuning work.

## Completed — Phase 11 (2026-07-26): mid/high-risk performance work — one adopted, one root-caused and closed for now

After Phase 10's low-risk backlog reached full closure, opened
**[`docs/plans/mid-high-risk-performance-ideas-20260726.md`](docs/plans/mid-high-risk-performance-ideas-20260726.md)**
to track genuinely correctness-risky performance work — extending or bisecting the JIT scheduler
itself. Two items worked:

- **Adopted: widened the main VM program scheduler's swap window**
  (`docs/experiments/main-scheduler-window-widening-20260726.md`). Added a 4-instruction fallback
  candidate to `scheduleProgram()`, tried only when the existing 3-window swap doesn't qualify —
  stays entirely within the scheduler's existing register-hazard model, no new hazard *class*.
  Passed the full 450-pair `test_jit_scheduler_stress` (the differential test built specifically
  for this scheduler), plus equivalence/determinism/encodings/KATs/mining-engine tests. Measured
  **+0.156% IPC average** across two on-device `perf stat` samples (cycles −0.148%, both
  consistent in direction), similar magnitude to the original scheduler's own adopted win.
- **Root-caused and closed for now, not proven impossible: superscalar `IMUL_RCP` register
  pre-assignment** (revisiting the idea #1 attempt reverted in Phase 10;
  `docs/experiments/superscalar-imul-rcp-preassignment-attempt.md`). Bisected with a temporary
  env-var cap and found the real mechanism: `randomx_calc_dataset_item_aarch64` is called via `bl`
  from inside the main program's own light-mode JIT body, and that call site doesn't protect
  x14/x15 (the main program's own live r6/r7) or x21-x28 (the main program's own pre-loaded
  `IMUL_RCP` literals) — the original code was only ever safe because it never wrote to those
  registers. Empirically, only x19 works as a sole preassigned register; the realistic safe budget
  (at most 1, not the 12 originally planned) makes the win too small to justify right now. Left
  open with a concrete revisit path (root-cause why x20 also fails; consider explicit save/restore
  at the call site) rather than closed permanently.

Two further Tier 2/3 ideas remain in the tracking doc (BOLT; a full dependency-graph list
scheduler) — not started, lower priority given the evidence gathered so far.
