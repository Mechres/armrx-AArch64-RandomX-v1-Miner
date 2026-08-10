# Closed-Levers Ledger (armrx)

> **Authoritative source.** This ledger is the single standing record of
> optimization-lever status (adopted / open / closed / dead) for armrx. When
> other docs (README, ROADMAP, STRATEGY, RETROSPECTIVE, `docs/archived/*`) disagree
> on a lever's status or a historical measurement, **this file wins**. Historical
> measurement figures quoted here or elsewhere may predate later re-baselines
> (e.g. the superscalar body was re-counted at **5,224 A64 instr/call** by the
> W1-1 census, `docs/experiments/w11-instruction-census.md`, superseding earlier
> `3,563` snapshots) — treat such numbers as dated, not authoritative.

**Purpose:** A single standing record of every optimization lever that was closed
(reverted / dead / not-adopted), noting (a) the DISTINCT DESIGNS tried, (b) whether the
kill criterion was PER-DESIGN or PER-FAMILY, and (c) whether a per-family re-attempt is
warranted under the 2026-08-07 rule (try N designs, close only if ALL fail). This exists
because the project's prior discipline was "try once, reject if worse" — which can miss a
variant that would have worked. Seed data from `git log` (324 commits) + RETROSPECTIVE.md
+ this session. Updated whenever a lever is closed or re-opened.

**Rule (2026-08-07, canonical):** kill criterion is PER LEVER FAMILY. A single failed
design closes only that design, not the lever. Close the lever only if ALL distinct
designs fail their gate.

---

## Session 2026-08-07 (Hermes + user, with delegated agents)

| Lever | Designs tried | Kill criterion | Outcome | Re-attempt warranted? |
|---|---|---|---|---|
| Dual-hash interleaving | 1 (measure interleaved vs single) | per-design (premise: no MAC slack) | DEAD — no H/s gain, no interlock reduction | No (binary; conclusive) |
| Tier 2(a) scalar fill | 1 (scalar path) | per-design | Reverted (207s vs 163.3 NEON) | No |
| Tier 2(b) across-item vectorization | **1** (4-wide fake-SIMD) | per-design | Reverted (165s > 163.3) | **Re-opened per-family (2026-08-07) → ALL 3 FAILED → CLOSED** |
| Hybrid hash-during-fill (Part 2+3) | **1** (instant + exclude-all-miner-cores) | per-design | Reverted (23.16 H/s) | Was re-explored as 3-variant dead-stop attempt (see below) |
| Hybrid dead-stop (3 variants) | 3 (tiered threshold / co-located / smaller default) | per-family (user push) | All failed H/s gate; dead-stop is optimal | No (physics: no free cores) |
| Tier 2(b) RE-ATTEMPT | 3 (8-wide / true-NEON-mulh / cacheline) | per-family | **ALL 3 FAILED** (A 165.30s, B broken-multiply at item0, C 164.70s) → **CLOSED per-family** | No (no 4th variant justified) |

**Tier 2(b) re-attempt** (`docs/briefs/2026-08-07-tier2b-multiwidth.md`): branches
`try/tier2b-8wide`, `try/tier2b-neon-mulh`, `try/tier2b-cacheline`. This is the correction
to the prior single-design closure.

---

## Pre-2026-08-07 history (from `git log`, 324 commits)

