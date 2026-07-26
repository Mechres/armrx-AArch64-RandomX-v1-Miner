# Changelog

## 2026-07-26 — Performance Plan Step 1 Run: Gate Closed, No Open Performance Leads Remain

`docs/plans/performance-plan-20260725.md`'s Step 1 — bound how much of the main VM program's
~2.2× IPC penalty is recoverable memory-latency stall versus architectural floor, before
attempting either of the two gated follow-on steps — was run to completion on-device.

- New `bench_armrx --scratchpad-real`/`--scratchpad-l1` flags and `VirtualMachine::run_execute_only()`
  isolate the JIT-compiled main-VM-program execute step and re-run it against either the real
  2 MiB scratchpad or a 16 KiB `memfd` tiled 128× across the same virtual range (so every JIT-
  computed address lands on the same L1-sized physical backing, with zero change to the JIT's own
  address-masking logic).
- `perf stat -e cycles,instructions`, 2000 iterations each, on-device: real scratchpad IPC 0.6572
  vs. L1-aliased IPC 0.6971 — instruction counts matched to 5 decimal places, a clean comparison.
  Forcing near-zero scratchpad latency bought only **+6.07% IPC**.
- Per the plan's own gate, that's a small recoverable gap against the region's ~2.2× overall
  penalty — **the stall is mostly not a memory-latency problem**. This closes Step 2 (`PRFM`
  prefetch) and Step 3 (bisecting the reverted memory-op scheduler hazard) without attempting
  either; both target latency, and Step 3 specifically would have accepted real correctness risk
  (silent wrong hashes) for a ceiling that turned out to be low. The residual penalty reads as
  architectural (in-order pipeline/dependency-chain-bound), not something further code-level work
  can chase. Full account in `docs/experiments/scratchpad-locality-bound-20260726.md`; `PLAN.md`
  Phase 9 and `NEXT_STEPS.md` updated to match.
- Also found and fixed along the way: `tools/devbox/devbox_mcp.py`'s `devbox_build`/`devbox_test`
  were silently serializing on-device builds and parallel `ctest` runs onto core 0 under
  `isolcpus` (same root mechanism as the worker-count/worker-placement bugs, just hitting the
  build tooling) — both now prefix their remote commands with `taskset -c
  "$(cat /sys/devices/system/cpu/online)"`.

## 2026-07-25 — `isolcpus`/`rcu_nocbs` — Real ~14% Hashrate Win, Biggest in Project History

The user installed `setcap` and granted explicit permission to edit the boot cmdline and reboot
the device, unblocking `PLAN.md`'s one remaining open item. Result:

- `isolcpus=1-7 rcu_nocbs=1-7` (core 0 left for kernel housekeeping) gives a reproducible **~28.4
  H/s aggregate 8-worker steady-state hashrate vs. ~24.9 H/s without it (+14%)** — measured across
  2 no-isolation and 4 isolated rounds (3 of 4 tightly reproducible, 1 anomaly). Every prior
  adopted change in this project has been sub-1%.
- Per-worker breakdown gives a clean mechanism, not just an aggregate number: the fast cluster
  (cores 0-3) is identical either way; the entire effect is 2-3 of the 4 slow-cluster (cores 4-7)
  workers randomly losing half their throughput to background OS work/interrupts without
  isolation, recovered with it.
- `nohz_full=1-7` silently no-ops on this kernel (`CONFIG_NO_HZ_FULL` not set) — real finding for
  future deployment.
- `--rt-priority`'s independent contribution is unconfirmed (functionally engaged, but no
  isolated round showed an effect beyond `isolcpus` alone); a `perf stat` attempt to verify hit a
  real, unresolved thread-attribution tooling gotcha against the full multi-threaded binary.
- Two stale historical baseline figures and an overconfident thermal-throttling attribution were
  caught and corrected mid-investigation — the user pushed back with direct knowledge of the
  hardware and the source documents each time, preventing this from being written up as a false
  "no effect" conclusion. Full account, including the false starts, in
  `docs/experiments/isolcpus-rt-priority-win.md`. `PLAN.md`'s Phase 8 closed with this result.

## 2026-07-25 — Performance-Planning Docs Reconciled; Phase 7 Archived

A second agent independently wrote `docs/plans/performance-plan-20260725.md` (gated, prioritized
steps) and `docs/plans/experimental-performance-ideas-20260725.md` (speculative backlog) on top
of this session's `docs/plans/future-performance-ideas-20260725.md`, creating three overlapping
docs. Fact-checked both new docs against the actual codebase before reconciling:

- Verified accurate: the region-IPC table, the Argon2-cache-lacks-`MADV_POPULATE_WRITE` claim
  (`src/vm.cpp:147` vs `src/argon2.cpp:277`), the superscalar `IMUL_RCP` register-pre-assignment
  asymmetry claim, and that `-fvisibility=hidden`/`-fno-semantic-interposition` and an explicit
  non-ASAN frame-pointer directive are both genuinely absent from `CMakeLists.txt`.
- Fixed a broken cross-reference in `performance-plan-20260725.md` (pointed at the wrong sibling
  doc for "the full speculative backlog").
- Re-labeled `experimental-performance-ideas-20260725.md`'s double-buffered-JIT "~1.76% max"
  figure as an estimated upper bound, not a measured result; flagged its `.p2align 6` idea as a
  duplicate of an already-known Deepseek-audit item.
- Merged `future-performance-ideas-20260725.md`'s unique content (PGO re-check tracking, tooling
  reconciliation note, misc. audit-flagged items) into `experimental-performance-ideas-20260725.md`
  as items 13-15, then retired the now-superseded original to
  `docs/archived/future-performance-ideas-20260725.md`.
- Split `PLAN.md`'s now-fully-closed Phase 7 into `docs/archived/plan_phase7_completed.md`
  (same pattern as Phases 1-5/6), leaving only the one open item (`--rt-priority`) as the new
  Phase 8. Updated all downstream references (`README.md`, `ROADMAP.md`, `CLAUDE.md`,
  `REASONIX.md`, `docs/experiments/memory-op-scheduler-attempt.md`).

## 2026-07-25 — Doc Organization: Experiment Writeup, Audit Moved, Future-Ideas Backlog

Follow-up housekeeping after the memory-op scheduler attempt and Deepseek audit (both below):

- Wrote up the full memory-op scheduler attempt as a proper experiment doc,
  `docs/experiments/memory-op-scheduler-attempt.md`, matching the format of
  `argon2-compress-copy-elimination.md`/`neon-vector-permute-aes.md` — problem, what was tried,
  result, investigation (including the two hypotheses ruled out), the CBRANCH-assert side
  finding, and where this leaves future work.
- Moved `PROJECT_AUDIT_REPORT_20260725_Deepseek.md` from the repo root into `docs/audits/`,
  alongside the Gemini/Hermes reports and the three scheduler-review reports. Updated the two
  stale "kept at repo root" notes in `changelogs.md` and `docs/archived/plan_phase6_completed.md`
  to reflect the new location.
- Wrote `docs/plans/future-performance-ideas-20260725.md` — a speculative, explicitly-unscheduled
  backlog of what to try if performance work resumes: bisecting the memory-op scheduler hazard
  properly, an explicit-`PRFM` alternative to scheduling for the main VM program's memory-op
  stalls, bounding the achievable win before committing to either, and an honest recommendation
  that further microarchitecture work needs a new specific hypothesis, not just more looking.
  Linked from `PLAN.md`'s Phase 7 header and `ROADMAP.md`'s Reference Docs table.

## 2026-07-25 — Memory-Op Scheduler Extension Tried, Reverted; a Real Assert Bug Found Along the Way

Follow-up to the entry below (peephole JIT closed, memory-op scheduler extension started).
Implemented the extension: flagged the memory-load opcodes (`*_M`) as `is_long_latency` in
`computeFootprint()`, making them eligible as swap triggers (`P`) the same way `IMUL_R`/
`IMUL_RCP` already were. No new hazard-model change seemed necessary — the existing
memory-memory-always-hazard rule already prevents a `*_M` op from ever swapping past another
`*_M` op, so marking it long-latency should only ever fill a stall with an independent register
op.

`test_jit_equivalence` failed immediately, on its first and most basic (seed, input) pair — the
first time this specific test has ever failed in this project's history. Reverting the memory-op
change alone (keeping everything else) made it pass again, confirming the extension itself is
the cause. Investigated the mechanism: checked whether `emitMemLoad`'s own `src==dst`
shared-physical-scratch-register special case (structurally similar to the *original* `src==dst`
hazard that required item 12's Q/R exclusion) was responsible, but that exact pattern was already
reviewed by the three independent code reviews (Deepseek, Gemini, Hermes) and confirmed
self-contained regardless of swap position — so it doesn't explain this divergence.

**The exact hazard mechanism was not conclusively identified.** Given the failure mode is silent
wrong hashes and this was an explicitly speculative, "may be a null result" experiment from the
outset, fully reverted rather than ship a targeted exclusion without being able to verify it —
this project's own standing rule is to trust empirical results over incomplete theory, but here
there was no working fix to trust, just a clean revert back to the known-good state.
`tools/jit_correlate.py`'s region-split extension (kept, unaffected) remains valuable diagnostic
infrastructure regardless of this outcome.

**Real bug found and fixed while investigating**: the CBRANCH defensive `ARMRX_ASSERT` added
earlier the same day (see the "Two Deepseek-Audit Hardening Items" entry below) fired repeatedly
during this investigation — on a completely normal `test_jit_equivalence` run, not the "may not
be" scenario. Its premise (that a CBRANCH targeting a never-written register is "theoretical
only," per the Deepseek audit's characterization) was factually wrong. Traced why the existing
behavior is actually correct and intentional: `register_usage_[creg] == -1` wraps `pc` to `0` via
the execute loop's `++pc`, i.e. "restart from VM instruction 0" — this exactly matches the JIT's
own `reg_changed_offset[]`, which is reset to `PrologueSize` (VM instruction 0's own code offset)
before every compile in `emitPrologueMix`. Both paths already handle this correctly; the assert
was flagging normal behavior as if it were exceptional. Removed (`4da77f0`).

**Performance work is closed out for this session.** Both the direct lead (peephole JIT) and its
evidence-backed follow-on (this memory-op extension) have now been tried or ruled out with
reasoning. The only genuinely open item remaining is `--rt-priority`/`isolcpus=` (blocked on
user device access).

## 2026-07-25 — Peephole JIT Coalescing Closed on Evidence; Memory-Op Scheduler Extension Started

Followed up on the ~22% of mining cycles item 14's opcode-level correlation left unattributed
("inside a worker JIT buffer but outside any superscalar entry"). Extended
`tools/jit_correlate.py` to split that bucket using the `code_size` boundary the tool already
parsed but never actually used to classify sample addresses — offsets below `CodeSize` are the
main per-hash VM program (regenerated every hash), offsets at/above it are the superscalar/
dataset-derivation region.

Ran two live `perf record` captures on the identical 8-worker mining workload used for item 14's
original correlation — one `-e cycles` (the existing default), one `-e instructions` (new) — to
get region-level relative IPC, not just a cycle-share number:

| Region | % of instructions | % of cycles | relative IPC |
|---|---|---|---|
| Main per-hash VM program | 9.23% | 20.04% | **0.461×** avg |
| Superscalar, opcode-attributed | 72.71% | 63.52% | 1.145× avg |
| Superscalar, unattributed (fixed wrapper chunks) | 2.49% | 2.38% | 1.046× avg |
| Outside any JIT buffer (named C++) | 15.56% | 14.05% | 1.107× avg |

**The main VM program region carries ~9% of dynamic instructions but ~20% of cycles — a ~2.2×
IPC penalty relative to the rest of the pipeline.** This is a stall signature (this region is
where the memory-operand opcodes `*_M`/`ISTORE` — ~48% of its code bytes per the original static
breakdown — do genuinely random 64-byte reads/writes into the 2 MiB scratchpad), not an
instruction-count signature. Branch misprediction was already ruled out separately (2.4%
hot-path miss rate, ~0.1-0.16% of cycles). The superscalar unattributed slice, once isolated,
turned out proportionate (~1.05× IPC) — not a real lead, just noise from the old lumped bucket.

**Conclusion: this is evidence against peephole JIT coalescing, not merely an unmet gate.**
Peephole's whole premise is code-density/instruction-count reduction; the one remaining
unexplained slice of cycles is expensive because of memory-latency stalls, which code-density
reduction can't fix. This is the same root cause every single instruction-count-reduction
attempt this project has tried has independently reached (CSEL, Newton-Raphson, NEON-AES ×3,
superscalar literal-pool relayout, `IMUL_RCP` literal-load elimination — all implemented,
measured, reverted). **Closed** — not starting the 3-6 week clean-room peephole rewrite against
evidence that specifically points away from it.

**In its place**: started extending the emitter lookahead scheduler to hide the main VM
program's memory-op latency — the same mechanism (reordering to hide stalls, not reducing
instruction count) that already produced a real, measured win for the superscalar region's
`IMUL_R`/`IMUL_RCP` stalls. `tools/jit_correlate.py`'s region-split extension is kept as
reusable diagnostic infrastructure regardless of how this turns out.

## 2026-07-25 — Two Deepseek-Audit Hardening Items Applied (Phase 7 Items 3-4)

Both small, cheap, optional items flagged by the Deepseek audit (see the "Full Codebase Audit"
entry below): added `-frounding-math` to `armrx_core`'s compile options (correctness-by-
construction for the RandomX VM's runtime `fesetround()` dependency, not a fix for an observed
bug), and a defensive `ARMRX_ASSERT` in `h_CBRANCH` for a target register that was never written
(`register_usage_[creg] == -1` would otherwise wrap `pc` to `0` via `int16_t` truncation +
`++pc`, silently restarting the program instead of a defined branch — theoretical only, RandomX's
program generator spec-guarantees this can't happen, zero cost in release builds). Verified:
local x86 full suite 7/7, on-device (AArch64) targeted suite 5/5, no new warning categories.
Committed `059b6fe`. Closes Phase 7's two optional items — `--rt-priority` (blocked on device
access) and peephole JIT (still gated) are the only genuinely open items remaining.

## 2026-07-25 — `PLAN.md` Phase 6 Archived, Docs Refreshed Project-Wide

Phase 6 had grown to ~710 of `PLAN.md`'s 770 lines of now-completed history — the same size
threshold that triggered the Phases 1-5 archive split on 2026-07-24. Split it out the same way
into `docs/archived/plan_phase6_completed.md`, following the established precedent exactly.
Folded in the scheduler-review fixes and the Deepseek audit verification (both entries below)
before archiving, so the historical record is complete. `PLAN.md` is down to 143 lines: Phases 1-5 summary (unchanged), a
new Phase 6 summary, and a renamed "Phase 7" section listing the two genuinely open items
(`--rt-priority`, blocked; peephole JIT, still gated) plus the two small optional hardening items
from the Deepseek audit.

Also refreshed `README.md` (corrected the "PGO Enabled"/"Instruction Scheduling" feature claims —
PGO is tooling-available-but-measured-null, not a performance feature; added the emitter
scheduler as an actual adopted feature), `ROADMAP.md` (Phase 6 table closed out with the missing
rows, P3/`--rt-priority` relabeled under Phase 7, two new Deepseek-audit hardening rows), and
`NEXT_STEPS.md` (fixed a stale unchecked PGO-recheck checkbox, added a Phase 7 summary at the
top). Committed `405755f` for the `PLAN.md`/archive split; this pass covers the remaining docs.

## 2026-07-25 — Full Codebase Audit (Deepseek) Reviewed and Verified

User supplied `PROJECT_AUDIT_REPORT_20260725_Deepseek.md` (initially kept at repo root; moved
into `docs/audits/` later the same day alongside the other audit reports). Per this project's
standing discipline, verified every concrete, checkable claim against actual code/live device
state before accepting any of it:

- **§1.1, scratchpad huge-page residency "unverified"** — refuted. Verified live on-device:
  `/proc/<pid>/smaps` merges adjacent same-protection anonymous VMAs into one entry, so the
  2 MiB scratchpad doesn't appear as a separate line item — it merges with the 256 MiB Argon2
  cache into one 258 MiB (264192 kB) region showing `AnonHugePages=264192`, i.e. 100%
  huge-page-backed for the combined region. Not a real gap; already covered by the original
  Phase 6 item 1 finding.
- **§1.2/§1.3 (`register_usage_[8]` initializer, rounding-mode sentinel)** — accurate but not
  new: both already have inline code comments explaining why they're safe. §1.3's specific claim
  traced through `reset_rounding_mode()` and confirmed a non-issue (it sets the cache to 0 *and*
  calls `fesetround(FE_TONEAREST)` in the same call, keeping cache and hardware state in sync).
- **§4.1 (CBRANCH with an unwritten target register wraps `pc` to 0)** — confirmed accurate by
  tracing `h_CBRANCH` → `execute_bytecode()`'s `pc = ibc.target` → `for (int pc = 0; pc < 256;
  ++pc)`. Correctly characterized by the audit as theoretical (RandomX's program generator
  spec-guarantees registers are written before being branched on) — no known real-world trigger.
- **§3.2 (`-frounding-math` missing from `CMakeLists.txt`)** — confirmed real via direct grep;
  the audit's own risk assessment (JIT emits raw AArch64 FP instructions, interpreter uses
  runtime bytecode dispatch, so the compiler can't constant-fold VM float values) holds up. The
  one genuinely actionable item from this audit — cheap, defensible, not yet applied.
- §2.1's cited scheduler IPC numbers (+0.233%/−0.036%) matched this project's own actual
  measurements exactly — a good signal the audit is grounded in real project docs, not
  fabricated.

**No new bugs found. No code changed** — one previously-open question (§1.1) closed as a
non-issue; one small optional hardening item (`-frounding-math`) tracked in `PLAN.md` Phase 7.

## 2026-07-25 — PGO Re-Check After Scheduler Landing: Confirmed Null (and a Core-Pinning Near-Miss)

Re-ran `devbox_pgo_build` (PLAN.md item 15) now that both scheduler commits changed the JIT's
instruction mix meaningfully. First attempt (unpinned `bench_armrx --full-hash-only`, PGO vs.
non-PGO) showed a misleading ~2x gap (4.47 vs. 2.24 H/s) that looked like a real PGO win — the
user caught the likely cause before it was accepted: this device's two 4-core clusters run at
different clock speeds (per the earlier "two L2 clusters" finding), and unpinned processes can
land on either. Confirmed directly: the identical non-PGO binary alone gives 4.48 H/s on core 0
and 2.24 H/s on core 4 — a 2x swing with zero code difference. Re-measured properly with
`taskset -c 0` pinning both binaries to the same core: PGO 223570.61 μs / 4.47 H/s vs. non-PGO
223203.66 μs / 4.48 H/s — identical within noise. **PGO remains a genuine null on this code
shape**, confirming the original 2026-07-24 finding still holds after the scheduler work. Build
reverted to the normal (non-PGO) configuration.

Also caught along the way: `devbox_build(reconfigure=true)` alone is not sufficient to get back to
a clean non-PGO build if the build directory still has leftover `.gcda` profile data from a prior
PGO run — CMakeLists.txt auto-detects the profile data and stays in PGO-USE mode regardless of the
flags passed, silently just relinking cached objects (zero recompilation). `clean=true` is required
to actually get a clean baseline. Worth remembering for any future PGO A/B comparison on this
project.

**Methodology note, worth internalizing**: any `bench_armrx` wall-clock comparison on this device —
not just `perf stat -p <pid>` A/B trials — must pin to a specific core. This generalizes item 12's
own `taskset` lesson (originally learned for `perf stat` comparisons) to plain wall-clock
benchmarking too.

## 2026-07-25 — Three Independent Scheduler Code Reviews (Deepseek, Gemini, Hermes) Acted On

Requested a deep, skeptical review of the emitter scheduler commits (`be94b1f`, `6712479`) given
it's consensus-critical code (silent-wrong-hash risk, not a crash risk) that had just gone
through a non-trivial correctness arc (two empirically-found hazards). Three independent
reviews, all converging on "no constructible failure scenario in the shipped code" — CBRANCH
anchor logic, the superscalar `IMUL_RCP` exclusion, index bounds, and cross-scheduler
independence all check out across all three.

**One real, convergent finding**: two of three (Deepseek, Hermes) independently caught that the
`src==dst` exclusion's doc comment was factually wrong about `h_IROL_R` — it claimed the handler
uses the shared x20 scratch register when `src==dst`; the actual code does the opposite (x20
only when `src!=dst`; `src==dst` takes a different, x20-free `ROR_IMM` path). Gemini restated
the original (wrong) claim without checking it against the handler's actual code — a real
quality gap in that review, not just noise. User chose not to use Gemini for future
scheduler-adjacent reviews as a result.

**Fixed**: rewrote the doc comment to drop the falsified "x20 race" theory and record what's
actually settled (the exclusion's *necessity* was already proven by the original bisection — a
real divergence reproduced with it disabled, for a genuine `src==dst` case,
`R=ISUB_R(dst==src==7)`) vs. still open (the exact mechanism — Hermes's sharper argument is that
the ordinary register-hazard check already covers same-register cases, so the exclusion can only
matter in the no-shared-register case, where the x20 story gives no hazard either). Also added,
per Hermes: a fail-safe assert in `resolveInstructionType()` (previously silently collapsed any
future unrecognized JIT handler to a NOP footprint — no hazard bits — permitting an unsafe swap
across it; now asserts loudly and fails safe, treating it as a barrier), and a comment
documenting why `generateSuperscalarHash()`'s `num32bitLiterals=64` pin is load-bearing for
scheduler safety.

**A side attempt to resolve the `src==dst` mechanism definitively — abandoned, not shipped.**
Tried building a hand-crafted-`Program` differential test (a new `VirtualMachine::
run_with_program_for_testing()` test hook, bypassing blake2b generation) to force the excluded
swap and observe directly whether it diverges. Hit an unrelated methodological trap instead:
comparing VM state after a single `run()` call (rather than the full 8-round
`randomx_calculate_hash()` chain) shows spurious divergence for any program that never touches
float registers — plausibly `run_jit()`'s `eMask`-into-`f[0]` side-channel trick leaving stale
data when nothing overwrites it, though this wasn't confirmed before the attempt was abandoned as
out of scope. Reverted the test-only hook and the test file entirely (`git checkout --
include/armrx/vm.hpp src/vm.cpp`) rather than ship half-understood infrastructure.

Verified 5/5 on-device after the doc/assert changes. Full review reports in
`docs/audits/{emitter-scheduler-review,jit_scheduler_code_review_gemini,scheduler-review-2026-07-25}.md`.
Committed `c92a1a9`.

## 2026-07-25 — Emitter Lookahead Scheduler Extended to Superscalar Path, Measured, Adopted

Follow-on to the same day's earlier scheduler entry (below). First measured the main-program-only
scheduler with `perf stat` (3 interleaved trials, `taskset`-pinned): a clean null (IPC +0.016%,
noise-level) plus a real, reproducible **+24.9% branch-misses** with no compensating benefit.
Root cause: `scheduleProgram()` was only wired into `emitPrologueMix()` (the main VM program,
executed once per hash), but item 14's own `perf`-correlation found the actual dominant `IMUL_R`/
`IMUL_RCP` cost (>35% of all mining cycles) lives in `generateSuperscalarHash()`'s output — the
dataset-derivation region, executed via `bl rx_calc_dataset_item` 16,384x/hash in light mode — a
completely separate JIT emission path the scheduler never touched. User chose to extend the
scheduler to the right region rather than revert.

**Extension**: `scheduleSuperscalarProgram()` + `computeSuperscalarFootprint()` in
`jit_compiler_a64.cpp`, targeting `SuperscalarInstructionType`'s 14 opcodes. Structurally simpler
than the main path: no CBRANCH/CFROUND (superscalar programs are spec-defined straight-line
integer sequences, so no barriers/anchors needed), no memory ops, a single flat 8-register file.
A new hazard specific to this path was found by *reading* the code, not stress-test bisection:
`IMUL_RCP`'s reciprocal literals are populated by a pre-pass in original program order, then
consumed by the main emission loop via a simple incrementing pointer — safe only if no two
`IMUL_RCP` instructions ever have their relative emission order changed by scheduling. Fixed by
excluding `IMUL_RCP` from a swap's two moving positions (checked the main path's own `h_IMUL_RCP`
too — it does *not* have this problem, since it computes its literal slot from its own call count,
entirely self-contained per call, unlike the superscalar pre-pass design).

**Verification**: a second dedicated stress test, `tests/test_jit_superscalar_scheduler_stress.cpp`
— deliberately shaped around **many distinct seeds** (100 seeds x 2 inputs = 200 pairs across 800
individual superscalar programs) rather than many inputs per seed, since `generateSuperscalarHash()`
compiles once per seed rotation and reuses that compiled code for every hash against that seed,
unlike the main program which recompiles fresh every hash. Both stress tests (test_jit_scheduler_stress
re-run: 450/450 still green with both schedulers active; test_jit_superscalar_scheduler_stress:
200/200) plus the full existing suite all green on-device.

**Final performance measurement**: 3-way `perf stat` comparison (baseline `906b96e` / main-only
`be94b1f` / full — separate git worktrees built side-by-side, `taskset -c 0`-pinned single-worker
steady-state windows via `perf stat -p <pid>` after a 10s cache-init warmup, 2 independent rounds
with reversed run order to rule out thermal drift, 6 samples/condition total). An unpinned first
attempt showed a sign flip between rounds (this device has two asymmetric 4-core L2 clusters,
previously documented) — pinning resolved it to a consistent signal: **full vs baseline: IPC
+0.233%, cycles −0.036%** (both rounds agreed: +0.183%/+0.284%); **full vs main-only: IPC +0.318%**
(+0.285%/+0.352%) — the superscalar extension is what makes this a net positive; main-only alone
remained a small, consistent regression vs baseline (−0.085% IPC) even after the extension's fixes.
Branch-miss rate rose from 2.54% to 3.14% (+23.8% relative) — a real cost, but net cycles/IPC still
improved, meaning the stall-hiding benefit outweighs it. Smaller than the original "2-6%" estimate
in `PLAN.md`, but a genuine, reproducibly-measured win (signal clearly exceeds the ~0.02-0.23%
spreads at each condition, matching the "perf stat is the right tool for single-digit-percent
effects" lesson from item 10's prefetch investigation). **Adopted.** See `PLAN.md` Phase 6 item 12
for the full account.

## 2026-07-25 — Emitter Lookahead Scheduler (PLAN.md Phase 6 Item 12): Implemented, Correctness-Verified

Direct follow-on from item 14's `perf`-correlation finding (`IMUL_R`/`IMULH_R`/`ISMULH_R`/`IMUL_RCP`
dominate mining cycles, >35% combined) — a conservative 2-3-instruction emitter lookahead scheduler
in `src/jit_compiler_a64.cpp` (`scheduleProgram()`) reorders VM-instruction *emission* order (never
their computed results) to fill the in-order Cortex-A53's stall after a long-latency multiply with
independent work.

**Correctness design** (full comment in `jit_compiler_a64.cpp` above `scheduleProgram()`):
RAW/WAR/WAW hazard analysis across the int/f/e register files independently; CBRANCH/CFROUND as
hard barriers; a CBRANCH "anchor" constraint (CBRANCH's generated code is a real backward-jumping
loop over a *physical code range*, so the last writer of its target register must never move
relative to its neighbors, unlike the interpreter's index-range loop body); and a fourth hazard
found only empirically by the stress test below — several ALU handlers (`h_ISUB_R`, `h_IMUL_R`,
`h_IXOR_R`, `h_IROL_R`) materialize a compile-time immediate into a shared physical scratch register
(x20) when the VM instruction's `src==dst`, invisible to a hazard model that only tracks the 8
VM-logical registers. Root-caused via bisection (binary-searching a global swap-count budget down to
one exact failing swap across 8 chained programs) and fixed by excluding any `src==dst` instruction
from a swap's two moving positions.

**Verification**: a new `tests/test_jit_scheduler_stress.cpp` — 450 (seed, input) JIT-vs-interpreter
differential pairs across 3 separate Argon2 caches, deliberately larger than the standard 16-seed
`test_jit_equivalence` suite, since the failure mode is a silent wrong hash for rare program shapes
that a small fixed corpus has no particular reason to hit. First run caught the `src==dst` bug
immediately; after the fix, all 450 pairs byte-identical. On-device runtime is genuinely ~2069s
(interpreter-dominated, ~2.6s/hash vs. JIT's ~0.23s/hash) — the initial 1200s `ctest` TIMEOUT budget
was too tight and produced a spurious timeout with zero mismatches reported before being killed, not
a hang; raised to 2400s after measuring the real per-hash cost directly. Full existing suite
(`armrx_tests`, `test_mining`, `test_jit_encodings`, `test_jit_determinism`, `test_jit_equivalence`)
also green on-device.

Performance measurement (deferred at the time this entry was written, prioritizing correctness
first per this project's own standing rule) found this main-program-only version to be a clean
null with a real branch-miss cost — see the entry above this one dated the same day for the full
story and the eventual adopted fix (extending the scheduler to the superscalar path). Kept here
unedited as the accurate record of what was known at this point in the session.

## 2026-07-25 — Dual External Audit Reviewed, Verified, Acted On

User supplied two independent full-codebase audits (`docs/audits/PROJECT_AUDIT_REPORT_Gemini_25072026.md`,
`docs/audits/PROJECT_AUDIT_REPORT_Hermes_25072026.md`). Per this project's standing discipline (verify
external claims before trusting them — see the LTO/fortify-headers misattribution and the
31%-branch-miss myth from earlier sessions), every concrete claim was checked against the
actual source before any fix landed, rather than implemented on either report's say-so.

**Fixed, all confirmed real by direct code inspection:**
- `vm.cpp` `dataset_read()`: `ARMRX_ASSERT`'s documented release-build semantics (log-and-
  continue) meant its OOB bounds check was a no-op in the exact build mode that ships, with
  the unchecked read executing immediately after. Verified the address math
  (`dataset_offset_ + (ma_ & 0x7fffffc0)`) is spec-guaranteed in-bounds today — latent, not
  currently exploitable — but added a targeted early-return at this one call site as
  defense-in-depth, without changing `ARMRX_ASSERT`'s general (intentional) semantics
  elsewhere.
- `mining_engine.cpp` `AffinityMode::BigOnly`: hardcoded `thread_id % 4`, ignoring
  `core_order_` (the already-detected, frequency-sorted topology) one branch away. New
  `count_top_frequency_cores()` derives the real big-cluster size from the same
  `cpuinfo_max_freq` data; `BigOnly` now pins to `core_order_[thread_id % big_core_count_]`.
- `worker_hashes_`: confirmed densely-packed `std::atomic<uint64_t>[]`, up to 8 workers per
  64-byte cache line. New `alignas(64)` `PaddedCounter` wrapper fixes it for the whole array,
  not just the first element (alignas on a struct pads `sizeof` up to the alignment too).
- `jit_compiler_a64.cpp` `h_IMUL_M`: stale `// sub` comment next to an `ARMV8A::MUL` emit
  (copy-paste artifact, no functional bug, real maintenance trap). Comment corrected.

