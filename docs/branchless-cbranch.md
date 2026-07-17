# Branchless CBRANCH — Research Notes

## Problem

CBRANCH is a RandomX VM instruction that conditionally jumps backward in the
instruction stream when a specific byte lane of a register becomes zero after
adding a masked immediate.

In the AArch64 JIT compiler (`jit_compiler_a64.cpp:1138-1163`), it emits:

```
add xD, xD, imm      ; add masked immediate to register
tst xD, mask          ; test register against byte mask
beq target            ; conditional backward branch
```

`beq target` is a **backward conditional branch**. The AArch64 branch predictor
predicts backward branches as **TAKEN**. However, the CBRANCH condition (a
specific byte lane becoming zero) is met only ~0.4% of the time for random
register values. This creates a **99.6% misprediction rate** for every CBRANCH
instruction.

Per OPTIMIZATION_REFERENCE.md, armrx has **13× more branch misses** than XMRig
(152M vs 11M), and distributed mispredictions are a significant contributor.

## Attempted Fix

Replace the single `beq target` (backward, predicted TAKEN) with:

```
bne .Lskip            ; forward, predicted NOT-taken (correct 99.6%)
b target              ; unconditional backward (always taken)
```

- `bne .Lskip` skips the next instruction when condition is NOT met (99.6% of
  the time) — correctly predicted NOT-taken.
- `b target` is only reached when condition IS met (~0.4%) — always correctly
  predicted as taken.

This should reduce CBRANCH mispredictions from ~99.6% to ~0.4%.

## Result: TEST HANG

When deployed, the KAT test suite (`armrx_tests`) hung indefinitely (120s
timeout vs normal ~16s). The branchless encoding was reverted to the original
`beq`.

## Suspected Root Cause

The unconditional `b` instruction uses a 26-bit signed offset (`imm26`),
while the original `beq` uses a 19-bit signed offset (`imm19`). The offset
calculation:

```cpp
int32_t branch_off = ((offset - static_cast<int32_t>(k)) >> 2);
emit32(0x14000000 | (branch_off & 0x03FFFFFF), code, k);
```

The `& 0x03FFFFFF` mask preserves bit 25 as the sign bit for the 26-bit
encoding. However, the `offset` variable was read BEFORE the `bne` was emitted,
and `k` advanced during `emit32(bne)`. The computation should be correct since
`k` is taken after the `bne` emission, but the interaction with
`emitAddImmediate` (which emits 1-3 instructions with variable length) may
cause incorrect offsets in edge cases.

**Potential issues to investigate:**

1. **`emitAddImmediate` variable length**: The `add` instruction at the top of
   `h_CBRANCH` emits 1-3 instructions depending on the immediate size. The
   `reg_changed_offset` was set based on the previous instruction's final `k`,
   which already included the correct length. This should be fine.

2. **Forward branch offset for `bne`**: The `bne` emits with a +1 instruction
   offset (`imm19 = 1`). This means "skip 1 instruction" (the `b`). Verified
   correct: `0x54000000 | (1 << 5) | 1` = `0x54000021`.

3. **26-bit sign extension for `b`**: The unconditional branch encoding uses
   a 26-bit signed offset. For backward branches (negative offset), the
   26-bit two's complement value must have bit 25 = 1. The
   `branch_off & 0x03FFFFFF` operation should preserve this if `branch_off`
   is a negative int32_t (since bit 25 of `0xFFFFFFXX & 0x03FFFFFF` = 1 for
   negative values).

4. **The `tst` instruction might affect the `bne` differently**: The `tst`
   instruction sets the Z flag when `(xD & mask) == 0`. `bne` (condition code
   `NE` = 1) branches when Z flag is CLEAR, i.e., when the condition is NOT
   met. This is the correct inversion.

5. **Most likely cause**: The `offset` variable (`reg_changed_offset[instr.dst]`)
   might be 0 (initialized by `memset` in the constructor) for the very first
   CBRANCH, causing it to branch to absolute position 0 in the code buffer.
   This would be true for both the old and new code, but the old `beq` has
   tighter bounds checking due to its 19-bit offset limitation.

## Next Steps for Future Attempts

1. **Profile first**: Run `perf stat` on the working build to isolate actual
   CBRANCH misprediction counts. The ROADMAP estimates +1-2% impact, but
   this should be verified with real hardware counters.

2. **Debug the bne+b encoding**: Build a minimal test case that emits just
   the CBRANCH sequence into a small code buffer and single-steps through it
   with GDB to verify the branch targets.

3. **Alternative approach — CSEL**: Instead of branching, use conditional
   select to compute the next instruction offset:
   ```
   add xD, xD, imm
   and xTemp, xD, mask
   cmp xTemp, #0
   csel xTarget, xCurrent, xCbrTarget, ne
   br xTarget
   ```
   This requires a free register for the target PC, which may not be available
   (x18 is free but using it for computed goto adds a BR instruction that may
   have its own pipeline cost).

4. **Alternative approach — Invert branch direction**: Place the jump target
   immediately after the CBRANCH and use `bne` to skip it (forward, predicted
   not-taken). This is essentially what the attempted fix did, but the
   encoding needs verification.

5. **Consider static branch hint**: GCC supports `__builtin_expect` but
   AArch64 assembly doesn't have explicit branch prediction hints. The
   forward/backward direction is the only static predictor.
