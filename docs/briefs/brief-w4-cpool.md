# Brief: W4 — Re-enable dense NEON literal-pool for superscalar C* immediates

**Author:** Hermes (measurement + device A/B). **Coder:** Reasonix (this change only).
**Date:** 2026-08-01. **Classification:** experimental/hard codegen change (user-approved pivot).
**Clean-room:** technique learned from BSD upstream RandomX reference at
`scratch_vm_study/upstream_rx/src/jit_compiler_a64.cpp` (readable; reimplement the *idea*,
do NOT copy GPL XMRig). The reference achieves 104.8M instr/hash @ IPC 1.54 on this exact
AArch64/Cortex-A53 hardware; armrx is 132.7M @ 1.369 — the gap is the C* immediate materialization.

---

## Root cause (measured)

armrx's `generateSuperscalarHash` pins `num32bitLiterals = 64` (src/jit_compiler_a64.cpp:1105)
with an explicit comment stating this *forbids* `IADD_C*`/`IXOR_C*` from using the shared NEON
literal pool, forcing every C* op through the MOVZ+MOVK fallback (3 instructions each).

The shared pool path already exists and is correct: `emitMovImmediate`
(src/jit_compiler_a64.cpp:1240-1265) — when `num32bitLiterals < 64` — emits a single
`UMOV`/`SMOV` from a fixed vector lane `vN.s[slot]` and writes the 32-bit immediate into the
dense contiguous region `ImulRcpLiteralsEnd[num32bitLiterals]` (line 1264). This is
**order-independent** (the constant is addressed by a fixed slot index written once at emit
time, not by PC-relative position), so it is safe under `scheduleSuperscalarProgram` reordering.

The BSD reference does the opposite: its `generateSuperscalarHash` resets `num32bitLiterals = 0`
(reference src line 324) and lets C* ops pool into the same 64-slot dense region. Result:
104.8M instr/hash @ IPC 1.54. armrx's pin is the reason it is +26.6% instructions.

This is NOT the W3 failed approach (W3 used PC-relative scattered `LDR`, which is
order-sensitive AND cache-thrashing). This is the reference's dense fixed-index vector pool,
which is both cache-friendly (one 256-byte block) and order-safe.

---

## Exact change (ONE line, plus verification)

**File:** `src/jit_compiler_a64.cpp`
**Line ~1105** (inside `JitCompilerA64::generateSuperscalarHash`, the `num32bitLiterals = 64;`
that is described by the multi-line comment about keeping C* out of the pool):

```cpp
-    num32bitLiterals = 64;
+    num32bitLiterals = 0;   // W4: re-enable dense NEON literal-pool for C* immediates
+                           // (matches BSD reference; fixed-index vector lanes are order-independent,
+                           // safe under scheduleSuperscalarProgram reordering)
```

That is the ONLY source edit for phase 1. Do NOT touch `emitMovImmediate` itself, the IMUL_RCP
literal pool (`literal_pos` / jumped-over pool), or `scheduleSuperscalarProgram`.

### Why this is safe (scheduler)
- The pooled constant is loaded via `UMOV dst, vN.s[slot]` where `slot` is fixed at emit time.
  Reordering the C* op within the emitted program does NOT change which constant it reads. The
  literal region `ImulRcpLiteralsEnd[0..63]` is written once during emission, never by the
  generated code. This is the same property the reference relies on.
- `ImulRcpLiteralsEnd` is a distinct region from the IMUL_RCP jumped-over pool (`literal_pos`),
  so C* pooling does not collide with reciprocal literals.

### Correctness gate (Hermes runs on device — AArch64 cross-build)
After this edit, the following MUST all pass (Hermes drives; do not skip):
1. `test_jit_equivalence` — 16 pairs byte-identical (JIT == interpreter).
2. `test_jit_dataset_2way` — 2-way dataset derivation bit-identical.
3. `test_jit_determinism` — deterministic across runs.
4. `test_jit_scheduler_stress` (450 pairs) — MUST pass (this caught the W3-2 memory-op divergence).
5. `test_jit_superscalar_scheduler_stress` (200 pairs) — MUST pass (this is the path that actually
   reorders the superscalar body; it is the critical regression guard for this change).

### Perf gate (Hermes runs on device under `perf stat`)
- `bench_armrx --full-hash-only` under `perf stat -e instructions,cycles,cache-misses,l1d_cache_refill,l2d_cache_refill`,
  pinned to a fast core. Baseline (isolcpus config, pre-change): 132.7M instr/hash, IPC 1.369,
  cache-misses ~0.70M/hash.
