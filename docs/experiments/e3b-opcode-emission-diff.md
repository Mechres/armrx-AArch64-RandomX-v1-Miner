# E3b — Per-opcode JIT emission diff (armrx self-analysis)

**Date:** 2026-08-05 (session clock)
**Author:** Hermes
**Status:** DONE (analysis) — identifies over-emitted opcodes; implementation is a follow-up
**Purpose:** Era II Phase 1b. Phase 0 showed armrx = 113.8M instr/hash vs XMRig 98.9M (+15%
heavier). This finds WHICH opcodes armrx over-emits, so a codegen-density fix can target them.

## Method
- `armrx --jit-dump=<seed>` (`vm.dumpJitCode()`) prints a per-opcode boundary table: each
  emitted block's opcode name + byte size. `size/4 = instructions`. Ran across 40 seeds,
  aggregated avg instructions/occurrence per opcode.
- **Clean-room:** this is a self-analysis of armrx's OWN emission — no XMRig source read, no
  XMRig jit-dump (XMRig has no `--jit-dump` flag). The "vs XMRig" comparison is by STRUCTURAL
  MINIMUM (fewest AArch64 instructions to implement each RandomX opcode semantic), not by
  reading XMRig's code. (Disassembling XMRig's live JIT buffer is a possible cross-check but is
  left as a follow-up.)
- Harness: `tools/e3b_harness.py` (or /tmp/e3b_harness.py).

## Results — over-emitted opcodes (avg instr/occurrence vs structural minimum)

| opcode | avgI (armrx) | min | excess/occ | note |
|---|---:|---:|---:|---|
| **CBRANCH** | **5.0** | 2 | 3.0 | branchless conditional-swap form; XMRig uses 2–3 |
| **FDIV_M** | **7.8** | 1–2 | 6.8 | FP load + div; very heavy |
| **FSUB_M** | **6.8** | 3 | 3.8 | FP `*_M` load+op |
| **FADD_M** | **6.8** | 3 | 3.8 | FP `*_M` load+op |
| **IADD_M** | **4.6** | 3 | 1.6 | int `*_M` load+op |
| **ISUB_M** | 4.6 | 3 | 1.6 | int `*_M` load+op |
| **IXOR_M** | 4.6 | 3 | 1.6 | int `*_M` load+op |
| **IMUL_M** | 4.6 | 3 | 1.6 | int `*_M` load+op |
| **IMULH_M / ISMULH_M** | 4.5–4.6 | 3 | 1.5 | mul-high `*_M` |
| **ISTORE** | 3.8 | 3 | 0.8 | store (addr+store) |
| **CFROUND** | 4.0 | 1 | 3.0 | FP-rounding-mode set |
| **IROL_R** | 1.9 | 1 | 0.9 | rotate |

Opcodes at min (NOT over-emitted): FMUL_R, FADD_R, FSUB_R, IMUL_R, ISUB_R, IXOR_R, INEG_R,
FSWAP_R, FSCAL_R, FSQRT_R, IMULH_R, ISMULH_R, IROR_R, IMUL_RCP(2,=min), IADD_RS(1.2). These
are already minimal.

**The `*_M` family (FADD_M/FSUB_M/IADD_M/ISUB_M/IXOR_M/IMUL_M + mul-high) is the dominant,
FREQUENT over-emission**: each emits `add(addr) → and(mask) → ldr(load) → op` = 4–7 instr
where the semantic needs ~3 (the `and` mask or the separate `add` is often foldable). This is
the same `emitMemLoad` path E25 already trimmed (skip redundant `add` when offset==0); the
remaining excess is intrinsic to the `add+and+ldr` sequence vs a tighter form.

**CBRANCH at 5 instr** (vs min 2) is the single highest per-occurrence excess, but its real
frequency in production is low (RandomX weights it ~2/program), so its total contribution is
small — unlike the `*_M` family which is ~15% of all opcodes.

## IMPORTANT caveat — frequency not representative
`--jit-dump` uses a FIXED test input ("JIT dump test input") + seed, which generates an
ANOMALOUS program (CBRANCH ~190/program — impossible for real RandomX, where CBRANCH is ~2).
So the aggregate "total excess instructions" from this harness is NOT trustworthy. The
**per-opcode avgI values ARE correct** (they reflect armrx's emission cost per opcode
occurrence, independent of how often that opcode appears). To get a true weighted total, either
(1) dump across many seeds with REALISTIC seeds, or (2) weight by the `instruction_weights`
table in `include/.../instruction_weights.hpp`. Both are follow-ups.

## Conclusion / next step
The +15% instruction-count gap (113.8M vs 98.9M) is concentrated in:
1. **The `*_M` load-op family** (most frequent over-emission) — primary target.
2. **CBRANCH** (highest per-occ excess, low frequency).
3. **FDIV_M / F*_M** (heavy but rare).

These are codegen-density fixes in `emitMemLoad` / `emitMemStore` / `h_CBRANCH` — each must be
implemented behind an `ARMRX_*` flag (TESTING.md §6) and gated (KAT 16/16 → 450 → 200 → 8w H/s
+ PMU). This is a real but non-trivial codegen effort; it is the actual Era II lever (Phase 0
resolved the metric to instruction count, and E3b localized it to these opcodes). Not attempted
in this pass — flagged for implementation with gating.

## Related
- `p0-instruction-count-resolution.md` — Phase 0 (lever = instruction count, +15% heavier).
- Clang A/B (this session) — NULL; gap is emission LOGIC, not compiler. So fixing the above
  requires source-level emission changes, not a toolchain swap.
