# Combined Audit — armrx Next Steps (2026-07-31)

> **Generated from two independent audits:**
> - AGY (Gemini 3.1 Pro High) — `agy -p` audit against the full codebase
> - Reasonix (DeepSeek V4 Pro) — 125,973-token read-only codebase analysis
>
> All 10 performance tracks (A–J) completed or closed. 2 adopted, 8 closed on evidence.

---

## Honest Bottom Line (Both Auditors Agree)

The codebase is **unusually well-characterized**. armrx achieves *better* IPC than XMRig (0.731 vs 0.612). The remaining ~10-12% gap is entirely an instruction-count deficit (~33.5% more instructions/hash), not a stall/branch/miss issue. The main VM program region has a ~2.2× IPC penalty that's 94% architectural (dependency-chain depth on an in-order core), not fixable by code changes.

**"There is effectively no headroom left on the microarchitectural (IPC) axis."** — AGY
**"The 94%-architectural ceiling on the main VM program stall is real."** — Reasonix

---

## Prioritized Next Steps

### Tier 0 — Trivial, Highest Impact, No Risk

#### T0-1. Enable Track G (NEON T-table AES) by Default — ✅ DONE (2026-08-01)
- **What:** Flip `ARMRX_ENABLE_NEON_TTABLE_AES` from OFF to ON in `CMakeLists.txt`. Code already implemented, KAT-verified (10,000-trial parity), microbenchmarked at +28.8% AES throughput.
- **Projected gain:** ~3.6% hashrate (12.3% of cycles × 28.8% speedup)
- **Risk:** Essentially zero — transforms unchanged, only data-movement is NEON-vectorized.
- **Effort:** 5 minutes. One line in `CMakeLists.txt`.
- **Verification:** `devbox_build -DARMRX_ENABLE_NEON_TTABLE_AES=ON`, then `devbox_full` (sync→build→test→bench). Compare hashrate at `--warmup=60 --seconds=180`.
- **Status:** Done. Flipped ON by default (commit 4888ba1); on-device `test_aes_hash` passed on Cortex-A53 (NEON path — host x86_64 only exercises scalar fallback).

#### T0-2. Fix `hardware_concurrency()` Under `isolcpus`
- **What:** When `isolcpus=1-7` is active, `std::thread::hardware_concurrency()` returns 1 (the unisolated core 0) under musl, silently dropping worker count from 8 to 1. Read `/sys/devices/system/cpu/online` instead.
- **Projected gain:** Prevents **87% throughput loss** on isolated-kernel deployments. Not a hashrate win but a footgun fix.
- **Risk:** Very low — replacing one syscall with a `/sys` read.
- **Effort:** ~1 hour.
- **Files:** `src/cli_parser.cpp:35` (primary), `src/mining_engine.cpp:35,236` (secondary).

---

### Tier 1 — Already Implemented, Just Needs Measurement

#### T1-1. Benchmark Track D2 (Cross-Hash Boundary Pipelining) On-Device
- **What:** Track D2 is implemented and KAT-verified on host but never benchmarked on-device. Overlaps AES finalization of hash N with AES fill of hash N+1.
- **Projected gain:** ~0.5–1% hashrate
- **Risk:** Low (code already correct on host). Dependencies: needs light-mode benchmark harness.
- **Effort:** 2-4 hours for harness, then measurement.
- **Gate:** Create light-mode D2 microbenchmark in `bench_armrx.cpp` (`--bench-d2-pipeline` flag) that tests `hash_and_fill_aes_interleaved_x4` in isolation against sequential pair. Then `perf stat` A/B.

#### T1-2. Clean `perf stat` Measurement Path for Full-Hash Benchmark
- **What:** `bench_armrx --full-hash-only` conflates setup cost with per-hash measurement (master plan found 2.77× discrepancy). Add `--perf-ready` flag that signals readiness AFTER warmup.
- **Projected gain:** Enables trustworthy measurement for all future work.
- **Risk:** Low — new flag in benchmark tool, touches no production code.
- **Effort:** 4-8 hours.
- **Files:** `tests/bench_armrx.cpp`.