- **PASS criteria:** instructions reduced (toward ~104.8M); cache-misses NOT tripled (the W3
  failure mode was 109M→339M). If cache-misses triple OR throughput regresses, REVERT.
- NOTE: device no longer runs isolcpus (user disabled 2026-08-01) — non-isolated numbers are the
  new baseline; compare change vs control on the same kernel config.

---

## Phase 1 result (2026-08-01): FAILED correctness — REVERTED

Dispatched to Reasonix; the one-line change applied and compiled. Device A/B (Hermes):
`test_jit_equivalence` → **FAIL** `seed="jit_equiv_seed_0" input="equivalence input 0_0"`
(JIT/interpreter mismatch). Immediate revert; equivalence re-confirmed 16/16 byte-identical,
EXIT=0. Stress tests (scheduler_stress, superscalar_scheduler_stress) were NOT reached — the
basic equivalence gate caught the divergence first.

### Root cause
`ImulRcpLiteralsEnd` is a **single shared literal region** in the code buffer, used by BOTH
`generateProgram` (main VM) and `generateSuperscalarHash` (superscalar). With the pin removed,
superscalar C* ops write their constants into `ImulRcpLiteralsEnd[0..63]` (via `emitMovImmediate`'s
`((uint32_t*)(code + ImulRcpLiteralsEnd))[num32bitLiterals] = imm` at line 1264), **overwriting**
the main VM's IMUL_RCP/immediate literals that occupy the same slots. The next main-loop
`generateProgram` read of slot 0..N then gets a superscalar constant → deterministic hash
mismatch. The original `num32bitLiterals = 64` pin existed precisely to keep superscalar C* ops
OUT of the shared `ImulRcpLiteralsEnd` region. The brief's "order-independent / safe" claim was
WRONG about the *region sharing* — fixed-index lanes are order-independent within a program, but
the region is SHARED ACROSS programs (main VM + superscalar), which is the actual hazard.

### What the reference does differently
The BSD reference's `generateProgram` and `generateSuperscalarHash` each manage their own literal
region (reference buffer layout separates them). armrx's buffer was wired so both emit into the
same `ImulRcpLiteralsEnd`. So a naive port of the reference's `num32bitLiterals = 0` is unsafe in
armrx's layout.

### Path forward (NOT a one-liner — phase 2-class)
To use the dense pool safely, allocate a **dedicated superscalar C* literal region** distinct from
`ImulRcpLiteralsEnd`:
1. Reserve a new contiguous block in `jit_compiler_a64_static.S` (e.g. `superscalar_cpool_end`
   marker) sized for N slots (start 64, extensible to 256/512).
2. Add a separate counter (e.g. `ssNumLiterals`) reset at `generateSuperscalarHash` entry, writing
   into the new region. `emitMovImmediate` for superscalar C* must target the new region/base.
3. Load the pool constants into dedicated NEON registers at the superscalar prologue (reserve
   v8–v15 or similar, preserving ABI), referenced by fixed lane index.
4. Re-run the full gate chain (equivalence → d2w → determinism → scheduler_stress →
   superscalar_scheduler_stress) + perf A/B.
This is a real codegen change (separate region + register allocation + static.S reservation) —
needs a fresh, careful brief. The W3-era `ARMRX_MAX_SWAPS` bisect instrument + stress tests
remain the safety net.

### Status
W4 phase-1 (un-pin) = **closed, unsafe, reverted**. Dense-pool technique is valid (reference
proves 104.8M/1.54 on this silicon) but requires dedicated region allocation in armrx — deferred
to a phase-2 brief. Tree restored to baseline (equivalence 16/16).

---

## Revert trigger
If ANY of: (a) a stress test fails, (b) cache-misses triple, (c) instructions do not decrease,
or (d) throughput regresses — revert `num32bitLiterals = 64;` and report. Do not partially apply.

## Deliverable from coder
- Apply the one-line edit. Confirm it compiles under the cross toolchain
  (`cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake -DARMRX_DISABLE_LTO=ON && cmake --build build-cross -j$(nproc) --target armrx`).
- Do NOT commit (Hermes owns the device verification + commit). Report the diff and build status.
- Do NOT modify `scratch_vm_study/` (nested repo, reference only).
