# armrx — Master Roadmap (Consolidated Final Plan)

> **Single source of truth.** This document merges and supersedes:
> `plan.md`, `improvement.txt`, `jit_plan.md`, `planned_improvements.md`, and the
> backlog items from `OPTIMIZATION_REFERENCE.md`. Where those docs disagreed or
> described already-completed work, the reconciliation is recorded in
> [§1 — Doc Reconciliation](#1-doc-reconciliation). The older docs are retained
> only as history; do not act on them without cross-checking here.
>
> **Method note.** Every `file:line` reference below was verified against the
> current source at consolidation time. Stale claims from the older docs are
> flagged `STALE` and struck from the action list.

---

## 0. Confirmed Baseline (record before any change)

Before touching anything, capture the green baseline so every later "KATs still
pass / perf improved" gate has a reference point.

- **Hardware:** Lenovo MSM8916 / Snapdragon 410, 8× Cortex-A53 @ ~1.2 GHz, 2 GiB RAM (postmarketOS, Linux 6.12, GCC 15.2 / musl).
- **Hashrate:** ~22 H/s, 8 workers light mode (4 big × 3.58 + 4 LITTLE × 1.86). XMRig on same HW: ~27 H/s → **~18% gap**.
- **Perf profile (OPTIMIZATION_REFERENCE.md):** JIT execution ≈ 98.5% of VM-loop time; JIT compile ≈ 1.5%. armrx executes **33% more instructions** than XMRig (64.3B vs 48.2B) at **+5% cycles** and **13× more branch misses**. The gap is *distributed codegen*, not one hotspot.

**Baseline gate (Phase 0.0):**
1. `ctest --test-dir build --output-on-failure` → green.
2. `./build/bench_armrx` → record H/s (commit to `PERF_BASELINE.txt`, gitignored).
3. Run the RandomX KAT vectors (`test_blake2b.cpp:191-200`: "This is a test", "Lorem ipsum dolor sit amet") in **both** JIT mode and interpreted mode (`flags & kRandOMXFlagJit == 0`). Record both green.
4. Snapshot /proc/self/maps after JIT setup — record the dataset/JIT page protections for the W^X regression test (Phase 0.3, Phase 3 W^X test).

If any of the above is not green today, **stop and fix the baseline first** — every subsequent verification checkpoint is meaningless without it.

---

## 1. Doc Reconciliation

Tasks proposed in the older docs that are **already done** or **already attempted** in the current code. These are removed from the action plan to prevent wasted re-work.

| Source claim | Verdict | Evidence in current code |
|---|---|---|
| `improvement.txt 1.2` — per-hash `chrono::now()` syscalls | ✅ DONE | Gated behind `#ifdef ARMRX_JIT_PROFILE` (vm.cpp:820-877) |
| `improvement.txt 1.5` — light-mode `flush_interval = 1` | ✅ DONE | `flush_interval = 64` (mining_engine.cpp:245) |
| `improvement.txt 2.3` — big.LITTLE-aware pinning | ✅ DONE | cpufreq-sorted core order (mining_engine.cpp:20-34) + `pthread_setaffinity_np` at thread creation (mining_engine.cpp:162-166) |
| `improvement.txt 2.4` — per-worker nonce partitioning | ✅ DONE | `local_nonce += num_threads_` stride, no shared atomic (mining_engine.cpp:225-228) |
| `improvement.txt 3.1` — NEON blake2b "unimplemented" | ✅ DONE | Full NEON compress G-function (blake2b.cpp:12, `#if defined(__aarch64__) && defined(__ARM_NEON)`) |
| `jit_plan.md A1` — ubfx scratchpad address | ✅ DONE | static.S (per OPTIMIZATION_REFERENCE table) |
| `jit_plan.md A2` — FSWAP_R `ext` instruction | ✅ DONE | jit_compiler_a64.cpp (per OPTIMIZATION_REFERENCE table) |
| `jit_plan.md A3` — NEON `ld1+sxtl` for FP loads | ✅ DONE | jit_compiler_a64.cpp:591-624 (per OPTIMIZATION_REFERENCE table) |
| `jit_plan.md A4` — next-iteration prefetch | ✅ DONE | static.S:365-372 ("Compute next iteration's scratchpad addresses" + prefetch) + :471 |
| `jit_plan.md B1` — Newton-Raphson FDIV/FSQRT | ❌ FAILED | Segfault / x29 corruption; reverted (OPTIMIZATION_REFERENCE "FAILED"). Do **not** retry without first proving IEEE parity against XMRig's AArch64 source. |
| `improvement.txt 1.6` — per-hash `block_template` *allocation* | ⚠️ STALE framing | `block_input = local_job.block_template;` (mining_engine.cpp:231) — the **allocation** is amortized by vector capacity reuse across hashes, but the **byte copy** still happens every hash. Re-scoped as O2. |
| `improvement.txt 1.1` — blake2b returns `std::vector` | ⚠️ PARTIAL | The vector-returning overload still exists (blake2b.cpp:217), but a span-output overload was added (blake2b.cpp:240). Need to confirm the hot path uses the span overload; if not, switch it. |
| `plan.md §2.3` claim "`mining_engine.cpp:245` flush_interval=64 verified" | ✅ accurate | — |

---

## 2. Security (priority order — these block any production recommendation)

### S3 (CRITICAL) — Out-of-bounds read in fast-mode `dataset_read`
- **Site:** vm.cpp:790-795. `datasetLine = dataset_.data() + address` with no check that `address + 64 ≤ dataset_.size()`. `set_dataset` (vm.cpp:197-199) accepts any span without size validation.
- **Fix:** Add `[[nodiscard]] bool set_dataset(std::span<const std::byte>)` that enforces `size == kRandomXDatasetBytes`, return false otherwise. Add a debug-mode bounds assertion in `dataset_read`.
- **⚠️ Design wrinkle:** light mode never uses the dataset (it calls `generate_dataset_item` from the cache, vm.cpp:798) and legitimately passes an empty dataset. The validation must only enforce the size when `flags_ & kRandOMXFlagFullMem`, OR the API must split into `set_dataset_fast()` / `set_dataset_light()`. **Do not** refuse all undersized spans — that breaks light mode.
- **Surfaced by:** ASan build (Phase 1.4) will immediately confirm.

### S1 (HIGH) — JSON injection in share submission / login
- **Sites:** stratum_client.cpp:358-398. `wallet_`, `password_`, `job.job_id` concatenated raw into JSON. A wallet containing `","admin":"1` can break Stratum semantics. Also affects the input parsers (stratum_client.cpp:70 has a known escape bug: `json[i-1] != '\\'` mis-detects `\\"` — an escaped backslash before a quote).
- **Fix:** `armrx::json_escape(string_view) -> string` helper applied to all TX message fields; replace the 3 hand-rolled input parsers with one real tokenizer (see §4, A3).

### S2 (HIGH) — W^X violation on the primary platform (Linux AArch64)
- **Sites:** jit_compiler_a64.cpp:135 unconditionally attempts `setPagesRWX` in the ctor; `RANDOMX_FORCE_SECURE` is defined (jit_compiler.hpp:73) for OpenBSD/NetBSD/macOS but **never referenced**; `enableAll()` (jit_compiler_a64.cpp:155) is public and re-enables RWX.
- **⚠️ Precision note (vs plan.md's framing):** armrx does *not* skip `enableExecution()` entirely — vm.cpp:842 calls it before invoking the JIT'd program. The genuine regressions are narrower: (a) the unconditional RWX *attempt* in the ctor, (b) `RANDOMX_FORCE_SECURE` being defined-but-ignored. The fix priority 1/2 split is right, just scoped tighter.
- **Fix:** (1) Honor `RANDOMX_FORCE_SECURE` in the ctor — skip `setPagesRWX` when set, accept per-hash RW/RX overhead (negligible per improvement.txt 1.3). On Linux 6.3+ prefer `pkey_mprotect` / `PR_SET_MDWE` for hardware-enforced W^X without syscall cost. (2) Make `enableAll()` private or delete. (Do *not* re-add `enableExecution()` inside `getProgramFunc()` — vm.cpp:842 already covers it; duplicating would double the mprotects.)

### S4 (MEDIUM) — `setPagesRWX` reachable from public API
- `enableAll()` public (jit_compiler_a64.hpp:69). Make private or `= delete`.

### S5 (MEDIUM) — `assert()` compiles out under NDEBUG
- aes_hash.cpp:74, 130, 207, 281 use plain `assert(output.size() % 64 == 0)`; Release (CMakeLists default) defines NDEBUG → inputs never validated in production.
- **Fix:** `ARMRX_ASSERT` macro that always evaluates. **⚠️ Hot-path cost:** aes_hash runs per scratchpad block — an unconditional check is not free. Use debug-assert + release `[[unlikely]]` branch that logs-and-skips rather than an unconditional hard check.

### S6 (MEDIUM) — No TLS certificate verification
- tls_client.cpp:32: `SSL_VERIFY_NONE`. Modern XMR pools overwhelmingly use Let's Encrypt. **Fix:** default `SSL_VERIFY_PEER` + `SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)`, add `--no-verify-tls` opt-out for self-signed pools.

### S7 (LOW) — Unaligned 32-bit JIT store
- jit_compiler_a64.hpp:84 `*(uint32_t*)(code + codePos) = val;` (technically UB; sibling `emit64` uses memcpy). Make `emit32` consistent with memcpy.

### S8 (LOW) — Dangling pointers across module boundaries
- vm.cpp:852, 855 — JIT function receives raw `cache_->blocks().data()` / `dataset_.data()`. If caller frees cache/dataset while a VM holds a JIT'd program, next `run()` reads freed memory. `set_cache` (vm.cpp:186-195) does not invalidate the JIT. **Fix:** document the contract in vm.hpp:67-68 and add `clear_cache_references()`, or hold `std::shared_ptr<const Argon2dCache>` in the VM.

---

## 3. Performance (only still-valid items, reconciled)

### Tier 0 — Free / near-free wins
| # | Optimization | Site | Est. impact | Notes |
|---|---|---|---|---|
| O1 | `alignas(16)` on `RegisterFile` — eliminates per-hash 2 KiB copy | vm.hpp:24-29; copies at vm.cpp:961-963, 969-971 | +1–2% | One line; zero spec risk; blake2b hot path |
| O2 | Thread-local block template reuse — patch nonce in place instead of copying template every hash | mining_engine.cpp:231 (re-scoped from improvement.txt 1.6; the *alloc* is already amortized, the *copy* is not) | +0.5–1% | Reserve once per job; re-sync on `job_generation_` change |
| O3 | Cache rounding mode in `rx_set_rounding_mode` (skip `fesetround` if unchanged) | vm.cpp:68-75 | +0.2% | CFROUND freq 1/256 |
| O4 | Confirm hot path uses the span-output `blake2b` overload, not the vector-returning one | blake2b.cpp:217 vs :240; call sites in vm.cpp hash chain | removes residual allocs if still on vector path | Verify first, then switch |

### Tier 1 — Medium effort, medium impact
| # | Optimization | Site | Est. impact | Notes |
|---|---|---|---|---|
| O5 | Huge pages for 2080 MiB dataset via `allocLargePagesMemory` (already exists, virtual_memory.c:206) | mining_engine.cpp:105 | −30% TLB misses in fast mode | Cache already got huge pages (+8.5%); dataset didn't |
| O6 | NEON Argon2 G-function (`blamka_add`/`permute_block` scalar-only today) | argon2.cpp:66-108 | −20% cache init | ~100M scalar mul-adds on cold start / new key |
| O7 | Replace runtime `gf_inverse` AES fallback with the already-shipped `randomx_aes_lut_enc` T-tables | aes.cpp:36-130, soft_aes.cpp:32 | ~100× on non-crypto builds | Matches upstream |
| O8 | Per-hash `mprotect` pair (W^X toggle) — minimize | vm.cpp:820-877 (enableWriting/enableExecution) | small, but the only remaining per-hash syscall pair | Option A: with S2 fix + `pkey_mprotect`/MDWE, drops to near-zero. Option B: only mprotect the patched immediates, not whole CodeSize |

### Tier 2 — JIT codegen (deferred to after Phase 0/1 security lands)
From `jit_plan.md`. **A1/A2/A3/A4 already done** (see §1). Remaining live items:
| # | Optimization | Site | Est. impact | Risk |
|---|---|---|---|---|
| O9 | A5 — load interleaving (reorder `ldp`/`eor` to overlap load-use latency) | static.S:230-241 | +2–3% | Medium-high — register pressure; all temp regs committed |
| O10 | C2 — dataset prefetch A/B test (`pldl2keep` vs `pldl1keep` at line 341 — L1 may evict scratchpad before use) | static.S:334 | ±0.5% | Very low, empirical |
| O11 | B2 — branchless CBRANCH (only if perf counters show mispredict stalls; A53 has 13× XMRig's branch misses per OPTIMIZATION_REFERENCE) | jit_compiler_a64.cpp:1062-1087 | +1–2% | Touching CBRANCH breaks parity unless exact semantics preserved — needs profiling first |

### Tier 3 — Research-gated, do not enable without KAT proof
| # | Item | Status |
|---|---|---|
| O12 | Newton-Raphson FDIV/FSQRT (jit_plan.md B1) | **FAILED once** (segfault, x29 corruption). Re-attempt only after (a) reading XMRig's AArch64 JIT source, (b) confirming RandomX FDIV output is FP32-precision (≈24-bit) so NR with enough refinement steps suffices, (c) passing the full KAT suite. Not in any committed phase. |

### Honest perf outlook
O1–O11 sum to roughly **+5–10%**, getting armrx from ~22 to ~23–24 H/s. The XMRig gap is ~18%; the remainder is distributed codegen (33% more instructions) that only closes with **Phase 3 — peephole JIT coalescing (major effort)**. **Do not present Phase 1–2 as reaching parity** — it gets roughly halfway.

---

## 4. Architecture & Refactor

| Pri | Refactor | Sites |
|---|---|---|
| P0 | Extract `armrx::json` module (escaping on output, real tokenizer on input); remove the 3 duplicated hand-rolled parsers (stratum_client.cpp:41-115, config.cpp:12-66) | new `include/armrx/json.hpp`, `src/json.cpp` |
| P1 | Split `VirtualMachine::run` into `runJit()` / `runInterp()`; add single `is_fast_mode()` helper (today: interpreter keys off `flags_ & kRandOMXFlagFullMem`, JIT keys off `dataset_.empty()` — two sources of truth) | vm.cpp:807-953, vm.hpp |
| P1 | Replace `compile_instruction` 392-line opcode ladder (24 cascading `if (opcode < ceil_X)` blocks, `auto dst = instr.dst % 8` duplicated ~20×) with `InstructionHandler[256]` dispatch table. **⚠️ Highest-risk single change — merge blocked on full RandomX KAT parity, not just the generic Phase-2 gate.** | vm.cpp:251-642 |
| P2 | Extract `PoolManager` (owns StratumClient list, failover state, reconnect cooldown — currently inline in main.cpp:420-480) and `CliParser`; wire `apply_cli_overrides` (declared config.hpp:37, never called — dead code) | new `src/pool_manager.cpp`, main.cpp |
| P2 | Delete dead JIT scaffolding: `CodeBuffer`/`CompilerState` (jit_compiler.hpp:38-67) never referenced; de-duplicate the re-declared RandomX flag constants in jit_compiler_a64.cpp:36-58 vs randomx_config.hpp | jit_compiler.hpp, jit_compiler_a64.cpp |
| P2 | Fix `const_cast` abuse in StratumClient message builders (methods declared const but mutate `request_id_` etc. via `const_cast<StratumClient*>(this)->...`) | stratum_client.cpp:336-379 |
| P3 | Template AES encrypt/decrypt + HW/fallback hash paths with `if constexpr` | aes.cpp:65-113, aes_hash.cpp |

---

## 5. Testing & CI (currently almost zero)

**Current coverage gap:** only 2 tests in CTest (CMakeLists.txt:103-115); `bench_armrx` is built but **not registered** — a JIT regression would not fail any CI. Direct coverage missing for TLS/Stratum/config/JIT/virtual_memory/TUI/cpu_features; vm has only 2 end-to-end KATs; aes has a single round.

**Phase 1 — trap regressions:**
- blake2b: multi-block (>128 B), output lengths {1,16,33,63}, rejection of `output_bytes ∈ {0,65}`.
- Direct `aes_decrypt_round` KAT (uncovered today).
- `Blake2Generator::get_byte/get_uint32` incl. data-refill branch.
- Register `bench_armrx` as a CTest smoke test (runs without crash; no perf assertion — perf belongs in a nightly job).
- Stratum happy-path with a loopback fake-pool listener thread: subscribe → authorize → notify → submit → accepted.

**Phase 2 — property & negative paths:**
- Determinism property test (same seed → same hash) + bijection-of-prefix up to nonce.
- Malformed JSON from pool (`{"id":1,"result":`, empty `params` arrays), submit `error` objects — no crash / UB.
- Config parser: missing file, nonexistent `--config=`, malformed JSON.
- cpu_features snapshot (assert `aes` flag matches `getauxval(AT_HWCAP) & HWCAP_AES` on AArch64).

**Phase 3 — JIT-specific:**
- `getCodeSize()` invariant (currently returns CodeSize only, excludes superscalar region — fix then assert).
- `static_assert(kRandomXProgramSize * kMaxInstrEncoding <= RANDOMX_PROGRAM_MAX_SIZE * 16)` — no such invariant exists today (configuration.h vs program.hpp vs static.S:281). **Critical JIT safety net.**
- `assert(engine[instr.opcode] != nullptr)` before JIT dispatch (jit_compiler_a64.cpp:174, 263) — today a null member-function pointer is UB on a malformed program.
- W^X transition test: after `enableExecution()` assert the page is `PROT_READ|PROT_EXEC` only via `/proc/self/maps`. Catches the S2 regression.
- Run the full KAT suite **also in interpreted mode** (`flags & kRandOMXFlagJit == 0`) — today only the JIT+HW-AES path is exercised on AArch64.

**Phase 4 — fuzzing:**
- libFuzzer on `randomx_calculate_hash(input, len∈[0,64], out)`.
- libFuzzer on the JSON parsers with a pool-supplied corpus.

**Sanitizers & CI:**
- CMake options `ARMRX_ENABLE_ASAN/UBSAN/TSAN`. ASan immediately surfaces the S3 OOB. TSan on `MiningEngine::start` to verify the `job_generation_` acquire/release (mining_engine.cpp:201-218).
- GitHub Actions matrix: x86_64 (asan+ubsan; tsan), AArch64 (cross-compile → run under qemu-user with KAT suite). Nightly tag: run bench vs committed baseline.

---

## 6. Developer Experience & Hygiene

- **Lint/format:** `.clang-format` (4-space, Allman-ish, `BindingUtil: false` to preserve SIMD line breaks) + `.clang-tidy` (bugprone-*, cert-*, cppcoreguidelines-pro-type-cstyle-cast, readability-magic-numbers, performance-*). `format`/`lint` CMake targets; document `cmake --build build --target format-check` in AGENTS.md.
- **Docs consolidation:** this file is the merge target for the planning docs. After the team ratifies ROADMAP.md, archive `planned_improvements.md` + `improvement.txt` + `jit_plan.md` (or symlink). Keep `OPTIMIZATION_REFERENCE.md` (the only retrospective).
- **`.gitignore` hygiene:** `*.txt` rule (line 6) hides tracked files like `improvement.txt` from `git status` — misleading; scope it tighter. `test_aarch64.cpp` at repo root is uncovered by the ignore. `last_con.md` (55 KB, appears to be a transcript) — **confirm with user whether it contains sensitive info, then remove from history or gitignore** (treat as Phase 0 hygiene, not Phase 3).
- **Local dev loop:** `Justfile`/`Makefile` (`build`, `test`, `bench`, `asan`, `cross-aarch64`) + a dev `CMakePresets.json` (Tests + ASan + Debug).

---

## 7. Phased Action Plan

### Phase 0 — Immediate (this week): security & correctness
| # | Task | Closes | Gate |
|---|---|---|---|
| 0.0 | Record green baseline (ctest + bench + KATs in JIT **and** interp + maps snapshot) | — | green recorded in `PERF_BASELINE.txt` |
| 0.1 | `set_dataset` size validation (fast-mode only, see S3 wrinkle) + debug bounds assert in `dataset_read` | S3 | ASan build clean with deliberately undersized dataset |
| 0.2 | `armrx::json_escape`; escape wallet/password/job_id in TX | S1 | negative-path JSON tests pass |
| 0.3 | Honor `RANDOMX_FORCE_SECURE` in ctor; make `enableAll()` private | S2, S4 | maps-snapshot test shows RX-only after enableExecution |
| 0.4 | `static_assert` linking Program size / `RANDOMX_PROGRAM_MAX_SIZE` / worst-case emit | — | compiles |
| 0.5 | `ARMRX_ASSERT` (debug-assert + release log-and-skip) replacing the 4 plain asserts | S5 | release build still validates |
| 0.6 | `assert(engine[instr.opcode] != nullptr)` before JIT dispatch | — | — |
| 0.7 | Hygiene: resolve `last_con.md`, fix `.gitignore` | §6 | — |

### Phase 1 — Short-term (next 2 weeks): perf & debt baseline
O1 (alignas RegisterFile), O2 (thread-local block template), O3 (rounding cache), O4 (verify blake2b overload), O5 (dataset huge pages), O7 (T-tables), O8 (minimize mprotect) — plus Phase-1 test batch, register `bench_armrx` in CTest, ASan/UBSan CMake options + x86_64 CI matrix, `.clang-format`/`.clang-tidy` baseline, extract `CliParser`/`PoolManager` and wire `apply_cli_overrides`.
**Gate:** bench ≥ 22 H/s; no new KAT failures; CI green on x86_64 + cross-compiled AArch64; ASan clean.

### Phase 2 — Medium-term (1–2 months): deeper structure
P1 `run` split + `is_fast_mode()`, P1 dispatch-table refactor (**KAT-gated**), P0 single `armrx::json` module, O6 (NEON Argon2), S6 (TLS verify), O9/O10/O11 (JIT codegen), Phase-2/3 tests, cross-compile CI under qemu.
**Gate:** all KATs pass; perf ≥ 23 H/s; ASan-clean CI permanently green.

### Phase 3 — Long-term (3+ months): polish & parity
Peephole JIT coalescing (the real path to closing the XMRig gap; "major effort"), HTTP Prometheus endpoint, share counters, Stratum V2, RandomX v2 path validation, hwloc pinning, upstream-KAT CI, ASan-clean hard gate.

---

## 8. Verification Plan
| Phase | Verification |
|---|---|
| 0 | ctest green; ASan x86_64 clean (no vm.cpp:793 OOB with undersized dataset); W^X maps-snapshot test passes |
| 1 | bench ≥ 22 H/s on the reference A53; no new KAT failures; CI green (x86_64 + cross AArch64) |
| 2 | all KATs pass (JIT + interp); perf ≥ 23 H/s; ASan-clean CI permanently green |
| 3 | HTTP endpoint returns non-empty JSON; Stratum V2 KATs pass; perf ≥ 24 H/s (~10% over baseline); full upstream RandomX KAT suite green in CI |
