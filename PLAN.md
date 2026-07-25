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

*Speculative future performance ideas beyond this section's items — not scheduled/gated work —
are tracked separately in
[`docs/plans/future-performance-ideas-20260725.md`](docs/plans/future-performance-ideas-20260725.md).*

1. **`--rt-priority` + `isolcpus=`/`nohz_full=`** — **blocked on manual device access.**
   `--rt-priority` currently falls back silently to the default scheduler (needs `setcap`,
   which isn't installed and there's no passwordless `sudo`); `isolcpus=`/`nohz_full=` need a
   kernel-cmdline edit + reboot, which needs explicit user sign-off regardless of privilege
   availability. Per Phase 6 item 3's interconnect-arbitration finding, tempered expectations
   either way: this is a hardware/interconnect effect, not scheduler-visible, so CPU isolation
   was never going to touch the 6→8-worker degradation directly — it might help the
   front-loaded memory-contention component marginally at best. Deferred until the user grants
   device access directly.

2. ~~**Peephole JIT coalescing**~~ ([`docs/plans/peephole-jit-plan.md`](docs/plans/peephole-jit-plan.md)) —
   **closed 2026-07-25, deprioritized on evidence, not just deferred.** The ~22% of cycles left
   unreconciled by item 14's opcode-level correlation ("inside a worker buffer but outside any
   superscalar entry") was narrowed by extending `tools/jit_correlate.py` to use the `code_size`
   boundary it already parsed but never applied — splitting that bucket cleanly by whether an
   address falls before or after the superscalar region starts. Two live `perf record` captures
   on the same 8-worker mining workload (one `-e cycles`, one `-e instructions`, same
   methodology as item 14) show the split precisely:

   | Region | % of instructions | % of cycles | relative IPC |
   |---|---|---|---|
   | Main per-hash VM program (offset < CodeSize) | 9.23% | 20.04% | **0.461×** avg |
   | Superscalar, opcode-attributed | 72.71% | 63.52% | 1.145× avg |
   | Superscalar, unattributed (fixed wrapper chunks) | 2.49% | 2.38% | 1.046× avg |
   | Outside any JIT buffer (named C++) | 15.56% | 14.05% | 1.107× avg |

   **The main VM program region carries ~9% of dynamic instructions but ~20% of cycles — a
   ~2.2× IPC penalty relative to the rest of the pipeline.** This is a stall signature, not an
   instruction-count signature: this region is where the memory-operand opcodes (`*_M`,
   `ISTORE` — ~48% of this region's code bytes per the original static breakdown) live, doing
   genuinely random 64-byte reads/writes into the 2 MiB scratchpad. Branch misprediction is
   already ruled out separately (2.4% hot-path miss rate, ~0.1-0.16% of cycles). The superscalar
   unattributed slice, once isolated, turned out proportionate (~1.0× IPC) — not a real lead at
   all, just measurement noise from the old lumped-together bucket.

   **Conclusion: this is evidence against peephole JIT coalescing, not just an unmet gate.**
   Peephole's entire premise is code-density/instruction-count reduction; the one remaining
   unexplained slice of cycles is disproportionately expensive *because of memory-latency
   stalls*, which code-density reduction cannot fix. This is also the same conclusion every
   single instruction-count-reduction attempt this project has tried has independently reached
   (CSEL, Newton-Raphson, NEON-AES ×3, superscalar literal-pool relayout, `IMUL_RCP`
   literal-load elimination — all implemented, measured, and reverted for exactly this reason).
   Not starting the 3-6 week clean-room rewrite against evidence that specifically points away
   from it. `tools/jit_correlate.py`'s region-split extension is kept as reusable diagnostic
   infrastructure regardless of this outcome.

5. ~~**Extend the emitter lookahead scheduler to hide memory-op latency in the main VM
   program**~~ — **tried 2026-07-25, caused a real JIT/interpreter divergence, reverted.** The
   natural follow-on to item 2's finding above: the same *mechanism* that already produced a
   real, measured win for the superscalar region's `IMUL_R`/`IMUL_RCP` stalls (reordering to
   hide long-latency-op stalls, not reducing instruction count) was a plausible, specific,
   falsifiable hypothesis for the main VM program's 2.2× IPC penalty too. Implemented: flagged
   the memory-load opcodes (`*_M`) as `is_long_latency` in `computeFootprint()`, making them
   eligible as swap triggers (`P`) — no new hazard-model change was believed necessary, since
   the existing memory-memory-always-hazard rule already prevents a `*_M` op from ever swapping
   past another `*_M` op.

   `test_jit_equivalence` failed immediately on its first (and most basic) seed/input pair —
   the first time this test has failed in the project's history. Reverting the change alone
   (keeping everything else) made it pass again, confirming the memory-op extension itself is
   the cause. Investigated the mechanism at length: checked whether `emitMemLoad`'s own
   `src==dst` shared-scratch-register special case (structurally similar to the *original*
   `src==dst` hazard from item 12) was responsible — but that pattern was already reviewed by
   the three independent code reviews and confirmed self-contained regardless of position, so
   it doesn't explain this. **The exact mechanism was not conclusively identified.** Given the
   failure mode is silent wrong hashes and this was an explicitly speculative, "may be a null
   result" experiment from the outset, the responsible choice was to fully revert rather than
   ship a targeted exclusion without being able to verify it — matching this project's own
   standing rule to trust empirical results over an unconfirmed theory, but here without a
   working fix to trust, just a clean revert. `tools/jit_correlate.py`'s region-split extension
   (item 2) is unaffected and kept.

   **Side fix, found and corrected while investigating**: the CBRANCH defensive `ARMRX_ASSERT`
   added in Phase 7 item 4 fired repeatedly during this investigation on a completely normal
   `test_jit_equivalence` run — its premise (that a CBRANCH targeting a never-written register
   is "theoretical only") was factually wrong. Traced why the existing behavior is actually
   correct and intentional (register_usage_[creg]==-1 wraps `pc` to 0, i.e. "restart from VM
   instruction 0," matching the JIT's own `reg_changed_offset[]` reset to `PrologueSize` before
   every compile). Removed the incorrect assert. Committed `4da77f0`.

   **Performance work is now closed out for this session** — both the direct lead (peephole
   JIT, item 2) and its evidence-backed follow-on (this item) have been tried or ruled out. The
   two genuinely open items are `--rt-priority` (item 1, blocked on device access) and nothing
   else on the performance side without a new measured hypothesis.

3. ~~**Add `-frounding-math` to `armrx_core`'s compile options**~~ — **done, 2026-07-25.**
   Flagged by the Deepseek audit (Phase 6 item 19), confirmed missing via direct grep, added to
   `CMakeLists.txt`. Verified: local x86 full suite 7/7, on-device targeted suite 5/5, no new
   warning categories. Committed `059b6fe`.

4. ~~**Defensive `ARMRX_ASSERT` for CBRANCH-with-unwritten-target-register**~~ — **done,
   2026-07-25.** Also from the Deepseek audit (Phase 6 item 19), confirmed accurate by tracing
   the code (`register_usage_[creg] == -1` wraps `pc` to `0` via `int16_t` truncation + `++pc`).
   Added to `h_CBRANCH` in `src/vm.cpp`. Theoretical-only trigger, zero cost in release builds.
   Verified 7/7 local, 5/5 on-device. Committed `059b6fe`.
