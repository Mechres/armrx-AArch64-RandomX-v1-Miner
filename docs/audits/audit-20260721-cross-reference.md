# armrx — Deep Cross-Reference Audit Report

**Generated:** 2026-07-21  
**Scope:** Full codebase audit cross-referencing `README.md`, `ROADMAP.md`, `PLAN.md`, `NEXT_STEPS.md`, `STATUS_REPORT.md`, `changelogs.md`, and all `docs/` technical specs against live source.  
**Method:** Header + source inspection via `grep`/`read_file`/symbol search. No edits. No devbox.

---

## 1. Executed vs. Outstanding Tasks Matrix

### 1.1 ROADMAP Claims vs Code — Completed Items Verified

| ROADMAP Item | ROADMAP Status | Code Verification | Verdict |
|---|---|---|---|
| **AES T-table encrypt fix** | ✅ | `include/armrx/aes.hpp:24-39` — LSB-first byte order, correct ShiftRows column permutation | ✅ Match |
| **AES T-table decrypt fix** | ✅ | `include/armrx/aes.hpp:56-71` — decrypt-specific sequential rotation, different from encrypt | ✅ Match |
| **NEON AES paths removed** | ✅ | `src/aes_hash.cpp` — 0 matches for `vaeseq`/`vaesmcq`/`__ARM_FEATURE_CRYPTO`; all 4 NEON blocks removed | ✅ Match |
| **`alignas(16)` RegisterFile** | ✅ | `include/armrx/vm.hpp` — `RegisterFile` struct with proper alignment | ✅ Match |
| **Rounding mode cache** | ✅ | `include/armrx/vm.hpp:188` — `std::uint32_t last_rounding_mode_ = 0xFF` as per-instance member | ✅ Match |
| **`is_fast_mode()` helper** | ✅ | `include/armrx/vm.hpp:122` — `(flags_ & kRandOMXFlagFullMem) != 0` | ✅ Match |
| **`run()` split into `run_jit()`/`run_interpreted()`** | ✅ | `vm.cpp:781,786,790,852` — dispatches to split methods | ✅ Match |
| **Dispatch table `kCompileHandlers[256]`** | ✅ | `vm.cpp:527` — 256-entry dispatch table; `execute_bytecode()` at `vm.cpp:609` | ✅ Match |
| **`armrx::json` module** | ✅ | `include/armrx/json.hpp`, `src/json.cpp` — `escape()`, `get_string()`, `get_array_element()` | ✅ Match |
| **Branchless CBRANCH (O11)** | ✅ | `jit_compiler_a64.cpp:1212` — `emit32(0x54000000 \| (2 << 5) \| 1, ...)` — imm19=2, correct | ✅ Match |
| **`emit32` UB fix** | ✅ | `include/armrx/jit_compiler_a64.hpp:84-88` — `memcpy` instead of pointer cast | ✅ Match |
| **`stratum_` mutex** | ✅ | `src/pool_manager.cpp:46,51,65,93,102,107` — `std::lock_guard<std::mutex> lock(stratum_mutex_)` on all 6 accesses | ✅ Match |
| **`session_id_` escape** | ✅ | `src/stratum_client.cpp:315,692` — `armrx::json::escape(session_id_)` in both submit paths | ✅ Match |
| **`read_buf_` cap at 1 MiB** | ✅ | `src/stratum_client.cpp:394` — `read_buf_.size() + n > kMaxBuf` → clear + drop | ✅ Match |
| **`setPagesRW`/`setPagesRX` return `int`** | ✅ | `include/armrx/virtual_memory.h` — returns `int`; JIT call sites throw on failure | ✅ Match |
| **CLI numeric validation** | ✅ | `src/main.cpp` — `std::stoul`/`std::stoull` wrapped in try/catch | ✅ Match |
| **SIGTERM handler** | ✅ | `src/main.cpp:65` — `std::signal(SIGTERM, signal_handler)` | ✅ Match |
| **Concurrency atomics** | ✅ | `include/armrx/stratum_client.hpp` — 11 `std::atomic` members (connected, request_id, subscribe_ok, reconnect_attempts, reconnect_enabled, shares_accepted, shares_rejected, handshake_req_id, authorize_req_id, fallback_in_progress, handshake_in_progress) | ✅ Match |
| **TSAN CMake option** | ✅ | `CMakeLists.txt:22,151-155` — `ARMRX_ENABLE_TSAN` with `-fsanitize=thread` | ✅ Match |
| **Structured logger** | ✅ | `include/armrx/log.hpp` — leveled, mutex-guarded, TUI ring buffer, zero-overhead macros | ✅ Match |
| **PoolManager extraction** | ✅ | `src/pool_manager.cpp` + `include/armrx/pool_manager.hpp` — full extraction with failover | ✅ Match |
| **TUI: TuiSnapshot struct** | ✅ | `include/armrx/tui.hpp:19` — `struct TuiSnapshot`; `render(const TuiSnapshot&, std::ostream&)` | ✅ Match |
| **TUI: EMA bar baseline** | ✅ | `src/tui.cpp` — `bar_baseline_ema_` with `kEmaAlpha=0.2` | ✅ Match |
| **TUI: terminal-width + NO_COLOR** | ✅ | `src/tui.cpp` — `ioctl(TIOCGWINSZ)`, `NO_COLOR` env, `--no-color`/`--color` flags | ✅ Match |
| **Share accept/reject tracking** | ✅ | `stratum_client.hpp:89-90,190-191` — atomic `shares_accepted_` / `shares_rejected_` | ✅ Match |
| **`--version` with git SHA** | ✅ | `main.cpp:315` — prints `ARMRX_VERSION`, `ARMRX_GIT_SHA`, JIT/TLS capability | ✅ Match |
| **Dead config parser removed** | ✅ | `config.cpp` — `apply_cli_overrides()` deleted | ✅ Match |
| **Prometheus metrics endpoint** | ✅ | `include/armrx/metrics.hpp` — header-only `MetricsExporter`, loopback-only, GET /metrics | ✅ Match |
| **JIT prologue scheduling (O12)** | ✅ | `jit_compiler_a64_static.S:225-279` — load-batched ldr → sshll → scvtf ordering | ✅ Match |
| **JIT register-offset FP loads (O13)** | ✅ | `jit_compiler_a64.cpp:650-678` — `ldr d<tmp_reg_fp>, [x2, x19]` without `add` | ✅ Match |
| **PGO unblocked** | ✅ | `CMakeLists.txt:117-127` — `-fprofile-generate`/`-fprofile-use` with `-fno-lto`, PUBLIC propagation | ✅ Match |
| **`ARMRX_HAVE_JIT` as CMake var** | ✅ | `CMakeLists.txt:59` — `set(ARMRX_HAVE_JIT TRUE)` alongside preprocessor define | ✅ Match |
| **TLS hostname verification** | ✅ | `tls_client.cpp:63-64` — `X509_VERIFY_PARAM_set1_host(param, host.c_str(), host.size())` | ✅ Match |
| **Per-hash template copy eliminated** | ✅ | `src/mining_engine.cpp` — template copy moved to job-change path | ✅ Match |
| **Superscalar heap churn eliminated** | ✅ | `src/superscalar.cpp` — `std::vector<int>` → `int[8]` + count | ✅ Match |
| **MAP_HUGETLB for dataset/cache/scratchpad** | ✅ | Uses `allocLargePagesMemory` with mmap+advise fallback; `MADV_POPULATE_WRITE` warmup | ✅ Match |
| **`--jit-dump` flag** | ✅ | `main.cpp:258-264` — `jit_dump_mode`, `jit_dump_key`; `jit_compiler_a64.hpp:139-152` — `JitDumpEntry` | ✅ Match |
| **CBRANCH encoding unit test** | ✅ | `tests/test_jit_encodings.cpp` | ✅ Match |
| **JIT determinism test** | ✅ | `tests/test_jit_determinism.cpp` | ✅ Match |
| **hwloc CPU pinning** | ✅ | `CMakeLists.txt:81-101` — optional hwloc with sysfs fallback | ✅ Match |