---

### Tier 2 — Worth Trying, Cheap to Fail

#### T2-1. Scratchpad Read-Prefetch via Register-Only Address Computation
- **What:** Insert `PRFM PLDL1KEEP` hints for scratchpad loads whose address is computed from registers 2-3 instructions ahead of use. No instruction reordering — pure hint insertion.
- **Why different from the failed memory-op scheduler extension:** That reordered *entire instruction emissions* across memory-op boundaries. This never changes code semantics, only prefetcher hints.
- **Projected gain:** ~0.1–0.5% (6% of the 2.2× IPC penalty is latency-driven — this targets a fraction of that).
- **Risk:** Low — no correctness risk, no reordering.
- **Effort:** 4-8 hours.
- **Files:** `src/jit_compiler_a64.cpp`, `emitMemLoad<>` and `emitMemLoadFP<>` templates.
- **Measurement:** `perf stat -e cycles,instructions,l1d_cache_refill` on `bench_armrx --full-hash-only`, taskset-pinned reversed-order trials.

#### T2-2. Dual-Issue-Aware Instruction Alignment (Track E/F2)
- **What:** Align long-latency op emissions to 8-byte boundaries so the following instruction dual-issues in the stall shadow. Cortex-A53 can dual-issue instructions at 8-byte alignment with independent functional units.
- **Projected gain:** ~0.1–0.5%
- **Risk:** Low-Medium — no correctness risk (instruction semantics unchanged). Null result is probable.
- **Effort:** 1-2 days.
- **Files:** `src/jit_compiler_a64.cpp` — insert NOP/`AND xzr, xzr, xzr` padding in `emit32()` calls.

#### T2-3. Worker/Core-0 Remapping Under `isolcpus` — ✅ DONE (2026-08-01)
- **What:** Read `/sys/devices/system/cpu/isolated` and exclude core 0 from worker pinning set. Under `isolcpus=1-7`, worker 0 maps to core 0 (unisolated) and contends with stratum/main thread, nullifying the isolcpus win in pool mining.
- **Projected gain:** Recovers ~14% isolcpus win for pool mining.
- **Risk:** Low-Medium. Losing worker 0 from isolated cores is better than worker 0 sharing core 0 with main thread.
- **Effort:** 4-8 hours.
- **Files:** `src/mining_engine.cpp:80-109` (core ordering), `src/mining_engine.cpp:333-351` (worker pinning).
- **Status:** Implemented. `filter_to_isolated()` applied to ALL `detect_core_order()` return paths (hwloc + sysfs, including the cpufreq-less fallback that originally skipped the filter — verified bug: worker 0 on core 0, core 7 idle on MSM8929). Default worker count capped to isolated count. On-device verified: workers pinned 1-7, main thread core 0, core 7 in use.

---

### Tier 3 — Design First, Do Not Implement Yet

#### T3-1. Track C Retry — Custom ABI for Dataset-Item Helper
- **What:** Re-attempt inline dataset-item helper. Use custom register allocation so `rx_calc_dataset_item` XORs results directly into live VM registers instead of saving/restoring 14 GPRs and relaying 64 bytes through memory.
- **Projected gain:** ~1–3% (16,384 calls/hash × eliminated frame overhead)
- **Risk:** **HIGH** — the first attempt hung on-device with unexplained divergence. The documented next diagnostic step (compare actual computed dataset-item values against reference) has never been run.
- **Gate:** Must run the documented diagnostic first before any re-attempt. See `docs/experiments/light-mode-dataset-item-prologue-attempt.md`.
- **File:** `src/jit_compiler_a64_static.S` (prologue/epilogue), `src/jit_compiler_a64.cpp`.