**Checked and refuted, saved from being acted on blindly:**
- Gemini's FPCR-leakage claim contradicted Hermes's own assessment of the identical code.
  Read `randomx_calculate_hash()` directly — straight-line, no early-return/exception path
  between `fegetenv`/`fesetenv` exists. Hermes was right.
- Gemini's "missing `isb` after `__builtin___clear_cache`" — GCC/Clang's AArch64
  implementation of that builtin already emits the full `dc cvau`/`ic ivau`/`dsb ish`/`isb`
  sequence; that's the entire point of using the builtin over hand-rolled asm. Very likely a
  false positive.
- Gemini's `munmap`-vs-`freePagedMemory` mismatch is real as an abstraction nit but
  functionally identical on Linux (`freePagedMemory` is a literal null-checked `munmap`
  there), and this project only targets Linux/AArch64. Correctly low priority, not fixed.

**Deferred, needs measurement not blind adoption** (per this phase's own protocol):
`-mtune=cortex-a53` default, interpreted-path prefetch, SIGSEGV/SIGBUS JIT-fault handler,
RWX-JIT reconsideration (already a known Phase-4 tradeoff, not a miss), windowed hash-rate
reporting, oversubscription warnings, BOLT.

Verified: local x86 full suite 7/7, on-device targeted JIT/correctness suite 5/5, both 100%.
Build warning count unchanged from baseline (56). See `PLAN.md` Phase 6 item 17.

## 2026-07-25 — Item 14 Follow-up: `IMUL_RCP` Literal-Load Elimination Tried, Measured, Reverted

Direct follow-on from the correlation script's finding (below): `IMUL_R`/`IMUL_RCP` are the
two most expensive superscalar opcodes by cycles. `IMUL_RCP`'s reciprocal is known at
JIT-compile time, so tried replacing its literal-pool `LDR` + `MUL` (2 instructions, 8-byte
literal + 4-byte load) with direct 64-bit immediate materialization: new
`emitMovImmediate64()` (`MOVZ`/`MOVN` for the first non-skippable 16-bit chunk, `MOVK` for up
to 3 more) + `MUL`, and removed the now-dead literal-pool mechanism for this opcode (nothing
else populated or read it).

**Correctness verification took a detour.** Wanted ASan as the strongest safety net given
the up-to-5-instruction worst case (vs. the previous 2) inside a fixed-size buffer. First
build attempt found `CalcDatasetItemSize`'s buffer isn't actually organized into fixed
per-instruction "slots" as initially assumed — `codePos` is a plain sequential bump allocator
across the whole region, so the real constraint is aggregate size, not a per-occurrence
boundary, which is more permissive than first feared. Then hit two infrastructure problems
getting ASan to actually build: (1) a real, pre-existing `CMakeLists.txt` bug — `ARMRX_ENABLE_ASAN`'s
`target_compile_options`/`target_link_options` were `PRIVATE` on the `armrx_core` static
library, which never propagates to the executables that link against it and need the
sanitizer runtime too; fixed to `PUBLIC` (kept, independent of this item's outcome, plus the
same fix applied to UBSAN/TSAN for consistency); (2) after that fix, discovered this device's
Alpine/musl toolchain doesn't ship `libasan` at all (`cannot find -lasan`, confirmed via
`find / -name libasan*` turning up nothing) — a hard toolchain limitation, not fixable here.
Fell back to this project's other proven-sufficient method: the full KAT/`test_jit_determinism`/
`test_jit_equivalence`/`test_jit_encodings` suite, 5/5 green on the new code.

**Measured via three properly-controlled `perf stat` trials**, old and new binaries preserved
side-by-side and rebuilt fresh, `timeout`-wrapped to a genuinely fixed wall-clock window (a
first attempt without the external `timeout` wrapper produced mismatched elapsed times
between runs — 67.8s vs 74.2s — and had to be discarded as not properly controlled), including
one trial with reversed run order to rule out thermal drift. Result, consistent across all
three: **IPC improved substantially as hypothesized (~0.729→~0.789, +8.2%)** — the literal-pool
load really was costing stall cycles — but **instruction count rose ~8.5-8.6%**, and for
matched completed work (identical hash counts across paired trials), **total cycles needed
rose ~0.25-0.34%**. A small, consistent, real regression — the stall-elimination benefit
almost exactly cancelled by the extra instructions' own cost, the same "cost relocated, not
eliminated" pattern as item 9's superscalar literal-pool relayout.

**Reverted** (`git checkout -- src/jit_compiler_a64.cpp include/armrx/jit_compiler_a64.hpp`),
KATs/JIT-determinism/equivalence/encodings re-confirmed 5/5 on the reverted build. The
`CMakeLists.txt` ASan-propagation fix was kept. Diagnostic value survives the revert:
`IMUL_R`/`IMUL_RCP`'s cost is confirmed genuinely latency-bound on this in-order core, not a
memory-access artifact — a future attempt would need to *hide* the latency via scheduling
(item L1, the emitter lookahead scheduler) rather than trade it for more instructions. See
`PLAN.md` Phase 6 item 14 for the full account.

## 2026-07-25 — Item 14: `tools/jit_correlate.py` — Real Opcode-Level Cycle Attribution Inside the JIT Buffer

