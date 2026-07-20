# Next Steps Plan

**Generated:** 2026-07-20  
**HEAD:** fba761e  
**Devbox:** Reachable (flaky)  

## Post-AES-fix Baseline (2026-07-20, Cortex-A53 8-core)
- **4.33 H/s** (230,995 μs/hash) — down 16.4% from pre-fix 5.18 H/s
- Root cause: **correct AES → correct program entropy**. Old number was buggy.
- 33% instruction gap vs XMRig is **now stale** — needs fresh measurement
- First bare run at 2.17 H/s suggests thermal throttling during cold starts

---

## Context

The AES T-table hash divergence has been fixed and committed (5 commits:
NEON removal → decrypt fix → encrypt+decrypt fix → docs/ROADMAP → postmortem).
33/33 tests pass. The devbox (192.168.10.156) is offline — all on-device work
(benchmarks, PMU counters) is blocked.

---

## Phase A — Local Cleanup (can do NOW)

| # | Item | Files | Effort | Priority |
|---|------|-------|--------|----------|
| A1 | **Stash debug traces in `scratch_vm_study/upstream_rx`** — 3 source files modified during AES debugging add `std::cout` tracing. These are local debugging artifacts, not intended to stay. Stash them to clean the dirty submodule. | `scratch_vm_study/upstream_rx/src/bytecode_machine.hpp`, `randomx.cpp`, `vm_interpreted.cpp` | 🟢 1 min | High |
| A2 | **Consolidate duplicate plan files** — `PLAN.md` (100 lines, clean master plan) and `plan.md` (261 lines, older detailed analysis) overlap. Move any unique content from `plan.md` into `PLAN.md` and remove `plan.md`. | `PLAN.md`, `plan.md` | 🟡 5 min | High |
| A3 | **Remove stale NEON header include** — `aes_hash.cpp:7-10` still includes `<arm_neon.h>` under `#if defined(__aarch64__)`, but no NEON intrinsics remain. Remove the dead guard + include. | `src/aes_hash.cpp` | 🟢 1 min | Medium |
| A4 | **Update `changelogs.md`** — The AES fix chain (commits `5ac93e8` → `fba761e`) is partially documented. Ensure the final status (all 3 bugs fixed, KATs verified on both arches) has a dated entry. | `changelogs.md` | 🟢 2 min | Medium |
| A5 | **Update `AGENTS.md`** — Add devbox MCP commands (`devbox_sync`, `devbox_build`, `devbox_test`, `devbox_bench`, `devbox_perf_stat`) to the commands table. Update AES/NEON gotchas. | `AGENTS.md` | 🟢 3 min | Medium |
| A6 | **Regenerate `STATUS_REPORT.md`** — After A1–A5, regenerate to reflect the cleaned state. Add a note that the 33% instruction gap was measured pre-AES-fix and needs re-baseline. | `STATUS_REPORT.md` | 🟢 2 min | Medium |

---

## Phase B — Devbox-Dependent (blocked until device reachable)

| # | Item | Tool | Est. effort | Rationale |
|---|------|------|-------------|-----------|
| B1 | **Re-baseline done** — 4.33 H/s (down from 5.18). Old 33% gap stale. | — | ✅ Done |
| B2 | **Measure NEON AES removal impact** — estimate ≤1.2% of 16.4% regression; rest is from correct program entropy | `devbox_bench` | ~20 min |

## Phase C — Re-prioritized after baseline

The **4.33 H/s is the honest baseline**. The old 5.18 H/s was from buggy code. Optimization targets should use this number — any improvement from here is real.

| # | Item | Rationale | Est. gain |
|---|------|-----------|-----------|
| 1 | **XMRig A/B comparison** — run XMRig on same device at same load to get fresh gap | Old 33% gap is stale | Baseline |
| 2 | **CBRANCH misprediction cost reduction** | Likely still the largest bottleneck | +5–15% |
| 3 | **Instruction scheduling for A53** | IPC 0.786 vs peak 2.0 | +5–10% |
| 4 | **Peephole JIT coalescing** | Per-opcode | +3–7% |
| B3 | **Fix CTest executable path for 4 unrun tests** — `bench_armrx`, `bench_opcodes`, `test_jit_encodings`, `test_jit_determinism` are "Not Run" by CTest because the binary path is wrong. Diagnose and fix `CMakeLists.txt`. | `CMakeLists.txt` + `devbox_build` + `devbox_test` | ~5 min | These tests exist but are invisible to CTest. |
| B4 | **CBRANCH misprediction cost reduction** — 34.42% branch miss rate, ~26% cycles wasted. CSEL/CINC evaluation, balanced path costs. | `jit_compiler_a64.cpp` + `devbox_perf_stat` | Days | Largest remaining bottleneck. Start after B1 baseline. |
| B5 | **Instruction scheduling for in-order A53** — IPC of 0.708 vs theoretical peak 2.0. Static FP load scheduling, register-offset FP loads. | `jit_compiler_a64_static.S`, `emitMemLoadFP()` | Days | After B4 or in parallel. |

---

## Phase C — Documentation / Structural Items

| # | Item | Effort | When |
|---|------|--------|------|
| C1 | **Reintroduce correct NEON AES encrypt paths** — AESE+AESMC was verified correct for encrypt ops. Add conditional guard to use NEON for encrypt-only, software T-table for decrypt. | 🟡 ~30 min | After B2 confirms the impact |
| C2 | **Cross-compile CI** — GitHub Actions + qemu-user. Not urgent (test on real hardware). | 🟡 opt-in | Optional |
| C3 | **Squash AES fix commits** before any public release — 7 commits (3 fixes + 3 docs + 1 postmortem) could be 1–2. | 🟢 2 min | Before public release |

---

## Immediate Action (this turn)

1. **A1** → Stash submodule debug traces
2. **A2** → Consolidate plan files
3. **A3** → Remove stale NEON include
4. **A4** → Update changelogs.md
5. **A5** → Update AGENTS.md  
6. **A6** → Regenerate STATUS_REPORT.md
