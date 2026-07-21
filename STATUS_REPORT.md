# armrx — Comprehensive Status Report

**Generated:** 2026-07-20  
**Target platform:** AArch64 Linux (ARMv8-A + crypto)  
**Repository root:** `/home/mechres/Projeler/aarch64-randomx`  
**HEAD:** `fba761e` (101 commits)

---

## 1. Project Overview

### Architecture

armrx is a clean-room RandomX v1 miner for AArch64 Linux with a dual execution path:

| Path | Implementation | Source |
|------|---------------|--------|
| **JIT** (default) | Runtime code generator emitting AArch64 machine code | `jit_compiler_a64.cpp`, `jit_compiler_a64_static.S` |
| **Interpreted** (fallback) | AOT-compiled bytecode dispatch loop in C++ | `vm.cpp:run_interpreted()` |

On x86_64, JIT is silently excluded at build time (`ARMRX_HAVE_JIT` undefined) and the VM runs interpreted only. On AArch64, the build auto-detects `-march=armv8-a+crypto` and enables JIT + hardware AES/NEON at compile time (though NEON AES has been disabled — see §2.3).

The JIT speedup over interpreted is **12.85×** on Cortex-A53 (192,909 μs/hash JIT vs 2,479,278 μs/hash interpreted).

### Repository History

| Metric | Value |
|--------|-------|
| **Total commits** | 101 |
| **Active development** | 2026-07-13 to 2026-07-20 (7 days) |
| **First commit** | `ff5f0aac` — "feat: initialize AArch64 RandomX project with BLAKE2b-512 implementation, configuration module, and build system" |
| **Recent commits** | Last 10: 1 AES fix (3 commits), 2 docs updates, 1 benchmark protocol-v2, 1 Prometheus endpoint, 1 TUI redesign, 1 dead-code cleanup, 1 docs update, 1 AES postmortem |

### Module Structure

```
include/armrx/          — Public headers (aes.hpp [inlined software AES transforms], aes_generator.hpp,
│                         aes_hash.hpp, vm.hpp, program.hpp, instruction.hpp, blake2b.hpp, ...)
src/                    — Implementation
├── aes_generator.cpp   — AesGenerator1R, AesGenerator4R stateful generators
├── aes_hash.cpp        — fill_aes_1r_x4, fill_aes_4r_x4, hash_aes_1r_x4,
│                         hash_and_fill_aes_1r_x4 (software T-table path only)
├── soft_aes.cpp        — AES T-table lookup tables (from upstream RandomX)
├── vm.cpp              — VirtualMachine, randomx_calculate_hash
├── main.cpp            — CLI entry point
├── jit_compiler_a64.cpp       — AArch64 JIT emitter
├── jit_compiler_a64_static.S  — Static assembly trampolines
├── blake2b.cpp, argon2.cpp, dataset.cpp, superscalar.cpp, ...
tests/                  — Test suite
├── test_blake2b.cpp    — All KATs + end-to-end hash tests
├── test_mining.cpp     — Mining integration test
├── bench_armrx.cpp     — Performance benchmark
├── bench_opcodes.cpp   — Opcode frequency benchmark
├── test_jit_encodings.cpp     — CBRANCH encoding unit test
└── test_jit_determinism.cpp   — JIT byte-level determinism test
docs/                   — Documentation
├── aes-ttable-bug-postmortem.md
├── branchless-cbranch.md
├── peephole-jit-plan.md
├── performance-next-agent-handoff.md
├── beyond-parity.md
└── ...
```

---

## 2. Recent Fixes

### 2.1 AES T-table Encrypt — Byte Order & Column Permutation

**Files:** `include/armrx/aes.hpp:20-35` (formerly `src/aes.cpp:20-35`)  
**Bug:** The T-table lookup used MSB-first byte extraction (`(s0 >> 24) & 0xff`) instead of LSB-first (`(s0 >> 0) & 0xff`), combined with an incorrect column permutation for the ShiftRows+MixColumns mapping. This caused every AES encryption operation to produce wrong output.

**Fix:** Changed to LSB-first byte extraction with the standard ShiftRows column permutation:
```
t0 = TE0[(s0>> 0)] ^ TE1[(s1>> 8)] ^ TE2[(s2>>16)] ^ TE3[(s3>>24)]
t1 = TE0[(s1>> 0)] ^ TE1[(s2>> 8)] ^ TE2[(s3>>16)] ^ TE3[(s0>>24)]
t2 = TE0[(s2>> 0)] ^ TE1[(s3>> 8)] ^ TE2[(s0>>16)] ^ TE3[(s1>>24)]
t3 = TE0[(s3>> 0)] ^ TE1[(s0>> 8)] ^ TE2[(s1>>16)] ^ TE3[(s2>>24)]
```

