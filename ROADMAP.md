# armrx — Status Tracker

> **Status tracker for completed and remaining work.**
> For the strategic master plan with ranked priorities, see [`PLAN.md`](PLAN.md).
> For the chronological record, see [`changelogs.md`](changelogs.md).

> **Current status (2026-07-23):** Two critical concurrency bugs found and fixed in an earlier session (fast-mode dataset corruption, `PoolManager` self-deadlock — see their postmortems in `docs/`), plus an on-device LTO build regression root-caused and fixed, both documented pool-failover gaps closed, and three constant-dedup refactors landed. A fresh codebase inspection (`PLAN.md` Phase 4) found two more real bugs — a worker thread that could be permanently killed by a malformed job, and a config-parsing crash risk — both now fixed and regression-tested. Also landed: CBRANCH branch-misprediction investigation (measured a CSEL rewrite, reverted as a net regression) and an Argon2 NEON diagonal-step vectorization (26.8% fewer instructions, 19.0% fewer cycles for cache init). See `NEXT_STEPS.md` for what's still open.
> See `docs/aes-ttable-bug-postmortem.md` for the AES fix analysis, and `docs/fast-mode-dataset-corruption-postmortem.md` / `docs/pool-failover-deadlock-postmortem.md` for this session's critical fixes.

## Baseline

- **Hardware:** Lenovo MSM8916 / Snapdragon 410, 8× Cortex-A53 @ ~1.2 GHz, 2 GiB RAM (postmarketOS, Linux 6.12, GCC 15.2 / musl).
- **Hashrate:** ~5.2 H/s single-thread, ~28–29 H/s 8 workers light mode.
- **Perf profile:** **98.24% of hash time is JIT execution**, 1.76% JIT compile. IPC 0.708 on A53. The widely-cited **31.08%** aggregate branch-miss rate does **not** represent the mining hot path — isolating `bench_armrx --full-hash-only` (2026-07-22) shows only **2.4%** there; the aggregate is 94.93% driven by `--attribution-only`'s non-representative interpreted-mode comparison run. See `docs/branchless-cbranch.md`'s "The 31.08% figure does not represent the mining hot path" section.
- **Region breakdown:** chain/final `run()` = **99%** of hash; AES scratchpad = 0.3%; Blake2b = 0.0%; get_final_result = 0.5%.

---

## ✅ Completed — Phase 0 (Security & Correctness)

| Task | Status |
|------|--------|
| 0.0 — Green baseline recorded (`PERF_BASELINE.txt`) | ✅ |
| 0.1 — S3: `set_dataset` size validation + bounds assert | ✅ |
| 0.2 — S1: JSON escape on TX fields | ✅ |
| 0.3 — S2/S4: `RANDOMX_FORCE_SECURE` honored, `enableAll()` deleted | ✅ |
| 0.4 — `static_assert` linking Program size invariants | ✅ |
| 0.5 — S5: `ARMRX_ASSERT` macro replacing plain `assert()` | ✅ |
| 0.6 — JIT dispatch null guard | ✅ |
| 0.7 — `.gitignore` hygiene | ✅ |
| S6 — TLS peer verification (`SSL_VERIFY_PEER`, `--no-verify-tls`) | ✅ |
| S7 — `emit32` UB (memcpy fix) | ✅ |
| S8 — Dangling pointer contract | ➡️ Moved to Phase 3 (JIT encapsulation) |

## ✅ Completed — Phase 1 (Performance & Tooling)

| O# | Optimization | Status |
|----|-------------|--------|
| O1 | `alignas(16)` on `RegisterFile` | ✅ |
| O2 | Thread-local block template reuse | ✅ |
| O3 | Rounding mode cache | ✅ |
| O4 | Hot path uses span-output `blake2b` overload | ✅ |
| O5 | Dataset huge pages (via `MappedMemory` + `MADV_HUGEPAGE`) | ✅ |
| O6 | NEON Argon2 G-function (2× SIMD, 128→64 `gb` calls) | ✅ |
| O7 | T-table AES fallback (replaces runtime `gf_inverse`) | ✅ |
| — | AES encrypt_transform fix: correct byte order + column permutation | ✅ |
| — | AES decrypt_transform fix: different column permutation from encrypt | ✅ |
| — | NEON AES paths removed (AESE/AESD operation order ≠ RandomX spec) | ✅ |
| O8 | Per-hash `mprotect` skip via `rwx_` flag | ✅ |
| O12 | JIT prologue instruction scheduling | ✅ |
| O13 | JIT register-offset FP loads | ✅ |
| — | `bench_armrx` registered in CTest (3 tests) | ✅ |
| — | ASan/UBSan CMake options | ✅ |
| — | `.clang-format` / `.clang-tidy` baseline configs | ✅ |

