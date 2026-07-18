# armrx — Master Roadmap (Consolidated Final Plan)

> **Current phase: Phase 2/3 — structured logger deployed, memory tier upgrades complete.**
> Phase 1 stabilization is complete. Peephole JIT tooling (--jit-dump, bench_opcodes,
> determinism/encoding tests) is delivered. Structured logger (log.hpp) replaces all
> raw std::cerr/cout across cross-thread log sites. Memory tier upgrades
> (MAP_HUGETLB + MADV_POPULATE_WRITE) deployed. Next: per-hash hot-path reductions,
> Prometheus endpoint, or PGO unblock.
> See [changelogs.md](changelogs.md) for the chronological record.

## Baseline

- **Hardware:** Lenovo MSM8916 / Snapdragon 410, 8× Cortex-A53 @ ~1.2 GHz, 2 GiB RAM (postmarketOS, Linux 6.12, GCC 15.2 / musl).
- **Hashrate:** ~24–27 H/s, 8 workers light mode (after all Phase 0–2 optimizations). XMRig on same HW: ~27 H/s → **~0–12% gap**.
- **Perf profile:** JIT execution ≈ 98.5% of VM-loop time; armrx executes **33% more instructions** than XMRig (64.3B vs 48.2B) and **13× more branch misses**. The gap is *distributed codegen*.

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
| O8 | Per-hash `mprotect` skip via `rwx_` flag | ✅ |
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

### Docs
| Doc | Description |
|-----|-------------|
| [`docs/branchless-cbranch.md`](docs/branchless-cbranch.md) | CBRANCH misprediction analysis, imm19 bug root cause, BTB aliasing caveat |
| [`docs/peephole-jit-plan.md`](docs/peephole-jit-plan.md) | Detailed Phase 3 plan: frequency data, allocation spot-check, per-opcode audit, hashrate veto |
| [`docs/next_phase_v2.md`](docs/next_phase_v2.md) | Comprehensive next-phase improvement plan (post-review v2) |

---

## 🔴 Remaining — Action List

### Performance

| # | Item | Site | Est. impact | Risk | Notes |
|---|------|------|-------------|------|-------|
| **P3** | **Peephole JIT coalescing** — [`docs/peephole-jit-plan.md`](docs/peephole-jit-plan.md) | `jit_compiler_a64.cpp`, `static.S` | ~+15–20% | 🔴 Major | 4-phase plan: tooling → frequency-informed opcode audit → cross-opcode → hashrate veto. The only path to close the 33% instruction-count gap. |
| PGO | Profile-Guided Optimization | CMake option (`ARMRX_PGO=GENERATE/USE`) | ~+5–10% | 🟡 Medium | Blocked: GCC 15 + musl `__gcov_*` linker crash. See `OPTIMIZATION_REFERENCE.md:47`. |

### Features

| # | Item | Effort | Notes |
|---|------|--------|-------|
| — | Stratum V2 protocol support | 🔴 Major | Next-gen pool compatibility |
| — | HTTP Prometheus metrics endpoint | 🟡 Medium | Monitoring / dashboard integration |
| — | Newton-Raphson FDIV/FSQRT (O12) | ⏸️ Frozen | Failed once (segfault). Do not retry without KAT proof and XMRig source study. |

### Maintenance

| # | Item | Effort | Notes |
|---|------|--------|-------|
| — | Cross-compile CI (GitHub Actions + qemu-user) | 🟡 Medium | Optional — you test on real hardware |
| — | Phase-2 test suite expansion | 🟡 Medium | Property tests, malformed JSON, etc. |

---

## Reference Docs

| Doc | Description |
|-----|-------------|
| [`docs/branchless-cbranch.md`](docs/branchless-cbranch.md) | CBRANCH misprediction analysis, imm19 bug root cause, BTB aliasing caveat |
| [`docs/peephole-jit-plan.md`](docs/peephole-jit-plan.md) | Detailed Phase 3 plan: frequency data, allocation spot-check, per-opcode audit, hashrate veto |
