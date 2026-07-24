# Performance Improvement Audit — armrx (AArch64 RandomX Miner)

**Date:** 2026-07-23 (audit) — updated 2026-07-23 (after both leads were
investigated on-device)
**Audited by:** Hermes Agent (code review pass)
**Scope:** Hot-path review for performance improvement opportunities.
**Constraint:** Original audit host was x86_64; the AArch64 JIT (98% of hash
time) cannot execute off-device. JIT-only claims must be measured on the dev
box (192.168.10.156). The software-AES path runs everywhere and was
benchmarked directly on the audit host.

---

## STATUS — both substantive leads were investigated on-device; both are now KNOWN NON-WINS against current code

This audit proposed two substantive performance leads. A follow-up agent
("Claude") adopted both into `PLAN.md` Phase 5 / `NEXT_STEPS.md` §5a and
investigated them honestly on real hardware. **Neither is a win on the
current codebase.** The audit's headline framing ("PGO = +19.3%, single
biggest lever") is now DISPROVEN. Record, so the next session does not repeat
either as a live opportunity:

1. **PGO (was "headline / single biggest lever, +19.3%")** — `devbox_pgo_build`
   was added and measured apples-to-apples: **4.27 H/s single-thread with PGO,
   4.27 H/s without** (two training durations, 15s and 90s, to rule out
   under-training). The +19.3% figure came from a 2026-07-21 measurement and is
   **stale**: substantial hot-path code changed since (Argon2 diagonal-step
   vectorization, the JIT startup log line, several correctness fixes), shifting
   the code shape PGO's compile-time decisions were tuned against. Do NOT repeat
   the "+19.3%" claim without re-measuring against current code — and current
   code shows no payoff.

2. **NEON `vtbl` software-AES (was "Tier-2, untried")** — Derived a full
   tower-field S-box from scratch, verified 256/256 SubBytes/InvSubBytes + 20,000
   random full-round parity trials against the scalar path (KATs green, 12/12
   on-device), flag-gated behind `ARMRX_ENABLE_NEON_AES` (default OFF). Measured:
   **~19.4% REGRESSION** on `fill_aes_1r_x4`/`hash_aes_1r_x4`. Same root cause as
   the earlier hardware-`AESE`/`AESD` rejection: per-block NEON load/store
   overhead cancels the lookup savings on Cortex-A53. This is the *same class of
   failure* as the 2026-07-20 hardware-AES attempt, confirming a durable
   hardware fact: **NEON does not help this workload's AES on this core.** Treat
   any future "vectorize the AES" suggestion as a likely regression until proven
   otherwise with a benchmark.

The genuinely productive output of the follow-up was fixing a real, pre-existing
bug in the devbox tooling itself (see "What was actually fixed" below), not a
performance gain.

---

## What was actually fixed (the one concrete win from this audit)

The follow-up found and fixed a real bug while validating `devbox_pgo_build`:
`_stash_and_run`, `tool_status`, and `tool_test` were `shlex.quote()`-ing paths
built from `cfg.remote_dir` (`"~/armrx"`), which single-quotes the string and
silently defeats shell tilde expansion. Every build/test/bench log was landing
in a disconnected literal `~` directory; `devbox_status`'s deployed-revision
check was permanently reading from that wrong location (always reporting no sync
had happened). Fixed by interpolating `remote_dir`-derived paths unquoted.
(`changelogs.md` 2026-07-23 "PGO Devbox Wiring" entry.) This is worth keeping
independent of the PGO payoff.

---

## Where the audit was right (still valid)

- The codebase is already heavily optimized. The JIT-execution path (98% of
  hash time) and the init/finalize software-AES cost are real and measured
  (benchmarked on x86_64: `fill_aes_1r_x4` 515 μs, `hash_aes_1r_x4` 1159 μs;
  region-attribution telemetry: ~12.35% init + ~9.30% finalize on A53).
