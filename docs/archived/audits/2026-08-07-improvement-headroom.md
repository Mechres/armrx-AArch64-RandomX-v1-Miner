# Improvement Headroom — where real gains still are (2026-08-07)

**Scope:** grounded review of where armrx can still improve, after the codegen/JIT perf
program was closed. Read end-to-end: `src/superscalar.cpp`, `src/mining_engine.cpp`,
`src/cpu_features.cpp`, `src/cli_parser.cpp`, `docs/experiments/next-iteration-plan.md`
(E1/E11/E12/E13/E15/E16/E19/E22), `docs/experiments/m1-miner-to-miner-pmu-diff.md`, and
diffed `src/superscalar.cpp` vs the upstream reference in `scratch_vm_study/upstream_rx/src/`
(line-by-line). Cross-checked against `git log --all`. NO code changes made; this file is
NOT committed.

**Headline:** the project is at parity (1w beats XMRig 5.11 vs 5.04; 8w 26.65 vs 28 = 95.2%).
The perf program is closed ONLY on the JIT-emission / codegen axis. One genuine, *unmeasured*
code lever remains (the superscalar timing model), plus three cheap deployment levers that are
already wired into the CLI but were never A/B'd.

---

## 🟢 THE real unmeasured lever — superscalar timing model (E19 → E22 → generator)

### What the data already shows
- E19 localized the 1-worker gap precisely: `other_interlock_stall` **2.12× XMRig**
  (+12.30 M cycles/hash = 154% of the gap) = **A53 integer-multiplier interlocks** on the
  dependency-dense superscalar body (~35% multiplies). The gap is **stalls/IPC, not
  instruction count** — armrx actually emits ~12% FEWER instructions than XMRig but loses on
  IPC 0.547 vs 0.654.
- E22 ruled out the **emitter** scheduler (identity order = H/s unchanged; the scheduler only
  hides ~12 M instr of latency-fill, density-positive, no regression). That pushed the frontier
  *upstream* to `src/superscalar.cpp` — the generated program order.

### What I just verified (clean-room, no code copy)
- Diffed armrx's `src/superscalar.cpp` against the upstream reference in-tree
  (`scratch_vm_study/upstream_rx/src/superscalar.cpp`):
  - `selectDestination` armrx (line 432-434) vs upstream (line 510): **byte-for-byte identical**
    — same multiply-anti-chaining (`lastOpGroup != IMUL_R`), same `allowChainedMul` fallback,
    same `IADD_RS` r5 exclusion.
  - `selectSource`, `fetchNext`, `mulCount` steering: **identical**.
  - ⇒ armrx's generator is a faithful port. **Register selection is NOT the delta.** The
    "generated program order differs from XMRig" TOP UNTESTED LEAD collapses: register choice is
    the same; XMRig's own AArch64 build uses the same x86 generator yet emits 2× better
    interlocks. So the difference is in how the model's *cycle counts* map to A53 silicon.

### The actual delta — the timing/port model
- armrx's `MacroOp` latencies (`superscalar.cpp:85-106`) and `scheduleUop`
  (`superscalar.cpp:497+`) model **x86 ports**: P0/P1/P5, `Imul_rr` latency 3, `Imul_r`/`Mul_r`
  latency 4, dual-issue P015.
- The **A53 is a single multiplier, 3–4 cycle latency, ~1 issue per 3 cycles**, 2-int-issue/cyc.
- The generator's `decodeCycle`/`depCycle` steering is tuned for a 3-port x86 core, NOT a
  1-mul-A53. `RANDOMX_SUPERSCALAR_LATENCY` decode limit + `mulCount`-gated `fetchNext` pick
  instruction spacing using the wrong microarchitecture's cost model → multiply-to-multiply
  spacing is not tightened for the A53's real single-port multiply bottleneck.

### The lever
- Build an **A53-accurate timing model** into the generator: single MUL port, 3–4 cyc latency,
  ~1 mul per 3 cyc; 2 int issues/cyc; correct `MacroOp` latencies for A53. Re-run generation and
  measure 1w/8w H/s + `other_interlock_stall` via the gated `--perf-ready` harness.
- **Why this is the highest-value untried lever:** it attacks the *exact* stall class E19 named
  (multiplier interlocks), is a *model* change (the generated program is data, not executable —
  KAT byte-identity is unaffected by cycle-count tweaks, so gating is cheap), and is the one
  remaining place upstream of emission. Ranked AHEAD of cross-LTO (~+1.9%, crash risk) which I
  previously flagged as "the" untried probe — this is bigger and more principled.
