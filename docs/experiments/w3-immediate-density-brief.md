# W3 — Superscalar immediate-materialization density (scheduler-safe)

**Author:** Hermes (design + verification). **Implementer:** Cursor (`agent -p --force`).
**Date:** 2026-08-01. **Status:** Ready to implement.

## Goal

Reduce the ~10% instruction-count excess in the superscalar dataset-derivation
region by replacing the 2-instruction `MOVZ/MOVN + MOVK` materialization of
`IXOR_C7..9` / `IADD_C7..9` large immediates with a 1-instruction `LDR` from a
per-program, **per-original-index** literal pool. Net: −1 instruction per C* op
(~714 ops/hash × 1 ≈ 714 A64 ≈ ~10% of the ~7,100-instr superscalar body ≈
~6 M instr/hash out of ~119 M total). This is the only evidence-backed
optimization lever remaining from the W1–W2 diagnosis (see
`docs/experiments/w23-opcode-cost.md` §4–5).

## Why this is scheduler-safe (the key insight)

`scheduleSuperscalarProgram()` (`jit_compiler_a64.cpp:715`) returns a vector of
**original program indices** `j` (the emit loop reads `const size_t j =
emit_order[idx]; const Instruction& instr = prog(j);` at `:1114`). The emission
order is reordered, but each emitted instruction KNOWS its original index `j`.

Therefore a literal pool laid out 1 slot per original index `j` is addressed
**order-independently**: the `LDR` emitted for instruction with original index
`j` always reads `pool_base + j*4`, regardless of where it lands in the emitted
stream. This is *strictly safer* than the existing `IMUL_RCP` reciprocal pool
(`jit_compiler_a64.cpp:1160`), which is addressed by a *sequential pointer*
(`literal_pos += 8`) and therefore still requires the scheduler to preserve
`IMUL_RCP` relative order (the `is_imul_rcp` swap-exclusion at `:727-728`).
Our C* pool needs no such exclusion — each load is self-indexed.

**Bonus:** the change makes the `num32bitLiterals = 64` pin
(`jit_compiler_a64.cpp:1080`) obsolete and lets us REMOVE it, eliminating the
fragile scheduler-safety dependency that audit
`docs/archived/audits/scheduler-review-2026-07-25.md` §4 flagged. After this
change, `emitMovImmediate` is no longer called from the superscalar path at all
(verify: grep superscalar `emitMovImmediate`/`emitAddImmediate` callers — only
`IXOR_C*`/`IADD_C*`, which we replace), so the pin's purpose is gone.

## Files to change

### 1. `src/jit_compiler_a64.cpp` — `generateSuperscalarHash()` (LIVE path)

Current relevant code:
- Pre-pass that fills the IMUL_RCP reciprocal pool, `:1099-1105`.
- `literal_pos = jmp_pos; emit32(B ...)` jump-over-pool, `:1108-1109`.
- Emit switch: `IXOR_C7..9` at `:1148-1153` (`emitMovImmediate(tmp_reg, ...)`
  + `EOR`); `IADD_C7..9` at `:1137-1147` (`emitAddImmediate(dst,dst,imm,13,...)`).
- `constexpr uint32_t tmp_reg = 12;` at `:1081` (IXOR_C uses x12; IADD_C used
  x13 via the emitAddImmediate 4th arg).
- `num32bitLiterals = 64;` pin + long comment, `:1069-1080`.

Changes:

(a) **Add a per-program C* immediate pool** in the pre-pass region. After the
existing IMUL_RCP reciprocal loop (`:1105`) and BEFORE the `B` jump
(`:1108`), do:

```cpp
// C* immediate pool: one 32-bit slot per original index j, addressed
// order-independently by the emitted LDR (see design note). Non-C*
// slots are unused (written 0) so slot j = base + j*4 always.
const uint32_t cimm_pool_base = codePos;
for (size_t j = 0; j < progSize; ++j) {
    const Instruction& pi = prog(j);
    const auto op = static_cast<SuperscalarInstructionType>(pi.opcode);
    const bool is_c = (op == SuperscalarInstructionType::IXOR_C7 ||
                       op == SuperscalarInstructionType::IXOR_C8 ||
                       op == SuperscalarInstructionType::IXOR_C9 ||
                       op == SuperscalarInstructionType::IADD_C7 ||
                       op == SuperscalarInstructionType::IADD_C8 ||
                       op == SuperscalarInstructionType::IADD_C9);
    emit32(is_c ? pi.getImm32() : 0u, code, codePos);
}
```

The `B` at `:1108` already jumps `jmp_pos` over `codePos`, which now includes
this pool, so execution skips it (data, never executed) exactly like the
IMUL_RCP pool. **Keep the existing `B` as-is** — its target is `codePos` after
both pools; no change needed there.

(b) In the emit switch, replace the two C* cases:

`IXOR_C7..9` (`:1148-1153`) →
```cpp
{
    // LDR tmp_reg(=x12), [literal = cimm_pool_base + j*4]
    int32_t off = static_cast<int32_t>(cimm_pool_base + j * 4 - codePos) / 4;
    off &= (1 << 19) - 1;
    emit32(ARMV8A::LDR_LITERAL | tmp_reg | (static_cast<uint32_t>(off) << 5), code, codePos);
    emit32(ARMV8A::EOR | dst | (dst << 5) | (tmp_reg << 16), code, codePos);
}
```
(`tmp_reg` is already x12 from `:1081`.)