- The "do not re-touch" list (LTO/IPO, Argon2 NEON, huge pages, template-copy
  elimination, worker-thread dataset reuse, big.LITTLE pinning, CBRANCH closed,
  hardware-AESE rejected) remains correct.

## Where the audit was WRONG (corrected above)

- PGO is NOT a +19.3% lever on current code.
- NEON software-AES is NOT an untried win — it is a confirmed ~19.4% regression.

---

## Updated recommendations (what is actually worth doing next)

Given PGO and NEON-AES are both exhausted, the only remaining code-level lever
the project itself has flagged is:

- **Peephole JIT coalescing** (`ROADMAP.md` "P3"; `docs/peephole-jit-plan.md`):
  documented −5–10%, but the plan itself says to re-evaluate because it was
  framed against the old 31% branch-miss baseline that does NOT represent the
  hot path (isolated hot path is 2.4%). High effort, uncertain payoff.
  Hashrate-vetoed on-device. **(2026-07-24: the plan's original XMRig-disassembly
  approach is permanently out of scope — clean-room boundary, `PLAN.md` Phase 6
  item 14; any revival must be self-directed against armrx's own code.)**

Lower-risk, non-JIT tuning knobs (measure on-device, not assumptions):
- `--stagger-ms` default experiment (memory-bus contention under 8-worker
  saturation; currently defaults to 0).
- Re-confirm the 33% instruction-count gap vs XMRig is still the real delta and
  re-derive which opcodes dominate it with current `bench_opcodes` data.

Do NOT enable: `ARMRX_FAST_MATH` (`-Ofast`, breaks FP KATs) or
`ARMRX_ENABLE_JIT_FAST_DIV_SQRT` (Newton-Raphson, −1.1% hashrate, frozen).

---

## Already done — do not re-touch

Closed-out optimizations confirmed in changelogs.md / ROADMAP.md:
- Argon2 NEON G-function + diagonal-step vectorization (26.8% fewer instructions,
  19.0% fewer cycles for cache init — seed-key-rotation latency, not steady-state
  hashrate).
- Huge-page tiers (dataset `MAP_HUGETLB`, cache, 2 MiB scratchpad +
  `MADV_POPULATE_WRITE`).
- Template-copy elimination from per-hash path; Superscalar heap-churn kill.
- Worker-thread dataset reuse for dataset init (barrier handshake).
- big.LITTLE-aware scheduling + hwloc CPU pinning.
- Prefetch hint tuning; interleaved JIT FP loads; register-offset FP loads.
- LTO/IPO; rounding-mode cache; alignas(16) RegisterFile.
- CBRANCH: **closed** — 2.4% hot-path branch-miss rate, not a real lever.
- NEON `AESE`/`AESD` hardware AES AND NEON `vtbl` software-AES: **both rejected**
  (wrong round order / per-block load-store overhead cancels win on Cortex-A53).
- PGO: plumbed + `devbox_pgo_build` available, but **measured 0% payoff on current
  code** — keep off by default.

---

## Verification evidence

Original audit host (x86_64):
- `cmake -S . -B build -DARMRX_BUILD_TESTS=ON` + `cmake --build build -j`: clean.
- `./build/bench_armrx --micro-only`: AES fill 515 μs, AES hash 1159 μs.
- `./build/armrx_tests`: KAT suite green.
- PGO GENERATE → train → USE: links cleanly, `.gcda` emitted (mechanism works;
  the 0% payoff is a runtime/code-shape fact, not a build failure).

On-device (Cortex-A53) — from follow-up investigation:
- PGO USE vs non-PGO: 4.27 H/s vs 4.27 H/s (15s and 90s training).
- NEON `vtbl` software-AES: ~19.4% regression on `fill_aes_1r_x4`/`hash_aes_1r_x4`.
- Full `ctest` 12/12 green with `ARMRX_ENABLE_NEON_AES=ON`.

## What was NOT verified (requires the AArch64 dev box)

- Any JIT-execution hashrate claim (JIT excluded on x86_64).
- Peephole JIT coalescing payoff (3–6 week effort, not yet started).
