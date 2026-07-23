# Changelog

## 2026-07-23 — Argon2 `memcpy` Copy-Elimination: Implemented, Measured, Reverted (No Net Win)

Closes out the last item in the Argon2 performance backlog (`NEXT_STEPS.md` §5, `PLAN.md` Phase 3 item C):

- **Investigated the tracked lead**: the diagonal-step profiling pass had also attributed 5.54% of cycles to `memcpy` — traced to `argon2_compress()`'s `auto permuted = result;`, a 1024-byte `Argon2Block` copy needed because `permute_block` mutates its argument in place (the algorithm needs both the original `R = previous^reference` and the permuted `Z` to compute the final XOR).
- **Implemented a fix**: gave `permute_block` an out-of-place `permute_block_into(src, dst)` sibling (both NEON and scalar variants) that fuses the copy into the row step's existing load/store instead of doing a separate whole-block `memcpy` first. Verified correct first: full KAT hashes, reference dataset-item checks, and `ctest` all green, both scalar (x86_64) and NEON (on-device) paths, before any benchmarking.
- **Measured honestly, reverted**: apples-to-apples `perf stat` (old code rebuilt fresh on-device) showed -2.86% instructions but **+0.35% cycles** — flat to slightly worse. Symbol-attributed `perf record` explained why: the `memcpy` cost didn't disappear, it relocated into the new function (`memcpy` 6.77%→3.56%, but a new `permute_block_into_neon` appeared at 12.74%) — glibc's `memcpy` was already about as fast as the hand-rolled replacement on this hardware. Reverted (`git checkout -- src/argon2.cpp`), same standard applied to the CBRANCH/CSEL investigation. Full account in `docs/argon2-compress-copy-elimination.md`.
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
- **Scope, stated honestly**: this speeds up seed-key-rotation *latency* (cache init runs once per ~2048 blocks, not per hash) — it does not change sustained steady-state hashrate. Full account in `docs/argon2-neon-diagonal-vectorization.md`.

## 2026-07-22 — CBRANCH CSEL: Implemented, Measured, Reverted; Root-Caused the 31.08% Figure

Closes the CBRANCH investigation started earlier the same day (see the "Precise Branch-Miss Attribution + Test Hardening" entry below):

- **Implemented the CSEL-based CBRANCH rewrite** `docs/branchless-cbranch.md` had sketched (`ands` for the masked value/flags, two `adr`s to compute both possible next-PC values, `csel` to pick one, single unconditional `br`). Caught one real bug via the KAT test before ever benchmarking: `csel`'s `Rn`/`Rm` register fields were transposed, silently inverting which address got selected — the JIT hash was simply wrong until fixed. Once correct (`ctest` 8/8 on-device), a clean apples-to-apples `perf stat` comparison (old `bne`/`b` code rebuilt fresh in a separate directory for a fair baseline, not compared against a number captured under different measurement overhead) showed CSEL is a **net regression**: +0.87% instructions, +0.32% cycles, **+46% branch-misses**, hashrate flat. **Reverted** to `bne`/`b` (`git checkout` back to `e563112`).
- **Root-caused the 31.08% branch-miss figure** that's justified CBRANCH-focused work across multiple sessions: it does not represent the mining hot path. Isolating `bench_armrx --full-hash-only` shows only **2.4%**. Running `perf stat` on each of `bench_armrx`'s three sections separately and summing reproduces the historical 31.08% aggregate almost exactly, confirming the reconciliation — the aggregate is **94.93%** driven by `--attribution-only`'s non-representative 30-sample interpreted-mode comparison run (never executed during real JIT mining), which is nearly identical to the old audit's "94.85% in Superscalar" claim — strong evidence that claim measured the same phenomenon but misattributed its cause. Recalculated real-world impact: ~0.11–0.16% of cycles lost to CBRANCH misprediction on the actual hot path, not the previously-estimated ~8.6–11.9%.
- **Conclusion: CBRANCH misprediction was never a meaningful real-world performance lever on this hardware.** No further JIT branch-encoding work is planned on this basis. Full account in `docs/branchless-cbranch.md`; `PLAN.md`, `ROADMAP.md`, `NEXT_STEPS.md` updated to close out this item.