`IADD_C7..9` (`:1137-1147`) →
```cpp
{
    // use x13 as scratch (caller-saved; ABI reason in existing comment)
    constexpr uint32_t add_scratch = 13;
    int32_t off = static_cast<int32_t>(cimm_pool_base + j * 4 - codePos) / 4;
    off &= (1 << 19) - 1;
    emit32(ARMV8A::LDR_LITERAL | add_scratch | (static_cast<uint32_t>(off) << 5), code, codePos);
    emit32(ARMV8A::ADD | dst | (dst << 5) | (add_scratch << 16), code, codePos);
}
```
(Remove the old `emitAddImmediate(dst, dst, instr.getImm32(), 13, code, codePos);`
call — we no longer use it here.)

(c) **Remove the `num32bitLiterals = 64;` line (`:1080`) and its explanatory
comment (`:1069-1079`)**, replacing with a one-line note that C* immediates
now use the per-index pool above (emit-safe under reordering, no NEON-pool /
scheduler-safety dependency). Do NOT touch `num32bitLiterals` elsewhere — the
main VM path (`emitPrologueMix`, `:749`) still sets it to 0 and uses the NEON
pool independently.

### 2. Buffer sizing — CRITICAL (avoid device segfault)

`CalcDatasetItemSize` (`jit_compiler_a64.cpp:118-131`) sizes the allocation
`allocMemoryPages(CodeSize + CalcDatasetItemSize)` (`:143`). Adding the pool
grows code by up to `progSize × 4` bytes per program. Max `progSize =
kSuperscalarMaxSize = 3*kSuperscalarLatency+2 = 512` (`randomx_config.hpp:20`),
× 8 programs (`kRandomXCacheAccesses = 8`, `randomx_config.hpp:18`) × 4 bytes =
**16,384 bytes**. Add this constant to `CalcDatasetItemSize`:

```cpp
// W3 immediate pool (per-program per-index C* immediates): 512 slots × 4B × 8 programs
+ (512u * 4u * 8u)
```
placed inside the `+` chain (e.g. right after the Epilogue term, `:130-131`).
This guarantees no overflow regardless of layout. (Body actually *shrinks* by 1
instr/C*; only the pool grows. The +16 KiB is conservative headroom.)

### 3. `src/jit_dataset_2way.cpp` — mirror for parity (NOT in live mining path)

This emitter is not wired into any production call site (`jit_compiler_a64_static.hpp:70-74`:
"Not used by generateSuperscalarHash() or any existing call site"). But
`test_jit_dataset_2way` exists as a bit-identical oracle for it, so mirror the
change for correctness parity:

- In `emitSuperscalarInstr` (`jit_dataset_2way.cpp:128`), replace the `IXOR_C*`
  case (`:152-157`) and `IADD_C*` case (`:147-151`) with the same LDR-from-pool
  + ALU pattern, using the per-stream `tmpReg` (already passed in). The pool is
  **shared single-copy** across the 2 streams (both process the same program /
  same index `j`), addressed by original index `j`.
- `generate()` (read it — around `jit_dataset_2way.cpp:175+`) must allocate the
  C* pool per program in its buffer and pass `cimmPoolBase` into
  `emitSuperscalarInstr`. Mirror the 1-way pre-pass: emit one uint32 slot per
  original index `j` (C* → imm, else 0) before the emitted body, inside the
  jumped-over / skipped region.
- Bump `calcDataset2WaySize()` (`:182`) by the same `512*4*8` (or however many
  programs it derives — read `generate()` to confirm the program count it loops
  over; it is `kRandomXCacheAccesses = 8`) so the allocation cannot overflow.

**If the 2-way mirror is awkward or risky, you MAY scope it out and instead
leave `jit_dataset_2way.cpp` untouched — BUT then you MUST note in your summary
that the 2-way path was intentionally left on the old MOVZ/MOVK path, and the
change is 1-way-only.** Do not half-implement it.

## Constraints (hard)

- **Clean room:** do NOT reference, copy, or diff against XMRig/GPL sources.
  Work only from the files in this repo.
- **Do not modify** `tests/`, `scratch_vm_study/`, or the main-VM
  `emitPrologueMix`/`emitMovImmediate` NEON-pool logic. The change is
  superscalar-only.
- **Do not touch** `scheduleSuperscalarProgram()` or the `is_imul_rcp`
  swap-exclusion — our pool is order-independent and needs no scheduler change.
- Keep `.clang-format` Allman style, 120-col. Match surrounding style.

## Correctness gate (must pass before any perf claim)

1. Host cross-build must compile clean:
   `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake && cmake --build build-cross -j$(nproc)`
2. The host cannot run the JIT (needs device). Cross-compile `armrx` and the
   test binaries; Hermes will scp to device and run:
   - `test_jit_equivalence` (1-way live path oracle) — must be byte-identical.
   - `test_jit_dataset_2way` (if 2-way mirror done) — must be bit-identical.
   - `test_jit_determinism` — JIT compiled twice → byte-identical code.
   These are the real correctness gates. If any fail, STOP and report; do not
   proceed to perf.

## Verification (Hermes runs on device)

- md5-verify the modified `armrx`/`test_*` vs a baseline built from `c5ac985`.
- `test_jit_equivalence`, `test_jit_dataset_2way`, `test_jit_determinism` byte-identical.
- Perf A/B (`perf stat -e instructions,cycles` over a fixed mining window):
  report `instructions/hash` and `H/s` for baseline vs modified. Expected:
  ~10% fewer instructions in superscalar region. H/s verdict = measured delta.

## Definition of done

- Cross-build clean.
- All three correctness tests pass on device (byte-/bit-identical).
- `changelogs.md`, `README.md`, `ROADMAP.md` updated by Hermes (not you).
- Perf A/B reported with a win/no-win verdict.

## Rollback

If any correctness test fails or perf shows regression/neutral with risk, revert
the diff entirely (the change is additive-to-a-pin; a clean `git checkout` of
the two source files restores baseline). No schema/data migrations involved.
