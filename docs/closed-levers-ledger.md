# Closed-Levers Ledger (armrx)

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
| E15/E16 multi-worker scaling (`9935734`) | measured | XMRig=28 H/s baseline; gap thermal + SoC asymmetry | **OPEN lever** (not closed — see below) |
| N1 ldp/stp adjacency (`88cda14`, `c5ac985`) | 2 (W2-2, W2-3) | CLOSED: no fusion possible | No (conclusive) |

---

## Levers still OPEN (armrx code, generic AArch64 — not device-specific deploy)
These are code changes in the miner that help ANY AArch64 RandomX device, not
device-specific tuning (isolcpus / clock-OPP unlock are deploy-level, OUT of scope
for armrx itself — omitted from this ledger on purpose).
- **Superscalar timing-model re-tune** (`improvement-headroom.md` top pick) — **ADOPTED
  (Design C) 2026-08-07** via `91f5b2f`. `src/superscalar.cpp` modeled x86 2-MUL ports;
  A53 has 1 MUL port. Design C set `dependent_=true` on `IMULH_R`/`ISMULH_R` MUL ops
  (2 lines) → `other_interlock_stall` halved (23.3→11.63M/hash) at **H/s parity**
  (bench_armrx 8w: main 13.48 vs C 13.98 H/s). Stall-win didn't lift H/s (8w bound by
  E16 interconnect, not multiplies) but is a free code-quality win. A/B reverted (broke
  reference hashes). **CLOSED-as-satisfied** (not dead — produced a keeper).
- **Core-0 / main-thread contention in pool mode** — **OPEN (the last remaining generic
  AArch64 code lever, 2026-08-07).** In 8w non-isolated pool mining one worker lands on
  core 0 alongside the main/stratum reader thread, JSON handling, and the 1 Hz console
  print — contention the local `bench_armrx` never has (explains why bench showed ~28.4
  parity but pool ran ~24.76 pre-isolcpus). Scheduling-only mitigation (deprioritize the
  main thread, or bind the stratum reader to an off-worker core, or throttle the console
  print) is **portable** (helps any big.LITTLE / dual-cluster AArch64, not device-specific)
  and low-risk. Estimated **~+2-4%** H/s. Per-family rule: this is a single-design lever
  (scheduling tweak); one clean design + on-device A/B suffices to close. Kill criterion:
  no H/s gain vs current pool baseline (~31.5 H/s with `--dataset-mb=512`) → CLOSED.
- **E16 multi-worker scaling** — **RETRACTED as a code lever (2026-08-07, `docs/archived/audits/2026-08-07-improvement-headroom.md`).** The 8w 5.4× vs XMRig 6.1× ratio is the **SoC's own two-cluster interconnect asymmetry that XMRig also bears** (XMRig fast 4.5 / weak 2.4 H/s per-core) — not a code deficit armrx can close. Only `isolcpus` (deploy-level, out of scope) moves it. **CLOSED as a silicon-bound mirage.** The *only* remaining code-adjacent item in this area is **main-thread / core-0 contention in pool mode** (~+2-4% via scheduling-only deprioritization of the main/stratum thread on core 0; portable, unmeasured). See "Open levers" below.

---

## How to use this ledger
When a lever is closed: add a row with the designs tried + kill-criterion type. When
re-opening (per-family rule): note it and link the brief. When adopting: move to changelog
as normal. This doc is the anti-"we missed a variant" record.
