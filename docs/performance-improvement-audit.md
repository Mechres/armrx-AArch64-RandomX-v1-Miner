# Performance Improvement Audit — armrx (AArch64 RandomX Miner)

**Date:** 2026-07-23
**Audited by:** Hermes Agent (code review pass)
**Scope:** Hot-path review for performance improvement opportunities.
**Constraint:** Audit host is x86_64; the AArch64 JIT (98% of hash time) cannot
execute off-device. JIT-only claims must be measured on the dev box
(192.168.10.156). The software-AES path runs everywhere and was benchmarked
directly on the audit host.

---

## TL;DR

The codebase is already heavily optimized (LTO/IPO, PGO plumbing, huge pages,
NEON Argon2, instruction scheduling, big.LITTLE pinning). The single biggest
remaining lever is **already implemented but not shipped**: PGO is plumbed into
CMake but the automated devbox build does not use it, even though it is worth
**+19.3% single-thread hashrate** (4.34 → 5.18 H/s). The top *code* change not
yet tried is **NEON-vectorizing the software T-table AES** (the dominant
init/finalize cost) — distinct from the hardware `AESE`/`AESD` paths that were
correctly rejected.

---

## Headline Finding — PGO is built but not shipped

PGO is plumbed into CMake via `-DARMRX_PGO=GENERATE|USE`
(`CMakeLists.txt:26,118-128`), and the changelog shows it is worth
**+19.3%** single-thread (4.34 → 5.18 H/s) and **+15%** at 8 threads
(25.28 H/s, NEXT_STEPS.md telemetry table).

But the automated build does NOT use it:

- `tools/devbox/devbox_mcp.py:56`
  ```python
  "build_flags": ["-DARMRX_ENABLE_NATIVE=ON", "-DARMRX_BUILD_TESTS=ON"],
  ```
- `README.md:21` advertises "PGO Enabled"
- `README.md:39` lists "Light mode JIT | 1 | 5.18 H/s" — the PGO number

The README advertises 5.18 H/s (the PGO result) while `devbox_build` produces
the 4.34 H/s non-PGO binary. The two-stage generate → train → use cycle is
never wired into the devbox flow, so the advertised number is not what a
default build yields.

**Verified end-to-end on the audit host (GCC 16.1):**
PGO `GENERATE` built clean, trained via `armrx --help`/`--jit-dump`, and PGO
`USE` linked cleanly (`-fprofile-generate`/`use` confirmed in compile + link
commands, `.gcda` profile data emitted). The mechanism works; only the build
wiring is missing.

**Fix:** Pure build wiring — add a `devbox_pgo_build` flow (generate on-device
with a representative workload, e.g. `armrx --mine --seconds=30`, then `USE`),
or at minimum document the two-stage release build. Zero code change, ~+19%
hashrate.

**Risk:** None to correctness (rebuild only); KATs already gate the flow.

---

## Tier 2 — Highest-value on-device experiment not yet tried

### NEON-vectorize the software T-table AES (dominant init/finalize cost)

The repo tried the **hardware** `AESE`/`AESD` instructions and dismissed all
NEON AES as "zero benefit" (changelogs.md, 2026-07-20). That was the hardware
instruction only (wrong round-ordering vs the RandomX AES spec). It never tried
`vtbl`/`vqtbl1q`-based vectorization of the **software** T-table path — the
standard technique (process 4 columns × 16 bytes via 16-byte table lookups, the
mbedTLS / Android OpenSSL approach).

**Why it matters — the cost is real and measured:**

The software AES in `include/armrx/aes.hpp`
(`encrypt_transform`/`decrypt_transform`) — 16 byte-indexed table lookups + XORs
per round — is the inner loop of:
- `fill_aes_1r_x4` (`init_scratchpad`) — ~12.35% of full hash on A53
- `hash_aes_1r_x4` (`get_final_result`) — ~9.30% of full hash on A53
  (NEXT_STEPS.md telemetry table)

**Benchmarked locally on the audit host (interpreted path, identical code):**
```
fill_aes_1r_x4 (2 MiB)    515.66 μs
hash_aes_1r_x4  (2 MiB)  1159.15 μs
```
AES scratchpad work dominates init/finalize and is a regular byte-lookup
workload that Cortex-A53's NEON `vtbl`/`vqtbl1q` is specifically good at.

