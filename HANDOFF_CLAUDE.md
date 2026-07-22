# Handoff: armrx — Next Session/Agent

**Session date:** 2026-07-21/22
**HEAD at handoff:** `8f1d24f` (pushed to `origin/main`)
**Devbox:** 192.168.10.156 (Cortex-A53, ~1.8 GiB RAM — see hardware caveat below)

This is a self-contained handoff for whoever picks this up next — human or agent. It covers what changed this session, the current state of the tree, two critical bugs found and fixed (with full postmortems), and a concrete, prioritized next-steps list. Read this before `NEXT_STEPS.md`/`ROADMAP.md`/`STATUS_REPORT.md` — those predate this session and are now stale in places; this document supersedes them for anything they disagree on.

---

## 1. What happened this session, in order

### Phase 1 of `PLAN.md` (small, mechanical fixes) — done
- `MetricsExporter` thread-detach use-after-free fixed (join on destroy instead of detach).
- Stratum nonce offset/size abstracted out of hardcoded literals into `StratumClient` config.
- Dead Windows code path in `virtual_memory.c` annotated (not deleted — see `PLAN.md` §3.3 for why).

### Phase 2 of `PLAN.md` — done, and two critical production bugs found along the way
- **Argon2d NEON permutation enabled** (`src/argon2.cpp`) — benchmarked ~16% faster than scalar on-device, was sitting disabled behind a wrong `#if 0` guard.
- **`main.cpp` split** into `CommandLineParser` (`src/cli_parser.cpp`) + `MinerApp` (`src/miner_app.cpp`) — `main.cpp` is now 10 lines. **This introduced a build regression on-device — see §3 below, not yet root-caused.**
- **`MiningEngine` worker-thread reuse for fast-mode dataset rebuilds** (§2.1) — while writing this feature's correctness test, found and fixed a **critical, pre-existing bug**: `initialize_dataset()`'s multi-threaded callers were passing the wrong output span, silently zero-filling most of any fast-mode dataset built with more than one thread. Fast-mode hashing had effectively been broken whenever `hardware_concurrency() > 1`. Full account: `docs/fast-mode-dataset-corruption-postmortem.md`. This also surfaced that `assert()` was silently compiled out project-wide under the default Release build (`-DNDEBUG`) — fixed with `-UNDEBUG` on the two affected test targets (`armrx_tests`, `test_mining`).
- **Mock Stratum protocol test suite** (`tests/test_pool_protocol.cpp`, §3.2) — 5 scenarios, zero networking test coverage existed before this. While writing the failover scenario, found and fixed a **second critical, pre-existing bug**: `PoolManager::tick()` self-deadlocked (locked a mutex, then called a function that locks the same non-recursive mutex again) the first time real multi-pool failover actually completed its cooldown and reconnected — a documented core feature (README: "automatic failover after 5 retries with a 2s cooldown") that would permanently freeze the miner's pool-management loop on first real use. Full account: `docs/pool-failover-deadlock-postmortem.md`.
- **LibFuzzer harness for `armrx::json`** (`tests/fuzz_json.cpp`, §3.1) — Clang-only, opt-in `ARMRX_BUILD_FUZZERS` CMake option (fails cleanly under GCC). 2.5M+ fuzz executions across two runs, zero crashes, zero ASAN findings — the parser-rewrite alternative in the plan is not currently justified by this evidence.

### Data-gathering (Phase 3 replacement, item C — done; no JIT code touched)
- Fresh on-device branch-miss re-baseline: **31.08%**, essentially unchanged from the pre-PGO `NEXT_STEPS.md` baseline (31.6%) despite everything landed since (PGO, prior JIT scheduling work, the AES fix, Argon2 NEON, worker-thread dataset reuse, the two critical fixes above). See `PLAN.md` §5 item C for full numbers and reasoning.
- **Not started, deliberately**: any CBRANCH/peephole JIT code changes. This was scoped as data-gathering only with an explicit decision gate — see §5 below.

### Also this session
- `CLAUDE.md` was generated via `/init` (repo root, **currently untracked/uncommitted** — matches how `AGENTS.md` is also kept local-only per `.gitignore`).

---

## 2. Current state — how to verify it yourself

