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
asymmetric multi-cluster ARM hardware.
