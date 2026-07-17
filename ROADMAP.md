# armrx — Master Roadmap (Consolidated Final Plan)

> **Current phase: Phase 3 (peephole JIT coalescing).** Phase 0–2 are substantially
> complete. Completed items are marked ✅ — remaining items are the live action list.
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
| S7 — `emit32` UB (mitigation deferred, low priority) | ⏸️ |
| S8 — Dangling pointer contract (docs deferred) | ⏸️ |

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
| — | Pool connection fixed (handshake race, JSON `"id"` parsing) | ✅ |
| — | KATs in both JIT + interpreted mode | ✅ |

---

## 🔴 Remaining — Action List

### Performance

| # | Item | Site | Est. impact | Risk | Notes |
|---|------|------|-------------|------|-------|
| **O11** | **Branchless CBRANCH** — replace `beq` (99.6% mispredicted) with `bne+b` or `csel` approach | `jit_compiler_a64.cpp` | +1–2% | 🟡 Medium | `docs/branchless-cbranch.md` written. Needs profiling first to confirm hotspot. **Recommended next step.** |
| **P3** | **Peephole JIT coalescing** — the only path to close the 33% instruction-count gap vs XMRig | `jit_compiler_a64.cpp`, `static.S` | ~+15–20% | 🔴 Major | Months of work. Study XMRig's AArch64 JIT output, iterate on generated code density. |

### Features

| # | Item | Effort | Notes |
|---|------|--------|-------|
| — | Stratum V2 protocol support | 🔴 Major | Next-gen pool compatibility |
| — | HTTP Prometheus metrics endpoint | 🟡 Medium | Monitoring / dashboard integration |
| — | hwloc-aware thread pinning | 🟡 Medium | Topology-aware placement |
| — | Newton-Raphson FDIV/FSQRT (O12) | ⏸️ Frozen | Failed once (segfault). Do not retry without KAT proof and XMRig source study. |

### Maintenance

| # | Item | Effort | Notes |
|---|------|--------|-------|
| — | Cross-compile CI (GitHub Actions + qemu-user) | 🟡 Medium | Optional — you test on real hardware |
| — | Phase-2 test suite expansion | 🟡 Medium | Property tests, malformed JSON, etc. |
| — | S7: `emit32` UB fix (use memcpy like `emit64`) | 🟢 Low | `jit_compiler_a64.hpp:84` |

---

## Recommendation

You're testing on real hardware, so CI is optional. For maximum performance:

1. **Branchless CBRANCH** (+1–2%, low risk once encoding is correct)
2. **Peephole JIT coalescing** (+15–20%, the real gap-closer)
3. Profile-guided optimization (PGO) via `-DARMRX_PGO=GENERATE/USE`

The `docs/branchless-cbranch.md` file has all the research done — fixing the encoding
is the most efficient next step. After that, Phase 3 peephole work is the only path
to matching XMRig's instruction count.
