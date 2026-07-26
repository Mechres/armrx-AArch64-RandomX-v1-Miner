# Next Steps Task List

**Updated:** 2026-07-26
**HEAD:** see `git log` for current
**Devbox:** 192.168.10.156

Phases 1–11 are all **fully resolved** — see `docs/archived/plan_completed_phases_1-5.md`,
`docs/archived/plan_phase6_completed.md`, `docs/archived/plan_phase7_completed.md`,
`docs/experiments/isolcpus-rt-priority-win.md`,
`docs/experiments/scratchpad-locality-bound-20260726.md`,
`docs/plans/experimental-performance-ideas-20260725.md`, and
`docs/plans/mid-high-risk-performance-ideas-20260726.md` for the full narratives.

*   [x] ~~Mid/high-risk performance work (`docs/plans/mid-high-risk-performance-ideas-20260726.md`)~~
    — **one adopted, one root-caused and closed for now, 2026-07-26.** Widened the main VM
    program scheduler's swap window (4-instruction fallback candidate) — passed the full 450-pair
    `test_jit_scheduler_stress`, measured +0.156% IPC avg across two `perf stat` samples. Adopted.
    See `docs/experiments/main-scheduler-window-widening-20260726.md`. Separately, root-caused why
    the earlier reverted superscalar `IMUL_RCP` register pre-assignment failed (a light-mode call
    boundary doesn't protect the main program's own live r6/r7 or its own `IMUL_RCP` literals) —
    real safe budget is at most 1 register, not worth pursuing right now, but left open with a
    concrete revisit path rather than closed permanently. `PLAN.md` Phase 11.

