# Next Steps Task List

**Updated:** 2026-07-23
**HEAD:** b406bd5 (at time of writing; see `git log` for current)
**Devbox:** 192.168.10.156

This file mirrors the prioritized, actionable subset of `PLAN.md`'s Phase 4 (a fresh
codebase inspection done 2026-07-22, after Phase 2/3 fully closed out). See `PLAN.md`
for the full evidence/reasoning behind each item; this is the short-list view.

---

## Current Telemetry Invariants (Cortex-A53, Light Mode, JIT)

| Metric | Software AES Baseline (SW) | PGO + SW AES Optimized | Δ |
|--------|:-------------------------:|:----------------------:|:-:|
| **Single-thread hashrate** | 4.34 H/s | **5.18 H/s** | **+19.3%** |
| **8-thread pool hashrate (pinned)** | ~22.0 H/s | **25.28 H/s** | **+14.9%** |
| **Init scratchpad** | 30,817 μs (12.35%) | 30,817 μs (12.35%) | — |
| **Get final result** | 23,207 μs (9.30%) | 23,207 μs (9.30%) | — |
| **Chain execution (VM)** | 194,692 μs (84.6%) | 170,860 μs (68.4%) | **−12.2%** |
| **Branch miss rate (aggregate, `bench_armrx` no flags)** | 31.6% | **31.08%** (re-measured 2026-07-22) | **essentially unchanged** |
| **Branch miss rate (isolated `--full-hash-only`, i.e. the real mining hot path)** | — | **2.4%** (measured 2026-07-22) | — |

The aggregate 31.08% figure does **not** represent the mining hot path — see
`docs/branchless-cbranch.md`'s "The 31.08% figure does not represent the mining
hot path" section. It's 94.93% driven by `bench_armrx --attribution-only`'s
non-representative interpreted-mode comparison run. The real hot path's rate
(2.4%) costs only ~0.1–0.16% of cycles to misprediction — CBRANCH work has
been tried (CSEL, 2026-07-22) and closed; see item 5 below.

---

## Prioritized Next Steps (PLAN.md Phase 4)

### 1. Correctness — do first, both are small and high-value
*   [x] ~~Fix `MiningEngine::worker_loop()` permanently killing a worker thread~~ —
    **done (2026-07-23).** `active = false; return;` → `active = false; continue;`
    on a bad nonce offset/size, matching every neighboring bad-state path. Regression
    test `test_worker_survives_bad_nonce_job()` added to `tests/test_mining.cpp`
    (asserts the *same* worker threads idle through a malformed job, then recover
    and mine normally once a valid job arrives).
*   [x] ~~Guard `config.cpp`'s numeric config-file parsing~~ — **done (2026-07-23).**
    `parse_pool_str()`'s port and `load_config()`'s `workers`/`difficulty`/`seconds`
    conversions wrapped in `try`/`catch`, matching `cli_parser.cpp`'s existing
    treatment of the equivalent CLI flags — malformed values now log a warning and
    fall back to `AppConfig`'s defaults instead of crashing. New
    `tests/test_config.cpp` (3 cases: malformed fields, valid fields, missing file).

### 2. Small hardening fix
*   [x] ~~`MetricsExporter::server_fd_` data race~~ — **done (2026-07-23).**
    `int server_fd_ = -1;` → `std::atomic<int> server_fd_{-1};`. No test added
    (one-line, zero-risk fix per the original finding; not TSan-verified this
    round, existing `-fsanitize=thread` build option covers it if ever re-checked).

### 3. Test coverage (lower priority — but found a real bug anyway)
*   [x] ~~`cli_parser.cpp` unit tests~~ — **done (2026-07-23).** New
    `tests/test_cli_parser.cpp` (15 cases: defaults, every flag family, malformed-value
    exit codes, `--version`/`--help`, unknown-argument handling, config-file/CLI-override
    precedence). **Found a real, previously-unknown bug while writing it**: `--config=<path>`
    was consumed by the config pre-scan but never recognized in the main flag loop, so it
    always fell through to "Unknown argument" and made the process `exit(64)` — i.e. the
    documented `--config=` flag was completely broken. Confirmed against the built `armrx`
    binary before fixing. Fixed by explicitly skipping `--config=` in the main loop
    (`src/cli_parser.cpp`) since the pre-scan already consumed its value.
    (`miner_app.cpp` remains untested — thin orchestration layer, lower value.)
