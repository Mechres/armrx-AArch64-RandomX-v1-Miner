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
*   [ ] **Further Argon2 profiling lead, not yet investigated.** The same
    `perf record -e cycles` pass that found the diagonal-step fix (212K samples,
    `bench_armrx --argon2-only`) also attributed 23.17% of cycles to
    `Argon2dCache::initialize`'s own code (the main loop/driver, not `gb()`/
    `permute_16_neon`, which the diagonal fix already addressed) and 5.54% to
    `memcpy` (likely `Argon2Block` copies in `argon2_compress`'s
    `auto permuted = result;` and similar). Neither has been profiled further — the
    diagonal-step fix only closed out the `gb()`/`permute_16_neon` share of the
    picture. Worth a look if more Argon2/seed-rotation-latency work is wanted;
    apply the same profile-first discipline (multi-event `perf stat` first, symbol
    attribution before any code change) that worked for the diagonal-step fix.

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