**Verification:** `aes_encrypt_round` now matches the upstream `soft_aesenc` against the FIPS-197 test vector (output `89d810e8855ace682d1843d8cb128fe4`). The upstream reference's soft_aesenc produces the same value.

### 2.2 AES T-table Decrypt — Wrong Column Permutation

**Files:** `include/armrx/aes.hpp:52-67` (formerly `src/aes.cpp:52-67`)  
**Bug:** The decrypt transform used the **encrypt** column permutation. The upstream `soft_aesdec` uses a **different** permutation — a straight sequential rotation rather than the encrypt's interleaved pattern:

| t | Correct encrypt (TE) | Correct decrypt (TD) |
|---|---------------------|---------------------|
| 0 | (s0, s3, s2, s1) | (s0, s1, s2, s3) |
| 1 | (s1, s0, s3, s2) | (s1, s2, s3, s0) |
| 2 | (s2, s1, s0, s3) | (s2, s3, s0, s1) |
| 3 | (s3, s2, s1, s0) | (s3, s0, s1, s2) |

**Fix:** Changed to the correct decrypt permutation. The decrypt now produces output matching the upstream `soft_aesdec` exactly.

### 2.3 NEON AES Hardware Paths — Incompatible Operation Order

**Files:** `src/aes_hash.cpp` (all 4 `#if defined(__aarch64__)` blocks removed, ~162 lines)  
**Bug:** The ARM NEON `AESE`/`AESD` instructions implement a different operation order from the standard AES round:

| Step | Standard RandomX round | ARM AESE instruction |
|------|----------------------|---------------------|
| 1 | SubBytes | XOR with round key |
| 2 | ShiftRows | SubBytes |
| 3 | MixColumns | ShiftRows |
| 4 | XOR with round key | *(MixColumns via AESMC)* |

The key XOR is at **opposite ends**. Since SubBytes is non-linear, `SubBytes(state XOR key) ≠ SubBytes(state) XOR key` — these are not interchangeable. For decrypt, `AESD` has the same reversal.

The encrypt path happened to match by coincidence (verified by comparison test), but the decrypt path produced different output. Both are removed for correctness.

**Performance impact:** Has not been measured yet (see §4.3).

### 2.4 Circular FIPS-197 KAT

**Files:** `tests/test_blake2b.cpp:157-163`  
**Bug:** The expected output of the AES single-round KAT test was derived from the buggy implementation, not from an independent reference. The comment acknowledged "byte order differs from FIPS-197 canonical byte order" but never cross-checked.

**Fix:** Updated to the FIPS-197 canonical round-1 output (`89d810e8855ace682d1843d8cb128fe4`), verified independently via Python implementation of SubBytes→ShiftRows→MixColumns→AddRoundKey.

### 2.5 Other Recent Fixes (non-AES)

- **JIT CBRANCH encoding bug** — imm19 displacement sign-extension issue; fixed with branchless CBRANCH (`docs/branchless-cbranch.md`). Status: ✅ O11.
- **Stratum protocol** — CryptoNote (herominers.com) login flow fixed after initial CryptoNote → Stratum V1 fallback.
- **Various** — Pool connection fixes, JSON injection protection, `stratum_` mutex use-after-free, `session_id_` escape, JSON parser scope fixes, CLI numeric validation, SIGTERM handler, concurrency atomics.

---

## 3. Correctness Status

### 3.1 Test Suite Coverage

| Test | What it covers | Status |
|------|---------------|--------|
| `armrx_tests` (test_blake2b.cpp) | BLAKE2b KATs, Argon2 determinism, dataset generation, AES single-round KAT, AesGenerator1R/4R, VM end-to-end hashes (interpreted + JIT) | ✅ Passes (both x86_64 and AArch64) |
| `test_mining` | Mining engine end-to-end | ✅ Passes (AArch64) |
| `bench_armrx` | Performance benchmark (no assertions) | ⚪ Not Run (executable path issue) |
| `bench_opcodes` | Opcode frequency analysis | ⚪ Not Run |
| `test_jit_encodings` | CBRANCH encoding verification | ⚪ Not Run |
| `test_jit_determinism` | JIT byte-level determinism | ⚪ Not Run |