*   [x] ~~`fill_aes_1r_x4`/`fill_aes_4r_x4`/`hash_aes_1r_x4`/`hash_and_fill_aes_1r_x4`
    direct tests~~ — **done (2026-07-23).** New `tests/test_aes_hash.cpp`: golden-output
    pin for `fill_aes_1r_x4` (captured from the current KAT-verified implementation),
    determinism + output-prefix-consistency checks for both fill functions, input-
    sensitivity check for `hash_aes_1r_x4`, and — the main new coverage —
    `hash_and_fill_aes_1r_x4`'s "combined" hash+fill in one pass is asserted to produce
    byte-identical results to calling `hash_aes_1r_x4()`/`fill_aes_1r_x4()` separately on
    the same inputs, pinning down the actual contract the fused function exists to provide.
*   [ ] `tls_client.cpp`/`tui.cpp` remain fully untested (need a mock TLS server / a
    terminal-capture harness respectively) — not attempted this round, larger lift.

### 4. Open decision — default kept for now, now disclosed (2026-07-23)
*   [x] ~~JIT buffer W^X vs RWX default~~ — **decision: keep RWX-by-default unchanged,
    but no longer silent.** `src/virtual_memory.c`'s `setPagesRWX()` still defaults to
    RWX unless `RANDOMX_FORCE_SECURE` is set at build time (explicit user direction:
    "let it stay for now... we may or may not change it later"). `JitCompilerA64`'s
    constructor (`src/jit_compiler_a64.cpp`) now logs which mode is active once per
    process. Revisit the actual default later if wanted — this item stays open only
    in the sense that the underlying tradeoff hasn't been re-decided, just disclosed.

### 5. Performance
*   [x] ~~CBRANCH / JIT branch-misprediction cost reduction~~ — **closed (2026-07-22),
    no further CBRANCH work planned.** Implemented the CSEL rewrite, measured it
    cleanly (apples-to-apples `perf stat`, old code rebuilt fresh for a fair
    baseline), and reverted: +46% branch-misses, flat hashrate, a net regression not
    an improvement. Separately found the 31.08% aggregate figure that justified this
    work doesn't represent the mining hot path — the isolated hot path's real rate is
    2.4%, costing ~0.1–0.16% of cycles, not the previously-estimated ~8.6–11.9%. Full
    account in `docs/branchless-cbranch.md`.
*   [x] ~~Argon2 NEON diagonal-step vectorization~~ — **done (2026-07-23).** 26.8%
    fewer instructions, 19.0% fewer cycles for `Argon2dCache::initialize()` (seed-
    key-rotation latency, not sustained hashrate). Full account in
    `docs/argon2-neon-diagonal-vectorization.md`.
*   [x] ~~`memcpy`/copy-elimination lead~~ — **tried, measured, reverted
    (2026-07-23), no net win.** Gave `permute_block` an out-of-place
    `permute_block_into(src, dst)` sibling to eliminate `argon2_compress`'s
    `auto permuted = result;` 1024-byte copy. Apples-to-apples `perf stat`
    (old code rebuilt fresh, on-device): -2.86% instructions but **+0.35%
    cycles** (flat-to-worse) — symbol attribution showed the `memcpy` cost
    (6.77%→3.56%) didn't disappear, it relocated into the new function
    (12.74%), netting out roughly even. glibc's `memcpy` was already about as
    fast as the hand-rolled replacement on this hardware. Reverted
    (`git checkout -- src/argon2.cpp`). Full account in
    `docs/argon2-compress-copy-elimination.md`.