- **Gate:** full KAT set (test_jit_equivalence 16/16, scheduler stress 450/200, determinism,
  2way) + 1w/8w H/s A/B. One device session after `devbox_sync`. No emitter/`*_M` change — W3-2
  stands, do NOT touch memory-op scheduling.

---

## 🟡 Cheap deployment levers (wired into CLI, never measured at 8w)

All exist in code; the experiment log shows none were A/B'd on this device at 8w:

### D1. `--stagger-ms=N` (mining_engine.cpp:426)
- Desyncs per-worker startup by `thread_id * stagger_ms` to break simultaneous scratchpad/multiply
  traffic across the shared two-cluster interconnect.
- Built *for* the exact contention the 8w complaints describe. **Untested.** Cheap A/B:
  `--pool-test --stagger-ms=N` sweep at 8w.

### D2. `--rt-priority` (mining_engine.cpp:413, SCHED_FIFO)
- Workers at RT priority. On non-isolcpus, a worker sharing core 0 with the stratum/console
  main thread is the repeatedly-floated 8w loss (README CAUTION: isolcpus + pool only matched
  benchmark, not pool, because main thread lands on core 0). RT priority removes that preemption.
- **Untested.** Needs CAP_SYS_NICE (falls back gracefully, already handled). One gated run.

### D3. Affinity-mode + main-thread core-0 deprioritization
- `--affinity-mode=BigOnly`/`Unpinned` (cli_parser.cpp:163-172) + deprioritizing the main/stratum
  thread off core 0 in pool mode.
- ROADMAP explicitly deferred the core-0 contention fix as "speculative, ~+2-4% at 8w, risk of
  regression." It was **never actually measured** with a `--pool-test` A/B. The rigorous test is
  real-pool `--pool-test` A/B (isolated-core main vs not), not a bench change.

---

## 🔴 Correction to earlier audit (supersedes the "LTO is the only probe" framing)
- Earlier audit listed "Lever 4 cross-LTO" as THE untried perf probe. It is untried, but
  ~+1.9% and crash-risk — small. The **superscalar timing-model lever (above) is bigger and
  more principled** and should be ranked ahead of it.
- The experiment log (more current than STRATEGY.md) **retracts** the "79%→90% scaling gap"
  framing: 8w scaling is *linear per-core*; the weak-cluster 0.53× is the SoC's own ratio that
  XMRig also bears (XMRig fast 4.5 / weak 2.4). So the only code gap is **per-instruction IPC
  (interlocks), not scaling** → points at the generator timing model, NOT at threading. The D1-D3
  levers above are worth a *cheap* sweep because they address the non-isolcpus pool-specific
  contention that the parity numbers (which include isolcpus) don't capture.

---

## RECOMMENDED NEXT SESSION
1. **Superscalar timing-model A53 re-tune** (the real lever): model change + KATs + 1w/8w H/s +
   `other_interlock_stall` A/B. Gated, one device session after `devbox_sync`.
2. **Cheap sweep** in the same session window if (1) is null or as a parallel investigation:
   `--stagger-ms`, `--rt-priority`, `--affinity-mode` — each a single `--pool-test` run.

---

## APPENDIX — runtime tuning sweep (measured 2026-08-05, HEAD 2899f12, on-device)

**Hardware state during sweep:** MSM8929/Snapdragon 415, no `isolcpus` (off ≥100 commits),
no cpufreq (`/proc/cpuinfo` max freq empty → every core reads 0), ~58–60°C under load,
8× Cortex-A53, light mode. All runs = real-pool `--pool-test` (same pool/wallet), 90–120s.

### Results (aggregate = whole-run summary H/s; live = 10s rolling-window Speed: line)