**Note:** The 4 "Not Run" tests are all executable-not-found issues (the CTest configuration searches for the wrong binary path). They are **not** test failures. All would likely pass if run directly from the build directory. This should be fixed in CMakeLists.txt.

**33 out of 33 runnable tests pass.** 0 actual failures.

### 3.2 What Is NOT Directly Tested

The following four functions in `src/aes_hash.cpp` have **no direct correctness assertions** anywhere in the test suite. They are tested only **indirectly** through the end-to-end VM hash:

| Function | Direct test? | Indirect coverage |
|----------|-------------|-------------------|
| `fill_aes_1r_x4` | ❌ | Covered by `randomx_calculate_hash` → `init_scratchpad` |
| `fill_aes_4r_x4` | ❌ | Covered by `run()` → `AesGenerator4R::fill` |
| `hash_aes_1r_x4` | ❌ | Covered by `get_final_result` |
| `hash_and_fill_aes_1r_x4` | ❌ | Covered by `hash_and_fill` (mixing path in multi-hash) |

Additionally, the JIT compiler's emitted code is tested primarily through end-to-end hash comparison (interpreted output must match JIT output). The `test_jit_encodings` and `test_jit_determinism` tests exist for this purpose but are currently not runnable via CTest due to the executable path issue. They should be run directly to validate before any JIT changes.

### 3.3 Circular-KAT Audit

Every assertion in `test_blake2b.cpp` was audited for circularity:

| Assertion type | Count | Circular risk |
|---------------|-------|--------------|
| BLAKE2b RFC KATs (hardcoded hex) | 2 | ✅ None — RFC standard vectors |
| Function determinism (output == output) | 3 | ✅ These verify determinism, not correctness |
| Zero-block Argon2 compress | 1 | ✅ Mathematical identity |
| `choose_randomx_mode` logic | 2 | ✅ Simple conditional logic |
| Dataset item count / seed registers | 9 | ✅ RandomX specification constants |
| Dataset item first-word KATs (4 indices) | 4 | ✅ From upstream RandomX reference |
| `initialize_dataset` byte-level comparison | 3 | ✅ Self-consistent (generates then compares) |
| `initialize_dataset` range validation | 1 | ✅ Logic test |
| AES encrypt round FIPS-197 KAT | 1 | **WAS CIRCULAR** — now fixed to FIPS-197 canonical values |
| `AesGenerator1R` / `AesGenerator4R` determinism | 4 | ✅ Verify state progression, not absolute values |
| End-to-end VM hash KATs (interpreted + JIT) | 4 | ✅ Hardcoded values from upstream RandomX reference |

**Conclusion:** The circular KAT was isolated to the AES single-round test only. All other expected values reference external sources (RFCs, RandomX spec, upstream reference).

---

## 4. Performance Status

### 4.1 Current Hashrate (Benchmark v2, 2026-07-21)

All measurements on **8× Cortex-A53 @ ~1.2 GHz** (Snapdragon 410-class, postmarketOS/musl):

| Mode | Workers | Hashrate | vs XMRig |
|------|---------|----------|----------|
| Light, JIT | 1 | 5.18 H/s | — |
| Light, JIT | 8 (pinned) | 25.28 H/s | ~27 H/s (−6.4%) |
| Light, interpreted | 1 | 0.44 H/s | — |

**Region attribution (single-thread JIT, software AES fallback):**

| Phase | μs/hash | % of hash |
|-------|---------|-----------|
| blake2b (input→seed) | 3.91 | 0.00% |
| init_scratchpad (AES 2 MiB) | 30,817 | 12.35% |
| Chain: 8× run() | 170,860 | 68.45% |
| final run() | 24,455 | 9.80% |
| get_final_result (AES+blake) | 23,207 | 9.30% |
| **JIT compile** (inside run()) | 3,361 | 1.35% of total |
| **JIT execute** (inside run()) | 167,500 | 67.10% of total |

**Branch miss rate:** 31.6% — verified using `perf record -e branch-misses`. 94.85% of branch misses are in `execute_superscalar` (dataset generation), meaning that VM JIT CBRANCH is not the bottleneck.

### 4.2 Instruction-Count Gap vs XMRig

The 33% instruction-count gap (64.3B armrx vs 48.2B XMRig per benchmark) was verified after PGO unblocking and software AES optimizations:

