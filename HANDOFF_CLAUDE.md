# Handoff: armrx — Next Session/Agent

**Session dates:** 2026-07-25 (this handoff supersedes the prior one, dated 2026-07-22/23/24, HEAD `88f4122` — everything below happened since then)
**HEAD at handoff:** `41dfa78` (pushed to `origin/main`)
**Devbox:** 192.168.10.156 (Cortex-A53, 8 cores, ~1.8-2 GiB RAM)

This is a self-contained handoff for whoever picks this up next — human or agent. Read this before `NEXT_STEPS.md`/`PLAN.md`/`ROADMAP.md`/`changelogs.md` — those are all kept current and cross-referenced, but this document gives you the narrative and the "why," which the trackers don't.

**Recurring theme this session, worth internalizing before you start**: an external audit's claim that something was "theoretical only, no known real-world trigger" turned out to be flatly wrong — it fired on the very first, most basic test case, the moment anyone actually ran the suite with it in place. Also: a huge, obviously-wrong-looking performance number (a ~2x PGO win) turned out to be a measurement artifact, caught only because the user pushed back on it before it got written down as real. **Verify before trusting — including your own audit-suggested hardening, and including anything that looks like an unusually clean win.**

---

## 1. What happened this session, in order

### Emitter lookahead scheduler landed for real (`PLAN.md` Phase 6 item 12)
Picking up from the previous session's item 14 finding (`IMUL_R`/`IMULH_R`/`ISMULH_R`/`IMUL_RCP` dominate mining cycles), implemented a conservative 2-3-instruction JIT emission scheduler. **First version (main VM program only) measured as a clean null** (+0.016% IPC, noise-level) plus a real +24.9% branch-miss cost — because the dominant IMUL cost actually lives in the superscalar/dataset-derivation region, a completely separate JIT emission path the scheduler never touched. **Extended it to that path** (`scheduleSuperscalarProgram()`), finding and fixing a second hazard along the way (the superscalar path's `IMUL_RCP` literal-pool consumption is order-sensitive in a way the main path's isn't — see `src/jit_compiler_a64.cpp`'s doc comment above `scheduleSuperscalarProgram()`). Verified via two dedicated large-N differential stress tests (450 + 200 pairs) plus the full existing suite. **Final measurement, `taskset`-pinned, two reversed-order rounds: +0.233% IPC / -0.036% cycles vs. baseline — small but real and reproducible.** Adopted.

### Three independent code reviews of the scheduler
Requested given the scheduler is consensus-critical (silent-wrong-hash risk). Deepseek, Gemini, and Hermes all reviewed independently. All three confirm no constructible failure scenario in the shipped code. **One real finding, caught by two of three**: the `src==dst` exclusion's doc comment was factually wrong about `h_IROL_R` (claimed it uses the shared x20 scratch register when `src==dst`; the code does the opposite). **Gemini missed this and just restated the false claim without checking the actual handler code** — a real quality gap, not noise; the user chose not to use Gemini for future scheduler-adjacent reviews as a result. Fixed the doc comment, added a fail-safe assert in `resolveInstructionType()`, documented why `num32bitLiterals=64` is load-bearing for the superscalar scheduler. Full reports in `docs/audits/{emitter-scheduler-review,jit_scheduler_code_review_gemini,scheduler-review-2026-07-25}.md`.

### PGO re-checked — still null, but almost documented a false 2x win
Re-ran `devbox_pgo_build` now that the scheduler changed the code shape. First (unpinned) comparison showed PGO at ~2x the non-PGO hashrate — **the user immediately, correctly diagnosed this as a core-cluster artifact before it was accepted as real** (this device has two 4-core clusters at different clock speeds; unpinned `bench_armrx` can land on either). Confirmed directly: the *identical* non-PGO binary alone gave 4.48 H/s on core 0 and 2.24 H/s on core 4 — a 2x swing with zero code difference. Re-measured with `taskset -c 0` pinning both binaries to the same core: PGO and non-PGO came out identical within noise. **PGO remains a confirmed null.** Also found: `devbox_build(reconfigure=true)` alone doesn't get you back to a clean non-PGO build if leftover `.gcda` profile data is still in the build dir — CMakeLists auto-detects it and silently stays in PGO-USE mode; `clean=true` is required.