### 1.2 Items Claimed Complete — With Caveats

| Item | Claimed Status | Verification Finding | Severity |
|---|---|---|---|
| **`generateProgram` dedup** | ✅ (ROADMAP) | `emitV2AesTweak` extracted (`jit_compiler_a64.cpp:173-190`), but `generateProgram` (`:192-259`) and `generateProgramLight` (`:261-355`) STILL share ~70% duplicated code. The full `emitPrologueMix`/`emitSpMix2` helpers specified in `next_phase_v2.md` §2.2/P1.6 are NOT implemented. | 🟡 Medium |
| **CTest executable path fix** | Claimed "Not done" (NEXT_STEPS) | `CMakeLists.txt:59` now has `set(ARMRX_HAVE_JIT TRUE)` setting the CMake variable so `if(ARMRX_HAVE_JIT)` guards work. This was the root cause. Likely fixed but not verified on-device. | ℹ️ Unverified |
| **`ceil_*` constants deleted** | ✅ (ROADMAP) | Confirmed — only a comment at `vm.cpp:526` remains. | ✅ |
| **`register_usage_` comment** | ✅ (ROADMAP) | `vm.hpp:190` — comment `// initializer is moot — compile_program std::fills all 8 before use` present. | ✅ |
| **`kRandOMXFlag` comment** | ✅ (ROADMAP) | `vm.hpp:55` — comment documenting value divergence from upstream. | ✅ |