## ✅ Completed — Phase 2 (Architecture & Structure)

| P# | Refactor | Status |
|----|----------|--------|
| P0 | `armrx::json` module (escape + tokenizer + `get_array_element`) | ✅ |
| P1 | `run()` split into `run_jit()` / `run_interpreted()` | ✅ |
| P1 | `is_fast_mode()` helper (single source of truth) | ✅ |
| P1 | `compile_instruction` dispatch table (`kCompileHandlers[256]`) | ✅ |
| P2 | `PoolManager` extraction | ✅ |
| P2 | Dead code cleanup (`CodeBuffer`/`CompilerState` removed) | ✅ |
| P2 | Flag constant de-duplication (4 values aliased to `armrx::kRandomX*`) | ✅ |
| P2 | `const_cast` abuse eliminated (8 casts → `mutable` members) | ✅ |
| — | `handle_notify` positional scanner → `get_array_element()` | ✅ |
| O9 | Load interleaving (NEON direct FP loads via `ldr dN` + `sshll`) | ✅ |
| O10 | Prefetch hint tuning (`pldl2keep` → `pldl1keep` for dataset) | ✅ |
| O11 | Branchless CBRANCH (`bne .Lskip; b target` — fixes 99.6% mispredict rate) | ✅ |
| — | `emit32` UB fix (pointer cast → `memcpy`) | ✅ |
| — | hwloc CPU pinning (optional, v2.12.2) | ✅ |
| — | TLS hostname verification | ✅ |
| — | `stratum_` mutex (use-after-free fix) | ✅ |
| — | `session_id_` escape (submit + keepalive) | ✅ |
| — | `get_array_first` dead-code fix (512-byte truncation) | ✅ |
| — | `find_key` scope fix (matches inside string values) | ✅ |
| — | `generateProgram`/`generateProgramLight` dedup (v2 AES-tweak) | ✅ |
| — | `read_buf_` cap at 1 MiB (OOM prevention) | ✅ |
| — | `setPagesRW`/`setPagesRX` return `int` (error propagation) | ✅ |
| — | `mining_engine` silent-swallow fix (log + deactivate) | ✅ |
| — | `json::escape` handles all U+0000–U+001F control chars | ✅ |
| — | CLI numeric arg validation (`try`/`catch` wrappers) | ✅ |
| — | SIGTERM handler (graceful shutdown) | ✅ |
| — | `reconnect_attempts_` → `std::atomic<unsigned>` | ✅ |
| — | `handshake_req_id_` / `authorize_req_id_` → `std::atomic` | ✅ |
| — | `rx_set_rounding_mode` static → per-instance member | ✅ |
| — | `ARMRX_ENABLE_TSAN` CMake option | ✅ |
| — | `vm.hpp` comments (flag divergence, `register_usage_` note) | ✅ |
| — | `jit_compiler_a64_static.S` stale comment fix (12→17) | ✅ |
| — | `ceil_*` constants deleted, `allocate()` comment fixed | ✅ |
| — | `reg_.a` init gated behind `if (!jit_)` | ✅ |
| — | `[DEBUG]` log line removed from `main.cpp` | ✅ |
| — | `main.cpp` SIGTERM handler | ✅ |
| — | `--jit-dump` flag with opcode boundary markers | ✅ |
| — | `bench_opcodes` frequency/byte-cost analyzer | ✅ |
| — | JIT determinism test (`test_jit_determinism`) | ✅ |
| — | CBRANCH encoding unit test (`test_jit_encodings`) | ✅ |
| — | Per-opcode audit (all 30 handlers reviewed) | ✅ |
| — | MAP_HUGETLB for dataset (MappedMemory) | ✅ |
| — | MAP_HUGETLB for cache (Argon2dCache) | ✅ |
| — | MAP_HUGETLB + MADV_POPULATE_WRITE for scratchpad | ✅ |
| — | Structured logger (`include/armrx/log.hpp`) | ✅ |
| — | Cross-thread log sites migrated to logger | ✅ |
| — | Template copy per-hash eliminated (P2.5) | ✅ |
| — | Superscalar heap churn eliminated (P2.5) | ✅ |
| — | TUI: TuiSnapshot + injectable ostream (U1.1-1.2) | ✅ |
| — | TUI: terminal-width + NO_COLOR (U1.3-1.4) | ✅ |
| — | TUI: EMA baseline + atexit cursor (U1.5-1.6) | ✅ |
| — | Share accept/reject counters (U3.1) | ✅ |
| — | `--version` flag with git SHA (U3.5) | ✅ |
| — | Dead config parser removed (U3.7) | ✅ |
| — | Prometheus metrics endpoint | ✅ |
| P2.6 | Profile-Guided Optimization (PGO) unblocked | ✅ |
| P2.7 | Software AES header inlining & register-passing | ✅ |
| P2.8 | big.LITTLE worker thread scheduling | ✅ |
| P2.9 | JIT instruction buffer size expansion (fixes literal pool corruption) | ✅ |
| P2.10| Fast Newton-Raphson division/sqrt math evaluation | ✅ |