### Full codebase audit (Deepseek) — verified, one non-issue closed, one small item found
Third-party audit supplied as `PROJECT_AUDIT_REPORT_20260725_Deepseek.md` (now in `docs/audits/`). Verified every concrete claim rather than accepting it. Closed one previously-open question (scratchpad huge-page residency — turned out to be merged with the Argon2 cache mapping in `/proc/pid/smaps`, 100% covered, not a real gap). Confirmed `-frounding-math` was genuinely missing from `CMakeLists.txt` (now added) and a CBRANCH-with-unwritten-target edge case existed (added a defensive assert — **this assert turned out to be based on a false premise, see below**).

### Peephole JIT coalescing closed — on evidence, not just deferral
Extended `tools/jit_correlate.py` to split its old "~22% of cycles unattributed" bucket using the `code_size` boundary it already parsed but never actually used to classify sample addresses. Two live `perf record` captures (cycles + instructions, same 8-worker workload) gave region-level relative IPC: **the main per-hash VM program region carries ~9% of dynamic instructions but ~20% of cycles — a ~2.2× IPC penalty**, a memory-op (scratchpad) stall signature, not an instruction-count signature. This is evidence *against* peephole JIT coalescing (whose entire premise is code-density reduction), matching the same root cause every other instruction-count-reduction attempt this project has ever tried has hit. Closed, not deferred. See `docs/archived/plan_phase7_completed.md` (item 2) and `docs/plans/performance-plan-20260725.md`.

### Memory-op scheduler extension — tried, caused a real regression, reverted
The natural follow-on: flag memory-load opcodes (`*_M`) as scheduler swap triggers, same mechanism that won for the superscalar region. **`test_jit_equivalence` failed on its first, most basic seed/input pair — the first failure of this specific test in the project's history.** Reverting the memory-op change alone fixed it, confirming it as the cause. Investigated at length (checked whether `emitMemLoad`'s own `src==dst`/x20 pattern was responsible — it wasn't, already reviewed and ruled out by all three code reviews). **The exact mechanism was never conclusively identified.** Given the failure mode is silent wrong hashes and this was an explicitly speculative experiment, fully reverted rather than ship an unverified fix. Full writeup: `docs/experiments/memory-op-scheduler-attempt.md`.

