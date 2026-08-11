# Brief: W4 phase-2 — dedicated superscalar C* literal pool (separate region + v16–v31)

**Author:** Hermes (measurement + device A/B). **Coder:** Reasonix (this change only).
**Date:** 2026-08-01. **Classification:** experimental/hard codegen change (user-approved pivot).
**Clean-room:** technique learned from BSD upstream RandomX reference (`scratch_vm_study/upstream_rx`),
readable; reimplement the *idea*, do NOT copy GPL XMRig. Reference achieves 104.8M instr/hash @
IPC 1.54 on this exact AArch64/Cortex-A53; armrx is 132.7M @ 1.369. The gap is C* immediate
materialization; the dense-pool technique is proven viable on this silicon.

---

## Why phase-1 failed (must not repeat)
Phase-1 un-pinned `num32bitLiterals` in `generateSuperscalarHash` (src/jit_compiler_a64.cpp:1105),
letting superscalar C* ops write into `ImulRcpLiteralsEnd` — the **shared** 64-slot region
(static.S:322-339, v0–v15) used by BOTH the main-VM `generateProgram` AND the superscalar
`generateSuperscalarHash`. Superscalar writes overwrote main-VM literals → `test_jit_equivalence`
FAIL seed_0. REVERTED (commit `d978d05`).

**Fix:** give the superscalar body its OWN pool region and its OWN NEON registers, leaving the
main VM's `ImulRcpLiteralsEnd`/v0–v15 entirely untouched. No shared state → no collision.

---

## Key facts (verified in tree)
- `ImulRcpLiteralsEnd = ((uint8_t*)randomx_program_aarch64_imul_rcp_literals_end) - base`
  (jit_compiler_a64.cpp:118). Region = 256 bytes = 64 slots = 16 vectors `literal_v0..v15`
  (static.S:322-339), each `.fill 2,8,0` (16 bytes = 4×32-bit). Loaded at main-VM prologue via
  `ldr qN, literal_vN` (e.g. static.S:221 `ldr q15, literal_v15`).
- `emitMovImmediate` (jit_compiler_a64.cpp:1240-1265): for `imm >= 2^16` and `num32bitLiterals < 64`,
  emits `UMOV`/`SMOV dst, vN.s[M]` (N=`num32bitLiterals/4`, M=`num32bitLiterals%4`) and writes
  `((uint32_t*)(code+ImulRcpLiteralsEnd))[num32bitLiterals] = imm`, then `++num32bitLiterals`.
  Else (slot ≥ 64 or imm<2^16) falls back to MOVZ/MOVN+MOVK.
- `num32bitLiterals` is a member (init 0, jit_compiler_a64.cpp:147). `generateSuperscalarHash`
  pins it to 64 (line 1105) — THIS is what forbids superscalar C* pooling.
- The superscalar body (`randomx_calc_dataset_item_aarch64`) is integer-only. Main-VM program uses
  v16–v31 (floats/masks, static.S:115-130, 262-433). The superscalar/dataset-item template does
  NOT use v16–v31 for float work (verify in step 0). So **v16–v31 are free for a superscalar pool.**

---

## Step 0 — VERIFY v16–v31 are free in the dataset-item template (coder must confirm)
Grep `src/jit_compiler_a64_static.S` for the `randomx_calc_dataset_item_aarch64` template body and
confirm it does not read/write v16–v31 (the earlier phase-1 divergence was about the SHARED
REGION, not registers — but a clobbered v16–v31 would still break the superscalar AES/data path).
If v16–v31 ARE used there, STOP and report; the pool must use a different register set (or a
non-NEON GPR spill with the dense-region trick). Do not guess.

## Step 1 — new static.S reservation (separate region)
In `src/jit_compiler_a64_static.S`, after `literal_v15` (line 339, inside `ImulRcpLiteralsEnd`),
add a NEW, separate reservation (do NOT enlarge `ImulRcpLiteralsEnd` — that would shift the
main-VM region):
```
        .balign 16
DECL(randomx_superscalar_cpool_end):
        ss_literal_v16: .fill 2,8,0   # 16 bytes = 4×32-bit slots, v16 lane 0..3
        ss_literal_v17: .fill 2,8,0
        ... up to ss_literal_v31     # 16 vectors × 16 bytes = 256 bytes = 64 slots
```
Export the symbol in `include/armrx/jit_compiler_a64_static.hpp` (mirror
`randomx_program_aarch64_imul_rcp_literals_end`). Start with 64 slots (matches reference); can
grow later. The `.balign 16` keeps NEON `ldr qN` happy.