### Docs
| Doc | Description |
|-----|-------------|
| [`docs/branchless-cbranch.md`](docs/branchless-cbranch.md) | CBRANCH misprediction analysis, imm19 bug root cause, BTB aliasing caveat |
| [`docs/peephole-jit-plan.md`](docs/peephole-jit-plan.md) | Detailed Phase 3 plan: frequency data, allocation spot-check, per-opcode audit, hashrate veto |
| [`docs/archived/next_phase_v3.md`](docs/archived/next_phase_v3.md) | Archived next-phase improvement plan (v3) — superseded by PLAN.md |
| [`docs/archived/next_phase_v2.md`](docs/archived/next_phase_v2.md) | Archived next-phase improvement plan (v2) |

## ✅ Completed — Phase 3 (This Session, 2026-07-22)

| Item | Status |
|------|--------|
| `MiningEngine` worker-thread reuse for dataset init (barrier via mutex/counter/condition_variable) | ✅ |
| **Critical fix:** fast-mode dataset corruption — wrong output span in multi-threaded `initialize_dataset()` calls (`docs/fast-mode-dataset-corruption-postmortem.md`) | ✅ |
| Mock Stratum protocol test suite (`tests/test_pool_protocol.cpp`, 7 scenarios) | ✅ |
| **Critical fix:** `PoolManager::tick()` self-deadlock on real multi-pool failover (`docs/pool-failover-deadlock-postmortem.md`) | ✅ |
| Pool-failover gap: pool dead from process startup never triggering failover | ✅ |
| Pool-failover gap: up to ~30s stale-reconnect-thread-join delay | ✅ |
| LibFuzzer harness for `armrx::json` (2.5M+ executions, zero findings) | ✅ |
| Argon2d NEON permutation benchmarked and enabled (~16% faster) | ✅ |
| `main.cpp` split into `CommandLineParser` + `MinerApp` | ✅ |
| On-device LTO build regression root-caused (Alpine `fortify-headers` + GCC LTO) and fixed | ✅ |
| AES round-key constants consolidated (`include/armrx/aes_keys.hpp`) | ✅ |
| Scratchpad L3 mask constants unified (`include/armrx/randomx_config.hpp`) | ✅ |
| `kCompileHandlers[256]` derived from `instruction_weights.hpp` instead of hand-maintained | ✅ |
| CBRANCH investigation: CSEL implemented, measured, reverted (net regression); root-caused the 31.08% branch-miss figure to a non-representative benchmark section | ✅ |
| Argon2 NEON diagonal-step vectorization: 26.8% fewer instructions, 19.0% fewer cycles for `Argon2dCache::initialize` (`docs/argon2-neon-diagonal-vectorization.md`) | ✅ |
| Argon2 `memcpy`/copy-elimination investigation: implemented, measured, reverted (no net win — cost relocated, didn't disappear) + follow-up `perf annotate` closed out the remaining "driver code" lead as inherent, already-vectorized XOR-combine work, not a bug (`docs/argon2-compress-copy-elimination.md`) | ✅ |
| `MiningEngine::worker_loop()` permanently killed a worker thread on a bad nonce offset/size — fixed (`active = false; continue;`), regression test added | ✅ |
| `config.cpp` numeric config-file fields unguarded against parse failure — fixed (try/catch, matching `cli_parser.cpp`), regression test added | ✅ |
| `MetricsExporter::server_fd_` data race (plain `int` across threads) — fixed (`std::atomic<int>`) | ✅ |
| `cli_parser.cpp` test coverage added (`tests/test_cli_parser.cpp`) — found and fixed a real bug: `--config=` always exited with "Unknown argument" (code 64) | ✅ |
| `aes_hash.cpp` direct helper test coverage added (`tests/test_aes_hash.cpp`) — golden pins + `hash_and_fill_aes_1r_x4` decomposition-equivalence check | ✅ |

---

## 🔴 Remaining — Action List

### Correctness

_All items found in the `PLAN.md` Phase 4 fresh-codebase inspection are now fixed — see Completed table above._

### Performance

| # | Item | Site | Est. impact | Risk | Notes |
|---|------|------|-------------|------|-------|
| ~~P4~~ | ~~Reduce JIT execution branch-misprediction cost (CSEL for CBRANCH)~~ | `jit_compiler_a64.cpp` | — | — | **Closed 2026-07-22 — implemented, measured, reverted.** CSEL gave +46% branch-misses and flat hashrate vs. the existing `bne`/`b`, not an improvement (BTB-aliasing: the JIT buffer regenerates every hash, so no encoding trick fixes the predictor-history problem). Separately found the 31.08% figure this item's "~8.6–11.9% of cycles" estimate was based on doesn't represent the mining hot path at all — the isolated hot path's real miss rate is 2.4%, costing ~0.1–0.16% of cycles. See `docs/branchless-cbranch.md`. No further CBRANCH JIT work planned. |
| **P3** | **Peephole JIT coalescing** — [`docs/peephole-jit-plan.md`](docs/peephole-jit-plan.md) | `jit_compiler_a64.cpp`, `static.S` | ~+5–10% | 🟡 Medium | Re-evaluate this estimate too — it was framed relative to the same now-corrected 31% branch-miss baseline ("modest vs branch-miss waste"). Not otherwise touched this session. |

### Open decision (not a bug)

| # | Item | Site | Notes |
|---|------|------|-------|
| — | JIT buffer W^X vs RWX default | `virtual_memory.c` | **Default kept RWX (explicit user direction, 2026-07-23), now disclosed via a startup log line in `jit_compiler_a64.cpp` instead of being silent.** Revisit the actual default later if wanted. |

### Features

| # | Item | Effort | Notes |
|---|------|--------|-------|
| — | Stratum V2 protocol support | 🔴 Major | Next-gen pool compatibility |
| — | HTTP Prometheus metrics endpoint | ✅ Completed | Serving GET /metrics, loopback-only |
| — | Newton-Raphson FDIV/FSQRT (O12) | ⏸️ Frozen | **Stale entry corrected 2026-07-23**: this used to describe an early segfault/`x29` register-corruption bug, which was since root-caused and fixed (`docs/WX_Alignment_and_LITTLE_Core_Profiling.md` §5). Newton-Raphson was then fully re-evaluated cleanly: 100% correctness/determinism pass in CTest, but measured **5.12 H/s vs 5.18 H/s for native hardware `fdiv`/`fsqrt` (−1.1%)** — kept OFF because it's slower on the in-order Cortex-A53 (FPU pipeline pressure from the 12-17 instruction approximation), not because of any remaining correctness risk. See `OPTIMIZATION_REFERENCE.md` and `changelogs.md` (2026-07-21). Do not re-enable by default; a future attempt would need to beat −1.1%, not just prove correctness. |

### Maintenance

| # | Item | Effort | Notes |
|---|------|--------|-------|
| — | Cross-compile CI (GitHub Actions + qemu-user) | 🟡 Medium | Optional — you test on real hardware |
| — | Test coverage: `tls_client.cpp`/`tui.cpp` remain fully untested | 🟡 Medium | Need a mock TLS server / terminal-capture harness respectively. `cli_parser.cpp`/`aes_hash.cpp` gaps closed 2026-07-23 — see `PLAN.md` Phase 4 item E. |

---

## Reference Docs

| Doc | Description |
|-----|-------------|
| [`docs/branchless-cbranch.md`](docs/branchless-cbranch.md) | CBRANCH misprediction analysis, imm19 bug root cause, BTB aliasing caveat |
| [`docs/peephole-jit-plan.md`](docs/peephole-jit-plan.md) | Detailed Phase 3 plan: frequency data, allocation spot-check, per-opcode audit, hashrate veto |
| [`docs/archived/next_phase_v3.md`](docs/archived/next_phase_v3.md) | Archived next-phase improvement plan (v3) — superseded by PLAN.md |
| [`docs/jit-buffer-size-audit.md`](docs/jit-buffer-size-audit.md) | JIT buffer size analysis and security audit |
| [`docs/archived/next_phase_v2.md`](docs/archived/next_phase_v2.md) | Archived next-phase improvement plan (v2) |