Continuation of the `perf record` work below: built the actual correlation script the
previous entry identified as the concrete next step. `dumpJitCode()`
(`src/jit_compiler_a64.cpp`) extended to print (1) a per-entry superscalar boundary table
matching the main program's existing format (previously only an aggregate-by-opcode table
existed for this region) and (2) the buffer's true allocated size (`CodeSize +
CalcDatasetItemSize`) plus `CodeSize` alone, giving ground truth for matching against
`/proc/<pid>/maps`.

New `tools/jit_correlate.py`: parses `--jit-dump` output, a `/proc/<pid>/maps` snapshot, and
`perf script -F ip` output; for each sample address, finds which worker's JIT buffer it falls
in (matched by size, rounded to the page boundary mmap/mprotect actually use) and looks up
the relative offset in the superscalar opcode table.

**Two real correctness issues found and fixed while building this, both would have silently
produced wrong numbers**:
- THP (`always` policy on this device) directly observed merging adjacent worker JIT buffers
  into single VMAs of 1x, 2x, and 4x the per-buffer size, all present in the same snapshot.
  Treating a merged region as one buffer would have computed the wrong relative offset for
  every worker but the first sharing that region. Fixed: any executable region whose size is
  a whole multiple of the per-buffer size is split into that many equal sub-regions before
  matching.
- `perf script -F ip` still emits the full call chain per sample (leaf frame first, then
  unwound callers) as a blank-line-separated block, not one address per line, since the
  recording used `-g`. Counting every frame as an independent sample would have inflated the
  total and skewed the distribution toward whatever functions happen to appear deep in call
  chains. Fixed: only the first line of each block is a real sample; the rest are ancestor
  frames, discarded.

Also hit (unrelated to the script itself): `pgrep -f './armrx --mine'` matched the
*invoking shell's own* command line, not the real `armrx` process, since the whole capture
sequence was passed to `sh -c '...'` as one string and therefore contains that substring
itself. Fixed by matching `/proc/<pid>/comm` exactly instead, which is invocation-independent.

**Result**: 473,783 samples processed. **14.25%** fell outside any worker JIT buffer —
matching the prior `perf report` pass's ~14-15% named-C++ estimate almost exactly, a solid
cross-check between two independently-built methods. Of the 85.75% inside a JIT buffer,
**73.96% (63.42% of all samples) matched a specific superscalar opcode** via the boundary
table. Per-opcode breakdown of *cycles*, not just instruction count: **`IMUL_R` (20.98% of
all samples) and `IMUL_RCP` (14.30%) together account for over 35% of every cycle spent
mining** — by a wide margin the two most expensive superscalar opcodes, consistent with
integer multiply's longer pipeline latency on an in-order Cortex-A53 and this project's
established "memory/latency-stall-bound" framing (not something to "optimize away" without
changing RandomX semantics — this is diagnostic, not a TODO). The remaining ~22% of total
samples are inside a worker buffer but outside any superscalar entry (the per-hash VM
program/fixed-wrapper region, regenerated every hash, not attributable per-opcode this way).

Verified: build clean on-device (43 warnings, matching the established baseline), all 3
JIT-relevant tests green (`test_jit_encodings`/`test_jit_determinism`/`test_jit_equivalence`).
`dumpJitCode()`'s changes only print additional metadata about already-emitted bytes; no
codegen changed. New tool is fully self-contained (armrx's own `--jit-dump`/`perf`/`/proc`
only) — no external miner's code or binaries involved, per the clean-room boundary. See
`PLAN.md` Phase 6 item 14 for the full account.

## 2026-07-24 — Item 14: Live `perf record` Self-Profiling Corrects the "Missing Instructions Are In C++" Hypothesis

Continuation of item 14's instruction-count reconciliation, this time with a live system
profiler instead of more static counting. On-device: `perf record -F 999 -g -e cycles` on
the real `./armrx --mine --workers=8` binary (not `bench_armrx`), 20s warmup + 60s
steady-state, 486K samples. All 8 workers confirmed contributing (worker[0-3] 3.20 H/s,
worker[4-7] 1.60 H/s — matches item 3's ~2:1 cluster-arbitration split almost exactly, a
good sanity check that this run was representative). This is the first live-profiler capture
of the mining hot path in this project; everything before was static (`--jit-dump`, `.S`
source counting).

**Two hiccups along the way, both benign**: (1) an earlier `devbox_build` call's remote
process kept running after the MCP client reported "Connection closed" and disconnected;
the next `devbox_build` invocation raced against it on the same output files, corrupting 4-5
linked binaries to 0 bytes. Fixed by killing the orphaned process tree and forcing a clean
relink (`rm` the corrupted stubs, rebuild) — full 12/12 on-device test pass confirmed
afterward. (2) The first `perf record` attempt used too short a warmup (5s) against a ~14s
cache-init cost, so cluster 1's workers (4-7) never got a single post-warmup hash in and
reported 0.00 H/s; redone with 20s warmup, all 8 workers contributed correctly.

**Finding**: only ~14-15% of self-time samples resolve to a named C++ symbol at all
(`randomx_calculate_hash` ~12%, `permute_16_neon`/`Argon2dCache::initialize` ~1-2%
combined). The remaining ~85% is unattributed — ~44% as thousands of distinct raw hex
addresses, ~41% not resolving a leaf frame at all (consistent with `-O3` frame-pointer
omission and zero unwind info in a raw JIT buffer). **Verified, not inferred**: dumped
`/proc/<pid>/maps` during a live run and found several ~150-370 KiB `rwxp` anonymous
mappings (one per worker) in the exact same address range as the unresolved samples —
proof these are genuinely inside the runtime-JIT-compiled code, not a profiling artifact.

**This corrects item 13's closing hypothesis** ("the remainder almost certainly lives in the
C++ side") rather than confirming it — `ROADMAP.md`'s own region breakdown already measured
AES scratchpad and Blake2b at 0.3%/0.0% of hash time, too small to hide ~60M instructions/
hash, and this session's ~14-15% named-C++ total corroborates that independently. The
missing instructions are still inside JIT-generated code, just not the three regions item
13/14 already counted statically (variable superscalar opcodes, fixed per-call wrappers,
main VM program's fixed per-iteration overhead). Next, still self-directed: correlate perf's
raw sample addresses against item 13's `JitDumpEntry` offset tables for real opcode-level
attribution inside the JIT buffer — a small script, not yet built. See `PLAN.md` Phase 6
item 14 for the full account.

## 2026-07-24 — Audit Pass: Clean-Room Boundary Doc Stragglers + Range-Validated Numeric Parsing + JIT Review Fixes

Follow-up audit after the clean-room boundary decision (entry below) plus a three-track
fresh-eyes review (JIT encodings, Stratum/pool parsing, config consumption path).

**Doc consistency — six stragglers still carried the old XMRig-internals framing, all fixed:**
- `docs/plans/peephole-jit-plan.md` — superseded-banner added at top (its whole methodology is
  XMRig disassembly; body kept as historical record).
- `docs/plans/performance-master-plan-20260724.md` (L2 row), `docs/plans/performance-master-plan.md`
  (L1 row), `docs/audits/performance-improvement-audit.md`, `docs/plans/performance-next-agent-handoff.md`
  (item 11 + Stage 4 step 4), `HANDOFF_CLAUDE.md` — reworded to the boundary + item 14's
  self-directed framing.
- `CLAUDE.md` — stale "known gap" claims removed (config.cpp guards and
  `MetricsExporter::server_fd_` atomic were already fixed and marked done in `NEXT_STEPS.md`).

**Range-validated numeric parsing (`src/config.cpp`, `src/cli_parser.cpp`):** the existing
try/catch guards couldn't catch two classes of bad input: `std::stoul("-1")` silently wraps
to 2^64−1 (`workers: -1` → attempt to spawn ~4B threads), and huge-but-valid values silently
truncate through narrowing casts (`port 65539` → port 3). Added a shared
`parse_bounded_ull()` helper (rejects leading `-`, enforces per-field maxima: ports ≤65535,
workers ≤4096, stagger-ms ≤60000) at all 10 numeric parse sites in both files. Verified
on-device: `--workers=-1` and `--metrics-port=65539` now exit 64 with a clear message.

**JIT review fixes (`src/jit_compiler_a64.cpp`):** (1) corrected the stale buffer-layout
comment claiming `.fill` reserves 16 words/instruction (6144 slots) — static.S actually
reserves 32 (12288), and the fast div/sqrt path emits ~20 words, so "correcting" the .fill
to the comment would have overflowed; (2) `h_IMUL_RCP`'s LDR-literal offset now masks to
imm19 like the superscalar path already did (defensive parity; safe under current layout).
Review otherwise clean: 0 encoding errors, 0 W^X issues, 0 branch off-by-one. Stratum/pool
review: no memory-safety findings (two informational: `hex_to_bytes` silently coerces
malformed hex; no early blob-length sanity check — both safely caught downstream).

**Verified:** x86 build + full ctest 7/7; on-device build + 12/12 (KATs, JIT determinism/
encodings/equivalence green; the 7 "BAD_COMMAND" failures in the first devbox run were the
known CTest binary-path flake — direct re-run passed 5/5).

## 2026-07-24 — Clean-Room Boundary Decision: No XMRig Internals Inspection, Item 14 Reframed

Prompted by a direct question from the user after item 13 landed: is inspecting XMRig's
JIT-generated machine code (as item 14 was originally scoped — `--jit-dump` + objdump,
region-by-region diff against XMRig's own codegen) in tension with this project's identity as
a clean-room implementation built independently against the RandomX spec, not derived from an
existing mining client (`CLAUDE.md`)? Answer: yes. Not illegal, but genuinely at odds with the
project's own stated standard, and worth a permanent line rather than a case-by-case judgment
call each time it comes up again.

**Decision**: black-box behavioral comparison against XMRig — hashrate, `perf stat` counters,
whole-process instruction counts — stays legitimate and the findings already obtained this way
(item 3's two-cluster topology discovery, the ~10-12% cluster-normalized gap, the ~33.5%
instruction/hash gap) are kept as accurate historical record, unchanged. Inspecting XMRig's
*internals* (disassembling its generated code, diffing it against armrx's own) is ruled out
going forward, permanently, not just deprioritized for scope reasons.

**Consequence**: `PLAN.md` Phase 6 item 14 — previously "region-scoped armrx-vs-XMRig
generated-code comparison (`--jit-dump` + objdump)" — reframed to a self-directed
instruction-count reconciliation for the superscalar/dataset-derivation region's remaining
~55% (fixed wrapper chunks, main-loop per-iteration overhead), using only armrx's own code and
first-principles ARM64 reasoning, the same method items 7-10 already used. `NEXT_STEPS.md`'s
matching long-term checklist entry and `ROADMAP.md`'s P3 row updated to match — P3 is gated on
this self-directed reconciliation, not on any XMRig-side comparison. Item 13's changelog entry
below (unchanged, kept as historical record) refers to "item 14 (the actual region-scoped
armrx-vs-XMRig comparison...)" — that forward-reference is superseded by this entry; item 13's
own findings are unaffected. See `PLAN.md`'s clean-room boundary note (added after item 13,
before the reframed item 14) for the full reasoning.

## 2026-07-24 — Phase 6 Item 13: Real Instrumentation for the Superscalar/Dataset-Derivation Path

`--jit-dump` only ever covered the fixed, 2047-instruction main VM program (executed once per
hash). Measured instructions/hash is ~132.93M (item 3's XMRig comparison), so the dominant
instruction volume had no tooling at all. Built it.

**Confirmed the mechanism by reading the assembly first**, not assuming it: `generateSuperscalarHash()`
is called from `VirtualMachine::set_cache()` (`vm.cpp:175`) — once per seed rotation, not once
per hash — and JIT-compiles the dataset-item-derivation code once. That compiled code then
*executes* (no recompilation) via `bl rx_calc_dataset_item` inside
`randomx_program_aarch64_vm_instructions_end_light` (`jit_compiler_a64_static.S:557`), reached
from every iteration of the main VM loop in light mode: `RANDOMX_PROGRAM_ITERATIONS` (2048) × 8
chained programs = **16,384 calls per hash**.

**Built the instrumentation**: extended the existing `JitDumpEntry`/`--jit-dump` mechanism
(same idea already used for the main program's opcode boundary table) to cover
`generateSuperscalarHash()`'s emitted code — new `superscalar_jit_dump_` member vector and
`getSuperscalarJitDump()` accessor (`jit_compiler_a64.hpp`), instrumented the per-instruction
emission loop inside `generateSuperscalarHash()` (`jit_compiler_a64.cpp`), extended
`dumpJitCode()` to print a per-opcode aggregate table for this region. One real bug caught
during the build: my first draft named the new aggregate totals `total_instr`/`total_bytes`,
shadowing existing locals of the same name earlier in `dumpJitCode()` (`-Wshadow` caught it
immediately — renamed to `ss_total_instr`/`ss_total_bytes`). Verified 12/12 on-device (KATs +
`test_jit_determinism`/`test_jit_equivalence`/`test_jit_encodings`) both before and after that
fix — the instrumentation only records metadata about already-emitted bytes and shouldn't
change codegen, but this touches the JIT compiler directly so it got full correctness
verification regardless.

**Real data, from `./armrx --jit-dump`**: one `generateSuperscalarHash()` call (= one
`rx_calc_dataset_item` compile) emits **3,563 instructions, 20,916 bytes** of variable
superscalar-opcode code. `IMUL_R` dominates at 14.82% of bytes; the `_C7`/`_C8`/`_C9`
immediate-constant variants of `IADD`/`IXOR` average 12 bytes/instruction (the same
`emitMovImmediate`+`EOR` / `emitAddImmediate` sequences already examined for the main program);
everything else is a lean 4 bytes/instruction. Scaled: 3,563 × 16,384 calls ≈ **58.4M
instructions/hash — about 44% of the ~132.93M total** from this region alone.

**Honest scope**: 58.4M is a *lower bound*. It covers only the variable superscalar-opcode
portion the new instrumentation tracks — not the fixed prefetch/mix/store-result wrapper chunks
copied around each of the 8 cache-access rounds inside one call, and not the main VM loop's own
fixed per-iteration wrapper code (interleaved FP/int loads, `FE_mix` AES tweak, the
`xor_with_dataset_line` step itself, `spMix` update, prefetch, store) that runs on all 16,384
iterations regardless of the small variable VM-instruction region. Those remain uninstrumented.
This closes the *tooling* gap item 13 existed to fill, and gives the first real, code-confirmed
number for the dominant region — but item 14 (the actual region-scoped armrx-vs-XMRig
comparison, and reconciling the remaining ~55%) is still open. Full account in `PLAN.md` Phase 6
item 13.

## 2026-07-24 — Phase 6 Item 10 Correction: Adopted After All — Wall-Clock Hashrate Wasn't Sensitive Enough

The "null result" conclusion below (same day, earlier entry) turned out to be wrong, caught by
the user questioning whether 2 non-interleaved passes were really enough to call it noise. Three
rounds of measurement, escalating in rigor:

1. **Round 1** (below): 2 passes/condition, non-interleaved (all no-prefetch runs first, then a
   rebuild, then all with-prefetch runs). +0.44% difference, read as noise. **Flaw pointed out**:
   the two conditions were never interleaved, so any time-based drift over that ~10+ minute span
   (thermal, anything) could manufacture the appearance of a difference independent of the code.
2. **Round 2**: built both binaries once (`armrx_with_prefetch`, `armrx_no_prefetch`), ran an
   interleaved A/B/A/B/A/B sequence (3 passes/condition) so drift would land on both conditions
   equally. Result: with-prefetch mean 1498.67 vs. no-prefetch mean 1504.67 (+0.40%, same
   direction/magnitude as round 1 — reassuring against pure drift), but the individual samples
   now *overlapped* (one with-prefetch run beat one no-prefetch run) — a two-sample t-test landed
   at only p≈0.07. Genuinely inconclusive with wall-clock hashrate at this sample size.
3. **Round 3**: switched to a lower-noise metric — `perf stat` cycles/instructions — instead of
   just adding more wall-clock passes. Interleaved, 2 samples/condition, 20s each: instructions
   completed in the fixed window averaged 66,224,771,994 (with-prefetch) vs. 66,810,942,939
   (no-prefetch) — **+0.885%, zero overlap** between the two with-prefetch samples
   (66,267,402,630 / 66,182,141,357) and the two no-prefetch samples (66,798,047,203 /
   66,823,838,674), with within-condition spread of only 0.04-0.13% — a 7-20× signal-to-noise
   ratio. Cycles agreed (+0.497%), and IPC was marginally *higher* without the hints (0.7325 vs.
   0.7296) — not a stall-hiding tradeoff, cleanly better on every axis measured.

**Adopted.** The three `prfm` lines in `.Lmain_loop` (`jit_compiler_a64_static.S:381-383`) are
now permanently commented out, with a dated explanation in the source pointing here. Verified
12/12 on-device (KATs + `test_jit_determinism`/`test_jit_equivalence`/`test_jit_encodings`) on
the exact adopted build. Scratch comparison binaries removed from the device.

**The methodology lesson, worth keeping**: this device's wall-clock hashrate noise (~0.2-0.5%
pass-to-pass) is large enough to swamp real effects of similar or smaller magnitude — round 1's
2-pass "null" read wasn't obviously wrong on its face, and round 2 adding *more of the same*
measurement only got to "suggestive." What actually resolved it was switching to a fundamentally
more sensitive measurement (`perf stat` instruction/cycle counts), not more samples of the same
kind. For any future single-digit-percent-or-smaller effect on this device, `perf stat` should be
the first tool reached for, not a fallback after wall-clock measurement is already ambiguous.

## 2026-07-24 — Phase 6 Item 10: Prefetch A/B Matrix, First Variant — Null Result

**Superseded by the correction above, same day — kept for the historical record of how the first
(wrong) conclusion was reached, and why.**

Confirmed the target first: `jit_compiler_a64_static.S:381-383`'s three `prfm` hints
(`pldl1keep`/`pldl1strm`/`pldl1keep+32`) sit inside `.Lmain_loop`, the main VM execution loop —
executed up to 16,384 times per hash (2048 iterations × 8 chained programs per hash), genuinely
hot, and distinct from the already-tuned `O10` dataset-item prefetch from an earlier phase (a
different, single `prfm` in `rx_calc_dataset_item_prefetch`).

Tested the cheapest, most informative variant first — removing all three hints entirely — before
trying finer-grained combinations. Commented out the three `prfm` lines, verified correctness on
device (12/12: KATs, `test_jit_determinism`, `test_jit_equivalence`, `test_jit_encodings`), then
measured 8-worker aggregate hashrate apples-to-apples (old code rebuilt fresh via `git stash`,
two passes each, thermal-settled, same methodology as item 2's original sweep):

- With prefetch (original): 1495, 1498 hashes/60s → avg **24.94 H/s**
- No prefetch: 1501, 1505 hashes/60s → avg **25.05 H/s**

A +0.44% difference — within this device's own established pass-to-pass noise (~0.2-0.3%,
visible in both conditions' internal spread). **Confirmed null, not a real win or loss.** This
specific hot loop's throughput doesn't hinge on these particular prefetch hints. Restored the
original code (`git stash drop`, no residual change) rather than adopt a no-op change. The
remaining matrix cells (single-hint variants, `L1STRM`/`KEEP` swaps) weren't tried — deprioritized
now that the all-or-nothing test showed no effect either direction, without a new hypothesis for
why a specific hint *combination* would matter when presence-vs-absence already didn't.

## 2026-07-24 — Phase 6 Item 11: Fused Hash-and-Fill Closed — Failed Its Own Benchmark Gate

Following the design notes' explicit prerequisite ("bench first, integrate only on a measured
win"), added a direct primitive-level benchmark to `tests/bench_armrx.cpp`'s
`bench_aes_primitives()` before touching any mining-engine code: `hash_and_fill_aes_1r_x4` (one
fused 2 MiB scratchpad traversal) against `hash_aes_1r_x4` + `fill_aes_1r_x4` run back-to-back
(two separate 2 MiB traversals — exactly what `randomx_calculate_hash()` does today at a nonce
boundary, via `get_final_result()` for nonce N followed by `init_scratchpad()` for nonce N+1 on
the same reused scratchpad buffer).

**Result (30 samples each, on-device, thermal at true idle 36-39°C beforehand)**:
- Separate (`hash_aes_1r_x4` + `fill_aes_1r_x4`): 57,084.78 μs
- Fused (`hash_and_fill_aes_1r_x4`): **59,124.84 μs — ~3.6% *slower***, not faster.

**Closed, no `mining_engine.cpp`/`vm.cpp` changes made.** The nonce-pipelining integration this
item called for (worker-loop restructuring into a first/next/last state machine, job-change/
shutdown flush logic, a new deterministic-nonce equivalence test) was never attempted — its
premise failed at the cheap, low-risk primitive-benchmark stage, before any of that real
implementation risk would have been incurred. Consistent with this session's other findings on
this in-order Cortex-A53: the fused loop keeps both hash-accumulation and fill-generation state
live simultaneously every 64-byte stride (more register pressure, more interleaved instruction
types per iteration), while the separate version gets two tight, specialized, uniform passes —
touching each scratchpad byte once didn't beat touching it twice with simpler per-pass
structure. The benchmark code is kept in `bench_armrx.cpp` as reusable reference, same
treatment as the NEON-AES experiment. Full account in `PLAN.md` Phase 6 item 11.

**Side note, resolved (2026-07-24, later the same day)**: the same benchmark run's separate
`--full-hash-only` pass showed 2.24 H/s — about half the ~4.27 H/s baseline. User hypothesis:
caused by the test landing on cores 4-7 (cluster 1). Verified directly:
- Pinned `bench_armrx --full-hash-only` via `taskset -c 0`: 4.48 H/s (baseline). Via
  `taskset -c 4`: 2.24 H/s (reproduced exactly). So it genuinely is core/cluster-specific.
- But first had to rule out a bigger methodological risk: `MiningEngine::worker_loop()` calls
  `pthread_setaffinity_np()` internally (`mining_engine.cpp:282-297`, driven by `hwloc`-detected
  `core_order_`), which could in principle silently override an external `taskset` on the whole
  `armrx` process. Verified via `/proc/<pid>/task/*/stat`'s processor field that `taskset -c 4`
  genuinely pins the real worker thread to physical CPU 4 (hwloc respects the inherited cpuset;
  no silent override).
- With that confirmed, ran `armrx --mine`, genuinely pinned to core 4, sustained 90s (60s
  steady-state): **4.27 H/s — matching baseline, not `bench_armrx`'s 2.24 H/s on the same
  core.** Both call the identical `randomx_calculate_hash()` with identical
  `kRandOMXFlagJit | kRandOMXFlagHardAes` flags (confirmed by reading both call sites); both
  `Argon2dCache` and `VirtualMachine` scratchpad allocation go through the identical
  huge-page/THP code path regardless of caller (confirmed by reading `argon2.cpp:262-277` /
  `vm.cpp:128-163`) — so it isn't a flags or memory-allocation difference either.
- **Conclusion**: the 2.24 H/s figure is real and reproducible, but it's an artifact isolated to
  `bench_armrx.cpp`'s standalone test harness specifically on core 4/cluster 1 — not a
  characteristic of real mining, which gets full speed on the identical, verifiably-pinned
  core. This means today's earlier per-core/per-cluster findings (uniform isolated performance
  across all 8 cores; identical cycles/instructions between clusters in isolation) — which all
  used the real `armrx --mine` path — **stand as correctly measured, not invalidated**. The
  narrower question of why `bench_armrx`'s loop specifically underperforms on cluster 1 (likely
  a compilation/optimization difference between the two separately-built binaries, unconfirmed)
  is left as a documented, low-priority oddity in the benchmark tool, not chased further.

## 2026-07-24 — SoC Identity Corrected (MSM8929/Snapdragon 415, Not MSM8916/410); New Phase 6 Item 13

User-supplied correction from the postmarketOS wiki: the device is **MSM8929 / Snapdragon 415**,
a genuine big.LITTLE-shaped octa-core — **4× Cortex-A53 @ 1.1 GHz + 4× Cortex-A53 @ 1.4 GHz** —
not "Lenovo MSM8916 / Snapdragon 410" as every doc in this project has said since its earliest
sessions (MSM8916 is a quad-core part). This directly explains the two-cluster L2 topology found
earlier the same day (item 3's "REVISED" section) rather than leaving it as an unexplained
oddity. Updated `README.md`, `ROADMAP.md`, `NEXT_STEPS.md`, `PLAN.md` wherever the old ID or its
"quad+quad, different/rebranded name" speculation appeared.

**One honesty-driven caveat added while correcting this**: the earlier isolated-cluster PMU test
(pinning 4 workers exclusively to each cluster, found identical cycles/instructions between them,
"no inherent clock difference") measured an effective ~772 MHz for *both* clusters — below
*both* rated maxes (1.1/1.4 GHz). That test still correctly shows the two clusters equal *to each
other* under those specific isolated conditions, so the "dynamic arbitration, not a static
frequency difference" conclusion for that comparison stands — but neither cluster reached its
official ceiling in that test, so the real 1.1/1.4 GHz asymmetry hasn't been separately isolated
from the arbitration effect under full 8-worker contention. Flagged as an open nuance, not
silently smoothed over.

**Added `PLAN.md` Phase 6 item 13** (renumbering old 13→14, 14→15, 15→16): "Build real
instrumentation for the superscalar/dataset-derivation path" — the missing prerequisite the
2026-07-24 region-scoped `--jit-dump` attempt exposed (it only covers the fixed 2047-instruction
main VM program, not the ~132.9M-instruction/hash dataset-derivation path that actually
dominates). Item 14 (region-scoped XMRig comparison) is now explicitly gated on item 13.

## 2026-07-24 — Major Finding: Two 4-Core L2 Clusters, Not One 8-Core Cluster; Retracts Item 3's "Power Cap" Hypothesis

Triggered by a direct question about why armrx wasn't matching XMRig's hashrate. Got a real
head-to-head data point (XMRig's own dev build, already compiled on-device, run against the
same pool/job) and followed the discrepancy all the way to a genuine, previously-undocumented
hardware fact about this device — the single most significant finding of the whole session,
found by chasing a "why" question rather than another planned checklist item.

- **XMRig's own per-core table on this device** (`slow`/light mode, same as armrx's light mode
  — XMRig hit the identical "not enough memory for dataset" fallback) showed a stark, non-
  uniform split: cores 0-3 at 4.6-4.7 H/s each, cores 4-7 at only 2.3-2.5 H/s each, aggregate
  28.28 H/s. Nobody — not this session, not five prior sessions of profiling, not either master
  plan — had ever looked at *per-core* granularity before; all prior measurement was
  aggregate-only, which hides this completely.
- **Ruled out "cores 4-7 are just weaker" first**: 8 isolated sequential single-core armrx runs
  (`taskset -c N --workers=1`, zero contention) showed all 8 cores identical — 67-68 hashes/15s
  each. No asymmetry in isolation.
- **Confirmed the split is real in armrx too, but only found it after catching a measurement
  artifact of its own**: `run_local_benchmark()` already prints real per-worker steady-state
  H/s (`worker[i]: X H/s`) — missed in every earlier command this session because the `grep`
  filters used to extract "Total Hashes computed" happened to strip those lines too. First
  attempts at a 25-40s steady-state window showed an exact 2:1 split, but the *absolute* hash
  counts were suspiciously identical between two differently-sized windows (128 vs. 64 hashes,
  both times) — a red flag, since per-worker counters are flushed in batches of 64
  (`mining_engine.cpp`, ~14s/batch at ~4.5 H/s), so a 25-40s window only captures 1-2 batches
  and is dominated by phase-alignment noise, not a real rate. **Re-ran with a 300s run / 60s
  warmup (240s steady-state window, ~15-16 batches/worker)** to average that noise out: the
  split held — worker[0-3] at 4.0-4.26 H/s, worker[4-7] a rock-steady 2.13 H/s. Real, not an
  artifact.
- **Confirmed the physical core mapping** via `/proc/<pid>/task/*/stat`'s processor field:
  armrx's worker[i] pins directly to physical core i (no reordering) — worker[0-3] = cores 0-3,
  worker[4-7] = cores 4-7, an *exact* match to XMRig's own core-ID split. Two independently-
  implemented miners agreeing on the identical core-group boundary is strong cross-validation
  this is a real device characteristic, not a bug in either one.
- **Found the root cause directly in kernel sysfs**:
  `/sys/devices/system/cpu/cpu0/cache/index2/shared_cpu_list` = `0-3`, and
  `cpu4/cache/index2/shared_cpu_list` = `4-7`. **This device has two separate 4-core L2 cache
  domains**, contradicting `lscpu`'s own "Cluster(s): 1, Core(s) per cluster: 8" self-report.
  The "Lenovo MSM8916 / Snapdragon 410" hardware ID this project has used since its earliest
  docs may itself be wrong or incomplete — MSM8916 is historically a quad-core part; this
  device's real SoC most likely has a "quad+quad" octa-core A53 design under a different or
  rebranded identifier. Not re-identified yet, flagged in `ROADMAP.md` so nobody derives
  conclusions from the wrong chip's public specs.
- **Ruled out a static per-cluster frequency/cache difference directly**: pinned 4 workers
  exclusively to cluster 0 (`taskset -c 0-3`) and, separately, 4 workers exclusively to cluster
  1 (`taskset -c 4-7`), each running *alone* with zero cross-cluster contention, same PMU events
  as the earlier item-3 sweep. Result: virtually identical — 46.394B vs. 46.398B cycles,
  33.535B vs. 33.540B instructions, 103.89M vs. 103.80M L2 refills (<0.01% apart). **No inherent
  clock or cache difference between the clusters.** Both are equally fast when either one has
  exclusive access to the shared downstream memory path.
- **Conclusion**: the 2:1 split is a **dynamic interconnect-arbitration effect, only appearing
  when both clusters compete for the shared memory path simultaneously** — not a static
  hardware asymmetry, not thermal throttling, not a power cap. Cluster 0 wins that arbitration
  under contention; cluster 1 loses roughly half its throughput.
- **This retracts item 3's original "core-count-triggered power/current cap" hypothesis**
  (2026-07-24, earlier the same day) — the apparent per-core clock drop measured at 6-8 workers
  (~772→641→567 MHz via cycles/wall-time) was simply the *average* of a full-rate cluster and
  an arbitration-losing (not slower-clocked) cluster once the worker count started spanning
  both, not a real dynamic frequency change.
- **Fully explains item 2's worker-sweep efficiency curve mechanistically, not just
  empirically**: workers 1-4 map to cluster 0 alone (confirmed mapping) — zero cross-cluster
  contention, hence 98.5% efficiency at 4 workers, essentially the isolated peak. Worker 5 is
  the *first* to land on cluster 1 and immediately eats the arbitration penalty, matching the
  sweep's own single biggest step-down (98.5%→89.2%). Workers 6-8 add the rest of cluster 1 at
  roughly half rate, producing the smooth-looking decline through 8 that item 2 measured.
- **A real, quantified, cluster-normalized answer to "why can't armrx match XMRig"**: comparing
  like-for-like instead of raw aggregates — XMRig cluster-0 sum 18.62 H/s vs. armrx 16.78 H/s
  (armrx at **90.1%** of XMRig); XMRig cluster-1 sum 9.67 H/s vs. armrx 8.52 H/s (armrx at
  **88.1%**). **armrx is consistently ~10-12% behind XMRig on both clusters independently** — a
  real, modest, now-precisely-quantified gap, not the vague "far behind" impression the raw
  aggregate comparison (24.95 vs. 28.28 H/s) gave by conflating a real code-level gap with this
  interconnect effect that hits both miners identically.
- **Not immediately actionable in software**: RandomX's per-hash workload is inherently
  symmetric across workers, so there's no obvious way to "protect" cluster 0 from cluster 1's
  presence without forfeiting cluster 1's real (if reduced) throughput entirely. Documented as a
  hardware characteristic to design around in future analysis, not a bug to fix. Also settles
  that `isolcpus=`/`nohz_full=` (item 4, already blocked on device privileges) were never going
  to touch this even if unblocked — it's an interconnect-hardware fact, not scheduler-visible.

Full evidence chain, all nine steps, in `PLAN.md` Phase 6 item 3's "REVISED" section.

**Follow-up, same day: found where the remaining ~10-12% gap actually comes from.** Attached
the same Cortex-A53 PMU event set used throughout this session directly to XMRig's own live
process (`perf stat -p <pid>`, 8 threads, real pool job, steady state) and compared against
armrx's own 8-worker numbers. Result: **not a stall/scheduling problem** — armrx's IPC (0.731)
and `ld_dep_stall`% (11.41%) are both *better* than XMRig's (0.612, 16.70%). It's an
**instruction-count gap**: derived from measured hashrates, armrx needs ~132.93M instructions
per hash vs. XMRig's ~99.57M — **33.5% more** — which nets out to ~181.96M vs. ~162.59M cycles
per hash, an **11.9% gap** that matches the cluster-normalized ~10-12% throughput gap almost
exactly (good cross-check). This is a real, measured instruction-count gap, not the old
debunked 31%-branch-miss-era estimate — satisfies `PLAN.md` item 14's gate in spirit, though
not yet region-scoped to specific opcodes/pipeline phases (whole-process comparison only).
Recommended next step is bounded (hours): break armrx's own instruction count down by
opcode/phase via `ARMRX_JIT_PROFILE`/`--jit-dump` before committing to anything larger like the
3-6 week peephole-JIT rewrite — narrow the target first, per this project's standing discipline.

**Did that breakdown, same day.** `--jit-dump`'s opcode boundary table (~2047 instructions
across all 8 chained programs; the recorded per-instruction `size` is accurate at compile time
regardless of later JIT-buffer reuse, so aggregating the full dump is statistically valid)
shows the instruction-count gap is not diffuse — two categories dominate: **memory-operand
opcodes (`*_M`) at 37.13% of all code bytes** (each needs a 4.5-7.9-instruction address-compute
preamble: `emitAddImmediate` + AND-mask + `LDR` [+ `SXTL`+`SCVTF` for FP variants] before the
actual operation), and **`CBRANCH` alone at 19.34%** (avg 5 ARM instructions per occurrence).
Together, over 56% of all emitted code bytes. Register-only opcodes are already at the
1-instruction floor — not a lead. Important scope distinction: CBRANCH's *address/threshold-
computation preamble* is a different, not-yet-investigated question from the already-closed
CSEL/branch-encoding investigation (`docs/experiments/branchless-cbranch.md`) — that was about eliminating
the conditional branch itself and measured a regression; this is about instructions *before*
the branch, not the branch encoding. Neither candidate has been attempted. Full table in
`PLAN.md` Phase 6 item 3.

**Correction, same day, before any code was touched: both candidates fell apart on closer
reading, and the breakdown targeted the wrong region.** Attempted to act on the two candidates
above and caught the problem before writing any code, by reading the actual implementation
first (the same discipline that's caught every stale claim this project has found):
- **Wrong region entirely**: the `--jit-dump` table only covers the *fixed* 2047-instruction
  main VM program, executed once per hash. Measured instructions/hash is ~132.93M — meaning
  over 99.998% of real instruction volume isn't in this table at all. It's almost certainly in
  the superscalar/dataset-item-derivation path (`generateSuperscalarHash()`, item 9's target),
  invoked far more often per hash in light mode than the fixed main program runs once.
- **Memory-operand candidate**: re-reading `emitAddImmediate` (`jit_compiler_a64.cpp:586-619`)
  shows it already uses the tightest available encoding (1-2 `ADD`-immediate instructions) for
  the whole scratchpad-masked immediate range. No slack.
- **CBRANCH candidate**: the `TBZ`/`TBNZ` fusion idea assumed a single-bit condition test.
  RandomX's CBRANCH actually tests an 8-bit-wide field (`static_assert(ConditionMask == 0xFF)`,
  `jit_compiler_a64.cpp:1204`). `TBZ`/`TBNZ` only test one bit. Doesn't apply.
- **No code was changed.** This is a documented negative result, same category as items 7/8's
  stale-claim closures and item 9's reverted regression: guessing at instruction-count fixes
  from an aggregate byte-count table, without reading the actual implementation and without
  confirming the table covers the dominant region, doesn't work. Finding the real ~10-12% gap
  would need either new instrumentation for the superscalar path (no `--jit-dump`-equivalent
  exists for it yet) or an actual binary-level comparison against XMRig's generated code — both
  meaningfully bigger than this pass's "hours, not weeks" scope. Full account in `PLAN.md`
  Phase 6 item 3.

## 2026-07-24 — Phase 6 Item 9: Superscalar Literal-Pool Relayout Implemented, Measured, Reverted

Following up on items 7-8's stale-claim closures, item 9's premise checked out as accurate
(`generateSuperscalarHash()` really did emit an inline literal pool + always-taken `B` branch
per superscalar program), so it was implemented, measured honestly apples-to-apples, and
reverted — a real but small regression, the same "cost relocated, not eliminated" shape as
this session's Argon2 `memcpy` copy-elimination attempt.

- **Implementation** (`src/jit_compiler_a64.cpp`, `generateSuperscalarHash()`): replaced the
  per-program inline literal pool + jump-over-branch (`RANDOMX_CACHE_ACCESSES` = 8 branches and
  8 small pools interleaved through the generated code) with a single leading always-taken
  branch and one consolidated pool. The pool is sized by an exact up-front count of `IMUL_RCP`
  instructions across all programs — a plain counting pass over already-generated superscalar
  program data, no heap allocation, consistent with this codebase's existing no-per-hash-
  allocation discipline (`P2.5` in `ROADMAP.md`). Because the pool's address is fixed and known
  before any code is emitted, each `LDR literal` site's imm19 offset is computed directly in a
  single forward pass — no two-pass backpatch buffer needed. Added a real `ARMRX_ASSERT` range
  check on the imm19 offset (the original code had none — silent truncation on overflow, latent
  but never hit given the small per-program pools; closes that dormant gap independent of this
  item's outcome) plus a literal-count consistency assert.
- **Correctness verified first**: full `ctest` 12/12 on-device, including `test_jit_determinism`,
  `test_jit_equivalence`, and `test_jit_encodings` — all pass, confirming the relocated pool and
  patched offsets produce byte-for-byte correct dataset items.
- **Measured apples-to-apples** (old code rebuilt fresh via `git stash`/`git stash pop`, same
  tool, `bench_armrx --full-hash-only` under `perf stat`, back-to-back, plus a direct `--mine`
  hashrate check for corroboration):

  | Metric | Old | New | Δ |
  |---|---|---|---|
  | Instructions | 73,577,742,754 | 73,517,541,488 | −0.08% |
  | Cycles | 96,639,682,181 | 98,238,320,919 | **+1.65%** |
  | IPC | 0.761 | 0.748 | worse |
  | Branches | 501,454,300 | 440,776,933 | **−12.1%** (mechanism worked as designed) |
  | Branch misses | 14,307,084 | 14,281,956 | −0.18% (flat) |
  | Hashrate (`--mine --seconds=60 --workers=1`) | 268 hashes | 265 hashes | **−1.12%** |

- **Outcome**: branches dropped exactly as the mechanism intended, but cycles and hashrate both
  got measurably worse, corroborating each other. Likely explanation: moving every program's
  literal loads to one distant, shared pool at the front of the function trades better I-fetch/
  branch-prediction behavior for worse D-cache locality on the `LDR`-literal accesses themselves
  — in the original design, each program's own literal was always just a few bytes from its own
  `LDR` reference (hot, recently-touched cache line); in the relayout, later programs' loads
  reach back across the entire preceding programs' generated code. On this memory-latency-
  stall-bound core (Phase 6's central IPC-0.7 finding), that cost more than the removed branches
  saved. **Reverted** (`git checkout -- src/jit_compiler_a64.cpp`), device rebuilt and
  `ctest` re-confirmed 12/12 on the restored code. No further superscalar literal-pool work
  planned without a new mechanism or hypothesis. Full account in `PLAN.md` Phase 6 item 9.

## 2026-07-24 — Phase 6 Items 7-8: Stale Claims, Already Implemented (Verify-Before-Trusting, Again)

Before implementing the Hermes plan's medium-term JIT-emitter items (7-9), read the actual
current code at each cited location first — the same discipline that caught the stale
31%-branch-miss figure, the Newton-Raphson `ROADMAP.md` mis-citation, and the already-tested
`--stagger-ms` claim in earlier sessions. Two of the three turned out to already be implemented:

- **Item 7 (register-offset FP loads): stale, closed, no code change.** The plan described
  `JitCompilerA64::emitMemLoadFP()` as emitting `add x19,x2,x19` + `ld1 {vN.2s},[x19]`. Reading
  the actual function (`src/jit_compiler_a64.cpp:654-681`) shows it already emits
  `ldr d<Rt>, [x2, tmp_reg]` directly (line 671-672) — decoded the raw instruction encoding by
  hand (`0xfc606800 | (tmp_reg<<16) | (2<<5) | tmp_reg_fp`) to confirm this is genuinely
  `LDR Dt, [Xn, Xm, LSL #0]`, not just a comment describing intent. `grep -n "ld1"` across the
  whole file returns nothing — the pattern the plan wanted fixed doesn't exist here. Matches
  `ROADMAP.md`'s own Phase 1 log: `O13 | JIT register-offset FP loads | ✅`, done in an earlier
  session, before this master plan was ever written.
- **Item 8 (static FP load/convert software-pipelining): stale, closed, no code change.** The
  plan described `jit_compiler_a64_static.S:236-263` as a serialized chain of `ldr`→`sshll`→
  `scvtf` triples. Reading the actual prologue (`:218-287`) shows its own comment already says
  "Interleaved loading of FP registers (d16-d23) and integer registers (F0-F3) to hide load
  latencies and FP execution delays" — and the code does exactly that: independent `ldr`
  batches, then `sshll` batches, then `scvtf` batches, further interleaved with independent
  integer XOR work to fill the gaps. More sophisticated than the plain grouping this item
  proposed. Matches `ROADMAP.md`'s Phase 1 log: `O12 | JIT prologue instruction scheduling | ✅`.
- **Item 9 (superscalar literal-pool relayout): verified accurate, genuinely still open.**
  Unlike 7/8, reading `generateSuperscalarHash()` (`src/jit_compiler_a64.cpp:406-505`) confirms
  the literal pool really is emitted inline before each program's code with an always-taken `B`
  branch jumping over it (lines 439-449), once per program — exactly as described. This is the
  only one of the three medium-term items confirmed real; next in line for implementation.

**Takeaway**: a plan authored by a different agent, even one that already correctly re-verified
the CSEL/PGO/NEON-AES/`--stagger-ms` closures, can still cite stale code state for *new*
proposals — its account of what's *already fixed* was accurate, but its account of what's
*still broken* (items 7-8) wasn't. Read the cited code before implementing, every time, not
just for claims about history.

## 2026-07-24 — Phase 6 Item 3: Multi-Worker PMU Attribution — Two Layered Mechanisms, TLB Rejected

Executed `PLAN.md` Phase 6 item 3 on-device: Cortex-A53-specific PMU events
(`l1d_cache_refill`, `l2d_cache_refill`, `ld_dep_stall` — cycles stalled specifically on a
load-miss dependency, discovered via `perf list` on this device rather than relying on generic
architectural aliases), sampled with `perf stat -p <pid>` against a live 15s steady-state
mining window at each of 1/2/4/6/8 workers, no counter multiplexing.

- **TLB-bound: definitively rejected.** dTLB misses stay below 1.4 per *million* instructions
  at every worker count tested — consistent with item 1's huge-page finding; TLB pressure is
  not a factor at any core count.
- **Two distinct mechanisms, not one, with different onset points** — refining rather than
  simply confirming the "DRAM-bandwidth vs. thermal-throttle vs. TLB" three-way framing both
  master plans posed:
  1. **Front-loaded L2/DRAM-adjacent contention.** L2 refill rate per instruction climbs
     steeply from 1→4 workers (2.14→3.10 per 1K instructions, +45%), then nearly flattens 4→8
     (+5.6% total). L1D refill rate stays flat (~4.8/1K instr) at every worker count, so this
     is specifically an L2-and-beyond effect. Tracks `ld_dep_stall`'s own rise (7.6%→12.25% of
     cycles) over the same range.
  2. **A per-core clock reduction onsetting specifically at 6+ workers**, not explained by the
     L2/stall data (which is nearly flat by then). Effective per-core clock — derived from
     `cycles / wall_seconds / worker_count` since this device exposes no `scaling_cur_freq` —
     holds flat at ~772–775 MHz for 1/2/4 workers, then drops to ~641 MHz at 6 workers (−17%)
     and ~567 MHz at 8 (a further −12%, −27% total from baseline). Post-run thermal-zone temps
     stayed mild throughout (36–50°C, idle baseline ~36–41°C) — well below where junction-
     temperature throttling typically first engages on this SoC class — so classic thermal
     throttling doesn't fully explain it; more consistent with a **core-count-triggered
     multi-core power/current cap**. Can't be confirmed directly: this device exposes neither
     `scaling_cur_freq` nor an obvious power-domain/governor knob (checked, absent).