| Config | Aggregate | Live Speed | Fast/wkr | Weak/wkr | Verdict |
|---|---:|---:|---:|---:|---|
| Baseline (8w, pinned, `AffinityMode::All`) | 24.75 | 26–27.4 | ~4.0 | ~2.15 | reference (120s) |
| 7w (cores 0–6, core 7 excluded via `taskset -c 0-6`) | 22.10 | 24.5–25.5 | ~3.95 | ~2.10 | −2.65 = exactly one weak-cluster core; no contention relief on siblings |
| `--affinity-mode=big-only` | 22.49 | 27.11 | ~3.7 | ~2.0 | **NO-OP on this device** (see below) |
| `--affinity-mode=unpinned` (OS schedules) | 22.39 | 26.75 | mixed | mixed | **worse + uneven** — OS scatters workers poorly across clusters |
| `--stagger-ms=50` | 24.38 | 27.33 | ~3.95 | ~2.13 | flat vs baseline (within thermal noise) |
| `--rt-priority` (SCHED_FIFO) | 24.35 | 26.24 | ~4.0 | ~2.10 | flat vs baseline (within thermal noise) |

### Key findings
1. **`--affinity-mode=big-only` is a no-op on this hardware.** Code path
   (`mining_engine.cpp:393`): pins to `core_order_[thread_id % big_core_count_]`.
   `big_core_count_ = count_top_frequency_cores(core_order_)` returns `core_order_.size()`
   (=8) because all cores report freq 0 (no cpufreq). So `BigOnly` = `All` here — it cannot
   isolate the fast cluster. Drop from the runtime toolkit for no-cpufreq devices.
2. **`--affinity-mode=unpinned` is strictly worse** (22.39 vs 24.75) and uneven — the default
   sequential pin (`AffinityMode::All`, `mining_engine.cpp:403`) is correct.
3. **`--stagger-ms=50` and `--rt-priority` are flat** — aggregate 24.38 / 24.35 vs 24.75
   baseline. All runs hit 60°C (the memory-stall cliff), so deltas are within run-to-run
   thermal noise. Neither shows a real win. The live 10s Speed (27.3) is the rolling-window
   number; the aggregate is the true whole-run rate and ≈ baseline.
4. **No runtime flag beats the flat ~26.7 live / ~24.8 aggregate.** The weak-cluster penalty is
   silicon (interconnect arbitration between the two L2 clusters), not scheduling-fixable
   without `isolcpus`.
5. **Excluding core 7 (7w) costs exactly one weak-cluster core (~2.65 H/s)** with zero effect on
   the other cores — confirms the per-core weak-cluster penalty is independent, not aggregate
   crowding from a sibling.

### Conclusion of the sweep
On this non-isolcpus, no-cpufreq device, the affinity / stagger / rt-priority runtime knobs are
**all flat to slightly negative** vs the default pinned 8-worker config. The only lever that ever
moved the number materially was `isolcpus=1-7 rcu_nocbs=1-7` (the project's own ~14% win, now
disabled for 100+ commits) — and that is a **kernel/boot cmdline change, not a runtime flag**.
Therefore:
- The **runtime tuning surface is exhausted at parity** — nothing left to gain there.
- The **superscalar timing-model re-tune** (§"THE real unmeasured lever") remains the ONLY
  untried *code* lever; everything else (stagger/rt-priority/affinity) has now been measured and
  found null. This re-confirms the audit's ranking: timing-model first, LTO second, runtime
  flags closed.
- If you want to recover the ~14%, it is a **boot-config** change (`isolcpus`), not a code or
  runtime-flag change — and it is the user's call (kernel reboot, core-0 housekeeping tradeoff).

### Build/repro notes (for re-running the sweep)
- HEAD cross-build: `rm -rf build-cross && cmake -S . -B build-cross
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake -DARMRX_DISABLE_LTO=ON &&
  cmake --build build-cross -j$(nproc)` → `build-cross/armrx`.
- Historical `7047cfc` (first rolling-window commit) was built in a git worktree
  (`git worktree add ../armrx-at-7047cfc 7047cfc`; needs a `.git_sha` file written because that
  commit's CMakeLists lacks the HEAD guard) — confirmed throughput-identical to HEAD (rolling
  window is display-only).
- Device binary path: `/tmp/cross/armrx` (executable; distinct from the NOEXEC staging tmpfs).
- Pool-test: `--pool-test --pool=tr.monero.herominers.com:1111 --wallet=<addr> --workers=8
  --seconds=90`.

**Bottom line:** the project is at parity and the JIT/codegen frontier is exhausted. The genuine
remaining code lever is the **superscalar generator's timing model** (it models x86 ports, not the
A53's single 3–4 cyc multiplier, and that mismatch is the most likely cause of the 2.12× interlock
excess E19 measured). Everything else is either a cheap deployment A/B (stagger/rt-priority/
affinity) or already closed on evidence.