Both x86_64 (this dev sandbox) and AArch64 (the devbox) are green as of `8f1d24f`:

```sh
# x86_64 (interpreted VM only, JIT excluded)
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
# → 4/4: armrx_tests, test_mining, bench_armrx, test_pool_protocol
```

On-device, **the standard command above currently fails to link `armrx`** — see the regression in §3. Use:

```sh
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON -DARMRX_DISABLE_LTO=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
# → 7/7: + bench_opcodes, test_jit_encodings, test_jit_determinism
```

The devbox MCP bridge (`tools/devbox/`, `devbox_status`/`devbox_sync`/`devbox_build`/`devbox_test`/`devbox_bench`/`devbox_perf_stat`) was not wired into this session — all on-device work this session went through direct SSH (`rsync` + remote `cmake`/`ctest`/`perf` commands), which worked fine but is more manual. If those MCP tools are available in the next session, prefer them.

**Device is memory-constrained (~1.8 GiB RAM)** — it cannot fit RandomX fast mode at all (`choose_randomx_mode()` confirms: needs ~2338 MiB, only ~1538 MiB available). Any fast-mode test/benchmark work needs a memory-availability guard; see `fast_mode_fits_on_this_host()` in `tests/test_mining.cpp` for the established pattern. `test_mining`'s two fast-mode scenarios correctly skip on this hardware.

---

## 3. Known issues left open, in priority order

### 3.1 Build regression: LTO fails to link `armrx` on-device (not root-caused)
The standard documented build command now fails on the on-device GCC15+musl toolchain — `armrx` (not `bench_armrx` or the test targets) hits a `vsnprintf`/`always_inline` inlining error inside `lto-wrapper`, caused by the `main.cpp` → `cli_parser.cpp`/`miner_app.cpp` split changing how LTO partitions that target. Workaround (`-DARMRX_DISABLE_LTO=ON`) is documented in `README.md`/`REASONIX.md`, but the root cause is unknown. Worth investigating: which specific translation unit or symbol triggers it (bisect by re-merging `cli_parser.cpp`/`miner_app.cpp` back into `main.cpp` temporarily, or try excluding one file at a time from LTO via `set_property(... INTERPROCEDURAL_OPTIMIZATION FALSE)` per-source if CMake supports it at that granularity). This is the same general class of GCC15+musl+LTO fragility as the earlier Newton-Raphson `x29` corruption bug (`docs/jit-buffer-size-audit.md`) — may or may not be fixable without a toolchain update.

### 3.2 Pool failover: two documented-but-unfixed gaps (`docs/pool-failover-deadlock-postmortem.md`)
1. A pool that's unreachable from process startup (vs. one that connects then drops) never triggers failover to the next pool — `reconnect_loop()` is only armed by a connection that was previously up going down.
2. Failover to the next pool can be delayed by up to ~30s beyond the already-real ~31s backoff, because `connect_to_current()`'s replacement of the old `StratumClient` blocks joining a reconnect thread that may be mid-`sleep_for()` for a doomed retry.

Neither is urgent, but both are real, and (2) especially would matter for anyone trying to reason about failover latency SLAs.

### 3.3 Small maintainability items (same bug class as the two critical fixes — duplicated logic silently drifting apart)
1. AES round-key constants duplicated in two different literal encodings across `src/aes_generator.cpp` and `src/aes_hash.cpp`.
2. Scratchpad L3 mask encoded 3 separate ways: `vm.cpp`'s `kScratchpadL3Mask`/`kScratchpadL3Mask64` vs. `jit_compiler_a64.cpp`'s three `Log2(RANDOMX_SCRATCHPAD_L3)` re-derivations.
3. (Lowest priority) `kCompileHandlers[256]` (`vm.cpp:527`) is a hand-maintained interpreter dispatch table, not derived from `instruction_weights.hpp`'s existing weight/REP macros.

None of these are currently causing bugs (both are covered by the JIT/interpreter equivalence KAT), but they're the exact shape of thing that caused this session's two critical bugs — worth closing opportunistically.

---

## 4. Documentation map (what's current vs. stale)