- **Practical read**: the item-2 hashrate sweep's smooth efficiency decline (98.5%→73.0%
  across 4→8 workers) is the sum of both effects — front-loaded memory contention explains
  most of the 1→4-worker decline, the clock cap explains most of the additional 6→8-worker
  decline. **Consequence for item 4** (`--rt-priority`/`isolcpus=`/`nohz_full=`): if the second
  mechanism really is a firmware/kernel power budget rather than scheduler jitter or
  temperature-triggered throttling, CPU isolation is unlikely to touch it — it might still help
  the first (memory-contention) mechanism marginally. Full table and reasoning in `PLAN.md`
  Phase 6 item 3.

## 2026-07-24 — Phase 6 Verification: Huge-Pages Already Coalesced, Worker Sweep Shows No Plateau, Two devbox MCP Bugs Fixed

Executed Phase 6 items 1–2 (`PLAN.md`) on-device, both closed with real, useful results —
plus two real bugs found and fixed in the devbox MCP tooling along the way.

- **Huge-page residency check (item 1): closed as a no-op.** While mining (8 workers, light
  mode, steady state): `AnonHugePages` covers 278,528 kB of 285,432 kB total anon RSS (97.6%),
  and the 256 MiB Argon2 cache mapping specifically shows 100% `AnonHugePages` coverage.
  `Private_Hugetlb`/`Shared_Hugetlb` are both 0 kB and `HugePages_Total: 0` — there is no real
  hugetlbfs pool on this kernel, so every `MAP_HUGETLB` request must be silently failing, but
  THP's `always` policy independently coalesces almost the entire working set anyway. Confirmed
  with `perf stat -e dTLB-load-misses,...` attached to the live mining process: 78,552
  dTLB-load-misses over 49.9B instructions (~1.6 per million instructions) — nothing like the
  "500× TLB reach" worst case either master plan hypothesized. **This closes items 5/6
  (disclose+prefault, reserve a hugetlb pool) as no-ops** — implementing them would change
  nothing measurable on this device.
- **Worker-count sweep (item 2): closed, no plateau found.** 4/5/6/7/8 workers, `--seconds=60`
  each, two full interleaved passes with a 90s cooldown between runs (thermal zones confirmed
  reset to the ~38–41°C idle baseline before every run; both passes agreed to within a few
  hashes at every point). Result: 16.82/19.04/21.13/23.21/24.95 H/s respectively (4.20/3.81/
  3.52/3.32/3.12 H/s per worker) — **98.5%/89.2%/82.5%/77.7%/73.0% scaling efficiency**, a
  smooth monotonic decline with no flat region anywhere in 4→8. **Neither of the two
  anticipated outcomes happened**: efficiency doesn't plateau, and 8 workers gives the highest
  absolute hashrate at every step (each additional worker still contributes ~41–52% of a full
  thread's rate). **The "bank a 6-worker default at equal hashrate" hypothesis is rejected** —
  there is no free lunch in this range. Scope caveat: each point is a 60s window; long-duration
  (15–30 min) sustained thermal throttling remains untested (item 3, PMU attribution).
- **`README.md` re-baselined** with the sweep's real numbers (4.27 H/s single-thread, 16.82/
  21.13/24.95 H/s at 4/6/8 workers), replacing the stale 5.18 H/s / 25.28 H/s "linear scaling"
  claim (closes item 15 early, since the data was already in hand).