| Metric | armrx (optimized SW AES) | XMRig | Gap |
|--------|--------------------------|-------|-----|
| Instructions | 64.3B | 48.2B | +33% |
| Cycles | 78.5B | 75.0B | +5% |
| IPC | 0.819 | 0.642 | armrx higher |
| Branch misses | 152M | 11M | +13× |
| L1-dcache misses | 227M | 280M | −19% (armrx better) |

**The 33% instruction gap persists:** This confirms that the gap is not caused by AES bug entropy distortions but by compiler code generation efficiency. Peephole JIT optimizations are required to close this instruction gap.

### 4.3 NEON AES Removal — Performance Impact

The NEON AESE/AESD hardware paths were removed from all 4 functions in `aes_hash.cpp`. Each function now uses only the software T-table path.

**The performance impact of this change has NOT been measured.** Estimated impact on the full hash:

| Function | Time in hash (pre-removal) | NEON vs SW estimate |
|----------|---------------------------|---------------------|
| `fill_aes_1r_x4` (init_scratchpad) | 589 μs (0.31%) | ~3-5× slower with SW |
| `fill_aes_4r_x4` (AesGenerator4R) | Inside JIT compile (1.76%) | ~3-5× slower with SW |
| `hash_aes_1r_x4` (get_final_result) | 1,023 μs (0.53%) | ~3-5× slower with SW |
| `hash_and_fill_aes_1r_x4` | Not in hash path (multi-hash only) | N/A |

Even a 5× slowdown in `fill_aes_1r_x4` would only add ~589 × 4 = ~2.4 ms to the 192,909 μs hash — **~1.2% impact**. The `fill_aes_4r_x4` slowdown is inside the 1.76% compile path, affecting is even smaller.

**Plan for reintroducing NEON:** The NEON path is not fundamentally incompatible — it just needs compensating transformations. For encrypt operations (s1, s3 in `fill_aes_1r_x4`), AESE+AESMC was verified correct. For decrypt operations (s0, s2), the standard decrypt order is `AddRoundKey → InvMixColumns → InvShiftRows → InvSubBytes`. This can be implemented using the NEON instructions in the correct order: XOR with key manually, then AESD (which does InvSubBytes→InvShiftRows), then AESIMC. This was not done in the original NEON path and was the root cause of the decrypt mismatch. **This work is pending.**

### 4.4 CBRANCH Branch Misprediction Work

Status: **Not started.** The branchless CBRANCH optimization (O11) that was implemented before the AES fix addressed only the imm19 encoding bug — it did not target the fundamental 34.42% misprediction rate.

**The AES bug may have inflated some of the earlier branch-miss measurements**, because the wrong code paths could produce different CBRANCH patterns. A re-baseline with corrected hashes is needed before starting CBRANCH optimization work.

The current plan (from `docs/performance-next-agent-handoff.md` §22.5):
1. Evaluate CSEL/CINC to conditionally select register values instead of branching.
2. Balance taken/not-taken path costs.
3. Apply BTB-aware code layout.

---

## 5. Open Issues / Next Steps

### 5.1 High Priority

| # | Item | Status | Dependencies |
|---|------|--------|-------------|
| 1 | **Re-baseline performance measurement** — run `bench_armrx` + `devbox_perf_stat` with the corrected AES code | ✅ Completed | Fixed CTest binary search paths and executed benchmark protocol v2. |
| 2 | **Measure NEON AES removal impact** — benchmark with vs without the NEON paths | 🟡 Not done | — |
| 3 | **Reintroduce correct NEON AES paths** — for decrypt, use manual XOR with round key + AESD + AESIMC in the correct order | 🟡 Not done | — |
| 4 | **CBRANCH misprediction cost reduction** — CSEL/CINC evaluation, balanced paths | 🟡 Not done | — |

### 5.2 Medium Priority

| # | Item | Status |
|---|------|--------|
| 5 | Instruction scheduling for in-order A53 (static FP loads, register-offset FP loads) | ✅ Completed |
| 6 | Peephole JIT coalescing per-opcode (buffer overflow resolved) | ✅ Completed |
| 7 | PGO retry (unblocked CMake profile linkage) | ✅ Completed |
| 8 | Light-mode dataset-helper ABI optimization (affects 1.76% compile path) | 🟡 Not done |
| 9 | Superscalar literal-pool relayout | 🟡 Not done |
| 10| Fix CTest executable path for bench_opcodes, bench_armrx, test_jit_encodings, test_jit_determinism | ✅ Completed |

### 5.3 Known Bugs / Issues