**Recommended shape:**
- New NEON `vtbl` path guarded by `__aarch64__` + a KAT-gated CMake flag.
- Scalar T-table path untouched (x86_64 + interpreter stay correct).
- The v2 AES mix block in `src/jit_compiler_a64_static.S:392` is a separate
  hardware-`AESE` block — keep as-is; this proposal does not touch the JIT's
  hardware path.
- Prototype + benchmark on the **interpreted path first** (KAT parity + speedup
  number) before any JIT integration.

**Risk:** Must be hashrate-vetoed on-device. Possible that per-block NEON
load/store overhead cancels the lookup speedup on A53 (this is exactly why the
earlier hardware-AES attempt was reverted) — which is why it must be measured,
not assumed.

---

## Tier 3 — Smaller / uncertain / tuning knobs

- **Peephole JIT coalescing** (`ROADMAP.md` "P3"; `docs/peephole-jit-plan.md`):
  documented −5–10%, but the plan itself says to "re-evaluate this estimate"
  because it was framed against the old 31% branch-miss baseline that has since
  been shown to NOT represent the hot path (isolated hot path is 2.4%). 3–6 week
  clean-room effort against XMRig's disassembly; high effort, uncertain payoff.
  Only after Tier 1 / Tier 2 land.

- **`--stagger-ms` default** (`src/mining_engine.cpp:313`): exists but defaults
  to 0. The worker sweep (changelogs.md, 2026-07-21) showed memory-bus
  contention drops little-core efficiency under 8-worker saturation; a small
  stagger desyncs the memory-heavy phases. Worth a 1-line default experiment
  on-device.

- **`ARMRX_FAST_MATH` (`-Ofast`)** and **`ARMRX_ENABLE_JIT_FAST_DIV_SQRT`**:
  both correctly OFF. `-Ofast` rewrites IEEE FP semantics and would break
  RandomX KATs. Newton-Raphson FDIV/FSQRT measured **−1.1% hashrate** (frozen,
  ROADMAP.md "Features"). Leave both OFF. Do NOT enable.

---

## Already Done — do not re-touch

Closed-out optimizations confirmed in changelogs.md / ROADMAP.md:
- Argon2 NEON G-function + diagonal-step vectorization (26.8% fewer
  instructions, 19.0% fewer cycles for cache init).
- Huge-page tiers (dataset `MAP_HUGETLB`, cache, 2 MiB scratchpad +
  `MADV_POPULATE_WRITE`).
- Template-copy elimination from per-hash path; Superscalar heap-churn kill.
- Worker-thread dataset reuse for dataset init (barrier handshake).
- big.LITTLE-aware scheduling + hwloc CPU pinning.
- Prefetch hint tuning; interleaved JIT FP loads; register-offset FP loads.
- LTO/IPO; rounding-mode cache; alignas(16) RegisterFile.
- CBRANCH: **closed** — 2.4% hot-path branch-miss rate, not a real lever
  (the 31.08% aggregate figure came from a non-representative benchmark
  section).
- NEON `AESE`/`AESD` hardware AES: correctly rejected (incompatible round
  order vs RandomX spec).

---

## Verification evidence (audit host, x86_64)

- `cmake -S . -B build -DARMRX_BUILD_TESTS=ON` + `cmake --build build -j`:
  clean (LTO/IPO enabled, hwloc found).
- `./build/bench_armrx --micro-only`: AES fill 515 μs, AES hash 1159 μs
  (confirms init/finalize software-AES cost).
- `./build/armrx_tests`: KAT suite green (both interpreted inputs).
- PGO GENERATE → train → USE: links cleanly, `-fprofile-generate`/`use`
  present in compile and link commands, `.gcda` emitted.

## What was NOT verified here (requires the AArch64 dev box)

- Any JIT-execution hashrate claim (JIT excluded on x86_64).
- On-device PGO hashrate delta (the +19.3% is from changelogs.md telemetry,
  reproduced on the A53).
- NEON software-AES speedup on Cortex-A53 (must be benchmarked on-device).

---

## Suggested next actions (for the next engineer)

1. **PGO into devbox release flow** — CMake + `devbox_mcp.py` two-stage
   generate/train/use, plus a one-line README clarification that 5.18 H/s is
   the PGO build. ~+19% hashrate, zero code risk.
2. **Prototype NEON `vtbl` software-AES** in `include/armrx/aes.hpp` behind a
   KAT-gated flag; benchmark the interpreted path here for parity + speedup,
   then gate on-device hashrate before JIT integration.
3. (Optional) `devbox_pgo_build` + a `--stagger-ms` default experiment.