### A real bug found while investigating the above: the CBRANCH assert was wrong
The defensive assert added earlier the same day (per the Deepseek audit's "theoretical only" claim about a CBRANCH targeting a never-written register) **fired repeatedly during the memory-op investigation, on a completely normal test run** — disproving its own premise directly. Traced why the existing behavior (in both interpreter and JIT) is actually correct and intentional: `register_usage_[creg] == -1` wraps `pc` to `0`, i.e. "restart from VM instruction 0," which exactly matches the JIT's own `reg_changed_offset[]` reset semantics. Removed the assert. **Lesson: an audit's "theoretical only" claim about a defensive assert needs the same empirical verification as an audit's bug report — don't just add the assert and move on.**

### Documentation reorganization
`PLAN.md`'s Phase 6 section had grown to ~710 of its 770 lines of completed history (the same size threshold that triggered the Phases 1-5 archive split last session) — split into `docs/archived/plan_phase6_completed.md`, following the exact same precedent. `PLAN.md` is now 143→~165 lines, Phase 7. `README.md`, `ROADMAP.md`, `NEXT_STEPS.md`, `changelogs.md` all brought into sync. Wrote `docs/experiments/memory-op-scheduler-attempt.md` and `docs/plans/future-performance-ideas-20260725.md` (a speculative, explicitly-unscheduled backlog — see below). Moved the Deepseek audit report from repo root into `docs/audits/`.

### Continuation: three overlapping planning docs reconciled, then Phase 8's `isolcpus`/`rt-priority` item resolved with a real ~14% win
A second agent independently wrote `docs/plans/performance-plan-20260725.md` and `docs/plans/experimental-performance-ideas-20260725.md` on top of this session's `future-performance-ideas-20260725.md`. Fact-checked both new docs against the codebase (all major claims held up — region-IPC table, Argon2 `MADV_POPULATE_WRITE` gap, superscalar `IMUL_RCP` asymmetry all confirmed real), fixed a broken cross-reference, merged the redundant doc's unique content into the backlog, retired it to `docs/archived/`. Then Phase 7 was archived the same way Phase 6 was (`docs/archived/plan_phase7_completed.md`), leaving Phase 8 with one item: `--rt-priority`/`isolcpus=`/`nohz_full=`, blocked all project long on device access.

**The user then installed `setcap` and granted explicit permission to edit the kernel boot cmdline and reboot the device.** What followed is the biggest measured win this project has found — every prior adopted change has been sub-1%:

- `isolcpus=1-7 rcu_nocbs=1-7` (core 0 left for kernel housekeeping) gives a reproducible **~28.4 H/s aggregate 8-worker hashrate vs. ~24.9 H/s without it — a real +14%.** Per-worker data shows the actual mechanism, not just an aggregate delta: the fast cluster (cores 0-3) is identical either way (4.26 H/s each); the entire effect is 2-3 of the 4 weak-cluster (cores 4-7) workers randomly losing half their throughput to background OS work/interrupts each run *without* isolation, recovered with it. `nohz_full=1-7` silently no-ops — confirmed via `/proc/config.gz` that this kernel has `CONFIG_NO_HZ_FULL` unset.
- `--rt-priority` is functionally engaged (`setcap cap_sys_nice+ep` on the binary, confirmed via absence of the `CAP_SYS_NICE` fallback warning) but shows no clear independent effect beyond `isolcpus` alone — every isolated round landed in the same ~28.4 H/s range with or without the flag. A `perf stat` attempt to get a low-noise reading hit a real, unresolved gotcha: attaching it to the full multi-threaded `armrx --mine` process gave near-zero cycle counts (it correctly measured a sanity-check busy loop, so the issue is specific to attributing cycles to this binary's worker pthread — not yet root-caused).
- **Caught and corrected three of my own mistakes mid-investigation, each time because the user pushed back with something specific**: (1) initially compared today's numbers against a 28.28 H/s figure that turned out to be *XMRig's* number from a different comparison, not armrx's — armrx's real historical baseline is 24.95 H/s (`README.md`); (2) also cited an even older 28.9 H/s figure from `docs/archived/beyond-parity_v2.md` that flags itself as stale in its own text and likely predates the AES T-table bug fix; (3) attributed one anomalous low reading to thermal throttling more confidently than the evidence supported — real thermal-zone sysfs data (32-39°C) argued against it once actually checked. **None of these were caught by me proactively — the user caught all three.** Full account in `docs/experiments/isolcpus-rt-priority-win.md`.

This is an **operational/deployment** finding (kernel boot cmdline), not a code change — can't be baked into `armrx` itself, but should be recommended to anyone deploying on similar asymmetric multi-cluster ARM hardware. `PLAN.md` Phase 8 is now closed with this result; no genuinely open item remains project-wide as of this handoff.

---

## 2. Current state — how to verify it yourself

Both x86_64 (dev sandbox) and AArch64 (devbox) are green as of `41dfa78`:

```sh
# x86_64 (interpreted VM only, JIT excluded)
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
# → 7/7: armrx_tests, test_mining, bench_armrx, test_config, test_cli_parser,
#        test_aes_hash, test_pool_protocol
```

```sh
# On-device (same command)
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
# → 14/14: + bench_opcodes, test_jit_encodings, test_jit_determinism,
#          test_jit_equivalence, test_aes_neon, test_jit_scheduler_stress,
#          test_jit_superscalar_scheduler_stress
```

Note: `test_jit_scheduler_stress` (450 pairs) and `test_jit_superscalar_scheduler_stress` (200
pairs) are each genuinely slow (~2000s / ~1300s, interpreter-dominated) — this is expected, not a
hang; see `CLAUDE.md`'s CTest path caveat note and budget accordingly if running the full suite
unattended.

Devbox build flags left in the default state:
```
ARMRX_PGO=OFF, ARMRX_ENABLE_NEON_AES=OFF, ARMRX_ENABLE_JIT_FAST_DIV_SQRT=OFF
```

**Devbox OS/kernel state is NOT default anymore, as of this handoff** — two persistent changes
were applied with the user's explicit sign-off and are still in effect:
- `/boot/extlinux/extlinux.conf` has `isolcpus=1-7 nohz_full=1-7 rcu_nocbs=1-7` appended to the
  boot cmdline (backup at `extlinux.conf.bak-20260725` for the no-isolation state, if ever
  needed). Confirmed via `/sys/devices/system/cpu/isolated` showing `1-7`. This is the source of
  the real ~14% hashrate win above — don't revert it without a reason.
- `setcap cap_sys_nice+ep` is applied to `/home/mechres/armrx/build/armrx`. This is a file
  attribute, not a build flag — **it does not survive a rebuild** (the file gets replaced), so
  re-run `sudo setcap cap_sys_nice+ep /home/mechres/armrx/build/armrx` after every
  `devbox_build`/`cmake --build` before relying on `--rt-priority` actually engaging `SCHED_FIFO`.

`-frounding-math` **is** now set unconditionally in `CMakeLists.txt` (not a flag, just added to
`armrx_core`'s compile options — see §1).

**Devbox MCP bridge**: same tools as before, plus a real limitation confirmed this session —
**this MCP host's client only tolerates one in-flight request per connection.** If you check on a
background build/test/bench via a fresh `devbox_shell` call while the earlier long-running call
is still actually running server-side, it will kill the connection. Wait for the completion
notification instead of polling with a fresh call. If the connection does drop, the user can
reconnect with `/mcp`; if it keeps happening, falling back to direct `ssh`/`scp` (using
`tools/devbox/devbox.json`'s host/user/`ssh_key` fields) works reliably and was used for the
heavier `perf record` capture work this session.

---

## 3. Documentation map (what's current vs. stale)

| Doc | Status |
|---|---|
| **This file** | Current as of `41dfa78`. Start here, never committed. |
| `PLAN.md` | Current — Phase 7 and Phase 8 both fully closed and archived. No genuinely open item remains. |
| `NEXT_STEPS.md` | Current — Phase 7 summary at top, Phase 6 history below it. |
| `changelogs.md` | Current — dated entries for everything this session, newest first. |
| `ROADMAP.md` | Current — Phase 6/7 completed tables closed out, Reference Docs table updated for the doc consolidation. |
| `README.md` | Current — feature claims corrected (PGO/scheduler honesty), Documentation section links updated post-consolidation. |
| `docs/archived/plan_phase6_completed.md`, `plan_phase7_completed.md` | Full Phase 6 and Phase 7 narratives, split out of `PLAN.md`. |
| `docs/experiments/memory-op-scheduler-attempt.md` | New this session — full writeup of the reverted scheduler extension. |
| `docs/experiments/isolcpus-rt-priority-win.md` | New this session — the real ~14% `isolcpus`/`rcu_nocbs` win, biggest measured in project history. |
| `docs/plans/performance-plan-20260725.md` | Gated, evidence-first plan against the main-VM-program 2.2× IPC lead (a second agent's more detailed rewrite of this session's original future-ideas Section 1). |
| `docs/plans/experimental-performance-ideas-20260725.md` | Speculative backlog covering other regions (superscalar, C++ overhead, cross-cutting); items 13-15 merged in from the now-retired future-ideas doc. |
| `docs/archived/future-performance-ideas-20260725.md` | This session's original future-ideas doc — superseded same-day by the pair above, retired with a pointer header. |
| `docs/audits/emitter-scheduler-review.md`, `jit_scheduler_code_review_gemini.md`, `scheduler-review-2026-07-25.md` | New this session — the three independent scheduler reviews. |
| `docs/audits/PROJECT_AUDIT_REPORT_20260725_Deepseek.md` | Moved into `docs/audits/` this session (was at repo root). |
| `tools/jit_correlate.py` | Extended this session — now splits its old unattributed bucket by the `code_size` region boundary. |
| `CLAUDE.md` / `AGENTS.md` / `REASONIX.md` | Brought current this session (were stale on `-frounding-math`, Phase 6→7, and the scheduler's final measured outcome; later re-synced for the Phase 7 archive + doc consolidation). `CLAUDE.md` never committed; `AGENTS.md`/`REASONIX.md` are tracked. |
| `docs/archived/plan_completed_phases_1-5.md` | Unchanged, still the Phase 1-5 archive. |
| `docs/plans/performance-next-agent-handoff.md` | Older deep reference (2026-07-19) — background only, superseded by this session's newer findings in most specifics. |
| `HANDOFF_GEMINI.md` | Much older prior handoff, superseded. |

---

## 4. Recommended next steps, prioritized

**No genuinely open item remains as of this handoff.** Every code-level performance lead
(peephole JIT, the memory-op scheduler extension) and the one deployment-level lead
(`--rt-priority`/`isolcpus=`/`nohz_full=`) have all been tried, measured, and either adopted or
ruled out with reasoning. See `docs/plans/performance-plan-20260725.md` (gated steps) and
`docs/plans/experimental-performance-ideas-20260725.md` (speculative backlog) for what's left if
someone wants to keep pushing on code-level performance — short version: the main VM program's
2.2× IPC penalty is real and unaddressed, but the obvious fix (scheduling) has a demonstrated
correctness risk, and a proper fix needs real bisection time, not another guess.

### Loose ends from the `isolcpus`/`rt-priority` win, not blocking
- The one anomalous 23.09 H/s isolated-round reading is unexplained — IRQ affinity
  (`/proc/irq/*/smp_affinity`) steering onto the isolated cores is the leading theory, not
  confirmed.
- `--rt-priority`'s independent contribution on top of `isolcpus` is unconfirmed — the `perf
  stat` thread-attribution gotcha (near-zero cycle counts against the full multi-threaded
  `armrx` binary) blocked getting a real low-noise reading. Worth fixing if anyone wants a
  precise answer rather than "no clear wall-clock effect."
- **Remember**: the devbox's kernel cmdline and the `armrx` binary's `setcap` state are both
  non-default now (see §2) — don't assume a fresh `devbox_build` alone preserves `--rt-priority`
  working, and don't revert the cmdline without a specific reason (it's the source of the +14%).

### If picking performance back up
- Read `docs/plans/performance-plan-20260725.md` first (Step 1: bound the achievable win before
  committing to anything). `docs/plans/experimental-performance-ideas-20260725.md` covers other
  regions if that plan's steps are exhausted. Shared honest recommendation: don't chase further
  microarchitecture wins without a new, specific, measured hypothesis — this codebase has been
  through six phases of profiling, three code reviews, and three audits.
- If you do want to re-attempt the memory-op scheduler idea, budget real bisection time
  (`ARMRX_MAX_SWAPS`-style env var, the same technique that solved the original `src==dst` hazard)
  — don't just re-apply the change and hope it works this time.

### Test coverage (lower priority, unchanged from prior handoff)
- `tls_client.cpp`/`tui.cpp` remain fully untested — would need a mock TLS server / terminal-
  output capture harness respectively.

### Open decision (not a bug, needs a maintainer call)
- JIT buffer W^X vs RWX default — kept RWX per explicit earlier direction, disclosed via a
  startup log line. Unchanged this session.

### Backlog (deprioritized per explicit earlier direction, not deleted)
- QEMU AArch64 GitHub Actions CI, Stratum V2 protocol support, `ARMRX_JIT_FAST_DIV_SQRT` CMake
  flag centralization.

---

## 5. Patterns worth carrying forward

**Verify audit-suggested *hardening*, not just audit-suggested bug reports, before trusting it.**
The CBRANCH assert's "theoretical only" premise was wrong — it fired on the very first test run.
An audit claiming something *can't* happen is exactly the kind of claim worth running the test
suite against before shipping a change based on it, the same discipline this project already
applies to audit-claimed *bugs*.

**A big, clean-looking win deserves the same skepticism as a bug report.** The ~2x PGO number
looked like a real, substantial win — and would have been documented as one if the user hadn't
pushed back immediately with a specific, correct alternative hypothesis (core-cluster placement).
Any surprisingly large effect size on this device, in either direction, should be checked against
`taskset` pinning before being trusted.

**Two scheduler hazards were empirically fixed without a fully-traced mechanism (`src==dst`,
superscalar `IMUL_RCP` ordering); a third was found and could not be fixed the same way (memory-op
extension).** The difference: the first two had a working, verifiable exclusion rule even without
full understanding. The third had no such rule — reverting fully, rather than shipping a guessed
fix, was the right call given the failure mode is silent wrong hashes. Don't confuse "empirically
validated fix, mechanism unclear" with "no fix found, revert" — they look similar but aren't.

**Profile first, measure honestly, keep negative results documented, one level deeper than usual
this session**: `tools/jit_correlate.py`'s region-split extension is a good example of getting
more out of existing tooling (a boundary value that was already being parsed but never used)
rather than building something new from scratch. Worth checking whether existing instrumentation
already has the answer before writing new instrumentation.
