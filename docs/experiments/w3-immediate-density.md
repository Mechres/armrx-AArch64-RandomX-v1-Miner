# W3 — Superscalar Immediate-Materialization Density (CLOSED: measured regression)

**Date:** 2026-08-01. **Author:** Hermes (design + device A/B + verdict). **Implementer:** Cursor
(`agent -p --force`) for the source edits; Hermes cross-compiled, md5-verified, ran the
correctness gate and the perf A/B, and reverted.

## Goal

The W2-3 diagnosis (`docs/experiments/w23-opcode-cost.md` §4–5) identified the only evidence-backed
optimization lever left: the ~10% instruction-count excess in the superscalar dataset-derivation region
comes from `IADD_C7/8/9` / `IXOR_C7/8/9` large-immediate materialization (MOVZ/MOVN + MOVK + ALU,
~3 A64 each). Proposal: replace the 2-instr MOVZ/MOVK (+1 ALU) with a single `LDR` from a per-program,
**per-original-index** literal pool (~714 C* ops/hash × 1 ≈ 714 A64 ≈ ~10% of the ~7,100-instr
superscalar body ≈ ~6 M instr/hash out of ~119 M total).

## Design (scheduler-safe by construction)

`scheduleSuperscalarProgram()` returns **original program indices** `j`, so each emitted instruction
knows its original index. A literal pool laid out 1 slot per original index `j` is addressed
**order-independently** (`LDR` from `pool_base + j*stride`), strictly safer than the existing `IMUL_RCP`
sequential-pointer pool (which still needs the scheduler's `is_imul_rcp` swap-exclusion). This also
let us remove the fragile `num32bitLiterals = 64` scheduler-safety pin.

Two emitters changed (live 1-way `generateSuperscalarHash` in `jit_compiler_a64.cpp`, and the
not-in-production 2-way mirror `emitSuperscalarInstr`/`generate()` in `jit_dataset_2way.cpp`).

## Bug 1 — register width (found by device equivalence failure)

First implementation used a 32-bit `LDR Wt` (`0x18000000`) to match the 4-byte pool slots. This
**zero-extends** the upper 32 bits. The reference `execute_superscalar` (`src/superscalar.cpp:746/751`)
uses `signExtend2sCompl(instr.getImm32())` — the 32-bit immediate is **sign-extended to 64 bits** for
`IADD_C*`/`IXOR_C*`. The original MOVZ/MOVN+MOVK path also sign-extended. So 32-bit-`LDR` loads were
wrong for any immediate with bit 31 set (≥ 0x80000000). → `test_jit_equivalence` failed seed_0.

## Bug 2 — offset/stride (the decisive root cause)

Corrected to mirror `IMUL_RCP` exactly: store the **sign-extended 64-bit** value via `emit64` (8-byte
slots, stride 8), load with the **64-bit** `LDR Xt` (`LDR_LITERAL` = `0x58000000`, already proven
correct for IMUL_RCP), offset `off = (pool_base + j*8 - codePos)/4; off &= (1<<19)-1;` — identical to
IMUL_RCP's formula. After this, **all three correctness tests passed on device**:

- `test_jit_equivalence`: 16/16 seed/input pairs byte-identical ✓
- `test_jit_dataset_2way`: 40,020 derivations across 20 seeds, all byte-identical to reference ✓
- `test_jit_determinism`: deterministic (hash + JIT output match) ✓

## Perf A/B — the decisive gate (net regression → reverted)

Method: cross-built `armrx` (musl GCC 16.1.0, `ARMRX_DISABLE_LTO=ON`), md5-verified, `scp` to device
(`/tmp/cross`). Single worker pinned to fast-cluster core 3 (`taskset -c 3`, to avoid the weak
cores 4–7 per AGENTS.md), `perf stat -e instructions,cycles,cache-misses` around
`--mine --workers=1 --warmup=10 --seconds=30`. Baseline = `c5ac985`; Modified = W3. Two independent
runs each (baseline was reproducible to the digit).

| Metric | Baseline (c5ac985) | Modified (W3) | Δ |
|---|---:|---:|---:|
| H/s (1 worker, core 3) | 4.49 / 4.49 | 3.59 / 3.79 | **−16% to −20%** |
| Instructions (30 s window) | 20,654M / 20,654M | 16,385M / 16,505M | **−20%** |
| Cycles | 29.1B / 29.2B | 29.3B / 29.2B | ~flat |
| Cache-misses | 109M / 109M | 339M / 322M | **+196%** |

**Verdict: measured regression. NOT shipped.** The change cuts ~20% of instructions (real) but
**triples cache misses** and collapses IPC (0.71 → 0.56), netting **−16 to −20% H/s**.

### Mechanism

Replacing register-only `MOVZ`/`MOVN`+`MOVK`+`ALU` with `LDR`-from-pool trades pure-register ops for
memory loads. `IMUL_RCP` already uses this exact pattern and is harmless because it has only ~239
loads/program. The C* change adds ~714 loads/program — **~3× the density** — scattered across the
per-program pool region (slots addressed by original index, so after scheduler reordering the loads are
at varying distances, touching many distinct cache lines that **compete with the I-cache for the code
itself**). On this in-order-ish Cortex-A53 (MSM8929, 765 MHz), the memory-load stall cost exceeds the
instruction-count saving. This is the **fourth** independent confirmation in this repo that "fewer
instructions ≠ faster" on this microarchitecture (after T2-2 dual-issue alignment, T2-1 PRFM hints,
and the 2026-07-24 fill-loop-hints removal — all measured regressions from adding instructions/loads).

## Decision

- Source **reverted bit-clean** (`git checkout src/jit_compiler_a64.cpp src/jit_dataset_2way.cpp`);
  `git` tree restored to `c5ac985` (only the brief doc remains untracked).
- The instruction-count analysis in `w23-opcode-cost.md` §4–5 remains **correct** — the lever is real,
  just not realizable as a *throughput* win on this hardware via literal-pool loads. Do NOT re-attempt
  the literal-pool approach; the MOVZ/MOVN+MOVK materialization is the right choice here.
- The `num32bitLiterals = 64` pin stays (its scheduler-safety role is unchanged).
- W3 closed. No remaining "ready/diagnostic" backlog items; the only W3-x implementation tracks
  (W3-1 Track C reopen, W3-2 address hoisting, W3-3 no-ABI trampoline) remain gated/blocked as before.

## Artifacts

- This doc (`docs/experiments/w3-immediate-density.md`)
- Dispatch brief (`docs/experiments/w3-immediate-density-brief.md`)
- Changelog entry 2026-08-01 (W3 closed, negative)
- README/ROADMAP: no feature row added (not shipped); W3 noted as closed-negative in the diagnostic
  backlog sweep.
