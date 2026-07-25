# armrx — Master Update and Improvement Plan

This document is the live master plan for the `armrx` RandomX AArch64 miner — the *open* work
only. It used to also carry the full narrative for every completed phase; that grew to 400+
lines of 100%-done history sitting in front of the actually-open work, so it was split out
2026-07-24 into **[`docs/archived/plan_completed_phases_1-5.md`](docs/archived/plan_completed_phases_1-5.md)**,
and Phase 6 was split out the same way 2026-07-25 into
**[`docs/archived/plan_phase6_completed.md`](docs/archived/plan_phase6_completed.md)** once it hit
the same size. Read this file for what's still open; read the archives for the full why-and-how
behind everything already shipped. The chronological, dated record of the same history lives in
`changelogs.md`; the short-list actionable view at any point in time lives in `NEXT_STEPS.md`;
completed/remaining status tables live in `ROADMAP.md`.

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

## Phase 7 — current (2026-07-25): open items

1. **`--rt-priority` + `isolcpus=`/`nohz_full=`** — **blocked on manual device access.**
   `--rt-priority` currently falls back silently to the default scheduler (needs `setcap`,
   which isn't installed and there's no passwordless `sudo`); `isolcpus=`/`nohz_full=` need a
   kernel-cmdline edit + reboot, which needs explicit user sign-off regardless of privilege
   availability. Per Phase 6 item 3's interconnect-arbitration finding, tempered expectations
   either way: this is a hardware/interconnect effect, not scheduler-visible, so CPU isolation
   was never going to touch the 6→8-worker degradation directly — it might help the
   front-loaded memory-contention component marginally at best. Deferred until the user grants
   device access directly.

2. **Peephole JIT coalescing** ([`docs/plans/peephole-jit-plan.md`](docs/plans/peephole-jit-plan.md),
   est. +5-10%, medium risk) — **still gated.** Phase 6 item 14's region-scoped instruction-count
   reconciliation explained ~63% of mining cycles (opcode-level, via `tools/jit_correlate.py`)
   but explicitly left ~22% of cycles ("inside a worker buffer but outside any superscalar
   entry") and the whole-process ~33.5% instruction-count gap to XMRig not fully reconciled at
   the region level. Do not start this without closing that gap further — per this project's
   own repeated lesson this phase (a correctly-implemented scheduler measured as a null purely
   because it targeted the wrong JIT region), guessing at a target without narrowing it first
   has a poor hit rate here.

3. **Add `-frounding-math` to `armrx_core`'s compile options** — small, cheap, defensive.
   Flagged by the Deepseek audit (Phase 6 item 19) and confirmed missing via direct grep; the
   RandomX VM depends on runtime `fesetround()` changes, and the JIT/interpreter's actual
   float value paths don't route through compiler-foldable C++ expressions today, so this is
   low-risk correctness-by-construction rather than a fix for an observed bug. Not yet applied.

4. **Consider a defensive `ARMRX_ASSERT` for CBRANCH-with-unwritten-target-register** — also
   from the Deepseek audit (Phase 6 item 19), confirmed accurate by tracing the code
   (`register_usage_[creg] == -1` wraps `pc` to `0` via `int16_t` truncation + `++pc`).
   Theoretical only — RandomX's program generator spec-guarantees registers are written before
   being branched on, no known real-world trigger. Zero-cost in release builds. Low priority,
   not yet applied.