*   [x] ~~`Argon2dCache::initialize`'s own driver-code cycle share (23.17%
    of the original profile)~~ — **investigated and closed (2026-07-23), no
    action needed.** `perf annotate` (debug-symbol rebuild, instruction-level
    attribution) found this isn't separate driver overhead: 92% of sampled
    instructions in the function cost ≈0%, including the actual address/
    reference-computation arithmetic (`j1`, `square`, `x`, `y`, `relative`,
    `reference`). Every hot instruction is a NEON `eor`/`ldr q`/`str q` —
    `argon2_compress()`'s own XOR-combine loops, auto-vectorized and inlined
    directly into `initialize`'s body by the compiler. It's inherent,
    spec-required compression work, already well-optimized — not a bug or
    missed optimization. **This closes out the Argon2 performance backlog**;
    no further actionable lead from the original profiling pass. Full account
    in `docs/argon2-compress-copy-elimination.md`.

### 5a. Performance — new leads from external audit (`docs/performance-improvement-audit.md`, 2026-07-23)

An untracked audit doc (`docs/performance-improvement-audit.md`, written by another
agent) proposed several leads. Each claim was independently fact-checked against
the actual codebase/history before being added here — one citation error was
found and corrected (see the Newton-Raphson `ROADMAP.md` fix, same date), but the
two substantive recommendations below checked out as accurate and non-redundant
with prior work.

*   [ ] **PGO is plumbed into CMake but not used by the default devbox build.**
    `tools/devbox/devbox_mcp.py:56`'s default `build_flags` is
    `["-DARMRX_ENABLE_NATIVE=ON", "-DARMRX_BUILD_TESTS=ON"]` — no `ARMRX_PGO`.
    Verified: this is worth **+19.3% single-thread / +14.9% at 8 threads**
    (4.34→5.18 H/s / ~22.0→25.28 H/s, matching this file's own telemetry table
    above), and `README.md` advertises the PGO numbers as if they were the
    default build's output. `devbox_build`'s `extra_flags` param **can** already
    pass `-DARMRX_PGO=USE` per-invocation (so this isn't a capability gap, just
    a default/automation gap) — there's no orchestrated generate→train→use flow.
    **Fix:** add a `devbox_pgo_build` flow (GENERATE, train via a representative
    workload e.g. `armrx --mine --seconds=30`, then USE), or at minimum document
    the two-stage release build and clarify in `README.md` that 5.18 H/s requires
    it. Zero code risk — rebuild-only, KATs already gate the flow.
