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

## Result: TEST HANG (first attempt — fixed with imm19=2)

When first deployed, the KAT test suite (`armrx_tests`) hung indefinitely (120s
timeout vs normal ~16s). The branchless encoding was reverted to the original
`beq`.

### Root Cause (found by review)

The `bne` forward-skip offset was `imm19=1`, but `B.cond` computes its target as
`PC_of_bne + imm19*4`. To skip the 4-byte `b target` instruction ahead, the
target must be `PC + 8`, requiring **`imm19=2`**.

With `imm19=1`, the `bne` landed on the `b target` instruction itself — meaning
the unconditional backward branch **always** executed, regardless of whether the
CBRANCH condition was met. This turned every CBRANCH into an unconditional
backward jump, causing the VM instruction pointer to loop forever.

### Fix

`0x54000000 | (1 << 5) | 1` → `0x54000000 | (2 << 5) | 1` (imm19=2, skip 8 bytes)

### Status

The fix is deployed, passing all KATs, and included in the current build.
The 5 hypotheses in the original "Suspected Root Cause" section below were
written before the real imm19 bug was identified — they are all **superseded**
and kept only as a record of the investigation process.

### BTB aliasing caveat

AArch64's static branch prediction (backward=taken, forward=not-taken) only
affects *cold* branches. Once the JIT'd program runs its ~2048 iterations,
the dynamic predictor learns the true behavior. However, the JIT buffer is
regenerated per program (every new seed), so a fixed code *address* sees a
rotating set of different branch behaviors over time — this BTB/history
aliasing may be a significant contributor to the 13× branch miss gap
regardless of single-branch encoding. The real fix may involve reducing
how often branch-heavy addresses are reused with different behavior.

The imm19 fix is correct and deployed; the `-0.5%` estimate remains
unverified until Phase 1 `perf stat -e branch-misses` confirms the
mispredict rate dropped on real hardware.

### Unit test recommendation — done (2026-07-22)