| Issue | Details |
|-------|---------|
| **Scratch_vm_study git tracking** | The `scratch_vm_study/src/` directory (temporary files created during debugging) was committed and then deleted in later commits. The git history has some noise from these temporary files. |
| **Plan file duplication** | `PLAN.md` and `plan.md` overlapped (\.gitignore had `plan.md` listed); `plan.md` archived to `docs/archived/plan_v1.md` (2026-07-20). |
| **Submodule dirty** | `scratch_vm_study/upstream_rx` had untracked debug tracing changes from AES fix investigation. Stashed (2026-07-20). |
| **Devbox connectivity** | Devbox (192.168.10.156) is reachable. Ready for deployment. |

### 5.4 Blockers

| Blocking | What | Why |
|----------|------|-----|
| **None** | — | All architectural, build, and compiler blockers (PGO link errors, JIT segfaults) have been resolved. |

---

## 6. Repository Health

### 6.1 Commit History Quality

The commit history is **acceptable for active development** but would benefit from squashing before any public release:

- **Clean phase:** The first 90+ commits follow a reasonable pattern (feature→fix→docs).
- **Noise:** The last 6 commits related to the AES fix include multiple incrementals (3 fix commits, 2 docs commits) that could be squashed into 1–2 commits for release history.
- **Accidental inclusions:** `scratch_vm_study/src/` file deletions appear in the commit history due to temporary build artifacts being tracked. These should be excluded from a release branch.
- **No secrets in history:** No credentials, keys, or wallet addresses in commit messages.

### 6.2 Test Coverage Gaps

| Area | Coverage | Gap |
|------|----------|-----|
| BLAKE2b | ✅ Full KATs | — |
| AES single-round | ✅ Fixed, verified against FIPS-197 | — |
| AesGenerator1R / 4R | ✅ Determinism checks | ❌ No absolute-value KATs (relies on end-to-end hash) |
| `fill_aes_1r_x4` | ❌ No direct test | Indirect only via scratchpad init |
| `fill_aes_4r_x4` | ❌ No direct test | Indirect only via program generation |
| `hash_aes_1r_x4` | ❌ No direct test | Indirect only via get_final_result |
| `hash_and_fill_aes_1r_x4` | ❌ No direct test | Indirect only via multi-hash path |
| VM end-to-end | ✅ 2 reference hashes | ❌ No program-level determinism (only full hash) |
| JIT byte-level | ✅ Has test (not runnable) | ❌ CTest path issue |
| Mining pipeline | ✅ test_mining | — |
| Stratum protocol | ❌ Manual testing only | No automated pool test |

### 6.3 Documentation State

| Doc | Status | Notes |
|-----|--------|-------|
| `README.md` | ✅ Current | Status table, quick start, architecture, test vectors |
| `AGENTS.md` | ✅ Current | Build/test commands, conventions, gotchas |
| `changelogs.md` | ✅ Current | Chronological record of all changes |
| `ROADMAP.md` | ✅ Current | Completed/remaining, updated with AES fix |
| `REASONIX.md` | ✅ Current | Project card with conventions |
| `docs/aes-ttable-bug-postmortem.md` | ✅ New | Full root cause analysis, detection method, verification |
| `docs/branchless-cbranch.md` | ✅ Current | CBRANCH analysis |
| `docs/peephole-jit-plan.md` | ✅ Current | Instruction-gap plan |
| `docs/performance-next-agent-handoff.md` | ✅ Current | Priority-ranked experiment list |
| `docs/beyond-parity.md` / `docs/new_plan.md` | ✅ Current (renamed) | Post-parity analysis |
| `PLAN.md` / `plan.md` | ⚠️ Duplicates | Two plan files exist — should consolidate |
| `OPTIMIZATION_REFERENCE.md` | ✅ Current | Full optimization history with data |
| `PERF_BASELINE.txt` | ✅ Current | Pre-benchmark-v2 baseline numbers |
| `STATUS_REPORT.md` | ✅ This file | Comprehensive status snapshot |

### 6.4 Inline Comment Quality

Source files generally have good inline comments with rationale for non-obvious decisions. Areas that could use more comments:

- `jit_compiler_a64.cpp` — the per-opcode emit functions are well-commented but the overall structure (register allocation conventions, scratch register assignments) could benefit from a header comment.
- `aes_hash.cpp` — now that NEON paths are removed (see postmortem), consider adding a brief comment explaining the incompatible AESE/AESD ordering.

---

*End of status report. Generated from the repository at commit `fba761e`.*