*   [ ] **Prototype NEON `vtbl`/`vqtbl1q`-vectorized software T-table AES.**
    Distinct from the two previously-tried-and-reverted approaches (hardware
    `AESE`/`AESD`/`AESMC` crypto-extension instructions, wrong round order vs
    RandomX spec, then a narrower `AESE`+`AESMC` re-enable that measured "zero
    benefit" and was reverted — `changelogs.md` 2026-07-20). The *current*
    `include/armrx/aes.hpp` `encrypt_transform`/`decrypt_transform` are 100%
    scalar byte-indexed table lookups, no NEON vectorization at all — genuinely
    untried. This is the inner loop of `fill_aes_1r_x4`/`hash_aes_1r_x4`
    (init_scratchpad ~12.35%, get_final_result ~9.30% of full hash on A53, per
    this file's telemetry table). **Fix:** prototype a `vtbl`-based vectorized
    T-table path guarded by `__aarch64__` + a KAT-gated flag, verify parity on
    the *interpreted* path first (KAT + speedup number) before any JIT
    integration — same profile-first discipline as every other perf change this
    session. **Risk:** must be hashrate-vetoed on-device; per-block NEON
    load/store overhead cancelling the lookup speedup on this Cortex-A53 is a
    real possibility (exactly what happened to the hardware-AES re-enable
    attempt) — measure, don't assume.
*   [ ] **(Lower priority, quick experiment) `--stagger-ms` default.**
    Currently defaults to 0 (`src/mining_engine.cpp:313`). A prior worker sweep
    (`changelogs.md` 2026-07-21) found 8-worker saturation drops per-worker
    efficiency 25% (4.25→3.18 H/s/worker) from shared-dataset memory-bus
    contention — a small nonzero stagger might desync the memory-heavy phases
    enough to help. Worth a 1-line on-device experiment, not a design change.

### Backlog (deprioritized per explicit user direction, not deleted)
*   QEMU AArch64 GitHub Actions CI.
*   Stratum V2 protocol support.
*   `ARMRX_JIT_FAST_DIV_SQRT` CMake flag centralization (low priority — the flag that
    actually caused a crash is already `PRIVATE`).

---

## Resolved Since Last Snapshot (2026-07-23)
*   [x] `MiningEngine::worker_loop()` bad-nonce-job worker death — `active = false; return;` → `active = false; continue;`, regression test added.
*   [x] `config.cpp` numeric config-file parsing crash — `try`/`catch` guards added around `workers`/`difficulty`/`seconds`/pool-port, matching `cli_parser.cpp`'s CLI-flag treatment, regression test added.
*   [x] `MetricsExporter::server_fd_` data race — now `std::atomic<int>`.
*   [x] `cli_parser.cpp` test coverage — new `tests/test_cli_parser.cpp`; found and fixed a real bug (`--config=` always exited with "Unknown argument", code 64).
*   [x] `aes_hash.cpp` direct helper test coverage — new `tests/test_aes_hash.cpp` (golden pins + `hash_and_fill_aes_1r_x4` decomposition-equivalence check).
*   [x] Argon2 NEON diagonal-step vectorization — 26.8% fewer instructions, 19.0% fewer cycles for `Argon2dCache::initialize()` (seed-key-rotation latency, not sustained hashrate). See `docs/argon2-neon-diagonal-vectorization.md`.
*   [x] CBRANCH branch-misprediction work — CSEL implemented, measured, and reverted (net regression); root-caused the 31.08% figure to a non-representative benchmark section, not the mining hot path. See `docs/branchless-cbranch.md`.
*   [x] On-device LTO link regression (Alpine `fortify-headers` + GCC LTO incompatibility) — root-caused and fixed, `CMakeLists.txt`.
*   [x] Both documented pool-failover gaps (dead-at-startup pool never failing over; up to ~30s stale-reconnect-thread-join delay) — `src/pool_manager.cpp`, `src/stratum_client.cpp`.
*   [x] AES round-key constants consolidated into `include/armrx/aes_keys.hpp`.
*   [x] Scratchpad L3 mask constants unified into `include/armrx/randomx_config.hpp`.
*   [x] `kCompileHandlers[256]` derived from `instruction_weights.hpp` instead of hand-maintained.

## Resolved Earlier (still valid)
*   [x] **`MetricsExporter` thread-detach use-after-free** — joins on destroy instead of detaching (the *narrower* `server_fd_` race above is a separate, newly-found issue).
*   [x] **Worker Thread Reuse for Dataset Initialization** — `MiningEngine::set_job()` reuses long-lived workers; also surfaced and fixed the fast-mode dataset-corruption bug (`docs/fast-mode-dataset-corruption-postmortem.md`).
*   [x] **Mock Stratum Socket Integration Tests** — `tests/test_pool_protocol.cpp`, 7 scenarios; also surfaced and fixed the `PoolManager` self-deadlock (`docs/pool-failover-deadlock-postmortem.md`).
*   [x] **JSON Parser Fuzzing** — `tests/fuzz_json.cpp`, 2.5M+ executions, zero findings.
*   [x] **NEON `permute_block_neon` benchmarking** — ~16% faster than scalar, enabled.
*   [x] **JIT Buffer Overflow Safety** — root-caused segfaults to PUBLIC flag leaks under GCC 15 LTO; retained doubled 32,768-byte buffer as defense-in-depth.
*   [x] **PGO Linker Errors** — unblocked compiler profile linkage across Alpine/musl and GCC 15.2.0.
*   [x] **CTest path caveat** — re-verified 2026-07-22: did not reproduce in any on-device run this session (all 7 tests pass via plain `ctest` every time). If this recurs, re-add a note here with the exact reproduction steps.