## 2026-07-22 — CBRANCH Precise Branch-Miss Attribution + Test Hardening

Prep work for CBRANCH JIT hot-path changes, done before touching any production code:

- **Executed the "profile first" prerequisite** every prior session recommended but none had done: `perf record -e branch-misses` with symbol attribution (not just `perf stat`'s aggregate count) on `bench_armrx --full-hash-only`. Cross-checked a large `[k]`-tagged (kernel-space) bucket against `strace -f -c` on the identical workload (53 syscalls, 6.6ms total) — ruling it out as real kernel work; it's PMU sampling skid on this Cortex-A53 (no ARM SPE), a measurement artifact rather than a performance lever. Confirmed CBRANCH is the JIT compiler's only emitted data-dependent conditional branch (grepped every `0x54xxxxxx` B.cond site in `jit_compiler_a64.cpp`), and confirmed Superscalar is *not* the dominant contributor on this hardware/mode (`generate_superscalar` 0.56% of samples) — refuting the old audit's "94.85% in Superscalar" claim for this case. Full numbers in `docs/branchless-cbranch.md`'s new "Precise attribution" section.
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

See `docs/pool-failover-deadlock-postmortem.md` for the full writeup (updated in place, not a new doc). Summary:

- **Fixed the AUTO-fallback gap**: a pool unreachable from process startup (DNS failure, connection refused before any handshake) never used to trigger failover, since `StratumClient::reconnect_loop()` is only armed by the reader thread noticing a *previously live* connection drop — a pool dead from the start never gets that chance, so `reconnect_attempts()` stayed 0 forever. Added `StratumClient::reconnect_loop_active()` (set synchronously before the reconnect thread spawns, cleared on every exit path) so `PoolManager::tick()` can distinguish "no reconnect loop has ever run" from "one is running but hasn't incremented its counter yet" — the naive `reconnect_attempts()==0` check can't tell these apart, and an earlier draft that used it directly raced ahead of the real exponential backoff (caught because `test_pool_failover` finished in under a second instead of ~31s — a test passing suspiciously fast is still a finding). `PoolManager` now tracks its own `sync_retry_count_` for the never-armed case, using the same 5-retries/2s-cooldown policy.
- **Fixed the stale-reconnect-thread join latency**: `connect_to_current()` destroying the old `StratumClient` used to block up to `kMaxBackoffMs` (30s) in `~StratumClient()`'s join, since `reconnect_loop()`'s `sleep_for()` can't be woken early. Switched to `std::condition_variable::wait_for()` against a new `reconnect_cv_`, woken by `disconnect()` right after it disables the loop — no lost-wakeup race since the predicate re-checks the atomic flag before ever blocking.
- **New test coverage** (`tests/test_pool_protocol.cpp`, now 7 scenarios): `test_failover_from_pool_dead_at_startup` and `test_disconnect_interrupts_reconnect_backoff`. `test_pool_failover`'s timeouts tightened back down (150s → 60s wait, matching the ~31s real backoff without the now-eliminated ~30s stale-join padding).
- Verified: x86_64 local `ctest` 4/4 (`test_pool_protocol` 36s). AArch64 on-device full `ctest` 7/7 (`test_pool_protocol` 36s), confirming `test_pool_failover` still exercises the genuine 1s/2s/4s/8s/16s backoff rather than short-circuiting it.

## 2026-07-22 — Root-Caused and Fixed the On-Device LTO Build Regression

- **Root-caused the on-device LTO link failure** (`CMakeLists.txt`) previously documented but not root-caused, and initially misattributed to the `main.cpp` → `cli_parser.cpp`/`miner_app.cpp` split. Bisection disproved that: building the pre-split commit (`d7ca542`) reproduces the identical failure, and forcing `-flto-partition=one` (single WHOPR partition) does not fix it either — ruling out LTO partitioning as the mechanism entirely.
- **Actual cause:** Alpine's `fortify-headers` package wraps libc calls (`vsnprintf`, reached via `std::to_string(double)` → libstdc++'s `__to_xstring`) in `extern`+`always_inline` functions incompatible with GCC LTO — GCC hard-errors ("function body can be overwritten at link time") instead of emitting an out-of-line call. Confirmed via `apk info`/`/var/log/apk.log` that the on-device toolchain was upgraded `gcc-15.2.0-r6 → r8` on 2026-07-13, well before this session — the bug has been latent since then, just not previously hit by a from-scratch LTO build of the `armrx` executable target specifically.
- **Fixed**: scoped `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` to just the `armrx` target when LTO is enabled, rather than disabling LTO project-wide via the existing `ARMRX_DISABLE_LTO` option. Verified on-device: the standard documented build command (LTO on, no workaround flag) now links `armrx` cleanly and `ctest` passes 7/7.
- Removed the now-stale `-DARMRX_DISABLE_LTO=ON` workaround notes from `README.md` and `REASONIX.md`; corrected the misattribution in `PLAN.md` §5 item C and `docs/fast-mode-dataset-corruption-postmortem.md`.

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