#### T3-2. Conservative Load Hoisting (Memory-Op Address Only)
- **What:** Hoist only the *address computation* (add-immediate + mask) for scratchpad memory ops, keeping the actual `LDR`/`STR` at its original position. Narrower than the failed full-memory-op swap.
- **Projected gain:** ~0.2–1%
- **Risk:** **HIGH** — the original memory-op scheduler extension failed with an unexplained JIT/interpreter divergence. The scratchpad address register (x2) is live and shared across memory ops.
- **Gate:** Requires: (a) specific hazard model for x2, (b) bisection harness (swap-count budget), (c) plan to abandon immediately on first test failure.
- **Do not attempt without all three gates met.**

#### T3-3. NEON Multiply Offload Analysis (Track F3 Follow-on)
- **What:** Determine if RandomX IMUL_R (64×64→64) ops can be transformed into lane-parallel 32×32→32 NEON ops. F3 confirmed NEON MUL V.4S at ~1 CPI (same as scalar, 4× throughput) but `umull v.2d` (schoolbook 64-bit) was worse.
- **Gate:** Opcode-frequency analysis first — extend `bench_opcodes` to report IMUL_R operand magnitudes. Only proceed if ≥30% of IMUL_R instances have one operand ≤ 2^32.
- **Status (corrected 2026-08-01):** gate has NOT been run. The previous "✅ gate check done" mark (added in f5c60dd, a T2-3 commit) was erroneous — `bench_opcodes.cpp` has no magnitude reporting and no gate writeup exists. **Spec caveat:** IMUL_R operands are runtime register values (the instruction has no immediate), so the gate cannot be answered by static program-generation sampling — it requires instrumented execution (e.g. hooking `h_IMUL_R` to record src/dst values across many seeds/programs). IMUL_RCP, by contrast, has a static 32-bit divisor by construction — but its reciprocal constant is ~full-width, so the F3 schoolbook analysis already ruled that route out.
- **Effort:** 1-2 days analysis, no implementation until analysis passes gate.

---

### Novel Angles (One Auditor Only)

#### From AGY (Gemini 3.1 Pro)

**N1. `ldp`/`stp` Fusion —** Coalesce adjacent 8-byte scratchpad ops into 16-byte load/store pairs when the JIT identifies two adjacent `IADD_M` or `ISTORE` ops targeting adjacent scratchpad boundaries. Cuts memory instruction count in half for those blocks. Not tried. No evidence it's feasible — depends on whether the JIT can statically prove adjacency.

**N2. Monolithic Register Pinning (No-ABI Approach) —** Abandon AAPCS entirely for the mining thread. Write a single outer assembly trampoline that allocates all 31 AArch64 registers globally for one hash lifecycle. Eliminates 100% of `stp`/`ldp` frame overhead. Extremely high effort, extremely high risk, but potentially the only way to close the instruction-count gap.

**N3. Blake2b NEON Vectorization Check —** Verify `blake2b.cpp` is actually using NEON for 128-bit mixing rounds. Standard C++ Blake2b often underutilizes NEON registers.

**N4. AArch32 (Thumb-2) Execution State —** If the kernel supports it, compiling in 32-bit ARM/Thumb-2 mode increases code density. Might improve L1I cache utilization on the memory-latency-bound main VM program. Loses expanded 64-bit register file.

**N5. Black-Box Binary Analysis of XMRig —** Compile XMRig and run `objdump`/`perf annotate` on the *binary* to count AArch64 instructions emitted per RandomX construct, without reading source code. Could identify the 33.5% instruction gap's origin. Clean-room legality depends on jurisdiction — consultation recommended before attempting.

#### From Reasonix (DeepSeek V4 Pro)

**N6. BOLT Post-Link Optimization —** Binary layout optimization using real `perf record` profiles. Expected null because 84%+ of cycles run from JIT buffers, not the static binary. Worth a quick try only to close the question (~1 day, no correctness risk).