| Doc | Status |
|---|---|
| **This file** | Current as of `8f1d24f`. Start here. |
| `PLAN.md` | Current — actively maintained this session, §5 has the up-to-date phase/task tracker. |
| `changelogs.md` | Current — dated entries for everything this session, newest first. |
| `docs/fast-mode-dataset-corruption-postmortem.md` | Current, critical fix #1. |
| `docs/pool-failover-deadlock-postmortem.md` | Current, critical fix #2. |
| `docs/audit-20260721-cross-reference.md` | **Partly stale** — several items it flagged as unresolved (JIT dedup, dead `getCode()` accessor, `MetricsExporter` cerr usage) turned out to already be fixed when re-checked this session. Don't trust its "remaining work" list without re-verifying against current code first. |
| `docs/performance-next-agent-handoff.md` | Still the deep performance reference (2026-07-19) — its §22.5 ranks "reduce CBRANCH misprediction cost" as the #1 lever, independently consistent with this session's fresh 31.08% branch-miss re-measurement. Worth reading in full before starting any JIT compiler work. |
| `NEXT_STEPS.md`, `ROADMAP.md`, `STATUS_REPORT.md` | **Stale** — all predate this session's changes (some predate the AES fix too). Don't use as a source of truth; `PLAN.md` + this file supersede them. Regenerating them is on the backlog (`PLAN.md` §5, "explicitly deferred"), not done this session. |
| `HANDOFF_GEMINI.md` | Prior session's handoff (2026-07-20/21, untracked). Superseded by this file for current state, but its performance-debugging narrative may still have useful context. |

---

## 5. Recommended next steps, prioritized

### If picking up performance work
`PLAN.md` §5 item C's decision gate is **open, not closed** — the fresh 31.08% branch-miss rate doesn't rule out CBRANCH/peephole JIT work, and it's independently the #1-ranked item in `docs/performance-next-agent-handoff.md`'s own analysis. But:
- `docs/branchless-cbranch.md`'s one prior implementation attempt was inconclusive (see that doc's own "Next Steps for Future Attempts").
- This is security-sensitive JIT hot-path surgery — wrong CBRANCH semantics means wrong hashes, the same bug class as this session's two critical fixes.
- Estimated gain is uncertain (5–15% per the old audit, not re-verified).

**Recommendation:** don't start this without treating it as its own scoped effort with the validation protocol in `docs/performance-next-agent-handoff.md` §19 (KATs + JIT/interpreted equivalence + byte-level determinism checks + before/after hashrate on real hardware, every step). Read that document's §4 (measurement foundation) and §22 (v2 findings) in full first.

### If picking up correctness/maintenance work
1. Root-cause the LTO regression (§3.1) — currently just worked around.
2. The two small pool-failover gaps (§3.2) if pool reliability becomes a priority.
3. The three constant-deduplication cleanups (§3.3) — low-risk, KAT-gated, good "first task" scope.

### If picking up test/coverage work
Phase 2's test coverage is done (mock Stratum, JSON fuzzing, dataset-reinit correctness). No specific gaps identified this session beyond what's listed above — but given how much this session's *new* tests found (two critical, previously-invisible bugs), a reasonable heuristic going forward: **any code path with zero prior test coverage is worth suspecting**, not just extending coverage for its own sake.

### Backlog (deprioritized per explicit user direction this session, not deleted)
- QEMU AArch64 GitHub Actions CI.
- Stratum V2 protocol support.
- `ARMRX_JIT_FAST_DIV_SQRT` CMake flag centralization (low priority even within the backlog — the flag that actually caused a crash is already `PRIVATE`).
- Regenerating `STATUS_REPORT.md`/`ROADMAP.md`/`NEXT_STEPS.md` from current state.

---

## 6. A pattern worth carrying forward

Both critical bugs this session were found by writing a *new* test for an *unrelated* feature (worker-thread dataset reuse; mock Stratum protocol coverage) and having that test fail or hang in a way that turned out to be a real bug, not a test bug. Both times, the instinct that mattered was: **when a new test fails or hangs unexpectedly, assume the code is wrong before assuming the test is wrong**, and trace the actual call chain/lock scope directly rather than immediately reaching for more timeout or a debugger. Both bugs were found this way in well under an hour each, once suspected.