## Step 2 — load the superscalar pool at the dataset-item prologue
At the start of `randomx_calc_dataset_item_aarch64` template (where the existing IMUL_RCP
`ldr qN, literal_vN` sequence lives for v0–v15), ADD the mirror for the superscalar pool:
```
        ldr q16, ss_literal_v16
        ldr q17, ss_literal_v17
        ... ldr q31, ss_literal_v31
```
This loads the superscalar C* constants into v16–v31 ONCE per `rx_calc_dataset_item` invocation.

## Step 3 — route superscalar C* to the new region
Add a member `uint32_t ssNumLiterals = 0;` and a pool-base member (or a boolean
`ssPoolActive_`) to `JitCompilerA64`. In `generateSuperscalarHash`:
- Set the pool base to `SsCpoolEnd = ((uint8_t*)randomx_superscalar_cpool_end) - base` and
  `ssPoolActive_ = true` (or pass region/reg-base explicitly to `emitMovImmediate`).
- Change `num32bitLiterals = 64;` (line 1105) → `ssNumLiterals = 0;` (use the SEPARATE counter
  so the main VM's `num32bitLiterals` is never touched).
- `emitMovImmediate` must, when emitting for the superscalar, write to `SsCpoolEnd[ssNumLiterals]`
  and emit `UMOV/SMOV dst, v(16 + ssNumLiterals/4).s[ssNumLiterals%4]`, then `++ssNumLiterals`.
  Cleanest: add an overload `emitMovImmediate(dst, imm, uint32_t pool_base, uint32_t reg_base,
  uint32_t& counter, uint8_t* code, uint32_t& codePos)` and have `generateSuperscalarHash` call it
  with `pool_base=SsCpoolEnd, reg_base=16, counter=ssNumLiterals`. Leave the default
  `emitMovImmediate(dst, imm, code, codePos)` (used by main VM) unchanged so the main path is
  byte-identical to baseline.
- `emitAddImmediate` (used by IADD_C*) calls `emitMovImmediate` internally — route it the same way
  when in superscalar context (pass the superscalar pool params through).

## Step 4 — correctness + perf gates (Hermes runs on device, AArch64 cross-build)
1. `test_jit_equivalence` — 16 pairs byte-identical (MUST pass; phase-1 failed here).
2. `test_jit_dataset_2way` — 2-way derivation bit-identical.
3. `test_jit_determinism` — deterministic.
4. `test_jit_scheduler_stress` (450 pairs) — MUST pass (caught W3-2).
5. `test_jit_superscalar_scheduler_stress` (200 pairs) — MUST pass (reorders superscalar body).
6. Perf A/B: `bench_armrx --full-hash-only` under `perf stat -e instructions,cycles,cache-misses,
   l1d_cache_refill,l2d_cache_refill`, pinned to a fast core. Baseline (reverted tree):
   132.7M instr/hash, IPC 1.369, cache-misses ~0.70M/hash. Target: instructions reduced (toward
   ~104.8M); cache-misses NOT tripled (W3's failure mode was 109M→339M). Since isolcpus is now
   OFF, compare change-vs-control on the SAME kernel config (non-isolated baseline).

---

## Revert trigger
If ANY of: (a) equivalence fails, (b) a stress test fails, (c) cache-misses triple, (d) instructions
do NOT decrease, or (e) throughput regresses — REVERT all four edits (static.S region, hpp export,
cpp emitMovImmediate overload + generateSuperscalarHash routing) and restore `num32bitLiterals = 64`
pin. Do not partially apply.

## Deliverable from coder
- Apply steps 0–3. Do NOT commit (Hermes owns device verification + commit).
- Confirm it compiles under cross toolchain:
  `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake -DARMRX_DISABLE_LTO=ON && cmake --build build-cross -j$(nproc) --target armrx`
- Do NOT modify `scratch_vm_study/` (nested repo, reference only).
- Report: (0) v16–v31-free confirmation, (1) exact diffs, (2) build status. Keep response < 250 words.

## Expected outcome
If the superscalar pool is correctly isolated (separate region + v16–v31), the ~714 C* ops pool
the first 64 distinct constants (1 instr each via UMOV vs 3 via MOVZ+MOVK), cutting instructions
toward ~104.8M and closing most of the +26.6% gap — matching the reference's proven result. This
is the active optimization lead (see ROADMAP "Next Up" #2 and brief-w4-cpool.md phase-1 root cause).