See `docs/pool-failover-deadlock-postmortem.md` for the full writeup. Summary:

- **Added mock Stratum protocol test suite** (`tests/test_pool_protocol.cpp`, PLAN.md §3.2): a loopback POSIX-socket mock server scripting 5 scenarios — Stratum V1 full flow (via AUTO's real CryptoNote-first-then-fallback negotiation), CryptoNote full flow, reconnect-backoff exhaustion, multi-pool failover, and malformed-input robustness. Zero networking test coverage existed before this.
- **Fixed a critical self-deadlock in `PoolManager::tick()`** (`src/pool_manager.cpp`): `tick()` held `stratum_mutex_` for its entire body and called `connect_to_current()` — which locks the same non-recursive mutex again — from inside that scope. Any real multi-pool failover event (a documented core feature: "automatic failover after 5 retries with a 2s cooldown") would permanently freeze the miner's pool-management loop the moment it tried to reconnect to the next pool. Found because `test_pool_failover()` hung indefinitely on first run; fixed by deferring the `connect_to_current()` call until after `tick()`'s lock is released.
- **Two additional findings, documented but not fixed (out of this test-writing task's scope):** (1) a pool unreachable from process startup (vs. one that connects then drops) never triggers failover at all, since `reconnect_loop()` is only armed by a connection that was previously up going down; (2) `connect_to_current()`'s replacement of the old `StratumClient` can block for up to ~30s more (beyond the already-real ~31s backoff) joining a reconnect thread mid-sleep for a doomed retry — invisible on a fast x86_64 sandbox, but directly surfaced by the on-device run's tighter timing via a real test failure (not a hang), and accommodated by widening the test's own timeout budget.
- Verified: x86_64 local `ctest` 4/4. AArch64 on-device full `ctest` 7/7, including `test_pool_protocol`'s 5 scenarios (64.6s).

## 2026-07-21 — Critical Fix: Fast-Mode Dataset Corruption, Plus Test-Suite Assertion and Build Fixes

See `docs/fast-mode-dataset-corruption-postmortem.md` for the full writeup. Summary:

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
  - §4.2 "Unified Compilation Flag Invariants" cited a crash caused by `ARMRX_JIT_FAST_DIV_SQRT` being `PUBLIC`; that flag was already changed to `PRIVATE` (commit `b814c17e`, see `docs/jit-buffer-size-audit.md`). Downgraded from a Phase 1 safety fix to an opportunistic cleanup.
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

- **Conducted JIT Safety Audit & Expanded JIT Buffer** (`src/jit_compiler_a64_static.S`, `docs/jit-buffer-size-audit.md`):
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
- **Peephole JIT plan** ([`docs/peephole-jit-plan.md`](docs/peephole-jit-plan.md)): Created detailed plan for Phase 3 — closing the 33% instruction-count gap vs XMRig. Covers 4 phases: tooling with opcode boundary markers and frequency histograms, per-opcode audit informed by frequency data, cross-opcode optimizations, and hashrate-vetoed validation. Incorporates review feedback: register allocation spot-check, CBRANCH encoding unit test, BTB aliasing caveat.
- **Branchless CBRANCH postmortem** ([`docs/branchless-cbranch.md`](docs/branchless-cbranch.md)): Updated with imm19 root cause, BTB aliasing caveat, and unit test recommendation. Superseded hypotheses marked for clarity.

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
- **O11 — Branchless CBRANCH research** (`docs/branchless-cbranch.md`): New file documenting the CBRANCH misprediction analysis.
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