These were mostly closed on a SINGLE design (the "implement → measure → cost moved →
revert" pattern, RETROSPECTIVE.md:175). Listed so a per-family re-attempt can be judged.
"Re-attempt?" = would multiple distinct designs likely change the conclusion.

| Lever (commit) | Design tried | Note | Re-attempt warranted? |
|---|---|---|---|
| PartialDataset wait_for_fill in mining (`ce1e8ee`) | 1 (drop block, hash during fill) | **Same as dead-stop Part 2+3** — stalled at 512 MiB. Tried before this session. | No (covered by dead-stop 3-variant attempt) |
| Track B inline hit path (`4809838`, `2886b04`, `27e7c41`) | 1 (inline bound check) | reverted; marked paused | Maybe (different inlining strategy) — low priority |
| Memory-op scheduler extension (`65f0101`, `7ac8125`) | 1 (extend `*_M` opcodes) | caused JIT/interpreter divergence, reverted, mechanism unidentified | **Maybe — distinct opcode subset could avoid divergence** (risky) |
| T2-1 PRFM hints (`05f4c70`) | 1 | measured regression, dead end | No (PRFM useless on this uarch) |
| T2-2 dual-issue alignment (`533b2fa`) | 1 | measured regression | No |
| W3 register-hoist (`9623c0d`) | 1 | REJECTED infeasible | No |
| W3-2 memory-op scheduler bisection (`7ac8125`) | 1 (bisection) | CONFIRMED UNSAFE, closed | No (mechanism found) |
| W4 dense NEON literal-pool (`d978d05`) | 1 (phase-1) | FAILED correctness, REVERTED (valid technique, needs dedicated region) | **Maybe — phase-2 dedicated region not tried** |
| E26 Approach A `*_M` hoist (`ebbac34`) | 1 | FAILED on device, reverted | Maybe (different hoist target) |
| T3-3 IMUL_R magnitude gate (`5035431`) | 1 (gate at 0.003%) | FAIL (≪30%), F3 lane-pack closed | No |
| W1-3 Blake2b share (`08a9160`) | 1 | ~0.5%, no action | No |
| Track I core-0 cost (`5926fd9`) | 1 | ~zero, closed | No |
| CBRANCH CSEL (`b406bd5`, RETROSPECTIVE:84) | 1 | branch-miss figure was wrong (2.4% not 31%) | No (premise invalid) |
| Argon2 memcpy copy-elimination (`88559a0`) | 1 | regression (cost relocated) | No |
| DAG scheduler (`cb01a20`) | 1 (gated) | −14.3% H/s, not adopted, kept gated | No (measured negative) |
| E15/E16 multi-worker scaling (`9935734`) | measured | XMRig=28 H/s baseline; gap thermal + SoC asymmetry | **RETRACTED as silicon-bound mirage (see below)** |
| N1 ldp/stp adjacency (`88cda14`, `c5ac985`) | 2 (W2-2, W2-3) | CLOSED: no fusion possible | No (conclusive) |

---

## Levers reconciled (armrx code, generic AArch64 — not device-specific deploy)
These are code changes in the miner that help ANY AArch64 RandomX device, not
device-specific tuning (isolcpus / clock-OPP unlock are deploy-level, OUT of scope
for armrx itself — omitted from this ledger on purpose).

**Correction (2026-08-07, independent 3rd-party review):** an earlier draft of this
section claimed "no open code levers remaining" and recorded the superscalar
timing-model re-tune as ADOPTED (Design C). Both were wrong — see below. The
superscalar family is NOT closed (all three designs failed), and two additional
portable levers (dead superscalar literal-pool, main-thread deprioritization) were
found open. Current honest state:

- **Superscalar timing-model re-tune** — **NOT ADOPTED; family RE-OPENED as failed
  (2026-08-07).** Three designs were tried:
  - A/B: modeled x86 2-MUL ports on the A53's single MUL port → **broke reference
    hashes** (reverted).
  - C (`91f5b2f`, later reverted by `8cdf311`): set `dependent_=true` on
    `IMULH_R`/`ISMULH_R` MUL ops. **Provably a NO-OP** — the generator calls
    `scheduleMop(..., scheduleCycle, scheduleCycle)` (`src/superscalar.cpp:623`), so
    `depCycle == cycle` and `isDependent()`'s `cycle = max(cycle, depCycle)` is
    inert. Verified independently: HEAD vs reverted tree produce **byte-identical**
    superscalar programs (FNV-equal across 512 programs; `armrx_tests` reference
    hashes unchanged). The "H/s parity / stall halved 23.3→11.63M" earlier reading
    was run-to-run/thermal noise. NOTE: the ledger previously also asserted E24
    already halved the same baseline to 6.18M — the two claims were mutually
    inconsistent; with C proven inert, E24's figure stands and C contributed nothing.
  - Under the project's per-family rule, since all three designs failed (A/B broke
    hashes, C inert) the family is **not a keeper** and was reverted. It is recorded
    here as **CLOSED-as-failed** (not dead-by-principle — simply no working design),
    and remains a candidate for a *different* future design if one is motivated.
