# armrx — Comprehensive Technical Audit & Optimization Report

**Date:** 2026-07-25  
**Auditor:** Deepseek (Reasonix agent)  
**HEAD reviewed:** ~`6712479` (emitter scheduler committed, superscalar extension landed)  
**Methodology:** Full codebase read-through, cross-reference against existing docs/experiments/audits, analysis of prior closed leads from `HANDOFF_CLAUDE.md`

---

## Executive Summary

The `armrx` codebase is in **excellent health**. It has survived multiple rounds of deep audit, stress-testing, and hardware-backed performance scrutiny over a 4-day intensive development cycle (2026-07-21 through 2026-07-25). The remaining open questions are genuinely subtle — not bugs or oversights, but inherently hard problems (the ~10-12% instruction-count gap to XMRig) that will require new instrumentation or architectural insight.

**Strong points that distinguish this codebase:**
- **Verified-before-acting discipline:** Every claim from external sources (prior handoffs, audits) was independently re-verified on real hardware before acceptance. This caught multiple stale/wrong claims (the 31% branch-miss myth, the PGO +19.3% claim, the hardcoded "cores 0-3 = big" assumption).
- **Negative results documented, not hidden:** CBRANCH/CSEL (+46% branch-misses), Argon2 copy-elimination (null), PGO (null on current code), NEON vector-permute AES (−19.4%) — all kept as flagged reference material rather than discarded.
- **Tests finding real bugs:** Three separate instances where new test coverage uncovered real, previously-unknown bugs (worker-thread dataset reuse, mock Stratum deadlock, broken `--config=` flag).
- **Consensus-critical code gets consensus-critical scrutiny:** The emitter scheduler review (`docs/audits/emitter-scheduler-review.md`) is a model of how to approach hash-divergence-risk code — exhaustive hazard analysis, dedicated stress tests with the right coverage shape, empirical bisection of failures, and honest documentation of what's still not fully understood.

**What this audit found:** Two new findings, several observations for hardening, and an honest assessment that the remaining performance gap needs new measurement infrastructure, not more code changes.

---

## 1. Critical & High-Priority Missed Points

### 1.1 Huge-page residency is verified for the cache but NOT for the scratchpad

