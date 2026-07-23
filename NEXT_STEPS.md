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
*   [ ] **`MetricsExporter::server_fd_` data race** (`include/armrx/metrics.hpp`) — plain
    `int` written by the background thread, read by the destructor on another thread,
    no synchronization. Change to `std::atomic<int>`.

### 3. Test coverage (lower priority, no bugs found — just exposure)
*   [ ] `cli_parser.cpp`/`miner_app.cpp` have zero automated unit tests.
*   [ ] `fill_aes_1r_x4`/`fill_aes_4r_x4`/`hash_aes_1r_x4`/`hash_and_fill_aes_1r_x4`
    (`aes_hash.cpp`) still only covered indirectly via end-to-end KAT hashes.
*   [ ] `tls_client.cpp`/`tui.cpp` remain fully untested (need a mock TLS server / a
    terminal-capture harness respectively).

### 4. Open decision — not a bug, needs a call from the maintainer
*   [ ] **JIT buffer W^X vs RWX default** (`src/virtual_memory.c`'s `setPagesRWX()`).
    Currently defaults to RWX unless `RANDOMX_FORCE_SECURE` is set at build time — a
    real, deliberate perf/security tradeoff that's currently invisible to operators
    (no log line, no `--help` mention). Decide: flip the default, or at least log
    which mode is active at startup.

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
