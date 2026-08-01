# W3-register-hoist — considered and REJECTED (infeasible for random immediates)

**Date:** 2026-08-01. **Context:** W1-4 re-baseline (commit `ca9d868`) showed armrx is
**+26.6% instructions / −11% IPC** vs the upstream reference JIT, and the dominant
instruction excess is `IADD_C*`/`IXOR_C*` imm-materialization (86% of superscalar-body
extras, per `w23-opcode-cost.md`). The remaining untried lever was **register-hoist**:
load each C7/C8/C9 constant once at program prologue into a register, then each occurrence
becomes a single `add`/`eor` (≈714 fewer instructions, zero memory traffic — unlike W3's
cache-thrashing literal pool). This note records why that lever is **infeasible**.

## The blocker: random 32-bit immediates exhaust the register file

- Each superscalar C* opcode carries its **own** immediate via `instr.getImm32()`
  (`src/jit_compiler_a64.cpp:1171,1176`). The superscalar program generates these from
  `gen.get_uint32()` — independent uniform-random 32-bit values.
- `w23-opcode-cost.md` already established: P(imm < 2^16) ≈ 2^−16 ≈ 0, so **every** C*
  op needs the 2-instruction `MOVZ`+`MOVK` (or `MOVN`+`MOVK`) materialization, then the ALU
  → 3 instructions. The constant is **different for (almost) every C* op**.
- Superscalar body has **~714 C* ops** per `generateSuperscalarHash` compile (W3's figure:
  `IXOR_C*` 387 + `IADD_C*` 327 = 714), i.e. **~714 distinct random 32-bit constants**.
- Usable registers inside the generated `rx_calc_dataset_item` code: x0–x7 = the 8-entry
  superscalar register file (must be preserved across the program); x10/x11 = prologue setup
  (reusable after); x12 = `tmp_reg`; x13 = scratch for `IADD_C*`; x14/x15 + x19–x28
  (callee-saved, available because the C caller saves them) ≈ **15–18 free registers total**.

**714 distinct random constants ≫ ~18 registers.** Hoisting every distinct constant is
impossible without spilling, and spilling reintroduces load traffic — which is exactly the
memory-access pattern W3 proved **regresses** (cache-misses tripled 109M→339M, −16% to −20%
H/s). So register-hoist saves nothing for the ~696 non-cached constants; it only helps if a
constant *repeats* across ops, and for uniform-random 32-bit data repeats are ≈0.

## Empirical cross-check (this session)

`bench_opcodes` (main-VM opcode frequency/byte histogram) was run on device:
- Confirms the **main VM has no C7/C8/C9 opcodes** — they are superscalar-only. The
  imm-materialization cost is entirely in the superscalar body (80.5% of all instructions,
  W1-1), not the main VM.
- Main-VM FAT opcodes (FSUB_M/FADD_M/FDIV_M @ 27–31 B, IADD_M/ISUB_M @ ~18.7 B) are the
  memory ops — already known, and W3-2 proved unsafe to retune (deterministic divergence
  under 200/450-pair stress).

This is consistent with the generator reading: `IADD_C*`→`emitAddImmediate(dst,dst,imm,13,…)`
and `IXOR_C*`→`emitMovImmediate(tmp,imm)+EOR` (lines 1162-1178), each imm unique.

## Conclusion

**Register-hoist is not a viable lever on this ISA/hardware.** The +26.6% instruction gap
from imm-materialization is **inherent to encoding random 32-bit constants in AArch64's
32-register file without cache-thrashing loads**. Both reduction strategies are now exhausted:

| Strategy | Result |
|---|---|
| Literal-pool load (W3) | Regressed: −16% to −20% H/s (cache-misses 109M→339M) |
| Register-hoist (this note) | Infeasible: 714 distinct random imm ≫ ~18 registers; spilling = memory traffic = same regression |
| Memory-op scheduler (W3-2) | Unsafe: deterministic JIT/interpreter divergence under stress |

Combined with W1-4 (IPC 1.369, only −11% vs reference; no catastrophe) and the known
main-VM stall (W3-2), **the ~10–20% gap to XMRig is a structural AArch64/in-order-A53
ceiling, not a fixable inefficiency.** Perf-optimization pursuit is closed on evidence.

## Status
- W3 family (immediate-materialization density): **CLOSED** — all three variants
  (literal-pool, register-hoist, memory-op-scheduling) exhausted/rejected with evidence.
- No remaining viable instruction-reduction or IPC lever for the superscalar/main-VM body.
- `ARMRX_MAX_SWAPS` bisect instrument + `bench_opcodes`/`bench_armrx` perf harnesses remain
  as reusable diagnostic infra.