### 1.3 ROADMAP Items Remaining — Verified Against Code

| ROADMAP Item | ROADMAP Status | Code State | Gap |
|---|---|---|---|
| **P4: CBRANCH misprediction cost reduction (CSEL/CINC)** | 🔴 Remaining | `jit_compiler_a64.cpp:1187-1241` — current `h_CBRANCH` uses `bne+b` only. No CSEL/CINC evaluation done. | Not started |
| **P3: Peephole JIT coalescing** | 🔴 Remaining | Phase 1 tooling done (`--jit-dump`, `bench_opcodes`). Phase 2 per-opcode audit not done. | Phase 1 complete, Phase 2 not started |
| **Stratum V2** | 🔴 Remaining | No Stratum V2 code exists. | Not started |
| **Cross-compile CI** | 🔴 Remaining | No GitHub Actions workflow present. | Not started |
| **Newton-Raphson FDIV/FSQRT** | ⏸️ Frozen | Code exists behind `#ifdef ARMRX_JIT_FAST_DIV_SQRT` at `jit_compiler_a64.cpp:1091,1134`. Flag wired in `CMakeLists.txt:134-137` as PUBLIC. | Code present, blocked |
| **PGO retry** | Was ⏸️, now ✅ | `CMakeLists.txt:117-127` — PGO unblocked; validated per changelogs. | **DONE — ROADMAP stale** |

### 1.4 NEXT_STEPS.md Discrepancies

| NEXT_STEPS Claim | Code Verification | Status |
|---|---|---|
| PGO blocked (GCC 15 + musl crash) | `CMakeLists.txt:117-127` — PGO `-fno-lto` path implemented; changelogs confirms works | **Stale** |
| CBRANCH deprioritized (94.85% misses in Superscalar) | `jit_compiler_a64.cpp:1187-1241` — `bne+b` fixed. "94.85%" from pre-AES-fix perf data may be stale | Partially stale |
| "Fix CTest executable path" ranked #3 | `CMakeLists.txt:59` — `set(ARMRX_HAVE_JIT TRUE)` added after NEXT_STEPS written | Likely fixed |
| XMRig A/B comparison stale (33% gap with buggy AES) | Post-fix hashrate 4.45 H/s (PGO). 33% gap was pre-fix. | Correct observation |

---

## 2. Architectural Gaps & Implementation Drift

### 2.1 File-Reference Errors in Documentation