~~Add a test that decodes the emitted `bne`/`b` bytes for a CBRANCH sequence
and asserts the computed target address.~~ `tests/test_jit_encodings.cpp` now
does exactly this. Getting it right required a new read-only
`getCodeBytes()`/`getJitCodeBytes()` accessor (deliberately const-only, unlike
the deleted mutable `getCode()` — see that function's own comment) and working
around a real subtlety: `randomx_calculate_hash()` runs several chained
internal rounds reusing the same JIT buffer (`emitPrologueMix` resets
`codePos` each compile), so `JitDumpEntry`s from earlier rounds share offset
numbers with — but point to memory since overwritten by — the final round.
The test now only trusts the last contiguous run of entries, and locates the
`bne` by its exact bit pattern rather than assuming a fixed position (the
variable-length `add` sequence, including a NEON `smov`/`umov` literal-pool
path, means the tst/bne/b tail isn't always at a fixed offset from the end of
the entry). Also added `tests/test_jit_equivalence.cpp`: a JIT/interpreter
equivalence sweep across many seeds×inputs (the existing KAT only checks 2
fixed inputs). Both pass cleanly against the *current* CBRANCH implementation
— this turns a "wait 120s for timeout" failure mode into an instant,
localized assertion for any future branch-encoding change.

---

## Superseded investigation notes

The following hypotheses were written before the real imm19 bug was found.
They are kept for reference but are **not the actual root cause**.

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

## Precise attribution (2026-07-22) — the "profile first" step, finally done

Every prior session recommended this exact step (`docs/archived/audits/audit-20260721-cross-reference.md`,
`docs/archived/next_phase.md`, `docs/plans/performance-next-agent-handoff.md` §22.6) but none had
actually executed it — only aggregate `perf stat` counting existed, which gives a single
branch-miss *count* with no attribution to *where* in the code the misses happen. This
session ran `perf record -e branch-misses -c 10000 -- ./bench_armrx --full-hash-only`
on-device (1423 samples, full run to completion) and `perf report --sort=dso`/`--sort=symbol`.

**Method note:** the JIT-generated machine code lives in a runtime-allocated buffer, not
the ELF's `.text` section, so `perf` can't symbolize it by name — it shows as a separate
`[JIT] tid <N>` mapping (distinct from generic `[unknown]`) once perf finishes a *complete*
(non-truncated) recording. An initial 150s-truncated capture mis-binned `[JIT]` samples into
`[unknown]`, which is why the final numbers below differ from an earlier same-session
estimate — always let `perf record` reach a clean exit before trusting the DSO breakdown.

**Results (full-run capture, 1423 samples):**

| Location | % of all samples |
|---|---|
| `[k]` kernel-space addresses | 46.03% |
| `bench_armrx` (statically-compiled code) | 35.63% |
| `[JIT] tid <N>` (JIT-generated code) | 10.54% |
| `ld-musl-aarch64.so.1` | 7.52% |
| `libgcc_s.so.1` / `[vdso]` | 0.28% |

The 46% kernel-space bucket was checked against `strace -f -c` on the identical workload:
**53 syscalls totaling 6.6ms** across the entire multi-second benchmark run (one-time
`mmap`/`mprotect`/`munmap` at startup/teardown — `enableWriting()`/`enableExecution()`
correctly no-op once `rwx_` is true, so no per-hash `mprotect` calls happen). With
essentially zero real kernel-space work, the `[k]` bucket is almost certainly **PMU
sampling skid** — this Cortex-A53 has no ARM SPE (Statistical Profiling Extension), so a
branch-miss counter overflow interrupt can attribute its recorded PC to the interrupt
handler itself before control returns to userspace, rather than to the instruction that
actually caused the miss. This is a measurement artifact of this specific chip, not a real
performance lever — treat the ~54% of samples landing in resolvable userspace code as the
trustworthy signal.

Within that resolvable ~54% (`--sort=symbol`, top entries):

| Symbol | % of all samples |
|---|---|
| `armrx::Argon2dCache::initialize` | 12.44% |
| `memcpy` | 6.32% |
| `armrx::VirtualMachine::run` | 3.37% |
| `permute_block_neon` | 3.23% |
| `permute_16_neon` | 2.95% |
| ~20 different `JitCompilerA64::h_*` handlers (JIT **compile**-time, not execution) | ~8–9% combined, each ≤1.12% |
| `armrx::generate_superscalar` | 0.56% |

**Two findings that matter for scoping this work:**

1. **CBRANCH is confirmed to be the JIT compiler's *only* emitted data-dependent conditional
   branch.** Grepping every `0x54xxxxxx` (B.cond) encoding in `jit_compiler_a64.cpp` finds
   exactly two sites: `h_CBRANCH`'s `bne` (always emitted) and `h_CFROUND`'s `bne` (only
   under `RANDOMX_FLAG_V2`, which isn't set in this benchmark and is 1/256 frequency even
   when it is). So essentially all of the `[JIT]` 10.54% is attributable to CBRANCH
   specifically — this is real signal, not diluted by some other opcode.
2. **Superscalar is confirmed *not* the dominant contributor here** (`generate_superscalar`
   at 0.56%, `execute_superscalar` not even in the top ~30 symbols) — this directly
   contradicts the old audit's "94.85% of misses are in Superscalar" claim, at least for
   this light-mode, full-hash workload on this hardware. `Argon2dCache::initialize` +
   its NEON permute helpers (18.62% combined) are actually a *larger* single contributor
   than CBRANCH's 10.54% — flagged for a future investigation, but out of scope for the
   current CBRANCH-only effort per explicit direction (2026-07-22).

**Bottom line: the "profile first" prerequisite is now satisfied.** CBRANCH work is
justified — it's a real, non-trivial, cleanly-attributed contributor, not a shot in the
dark — but it is not the *only* lever, and shouldn't be oversold as one.

## CSEL tried, measured, and reverted (2026-07-22) — a small net regression

Implemented item 3 below in full: `ands xTemp, dst, mask` (same bitmask
encoding as the existing `tst`, but with a real destination so the masked
value is available, not just flags) → `adr` the fallthrough address → `adr`
the backward target → `csel` between them → single unconditional `br`. Only
`x19`/`x20` are genuinely free scratch registers in this JIT's fixed
allocation (`src/jit_compiler_a64_static.S`'s "Register allocation" block) —
**`x18` is explicitly reserved** ("platform register, don't touch it"),
contradicting this doc's own earlier assumption that it might be usable.

Caught one real bug via the KAT test before ever benchmarking: the `csel`'s
`Rn`/`Rm` register fields were transposed (`Rm` is bits 20-16, `Rn` is bits
9-5 — easy to get backwards), which silently inverted which address got
selected when the branch condition was met. The JIT hash was simply wrong
until fixed — exactly the failure mode `docs/plans/performance-next-agent-handoff.md`
§19's "run KATs before benchmarking" step exists to catch.

Once correct, a clean **apples-to-apples** `perf stat` comparison (same tool,
same `--full-hash-only` workload, old `bne`/`b` code rebuilt fresh in a
separate directory for a fair baseline — not compared against an earlier
number captured under `perf record`'s higher sampling overhead) showed CSEL
is a net regression, not an improvement:

| | `bne`/`b` (current) | CSEL (reverted) | Δ |
|---|---|---|---|
| Hashrate | 4.48 h/s | 4.47 h/s | flat (noise) |
| Instructions | 74.80B | 75.45B | +0.87% |
| Cycles | 98.77B | 99.09B | +0.32% |
| Branch-misses | 14.1M | 20.7M | **+46%** |
| Branch-miss rate | 2.40% | 3.51% | worse |

Consistent with the BTB-aliasing caveat above: since the JIT buffer is
regenerated every hash, no encoding trick fixes the underlying predictor-
history problem — the extra indirect `br` just gives the predictor one more
thing to guess wrong, for the cost of 2 more always-executed instructions.
**Reverted** to `bne`/`b` (`git checkout` back to the `e563112` state).

## The 31.08% figure does not represent the mining hot path (found 2026-07-22)

Getting a fair CSEL baseline required isolating `bench_armrx --full-hash-only`,
which showed a branch-miss rate of only **2.4%** — nothing like the 31.08%
figure that has justified CBRANCH-focused work across multiple sessions.
Running `perf stat` on each of `bench_armrx`'s three sections separately and
summing reproduces the historical aggregate almost exactly (169.35B
instructions, 224.68B cycles, 7.80B branches, 2.43B misses, 31.12% rate vs.
the reported 31.08%), confirming the reconciliation is sound:

| Section | Branches | Misses | Local rate | Share of total misses |
|---|---|---|---|---|
| `--full-hash-only` (the actual mining hot path) | 589M | 14.1M | 2.4% | **0.58%** |
| `--micro-only` | 472M | 109M | 23.1% | 4.49% |
| `--attribution-only` | 6.74B | 2.31B | 34.2% | **94.93%** |

`--attribution-only` includes a 30-sample **interpreted-mode** comparison run
(reported as "JIT speedup: 11.37×") that never executes during real JIT
mining on AArch64 — the interpreter's dispatch-table branching is inherently
far more mispredictable than JIT'd code, and it (plus that section's own
internal phase-timing sub-benchmarks) dominates the aggregate. 94.93% is
nearly identical to the old audit's much-cited "94.85% in Superscalar" claim
— strong evidence that claim measured this same real phenomenon but
misattributed it to Superscalar dataset generation rather than the actual
cause.

**Real-world impact, recalculated**: 14.1M mispredictions on the actual hot
path at an 8–11 cycle penalty costs only **~0.11–0.16% of total cycles** —
not the "~8.6–11.9%" previously estimated by applying the diluted 31% rate
uniformly to the whole workload. **CBRANCH misprediction was never a
meaningful real-world performance lever on this hardware.** This closes the
CBRANCH investigation started in this file — no further JIT branch-encoding
work is justified by the data. If `bench_armrx`'s aggregate branch-miss
number is ever cited again, isolate `--full-hash-only` first, or caveat that
the default (no-flag) run's PMU counters are dominated by non-representative
benchmark code, not the mining hot path.

## Next Steps for Future Attempts

1. ~~**Profile first**: Run `perf stat` on the working build to isolate actual
   CBRANCH misprediction counts.~~ Done above (2026-07-22) — use `perf record` with
   symbol attribution, not just `perf stat`'s aggregate count.

2. **Debug the bne+b encoding**: Build a minimal test case that emits just
   the CBRANCH sequence into a small code buffer and single-steps through it
   with GDB to verify the branch targets. *(Superseded — the real bug in a
   related encoding, found 2026-07-22, was a transposed register field in
   `csel`, caught directly by the KAT test instead; no GDB session needed.)*

3. ~~**Alternative approach — CSEL**~~: Instead of branching, use conditional
   select to compute the next instruction offset:
   ```
   add xD, xD, imm
   and xTemp, xD, mask
   cmp xTemp, #0
   csel xTarget, xCurrent, xCbrTarget, ne
   br xTarget
   ```
   **Done and reverted (2026-07-22) — see the section above.** Measured a net
   regression (more instructions, more cycles, 46% more branch-misses, flat
   hashrate), not an improvement. `x18` is NOT free, contrary to what this
   item originally assumed — see `src/jit_compiler_a64_static.S`'s register
   allocation comment block.

4. **Alternative approach — Invert branch direction**: Place the jump target
   immediately after the CBRANCH and use `bne` to skip it (forward, predicted
   not-taken). This is essentially what the attempted fix did, but the
   encoding needs verification.

5. **Consider static branch hint**: GCC supports `__builtin_expect` but
   AArch64 assembly doesn't have explicit branch prediction hints. The
   forward/backward direction is the only static predictor.
