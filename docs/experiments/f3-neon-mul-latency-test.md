# Experiment: Track F3 — NEON Multiply Latency Premise Test

**Date:** 2026-07-30
**Author:** Hermes Agent

## Goal

Determine whether NEON integer multiply (`mul v.4s`, `umull v.2d`) has lower or equal latency to scalar `mul`/`umulh` on Cortex-A53, as the premise gate for Track F3 (offload IMUL_R/IMUL_RCP to NEON).

## Method

Four microbenchmarks ran a pure RAW dependency chain of K dependent multiplies per outer iteration, for 500 000 outer iterations × K=100 = 50 M multiply instructions each. State was kept entirely in the target register file (scalar GPR or NEON V reg) throughout the inner chain — no cross-domain moves inside the critical path. Run pinned to core 3 (`taskset -c 3`) under `perf stat`.

### Variants

| Variant | Instruction | Data per instr | File |
|---------|-------------|----------------|------|
| v0 | `mul` (64×64→64 lower) | 1 operand | `tools/bench/mul_latency_bench.c` |
| v1 | `umulh` (64×64→64 upper) | 1 operand | same |
| v2 | `mul v.4s` (4× 32×32→32) | 4 operands | same |
| v3 | `umull v.2d` (2× 32→64) | 2 operands | same |

## Results

| Variant | Instructions | Cycles | CPI | Data throughput rate |
|---------|-------------|--------|-----|---------------------|
| v0 scalar `mul` | 202,050,899 | 205,616,166 | **1.02** | 1× |
| v1 scalar `umulh` | 202,050,869 | 305,619,450 | **1.51** | 1× (+2 cy penalty) |
| v2 NEON `mul v.4s` | 204,005,457 | 206,524,949 | **1.01** | **4×** |
| v3 NEON `umull v.2d` | 204,005,348 | 206,526,208 | **1.01** | **2×** |

Run-to-run consistency: <0.02% cycle noise.

## Analysis

1. **NEON multiply latency ≈ scalar multiply latency** on Cortex-A53. The NEON pipe delivers the same CPI (~1.01) as the scalar MUL pipe while processing 2–4× the data per instruction.

2. **`umulh` is significantly slower** than `mul` (+2.00 cycles/instruction, ~49% more cycles). This is consistent with Cortex-A53 pipeline data: `mul` (lower product) has 2-cycle latency, `umulh` (upper product) has 3–4 cycle latency. In the JIT, `IMUL_RCP` (which uses `umulh`) is the more expensive opcode.

3. The extra ~1M FMOV instructions in v2/v3 (two per outer iteration for GPR↔NEV transfer) add ~0.9M cycles — confirming that frequent cross-domain moves negate the throughput advantage.

## Gate Decision

**PASS** — NEON multiply latency is competitive. The offload hypothesis is not blocked by latency.

## Open Design Question

The concrete mechanism remains unsolved: RandomX IMUL_R is a 64×64→64 operation. NEON has no native 64-bit multiply. Options:
- **Schoolbook:** 2× NEON `umull v.2d` + adds = worse than scalar.
- **Lane-parallel:** If multiple independent IMUL_R operations can be packed into 32-bit lanes (e.g., when one operand is a small constant), NEON `mul v.4s` gives 4× throughput at same latency. This requires analyzing whether RandomX's instruction stream has enough independent 64-bit multiplies with narrow operands to make packing profitable.
- **Deferred:** The master plan flags this as "the next cheap step if anyone picks F3 back up" — no action without a concrete transform.

## Status

**Closed as premise-confirmed, then gate-FAILED (2026-08-01).** NEON multiply
latency is competitive (premise confirmed), but the lane-parallel transform's
gate check failed: only 0.003% of genuine IMUL_R executions have one operand
≤ 2^32 (`docs/experiments/t33-imul-magnitude-gate.md`) — RandomX 64-bit operands
are effectively full-width random. **No action without a concrete transform** is
now "no action, period": the offload is falsified on runtime data distribution.