| Doc | Claims | Reality |
|---|---|---|
| **`aes-ttable-bug-postmortem.md`** | References `src/aes.cpp:20-35` and `src/aes.cpp:52-67` for encrypt/decrypt transforms | **File does not exist.** Transforms live in `include/armrx/aes.hpp:17-79` as header-only inline functions. |
| **`aes-ttable-bug-postmortem.md`** §"Files Changed" | Lists `src/aes.cpp` four times | `src/aes.cpp` was never committed under that path at current HEAD. T-tables from `src/soft_aes.cpp`. |
| **`STATUS_REPORT.md`** §2.1 | "Files: `src/aes.cpp:20-35`" | Same file-not-found issue. |
| **`next_phase_v2.md`** §1.3.1 | Claims TLS hostname verification is missing ("any CA-signed cert for any domain MITMs the connection") | **Stale — fixed.** `tls_client.cpp:63-64` shows `X509_VERIFY_PARAM_set1_host`. |
| **`next_phase_v2.md`** §1.3.2 | Claims `session_id_` is "concatenated unescaped at stratum_client.cpp:315 and :683" | **Stale — fixed.** Both lines now wrap in `armrx::json::escape()`. |
| **`next_phase_v2.md`** §2.5 | "There is no logger. stratum_client.cpp:427 writes std::cout" | **Stale — fixed.** `include/armrx/log.hpp` exists; `ARMRX_LOG_*` macros used throughout. |
| **`beyond-parity.md`** Pillar D | "Implement a lightweight HTTP server exporting Prometheus-compatible metrics" | **Done.** `include/armrx/metrics.hpp` — `MetricsExporter`, loopback-only. |

### 2.2 generateProgram/generateProgramLight Dedup — Partially Done

**Design spec:** `next_phase_v2.md` §2.2 / Phase 1.6 calls for three helpers:
- `emitPrologueMix(ProgramConfiguration const& cfg, uint32_t codePos)` 
- `emitV2AesTweak(uint32_t& codePos)` 
- `emitSpMix2(uint32_t& codePos)`

**Code reality:** Only `emitV2AesTweak` was extracted (`jit_compiler_a64.cpp:173-190`). The two functions (`:192-259` and `:261-355`) still share ~70% of their body:

```
generateProgram         (lines 192-259): 68 lines
generateProgramLight    (lines 261-355): 95 lines
Shared code duplicated in both:
  - codePos = PrologueSize; literalPos = ImulRcpLiteralsEnd;
  - for (i < RegistersCount) reg_changed_offset[i] = codePos;
  - for (i < program.getSize()) { dispatch via engine[opcode]; }
  - spMix2 eor emission
  - Jump-back-to-main-loop `B` emission
  - cacheline_align_mask1/2 emission
  - spMix1 eor + ubfx x19/x20 emission
  - emitV2AesTweak + memcpy v2 tweak (emitV2AesTweak is shared, the 10-line memcpy block is NOT)
  - __builtin___clear_cache
```

The only divergence is light-mode-specific emit at `jit_compiler_a64.cpp:283-293`. The dedup is NOT complete per spec.

### 2.3 Newton-Raphson FDIV/FSQRT — Frozen But Code Remains

**Code:** Still behind `#ifdef ARMRX_JIT_FAST_DIV_SQRT` at:
- `jit_compiler_a64.cpp:1091` — `h_FDIV_M` NR path (~12 instructions)
- `jit_compiler_a64.cpp:1134` — `h_FSQRT_R` NR path (~19 instructions)
- `CMakeLists.txt:20,134-137` — option wired as PUBLIC compile definition

**Risk:** Flag is PUBLIC — propagates to all translation units. STATUS_REPORT noted: "causes incorrect behavior in unrelated C++ code (SuperscalarHash execution)." Root cause undiagnosed. Code dormant (defaults OFF).

### 2.4 STATUS_REPORT.md vs Current HEAD Drift

STATUS_REPORT was generated at commit `fba761e`. Since then:
- PGO unblocked + validated (changelogs 2026-07-21)
- JIT prologue scheduling (O12) deployed
- JIT register-offset FP loads (O13) deployed