**N7. Per-Cluster Frequency/Governor Tuning —** If `cpufreq` sysfs were available, pin slow cluster (cores 4-7) to higher minimum frequency. Not possible on this device (no `cpufreq` sysfs). Operational/deployment, not code.

**N8. Hybrid JIT/Interpreter for Main VM Program Only —** Both auditors agree this contradicts evidence (94% ceiling says stall is dependency-chain depth, not something dispatch bubbles would fix). Kept as "educational, not actionable."

---

## Quick-Look Ranking

| # | Idea | Gain | Risk | Effort | Ready? |
|---|------|------|------|--------|--------|
| **1** | Enable Track G (T0-1) | **~3.6%** | Zero | 5 min | ✅ **Done** |
| **2** | Fix `hardware_concurrency()` (T0-2) | Prevents 87% loss | Low | 1 hr | ✅ **Done** |
| **3** | Benchmark D2 (T1-1) | ~0.5–1% | Low | 2-4 hr | ✅ **Done** (device A/B 2026-08-01 — `--bench-d2-pipeline`, −2.77% interleaved, ~0.34% E2E; see `docs/experiments/t11-d2-microbenchmark.md`) |
| **4** | Clean perf stat path (T1-2) | Enables all work | Low | 4-8 hr | ✅ **Done** (device-verified 2026-08-01 — see `docs/experiments/t12-perf-ready-first-run.md`) |
| **5** | PRFM hints (T2-1) | ~0.1–0.5% | Low | 4-8 hr | ❌ **Closed — regression** (device A/B 2026-08-01: +0.26–0.50% cycles, +0.47% instructions, l1d unchanged — no prefetch effect on in-order A53; corroborates 2026-07-24 fill-loop removal, see `docs/experiments/t21-prfm-hints.md`) |
| **6** | Worker/core-0 remapping (T2-3) | Recovers ~14% | Low-Med | 4-8 hr | ✅ **Done** |
| **7** | Dual-issue alignment (T2-2) | ~0.1–0.5% | Low-Med | 1-2 days | ❌ **Closed — regression** (device A/B 2026-08-01: cycles +0.13/+0.20%, instructions +0.47% NOP overhead, IPC +0.3% — alignment *worked* but net negative; see `docs/experiments/t22-dual-issue-alignment.md`) |
| **8** | Track C retry (T3-1) | ~1–3% | **High** | Days-weeks | **Blocked** |
| **9** | Load hoisting (T3-2) | ~0.2–1% | **High** | 2-4 days | **Design only** |
| **10** | NEON mul analysis (T3-3) | ~0–2% | Analysis | 1-2 days | ⚠️ **Gate NOT done — status corrected 2026-08-01** (the earlier "✅ gate check done", added in f5c60dd, was an error: bench_opcodes was never extended and no analysis writeup exists; gate requires instrumented IMUL_R operand-magnitude sampling — runtime values, not static program generation) |
| **11** | Novel angles (N1–N8) | Varies | Varies | Varies | Investigate |

---

## Files Referenced

- `CMakeLists.txt` — Track G default
- `src/cli_parser.cpp:35` — hardware_concurrency fix
- `src/mining_engine.cpp:35,236,80-109,333-351` — worker count + core remapping
- `src/jit_compiler_a64.cpp` — PRFM hints, dual-issue alignment, load hoisting
- `src/jit_compiler_a64_static.S` — Track C retry
- `tests/bench_armrx.cpp` — perf stat infrastructure, D2 harness
- `src/aes_hash.cpp:372` — D2 interleaved function
- `src/vm.cpp:991` — D2 pipeline entry
- `docs/experiments/light-mode-dataset-item-prologue-attempt.md` — Track C diagnostic
- `docs/experiments/d2-hash-fill-pipeline.md` — D2 experiment writeup
- `docs/experiments/f3-neon-mul-latency-test.md` — F3 premise