- **Core-0 / main-thread contention in pool mode** — **RE-OPENED (2026-08-07,
  independent review), then MEASURED AS NO-OP (2026-08-07 device A/B).** The
  earlier "CLOSED as DEAD" reasoning was unsound (false dichotomy + waived
  per-family rule). The real, portable lever is **deprioritizing the main thread**
  (which runs the pool tick + console-status loop), implemented opt-in as
  **`--main-thread-policy=default|idle|batch|nice=N`** (`sched_setscheduler` /
  `setpriority`, no CAP_SYS_NICE needed to *lower* priority; default = unchanged).
  Correctness gate PASSED on-device (test_jit_equivalence 16/16 + 4 KATs, cross-built
  GCC-16 musl). **Decisive gate — real-pool `--pool-test` A/B (lenovo / MSM8929 A53,
  no isolcpus, `--dataset-mb=512 --workers=8 --seconds=360`, identical `taskset -c 0-7`,
  built-in test pool+wallet, both arms):**
  - **Baseline (`main`, no policy):** steady-state aggregate **~32.2 H/s** (live
    Speed 31.8–32.8; INST agg 31–33; fast-cluster 4.2–6.0 / weak-cluster 2.4–3.0 per worker).
  - **Treatment (`try/main-thread-deprioritize`, `--main-thread-policy=nice=10`):** steady
    **~31.5 H/s** (live Speed 30.8–32.2; INST agg 29.6–33.1).
  - **Δ ≈ −0.7 H/s (≈ −2%), within run-to-run thermal/working-set noise (CPU held 57°C
    both arms).** The brief dips in the treatment arm correlated with CryptoNote job
    rotations, not the policy. **Conclusion: deprioritizing the main thread produces no
    measurable H/s change** — under `SCHED_OTHER`, the OS already yields the main
    (non-realtime) thread to the `SCHED_OTHER` workers; the main thread's pool-tick /
    console work is too infrequent/light to steal meaningful worker-0 cycles. (Note:
    this was measured WITHOUT `--rt-priority`; with workers on `SCHED_FIFO` the main
    thread already yields by priority class, so the lever is even less likely to help
    there. A future `idle`/`SCHED_IDLE` probe could be re-run if a specific regression
    is observed, but as a standalone lever it is closed as measured-null.) Recorded as
    **CLOSED-as-measured-no-effect**; branch `try/main-thread-deprioritize` kept as
    evidence. The code is opt-in (default = unchanged behavior) so it carries no
    regression risk if left in the tree.
  `try/remove-dead-superscalar-cpool`, commit `1b10f02`).** `emitCpoolImmediate`
  unconditionally emits MOVZ/MOVN+MOVK for every C* immediate and never reads the
  per-program 128-slot (1 KB) inline pool that was reserved + zeroed for every
  superscalar dataset-item program. The pool state (`cpoolBase_`/`cpoolLiteralPos_`/
  `cpoolSlot_`) was written but never consumed. Removed ~8 KB of dead data spliced
  into the hottest code region (superscalar body ≈ 80.5% of all instructions). Pure
  dead-code removal; `armrx_tests` reference hashes byte-identical. Portable
  code-quality / I-cache win, no behavioral change.
- **Worker-local buffer reuse (D2)** — **ADOPTED (ledger corrected 2026-08-10).** `worker_loop`
  (`src/mining_engine.cpp:476-477`) already declares `block_input` / `next_block` as
  **per-worker local vectors**, and `:554` copies the job template into the existing
  buffer (`resize` only on job change) — no per-iteration heap realloc. The reusable
  per-worker buffer was therefore already realized; the prior "OPEN, untried" label was
  stale (post-audit re-review, Luna #7 / Deepseek closure-finding #7, both source-verified).
  Portable, low-risk, small expected impact. **Distinct from "Cross-hash boundary
  pipelining (Track D2)"** (README:166, also adopted) — that is a different D2 lever.
- **Cross-LTO (D1)** — **build-conditional, not a code lever.** LTO is wired in
  CMake (`ARMRX_DISABLE_LTO`, off by default) but disabled on the musl cross
  toolchain (GCC 15 + musl crash history). On a non-musl AArch64 build (Debian/
  Ubuntu gcc) IPO engages and may recover the ~+1.9% previously measured. Out of
  scope for the musl deploy path; noted for non-musl packagers.
- **E16 multi-worker scaling** — **RETRACTED as a code lever (2026-08-07, `docs/archived/audits/2026-08-07-improvement-headroom.md`).** The 8w 5.4× vs XMRig 6.1× ratio is the **SoC's own two-cluster interconnect asymmetry that XMRig also bears** (XMRig fast 4.5 / weak 2.4 H/s per-core) — not a code deficit armrx can close. Only `isolcpus` (deploy-level, out of scope) moves it. **CLOSED as a silicon-bound mirage.**

---

## How to use this ledger
When a lever is closed: add a row with the designs tried + kill-criterion type. When
re-opening (per-family rule): note it and link the brief. When adopting: move to changelog
as normal. This doc is the anti-"we missed a variant" record.