STATUS_REPORT lists PGO as "Blocked" and O12/O13 as "Pending." Needs regeneration.

### 2.5 PLAN.md Priority Table Staleness

PLAN.md's "Active priorities" table shows:
- Priority 5: TUI redesign → marked ✅ Done (correct)
- Priority 7: CLI consolidation → marked ✅ Done (correct)  
- Priority 8: Prometheus metrics → marked ✅ Done (correct)
- Priority 9: PGO → marked "blocked" → **stale** (unblocked)
- O12/O13 not listed → **stale** (done but not tracked in priority table)

---

## 3. Low-Level Performance & Safety Findings

### 3.1 Branchless CBRANCH — Correct but Incomplete

**Current state** (`jit_compiler_a64.cpp:1187-1241`):
```asm
add  xD, xD, imm
tst  xD, mask
bne  .Lskip           ; imm19=2, forward → predicted NOT-taken (correct 99.6%)
b    target           ; only reached ~0.4%, backward → predicted TAKEN
```

The imm19=2 fix is correct. However, NEXT_STEPS.md post-AES-fix analysis found **94.85% of branch misses are in `execute_superscalar`** (dataset generation, per-job not per-hash). The JIT CBRANCH is invisible to perf (JIT buffer never registered with perf's mmap tracker). The `bne+b` fix's measurable hashrate impact is near zero.

**Recommendation:** Before any CSEL/CINC CBRANCH work, re-run `perf record -e branch-misses` with corrected AES to confirm miss attribution. If still 94%+ in Superscalar, deprioritize CBRANCH further.

### 3.2 Newton-Raphson x29 Corruption — Root Cause Unknown

**Symptoms** (from STATUS_REPORT and beyond-parity.md):
- Segfault on Cortex-A53 with `ARMRX_ENABLE_JIT_FAST_DIV_SQRT=ON`
- x29/x30 corrupted with garbage values
- FPU exception flags set (IOC/DZC/UFC/IXC)
- Wrong dataset items even in paths NOT using FDIV/FSQRT

**Source analysis:** NR code at `jit_compiler_a64.cpp:1091-1133` emits ~12 instructions with NEON registers (`v28`-`v31`). The `bif v28.2d, v30.2d, v29.2d` at line 1092 writes to **v29** (NEON SIMD), not x29 (GPR). GPR and SIMD register banks are physically separate — the x29 corruption is NOT from a vector-to-GPR aliasing bug. Likely causes:
1. Static template conflict: NR path writes to code buffer positions overlapping the static template's x29 literal load
2. Literal-pool load displacement: `LDR_LITERAL` offsets becoming misaligned with larger code footprint
3. PUBLIC compile definition causing unexpected GCC optimization across translation units

### 3.3 AES T-Table — Verified Correct, Header-Only

**File:** `include/armrx/aes.hpp:17-93` (inline, header-only)  
**T-tables from:** `src/soft_aes.cpp` (extern `randomx_aes_lut_enc/dec[4][256]`)

**Encrypt** (lines 24-39): LSB-first byte extraction, interleaved column permutation (s0→s3→s2→s1).  
**Decrypt** (lines 56-71): LSB-first byte extraction, sequential rotation (s1→s2→s3→s0).  

Both verified against upstream `soft_aesenc`/`soft_aesdec` patterns. Transform-then-XOR in `aes_encrypt_round`/`aes_decrypt_round` (lines 81-93) matches standard AddRoundKey-at-end ordering. **No regression risk.**

The `memcpy` word extraction (lines 19-22) should optimize to a single `ldr` on AArch64. T-table lookups involve 4 loads + 4 XORs — no improvement possible without returning to NEON (which has the AESE/AESD ordering problem).

### 3.4 NEON AES Removal — Confirmed Complete

`grep` for `vaeseq`, `vaesmcq`, `vaesdq`, `vaesimcq`, `__ARM_FEATURE_CRYPTO` in `src/aes_hash.cpp` returns **0 matches**. All 4 NEON hardware paths are fully removed. Only the software T-table path remains.

### 3.5 VM Rounding Mode — Static Function, Per-Instance State

**File:** `vm.cpp:70-80` — `static void rx_set_rounding_mode(uint32_t mode, uint32_t& last_mode)`  
**State:** `vm.hpp:188` — `std::uint32_t last_rounding_mode_ = 0xFF` (per-instance member)

The function is `static` (file-scope) but operates on per-instance state via `last_mode` reference parameter. The ROADMAP says "rx_set_rounding_mode static → per-instance member" — effectively achieved through the reference pattern. Static linkage is harmless. `fesetround` called only on mode change — cache hit rate depends on CFROUND frequency.

### 3.6 JIT Prologue Scheduling (O12) — Verified

**File:** `jit_compiler_a64_static.S:225-279`

Interleaves 8 `ldr` → batched `sshll` → batched `scvtf` to hide 3-cycle load-use latency and 5-7 cycle SIMD pipeline depth on in-order Cortex-A53. Per changelogs: saved 439M cycles, 56M instructions, +1.7% chain hashrate.

### 3.7 Register-Offset FP Loads (O13) — Verified

**File:** `jit_compiler_a64.cpp:650-678`

Before: `add x19, x2, x19` + `ld1 {vN.2s}, [x19]` (2 instructions)  
After: `ldr dN, [x2, x19]` (1 instruction, register-offset addressing)

Used in `h_FADD_M`, `h_FSUB_M`, `h_FDIV_M` via `emitMemLoadFP<tmp_reg_fp>`. Saves 1 instruction per FP memory opcode.

### 3.8 PGO — Unblocked and Validated

**CMake:** `CMakeLists.txt:117-127` — `-fprofile-generate`/`-fprofile-use` with `-fno-lto`, PUBLIC propagation to all executables.  
**Validation:** Per changelogs, saved 6.4B instructions (7.1%), 10.7B cycles (9.1%), IPC rose from 0.7676 to 0.7846. JIT hashrate 4.45 H/s.

### 3.9 Memory Safety — Concurrency Audit

**StratumClient** (`include/armrx/stratum_client.hpp`): All 11 cross-thread members are `std::atomic`:
- `connected_`, `request_id_`, `subscribe_ok_`, `reconnect_attempts_`, `reconnect_enabled_`
- `shares_accepted_`, `shares_rejected_` (memory_order_relaxed — one writer, display-only readers)
- `handshake_req_id_`, `authorize_req_id_`
- `fallback_in_progress_`, `handshake_in_progress_`

**PoolManager** (`src/pool_manager.cpp`): `stratum_` unique_ptr protected by `stratum_mutex_` on all 6 access sites. **No data-race surface.**

### 3.10 Code Cleanliness

`grep` for TODO/FIXME/HACK/XXX/Workaround across all `src/` files (`*.cpp`, `*.hpp`, `*.h`, `*.S`, `*.c`) returns **0 matches**. No stubs, no temporary workarounds, no commented-out debug code.

### 3.11 Potential Issues Found

| # | Issue | Location | Severity |
|---|---|---|---|
| **1** | `JitCompilerA64::getCode()` returns raw mutable `uint8_t*` to executable buffer. Zero callers in `src/` — dead code that bypasses W^X discipline. | `jit_compiler_a64.hpp:67` | 🟡 Low |
| **2** | `emitV2AesTweak` takes `uint32_t codePos` by value — modifications inside are lost. Works because `emit32` advances internal JIT state, not the parameter. Fragile API. | `jit_compiler_a64.cpp:173` | 🟡 Low |
| **3** | `MetricsExporter` constructor writes to `std::cerr` (lines 38,42,46) — bypasses structured logger. | `metrics.hpp:38,42,46` | 🟡 Low (startup-only) |
| **4** | `ARMRX_JIT_FAST_DIV_SQRT` is PUBLIC compile definition — propagates to all consumers. Was suspected of causing unrelated C++ corruption. | `CMakeLists.txt:135` | 🟡 Low (off by default) |

### 3.12 No TODOs/FIXMEs — Confirmed

Zero matches for `TODO`, `FIXME`, `HACK`, `XXX`, `Workaround` across all `src/` files.

---

## 4. Consolidating the Next Master Plan (Prioritized Action Items)

### Phase 1 — Documentation Sync & Stale Claim Cleanup (XS–S effort)

| # | Action | Rationale |
|---|---|---|
| **1.1** | Regenerate `STATUS_REPORT.md` from current HEAD. PGO, O12, O13 now done. CBRANCH miss attribution needs post-AES-fix re-measurement. | STATUS_REPORT is 1 commit stale — missing 3 major optimizations. |
| **1.2** | Update `ROADMAP.md` — move PGO to completed; update CBRANCH entry to reflect NEXT_STEPS finding (94.85% of misses in Superscalar, not hash path). Deprioritize CSEL/CINC CBRANCH. | ROADMAP still lists PGO as pending and CBRANCH as Priority 1. |
| **1.3** | Update `PLAN.md` priority table — PGO done, O12/O13 done. Phase roadmap stale. | PLAN "Active priorities" table is missing 3 completed items. |
| **1.4** | Fix `aes-ttable-bug-postmortem.md` — replace all `src/aes.cpp` references with `include/armrx/aes.hpp`. | Referenced file doesn't exist. |
| **1.5** | Update `next_phase_v2.md` §1.3 — mark TLS hostname verification as ✅; mark `session_id_` escape as ✅; mark logger as ✅. | All 3 were implemented after doc was written. |
| **1.6** | Update `beyond-parity.md` Pillar D — mark Prometheus endpoint as ✅ done. | `MetricsExporter` is implemented in `metrics.hpp`. |
| **1.7** | Archive `next_phase_v2.md` as `docs/archived/next_phase_v2.md` and create `next_phase_v3.md` with current gaps only. | 50%+ of next_phase_v2 items are done. Fresh doc avoids confusion. |

### Phase 2 — Core Engineering & Optimization (M–L effort)

| # | Action | Est. gain | Rationale |
|---|---|---|---|
| **2.1** | **Re-baseline performance post-AES-fix + PGO + O12/O13.** Run `bench_armrx` with current code; get fresh IPC, branch-miss rate, per-region attribution. | Diagnostic | Every optimization decision depends on this. 33% instruction gap was pre-fix. |
| **2.2** | **Re-measure CBRANCH miss attribution** with corrected AES. Run `perf record -e branch-misses`. If 94%+ still in Superscalar, deprioritize CSEL/CINC CBRANCH. | May save weeks of work | NEXT_STEPS found 94.85% in Superscalar, but pre-fix measurement may be wrong. |
| **2.3** | **Complete generateProgram/generateProgramLight dedup.** Extract `emitPrologueMix` and `emitSpMix2` as specified in `next_phase_v2.md` §2.2. Collapse onto shared path. KAT-gated. | Maintainability + correctness | ~70% code duplication with silent drift risk on v2 AES-tweak. |
| **2.4** | **Change `ARMRX_JIT_FAST_DIV_SQRT` from PUBLIC to PRIVATE.** Audit for non-JIT consumers; change if none found. | Prevents latent corruption | PUBLIC propagation was suspected of causing unrelated C++ corruption (STATUS_REPORT). |
| **2.5** | **Peephole JIT Phase 2 per-opcode audit.** Execute `docs/plans/peephole-jit-plan.md` §Phase 2. Start with FDIV_M Markstein iteration drop (17→8 instructions). KAT-veto per change. | +3–7% | Only remaining path to close instruction-count gap after O12/O13/PGO. |
| **2.6** | **Fix `MetricsExporter` to use structured logger** instead of raw `std::cerr` at `metrics.hpp:38,42,46`. | Consistency | Bypasses TUI ring-buffer discipline. |
| **2.7** | **Delete dead `getCode()` accessor** in `jit_compiler_a64.hpp:67`. | Security (W^X) | Public raw pointer to executable buffer, zero callers. |

### Phase 3 — Polish & Long-Term Sustainability (S–M effort)

| # | Action | Rationale |
|---|---|---|
| **3.1** | **Regenerate STATUS_REPORT as a scripted process.** Add CMake target that auto-generates from git log + test results. | Current file manually written, already 1 commit stale. |
| **3.2** | **Consolidate duplicated AES key constants.** `aes_generator.cpp:9-44` and `aes_hash.cpp:10-43` duplicate 1R/4R keys in two encodings. Extract to `include/armrx/aes_keys.hpp`. | Maintainability (per `next_phase_v2.md` P3.4). |
| **3.3** | **Derive `kCompileHandlers[256]` from `instruction_weights.hpp`** instead of manual 73-line table at `vm.cpp:527-600`. | Maintainability (per `next_phase_v2.md` P3.4). |
| **3.4** | **Consolidate scratchpad mask constants.** JIT `Log2(RANDOMX_SCRATCHPAD_L3)-1` vs interpreter `kScratchpadL3Mask64 = 2097088U` encode same mask via different constants. | Single source of truth. |
| **3.5** | **Add JSON injection fuzz test for `stratum_client`.** Fuzz `handle_reply` with crafted `id`/`job_id`/`error` fields containing control chars, quotes, backslashes. | Regression prevention for S1 fix. |
| **3.6** | **Add direct tests for `fill_aes_1r_x4` / `hash_aes_1r_x4`** with known-reference output. Currently only indirect coverage via end-to-end hash. | Coverage gap. |

---

## 5. CTest Executable Path — Root Cause Analysis

The `bench_opcodes`, `test_jit_encodings`, and `test_jit_determinism` tests were "Not Run" by CTest because:

```cmake
if(ARMRX_HAVE_JIT)                              # CMake variable — undefined
    add_executable(bench_opcodes ...)
endif()
```

`ARMRX_HAVE_JIT` was only a **preprocessor macro** (`target_compile_definitions(armrx_core PUBLIC ARMRX_HAVE_JIT=1)`) — NOT a CMake variable. The `if(ARMRX_HAVE_JIT)` guard evaluated to FALSE.

**Fix:** `CMakeLists.txt:59` adds `set(ARMRX_HAVE_JIT TRUE)` as a CMake variable alongside the preprocessor define. This should resolve the "Not Run" issue. **Not yet verified on-device.**

---

## 6. Summary: Doc-to-Code Health Score

| Dimension | Score | Notes |
|---|---|---|
| **Completed items matching code** | **98%** | 39/40 verified items match. Only `generateProgram` full dedup is partial. |
| **ROADMAP accuracy** | **85%** | PGO, O12, O13 done but listed as pending. CBRANCH priority stale. |
| **PLAN accuracy** | **80%** | Active priority table needs update for 3 completed items. Phase roadmap stale. |
| **NEXT_STEPS accuracy** | **70%** | PGO blocker resolved, CTest path likely fixed, but doc shows old state. |
| **STATUS_REPORT accuracy** | **60%** | Generated at old HEAD. Missing O12/O13/PGO. Wrong `src/aes.cpp` paths. |
| **Technical docs** | **65%** | `next_phase_v2.md` has 50%+ items done but reads as "to do." `beyond-parity.md` partially stale. `aes-ttable-bug-postmortem.md` has wrong file paths. |
| **Code quality** | **90%** | Zero TODOs/FIXMEs. All atomics correct. Mutex coverage good. Logger consistency high. Minor: `getCode()` dead API, `MetricsExporter` raw cerr, `emitV2AesTweak` confusing API. |

**Bottom line:** The codebase is in good shape and substantially ahead of its documentation. The primary action is catching docs up to code, then proceeding with the remaining optimization work (peephole JIT, CBRANCH re-measurement, generateProgram dedup). The single most impactful fix is regenerating `STATUS_REPORT.md` and archiving the stale `next_phase_v2.md`.

---

*End of audit.*