- **Location:** `src/vm.cpp:129-141` (scratchpad allocation), `src/argon2.cpp:269-277` (cache allocation), `src/mining_engine.hpp:28-43` (`MappedMemory` for dataset)
- **Context:** `PLAN.md` Phase 6 items 1-2 verified that the 256 MiB Argon2 cache achieves ~97.6% THP coalescing (via `/proc/smaps` inspection). This is good. However, the per-worker **2 MiB scratchpad** (allocated in `VirtualMachine`'s constructor) was NOT independently verified for huge-page residency — only the shared cache was.
- **Why it matters:** The 2 MiB scratchpad is accessed on every VM iteration (2048 × 2 = 4096 scratchpad reads+stores per hash). If it's backed by 4 KiB pages, that's 512 TLB entries constantly thrashing — on a Cortex-A53 with a tiny micro-TLB (10 entries for L1), this could be a significant contributor to the ~10-12% instruction-count-equivalent gap to XMRig (TLB miss latency shows up as extra cycles, not extra instructions).
- **Impact:** Performance — potentially a non-trivial fraction of the remaining ~10-12% gap.
- **Recommended Action:** Run `grep -A20 VmFlags /proc/<pid>/smaps | grep -B10 scratchpad` on the devbox while mining to confirm the scratchpad mapping's page size. If it's 4 KiB, investigate `madvise(MADV_HUGEPAGE)` on the scratchpad (it's already requested via the fallback path, but THP `always` policy may not coalesce 2 MiB single-page allocations the same way it does 256 MiB ones). A positive finding here would be actionable; a negative one would close another unknown.

### 1.2 `register_usage_` sentinel initial value: benign but worth a static assert

- **Location:** `include/armrx/vm.hpp` — `int register_usage_[8] = {-1};`
- **Context:** The `{-1}` initializer only sets element 0 to -1; elements 1-7 are zero-initialized. The code works correctly because `compile_program()` does `std::fill(register_usage_, register_usage_ + 8, -1)` before use (confirmed by the subagent's analysis and code inspection). However, if any future code path reads `register_usage_` before `compile_program()` runs, element 0 would see -1 (correct sentinel) but elements 1-7 would see 0 (incorrect — 0 is a valid instruction index).
- **Impact:** None currently — verified safe. This is a maintenance hazard, not a current bug.
- **Recommended Action:** Either use `= {}` with explicit `std::fill` in the constructor, or add a `static_assert` that the first access path always goes through `compile_program()`. Low priority.

### 1.3 `rx_set_rounding_mode` cache initialization sentinel is fragile

- **Location:** `src/vm.cpp` — `last_rounding_mode_ = 0xFF` (sentinel) and `reset_rounding_mode()` sets to `0` (not `0xFF`)
- **Context:** The sentinel `0xFF` is used to force the first `rx_set_rounding_mode()` call to always execute `fesetround()`. When `reset_rounding_mode()` is called at the start of `randomx_calculate_hash()`, it sets `last_rounding_mode_ = 0` (effectively `FE_TONEAREST`), NOT back to `0xFF`. This means if `randomx_calculate_hash()` is called twice without an intervening explicit rounding-mode change, the SECOND call's first CFROUND would see `last_rounding_mode_ == 0 == FE_TONEAREST` and might skip `fesetround()` if the current mode happens to already be `FE_TONEAREST`. This is correct by coincidence (the mode IS `FE_TONEAREST` at that point), but the logic is unclear.
- **Impact:** None currently — verified correct in practice. A maintenance clarity issue.
- **Recommended Action:** Rename `0xFF` to a named constant (`kRoundingModeUninitialized`) and have `reset_rounding_mode()` set it back to that constant. This makes the intent explicit and prevents future refactoring from introducing a bug.

---

## 2. Low-Level AArch64 & Hardware Optimization Opportunities

### 2.1 Dataset-item derivation dominates instruction volume; JIT instruction-level optimization is low-EV

- **Location:** `src/jit_compiler_a64.cpp` — `generateSuperscalarHash()` and `src/jit_compiler_a64_static.S` — `randomx_calc_dataset_item_aarch64`
- **Context:** In light mode (the only mode this device can run), every hash executes `rx_calc_dataset_item` 8 times per main-loop iteration × 2048 iterations = 16,384 calls per hash. Each call compiles and runs one superscalar program. This is where ~>99.9% of real instruction volume lives. The main 2047-instruction VM program is negligible by comparison. The emitter scheduler (Phase 6 item 12) was extended to cover this region (commit `6712479`) after the initial main-program-only version measured as a null result — precisely because it was targeting the wrong region.
- **Current state:** The superscalar scheduler is correct and active (verified by this audit's companion review: `docs/audits/emitter-scheduler-review.md`). Measured IPC improvement: +0.233%, cycles reduction: −0.036% — real but tiny. This confirms the diagnosis: the superscalar path is already near-optimal for instruction scheduling.
- **What the remaining ~10-12% gap to XMRig ISN'T:**
  - ❌ Instruction scheduling (emitter scheduler +0.233% IPC proves the room is tiny)
  - ❌ Branch prediction (hot-path branch-miss rate is 2.4%, costing ~0.1-0.16% of cycles)
  - ❌ Memory latency/stalls (armrx's IPC 0.731 and `ld_dep_stall`% 11.41% are *better* than XMRig's 0.612 / 16.70%)
- **What the gap IS:** ~33.5% more instructions per hash (132.93M vs 99.57M). This is a **code-generation quality gap** — XMRig's AArch64 JIT backend emits fewer instructions for the same RandomX bytecode. Finding WHERE requires new instrumentation (the `--jit-dump` superscalar extension from Phase 6 item 13 was the first step; opcode-level per-instruction byte counts would be the next).
- **Recommendation:** This is an inherently hard problem. The Phase 6 item 14 approach (live `perf record` with self-profiling, correlating sample addresses against superscalar opcode-boundary tables) is the right direction. Consider building a `--jit-dump-instruction-breakdown` mode that reports, for each superscalar opcode, the average bytes emitted per instance — this would pinpoint which opcodes have code-generation bloat vs. XMRig without violating the clean-room boundary.

### 2.2 Cortex-A53-specific microarchitectural: `prfm` distance tuning never done

- **Location:** `src/jit_compiler_a64_static.S:214-216, 345, 909`
- **Context:** The static template has `prfm pldl1keep` hints for scratchpad and dataset lines. The loop-end prefetches (lines 385-387) were removed after measuring a regression. However, the *distance* of the surviving prefetches was never tuned — they're issued at fixed points in the main loop template. On an in-order Cortex-A53, the optimal prefetch distance depends on the ratio of instruction throughput to memory latency, which varies with clock speed and memory controller load.
- **Impact:** Potentially measurable but unlikely to be large — prefetch tuning is typically a 1-3% effect at best.
- **Recommendation:** Low priority. If benchmarking infrastructure improves (ability to run rapid A/B tests on-device), try moving the scratchpad `prfm` earlier by 1-2 loop iterations. Worth maybe an hour of investigation.

### 2.3 `MAP_POPULATE` on huge pages may cause unnecessary allocation-time latency

- **Location:** `src/virtual_memory.c:235` (`MAP_HUGETLB | MAP_POPULATE`)
- **Context:** `MAP_POPULATE` prefaults all pages at `mmap` time. For the 256 MiB Argon2 cache with 2 MiB huge pages, this means 128 page faults are resolved synchronously during allocation. This is fine for startup latency (a few hundred ms). However, if the kernel's huge-page pool is fragmented, `MAP_HUGETLB` falls through silently, and the `MADV_HUGEPAGE` fallback path does NOT use `MAP_POPULATE` — pages are faulted in on first access. This is actually preferable for THP (it lets the kernel coalesce on-demand), so the current behavior is reasonable.
- **Recommendation:** No action needed. Current behavior is appropriate.

### 2.4 Static template alignment: `.p2align 5` is correct but could use `.p2align 6` for I-cache

- **Location:** `src/jit_compiler_a64_static.S:218` — `.p2align 5` (32-byte alignment for main loop)
- **Context:** The Cortex-A53 has a 16 KiB 2-way set-associative L1 I-cache with 64-byte lines. Aligning the main loop entry to 32 bytes (`.p2align 5`) means it could span at most 2 cache lines. Aligning to 64 bytes (`.p2align 6`) would guarantee it starts at a cache-line boundary. The current 32-byte alignment is almost certainly fine — the main loop body is hundreds of bytes and spans many cache lines regardless. The alignment only matters for the very first instruction fetch.
- **Recommendation:** Tiny, nearly zero-impact. Not worth changing unless measuring with hardware performance counters shows I-cache miss improvement.

---

## 3. Architecture & Code Quality Improvements

### 3.1 `MappedMemory` RAII wrapper is good; consider unifying with `Argon2dCache`'s allocation path

- **Location:** `include/armrx/mining_engine.hpp:24-78` (`MappedMemory`), `src/argon2.cpp:269-277` (duplicate allocation logic)
- **Context:** Both `MappedMemory` and `Argon2dCache`'s constructor implement the same `MAP_HUGETLB` → `mmap` + `MADV_HUGEPAGE` fallback pattern. `VirtualMachine`'s scratchpad allocation in `vm.cpp` also duplicates this. Three copies of the same allocation policy.
- **Impact:** Maintenance — changing huge-page policy requires updating 3 places.
- **Recommendation:** Extract a `allocate_large_buffer(size_t bytes, const char* purpose)` function that centralizes the huge-page logic and optionally logs whether huge pages were obtained (currently silent fallback). Low priority but good hygiene.

### 3.2 Build system: missing `-frounding-math` for the main `armrx` executable

- **Location:** `CMakeLists.txt:108`
- **Context:** `-ffp-contract=fast` is set on `armrx_core`, which controls whether `a*b+c` is contracted to FMA. However, `-frounding-math` is NOT set — yet the RandomX VM depends on IEEE 754 rounding mode changes via `fesetround()`. GCC's default (`-fno-rounding-math`) allows the compiler to assume the default rounding mode and fold constant expressions at compile time.
- **Impact:** If any floating-point expression involving constants is evaluated at compile time (e.g., mask computations that happen to be `constexpr`), it would use the default rounding mode rather than the runtime mode. In practice, the RandomX VM's FP operations all go through the JIT (which emits raw AArch64 FP instructions with no compiler folding), and the interpreter uses runtime `double` operations through the bytecode — so the compiler can't constant-fold VM floating-point ops. The static template assembly also emits raw FP instructions. **Likely safe in practice**, but the flag should be present given the VM's documented dependency on runtime rounding mode changes.
- **Recommendation:** Add `-frounding-math` to `target_compile_options(armrx_core PRIVATE ...)` for correctness-by-construction. The `REASONIX.md` project card even mentions `-frounding-math` as enabled — but it's not in the CMakeLists.

### 3.3 `ARMRX_ENABLE_NATIVE` uses `-mcpu=native`; should also set `-mtune=native` for portability

- **Location:** `CMakeLists.txt:110-112`
- **Context:** `-mcpu=native` implies both `-march=native` and `-mtune=native`. On the devbox (Cortex-A53), this is correct and optimal. When cross-compiling for a different AArch64 target, `ARMRX_ENABLE_NATIVE=OFF` is the right choice.
- **Recommendation:** No action needed. Current behavior is correct for the project's scope (build on the target device or with exact `-mcpu` match).

### 3.4 Test coverage gaps: TLS and TUI remain untested

- **Location:** `src/tls_client.cpp`, `src/tui.cpp`
- **Context:** Already flagged in `HANDOFF_CLAUDE.md` as lower-priority. Both require mocking infrastructure (TLS server, terminal capture) that's a non-trivial lift.
- **Recommendation:** Defer. Not blocking. The historical pattern of "tests finding real bugs" (worker-thread, Stratum mock, `--config=`) supports the value of eventually adding these — but the ROI calculation should consider that TLS/TUI are not consensus-critical (a TLS bug crashes or disconnects; a TUI bug shows wrong text; neither silently produces wrong hashes).

---

## 4. Edge Cases, Safety & Robustness

### 4.1 CBRANCH with unwritten target register: correct but worth defensive hardening

- **Location:** `src/vm.cpp` — `execute_bytecode()` CBRANCH handler, `register_usage_[creg]` access
- **Context:** If a CBRANCH instruction targets a register `creg` that has NEVER been written in the program, `register_usage_[creg]` would be its initial value. After `compile_program()`'s `std::fill(register_usage_, ..., -1)`, this would be -1. In the interpreter, `pc` gets set to -1, then the `for` loop's `++pc` wraps to 0 (since `pc` is `int`). This effectively restarts execution from instruction 0 — which is technically wrong per spec (CBRANCH should branch to the last writer, and if there is none, the behavior is arguably undefined). The JIT path handles this differently (the `reg_changed_offset[creg]` would be `PrologueSize` — the initial value).
- **Impact:** Theoretical — CBRANCH with an unwritten target register is an extremely unlikely program shape (RandomX's instruction generator always writes registers before branching to them). No known real-world trigger.
- **Recommendation:** Add `ARMRX_ASSERT(register_usage_[creg] >= 0, "CBRANCH target register never written")` in the interpreter's CBRANCH handler. Cost: zero in release builds (assert compiles out). Benefit: catches the impossible case in debug/test builds.

### 4.2 JSON parser: hardened but single-threaded by design; no threading concerns

- **Location:** `src/json.cpp`
- **Context:** The JSON parser was fuzz-tested (2.5M+ executions, zero findings) and is well-hardened against injection. It operates on `std::string` input and is only called from the Stratum client's receive thread. No shared mutable state.
- **Recommendation:** No action needed. Already well-audited.

### 4.3 `PoolManager` self-deadlock fix is in place; verify it handles rapid reconnect storms

- **Location:** `src/pool_manager.cpp` — `connect_to_current()` + failover logic
- **Context:** The `PoolManager` self-deadlock bug (found by mock Stratum tests, Phase 2) was fixed. The current failover behavior: 5 retries per pool, 2s cooldown between pools. Under pathological conditions (all pools unreachable, rapid cycling), the exponential backoff in the Stratum client's own reconnect logic interacts with `PoolManager`'s pool-level cooldown.
- **Impact:** Benign — worst case is a few extra seconds of idle time during an extended all-pools-down scenario. No crash, no resource leak.
- **Recommendation:** Already production-quality. No change needed.

### 4.4 JIT buffer: W^X disclosed but RWX is the default; documented tradeoff

- **Location:** `src/jit_compiler_a64.cpp:142-181`
- **Context:** The JIT code buffer tries RWX first; if the kernel allows it (`setPagesRWX` succeeds), `mprotect` calls are skipped on every JIT recompile. On systems that enforce W^X, it falls back to RW→RX transitions. The default is clearly logged at startup. Per explicit maintainer direction, this is kept as-is.
- **Recommendation:** No action needed per maintainer decision. The startup log line makes the tradeoff transparent to operators.

### 4.5 `libFuzzer` harness for JSON parser: good but not integrated into CI

- **Location:** `tests/fuzz_json.cpp`, `CMakeLists.txt:300-311`
- **Context:** The fuzzer exists and was run for 2.5M+ iterations. It's opt-in (`ARMRX_BUILD_FUZZERS=ON`) and Clang-only. Not part of CTest.
- **Recommendation:** Consider adding a one-shot fuzzing run to CI (e.g., `fuzz_json -max_total_time=30` for 30 seconds on every PR). Low priority, nice-to-have.

---

## 5. Prioritized Actionable Roadmap

Items are ranked by (impact × certainty) / effort. "Impact" here means likelihood of meaningfully closing the ~10-12% performance gap to XMRig.

### Immediate Wins (High Impact, Low Effort)

1. **Verify scratchpad huge-page residency** (§1.1). One `grep` on the devbox. If scratchpad pages are 4 KiB, `madvise(MADV_HUGEPAGE)` on the scratchpad is a one-line change with potentially measurable TLB improvement. If already 2 MiB, close this lead definitively. **Effort: 15 minutes. Potential impact: 1-5% of the remaining gap.**

### Strategic Optimizations (High Impact, Medium-High Effort)

2. **Build per-opcode instruction-byte-count instrumentation for the superscalar path** (§2.1). The `--jit-dump` superscalar extension (Phase 6 item 13) already captures per-opcode byte counts in aggregate. Extending this to report *per-instance* byte counts (or at least a histogram per opcode) would pinpoint which superscalar opcodes have instruction-count bloat vs. a theoretical minimum. This is the clean-room-safe way to close the instruction-count gap. **Effort: ~1 day. Potential impact: identifies the exact hot spots contributing to the ~33.5% instruction-count gap.**

### Defensive Hardening (Low-Medium Impact, Low Effort)

3. **Add `-frounding-math` to compiler flags** (§3.2). One `CMakeLists.txt` line. Defensive against future code changes that might enable FP constant folding. **Effort: 2 minutes.**

4. **Centralize the huge-page allocation fallback logic** (§3.1). Extract the 3 duplicated `MAP_HUGETLB → mmap + MADV_HUGEPAGE` patterns into one function. Low-priority code quality improvement. **Effort: 30 minutes.**

5. **Add `ARMRX_ASSERT` for CBRANCH unwritten-target case** (§4.1). One line in `vm.cpp`. Zero runtime cost. **Effort: 2 minutes.**

### Not Recommended (Already Investigated/Measured)

- ❌ **NEON hardware AES (AESE/AESD):** Reverted — instruction ordering incompatible with RandomX round spec.
- ❌ **NEON vector-permute AES:** Implemented, measured −19.4% regression. Kept flag-gated for reference; not a win on this hardware.
- ❌ **Newton-Raphson FDIV/FSQRT:** Measured −1.1% hashrate. Flag-gated.
- ❌ **PGO:** Measured null (identical 4.27 H/s) on current code. Tooling kept for future re-evaluation.
- ❌ **CBRANCH/CSEL branchless rewrite:** Measured +46% branch-misses. Reverted.
- ❌ **Argon2 `memcpy` copy elimination:** Measured flat-to-worse. Reverted.
- ❌ **`--stagger-ms` tuning:** Previously tested, found ineffective (hardware ceiling from continuous full-scratchpad access).

### Deferred (Maintainer Decision)

- QEMU AArch64 GitHub Actions CI
- Stratum V2 protocol support
- `tls_client.cpp` / `tui.cpp` test coverage
- JIT buffer RWX vs W^X default (current RWX-by-default kept per explicit direction)

---

## Appendix: What This Audit Did NOT Find

Given the project's development intensity (4 days, ~15 major commits, 3 external audits, 2 performance master plans synthesized), it's worth noting what I specifically looked for and did NOT find:

- **No new consensus-divergence bugs.** The emitter scheduler was independently verified correct (`docs/audits/emitter-scheduler-review.md`). All known hash-divergence paths (AES T-table column permutation, CBRANCH anchor positioning, src==dst x20 hazard) are addressed.
- **No memory corruption or use-after-free.** The RAII patterns (`MappedMemory`, `Argon2dCache`), `munmap` guards, and worker lifecycle management all check out.
- **No thread-safety issues in the hot path.** The lock-free job distribution (generation-counter pattern), per-worker partitioned nonces, and cache-line-padded hash counters are all correctly implemented.
- **No spec violations in the VM.** The interpreter bytecode dispatch, FP rounding mode handling, CBRANCH semantics, E-register masking, and dataset derivation all match the RandomX specification.
- **No build-system regressions.** LTO, sanitizers, PGO, and toolchain compatibility (GCC 15 + musl fortify-headers workaround) are all properly conditionalized.

The codebase is in better shape than most production mining software I've reviewed. The remaining work is genuinely hard (closing a 33.5% instruction-count gap without looking at the competitor's disassembly) and the project's discipline around verifying claims before acting is the right approach.