*   [x] ~~Experimental performance backlog (`docs/plans/experimental-performance-ideas-20260725.md`)~~
    — **worked to full closure 2026-07-26.** 4 adopted (compiler flags #6+#12 +0.298% IPC avg,
    Argon2 `MADV_POPULATE_WRITE` #10, `.p2align 6` #11), 5 closed on evidence (#2, #5, #7, #8, #9),
    1 diagnostic done (#4 — AES fill/hash is the biggest named-C++ cost at ~12.3% of cycles), 1
    tried-and-reverted (#1 — real `test_jit_equivalence` failure, see
    `docs/experiments/superscalar-imul-rcp-preassignment-attempt.md`). Nothing remains
    unaddressed in this file. `PLAN.md` Phase 10.

*   [x] ~~Performance plan Step 1 (bound the main-VM-program scratchpad-locality recoverable
    gap)~~ — **done 2026-07-26, small result (+6.07% IPC), closes Steps 2-3 without attempting
    either.** `bench_armrx --scratchpad-real`/`--scratchpad-l1` + `perf stat` on-device: forcing
    the scratchpad to be effectively L1-resident only recovered 6% of IPC against the region's
    ~2.2× overall penalty — the stall is mostly architectural, not memory-latency. **No genuinely
    open performance lead remains project-wide.** See
    `docs/experiments/scratchpad-locality-bound-20260726.md`.

*   [x] ~~`--rt-priority` + `isolcpus=`/`nohz_full=`~~ — **done 2026-07-25, a real ~14% win, the
    biggest measured win in this project's history.** The user installed `setcap` and granted
    device access (cmdline edit + reboot). `isolcpus=1-7 rcu_nocbs=1-7` gives a reproducible
    ~28.4 H/s aggregate 8-worker hashrate vs. ~24.9 H/s without it. Mechanism: cores 4-7 stop
    losing throughput to background OS work/interrupts once isolated; cores 0-3 unaffected
    either way. `nohz_full=1-7` silently no-ops on this kernel (`CONFIG_NO_HZ_FULL` not set).
    `--rt-priority`'s independent contribution is unconfirmed. This is an operational/deployment
    recommendation (boot cmdline), not a code change. Full account, including two corrected false
    starts, in `docs/experiments/isolcpus-rt-priority-win.md`.

*   [ ] **New, real bug found the same night — not yet fixed**: with `isolcpus` set, running
    `armrx` without an explicit `--workers=N` silently picks 1 worker instead of 8 (confirmed
    live during a real overnight pool-mining run). `src/cli_parser.cpp:35`'s default
    (`std::thread::hardware_concurrency()`) reads the calling process's own affinity mask on this
    musl toolchain, which `isolcpus` restricts new processes to (core 0 only). Fix: use a true
    online-CPU-count method (e.g. parse `/sys/devices/system/cpu/online`) instead. Workaround
    until fixed: always pass `--workers=<N>` explicitly on an `isolcpus`-configured host.

*   [ ] **Second, deeper bug found the next night — not yet fixed, no workaround exists**: even
    with `--workers=8` passed correctly, a full overnight real-pool run with `isolcpus` active
    sustained only ~24.76 H/s, matching the *pre-isolcpus* baseline (24.9 H/s), not the 28.4 H/s
    benchmarked win. Root cause: this device has no `cpufreq` sysfs, so
    `detect_core_order()` (`src/mining_engine.cpp:82-109`) falls back to sequential order
    `[0..7]`; with `AffinityMode::All`, worker 0 lands on core 0 — the one core `isolcpus=1-7`
    leaves unisolated. Under real pool mining (unlike the local `--seconds=N` benchmark that
    measured the 28.4 H/s figure), core 0 also hosts the stratum reader thread, JSON/job
    handling, and the per-second console print, so worker 0 eats that contention for the whole
    run. `--workers=7` does **not** work around it (same modulo scheme still maps worker 0 to
    core 0). Real fix: have `MiningEngine` read `/sys/devices/system/cpu/isolated` and exclude
    non-isolated cores from the worker pool. See
    `docs/experiments/isolcpus-rt-priority-win.md`'s "Second footgun" section.

No genuinely open performance items remain as of 2026-07-26 (the gated plan's Step 1 result
above closed Steps 2-3). The two isolcpus-related deployment bugs above (worker-count default,
worker-to-core placement) are the only real open items project-wide. If performance work resumes
anyway, `docs/plans/experimental-performance-ideas-20260725.md` is the speculative, unmeasured
backlog to start from — `docs/plans/performance-plan-20260725.md`'s gated steps are all closed.

Everything from here down is Phase 6's history, kept as the short-list view of already-completed
work. See `PLAN.md` for the full evidence/reasoning behind each item.

---

## Major finding (2026-07-24): this device has two 4-core L2 clusters, not one 8-core cluster

Found while investigating "why can't armrx match XMRig's hashrate here" with a real XMRig run
on the same device — see `PLAN.md` Phase 6 item 3's "REVISED" section for the full evidence
chain. Short version:

- Kernel cache-topology sysfs (`/sys/devices/system/cpu/cpu{0,4}/cache/index2/shared_cpu_list`)
  shows **cores 0-3 share one L2, cores 4-7 share a separate L2** — contradicting `lscpu`'s own
  "Cluster(s): 1" self-report. **Confirmed (per postmarketOS wiki): the real chip is MSM8929 /
  Snapdragon 415**, a genuine big.LITTLE-shaped octa-core (4×1.1 GHz + 4×1.4 GHz Cortex-A53) —
  the "Lenovo MSM8916/Snapdragon 410" ID this project assumed since its earliest docs was wrong.
- Under full 8-worker contention, cores 0-3 sustain ~4.0-4.26 H/s each, cores 4-7 sustain a flat
  ~2.13 H/s each — a real, reproducible ~2:1 split (confirmed with a long 240s steady-state
  window after a short-window test's numbers turned out to be a measurement-quantization
  artifact from the 64-hash atomic-flush batching).
- **Not a static per-cluster difference**: pinning 4 workers exclusively to each cluster in
  isolation gives *identical* cycles/instructions/L2-refills between them (<0.01% apart). The
  split only appears when both clusters compete for the shared memory path simultaneously — a
  dynamic interconnect-arbitration effect, not a frequency or cache difference.
- **This retracts item 3's original "core-count-triggered power/current cap" hypothesis** — the
  apparent per-core clock drop at 6-8 workers was the average of a full-rate cluster and an
  arbitration-losing (not slower) cluster once the worker count spanned both.
- **Fully explains item 2's efficiency curve**: workers 1-4 map to cluster 0 alone (zero
  cross-cluster contention → 98.5% efficiency, the isolated peak); worker 5 is the first to land
  on cluster 1 and immediately eats the arbitration penalty (98.5%→89.2%, the sweep's single
  biggest step); workers 6-8 add the rest of cluster 1 at roughly half rate.
- **Quantified, cluster-normalized gap to XMRig** (same device, same job, same fallback mode):
  armrx is at **90.1%** of XMRig on cluster 0 (16.78 vs. 18.62 H/s) and **88.1%** on cluster 1
  (8.52 vs. 9.67 H/s) — a real, modest **~10-12% gap on both clusters independently**, not the
  vague "far behind" impression the raw aggregate (24.95 vs. 28.28 H/s) gave by itself.
- **Where the ~10-12% gap actually comes from** (same Cortex-A53 PMU events attached live to
  XMRig's own process, 8 threads, real pool job): **not a stall/scheduling problem** — armrx's
  IPC (0.731) and `ld_dep_stall`% (11.41%) are both *better* than XMRig's (0.612, 16.70%). It's
  an **instruction-count gap**: armrx needs ~33.5% more instructions per hash (132.93M vs.
  99.57M), which nets out to ~11.9% more cycles per hash — matching the cluster-normalized gap
  almost exactly. This is a real, measured instruction-count gap (not the old debunked 31%-
  branch-miss-era estimate), satisfying `PLAN.md` item 14's gate in spirit, though not yet
  region-scoped to specific opcodes/phases. See `PLAN.md` Phase 6 item 3 for the full comparison.
- **Region-scoped breakdown attempted (2026-07-24) — wrong region, both candidates fell apart,
  a negative result.** `--jit-dump`'s opcode boundary table showed memory-operand opcodes
  (`*_M`) at 37.13% of code bytes and `CBRANCH` at 19.34%, initially read as two concrete
  candidates for the instruction-count gap. Neither survived closer scrutiny: (1) this table
  only covers the *fixed, 2047-instruction main VM program* — measured instructions/hash is
  ~132.93M, so >99.998% of real instruction volume is elsewhere (almost certainly the
  superscalar/dataset-derivation path, invoked far more often in light mode, not covered by
  this dump); (2) `emitAddImmediate` already uses the tightest available encoding for this
  immediate range (checked by reading it, not assuming); (3) CBRANCH's condition tests an
  8-bit-wide field, not a single bit, so the `TBZ`/`TBNZ` fusion idea doesn't apply. **No code
  changed.** Finding the real gap needed new instrumentation for the superscalar path (item 13
  built this) — a binary-level XMRig comparison was also considered and is deliberately not
  pursued; see `PLAN.md`'s clean-room boundary note after Phase 6 item 13. See `PLAN.md`
  Phase 6 item 3 for the full correction and `changelogs.md` for the dated account.

---

## Current Telemetry Invariants (Cortex-A53, Light Mode, JIT)

**Re-baselined 2026-07-24 (Phase 6 items 1–2, done).** Fresh worker-count sweep data
(two passes, thermal-settled, `README.md` updated to match):

| Workers | Avg H/s | H/s/worker | Efficiency vs. 4.27 H/s single-thread |
|---|---|---|---|
| 1 | 4.27 | 4.27 | 100% (baseline) |
| 4 | 16.82 | 4.20 | 98.5% |
| 5 | 19.04 | 3.81 | 89.2% |
| 6 | 21.13 | 3.52 | 82.5% |
| 7 | 23.21 | 3.32 | 77.7% |
| 8 | 24.95 | 3.12 | 73.0% |

No plateau anywhere 4→8 — efficiency declines smoothly, 8 workers is still the highest-throughput
choice. The old 5.18 H/s / 25.28 H/s "linear scaling" figures were stale and did not reproduce
(see `docs/archived/plan_completed_phases_1-5.md` Phase 5's PGO finding — both PGO and non-PGO
measure identically at 4.27 H/s on current code). **Scope caveat**: each sweep point is a 60s
window; long-duration (15–30 min) sustained thermal throttling is a distinct, still-open question
(item 3, PMU attribution, below).

| Metric | Software AES Baseline (SW) | PGO + SW AES Optimized | Δ |
|--------|:-------------------------:|:----------------------:|:-:|
| **Init scratchpad** | 30,817 μs (12.35%) | 30,817 μs (12.35%) | — |
| **Get final result** | 23,207 μs (9.30%) | 23,207 μs (9.30%) | — |
| **Chain execution (VM)** | 194,692 μs (84.6%) | 170,860 μs (68.4%) | **−12.2%** |
| **Branch miss rate (aggregate, `bench_armrx` no flags)** | 31.6% | 31.08% | **not representative — see below** |
| **Branch miss rate (isolated `--full-hash-only`, i.e. the real mining hot path)** | — | **2.4%** | — |
| **IPC (isolated hot path)** | — | **0.708** (~35% of dual-issue peak) | — |

The aggregate 31.08% figure does **not** represent the mining hot path — see
`docs/experiments/branchless-cbranch.md`'s "The 31.08% figure does not represent the mining
hot path" section. It's 94.93% driven by `bench_armrx --attribution-only`'s
non-representative interpreted-mode comparison run. The real hot path's rate
(2.4%) costs only ~0.1–0.16% of cycles to misprediction — CBRANCH work has
been tried (CSEL, 2026-07-22) and closed; see the "Resolved" section below.
**IPC 0.708 is Phase 6's central fact**: this is a memory-latency-stall-bound
workload, not instruction-throughput-bound — see `PLAN.md` Phase 6 for why that
reframes every remaining performance lead.

---

## Prioritized Next Steps (PLAN.md Phase 6, 2026-07-24)

Synthesis of two independent master plans (`docs/plans/performance-master-plan.md`,
`docs/plans/performance-master-plan-20260724.md`) — see `PLAN.md` Phase 6 for the full
agree/diverge reconciliation. Nothing below has started yet.

### Short-term (hours, zero/low code risk — on-device only, do first)
*   [x] **Huge-page residency check — done (2026-07-24), result: no-op.** While mining: `AnonHugePages` covers 97.6% of anon RSS (100% for the 256 MiB Argon2 cache mapping specifically), despite `HugePages_Total: 0` (no real hugetlbfs pool) — THP's `always` policy is already coalescing almost everything. dTLB-load-misses: ~1.6 per million instructions (negligible). **The two contingent items below are closed as no-ops as a result.**
*   [x] **Worker-count sweep — done (2026-07-24), result: no plateau.** See the telemetry table above. 8 workers remains the highest-throughput choice; there is no free-lunch lower-worker-count default.
*   [x] **Multi-worker PMU attribution — done (2026-07-24), TLB rejected, "power cap" hypothesis retracted and replaced.** dTLB misses negligible at every worker count (<1.4/million instructions) — TLB is not a factor. L2-refill-rate/instruction and `ld_dep_stall` both rise steeply 1→4 workers (front-loaded memory contention) then flatten. The apparent per-core clock drop at 6-8 workers (~772→641→567 MHz) was **not** a power/current cap — see "Major finding" above: this device has two 4-core L2 clusters, and the drop is just the average of a full-rate cluster (0-3) and an arbitration-losing cluster (4-7) once the worker count spans both. See `PLAN.md` Phase 6 item 3's "REVISED" section for the full evidence chain.
*   [ ] `--rt-priority` + `isolcpus=`/`nohz_full=` — **blocked on manual device access (2026-07-24)**: `--rt-priority` needs `setcap`/`CAP_SYS_NICE` (not installed, no passwordless sudo); `isolcpus=`/`nohz_full=` need a kernel-cmdline edit + reboot (needs explicit user sign-off). Deferred until the user grants device access directly. Per item 3, don't expect this to touch the 6→8-worker clock-cap effect if it's a firmware power budget.

### Contingent on the huge-page check — closed, no-op (2026-07-24)
*   [x] ~~Disclose + prefault the Argon2 cache's huge-page fallback path~~ (`argon2.cpp:270`) — not needed, already ~100% THP-coalesced.
*   [x] ~~Reserve a persistent hugetlb pool at boot~~ (`vm.nr_hugepages`) — not needed, same reason.

### Medium-term (1-2 weeks, moderate risk — only after short-term verification lands)
*   [x] ~~Register-offset FP loads~~ — **closed 2026-07-24: stale claim, already implemented** (`JitCompilerA64::emitMemLoadFP()` already emits `ldr dN,[x2,tmp_reg]` directly; no `add`+`ld1` pattern exists anywhere in the file). Matches `ROADMAP.md` `O13`. No code change made.
*   [x] ~~Static FP load/convert software-pipelining~~ — **closed 2026-07-24: stale claim, already implemented** (the static prologue is already interleaved/pipelined, per its own comment). Matches `ROADMAP.md` `O12`. No code change made.
*   [x] ~~Superscalar literal-pool relayout~~ — **implemented, measured, reverted (2026-07-24), a real but small regression.** Branches dropped 12.1% as designed, but cycles rose +1.65%, IPC fell (0.761→0.748), and hashrate corroborated it (268→265 hashes/60s, −1.12%). Same "cost relocated, not eliminated" pattern as the Argon2 `memcpy` attempt — likely traded branch/I-fetch savings for worse D-cache locality on the relocated literal loads. Reverted (`git checkout -- src/jit_compiler_a64.cpp`), KATs re-confirmed 12/12. See `PLAN.md` Phase 6 item 9.
*   [x] ~~Prefetch A/B matrix~~ — **adopted 2026-07-24: real small win, confirmed via low-noise `perf stat` after wall-clock hashrate alone was inconclusive.** Removed all three `prfm` hints from `.Lmain_loop` (`jit_compiler_a64_static.S:381-383`, up to 16,384 executions/hash) permanently. Wall-clock hashrate (2 non-interleaved passes, then 3 interleaved passes) hovered at a suggestive but unconfirmed +0.4% (p≈0.07). Switching to `perf stat` cycles/instructions (much lower noise) settled it: +0.885% more instructions completed in a fixed 20s window, zero overlap between conditions, 7-20× signal-to-noise ratio. KATs/JIT-determinism/equivalence 12/12 on the final adopted build. See `PLAN.md` Phase 6 item 10 for the full three-round methodology writeup — worth reading as a lesson in when wall-clock benchmarking isn't sensitive enough.
*   [x] ~~Fused hash-and-fill nonce pipeline~~ — **closed 2026-07-24: benchmarked, failed its own gate.** New primitive-level benchmark added to `bench_armrx.cpp` (`--micro-only`): fused `hash_and_fill_aes_1r_x4` (59,124.84 μs) is ~3.6% *slower* than the two separate `hash_aes_1r_x4`+`fill_aes_1r_x4` calls it would replace (57,084.78 μs). No `mining_engine.cpp` integration attempted — the premise failed at the cheap benchmark stage, before any of the real (job-lifecycle bookkeeping) implementation risk. See `PLAN.md` Phase 6 item 11.
*   [x] ~~`IMUL_RCP` literal-load elimination (superscalar path)~~ — **closed 2026-07-25: implemented, measured, reverted, small real regression.** Replaced the literal-pool `LDR`+`MUL` with direct 64-bit immediate materialization (`MOVZ`/`MOVN`+up to 3x `MOVK`) to eliminate a load-to-use stall the correlation script's `IMUL_R`/`IMUL_RCP` finding pointed at. IPC improved as hypothesized (+8.2%), but instruction count rose ~8.5% and net cycles for identical completed work rose ~0.25-0.34% across three properly-controlled `perf stat` trials (incl. one reversed-order to rule out thermal drift) — stall savings almost exactly cancelled by the extra instructions' cost. Reverted; KATs/JIT-determinism/equivalence/encodings re-confirmed 5/5 green on the reverted build. A real, unrelated `CMakeLists.txt` bug (ASan flags `PRIVATE` instead of `PUBLIC` on `armrx_core`, never reaching consuming executables) was found and fixed along the way, kept independent of this item's outcome. See `PLAN.md` Phase 6 item 14 for the full account.

### Long-term (weeks, high risk — do not start without a measured hypothesis from medium-term items)
*   [x] ~~Conservative 2-3-instruction emitter lookahead scheduler~~ — **adopted, 2026-07-25.** First version (main VM program only) measured as a clean null (IPC +0.016%) plus a real +24.9% branch-miss cost — the actual dominant `IMUL_R`/`IMUL_RCP` cost (item 14) lives in the superscalar/dataset-derivation path, a separate JIT emission path the scheduler didn't originally touch. Extended to that path (`scheduleSuperscalarProgram()`, its own `IMUL_RCP` literal-pool-ordering hazard found by code reading and fixed via a Q/R exclusion). Two dedicated differential stress tests (450-pair main-program `tests/test_jit_scheduler_stress.cpp`, 200-pair/100-seed superscalar `tests/test_jit_superscalar_scheduler_stress.cpp`) plus the full existing suite all green. Final `perf stat` measurement (`taskset`-pinned, 2 reversed-order rounds, 6 samples/condition): full vs baseline IPC +0.233%, cycles −0.036%, consistent both rounds — smaller than the original 2-6% estimate but real and reproducible. See `PLAN.md` Phase 6 item 12.
*   [x] ~~Build real instrumentation for the superscalar/dataset-derivation path~~ — **done 2026-07-24.** Confirmed the mechanism by reading the assembly: `generateSuperscalarHash()` compiles once per seed rotation (`vm.cpp:175`), but the compiled code executes via `bl rx_calc_dataset_item` on *every* main-loop iteration in light mode — 2048 × 8 = **16,384 calls/hash**. Extended the `JitDumpEntry`/`--jit-dump` mechanism to cover this region (new `superscalar_jit_dump_`/`getSuperscalarJitDump()`, instrumented `generateSuperscalarHash()`, extended `dumpJitCode()`'s output). Verified 12/12. Real data: one call = 3,563 instructions/20,916 bytes → ×16,384 ≈ **58.4M instructions/hash, ~44% of the ~132.93M total** — a lower bound (fixed wrapper chunks and the main loop's own per-iteration overhead aren't tracked yet). See `PLAN.md` Phase 6 item 13 for the full per-opcode table and honest scope of what's still unreconciled.
*   [x] ~~Self-directed instruction-count reconciliation for the superscalar/dataset-derivation region~~ — **done (2026-07-24): static counting, live `perf record` profiling, and a real opcode-level correlation script all landed.** `perf record -g` on `./armrx --mine` first showed only ~14-15% of self-time in named C++ symbols (corrected the earlier hypothesis that the missing instructions live in C++ — AES/Blake2b are separately measured at 0.3%/0.0% of hash time, too small). New `tools/jit_correlate.py` then correlated raw `perf` sample addresses against item 13's `JitDumpEntry` offset tables (matching worker JIT buffers in `/proc/<pid>/maps` by size, splitting THP-merged multi-worker regions back into individual buffers): **63.42% of all cycles matched a specific superscalar opcode**, dominated by `IMUL_R` (20.98%) and `IMUL_RCP` (14.30%) — together over 35% of every cycle spent mining. 14.25% of samples fell outside any JIT buffer, cross-validating almost exactly against the earlier ~14-15% named-C++ estimate. No XMRig comparison, per `PLAN.md`'s clean-room boundary note. See `PLAN.md` Phase 6 item 14 for the full account, including two real tooling gotchas (a `pgrep -f` false match, and `perf script`'s call-graph output needing leaf-frame-only filtering) hit and fixed along the way.
*   [x] ~~Re-run `devbox_pgo_build` after any medium/long-term item lands meaningfully~~ — **done
    2026-07-25, still a confirmed null after the scheduler landed.** See the PGO entry in
    "Resolved — Phases 1–5" below for the full account, including the core-cluster measurement
    artifact caught and corrected mid-session.
*   [x] ~~Three independent code reviews of the emitter scheduler (Deepseek, Gemini, Hermes)~~ —
    **done 2026-07-25.** All confirm no constructible failure scenario; two of three
    independently caught a real doc-comment error (the `src==dst` exclusion's stated mechanism
    was factually wrong about `h_IROL_R`). Fixed the doc comment, added a fail-safe assert in
    `resolveInstructionType()`, documented why `num32bitLiterals=64` is load-bearing. Committed
    `c92a1a9`. Full reports in `docs/audits/`.
*   [x] ~~Full codebase audit (Deepseek)~~ — **done 2026-07-25.** Every concrete, checkable claim
    verified against actual code/live device state. No new bugs; one previously-open question
    (scratchpad huge-page residency) closed as a non-issue (verified live: merges with the
    Argon2 cache mapping into one 258 MiB region, 100% `AnonHugePages`); one small optional
    hardening items found (`-frounding-math`, CBRANCH assert) — both applied same day, see above.
*   [x] ~~Re-baseline `README.md`'s stale H/s figures honestly~~ — **done (2026-07-24)**, using the worker-count sweep data above.

**Not adopted / explicitly deprioritized:** full peephole-JIT coalescing without a fresh gap measurement; any further NEON/hardware-AES attempt (three independent measured regressions already); custom allocators/memory pooling (no hot-path allocation exists to pool).

### Backlog (deprioritized per explicit user direction, not deleted)
*   QEMU AArch64 GitHub Actions CI.
*   Stratum V2 protocol support.
*   `ARMRX_JIT_FAST_DIV_SQRT` CMake flag centralization (low priority — the flag that
    actually caused a crash is already `PRIVATE`).
*   `tls_client.cpp`/`tui.cpp` test coverage (need a mock TLS server / terminal-capture
    harness respectively — bigger lift than the `cli_parser.cpp`/`aes_hash.cpp` work already
    done).

---

## Resolved — Phase 6 (completed 2026-07-25)

*   [x] **Superscalar literal-pool relayout (item 9), 2026-07-24 — implemented, measured,
    reverted.** Real regression: cycles +1.65%, IPC 0.761→0.748, hashrate −1.12% (268→265
    hashes/60s), despite branches dropping 12.1% exactly as designed. Reverted; KATs
    re-confirmed 12/12 on the restored code. See `PLAN.md` Phase 6 item 9 for the full account.
*   [x] **Items 7-8 (register-offset FP loads, static FP load/convert pipelining), 2026-07-24 —
    both stale claims, already implemented in an earlier phase** (`ROADMAP.md` `O13`/`O12`).
    No code change made or needed; verified by reading the actual current code before touching
    it, the same discipline that's caught several other stale claims this project's history.
*   [x] **Multi-worker PMU attribution (item 3), 2026-07-24 — two layered mechanisms, TLB rejected.**
    Cortex-A53-specific PMU events (`l1d_cache_refill`, `l2d_cache_refill`, `ld_dep_stall`) at
    1/2/4/6/8 workers. dTLB misses negligible everywhere (rules out TLB). L2-refill-rate/instruction
    and `ld_dep_stall` both rise steeply 1→4 workers then flatten (front-loaded memory contention).
    A separate, real per-core clock reduction (derived from cycles/wall-time, since `scaling_cur_freq`
    isn't exposed) onsets at 6+ workers (~772 MHz flat at 1/2/4 → ~641 MHz at 6 → ~567 MHz at 8)
    while thermal-zone temps stay mild (36–50°C) — more consistent with a core-count-triggered
    power/current cap than classic thermal throttling. See `PLAN.md` Phase 6 item 3 for the full table.
*   [x] **devbox MCP tooling: two real bugs found and fixed while running items 1–2, 2026-07-24.**
    `tool_test()` used the 120s `"default"` timeout instead of a bench-scale one (always timed
    out on the full ~400s+ suite) — added a dedicated `"test"` bucket (900s). Separately, the
    single-threaded blocking server couldn't answer the host's liveness `ping` during any long
    call, causing mid-call disconnects — fixed by running `tools/call` dispatch in worker
    threads (with a `_device_lock` still serializing actual device operations). Verified: a real
    `devbox_test` call ran 612.72s (12/12 passed) with the connection staying up throughout.
    See `PLAN.md` Phase 6 for the full account.

---

## Resolved — Phases 1–5 (full detail: `docs/archived/plan_completed_phases_1-5.md`)

*   [x] `MiningEngine::worker_loop()` bad-nonce-job worker death — `active = false; return;` → `active = false; continue;`, regression test added.
*   [x] `config.cpp` numeric config-file parsing crash — `try`/`catch` guards added around `workers`/`difficulty`/`seconds`/pool-port, matching `cli_parser.cpp`'s CLI-flag treatment, regression test added.
*   [x] `MetricsExporter::server_fd_` data race — now `std::atomic<int>`.
*   [x] `cli_parser.cpp` test coverage — new `tests/test_cli_parser.cpp`; found and fixed a real bug (`--config=` always exited with "Unknown argument", code 64).
*   [x] `aes_hash.cpp` direct helper test coverage — new `tests/test_aes_hash.cpp` (golden pins + `hash_and_fill_aes_1r_x4` decomposition-equivalence check).
*   [x] Argon2 NEON diagonal-step vectorization — 26.8% fewer instructions, 19.0% fewer cycles for `Argon2dCache::initialize()` (seed-key-rotation latency, not sustained hashrate). See `docs/experiments/argon2-neon-diagonal-vectorization.md`.
*   [x] Argon2 `memcpy` copy-elimination — implemented, measured, reverted (no net win, cost relocated not eliminated). See `docs/experiments/argon2-compress-copy-elimination.md`.
*   [x] CBRANCH branch-misprediction work — CSEL implemented, measured, and reverted (net regression); root-caused the 31.08% figure to a non-representative benchmark section, not the mining hot path. See `docs/experiments/branchless-cbranch.md`.
*   [x] On-device LTO link regression (Alpine `fortify-headers` + GCC LTO incompatibility) — root-caused and fixed, `CMakeLists.txt`.
*   [x] Both documented pool-failover gaps (dead-at-startup pool never failing over; up to ~30s stale-reconnect-thread-join delay) — `src/pool_manager.cpp`, `src/stratum_client.cpp`.
*   [x] AES round-key constants consolidated into `include/armrx/aes_keys.hpp`.
*   [x] Scratchpad L3 mask constants unified into `include/armrx/randomx_config.hpp`.
*   [x] `kCompileHandlers[256]` derived from `instruction_weights.hpp` instead of hand-maintained.
*   [x] PGO devbox wiring (`devbox_pgo_build`) — tool shipped and mechanically correct (also fixed a real tilde-expansion bug affecting the whole devbox toolchain), but the claimed +19.3% payoff did **not** reproduce on current code (measured identical 4.27 H/s PGO vs. non-PGO). **Re-checked 2026-07-25 after the scheduler landed (PLAN.md item 15) — still a confirmed null** (`taskset`-pinned, same-core: 4.47 vs. 4.48 H/s). An unpinned first attempt showed a misleading ~2x gap that turned out to be this device's core-cluster clock asymmetry, not PGO — any future re-check must pin to a specific core. Tool kept for future re-evaluation.
*   [x] NEON `vtbl`/`vqtbl1q`-vectorized software AES — derived from scratch, exhaustively verified correct (256/256 S-box match, 20,000-trial parity, first-try-correct on hardware), measured as a real ~19.4% regression. Kept flag-gated (`ARMRX_ENABLE_NEON_AES`, default OFF) as reference. See `docs/experiments/neon-vector-permute-aes.md`.
*   [x] `--stagger-ms` default — confirmed already tested in an earlier session (`docs/archived/beyond-parity_v2.md`, 4 stagger values, "+0%, hardware ceiling"), not re-run; default (0) left unchanged.
*   [x] `MiningEngine` worker-thread reuse for dataset initialization — also surfaced and fixed the fast-mode dataset-corruption bug (`docs/postmortems/fast-mode-dataset-corruption-postmortem.md`).
*   [x] Mock Stratum socket integration tests — `tests/test_pool_protocol.cpp`, 7 scenarios — also surfaced and fixed the `PoolManager` self-deadlock (`docs/postmortems/pool-failover-deadlock-postmortem.md`).
*   [x] JSON parser fuzzing — `tests/fuzz_json.cpp`, 2.5M+ executions, zero findings.
*   [x] NEON `permute_block_neon` benchmarking — ~16% faster than scalar, enabled.
*   [x] JIT buffer overflow safety — root-caused segfaults to `PUBLIC` flag leaks under GCC 15 LTO; retained doubled 32,768-byte buffer as defense-in-depth.
*   [x] PGO linker errors — unblocked compiler profile linkage across Alpine/musl and GCC 15.2.0.
*   [x] `MetricsExporter` thread-detach use-after-free — joins on destroy instead of detaching.
*   [x] `main.cpp` split into `CommandLineParser` + `MinerApp`.
*   [x] JIT buffer W^X vs RWX default — kept RWX (explicit user direction), now disclosed via a startup log line instead of silent.
*   [x] CTest path caveat — re-verified 2026-07-22: did not reproduce in any on-device run (all 7 tests pass via plain `ctest` every time). If this recurs, re-add a note here with exact reproduction steps.