- **devbox MCP tooling: two real bugs found and fixed while running the above**
  (`tools/devbox/devbox_mcp.py`), matching the same "verify the tooling, not just the target
  claim" discipline that found the tilde-expansion bug during Phase 5's PGO work:
  1. `tool_test()` called `cfg.timeout()` — the 120s `"default"` bucket — instead of a
     bench-scale one, so any unfiltered `devbox_test` call (the full 12-test suite, `bench_armrx`
     alone ~300s) was essentially guaranteed to time out. Added a dedicated `"test"` timeout
     bucket (900s) and used it at that call site; added to `devbox.json`/`devbox.example.json`.
  2. The MCP server itself is a single-threaded, synchronous stdin-read loop that ran tool
     handlers inline — while a long call executed, the server couldn't read or answer anything
     else on stdin (including the host's own liveness `ping`), so the host concluded the server
     had hung and force-reconnected mid-call, losing in-flight work twice during this session
     before being root-caused. Fixed by running each `tools/call` dispatch in a worker thread
     (main loop stays free to answer `ping`), with a `_device_lock` around the actual remote
     SSH/rsync invocations so this doesn't let two device operations race each other.
  **Verified**: a real single `devbox_test` call completed cleanly in 612.72s (12/12 passed),
  connection staying up the whole time unattended — past both the old 120s bucket and the 600s
  bench bucket. (Manually firing a *second* concurrent tool call while one was in flight reliably
  killed the connection immediately — this MCP host's client appears to only tolerate one
  in-flight request per connection; that's separate from, and not fixed by, the threading change.)

## 2026-07-24 — Dual Performance Master-Plan Synthesis (PLAN.md Phase 6) + Doc Reorg

Two independent performance master plans were produced against HEAD `88f4122`:
`docs/plans/performance-master-plan.md` (this assistant) and
`docs/plans/performance-master-plan-20260724.md` ("Hermes" agent). Both explicitly build on and
re-verify Phase 3/5's closed-leads list (CSEL/CBRANCH, NEON AES ×3, Newton-Raphson, PGO,
`--stagger-ms`) without reopening any of them.

- **Reconciled rather than run in parallel.** Both plans independently agree on the same two
  highest-EV, zero-code-risk next steps — huge-page residency verification for the 256 MiB
  Argon2 cache and 2 MiB scratchpad (asserted via `MAP_HUGETLB`/`MADV_HUGEPAGE` "succeeding,"
  never actually confirmed on-device), and a worker-count sweep (never run on this hardware,
  currently ~68% scaling efficiency at 8 workers vs. ideal). Both plans independently frame
  IPC 0.708 as proof this is a memory-latency-stall-bound workload, not instruction-
  throughput-bound — the reason every closed instruction-count lead (CSEL, Newton-Raphson,
  NEON-AES) failed.
- **One real divergence, reconciled rather than picked between:** this assistant's plan ranks
  instruction-level JIT work (peephole coalescing, literal-pool relayout) as low-EV until the
  memory-side questions are answered; the Hermes plan proposes specific emitter changes
  (register-offset FP loads, static load/convert software-pipelining, literal-pool relayout)
  framed as *latency-hiding*, not instruction-count reduction — mechanistically distinct from
  the already-falsified CSEL/Newton-Raphson category. Adopted both: the shared verification
  step first, the Hermes-plan emitter items as a gated second tier, the full peephole-JIT
  rewrite as lowest-EV pending a fresh region-scoped instruction-count comparison.
- **Adopted as `PLAN.md` Phase 6**: a 15-item phased plan (4 short-term verification items,
  2 contingent-on-verification items, 5 medium-term JIT-emitter items with file:line targets
  and required test gates, 4 long-term/high-risk items) — see `PLAN.md` for the full list.
  Nothing in it has started yet; this entry is planning/documentation only.
- **Doc reorg**: `PLAN.md` had grown to 400+ lines, almost entirely completed-phase narrative
  sitting in front of the actually-open work. Split Phases 1–5's full narrative out to
  `docs/archived/plan_completed_phases_1-5.md` (verbatim, nothing altered), leaving `PLAN.md`
  as a lean current-state doc: a short completed-work summary plus Phase 6 in full. Synced
  `NEXT_STEPS.md` (new Phase 6 actionable checklist, stale-telemetry warning added to the
  hashrate table, resolved-items list consolidated) and `ROADMAP.md` (status note, baseline
  hashrate section marked stale pending re-measurement, 10 new Performance-table rows for
  Phase 6, reference-docs table updated) to match.

## 2026-07-23 — `--stagger-ms` Lead Closed: Already Tested Previously, Found Ineffective

Closes the third and final adopted lead from the external audit (`PLAN.md` Phase 5, `NEXT_STEPS.md` §5a) — without spending on-device time, since it turned out to be redundant with prior work:

- Before running the suggested experiment, checked whether it had already been tried. It had: `docs/archived/beyond-parity_v2.md` documents startup stagger tested at 5, 20, 100, and 1000ms on this same device, with the explicit finding "+0% (tested, ineffective)... a hardware ceiling."
- **Why it can't work**: RandomX's memory pressure is continuous — the full 2 MiB scratchpad is touched on *every* hash iteration, not just at startup — so a one-time launch-time delay can't desync steady-state phase alignment across 8 workers the way the audit's suggestion assumed. The 8-worker efficiency drop (25%, `changelogs.md` 2026-07-21) is single-channel LPDDR3 bandwidth saturation, a hardware ceiling, not a fixable scheduling artifact.
- Left `stagger_ms_`'s default (0, `src/mining_engine.cpp:313`) unchanged. This finding predates this session's other work and was not re-verified against the current codebase — flagged as a caveat rather than treated as settled, but not re-run given how mechanism-clear and hardware-fundamental the prior result is.

## 2026-07-23 — NEON Vector-Permute AES: Derived, Exhaustively Verified, Measured as a Regression

Implements adopted lead #2 from the external performance audit (`PLAN.md` Phase 5, `NEXT_STEPS.md` §5a):

- **Derived a full "vector-permute AES" S-box from scratch, in Python, before writing any C++**: found a root of AES's defining polynomial (x⁸+x⁴+x³+x+1) inside a tower-field representation GF(2⁴)[y]/(y²+y+λ), giving a provably correct isomorphism between GF(2⁸) (the AES field) and GF(2⁴)² — every sub-step of this then fits ARM NEON's 16-entry `vtbl`/`vqtbl1q` instructions, unlike the 256-entry T-tables this project uses (`src/soft_aes.cpp`), which don't. Verified byte-for-byte against the standard FIPS-197 S-box/inverse-S-box for all 256 values, and the full round structure (ShiftRows/MixColumns and their inverses) against 3000 random trials matching this codebase's actual `randomx_aes_lut_enc`/`randomx_aes_lut_dec` semantics — before ever touching an ARM intrinsic.
- **Implemented** `encrypt_transform_neon`/`decrypt_transform_neon` (`include/armrx/aes.hpp`), gated behind a new `ARMRX_ENABLE_NEON_AES` CMake option (default OFF, matching `ARMRX_ENABLE_JIT_FAST_DIV_SQRT`'s established pattern) — every other call site (`aes_hash.cpp`, `aes_generator.cpp`) unchanged. Confirmed this is genuinely distinct from the two previously-reverted *hardware* `AESE`/`AESD`/`AESMC` attempts (2026-07-20 entry below) — a software vector-permute S-box has no fixed-AddRoundKey-position constraint.
- **New `tests/test_aes_neon.cpp`**: 256/256 exact match for SubBytes and InvSubBytes against the standard S-box (broadcast-tested across all 16 NEON lanes), plus 20,000 random full-round parity trials comparing scalar vs NEON directly, both directions. **Compiled and passed on the first attempt on real hardware, zero bugs found** — a direct result of the exhaustive prior mathematical verification. Full KATs and `tests/test_aes_hash.cpp`'s existing golden pins stayed byte-identical with the flag on; full `ctest` 12/12 green on-device.
- **Measured the actual payoff honestly, apples-to-apples** (twice, to rule out a thermal artifact — the first run had high variance, the second was clean and matched the first's mean): a real **~19.4% regression** on `fill_aes_1r_x4`/`hash_aes_1r_x4` (28.4ms→33.9ms / 28.6ms→34.2ms, `bench_armrx --micro-only`). Same root cause as the 2026-07-20 hardware-AES finding: per-block NEON load/store overhead cancels the lookup savings on this Cortex-A53, independent of which specific NEON AES technique is tried.
- **Outcome**: kept, not reverted — since it's flag-gated (not an unconditional rewrite like CSEL), "not adopted" just means the default stays OFF. The implementation, its exhaustive test coverage, and the from-scratch mathematical derivation are reusable reference material even though the performance didn't pan out on this hardware. Full account in `docs/experiments/neon-vector-permute-aes.md`.

## 2026-07-23 — PGO Devbox Wiring: Tool Shipped, Real Infra Bug Fixed, Payoff Claim Did Not Reproduce

Implements adopted lead #1 from the external audit (`PLAN.md` Phase 5, `NEXT_STEPS.md` §5a):

- **Added `devbox_pgo_build`** (`tools/devbox/devbox_mcp.py`): orchestrates GENERATE (clean rebuild) → train (`armrx --mine --seconds=N`, sustained light-mode JIT mining per `docs/plans/performance-next-agent-handoff.md` §10.3, not CLI startup/cache init) → USE (reconfigure + rebuild consuming the collected `.gcda` profile data), all in one call. LTO auto-disables for both PGO stages via existing `CMakeLists.txt` logic.
- **Found and fixed a real, pre-existing bug in the devbox tooling itself** while validating the new tool: `_stash_and_run`, `tool_status`, and `tool_test` were `shlex.quote()`-ing paths built from `cfg.remote_dir`, which single-quotes the string and silently defeats shell tilde expansion — since this project's actual `devbox.json` uses `remote_dir: "~/armrx"`, every build/test/bench log was landing in a disconnected literal `~` directory instead of the real repo tree, and `devbox_status`'s deployed-revision check was permanently reading from that same wrong location (always reporting no sync had happened, even right after a real one). Confirmed this had been silently active across earlier sessions too (found stale logs from unrelated prior runs in the bogus directory). Fixed by interpolating `remote_dir`-derived paths unquoted, matching the convention `tool_build`'s own commands already used correctly — `remote_dir` is a trusted config value, not attacker-controlled input.
- **Measured the actual payoff honestly, apples-to-apples**: rebuilt a fresh non-PGO baseline and the PGO USE build (two training durations tried, 15s and 90s, to rule out under-training) on the same device, back-to-back. Both measured **identical 4.27 H/s** single-thread steady-state (`armrx --mine --seconds=60 --workers=1`) — not the historically-claimed 5.18 H/s (+19.3%). Confirmed this isn't a broken flow: `.gcda` files were real and non-empty, `-fprofile-use -fno-lto` confirmed present in `armrx`'s actual link command, KATs passed on every build. Most likely explanation: substantial hot-path code has changed since the 2026-07-21 measurement that produced +19.3% (Argon2 diagonal-step vectorization, the JIT startup log line, several correctness fixes), shifting the code shape PGO's compile-time decisions were originally tuned against.
- **Outcome**: the tool is kept (mechanically correct, useful for future re-evaluation), but the audit's "+19.3%, single biggest lever" claim is now known to be stale and should not be repeated without re-measuring against the codebase at the time.

## 2026-07-23 — External Performance Audit: Verified, Fixed a Stale Doc, Adopted Two Leads

Another agent's `docs/audits/performance-improvement-audit.md` proposed several performance leads. Fact-checked each claim against the actual codebase/history before acting on any of it (standard practice for externally-sourced recommendations):

- **Fixed a stale `ROADMAP.md` entry** the audit's citation exposed: the Newton-Raphson FDIV/FSQRT row said "Failed once (segfault). Do not retry..." — describing an *earlier*, separate `x29`-register-corruption bug that was since root-caused and fixed (`docs/audits/WX_Alignment_and_LITTLE_Core_Profiling.md` §5). Newton-Raphson was then cleanly re-evaluated: 100% correctness/determinism pass, but measured 5.12 H/s vs 5.18 H/s for native hardware `fdiv`/`fsqrt` (−1.1%, `OPTIMIZATION_REFERENCE.md`, `changelogs.md` 2026-07-21) — kept off for performance, not safety. Corrected the entry to reflect this.
- **Verified and adopted two substantive leads** into `PLAN.md` Phase 5 / `NEXT_STEPS.md` §5a: (1) PGO is plumbed into CMake but the default devbox build doesn't use it (confirmed — `tools/devbox/devbox_mcp.py:56`'s default flags have no `ARMRX_PGO`; the +19.3%/+14.9% figures match this project's own telemetry exactly); (2) a NEON `vtbl`-vectorized software T-table AES path is genuinely untried — confirmed distinct from the two previously-reverted *hardware* `AESE`/`AESD`/`AESMC` attempts (`changelogs.md` 2026-07-20); the current software AES path is 100% scalar.
- Both adopted items carry the same profile-first, hashrate-vetoed-on-device discipline used for every other performance change this session — neither is implemented yet, both are tracked as next steps.

## 2026-07-23 — Argon2 `memcpy` Copy-Elimination: Implemented, Measured, Reverted (No Net Win)

Closes out the last item in the Argon2 performance backlog (`NEXT_STEPS.md` §5, `PLAN.md` Phase 3 item C):

- **Investigated the tracked lead**: the diagonal-step profiling pass had also attributed 5.54% of cycles to `memcpy` — traced to `argon2_compress()`'s `auto permuted = result;`, a 1024-byte `Argon2Block` copy needed because `permute_block` mutates its argument in place (the algorithm needs both the original `R = previous^reference` and the permuted `Z` to compute the final XOR).
- **Implemented a fix**: gave `permute_block` an out-of-place `permute_block_into(src, dst)` sibling (both NEON and scalar variants) that fuses the copy into the row step's existing load/store instead of doing a separate whole-block `memcpy` first. Verified correct first: full KAT hashes, reference dataset-item checks, and `ctest` all green, both scalar (x86_64) and NEON (on-device) paths, before any benchmarking.
- **Measured honestly, reverted**: apples-to-apples `perf stat` (old code rebuilt fresh on-device) showed -2.86% instructions but **+0.35% cycles** — flat to slightly worse. Symbol-attributed `perf record` explained why: the `memcpy` cost didn't disappear, it relocated into the new function (`memcpy` 6.77%→3.56%, but a new `permute_block_into_neon` appeared at 12.74%) — glibc's `memcpy` was already about as fast as the hand-rolled replacement on this hardware. Reverted (`git checkout -- src/argon2.cpp`), same standard applied to the CBRANCH/CSEL investigation. Full account in `docs/experiments/argon2-compress-copy-elimination.md`.
- **Follow-up, same day**: rebuilt `bench_armrx` with debug symbols (`-g`, same optimization flags) for `perf annotate` instruction-level attribution of `Argon2dCache::initialize`'s remaining 23-25% cycle share. Found it isn't separate driver overhead at all: 92% of sampled instructions cost ≈0%, including the actual address/reference-computation arithmetic (`j1`, `square`, `x`, `y`, `relative`, `reference`). Every hot instruction is a NEON `eor`/`ldr q`/`str q` — `argon2_compress()`'s own XOR-combine loops, auto-vectorized and inlined directly into `initialize`'s body by the compiler. **This closes out the entire Argon2 performance backlog**: it's inherent, spec-required compression work, already well-optimized, not a missed optimization.

## 2026-07-23 — JIT Buffer RWX/W^X Mode Now Disclosed at Startup

Closes PLAN.md Phase 4 item F (open hardening-posture decision):

- **Decision (explicit, user-directed): keep the RWX-by-default JIT buffer behavior unchanged for now** ("we may or may not change it later") — no perf/security tradeoff was altered.
- **Made it visible instead of silent.** `JitCompilerA64`'s constructor (`src/jit_compiler_a64.cpp`) now logs which protection mode is active — `"JIT code buffer: RWX (...)"` or `"JIT code buffer: W^X enforced (...)"` — exactly once per process, guarded by a static `std::atomic<bool>` since one `JitCompilerA64` exists per worker thread and all of them land on the identical result (same process, same kernel policy).
- **Verified on-device**: with 2 workers, the line fires exactly once (not twice), correctly reporting `RWX` on this device's stock Linux kernel.

## 2026-07-23 — MetricsExporter Data Race Fix + Test Coverage Gaps Closed (Found a Real `--config=` Bug)

Closes PLAN.md Phase 4 items C and E.1/E.2:

- **`MetricsExporter::server_fd_` data race fixed** (`include/armrx/metrics.hpp`): plain `int` written by the background server thread and read by the destructor on another thread with no synchronization, a real (if narrow) data race under the C++ memory model. Changed to `std::atomic<int>`. One-line, zero-risk fix; no new test added (the existing `-fsanitize=thread` build option is the tool to re-verify with if this area is revisited).
- **New `tests/test_cli_parser.cpp`** (15 cases covering every flag family, malformed-value exit codes, `--version`/`--help`, unknown-argument handling, and config-file/CLI-override precedence) — `cli_parser.cpp` previously had zero automated tests. **Found a real, previously-unknown bug while writing it**: `--config=<path>` was consumed by the config pre-scan (to load defaults before CLI overrides) but never recognized in the main flag-parsing loop, so it always fell through to `"Unknown argument: --config=..."` and made the process exit with code 64 — the documented `--config=` flag was completely broken for any invocation using it. Confirmed against the actual built `armrx` binary before fixing (`./armrx --config=/tmp/x.json --help` exited 64 pre-fix, 0 post-fix). Fixed with an explicit `continue` on `--config=` in the main loop (`src/cli_parser.cpp`).
- **New `tests/test_aes_hash.cpp`**: direct coverage for `fill_aes_1r_x4`/`fill_aes_4r_x4`/`hash_aes_1r_x4`/`hash_and_fill_aes_1r_x4` (`aes_hash.cpp`), previously only exercised indirectly via full end-to-end RandomX KAT hashes. Includes a golden-output pin for `fill_aes_1r_x4` (captured from the current KAT-verified-correct implementation) plus determinism/prefix-consistency/input-sensitivity checks, and — the main new coverage — a decomposition-equivalence check proving `hash_and_fill_aes_1r_x4`'s combined hash+fill pass produces byte-identical results to calling `hash_aes_1r_x4()`/`fill_aes_1r_x4()` separately on the same inputs, which is the actual contract that fused function exists to provide.
- **Verified**: full `ctest` green locally (x86_64, 7/7 including the two new tests) and on-device (AArch64 JIT).

## 2026-07-23 — Two Phase 4 Correctness Fixes: Worker-Thread Death on Bad Nonce Job, Config Parse Crash

Closes items A and B from `PLAN.md`'s Phase 4 fresh-codebase-inspection findings:

- **`MiningEngine::worker_loop()` no longer permanently kills a worker thread** on a bad nonce offset/size (`src/mining_engine.cpp`). When `update_nonce_in_template()` failed (`nonce_offset + nonce_size > block_template.size()`, reachable via a malformed/truncated pool job), the handler set `active = false` then called `return;`, which exited `worker_loop()` entirely — ending that thread for the rest of the process's life, silently degrading hashrate with no crash. Changed to `active = false; continue;`, matching every neighboring bad-state path in the same function. New regression test `test_worker_survives_bad_nonce_job()` (`tests/test_mining.cpp`) feeds a deliberately malformed job, confirms both workers log the error and idle (`total_hashes() == 0`) rather than dying, then confirms the *same* threads pick up a subsequent valid job and mine normally.
- **`config.cpp`'s numeric config-file fields are now exception-guarded.** `parse_pool_str()`'s port parsing and `load_config()`'s `workers`/`difficulty`/`seconds` parsing called `std::stoul`/`std::stoull` directly on raw JSON-extracted strings with no `try`/`catch`, unlike `cli_parser.cpp`'s already-guarded equivalent CLI flags. Since `load_config_with_fallback()` runs unconditionally on every launch (auto-probing `$ARMRX_CONFIG`/`~/.config/armrx/config.json`/`./armrx.conf` even with no `--config=` flag), a single malformed default config crashed the whole miner via an unhandled exception before it ever logged anything useful. Each conversion is now wrapped in try/catch, logging a warning and falling back to `AppConfig`'s default on failure. New `tests/test_config.cpp` (3 cases: malformed fields all at once, valid fields still parse correctly, missing file returns defaults).
- **Verified**: both fixes built and tested locally (x86_64 interpreter, full `ctest` green including the two new tests) and on-device (AArch64 JIT, full `ctest` green).

## 2026-07-23 — Argon2 NEON Diagonal-Step Vectorization (26.8% Fewer Instructions, 19.0% Fewer Cycles)

Following the CBRANCH investigation's own recommendation to look at `Argon2dCache::initialize` next:

- **Profiled first** (`bench_armrx --argon2-only`, a new isolated benchmark added for this): multi-event `perf stat` (IPC 0.65, branch-miss rate 2.0%, cache-miss rate 0.3%) ruled out both branch-misprediction and memory-boundedness, despite Argon2's memory-hardness design making the latter a reasonable prior. `perf record -e cycles` (212K samples) then attributed **38.09% of all cycles to the scalar `gb()` mixing function alone**.
- **Root cause**: `permute_16_neon()`'s 4 "diagonal" mixing rounds fell back to sequential scalar `gb()` calls (long dependency chains, no ILP), while its 4 "column" rounds already get 2x NEON parallelism via `gb_neon()` — because the diagonal register-pairs aren't memory-adjacent, so the straightforward `vld1q_u64` load doesn't work for them directly.
- **Fix** (`src/argon2.cpp`): gather the one non-adjacent operand pair per diagonal group via `vcombine_u64(vld1_u64(...), vld1_u64(...))`, reusing the existing `gb_neon()` unchanged — the same gather/scatter approach `permute_block_neon()`'s outer loop already uses for non-adjacent columns, applied one level deeper.
- **Verified**: KAT hashes and reference dataset-item first-words byte-identical (both interpreted and JIT), `ctest` 8/8 on-device + 4/4 on x86_64 (scalar path untouched). Apples-to-apples `perf stat` (old code rebuilt fresh, both runs back-to-back to control for this device's real thermal/frequency variance between runs) shows **26.8% fewer instructions, 19.0% fewer cycles** (11,364 → 9,204 cycles per `argon2_compress` call).
- **Scope, stated honestly**: this speeds up seed-key-rotation *latency* (cache init runs once per ~2048 blocks, not per hash) — it does not change sustained steady-state hashrate. Full account in `docs/experiments/argon2-neon-diagonal-vectorization.md`.

## 2026-07-22 — CBRANCH CSEL: Implemented, Measured, Reverted; Root-Caused the 31.08% Figure

Closes the CBRANCH investigation started earlier the same day (see the "Precise Branch-Miss Attribution + Test Hardening" entry below):

- **Implemented the CSEL-based CBRANCH rewrite** `docs/experiments/branchless-cbranch.md` had sketched (`ands` for the masked value/flags, two `adr`s to compute both possible next-PC values, `csel` to pick one, single unconditional `br`). Caught one real bug via the KAT test before ever benchmarking: `csel`'s `Rn`/`Rm` register fields were transposed, silently inverting which address got selected — the JIT hash was simply wrong until fixed. Once correct (`ctest` 8/8 on-device), a clean apples-to-apples `perf stat` comparison (old `bne`/`b` code rebuilt fresh in a separate directory for a fair baseline, not compared against a number captured under different measurement overhead) showed CSEL is a **net regression**: +0.87% instructions, +0.32% cycles, **+46% branch-misses**, hashrate flat. **Reverted** to `bne`/`b` (`git checkout` back to `e563112`).
- **Root-caused the 31.08% branch-miss figure** that's justified CBRANCH-focused work across multiple sessions: it does not represent the mining hot path. Isolating `bench_armrx --full-hash-only` shows only **2.4%**. Running `perf stat` on each of `bench_armrx`'s three sections separately and summing reproduces the historical 31.08% aggregate almost exactly, confirming the reconciliation — the aggregate is **94.93%** driven by `--attribution-only`'s non-representative 30-sample interpreted-mode comparison run (never executed during real JIT mining), which is nearly identical to the old audit's "94.85% in Superscalar" claim — strong evidence that claim measured the same phenomenon but misattributed its cause. Recalculated real-world impact: ~0.11–0.16% of cycles lost to CBRANCH misprediction on the actual hot path, not the previously-estimated ~8.6–11.9%.
- **Conclusion: CBRANCH misprediction was never a meaningful real-world performance lever on this hardware.** No further JIT branch-encoding work is planned on this basis. Full account in `docs/experiments/branchless-cbranch.md`; `PLAN.md`, `ROADMAP.md`, `NEXT_STEPS.md` updated to close out this item.

## 2026-07-22 — CBRANCH Precise Branch-Miss Attribution + Test Hardening

Prep work for CBRANCH JIT hot-path changes, done before touching any production code:

- **Executed the "profile first" prerequisite** every prior session recommended but none had done: `perf record -e branch-misses` with symbol attribution (not just `perf stat`'s aggregate count) on `bench_armrx --full-hash-only`. Cross-checked a large `[k]`-tagged (kernel-space) bucket against `strace -f -c` on the identical workload (53 syscalls, 6.6ms total) — ruling it out as real kernel work; it's PMU sampling skid on this Cortex-A53 (no ARM SPE), a measurement artifact rather than a performance lever. Confirmed CBRANCH is the JIT compiler's only emitted data-dependent conditional branch (grepped every `0x54xxxxxx` B.cond site in `jit_compiler_a64.cpp`), and confirmed Superscalar is *not* the dominant contributor on this hardware/mode (`generate_superscalar` 0.56% of samples) — refuting the old audit's "94.85% in Superscalar" claim for this case. Full numbers in `docs/experiments/branchless-cbranch.md`'s new "Precise attribution" section.
- **Strengthened `tests/test_jit_encodings.cpp`**, which previously only checked each CBRANCH's emitted size (≥4 bytes) despite its own file comment claiming to verify branch targets. Now decodes the actual `bne`/`b` bytes and asserts the computed target is real, backward, and in-bounds — via a new read-only `getCodeBytes()`/`getJitCodeBytes()` accessor (deliberately const-only, unlike the deleted mutable `getCode()`). Discovered and worked around a real subtlety along the way: `randomx_calculate_hash()` runs several chained internal rounds reusing the same JIT buffer, so dump entries from earlier rounds share offset numbers with — but point to memory since overwritten by — the final round.
- **Added `tests/test_jit_equivalence.cpp`**: a JIT/interpreter equivalence sweep across 8 seeds × 2 inputs, vs. the existing KAT's 2 fixed inputs. Bounded with an explicit `TIMEOUT 600` given a prior CBRANCH bug's documented history of a 120s hang.
- Verified: `ctest` 4/4 on x86_64, 8/8 on-device (up from 7).

## 2026-07-22 — Fresh Codebase Inspection (PLAN.md Phase 4) + Doc Maintenance

- **Ran a from-scratch codebase inspection** (not a restatement of prior, partly-stale audits) after Phase 2/3 fully closed out. Found and documented in `PLAN.md`'s new Phase 4 section (not yet fixed, pending prioritization): (A) `MiningEngine::worker_loop()` permanently kills a worker thread on a bad nonce offset/size instead of skipping the job (`mining_engine.cpp:418-423`, `return;` should match the fallthrough pattern every neighboring error path uses) — pool-triggerable, silent hashrate degradation; (B) `config.cpp`'s numeric config-file fields (`workers`/`difficulty`/`seconds`/pool port) aren't exception-guarded against parse failure the way `cli_parser.cpp`'s equivalent CLI flags already are, so a malformed default config crashes the miner on every launch; (C) `MetricsExporter::server_fd_` is a plain `int` touched from two threads without synchronization; (D) `NEXT_STEPS.md`/`STATUS_REPORT.md`/`CLAUDE.md`'s own CTest-path caveat were stale/incorrect; (E) test coverage gaps (`cli_parser.cpp`, direct AES helper KATs, `tls_client.cpp`/`tui.cpp`); (F) the JIT buffer's RWX-by-default posture is a real, currently-invisible-to-operators hardening tradeoff, flagged as a decision for the maintainer rather than a unilateral fix.
- **Archived `STATUS_REPORT.md`** to `docs/archived/status-report-20260720.md` — a one-time dated deep-dive snapshot (HEAD `fba761e`, 2026-07-20), same genre as the other already-archived planning docs, now several sessions stale and superseded by `PLAN.md` plus the dated postmortems.
- **Regenerated `NEXT_STEPS.md`** from current state (previously dated 2026-07-21, HEAD `a3a7244`, listing multiple already-fixed items — including the `MetricsExporter` thread-detach race and worker-thread dataset reuse — as still open).
- **Updated `ROADMAP.md`**: added a "Completed — Phase 3 (This Session)" section covering everything landed 2026-07-22, refreshed the branch-miss baseline to the re-measured 31.08%, and rewrote the Remaining action list to match the Phase 4 findings above.
- **Corrected two stale claims in `CLAUDE.md`**: the JSON parser fuzzing harness was listed as "a planned next step" (it's done, `tests/fuzz_json.cpp`); the documented CTest "Not Run" path caveat did not reproduce in any on-device run this session (all 7 tests passed cleanly via plain `ctest` every time) — noted as re-verified rather than removed outright, in case it's environment-specific and recurs.

## 2026-07-22 — Derived kCompileHandlers[256] from instruction_weights.hpp

- `src/vm.cpp`'s hand-written 256-entry `kCompileHandlers` dispatch table (73 lines of manually-counted opcode ranges) is now built from `instruction_weights.hpp`'s `RANDOMX_FREQ_*`/`REPN`/`WT` macros via `INST_HANDLE(x)`, mirroring the identical pattern `jit_compiler_a64.cpp` already uses to build its own 256-entry opcode table. Confirmed the hand-written ranges matched the frequency table's values exactly, in the same order, before making the change. The JIT and interpreter's opcode-to-instruction-type maps are a correctness-critical invariant (both must dispatch every opcode identically); building both from the one spec-derived table means a typo in either hand-maintained copy can no longer cause silent drift between them.
- Verified: KAT hashes byte-identical before/after on both x86_64 (interpreter path) and on-device (JIT path). `ctest` 4/4 on x86_64, 7/7 on-device including `test_jit_encodings`/`test_jit_determinism`.
- This completes all 3 items in `PLAN.md` §5 item E — the full constant-dedup list from the prior handoff is now done.

## 2026-07-22 — Consolidated Duplicated AES Round-Key and Scratchpad-Mask Constants

- **AES round-key constants** (`src/aes_generator.cpp`, `src/aes_hash.cpp`): confirmed numerically that `aes_generator.cpp`'s `key0..key3`/`key4r0..key4r7` and `aes_hash.cpp`'s `key1r_0..key1r_3`/`key4r_0..key4r_7` were the exact same 12 RandomX-spec round-key blocks encoded two different ways (raw byte-array literals vs. `build_aes_block()` from big-endian words) before touching any code. Extracted to a new `include/armrx/aes_keys.hpp` (`kAesGen1RKey0..3`, `kAesGen4RKey0..7`); both files now share one definition. `aes_hash.cpp`'s own unique `hash_state_*`/`hash_xkey_*` constants stay local but now reuse the header's `build_aes_key()` helper instead of a second copy of it.
- **Scratchpad L3 mask constants** (`include/armrx/randomx_config.hpp`, `src/vm.cpp`, `src/jit_compiler_a64.cpp`): `vm.cpp`'s four `kScratchpadL*Mask` constants and `jit_compiler_a64.cpp`'s separately hardcoded `ScratchpadL3Mask` literal are now all derived from one `scratchpad_mask()` constexpr helper, with the exact prior literal values confirmed numerically before landing. Also collapsed `jit_compiler_a64.cpp`'s three independent `Log2(RANDOMX_SCRATCHPAD_L3)` re-derivations into a single named `ScratchpadL3Log2` constant.
- Both changes are pure constant-sourcing refactors with zero intended behavior change — verified by comparing KAT/JIT hash output byte-for-byte before and after, not just running the test suite and trusting green. `ctest` 4/4 on x86_64 (interpreter path), 7/7 on-device (JIT path, including `test_jit_encodings`/`test_jit_determinism`, the tests that would catch a JIT byte-code regression from this class of change).
- This completes `PLAN.md` §5 item E's constant-dedup list (item 3, `kCompileHandlers[256]`, remains optional/lowest-priority and undone).

## 2026-07-22 — Fixed Both Documented Pool-Failover Gaps

See `docs/postmortems/pool-failover-deadlock-postmortem.md` for the full writeup (updated in place, not a new doc). Summary:

- **Fixed the AUTO-fallback gap**: a pool unreachable from process startup (DNS failure, connection refused before any handshake) never used to trigger failover, since `StratumClient::reconnect_loop()` is only armed by the reader thread noticing a *previously live* connection drop — a pool dead from the start never gets that chance, so `reconnect_attempts()` stayed 0 forever. Added `StratumClient::reconnect_loop_active()` (set synchronously before the reconnect thread spawns, cleared on every exit path) so `PoolManager::tick()` can distinguish "no reconnect loop has ever run" from "one is running but hasn't incremented its counter yet" — the naive `reconnect_attempts()==0` check can't tell these apart, and an earlier draft that used it directly raced ahead of the real exponential backoff (caught because `test_pool_failover` finished in under a second instead of ~31s — a test passing suspiciously fast is still a finding). `PoolManager` now tracks its own `sync_retry_count_` for the never-armed case, using the same 5-retries/2s-cooldown policy.
- **Fixed the stale-reconnect-thread join latency**: `connect_to_current()` destroying the old `StratumClient` used to block up to `kMaxBackoffMs` (30s) in `~StratumClient()`'s join, since `reconnect_loop()`'s `sleep_for()` can't be woken early. Switched to `std::condition_variable::wait_for()` against a new `reconnect_cv_`, woken by `disconnect()` right after it disables the loop — no lost-wakeup race since the predicate re-checks the atomic flag before ever blocking.
- **New test coverage** (`tests/test_pool_protocol.cpp`, now 7 scenarios): `test_failover_from_pool_dead_at_startup` and `test_disconnect_interrupts_reconnect_backoff`. `test_pool_failover`'s timeouts tightened back down (150s → 60s wait, matching the ~31s real backoff without the now-eliminated ~30s stale-join padding).
- Verified: x86_64 local `ctest` 4/4 (`test_pool_protocol` 36s). AArch64 on-device full `ctest` 7/7 (`test_pool_protocol` 36s), confirming `test_pool_failover` still exercises the genuine 1s/2s/4s/8s/16s backoff rather than short-circuiting it.

## 2026-07-22 — Root-Caused and Fixed the On-Device LTO Build Regression

- **Root-caused the on-device LTO link failure** (`CMakeLists.txt`) previously documented but not root-caused, and initially misattributed to the `main.cpp` → `cli_parser.cpp`/`miner_app.cpp` split. Bisection disproved that: building the pre-split commit (`d7ca542`) reproduces the identical failure, and forcing `-flto-partition=one` (single WHOPR partition) does not fix it either — ruling out LTO partitioning as the mechanism entirely.
- **Actual cause:** Alpine's `fortify-headers` package wraps libc calls (`vsnprintf`, reached via `std::to_string(double)` → libstdc++'s `__to_xstring`) in `extern`+`always_inline` functions incompatible with GCC LTO — GCC hard-errors ("function body can be overwritten at link time") instead of emitting an out-of-line call. Confirmed via `apk info`/`/var/log/apk.log` that the on-device toolchain was upgraded `gcc-15.2.0-r6 → r8` on 2026-07-13, well before this session — the bug has been latent since then, just not previously hit by a from-scratch LTO build of the `armrx` executable target specifically.
- **Fixed**: scoped `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` to just the `armrx` target when LTO is enabled, rather than disabling LTO project-wide via the existing `ARMRX_DISABLE_LTO` option. Verified on-device: the standard documented build command (LTO on, no workaround flag) now links `armrx` cleanly and `ctest` passes 7/7.
- Removed the now-stale `-DARMRX_DISABLE_LTO=ON` workaround notes from `README.md` and `REASONIX.md`; corrected the misattribution in `PLAN.md` §5 item C and `docs/postmortems/fast-mode-dataset-corruption-postmortem.md`.

## 2026-07-22 — Fresh Performance Re-Baseline; Documented On-Device LTO Build Regression

- **Re-measured branch-miss rate on-device** (Cortex-A53, `perf stat -e instructions,cycles,branches,branch-misses ./build/bench_armrx`): **31.08%**, essentially unchanged from the pre-PGO `NEXT_STEPS.md` baseline (31.6%) despite everything landed since (PGO, O12/O13, AES fix, Argon2 NEON, worker-thread dataset reuse, two critical bug fixes this session). ~8.6–11.9% of total cycles estimated lost to misprediction penalty. See `PLAN.md` §5 item C for the full numbers and reasoning on why the old "94.85% of misses are in dataset generation, not per-hash" claim doesn't transfer to this light-mode-only hardware. No JIT compiler code changed — data-gathering only, per the plan's decision gate; a recommendation is recorded but CBRANCH/peephole work has not been started.
- **Found and documented (not root-caused) a real build regression**: the standard documented build command (no `-DARMRX_DISABLE_LTO=ON`) now fails to link the `armrx` executable on the on-device GCC15+musl toolchain, caused by the earlier `main.cpp` → `cli_parser.cpp`/`miner_app.cpp` split changing how LTO partitions that target. `README.md` and `REASONIX.md` updated with a note that `-DARMRX_DISABLE_LTO=ON` is currently required for on-device builds.

## 2026-07-22 — Phase 2 Complete: JSON Parser Fuzzing

- **Added a LibFuzzer harness for `armrx::json`** (`tests/fuzz_json.cpp`, PLAN.md §3.1): fuzzes the module's full public API (`get_string`/`get_raw`/`get_array_first`/`get_str_array`/`get_object`/`get_array_element`/`escape`) directly, since the mock Stratum tests already exercise `handle_line`'s dispatch logic with valid messages — this harness's scope is specifically "hostile bytes crash the parser module."
- **New opt-in CMake option `ARMRX_BUILD_FUZZERS`** (default `OFF`, Clang-only — `-fsanitize=fuzzer` isn't supported by the GCC toolchain the rest of the project builds with). Configuring with it enabled under the default GCC compiler fails cleanly at configure time with a `FATAL_ERROR` explaining the Clang requirement. Compiles `src/json.cpp` directly into the fuzz binary rather than linking `armrx_core`, sidestepping any question of GCC/Clang object compatibility (that module has no other project dependencies).
- **Result: two clean fuzzing passes (942K + 1.57M executions, 61s + 121s), zero crashes, zero ASAN findings.** Given that, PLAN.md §3.1's second mitigation option (rewriting the parser as a hardened SAX parser) is not pursued for now — the fuzzing evidence doesn't currently justify it.
- Verified: default GCC build (`ARMRX_BUILD_FUZZERS=OFF`) unaffected — local `ctest` 4/4 unchanged. `-DCMAKE_CXX_COMPILER=clang++ -DARMRX_BUILD_FUZZERS=ON` builds and runs `fuzz_json` cleanly, independent of `armrx_core`.
- **This completes PLAN.md Phase 2** (all 5 tasks now done: `main.cpp` split, worker-thread dataset reuse, mock Stratum tests, JSON fuzzing, Argon2 NEON). Two of Phase 2's own correctness/coverage tasks each caught a critical pre-existing production bug along the way (fast-mode dataset corruption; `PoolManager` failover self-deadlock — see their respective postmortems). PLAN.md's Phase 3 (originally QEMU CI + Stratum V2 + generic JIT tuning) is deprioritized per direction and replaced with a narrower, code-verified next-steps plan: a fresh on-device performance re-baseline/branch-miss re-measurement, and two small constant-deduplication cleanups (AES round keys, scratchpad L3 mask) — same bug class as this session's two critical fixes. See PLAN.md §5 for the full replacement plan.

## 2026-07-22 — Critical Fix: PoolManager Failover Self-Deadlock; Mock Stratum Protocol Tests

See `docs/postmortems/pool-failover-deadlock-postmortem.md` for the full writeup. Summary:

- **Added mock Stratum protocol test suite** (`tests/test_pool_protocol.cpp`, PLAN.md §3.2): a loopback POSIX-socket mock server scripting 5 scenarios — Stratum V1 full flow (via AUTO's real CryptoNote-first-then-fallback negotiation), CryptoNote full flow, reconnect-backoff exhaustion, multi-pool failover, and malformed-input robustness. Zero networking test coverage existed before this.
- **Fixed a critical self-deadlock in `PoolManager::tick()`** (`src/pool_manager.cpp`): `tick()` held `stratum_mutex_` for its entire body and called `connect_to_current()` — which locks the same non-recursive mutex again — from inside that scope. Any real multi-pool failover event (a documented core feature: "automatic failover after 5 retries with a 2s cooldown") would permanently freeze the miner's pool-management loop the moment it tried to reconnect to the next pool. Found because `test_pool_failover()` hung indefinitely on first run; fixed by deferring the `connect_to_current()` call until after `tick()`'s lock is released.
- **Two additional findings, documented but not fixed (out of this test-writing task's scope):** (1) a pool unreachable from process startup (vs. one that connects then drops) never triggers failover at all, since `reconnect_loop()` is only armed by a connection that was previously up going down; (2) `connect_to_current()`'s replacement of the old `StratumClient` can block for up to ~30s more (beyond the already-real ~31s backoff) joining a reconnect thread mid-sleep for a doomed retry — invisible on a fast x86_64 sandbox, but directly surfaced by the on-device run's tighter timing via a real test failure (not a hang), and accommodated by widening the test's own timeout budget.
- Verified: x86_64 local `ctest` 4/4. AArch64 on-device full `ctest` 7/7, including `test_pool_protocol`'s 5 scenarios (64.6s).

## 2026-07-21 — Critical Fix: Fast-Mode Dataset Corruption, Plus Test-Suite Assertion and Build Fixes

See `docs/postmortems/fast-mode-dataset-corruption-postmortem.md` for the full writeup. Summary:

- **Fixed silent fast-mode dataset corruption** (`src/mining_engine.cpp`): `MiningEngine::set_job()`'s multi-threaded dataset build (both the pre-existing temp-thread fallback and the new §2.1 worker-reuse path) called `initialize_dataset()` with the full dataset buffer regardless of each thread's `start_item`, when `initialize_dataset()`'s contract is to write relative to `output[0]`. Every thread past the first overwrote the same starting bytes instead of its own region, leaving most of any fast-mode dataset built with more than one thread zero-filled — meaning fast-mode hashes have been wrong whenever more than one thread participated in a dataset build, which is the normal case. Fixed both call sites to pass the correct per-thread sub-span. Predates this session; found only because a new correctness test (added for §2.1, see below) happened to cross-check partitioned output against a reference for the first time.
- **Fixed `assert()` being silently compiled out under the default Release build** (`CMakeLists.txt`): `-DNDEBUG` (set by CMake's default `Release` build type) strips `assert()` entirely. `tests/test_blake2b.cpp` (the RandomX KAT suite) and `tests/test_mining.cpp` both rely on plain `assert()`, meaning every "tests passed" result from the standard, documented build workflow had not actually been checking those assertions. Added `-UNDEBUG` to the `armrx_tests` and `test_mining` CMake targets specifically (not the shipped `armrx`/`armrx_core`) to force real checking. Verified with a positive control (a deliberately-broken assertion now genuinely aborts).
- **Test-suite memory-awareness for constrained hardware** (`tests/test_mining.cpp`): the new §2.1 correctness tests originally forced `RandomXMode::fast` directly via `MiningEngine`'s constructor, bypassing the memory-availability check `MinerApp::run()` normally applies before ever attempting fast mode. On the ~1.8 GiB on-device Cortex-A53 devbox (which the miner's own `--mode=fast` check already refuses — "requires 2338 MiB", "Available memory: 1538 MiB"), this reliably killed the test process. Added `fast_mode_fits_on_this_host()` (reusing `choose_randomx_mode()` from `include/armrx/memory.hpp`, the same helper the CLI path uses) to skip — not fail — the two fast-mode tests when they won't fit, with a visible `SKIPPED` message. Also replaced the correctness check's independent second full (~2080 MiB) reference-dataset build with a comparison against a light-mode `VirtualMachine` (RandomX guarantees light-mode on-the-fly item generation equals the fast-mode materialized value for the same seed) — cheaper and a strictly stronger assertion.
- **Documented the GCC 15 + musl + LTO `armrx` link failure** triggered by the `main.cpp` → `cli_parser.cpp`/`miner_app.cpp` split (§1.3): reproduces on-device only (Alpine/musl GCC 15.2.0), not on the x86_64 dev sandbox. Same class of fragility as the previously-documented Newton-Raphson `x29` corruption bug; same existing workaround (`-DARMRX_DISABLE_LTO=ON`) resolves it. Not changed as a project-wide default — used explicitly for this session's on-device validation.
- **Verified end-to-end on real AArch64 hardware** (device at 192.168.10.156, built with `-DARMRX_DISABLE_LTO=ON`): full `ctest` 6/6 passing — `armrx_tests` 15.5s, `test_mining` 17.6s (fast-mode tests correctly skipped), `bench_armrx` 570.3s, `bench_opcodes` 354.0s, `test_jit_encodings` 88.9s, `test_jit_determinism` 18.2s. Also verified on x86_64 (31 GiB RAM, fast-mode tests fully exercised, not skipped): local `ctest` 3/3.

## 2026-07-21 — Phase 2: Argon2 NEON Enabled, main.cpp Split into CommandLineParser/MinerApp

- **Enabled Argon2d NEON permutation** (`src/argon2.cpp`, `tests/bench_armrx.cpp`):
  - Added a permanent `argon2_compress` micro-benchmark (bench section "3b") since the suite had no timing coverage for `Argon2dCache::initialize()`'s hot path.
  - Benchmarked scalar vs NEON `permute_block` on-device (Cortex-A53, pinned core), reproduced twice: NEON is ~16% faster per compress (11.87 μs → 9.95 μs; 84,218 → 100,533 compress/s).
  - Flipped the guard that was accidentally disabling `permute_block_neon` (`#if 0 // defined(__aarch64__) && defined(__ARM_NEON)` → the real `#if defined(...)`), permanently enabling it on AArch64+NEON builds. x86_64/non-NEON builds unaffected (still take the scalar `#else` path).
  - Correctness: `armrx_tests`' KAT suite is itself the regression test here, since `Argon2dCache::initialize()` (~786k `permute_block` calls for the default config) determines every hash output. Passed 6/6 on-device ctest with NEON enabled.
- **Split `main.cpp` into `CommandLineParser` + `MinerApp`** (`include/armrx/cli_parser.hpp`, `src/cli_parser.cpp`, `include/armrx/miner_app.hpp`, `src/miner_app.cpp`, `src/main.cpp`, `CMakeLists.txt`):
  - `CommandLineParser::parse()` resolves config-file defaults + CLI overrides into a `MinerOptions` struct, handling `--help`/`--version`/invalid-arg as an early-exit result instead of `main()` doing it inline.
  - `MinerApp` owns SIGINT/TERM handling and the four run modes (init-cache, JIT dump, local benchmark, pool mining) as private methods driven from `run()`.
  - `main.cpp` shrank from 800+ lines to 10.
  - Caught mid-refactor: the first draft lost the `--pool`/`--wallet` validation's exit code 64 (collapsed into a `return;` inside a `void` method that fell through to the default `cpu.aarch64 ? 0 : 2`). Fixed by moving that validation back into `run()`, mirroring the existing fast-mode-memory-check pattern.
  - Verified: local build clean; `--help`/`--version`/invalid-arg output diffed byte-identical against the pre-refactor binary; all early-return exit codes re-checked manually. 3/3 local ctest, 6/6 on-device ctest (native AArch64 build, `--version` confirms `AArch64 JIT: enabled`).

## 2026-07-21 — PLAN.md Verification Pass and Phase 1 Fixes

- **Verified `PLAN.md` against current HEAD and corrected stale items**:
  - §2.3 "JIT Memory Page Recycling" rested on a false premise — `allocMemoryPages` is called once per worker thread (`JitCompilerA64` ctor, via `VirtualMachine`), not per JIT compile. Marked "investigated, not an issue" instead of left as an open task.
  - §4.2 "Unified Compilation Flag Invariants" cited a crash caused by `ARMRX_JIT_FAST_DIV_SQRT` being `PUBLIC`; that flag was already changed to `PRIVATE` (commit `b814c17e`, see `docs/audits/jit-buffer-size-audit.md`). Downgraded from a Phase 1 safety fix to an opportunistic cleanup.
  - §3.3 "Windows Privilege Least-Privilege Alignment" — the project has no Windows build support anywhere (`_WIN32`/`_MSC_VER`/`__CYGWIN__` only appear in `virtual_memory.c`, inherited from upstream RandomX), and the caller (`MappedMemory`) already degrades gracefully on `NULL`. Reclassified from "security fix" to "dead code."
  - §1.1/§1.2 tightened with exact current-code details (both stratum nonce call sites; the precise remaining gap in `MetricsExporter`'s thread lifecycle).
  - Rewrote §5's phase roadmap so tasks are grouped by actual effort/risk instead of by original topic area.
- **Implemented Phase 1 (quick, low-risk fixes)**:
  - `include/armrx/metrics.hpp`: removed `thread_.detach()`; destructor now calls `thread_.join()` after `shutdown()`, closing the exit-time use-after-free window where the detached socket thread could run past `MetricsExporter`/`main()` teardown.
  - `include/armrx/stratum_client.hpp`, `src/stratum_client.cpp`: added `nonce_offset_`/`nonce_size_` members (Monero defaults 39/4) and a `set_nonce_config()` setter; both hardcoded call sites (`handle_notify` for Stratum V1, `process_cryptonote_job` for CryptoNote) now read from one source of truth.
  - `src/virtual_memory.c`: added a header comment marking the `_WIN32`/`__CYGWIN__` branches as vestigial/unreachable in this Linux-only project, rather than surgically deleting them out of a file that interleaves Windows/Apple/BSD paths in the same functions.
  - Verified on x86_64 dev sandbox: `cmake --build build -j` clean, `ctest --test-dir build --output-on-failure` 3/3 passing (`armrx_tests`, `test_mining`, `bench_armrx`) — interpreted-only build, JIT excluded.
  - Verified on real AArch64 hardware (device at 192.168.10.156, via direct SSH since the devbox MCP tools weren't wired into this session): native `cmake --build` clean, `ctest` 6/6 passing, including the JIT-only `bench_opcodes`/`test_jit_encodings`/`test_jit_determinism` that don't build on x86_64.

## 2026-07-21 — JIT Buffer Overflow Resolution and Newton-Raphson Evaluation

- **Conducted JIT Safety Audit & Expanded JIT Buffer** (`src/jit_compiler_a64_static.S`, `docs/audits/jit-buffer-size-audit.md`):
  - Audited code sizes, showing that the 19,045-byte estimate was a cumulative count of 8 chained programs combined.
  - Proved that a single program occupies ~2,380 bytes, utilizing only 14.5% of the original 16,384-byte buffer.
  - Calculated that the worst-case program size is strictly under 13.3 KB, meaning the original buffer was already 100% safe.
  - Retained the expanded 32,768-byte buffer size as a defense-in-depth security measure.
- **Evaluated Fast Newton-Raphson JIT Math** (`CMakeLists.txt`):
  - Validated fast Newton-Raphson `FDIV_M`/`FSQRT_R` (ARMRX_ENABLE_JIT_FAST_DIV_SQRT), passing 100% of all correctness and determinism tests in CTest.
  - Measured single-thread performance: Newton-Raphson math yielded **5.12 H/s** compared to **5.18 H/s** for native hardware division (a ~1.1% hashrate decrease) due to FPU pipeline pressure. Kept OFF by default.
- **Updated Project Master Plan** (`PLAN.md`):
  - Re-wrote the master update and improvement plan, detailing architectural refactoring (joinable socket threads, generalized nonces), performance paths, mock stratum testing, and a phased execution roadmap.

## 2026-07-21 — Architectural Refactoring, Steady-State Benchmarking, and Worker-Count Sweep

- **Architectural Cleanup & Security Scoping** (`src/jit_compiler_a64.cpp`, `include/armrx/jit_compiler_a64.hpp`, `CMakeLists.txt`, `include/armrx/metrics.hpp`):
  - Deduplicated JIT loop setup by extracting `emitPrologueMix` and `emitSpMix2` to reduce JIT function body duplication by ~70%.
  - Encapsulated executable page references by removing the unused public `getCode()` accessor.
  - Restricted compilation scope of `ARMRX_JIT_FAST_DIV_SQRT` to `PRIVATE` in `CMakeLists.txt`.
  - Refactored `MetricsExporter` to output loopback metrics via the structured logger instead of direct raw calls to `std::cerr`.
- **Steady-State Benchmarking & Warmup Logic** (`src/main.cpp`, `src/mining_engine.cpp`, `include/armrx/mining_engine.hpp`):
  - Implemented `--warmup=<seconds>` option to specify a dataset initialization / JIT stabilization window.
  - Implemented `snapshot()` method in `MiningEngine` to atomically capture per-worker total hashes and timestamps.
  - Calculated post-warmup steady-state total and per-worker hashrates between two snapshot points to eliminate startup bias.
- **Worker-Count Sweep Benchmark** (`tools/sweep_workers.py`):
  - Swept configurations from 4 to 8 workers on the Snapdragon 410 (8× Cortex-A53 CPU, 4 big cores cluster + 4 little cores cluster).
  - Executed 180s runs repeated 3 times with 40s warmups.
  - Excluded contaminated first run of Config C caused by background sweeps, confirming that:
    - Big cores run at exactly **4.11 H/s/thread** completely independent of active thread counts.
    - Little cores run at exactly **2.28 H/s/thread** up to 3 threads, with minor scheduler/frequency drop to **1.83 H/s/thread** under full 8-worker saturation.
    - Contention is minimal, and total hashrate scales monotonically from **16.45 H/s** (4 big) to **25.28 H/s** (4 big + 4 little, pinned).
    - Sequential pinning (`A_pinned`) outperforms unpinned OS-scheduled mode (`A_unpin`) with higher mean and lower variance.

## 2026-07-21 — Software AES Inlining and big.LITTLE Scheduling

- **Optimized Software AES Path by Inlining and Register Passing** (`include/armrx/aes.hpp`, `src/aes.cpp`):
  - Moved the definitions of `encrypt_transform`/`decrypt_transform` and `aes_encrypt_round`/`aes_decrypt_round` into `include/armrx/aes.hpp` as `inline` functions.
  - Changed parameters to pass `AesBlock` by value. Because `AesBlock` is exactly 16 bytes, the compiler now passes it directly in AArch64 registers (`x0`/`x1` or `q0`), completely eliminating memory overlap checks, dynamic stack frames, and over 1 million calls to `memcpy`/`memmove` via the PLT per 2MB scratchpad.
  - Removed `src/aes.cpp` and updated `CMakeLists.txt`.
  - **Verification Results** (on big core CPU 0):
    - `fill_aes_1r_x4` (init_scratchpad): improved from **16.8 ms to 14.1 ms** (**+19.3% faster**).
    - `hash_aes_1r_x4` (get_final_result): improved from **17.0 ms to 14.7 ms** (**+16.0% faster**).
    - Overall single-thread hashrate: raised from **4.45 H/s to 4.55 H/s** (**+2.25% speedup**).

- **Implemented big.LITTLE-Aware Worker Scheduling** (`src/mining_engine.cpp`, `src/main.cpp`):
  - Added `--affinity-mode=all|unpinned|big-only` flag and config setting to customize thread pinning.
  - Confirmed core topology: cores 0-3 are big cores (`cpu@100-103`), cores 4-7 are LITTLE cores (`cpu@0-3`).
  - **Benchmarked Scheduling Policies** (on 8× Cortex-A53 SoC, 20-second mine run):
    - **Policy A (pinned 8 threads):** Pinned 4 on big cores, 4 on LITTLE cores sequentially. Achieved **25.35 H/s** (507 hashes).
    - **Policy B (unpinned 8 threads):** Threads freely scheduled by OS. Achieved **25.55 H/s** (511 hashes) - best total hashrate.
    - **Policy C (big-cores-only 4 threads):** Ran 4 workers pinned to big cores 0-3. Achieved **17.00 H/s** (340 hashes).
    - **Contention Findings:** Worker efficiency under Policy C was **4.25 H/s/worker**, but dropped to **3.18 H/s/worker** under Policy A/B (a **25% reduction** due to memory bus contention during shared 256 MiB dataset access). However, total H/s is still 50% higher with all 8 threads.

## 2026-07-21 — Optimize instruction scheduling and JIT FP loads

- **Unblocked Profile-Guided Optimization (PGO)** (`CMakeLists.txt`):
  - Fixed a CMake bug where PGO compile and link options (`-fprofile-generate`/`-fprofile-use`) were `PRIVATE` to `armrx_core`, causing dependent executables to miss gcov symbol linkage and fail with ld SEGSEGV. Propagated them as `PUBLIC`.
  - Fixed a JIT test configuration bug: JIT-only tests/benchmarks (`bench_opcodes`, `test_jit_encodings`, `test_jit_determinism`) were conditionally wrapped in `if(ARMRX_HAVE_JIT)`, but `ARMRX_HAVE_JIT` was only defined as a compiler preprocessor macro and not a CMake variable. Explicitly set `ARMRX_HAVE_JIT` as a CMake variable on AArch64 systems.
  - Verification results (on big core CPU 0):
    - Successfully compiled, linked, and validated all 6 tests with PGO USE.
    - Saved **6.4 billion instructions** (7.1% reduction) and **10.7 billion cycles** (9.1% reduction) on the region-attribution benchmark.
    - Increased JIT pipeline efficiency with IPC rising from **0.7676 to 0.7846** (+2.2%).
    - Sped up `generate_dataset_item` by **10.6%**, `initialize_dataset` by **7.6%**, and interpreted mode by **8.7%**.
    - Raised overall JIT hashrate to **4.45 H/s** (median 224,922 μs).
    - All KATs passing 100%.

- **Interleaved FP loads and conversions in main loop** (`src/jit_compiler_a64_static.S`): Reordered prologue instructions to hide the 3-cycle load-use penalties of `ldp`/`ldr` and the 5-7 cycle latencies of the `sshll`/`scvtf` pipelines.
- **Implemented register-offset FP loads in JIT compiler** (`src/jit_compiler_a64.cpp`): Replaced the serial `add x19, x2, x19` + `ld1 {v.2s}, [x19]` instruction pair with a single register-offset load `ldr d<tmp_reg_fp>, [x2, x19]`. This directly eliminated 1 instruction from every memory load FP operation and removed serialization stalls.
- **Verification Results**:
  - Saved **56 million instructions** and **439 million cycles** on the standard region-attribution benchmark.
  - Improved JIT execution loop (chain) hashrate by **1.7%** (from 169.8 ms down to 166.9 ms per hash).
  - Raised overall hashrate from **4.41 H/s to 4.43 H/s** (median 226,651 μs -> 225,911 μs).
  - Verified 100% correct and deterministic execution against all KATs.

## 2026-07-20 — Fix hash divergence: correct AES T-table transforms

### Root cause: two bugs in software AES implementation

**Bug 1: Wrong byte order and column permutation in encrypt_transform**
`src/aes.cpp`: The AES encrypt T-table lookup used reversed byte order
(MSB-first instead of LSB-first) within each 32-bit word, AND used a
wrong column permutation pattern. This caused ALL AES encryption operations
to produce incorrect output. The FIPS-197 KAT in test_blake2b.cpp was
circular (expected value derived from the buggy code).

Fix: Correct byte order (LSB-first) and column permutation to match the
standard SubBytes→ShiftRows→MixColumns→AddRoundKey sequence.

**Bug 2: Wrong column permutation in decrypt_transform (different from encrypt)**
`src/aes.cpp`: The AES decrypt T-table lookup used the SAME column permutation
as encrypt, but the upstream RandomX soft_aesdec uses a DIFFERENT permutation
(a straight sequential rotation: s0,s1,s2,s3 → s1,s2,s3,s0 → etc.).

Fix: Use the correct decryption-specific column permutation, matching the
upstream's soft_aesdec.

**Bug 3: Incorrect NEON hardware AES path**
`src/aes_hash.cpp`: The ARM NEON `AESE`/`AESD` instructions implement a
different operation order than the RandomX AES round specification.
`AESE` applies AddRoundKey at the START (before SubBytes), while the
standard applies it at the END (after MixColumns). `AESD` has a similar
ordering reversal for decrypt. This caused all four AES-hash functions
(fill_aes_1r_x4, fill_aes_4r_x4, hash_aes_1r_x4, hash_and_fill_aes_1r_x4)
to produce incorrect results on AArch64.

Fix: Removed all `#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)`
NEON hardware paths from aes_hash.cpp. All AES operations now go through
the software T-table path, which correctly implements the standard AES
round order.

**Files changed:** src/aes.cpp, src/aes_hash.cpp, tests/test_blake2b.cpp, CMakeLists.txt

### Post-fix benchmark re-baseline
- **Hashrate dropped from 5.18 to 4.34 H/s** (−16.4%). Root cause confirmed by region attribution: the NEON AES removal changed init_scratchpad from 589 μs → 17,554 μs (29.8×) and get_final_result from 1,023 μs → 17,974 μs (17.6×). Chain execution (VM JIT) barely changed (+2%).
- **NEON AES encrypt re-enable attempted** — AESE+AESMC added for encrypt operations. Benchmarked as zero benefit on Cortex-A53 (17,549 μs vs 17,554 μs — within noise). Per-block NEON load/store overhead cancels AES instruction speedup on this core. Reverted.
- **Branch-miss profile** (perf record -e branch-misses): **94.85%** of branch misses are in `execute_superscalar` (dataset generation, runs per-job, not per-hash). The JIT CBRANCH is invisible to perf (JIT buffer never registered) and the `bne+b` fix handles it. CBRANCH is confirmed **not the bottleneck** on the hash path.
- **33% instruction-count gap vs XMRig is now stale** — measured with buggy AES. Needs fresh comparison.
- **KATs verified** on-device (armrx_tests: 15.95s on Cortex-A53).
- **Build fixes**: removed dead `debug_hash` CMake target (file deleted in ed512e5 but CMake left behind); added `ARMRX_DISABLE_LTO` option (GCC 15 + musl LTO crash with fortified vsnprintf).

### Cleanup
- Stashed debug tracing code in `scratch_vm_study/upstream_rx` submodule
- Archived old `plan.md` → `docs/archived/plan_v1.md` (superseded by PLAN.md)
- Removed stale `#include <arm_neon.h>` from `src/aes_hash.cpp`
- Updated AGENTS.md with devbox MCP commands, AES fix status

## 2026-07-19 (Benchmark protocol v2 — region attribution & PMU baseline)

### Measurement foundation (Stage 1)

- **`tests/bench_armrx.cpp`**: Complete rewrite — benchmark protocol v2.
  - **Proper statistical reporting**: Median, min, max, σ%, sample count per benchmark.
  - **Deterministic random index sequences**: Precomputed via fixed-seed `mt19937` for `load_cache_line` and `generate_dataset_item` — no more fixed cache-line-42 trap.
  - **Honest benchmark sizing**: `fill_aes_1r_x4` now actually benchmarks full 2 MiB scratchpad fill, not 64 bytes.
  - **Region attribution**: Manual replication of the `randomx_calculate_hash` pipeline with per-phase timing (blake2b input, init_scratchpad, chain loop, final run, get_final_result).
  - **JIT compile vs execute separation**: When built with `-DARMRX_JIT_PROFILE=ON`, reports per-program compile/execute times.
  - **Full hash throughput**: 500-sample percentile distribution (P0/P1/P5/P25/P50/P75/P95/P99/max/mean).
  - **CLI flags**: `--attribution-only`, `--full-hash-only`, `--micro-only` for focused `perf stat` runs.
  - **Interpreted comparison**: Reports JIT speedup factor.

### Key findings (AArch64 Cortex-A53, light mode, JIT)

| Metric | Value |
|--------|-------|
| Single-thread hashrate | **5.18 H/s** (192,909 μs/hash median, σ=1.4%) |
| JIT compile (% of hash) | **1.76%** (3,361 μs/hash) |
| JIT execute (% of hash) | **98.24%** (187,530 μs/hash) |
| IPC | **0.708** |
| Branch miss rate | **34.42%** |
| JIT speedup over interpreted | **12.85×** |

The dominant bottleneck is **JIT execution** (generated VM code), not JIT compilation or AES/Blake2b helpers. The 34.42% branch miss rate is inherent to RandomX's unpredictable CBRANCH — this is the #1 cycle sink on in-order Cortex-A53.

### TUI redesign (Phase U1)
- **`TuiSnapshot` struct** (`include/armrx/tui.hpp`): Replaced the 11-parameter `render()` function with a `const TuiSnapshot&` value type. Status enum replaces raw ANSI strings. Adding a new field is now a 2-site edit instead of 4.
- **Injectable output stream** (`include/armrx/tui.hpp`, `src/tui.cpp`): `render()` takes an optional `std::ostream&` (default `std::cout`). Enables unit testing by passing a `std::ostringstream`.
- **Terminal-width awareness** (`src/tui.cpp`): Queries `ioctl(TIOCGWINSZ)` each frame. Truncates pool name with ellipsis, scales bar width to terminal columns. Fixes scrollback corruption bug (U2) — no line wrapping means `prev_lines_` cursor math stays correct.
- **`NO_COLOR` policy** (`src/tui.cpp`, `src/main.cpp`): Honors `NO_COLOR` env var and `TERM=dumb`. `--no-color` / `--color` CLI flags force override. All ANSI escape codes gated behind `use_color_` flag. Cursor hide/show also gated.
- **Worker-bar EMA baseline** (`src/tui.cpp`): Added `bar_baseline_ema_` with `kEmaAlpha=0.2` (~5s time constant) to smooth per-frame bar jitter.
- **`atexit` cursor restore** (`src/tui.cpp`): Registers `atexit(atexit_show_cursor)` in constructor — async-signal-safe `write()` call ensures cursor is restored even if `SIGTERM` kills the process before `~Tui()` runs.

### Pool share tracking (U3.1)
- **Share accept/reject counters** (`include/armrx/stratum_client.hpp`, `src/stratum_client.cpp`, `src/pool_manager.cpp`, `src/main.cpp`): `StratumClient` now tracks `shares_accepted_` and `shares_rejected_` atomically, incremented in `handle_reply` when the pool responds. `PoolManager` exposes them with mutex guard. `TuiSnapshot` populated with real accept/reject counts. TUI now shows "Shares: N (acc: M rej: K)" instead of just total submitted.

### CLI consolidation (U3.5, U3.7)
- **`--version` flag** (`src/main.cpp`, `CMakeLists.txt`): Prints version (`0.2.0`), git SHA, build date, JIT/TLS capability lines. Version info embedded via `.git_sha` file written by sync process.
- **Dead code deleted** (`src/config.cpp`, `include/armrx/config.hpp`): Removed `apply_cli_overrides()` function — 31 lines of dead code not called since `PoolManager` extraction. CLI parsing lives only in `main.cpp`.

### Prometheus metrics endpoint
- **`include/armrx/metrics.hpp`**: Header-only `MetricsExporter` class. Listens on `localhost:port`, serves `GET /metrics` in Prometheus text format. Exports hashrate, total hashes, shares (submitted/accepted/rejected), pool connection status, and optional JIT profile counters. No external dependencies — pure POSIX sockets. Detached background thread, clean shutdown via atomic flag.
- **`--metrics-port=N` flag** (`src/main.cpp`): Enables the endpoint in both benchmark and pool mining modes. Binds to loopback only for security.

### Documentation
- **Created `docs/beyond-parity.md`**: Outlined the roadmap to go beyond XMRig performance parity on AArch64. Key pillars include worker thread phase staggering, Newton-Raphson division/sqrt JIT debugging/simplification, Superscalar JIT instruction scheduling, and test suite expansion.

### Structured logger
- **`include/armrx/log.hpp`**: New header-only leveled logging module (`trace`/`debug`/`info`/`warn`/`error`) with mutex-guarded sink, TUI-mode ring buffer, and zero-overhead gating macros (`ARMRX_LOG_INFO`, `ARMRX_LOG_WARN`, etc.). Writes to stdout for info/debug, stderr for warn/error. In TUI mode, suppresses console output and routes messages to a 256-entry ring buffer the TUI can read.
- **Replaced all raw `std::cerr`/`std::cout` in cross-thread log sites** (`src/stratum_client.cpp`, `src/pool_manager.cpp`, `src/tls_client.cpp`, `src/mining_engine.cpp`): These were racing with the main-thread TUI writes, causing layout corruption under `--tui`. Protocol dump messages (`>>`/`<<`) demoted to DEBUG level.

### Per-hash hot-path reductions (P2.5)
- **Template copy eliminated from per-hash path** (`src/mining_engine.cpp`): Moved `block_input = local_job.block_template` (full block copy, ~76 bytes) from the per-hash worker loop into the job-change guard block. Only nonce bytes are patched per hash via `update_nonce_in_template()`.
- **Superscalar heap churn eliminated** (`src/superscalar.cpp`): Replaced heap-allocating `std::vector<int>` with stack-based `int[8]` + size counter in `selectDestination()` and `selectSource()`. These are called multiple times per SuperscalarHash program generation — eliminates dozens of malloc/free pairs during cache init and dataset item derivation.

### Memory tier upgrades

### JIT introspection tooling
- **`--jit-dump` flag** (`src/main.cpp`, `jit_compiler_a64.hpp/cpp`, `vm.hpp`): New CLI flag that compiles one RandomX program and dumps the emitted JIT code as hex with opcode boundary markers. Each instruction's (opcode, byte offset, emitted size) is recorded by instrumenting the dispatch loops in `generateProgram`/`generateProgramLight`. Opcode names are derived from frequency weights matching the `engine[256]` dispatch table.
- **`bench_opcodes` frequency analyzer** (`tests/bench_opcodes.cpp`): Runs N random seeds, collects per-opcode frequency and byte-cost histograms via the JIT dump API. First 20-seed run confirmed distribution matches expected weights. Key findings: FDIV_M (35.2 avg bytes, 1.5%), FADD_M/FSUB_M (~31 avg bytes, ~2% each), all high-frequency opcodes already at minimum 4 bytes on AArch64.
- **Per-opcode audit** completed: Most handlers are already optimal. The 33% instruction gap vs XMRig is distributed codegen (armrx CPI 1.22 vs XMRig 1.56 despite 33% more instructions). No single optimization target found.
- **CBRANCH encoding unit test** (`tests/test_jit_encodings.cpp`): Verifies all CBRANCH entries have valid emitted sizes across 5 random seeds.
- **JIT determinism test** (`tests/test_jit_determinism.cpp`): Same seed compiled twice produces byte-identical JIT dump and identical hash.

### Memory tier upgrades
- **Dataset (2 GiB)** (`include/armrx/mining_engine.hpp`): `MappedMemory` now calls `allocLargePagesMemory` (MAP_HUGETLB | MAP_POPULATE) first, falls back to plain mmap + MADV_HUGEPAGE. Eliminates THP dependency for the largest allocation.
- **Cache (256 MiB)** (`src/argon2.cpp`): Argon2dCache uses same try-allocLargePagesMemory-first pattern with fallback.
- **VM scratchpad (2 MiB per VM)** (`src/vm.cpp`): Same hugely-page pattern (exactly one 2 MiB huge page). Adds `MADV_POPULATE_WRITE` warmup (Linux 5.14+) to prefault pages and avoid cold-start TLB misses, with `memset` fallback for older kernels.
- **Virtual memory include** (`src/vm.cpp`, `src/argon2.cpp`): Added `#include "armrx/virtual_memory.h"` to access the `allocLargePagesMemory` function that was previously unused by all callers.

### Security
- **`read_buf_` cap at 1 MiB** (`src/stratum_client.cpp:391`): Prevents OOM from a malicious pool streaming data without newline terminators. Connection is dropped on overflow.
- **`setPagesRW`/`setPagesRX` return `int`** (`include/armrx/virtual_memory.h:40-41`, `src/virtual_memory.c:172-199`): mprotect errors are now propagated instead of silently swallowed. JIT call sites (`jit_compiler_a64.cpp:159-169`, `vm.cpp:163-166,799-807`) throw `std::runtime_error` on failure.
- **CLI numeric arg validation** (`src/main.cpp:136,156,161,176`): `std::stoul`/`std::stoull` calls now wrapped in `try`/`catch` with friendly error messages instead of `std::terminate`.
- **SIGTERM handler** (`src/main.cpp:60`): Added alongside the existing SIGINT handler for graceful shutdown.
- **`mining_engine` silent-swallow fix** (`src/mining_engine.cpp:297-302`, `include/armrx/mining_engine.hpp:100`): `update_nonce_in_template` now returns `bool`; call site logs the error and deactivates the worker on bad nonce offset.
- **`json::escape` control character coverage** (`src/json.cpp:43-57`): Now escapes all U+0000–U+001F characters via `\u00xx`, not just `\`, `"`, `\n`, `\r`, `\t`.

### Concurrency
- **`reconnect_attempts_` → `std::atomic<unsigned>`** (`include/armrx/stratum_client.hpp:183`): Eliminates torn reads when `PoolManager::tick` reads the counter from the main thread while the reconnect thread writes it.
- **`handshake_req_id_` / `authorize_req_id_` → `std::atomic<std::uint64_t>`** (`include/armrx/stratum_client.hpp:196-197`): Cross-thread reads from reader thread, writes from main thread during connect.
- **`subscribe_ok_` → `std::atomic<bool>`** (`include/armrx/stratum_client.hpp:177`): Reader thread writes, main thread reads.
- **`rx_set_rounding_mode` static cache → per-instance** (`include/armrx/vm.hpp:174`, `src/vm.cpp:69-73,735`): Moved the `last_mode` cache from a `static` variable (shared across all VMs/threads) to a `last_rounding_mode_` member of `VirtualMachine`.
- **`ARMRX_ENABLE_TSAN` CMake option** (`CMakeLists.txt:11,137-141`): Mirrors the existing ASan/UBSan options. Use `-DARMRX_ENABLE_TSAN=ON` for thread sanitizer builds.

### Maintainability
- **`vm.hpp` comments** (`include/armrx/vm.hpp:53-57,175`): Documented the `kRandOMXFlag*` value divergence from upstream RandomX (Jit=4 vs upstream FULL_MEM=4), and noted the `register_usage_` initializer is moot (`compile_program` `std::fill`s all 8 before use).
- **Stale JIT comment fix** (`src/jit_compiler_a64_static.S:273`): FDIV_M instruction count corrected from 12 to 17.
- **`ceil_*` constants deleted** (`src/vm.cpp:109-141`): Dead opcode-frequency ceiling constants that were unused after the dispatch-table refactor.
- **`allocate()` comment fixed** (`src/vm.cpp:179`): Corrected from stale `std::vector` to `mmap`.
- **`reg_.a` init gated** (`src/vm.cpp:199-225`): Skipped under JIT mode since `run_jit()` overwrites `reg_.a` with `config.eMask`.
- **`[DEBUG]` log removed** (`src/main.cpp:377-378`): Production noise removed now that `PoolManager::connect` handles connection logging.

### Security
- **TLS hostname verification** (`src/tls_client.cpp:59-67`): Added `X509_VERIFY_PARAM_set1_host()` call before `SSL_connect()`. Previously only SNI was set — any CA-signed cert for any domain would pass. `--tls` now authenticates the server.
- **`session_id_` escaped in submit and keepalive frames** (`src/stratum_client.cpp:315,683`): `session_id_` from pool login responses is now wrapped in `armrx::json::escape()`. This was a partial regression of the S1 JSON injection fix.

### Correctness
- **`stratum_` mutex** (`include/armrx/pool_manager.hpp`, `src/pool_manager.cpp`): Added `stratum_mutex_` guarding `PoolManager::stratum_` assignment and access. Worker threads calling `submit_share()` and main thread reassigning `stratum_` during failover no longer race.
- **`get_array_first` dead-code bug** (`src/json.cpp:126-131`): Removed first `if (json[pos] == '"')` branch which had a 512-byte truncation bug and rendered the correct second branch unreachable. Affected `mining.set_target`, `set_difficulty`, `set_extranonce` parsing.
- **`find_key` scope fix** (`src/json.cpp:64-77`): Now requires the character before a matched key to be `{` or `,` (object-key position). A pool returning `{"error":"\"method\" bad","method":"login"}` no longer returns garbage for `method`.

### Maintainability
- **`generateProgram`/`generateProgramLight` dedup** (`src/jit_compiler_a64.cpp`): Extracted the duplicated 24-line v2 AES-tweak block into a shared `emitV2AesTweak()` static member function. Both fast and light generation paths now share one implementation, eliminating a silent drift risk for v2-mode programs.

### Security
- **`session_id_` escaped in submit and keepalive frames** (`src/stratum_client.cpp:315,683`): `session_id_` from pool login responses is now wrapped in `armrx::json::escape()` before inclusion in submit and keepalive messages. This was a partial regression of the S1 JSON injection fix — `wallet_`, `password_`, and `job_id` were already protected.

### Correctness
- **`get_array_first` dead-code bug** (`src/json.cpp:126-131`): Removed the first `if (json[pos] == '"')` branch in `get_array_first` which had a 512-byte truncation bug and caused the correct second branch to be unreachable. The dead code made `json::get_array_first` silently truncate values near 512 bytes, affecting `mining.set_target`, `mining.set_difficulty`, and `mining.set_extranonce` parsing.

## 2026-07-17 (emit32 UB fix + hwloc pinning)

### Cleanup
- **emit32 UB fix** (`include/armrx/jit_compiler_a64.hpp`): Changed `*(uint32_t*)(code + codePos) = val` to `memcpy(code + codePos, &val, sizeof(val))`, matching the existing `emit64` pattern. Eliminates technically-UB unaligned uint32_t access.

### Infrastructure
- **hwloc-aware CPU pinning** (`CMakeLists.txt`, `src/mining_engine.cpp`): Added optional hwloc detection via `pkg-config`. `detect_core_order()` now uses `libhwloc` (v2.12.2) to enumerate processing units when available, falling back to the existing sysfs cpufreq sorting. Set `ARMRX_HAVE_HWLOC=1` in build flags. hwloc provides more accurate topology discovery on heterogeneous systems.

### Documentation
- **Peephole JIT plan** ([`docs/plans/peephole-jit-plan.md`](docs/plans/peephole-jit-plan.md)): Created detailed plan for Phase 3 — closing the 33% instruction-count gap vs XMRig. Covers 4 phases: tooling with opcode boundary markers and frequency histograms, per-opcode audit informed by frequency data, cross-opcode optimizations, and hashrate-vetoed validation. Incorporates review feedback: register allocation spot-check, CBRANCH encoding unit test, BTB aliasing caveat.
- **Branchless CBRANCH postmortem** ([`docs/experiments/branchless-cbranch.md`](docs/experiments/branchless-cbranch.md)): Updated with imm19 root cause, BTB aliasing caveat, and unit test recommendation. Superseded hypotheses marked for clarity.

### Performance
- **O11 — Branchless CBRANCH** (`src/jit_compiler_a64.cpp`): Replaced `beq target` (backward conditional branch, predicted TAKEN but only taken ~0.4%) with `bne .Lskip; b target` (forward `bne` predicted NOT-taken, correct 99.6%; unconditional `b` always correct). Root cause of first-attempt hang: `bne` offset was `imm19=1` but should be `imm19=2`. All KATs pass.

### Cleanup
- **`handle_notify` positional scanner replaced** (`src/stratum_client.cpp`): Removed the last hand-rolled JSON parser — a 40-line positional array scanner in `handle_notify`. Replaced with 4 calls to `armrx::json::get_array_element()`. Added `get_array_element(json, key, index)` to the JSON module (`include/armrx/json.hpp`, `src/json.cpp`) as a general-purpose Nth-element array extractor.
- **Flag constant de-duplication** (`src/jit_compiler_a64.cpp`): 4 constants (`RANDOMX_CACHE_ACCESSES`, `RANDOMX_SUPERSCALAR_LATENCY`, `RANDOMX_SCRATCHPAD_L3`, `CacheSize`) now alias `armrx::kRandomX*` project constants via `static_cast`. Documented the `RANDOMX_FLAG_*` to armrx flag mapping and the intentional difference between `RANDOMX_DATASET_BASE_SIZE` (2 GiB) and `kRandomXDatasetBytes` (2080 MiB).

## 2026-07-17 (PoolManager extraction + execute_bytecode)

### Architecture
- **PoolManager extraction** (`include/armrx/pool_manager.hpp`, `src/pool_manager.cpp`, `src/main.cpp`): Extracted inline pool failover, StratumClient lifecycle, and reconnect rotation logic from `main.cpp` into a dedicated `armrx::PoolManager` class. Replaces ~50 lines of inline lambdas and state variables with clean API (`connect()`, `tick()`, `submit_share()`, `current_pool_name()`).

### Performance (experimental)
- **execute_bytecode dispatch table** (`src/vm.cpp`): Attempted to replace the `switch(ibc.type)` (30-case, compiler-optimized) with an explicit `kExecHandlers[30]` function pointer table. **Reverted** — indirect function calls prevented compiler inlining on the hot path (~500K dispatches per hash), causing ~50% hashrate regression. The `switch` compiles to the same jump table but allows the compiler to inline across case boundaries, which is critical at this dispatch frequency.

## 2026-07-17 (NEON load interleaving + prefetch A/B test)

### Performance
- **NEON Argon2 G-function** (`src/argon2.cpp`): Vectorized the Argon2 `blamka_add` and `gb` (G-function) using AArch64 NEON `uint64x2_t` intrinsics. Process 2 G-functions in parallel per SIMD iteration.
- **O9 — NEON direct FP scratchpad loads** (`src/jit_compiler_a64_static.S`): Replaced `ldpsw` + 2×`ins` + `scvtf` (4 instructions with GPR→NEON cross-pipe stalls) with `ldr dN` + `sshll` + `scvtf` (3 instructions, all NEON). Eliminates the GPR round-trip bottleneck for group F/E register loads at the start of each JIT iteration. Reduced KAT test time by ~31% on Cortex-A53.
- **O10 — Prefetch hint tuning** (`src/jit_compiler_a64_static.S`): Adjusted dataset cache line and scratchpad prefetch hints (`pldl1keep` vs `pldl2keep`, `pldl1strm`) based on empirical testing.
- **O11 — Branchless CBRANCH research** (`docs/experiments/branchless-cbranch.md`): New file documenting the CBRANCH misprediction analysis.
- **PoolManager extraction** (`include/armrx/pool_manager.hpp`, `src/pool_manager.cpp`, `src/main.cpp`): Extracted inline pool failover, StratumClient lifecycle, and reconnect rotation logic from `main.cpp` into a dedicated `armrx::PoolManager` class. Replaces ~50 lines of inline lambdas and state variables with clean API (`connect()`, `tick()`, `submit_share()`, `current_pool_name()`).

### Security
- **S6 — TLS peer verification enabled by default** (`src/tls_client.cpp`, `include/armrx/tls_client.hpp`): Changed default certificate verification from `SSL_VERIFY_NONE` to `SSL_VERIFY_PEER` and set minimum TLS protocol to 1.2. Added `--no-verify-tls` CLI flag as opt-out for pools using self-signed certificates.

### Cleanup
- **Dead code removal** (`include/armrx/jit_compiler.hpp`): Removed `CodeBuffer` (~60 lines) and `CompilerState` structs — never referenced anywhere in the codebase. These were upstream RandomX scaffolding unused by the aarch64 JIT compiler.
- **`const_cast` abuse eliminated** (`include/armrx/stratum_client.hpp`, `src/stratum_client.cpp`): Marked `request_id_`, `handshake_req_id_`, `authorize_req_id_`, and `subscribe_try_` as `mutable`. Removed all 8 `const_cast<StratumClient*>(this)->` expressions from the message builder methods.

### Fixes
- **JSON `id` field parsing** (`src/stratum_client.cpp`): Changed `get_string(line, "id")` to `get_raw(line, "id")` in `handle_reply`. The `"id"` field in Stratum JSON is numeric (`"id":1`), not quoted, so `get_string` returned empty and the handshake response was never matched to `handshake_req_id_`. Caused "handshake timed out" even after successful login.
- **Race condition on protocol fallback** (`src/stratum_client.cpp`): Set `handshake_in_progress_` flag in `connect()` via RAII guard. Reader thread now only spawns a reconnect thread when the handshake is not in progress, preventing concurrent `connect()` calls from racing on `sockfd_`.

### Verification
- CTest: 100% passed (2/2).
- Pool connection to tr.monero.herominers.com verified working: login → job dispatch → mining at ~24 H/s.

## 2026-07-16 (Phase 2 — VM refactor + JSON module)

### Architecture
- **`armrx::json` module** (`include/armrx/json.hpp`, `src/json.cpp`): Extracted a shared JSON utility module consolidating 4 hand-rolled parser functions from `stratum_client.cpp` (`json_get`, `json_get_array_first`, `json_rpc`) and 4 from `config.cpp` (`json_str`, `json_bool`, `json_num`, `json_str_array`) into a single `armrx::json` namespace with 7 public functions: `escape`, `get_string`, `get_raw`, `get_array_first`, `get_str_array`, `get_object`, `rpc_envelope`. The new module fixes the `\\"` escape handling bug present in both old parsers (which only checked `json[i-1] != '\\'` instead of counting consecutive backslashes).
- **`is_fast_mode()` helper** (`include/armrx/vm.hpp`, `src/vm.cpp`): Added single source of truth `(flags_ & kRandOMXFlagFullMem)` — replaces the dual check (`flags_` in interpreter, `dataset_.empty()` in JIT). Applied consistently in `dataset_read()` and `run_jit()`.
- **`run()` split** (`include/armrx/vm.hpp`, `src/vm.cpp`): `run()` now dispatches to `run_jit()` (AArch64 JIT path, gated by `#ifdef ARMRX_HAVE_JIT`) or `run_interpreted()` (bytecode interpreter). Shared setup remains in `run()`.
- **Dispatch table for instruction compilation** (`include/armrx/vm.hpp`, `src/vm.cpp`): Replaced the 392-line, 24-block cascading `if (opcode < ceil_X)` ladder with a `kCompileHandlers[256]` table of member function pointers. Each opcode maps to one of 30 handler methods (h_IADD_RS through h_NOP). Two static helpers (`compile_mem_op`, `compile_alu_reg`) eliminate the 6× duplicated memory-op pattern.

### Verification
- CTest: all KATs pass (armrx_tests 16s, test_mining 11s) on real AArch64 hardware.
- Both JIT and interpreted execution paths verified with identical hash outputs.

### Performance
- **O1 — `alignas(16)` on RegisterFile** (`include/armrx/vm.hpp`): Added 16-byte alignment to the RegisterFile struct. Eliminates unaligned copies in the blake2b hot path.
- **O3 — Rounding mode cache** (`src/vm.cpp`): `rx_set_rounding_mode` now caches the last rounding mode in a `static` variable and skips the `fesetround` syscall when unchanged. CFROUND frequency is 1/256, so ~99.6% of calls are no-ops.
- **O7 — T-table AES fallback** (`src/aes.cpp`): Replaced the runtime `gf_inverse` → `sbox` → `gf_multiply` AES SubBytes+ShiftRows+MixColumns implementation with precomputed T-table lookups (`randomx_aes_lut_enc[4][256]` / `randomx_aes_lut_dec[4][256]` from `soft_aes.cpp`). Reduces software AES latency from ~hundreds of GF(2^8) ops to 16 table lookups per round.

### Build System & Tooling
- **bench_armrx registered in CTest** (`CMakeLists.txt`): The benchmark is now discoverable via `ctest --test-dir build`, preventing silent JIT regressions.
- **ASan/UBSan CMake options** (`CMakeLists.txt`): Added `-DARMRX_ENABLE_ASAN=ON` and `-DARMRX_ENABLE_UBSAN=ON` for sanitizer builds. S3 OOB check can now be confirmed with ASan.
- **`.clang-format` / `.clang-tidy` baseline**: Added project-wide formatting and linting configurations. No CI integration yet.

### Verification
- CTest: 100% passed (3/3) — armrx_tests + test_mining + bench_armrx.
- Benchmark: stable at ~4 H/s (light JIT, single-thread) — no regression from AES rewrite.

## 2026-07-16 (Phase 0 — Security & correctness baseline)

### Security
- **S3 — OOB dataset read fixed** (`vm.hpp`, `vm.cpp`, `mining_engine.cpp`): `set_dataset()` changed from `void` to `[[nodiscard]] bool`. In fast mode, validates `dataset.size() == kRandomXDatasetBytes` and returns `false` on mismatch. Added debug-mode bounds assertion in `dataset_read`. Call site in mining engine checks the return value and skips the job on failure.
- **S1 — JSON injection in Stratum TX** (`stratum_client.cpp`): Added `json_escape(string_view)` helper that escapes `\`, `"`, `\n`, `\r`, `\t`. Applied to `wallet_`, `password_`, and `job.job_id` in all three message builder functions (login, authorize, submit).
- **S2/S4 — W^X security** (`jit_compiler_a64.cpp`, `jit_compiler_a64.hpp`): `RANDOMX_FORCE_SECURE` now honored in the constructor — skips `setPagesRWX()` when set. Removed `enableAll()` entirely (dead code — never called).
- **S5 — Always-on assertions** (`include/armrx/assert.hpp` new): `ARMRX_ASSERT` macro that evaluates in all build configurations. Debug builds `abort()` on failure; release builds log a warning and continue. Replaced 5 plain `assert()` calls in `aes_hash.cpp` and `vm.cpp` that compiled out under `NDEBUG`.
- **S8 — JIT dispatch null guard** (`jit_compiler_a64.cpp`): Added `ARMRX_ASSERT(engine[instr.opcode] != nullptr, ...)` before both dispatch calls in `generateProgram` and `generateProgramLight`. Prevents UB from null member-function pointers.

### Correctness
- **JIT code buffer invariant** (`jit_compiler_a64.cpp`): Added `static_assert(RANDOMX_PROGRAM_MAX_SIZE == 384)` verifying the RandomX v1 constant that governs the `.fill RANDOMX_PROGRAM_MAX_SIZE*16` reservation in the static assembly template.
- **KAT tests now run in JIT mode** (`tests/test_blake2b.cpp`): Added a `#ifdef ARMRX_HAVE_JIT` section that creates a second VM with `kRandOMXFlagHardAes | kRandOMXFlagJit` and runs the same KAT vectors through the JIT compiler. Previously only the interpreted path was tested.

### Infrastructure
- **Devbox SSH fix** (`tools/devbox/devbox_mcp.py`): Added `-F /dev/null` to all SSH/rsync invocations to bypass the broken `/etc/ssh/ssh_config.d/20-systemd-ssh-proxy.conf` symlink (owned by `nobody`, blocking OpenSSH).
- **`.gitignore` hygiene** (`.gitignore`): Replaced overly broad `*.txt` with specific `PERF_BASELINE.txt`. Added `test_aarch64.cpp`. Removed duplicate `scratch_vm_study/upstream_rx`.
- **`PERF_BASELINE.txt`**: Recorded baseline performance (light mode JIT: 4.03 H/s single-thread, interpreted: 0.38 H/s).

### Verification
- CTest: 100% passed (2/2) — all KATs green in both interpreted and JIT mode.
- Benchmark: stable at 4.03 H/s (light JIT, single-thread) — no regression from security fixes.

### Added
- **`OPTIMIZATION_REFERENCE.md`**: Comprehensive document cataloging every optimization tried, what worked (✅), what failed (❌), what's deferred (⏸️), with perf analysis, build flags reference, and CLI reference.
- **XMRig `xmrig-dev` source added**: For direct comparison of JIT code generation.
- **`armrx-devbox` SSH & rsync Setup**: Configured the target PostmarketOS/Alpine AArch64 devbox (`192.168.10.156`) with the local `id_ed25519` SSH key. Installed `rsync` on the remote device via `apk` to enable MCP-driven synchronization.
- **`armrx-devbox` verification pass**: Successfully triggered status query, directory synchronization, code compilation (build), and CTest unit suite execution on the actual AArch64 device. All tests passed.
- **`armrx-devbox` tool extensions**:
  - Added support for custom parallel build job bounds (`build_jobs` in configuration or `parallel` tool argument) and appending custom configure flags (`extra_flags` in `devbox_build`).
  - Integrated `lscpu` outputs and policy-based cpufreq nodes into `devbox_status`.
  - Implemented the `devbox_perf_stat` tool to profile execution on the target device via `perf stat`, returning structured JSON reports with instructions, cycles, IPC, and branch-miss rates.

### Analysis (perf comparison on same hardware)
- armrx: 64.3B instructions vs XMRig: 48.2B — **33% more instructions** is the primary gap
- armrx: 152M branch misses vs XMRig: 11M — **13× more**
- Static ASM and JIT handlers are nearly identical — gap is distributed across years of XMRig's iterative profiling
- Buffer enlarged 4608→6144 slots, dataset prefetch changed `pldl1keep`→`pldl1strm` (matched to XMRig)

## 2026-07-14 (AArch64 JIT Loop Branch & ODR fixes)

### Fixed
- **JIT Loop Branch Relocation Fix** (`src/jit_compiler_a64_static.S`): Introduced a local label `.Lmain_loop` for the conditional loop branch (`bne .Lmain_loop`), eliminating the `R_AARCH64_CONDBR19` relocation against the global symbol `DECL(randomx_program_aarch64_main_loop)`. This prevents linker-induced loop branch corruption under Position Independent Executable (PIE) builds.
- **MiningEngine ODR Violation and Exit Crash Fix** (`CMakeLists.txt`): Changed the visibility of the `ARMRX_JIT_PROFILE` compile definition on the `armrx_core` target from `PRIVATE` to `PUBLIC`. This ensures that all downstream targets linking against `armrx_core` (e.g., `test_mining`) see the same struct layout for `MiningEngine` (specifically the JIT profiling fields), resolving a classic ODR violation that caused segmentation faults on program exit on musl libc systems.

## 2026-07-14 (AArch64 JIT Optimizations: ubfx, ext, direct NEON load)

### Added
- **ubfx Scratchpad Address Computation** (`src/jit_compiler_a64_static.S`, `src/jit_compiler_a64.cpp`): Implemented A1 optimization. Replaced 5-instruction mask-and-add scratchpad calculations with 4-instruction sequences using `ubfx` and static shifted `add` instructions.
- **FSWAP_R vector extract** (`src/jit_compiler_a64.cpp`): Implemented A2 optimization. Replaced 3-instruction `FSWAP_R` register element moves with a single vector extract (`ext`) instruction.
- **NEON Direct FP Memory Load** (`src/jit_compiler_a64.cpp`): Implemented A3 optimization. Rewrote `emitMemLoadFP` to load memory directly into floating-point registers using `ld1` and sign-extend inside NEON via `sxtl`, avoiding GPR moves and eliminating high-latency cross-port register transfer stalls (`ins`).
- **Benchmark runtime optimization** (`tests/bench_armrx.cpp`): Reduced interpreted mode benchmark iterations from 200 to 10 on slow platforms and enabled stdout flushing.

## 2026-07-14 (Per-hash overhead elimination + NEON Blake2b + memory tuning)

### Added
- **Span-based blake2b** (`include/armrx/blake2b.hpp`, `src/blake2b.cpp`): New overload `blake2b(span_in, output_ptr, output_bytes)` writes directly into caller buffer. Existing vector-return overload preserved for backwards compatibility.
- **NEON-accelerated Blake2b compress** (`src/blake2b.cpp`): AArch64 NEON `uint64x2_t` path processes G-function calls in pairs, using `vaddq_u64`, `vsriq_n_u64`/`vshlq_n_u64` for SIMD rotation, and `vst1q_u64` for bulk state save. Reduces round latency by 2–3× on Cortex-A53. Scalar fallback for x86_64.
- **JIT profiling gated behind ARMRX_JIT_PROFILE** (`CMakeLists.txt`, `vm.cpp`, `vm.hpp`, `mining_engine.hpp`, `mining_engine.cpp`): New CMake option (default OFF). When disabled, 3 per-hash `clock_gettime` syscalls and timer accumulation are compiled out. Production builds skip the profiling entirely.
- **RWX JIT code region with fallback** (`jit_compiler_a64.cpp`, `virtual_memory.c/h`): Constructor tries `mprotect(..., RWX)` once; if the kernel allows it (most Linux kernels), per-hash `mprotect` calls become no-ops via `rwx_` flag; otherwise falls back to original RW↔RX transitions.
- **Big.LITTLE-aware core pinning** (`mining_engine.cpp`): Reads `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` at startup, sorts cores by max frequency descending, pins worker threads to fastest cores first.
- **Per-worker nonce partitioning** (`mining_engine.cpp`, `mining_engine.hpp`): Each worker uses `thread_id + k * num_threads_` — removes the per-hash shared atomic `fetch_add`.
- **Huge-page dataset** (`mining_engine.cpp`, `mining_engine.hpp`): Replaced `shared_ptr<vector<byte>>` dataset allocation with `MappedMemory` RAII class backed by `mmap` + `madvise(MADV_HUGEPAGE)`. Reduces TLB pressure on 2 GiB fast-mode dataset.
- **`--mlock` flag** (`main.cpp`): Calls `mlockall(MCL_CURRENT|MCL_FUTURE)` to lock all pages in RAM, preventing mid-hash page faults.
- **`--rt-priority` flag** (`main.cpp`, `mining_engine.cpp`, `mining_engine.hpp`): Sets `SCHED_FIFO` priority 1 on worker threads; warns if `CAP_SYS_NICE` unavailable.

### Removed
- **Per-hash heap allocations in `randomx_calculate_hash`** (`vm.cpp`, `blake2b.cpp`): All `std::vector<std::byte>` temporaries replaced with `alignas(16) std::array<std::byte, N>` stack buffers. Eliminates ~15 malloc/free pairs per hash.
- **Per-hash block template copy** (`mining_engine.cpp`): Reuses a worker-local vector, resized only on job change. Eliminates 1 allocation per hash.
- **Light-mode flush_interval=1** (`mining_engine.cpp`): Unified `flush_interval=64` in both light and fast modes. Saves 5+ atomic ops per hash in light mode.
- **Dataset prefetch `pldl2strm` → `pldl1keep`** (`jit_compiler_a64_static.S:341`): Matches the verified +2.2% prefetch upgrade already applied to the dataset-item derivation prefetch.
- **`-frounding-math`** (`CMakeLists.txt`): Replaced with `-ffp-contract=fast -funroll-loops`. On AArch64 with JIT, FP rounding is handled by `msr fpcr` in the JIT prologue, not C++.

### Result
Baseline: ~23 H/s on 8× Cortex-A53 (previous changelog). After these changes the expected gain comes from: no heap allocation stalls, no mprotect syscalls, no profiling syscalls, partitioned nonces (no atomic contention), huge-page dataset (reduced TLB misses), big.LITTLE-aware pinning, and NEON Blake2b.

### Added
- **Scratchpad cache prefetch** (`src/jit_compiler_a64_static.S`): Added `prfm pldl1keep` instructions in the JIT main loop to prefetch three scratchpad cache lines (spAddr0, spAddr1, spAddr1+32) before the load instructions execute. Hides memory latency on Cortex-A53's in-order dual-issue pipeline.
- **Dataset cache line prefetch upgraded** (`src/jit_compiler_a64_static.S`): Changed `prfm pldl2strm` (L2 streaming hint, next-line eviction) to `prfm pldl1keep` (L1 keep hint) for the cache line read in `rx_calc_dataset_item_prefetch`. Since the data is XOR'd immediately after the SuperscalarHash computation, L1 residency avoids a costly L1→L2 refill. Net gain: +2.2% across all cores.
- **TUI per-worker bars fixed** (`src/tui.cpp`, `src/mining_engine.cpp`): Worker hash counters were allocated but never incremented (flushes went only to `total_hashes_`). Added `worker_hashes_[thread_id].fetch_add()` alongside the total flush, enabling live per-core bars in the TUI.

### Result
Steady hashrate of **21.8 H/s** on 8× Cortex-A53 (big: ~3.58 H/s per core, LITTLE: ~1.86 H/s per core). JIT profile: 1.6% compile, 98.4% execute.

## 2026-07-14 (NEON SIMD Vectorization & JIT/Cache Optimizations)

### Added
- **NEON SIMD Vectorized Superscalar execution** (`include/armrx/superscalar.hpp`, `src/superscalar.cpp`): Added `execute_superscalar_neon` using AArch64 NEON intrinsics (`vaddq_u64`, `vsubq_u64`, `veorq_u64`, etc.) to process two items in parallel using `uint64x2_t` registers.
- **Vectorized Dataset initialization** (`src/dataset.cpp`): Updated `initialize_dataset` under `__aarch64__` to process dataset items in pairs of 2, calling `execute_superscalar_neon` and parallelizing cache line XOR lookups.
- **Dataset generation benchmark** (`tests/bench_armrx.cpp`): Added micro-benchmark for `initialize_dataset` generating 5,000 items in a batch.
- **Argon2d Cache Huge Page allocation** (`include/armrx/argon2.hpp`, `src/argon2.cpp`): Converted the 256 MiB Argon2d cache memory layout from standard `std::vector` (malloc heap pages) to a raw block allocated via `mmap` with transparent huge page hint `madvise(MADV_HUGEPAGE)`. This reduces translation lookaside buffer (TLB) thrashing under random Light Mode lookups, increasing hashrate to ~23 H/s.
- **JIT compilation timing profiling** (`include/armrx/vm.hpp`, `src/vm.cpp`, `include/armrx/mining_engine.hpp`, `src/mining_engine.cpp`, `include/armrx/tui.hpp`, `src/tui.cpp`, `src/main.cpp`): Integrated high-resolution JIT compilation and execution timers inside `VirtualMachine::run`, periodically reported as a breakdown in the terminal UI dashboard and CLI logger.
- **Link Time Optimization (LTO/IPO) integration** (`CMakeLists.txt`): Enabled Interprocedural Optimization (LTO) across all build targets. This allows cross-translation unit optimization and inlining, reducing remote ctest runtime by ~48% (from 51.2s to 26.5s) and speeding up mining framework logic.
- **JIT Loop Alignment optimization** (`src/jit_compiler_a64_static.S`): Added `.p2align 5` before `randomx_program_aarch64_main_loop` in the static assembly template. This aligns the JIT compiled program's main loop entry point to a 32-byte boundary, optimizing instruction fetch unit utilization and branch prediction accuracy on Cortex-A53.

## 2026-07-13 (CryptoNote/Herominers Stratum support)

### Added
- **CryptoNote Stratum Protocol support** (`src/stratum_client.cpp`, `include/armrx/stratum_client.hpp`): Added support for the CryptoNote JSON-RPC stratum protocol (including `login`, `keepalived`, and `submit` methods).
- **Auto-fallback handshake** (`src/stratum_client.cpp`): Added automatic fallback from Stratum V1 (`mining.subscribe`) to CryptoNote (`login`) when the pool rejects Stratum V1.
- **Improved JSON parsing** (`src/stratum_client.cpp`): Fixed `json_get` helper to support parsing nested JSON objects and arrays correctly by scanning matching brace/bracket depths.
- **Stable keepalive handling** (`src/stratum_client.cpp`): Suppressed keepalive responses (`KEEPALIVED`) in the share submission response handler to avoid treating them as rejected shares.
- **Race-free fallback connection lifecycle** (`src/stratum_client.cpp`, `include/armrx/stratum_client.hpp`): Introduced `fallback_in_progress_` state to prevent redundant reconnect triggers and duplicate client threads during handshake fallback.
- **SIGPIPE signal ignore** (`src/main.cpp`): Ignore `SIGPIPE` globally to prevent OpenSSL shutdown alert writes to closed socket descriptors from abruptly terminating the program.
- **Non-blocking login handshake** (`src/stratum_client.cpp`): Complete the connection handshake promise before running the synchronous job callback, avoiding handshake timeouts on slower CPUs (like Cortex-A53) during Argon2d cache initialization.
- **JIT compilation in Light Mode** (`src/vm.cpp`, `src/jit_compiler_a64.cpp`, `include/armrx/jit_compiler_a64.hpp`): Enabled JIT compilation in Light mode on AArch64 by compiling Superscalar programs in `set_cache()` and matching const parameter layouts. Corrected the `CacheSize` constant in the JIT compiler from `2 GiB` to the correct cache size of `256 MiB`, fixing the out-of-bounds cache line alignment mask that caused segmentation faults. This speeds up Light mode hashrate from 1.6 H/s to hardware JIT speed (~28 H/s).

## 2026-07-13 (CryptoNote protocol + 21 H/s light-mode milestone)

### Added
- **Dual-protocol Stratum client** (`stratum_client.hpp/cpp`): Auto-detects pool protocol — tries standard Stratum V1 (`mining.subscribe`) first, falls back to CryptoNote (`login`) on rejection. Handles `keepalive`, nested JSON job parsing, and CryptoNote share submission format.
- **`StratumProtocol` enum** with `AUTO`, `STRATUM_V1`, `CRYPTONOTE` modes. Handshake lifecycle with fallback tracking prevents race conditions during protocol switch.

### Fixed (performance: 1.2 → 21 H/s)
- **`CacheSize` constant in JIT compiler** (`jit_compiler_a64.cpp`): Was `2147483648` (2 GiB, dataset size) — corrected to `268435456` (256 MiB, cache size). The 8× too large cache mask caused out-of-bounds reads and segfaults in light mode. **This was the primary 22× performance bottleneck.**
- **Light-mode JIT enabled**: `set_cache()` now calls `jit_->generateSuperscalarHash()` to compile SuperscalarHash programs. Cache data pointer passed to `mem_regs.memory` for the JIT light-mode dataset derivation path.
- **Non-blocking handshake**: Moved handshake promise fulfillment before Argon2d cache init in job callback — prevents connection timeouts on slow hardware.
- **`kRandOMXFlagJit` unconditional**: JIT enabled regardless of `kRandOMXFlagFullMem` so light-mode VMs get JIT compilation.
- **SIGPIPE ignored**: Prevents OpenSSL `close_notify` writes on closed sockets from crashing the process.
- **Hashrate flushing per hash**: Worker threads flush local counter after each hash in interpreted/light mode for instant accurate reporting.

## 2026-07-13 (CPU affinity + per-worker counters + pool failover)

### Added
- **CPU affinity pinning** (`src/mining_engine.cpp`): Each worker thread pinned to `thread_id % hardware_concurrency()` via `pthread_setaffinity_np` — eliminates core migration overhead.
- **Per-worker hash counters** (`include/armrx/mining_engine.hpp`, `src/mining_engine.cpp`): Per-thread `std::atomic<uint64_t>` array with local accumulator batching (flush every 64 hashes). Exposed via `worker_hash_rate(thread_id)`.
- **Multiple pool failover** (`src/main.cpp`): Accept multiple `--pool=host:port` arguments. After 5 retries on the current pool, automatically cycles to the next with a 2s cooldown.
- **`--help` updated**: Documents `--tls`, multi-pool, and all CLI flags.

### Remaining
- Only two items left in the priority list: config file (medium) and Stratum V2 (high).

## 2026-07-13 (TLS/SSL pool connections + documentation sweep)

### Added
- **TLS/SSL support for pool connections** (`src/tls_client.cpp`, `include/armrx/tls_client.hpp`): Optional OpenSSL-based TLS wrapping. RAII `TlsClient` class with `SSL_connect`, SNI hostname, cipher reporting, and clean shutdown. Disabled at build time when OpenSSL is not found.
- **`--tls` / `--no-tls` CLI flag** (`src/main.cpp`): Enables encrypted pool connections (default: off).
- **CMake**: `find_package(OpenSSL QUIET)` — auto-detects OpenSSL; links `OpenSSL::SSL` + `OpenSSL::Crypto` and defines `ARMRX_HAVE_TLS=1` when found.

### Modified
- **`include/armrx/stratum_client.hpp`**: Added `enable_tls()` setter, optional `TlsClient` member under `#ifdef ARMRX_HAVE_TLS`.
- **`src/stratum_client.cpp`**: `connect()` wraps TCP socket with TLS when enabled; `write_all()`/`read_line()` route through TLS; `disconnect()` tears down TLS before closing socket.
- **`planned_improvements.md`**: Marked TLS/SSL as complete; collapsed priority table to 6 remaining items.

## 2026-07-13 (README restructure + auto-reconnect + huge pages)

### Added
- **Auto-reconnect with exponential backoff** (`src/stratum_client.cpp`): When the pool connection drops, the client now automatically retries with 1s → 2s → 4s → … → 30s cap backoff, configurable via `set_reconnect_config(max_retries, base_delay_ms)`. Error callback is only called after all retries are exhausted (default: 10 retries, then give up).
- **Huge pages for scratchpads** (`src/vm.cpp`): `madvise(MADV_HUGEPAGE)` applied after each 2 MiB scratchpad allocation, prompting the kernel to promote to transparent huge pages for reduced TLB pressure.

### Modified
- **`README.md`**: Full restructure — badges header, Quick Start / Usage / Architecture / Status sections with tables, tighter prose (~40% shorter). Replaced verbose "Implementation order" with a component checklist.
- **`include/armrx/stratum_client.hpp`**: Added `set_reconnect_config()`, `reconnect_attempts()` getter, `reconnect_loop()` private method, and backoff state members.
- **`src/main.cpp`**: Pool loop now runs `while (keep_running)` regardless of connection state. Status line shows yellow "Reconnecting (attempt N)..." when offline. Initial connection failure no longer exits — reconnect loop handles retries.
- **`planned_improvements.md`**: Marked auto-reconnect and huge pages as complete; renumbered priority table.

## 2026-07-13 (AArch64 build verification + JIT fixes)

### Added
- **`src/instruction_weights.hpp`**: Instruction frequency `#define`s and REP macros for the JIT compiler's 256-entry opcode handler table. Covers all 30 RandomX v1 opcode weights and REP0–REP256 expansion macros.
- **`src/configuration.h`**: Minimal assembly-compatible header defining `RANDOMX_PROGRAM_MAX_SIZE=384` required by `jit_compiler_a64_static.S`.
- **`src/soft_aes.cpp`**: AES lookup tables (`randomx_aes_lut_enc[4][256]`, `randomx_aes_lut_dec[4][256]`) extracted from upstream RandomX, needed by the JIT compiler's soft-AES fallback path.
- **`REASONIX.md`**: Project card capturing stack, layout, commands, conventions, and gotchas for future Reasonix sessions.

### Fixed
- **`CMakeLists.txt`**: Changed `LANGUAGES CXX` to `LANGUAGES C CXX ASM` so `virtual_memory.c` actually compiles instead of silently dropping the C source.
- **`src/jit_compiler_a64.cpp`**: Added 12 missing upstream constants (`RANDOMX_SCRATCHPAD_L1/L2/L3`, `CacheLineSize`, `CacheSize`, `ScratchpadL3Mask`, `ConditionMask/Offset`, `StoreL3Condition`, `RegisterNeedsDisplacement`) that were referenced but never defined. Fixed API mismatches (`getSize()`→`size()`, `getAddressRegister()`→`address_register()`), replaced `randomx_reciprocal_fast` with the project's `randomx_reciprocal`, and inlined the `isZeroOrPowerOf2` check.

### Verified
- **First full build + test pass on real AArch64 hardware** (Lenovo/MSM8916, postmarketOS edge, Linux 6.12.1, GCC 15.2.0). Both `armrx_tests` and `test_mining` pass, confirming:  
  - Reference hash `Input1` = `639183aae1bf4c9a35884cb46b09cad9175f04efd7684e7262a0ac1c2f0b4e3f` ✅  
  - Reference hash `Input2` = `300a0adb47603dedb42228ccb2b211104f4da45af709cd7547cd049e9489c969` ✅  
  - JIT + hardware AES/NEON pipeline fully operational on ARMv8-A with crypto extensions.

## 2026-07-13

### Added
- Created interpreted RandomX Virtual Machine execution engine (`src/vm.cpp`, `include/armrx/vm.hpp`) including integer and floating-point registers, scratchpad reads/writes, compiler thresholds, and interpreted execution loop.
- Added software AES encryption/decryption round primitives and scratchpad filling logic (`src/aes_hash.cpp`, `include/armrx/aes_hash.hpp`).
- Added end-to-end VM hash parity tests in `tests/test_blake2b.cpp` to validate against reference inputs `"This is a test"` and `"Lorem ipsum dolor sit amet"`.
- Implemented multi-threaded `MiningEngine` (`src/mining_engine.cpp`, `include/armrx/mining_engine.hpp`) for orchestrating mining loops, thread-local VM instances, and memory mode transitions.
- Added `Target` structure and `meets_target` verification logic (`include/armrx/mining_common.hpp`) to compare computed hashes against target difficulties.
- Added target difficulty translation, signal handling, and runtime benchmark duration controls to `src/main.cpp`.
- Added unit tests `tests/test_mining.cpp` to validate target difficulty boundary checks and worker orchestration lifecycles.

### Fixed
- Fixed AES decryption round sequence (`aes_decrypt_round` in `src/aes.cpp`) to apply standard Inverse ShiftRows -> Inverse SubBytes -> Inverse MixColumns -> AddRoundKey order.
- Corrected `build_aes_block` word mapping to follow standard little-endian format.
- Modified `init_scratchpad` to update the seeding `tempHash` in-place, passing the state-modified seed to the first VM program execution.
- Corrected floating-point instruction compilation frequency thresholds (`ceil_` ceilings in `src/vm.cpp`) to align with standard configuration frequencies.
- Added zero-initialization of integer registers `reg_.r` on VM setup to prevent cross-run state leaks.
- Removed unused parameter warnings from `execute_bytecode`.

### Modified
- Updated `README.md` to document the completed interpreted VM implementation details, status, and verification test vectors.
- Updated `CMakeLists.txt` to compile `src/mining_engine.cpp` with pthread linkages, and register `test_mining` to the build and CTest validation pipeline.

## 2026-07-13 (AArch64 JIT + Hardware AES)

### Added
- **Hardware AES round intrinsics** (`src/aes_hash.cpp`): Added conditional `#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)` fast paths for all four core functions (`fill_aes_1r_x4`, `fill_aes_4r_x4`, `hash_aes_1r_x4`, `hash_and_fill_aes_1r_x4`). Uses `vaeseq_u8`/`vaesmcq_u8`/`vaesdq_u8`/`vaesimcq_u8` NEON intrinsics; falls back to software-GF(2^8) path on non-AArch64 targets.
- **JIT compiler infrastructure**: Ported `jit_compiler_a64.cpp`, `jit_compiler_a64_static.S`, and `jit_compiler_a64.hpp` from upstream RandomX under `src/` and `include/armrx/`. Adapted includes, namespace (`armrx`), constants (`RegistersCount`, `RANDOMX_FLAG_*`, `RANDOMX_CACHE_ACCESSES`, `RANDOMX_SUPERSCALAR_LATENCY`), and removed upstream-specific dependencies.
- **Virtual memory support** (`src/virtual_memory.c`, `include/armrx/virtual_memory.h`): Ported upstream POSIX `mmap`/`mprotect` page allocator (writable + executable memory pages needed for JIT code emission).
- **JIT VM integration** (`include/armrx/vm.hpp`, `src/vm.cpp`): Added `kRandOMXFlagJit` flag; `VirtualMachine` conditionally constructs `JitCompilerA64` and compiles each program at runtime via `generateProgram`, wires `ProgramConfiguration` from VM state, and calls `getProgramFunc()` instead of entering the interpreted loop when `ARMRX_HAVE_JIT` is defined and the flag is set.
- **Program/ProgramConfiguration types** (`include/armrx/program.hpp`): Introduced `ProgramConfiguration` and `MemoryRegisters` structs used by the JIT compiler ABI.

### Modified
- **`CMakeLists.txt`**: Detects AArch64 target processor via `CMAKE_SYSTEM_PROCESSOR`; enables `ASM` language and adds JIT/ASM sources (`jit_compiler_a64.cpp`, `jit_compiler_a64_static.S`, `virtual_memory.c`) only on that platform; sets `-march=armv8-a+crypto` and `ARMRX_HAVE_JIT=1`.
- **`include/armrx/vm.hpp`**: Replaced raw `std::array<Instruction, 256> program_{}` with `Program program_{}` for compatibility with JIT compiler API; added optional `std::unique_ptr<JitCompilerA64> jit_` member under `#ifdef ARMRX_HAVE_JIT`.
- **`README.md`**: Updated step 5 as complete; added build notes explaining AArch64-specific flags and x86_64 fallback behaviour.

### Fixed
- Fixed stray `c` prefix character on `aes_encrypt_round` function declaration in `src/aes.cpp`.

## 2026-07-13 (Stratum V1 Client)

### Added
- **`include/armrx/stratum_client.hpp`**: Declared `StratumClient` class implementing Monero Stratum V1 protocol. Exposes `connect()`, `disconnect()`, `submit_share()`, `set_job_callback()`, and `set_error_callback()`.
- **`src/stratum_client.cpp`**: Full Stratum V1 implementation:
  - TCP socket connection via POSIX `getaddrinfo` / `connect`, with `TCP_NODELAY` for low-latency share submission.
  - Background reader thread that accumulates line-delimited JSON messages from the pool.
  - `mining.subscribe` — sends agent string, extracts `extranonce1` from pool reply.
  - `mining.authorize` — authenticates wallet address and password with the pool.
  - `mining.notify` — parses job ID, block template blob, target, and seed hash; dispatches via `JobCallback`.
  - `mining.set_target` — updates 32-byte target from 64-char hex string.
  - `mining.set_difficulty` — converts numeric difficulty to 32-byte target using the same `2^256 / D` algorithm as the local miner.
  - `mining.submit` — serialises nonce as little-endian hex and sends a JSON-RPC submit message.
  - Minimal hand-rolled JSON extractor (no external library dependency).
- **`src/main.cpp`**: Added pool mining mode with three new CLI flags:
  - `--pool=host[:port]` — pool server address (default port 3333).
  - `--wallet=<address>` — Monero wallet address used as worker login.
  - `--password=<pw>` — pool worker password (default `x`).
  - `--help` updated to document pool mining section.
  - Pool mode wires `StratumClient::set_job_callback` → `MiningEngine::set_job`, and `MiningEngine::ShareCallback` → `StratumClient::submit_share`, with a live H/s status line.

### Modified
- **`CMakeLists.txt`**: Added `src/stratum_client.cpp` to `armrx_core` source list.
- **`README.md`**: Added **§3 Pool Mining (Stratum V1)** usage section; marked step 6 as complete in the implementation order list.

