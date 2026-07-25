# armrx — Comprehensive Technical Audit & Optimization Report
Hermes Agent (Hy3)
*Audit date: 2026-07-25 04:11 · HEAD `6a7b71a` · Auditor role: Senior Systems Architect / Low-Level Engineer / Security Auditor · Mode: strict read-only (no source files modified)*

## Executive Summary

`armrx` is a mature, well-tended AArch64 RandomX v1 miner. The codebase is in **good health**: correctness is anchored by KATs (Blake2b/AES/full-hash), the JIT is a faithful port of the upstream tevador/SChernykh AArch64 template, and the interpreter and JIT share a single spec-derived opcode-dispatch table (`instruction_weights.hpp`) so they cannot silently drift apart. The prior handoff (`HANDOFF_CLAUDE.md`) documents an unusually disciplined "verify-before-trusting / measure-honestly" culture, and it shows — most of the obvious low-hanging perf fruit has already been picked, measured, and (where it didn't help on the Cortex-A53 target) honestly reverted.

That said, this audit found a **genuine memory-safety defect** (release-mode OOB read that logs-then-proceeds), several **microarchitecture blind spots** (no `-mcpu=cortex-a53` tuning on the shipped cross-build, no software prefetch in the interpreted dataset path, false sharing on the per-worker counters, hardcoded big-core mask), a **security posture decision worth revisiting** (RWX JIT buffer by default, no SIGSEGV guard around JIT execution despite it being in scope), and a handful of **correctness/robustness edge cases and stale comments**. None are hair-on-fire, but item 1.1 and 1.2 deserve prompt attention.

Overall grade: **B+ / production-capable**, with a clear, mostly-low-effort path to A.

---

## 1. Critical & High-Priority Missed Points

### 1.1 Release-mode out-of-bounds dataset read logs but does not stop
- **Location:** `src/vm.cpp:712-718` (`dataset_read`), macro at `include/armrx/assert.hpp:29-38`
- **Context:** `dataset_read()` guards the fast-mode path with `ARMRX_ASSERT(address + 64 <= dataset_.size(), "dataset_read OOB")`. In **release builds (NDEBUG defined — which is the default `Release` build type, CMakeLists.txt:29-32)** `ARMRX_ASSERT` only prints to `stderr` and **falls through**. Control then proceeds into the loop `r[i] ^= load64(datasetLine + 8*i)` reading up to 64 bytes past the mapped dataset. Because `datasetLine = dataset_.data() + address`, an out-of-range `address` produces a wild read.
- **Impact:** Memory-safety violation (potential SIGSEGV or silent data corruption) in exactly the mode that ships. `address` derives from `dataset_offset_ + (ma_ & 0x7fffffc0)` where `dataset_offset_ = (entropy_[13] % 524288) * 64` — bounded by construction today, so this is currently latent, but the guard is the only line of defense and in release it is a no-op that still executes the unsafe access. An assertion that detects a fault and then performs the faulting operation anyway is worse than no assertion, because it advertises safety it doesn't provide.
- **Recommended Action:** In the release branch of `ARMRX_ASSERT`, make the failing check **actually skip the dangerous work** — either `return`/`continue` semantics at the call site, or convert this specific hot-but-critical check to `if (address + 64 > dataset_.size()) { log; return; }`. At minimum, the `dataset_read` fast path should early-return on the bounds failure instead of falling through. Consider a distinct `ARMRX_CHECK_OR_RETURN(cond, ret, msg)` macro for cases where continuing is unsafe.

### 1.2 No SIGSEGV/SIGBUS guard around JIT-generated code execution
- **Location:** `src/miner_app.cpp:61-67` (`install_signal_handlers`), execution at `src/vm.cpp:798-801` (`getProgramFunc()(...)`)
- **Context:** The audit scope explicitly calls out "signal handling (e.g., SIGSEGV during JIT execution), and graceful fallback mechanisms." Only `SIGINT`/`SIGTERM`/`SIGPIPE` are handled. A bug in the JIT emitter, a corrupted RWX buffer, or a stray unaligned/OOB scratchpad access inside generated code will deliver an uncatchable `SIGSEGV`/`SIGBUS` that takes down the whole process (and every worker) with no diagnostic beyond a kernel core dump.
- **Impact:** A single JIT/scratchpad fault kills the miner instead of degrading one worker. On a headless mining box this is a silent outage. Also weakens defense-in-depth for the RWX decision in 1.3.
- **Recommended Action:** Install a `sigaction` (not `std::signal`) handler for `SIGSEGV`/`SIGBUS` that at least emits the faulting address + a note "fault inside JIT region [base,base+size)" before re-raising with the default disposition, so operators get an actionable log line. A fuller design (per-thread `sigsetjmp` around the JIT call to deactivate just that worker) is a larger lift but is the "graceful fallback" the scope asks for.

### 1.3 JIT code buffer is RWX by default (W^X violation)
- **Location:** `src/jit_compiler_a64.cpp:158-180`, `src/virtual_memory.c:210-213` (`setPagesRWX`)
- **Context:** The constructor attempts `setPagesRWX` once and, if the kernel permits, leaves the JIT buffer **readable + writable + executable for the process lifetime** to skip per-recompile `mprotect` syscalls. This is disclosed via a one-time log line and was explicitly kept "for now" per the handoff — so it's a *known, deliberate* tradeoff, not an oversight. It is flagged here because an audit must: a permanently-writable executable mapping is the textbook target for code-injection escalation, and it is regenerated with fresh code every single hash.
- **Impact:** Security hardening gap. Combined with 1.2 (no fault containment), a memory-corruption bug anywhere in the process could be promoted to arbitrary code execution via the RWX region.
- **Recommended Action:** This is a maintainer policy call, but recommend flipping the default to W^X (`enableWriting`/`enableExecution` already implement the RW↔RX flip correctly) and making RWX an explicit opt-in flag (`--unsafe-rwx-jit` / `ARMRX_ALLOW_RWX`). The per-hash `mprotect` pair cost should be re-measured on the A53 — it is typically <0.3% and buys a real security property. If RWX stays default, pair it with 1.2 at minimum.

### 1.4 Hardcoded "big core" mask ignores discovered topology
- **Location:** `src/mining_engine.cpp:283-289` (`AffinityMode::BigOnly`)
- **Context:** `BigOnly` pins workers to CPUs `0,1,2,3` via `thread_id % 4`, assuming the big cluster is always cores 0-3. Meanwhile `detect_core_order()` (lines 26-109) already does the correct thing — it reads `cpuinfo_max_freq` and sorts cores by frequency descending — but `BigOnly` throws that information away and hardcodes indices.
- **Impact:** On any SoC where the big cluster isn't cores 0-3 (common on DynamIQ / mixed layouts, and on many Snapdragon/MediaTek parts the big/prime cores are the *high* indices), `--big-only` pins to the LITTLE cores and *loses* performance while claiming to do the opposite. Correctness-of-intent bug.
- **Recommended Action:** Derive the big-core set from `core_order_` (the top-N highest-frequency entries), e.g. pin to `core_order_[thread_id % big_count]` where `big_count` is the number of cores sharing the max frequency. Reuse the topology you already detected.

---

## 2. Low-Level AArch64 & Hardware Optimization Opportunities

### 2.1 Shipped cross-build has no `-mtune`/`-mcpu` for the target
- **Location:** `CMakeLists.txt:63` (`-march=armv8-a+crypto`), `110-112` (`-mcpu=native` only under `ARMRX_ENABLE_NATIVE`)
- The default build targets generic `armv8-a+crypto` with **no scheduling model**. `-mcpu=native` is only added when `ARMRX_ENABLE_NATIVE=ON`, which the docs show is used, but the *default* artifact is un-tuned. The known target is a Cortex-A53 (`HANDOFF_CLAUDE.md:5`). The A53 is a dual-issue in-order core — instruction *scheduling* matters enormously there, far more than on OoO cores. Building generic-armv8 leaves the compiler guessing the pipeline model.
- **Recommendation:** Add `-mtune=cortex-a53` (or `-mcpu=cortex-a53+crypto`) as the default for the device build, or expose an `ARMRX_TUNE` cache var. This is a genuine free win specifically because the A53 is in-order — GCC's A53 pipeline model reorders for dual-issue and works around the well-known A53 erratum/`madd` forwarding quirks. Worth a back-to-back `bench_armrx` measurement; low effort, plausibly low-single-digit percent.

### 2.2 No software prefetch in the interpreted dataset read path
- **Location:** `src/vm.cpp:712-728` (`dataset_read`), `src/dataset.cpp:34-64` (`generate_dataset_item`)
- The JIT static template already uses `prfm` (9 occurrences in `jit_compiler_a64_static.S`), but the **C++ interpreted** dataset read (light-mode fallback and x86 build) issues no `__builtin_prefetch` before the 64-byte strided XOR loop. The A53 has a short pipeline and small MSHR count; a `prfm pldl1keep` one iteration ahead of the cache-line read in `generate_dataset_item`'s cache-access loop (line 43-57) can hide L2 latency.
- **Recommendation:** Add `__builtin_prefetch(next_line, 0, 3)` in the `kRandomXCacheAccesses` loop. Interpreted mode isn't the hot path on-device (JIT is), so impact is limited to light-mode/non-AArch64, but it's a cheap, correct improvement and closes a real asymmetry with the JIT path.

### 2.3 False sharing on the per-worker hash counters
- **Location:** `include/armrx/mining_engine.hpp:137` (`std::unique_ptr<std::atomic<std::uint64_t>[]>`), written at `src/mining_engine.cpp:446`
- `worker_hashes_` is a densely-packed array of `std::atomic<uint64_t>` (8 bytes each), so 8 workers' counters share a single 64-byte cache line. Each worker does a relaxed `fetch_add` every 64 hashes. The flush cadence (once per 64 hashes) keeps this *low-frequency*, so real impact is small — but it is textbook false sharing and trivially removable.
- **Recommendation:** Pad each counter to a cache line: `struct alignas(64) WorkerCounter { std::atomic<uint64_t> v; };`. Same fix applies to `total_hashes_` if it ends up adjacent to hot data. Low effort; measure to confirm it's noise vs. signal on the A53's 64-byte lines.

### 2.4 `randomx_reciprocal` recomputed per-hash for main-program IMUL_RCP
- **Location:** `src/jit_compiler_a64.cpp:967` (`h_IMUL_RCP` → `randomx_reciprocal(divisor)`), and `src/vm.cpp:361`
- Each per-hash program regeneration recomputes the reciprocal from the divisor via the (branchy, loop-based) `randomx_reciprocal`. The superscalar/dataset path correctly uses a precomputed `reciprocalCache`; the per-hash program path does not. Divisors are drawn from program entropy so caching across hashes isn't trivially valid, but within the ~16 IMUL_RCP instructions of one program there may be repeats, and the reciprocal computation itself is not free.
- **Recommendation:** Profile whether `randomx_reciprocal` shows up in the JIT-compile phase (`ARMRX_JIT_PROFILE` already separates compile vs. execute time). If so, a small per-program memoization or a faster reciprocal routine is worth it. Low confidence — likely minor — but currently unmeasured.

### 2.5 BOLT post-link optimization is unexplored
- **Location:** build system (`CMakeLists.txt`) — PGO present (27, 119-129), BOLT absent
- The scope explicitly lists "BOLT optimization readiness." PGO is wired (though its payoff didn't reproduce per the handoff). BOLT operates on a different axis (basic-block reordering / I-cache layout of the *fixed* C++ hot loop and the AES/blake paths) and is complementary to PGO. Given the A53's small I-cache, block layout matters.
- **Recommendation:** As a future experiment, add an optional BOLT pass over the `armrx` binary (perf-guided). Frame it like the existing PGO tooling — opt-in, measured honestly, kept-if-it-helps. Medium effort, uncertain payoff, but it's the one named lever not yet pulled.

### 2.6 SVE/SVE2 correctly not used (informational, not a gap)
- The A53 has no SVE, so the absence of SVE paths is correct, not an oversight. The NEON diagonal-Argon2 vectorization (`dataset.cpp:88-135`) and the flag-gated NEON-permute AES are the appropriate SIMD investments for this ISA level. No action — noted so a future reader doesn't "discover" a non-issue.

---

## 3. Architecture & Code Quality Improvements

### 3.1 Stale/incorrect comments in JIT emitters
- **Location:** `src/jit_compiler_a64.cpp:883-884` (`h_IMUL_M`) — comment says `// sub dst, dst, tmp_reg` but the code emits `ARMV8A::MUL` (correct behavior, wrong comment). Same copy-paste artifact worth a scan across the `h_*` handlers.
- **Impact:** None functionally; a maintenance trap. Someone "fixing the code to match the comment" would introduce a real bug.
- **Recommendation:** Correct the comment to `// mul dst, dst, tmp_reg`. Quick grep for other mismatched op comments in the emitter block.

### 3.2 Hash-rate reporting mixes warmup into the denominator
- **Location:** `src/mining_engine.cpp:242-262` (`worker_hash_rate` / `hash_rate`)
- Both divide cumulative hashes by wall-clock since `start_time_`, so the reported H/s is a lifetime average that never sheds the cold-start/dataset-init warmup. The benchmark path (`miner_app.cpp:174-189`) already takes a post-warmup `snapshot()` and diffs — the good pattern — but the live `hash_rate()` used for the TUI/metrics does not.
- **Recommendation:** Have `hash_rate()` compute a windowed rate (diff of two recent snapshots) rather than lifetime average, so the operator-facing number reflects steady state. Low effort, improves the number everyone actually looks at.

### 3.3 Oversubscription when `--threads` exceeds core count
- **Location:** `src/mining_engine.cpp:294` (`core_order_[thread_id % core_order_.size()]`)
- With `AffinityMode::All` and more threads than cores, `% core_order_.size()` pins multiple workers to the same core. For a memory-latency-bound workload like RandomX that's usually counterproductive (scratchpad thrash). There's no warning when `num_threads_ > core_order_.size()`.
- **Recommendation:** Emit a one-time warning when workers exceed physical cores, and/or clamp the default worker count to core count in auto mode.

### 3.4 `std::signal` instead of `sigaction`
- **Location:** `src/miner_app.cpp:62-63`
- `std::signal` has weaker, less portable semantics than `sigaction` (handler reset behavior, no `SA_RESTART` control). For a long-running daemon, `sigaction` with explicit flags is the robust choice, and is required anyway for the 1.2 SIGSEGV work.
- **Recommendation:** Migrate to `sigaction` when addressing 1.2.

### 3.5 Dead multi-platform code retained in `virtual_memory.c`
- **Location:** `src/virtual_memory.c` (Windows/Cygwin/Apple/BSD branches)
- Acknowledged in the file header comment as intentional (upstream parity, intertwined branches). Not a defect — noted only so the audit is complete. Keeping it is defensible; no action required.

---

## 4. Edge Cases, Safety & Robustness

### 4.1 `MemAvailable` absence silently forces light mode
- **Location:** `src/memory.cpp:24-33`, `57-64`
- If `/proc/meminfo` lacks `MemAvailable` (kernels < 3.14) or is unreadable, `mem_available()` returns `nullopt` → `available_memory()` reports 0 bytes → `choose_randomx_mode` always picks light. Correct fail-safe direction (never over-commits), but silent: a user on an odd kernel gets half the hashrate with no explanation.
- **Recommendation:** Log a warning when the memory probe fails and light is forced by an *unknown* (not genuinely-small) memory figure.

### 4.2 cgroup v1 "unlimited sentinel" handled; v2 partial
- **Location:** `src/memory.cpp:35-53`
- v1 guards against the `1<<60` sentinel (line 46). v2's `memory.max == "max"` is handled by `read_number` returning `nullopt` (line 17). Looks correct. One gap: v2 `memory.high` (soft limit) is ignored — a process can be throttled below `memory.max`. Minor; note only.

### 4.3 Live dataset re-init: worker participation vs. shutdown race — appears correct
- **Location:** `src/mining_engine.cpp:159-236` (`set_job`) and `282-379` (`worker_loop`)
- The generation-counter handshake, the separate `dataset_init_mutex_`, and the `stop()` path notifying `dataset_init_cv_` (line 150) to unblock a `set_job` waiting on a rebuild are all correctly reasoned. The CV wait predicate rechecks `!running_` (line 193). This is solid concurrency work. **No defect found** — validated as part of the audit and called out as a strength.

### 4.4 `update_nonce_in_template` bad-offset path — correctly fixed
- **Location:** `src/mining_engine.cpp:418-430`
- The prior "return kills the worker forever" bug is fixed (now `active=false; continue;`). Confirmed correct and matches sibling bad-state handling. No action.

### 4.5 Scratchpad huge-page fallback is robust
- **Location:** `src/vm.cpp:128-151`, `include/armrx/mining_engine.hpp:28-44`
- `allocLargePagesMemory` → plain `mmap` + `MADV_HUGEPAGE` → `MADV_POPULATE_WRITE`/memset warmup. Correct RAII, correct fallback chain, prefaults to avoid first-hash page-fault stalls. Strength; no action.

### 4.6 FP environment save/restore per hash is correct for determinism
- **Location:** `src/vm.cpp:911-935` (`randomx_calculate_hash` `fegetenv`/`fesetenv`), rounding cache at `71-81`, `200-203`
- Save/restore of the full FP environment around each hash, plus the `last_rounding_mode_` cache invalidation on reset, correctly isolates RandomX's rounding-mode mutations from the host. `-ffast-math` is **off by default** (only `-ffp-contract=fast`, CMakeLists.txt:108), which is the right call — `ARMRX_FAST_MATH`/`-Ofast` would break IEEE determinism and must never be default for a consensus hash. Strength; recommend a comment in CMakeLists warning that `ARMRX_FAST_MATH` breaks hash correctness.

---

## 5. Prioritized Actionable Roadmap

1. **[High impact, low effort] Fix the release-mode OOB fall-through (1.1).** Make the `dataset_read` bounds check actually prevent the read in release builds. This is the one genuine memory-safety defect; ~10 lines.
2. **[High impact, low effort] Fix `BigOnly` to use detected topology (1.4).** Currently `--big-only` can pin to the wrong cluster and lose performance. Reuse `core_order_`.
3. **[Medium impact, low effort] Add `-mtune=cortex-a53` to the device build (2.1)** and measure `bench_armrx` back-to-back. In-order core ⇒ scheduling model matters; plausible free win.
4. **[Security, low-medium effort] Add a SIGSEGV/SIGBUS handler (1.2) and reconsider RWX-by-default (1.3).** At minimum log faults in the JIT region; ideally flip default to W^X behind an opt-in flag.
5. **[Low impact, low effort] Pad per-worker counters against false sharing (2.3); fix stale `mul`/`sub` comment (3.1); windowed hash-rate reporting (3.2).** Cheap hygiene batch.
6. **[Correctness UX, low effort] Warn on thread oversubscription (3.3) and on forced-light-from-unknown-memory (4.1).**
7. **[Strategic, uncertain payoff] Evaluate BOLT (2.5) and interpreted-path prefetch (2.2)** as measured experiments, following the project's established "measure honestly, keep-if-it-helps" protocol.

### Closing note
The `HANDOFF_CLAUDE.md` "verify external claims before acting" discipline is real and has clearly kept this codebase honest — several plausible-sounding perf ideas were correctly rejected after measurement. Every recommendation above (especially 2.1, 2.2, 2.3, 2.4, 2.5) should be **measured apples-to-apples on the A53 before adoption**, per that same protocol. The correctness/safety items (1.1, 1.4, 3.1) stand on their own and don't need benchmarking to justify.
