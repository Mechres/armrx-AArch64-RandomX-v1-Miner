# Superscalar IMUL_RCP register pre-assignment — tried, caused a real JIT/interpreter divergence, reverted (2026-07-26)

**Status: reverted.** This is `docs/plans/experimental-performance-ideas-20260725.md` idea #1.
Implemented, failed the very first differential test case, reverted in full.

## The idea

The main VM program's `h_IMUL_RCP` (`src/jit_compiler_a64.cpp`) pre-assigns physical registers
(x30, x29, ..., x21, x11, x0 — 12 total) for a program's first 12 `IMUL_RCP` reciprocal literals,
loading them once and referencing them directly (`MUL dst, dst, literal_reg`) instead of a
`LDR_LITERAL` + `MUL` pair for each use. The idea was to do the same for the superscalar
(dataset-derivation) path's `generateSuperscalarHash()`, which still does `LDR_LITERAL` + `MUL`
unconditionally for every `IMUL_RCP` instruction.

## Register selection — the backlog doc's claim was wrong

The doc claimed "the superscalar path uses x0..x7 for VM registers, leaving x9..x15 and x19..x28
as available literal registers." Reading `src/jit_compiler_a64_static.S`'s actual
`randomx_calc_dataset_item_aarch64` wrapper before touching any code: x8 (dataset base pointer),
x9 (output pointer), x10 (per-iteration carried `registerValue`), x11 (per-iteration prefetch
scratch, recomputed from x10 every iteration), and x12/x13 (literal-load temp / cache-line mix
scratch) are **all live across the whole per-program instruction stream** — x9 is not free,
contradicting the doc. The wrapper's own save/restore only covers x0-x13 (`stp`/`ldp` pairs at the
function's entry/exit), unlike the main VM program's entry (`randomx_program_aarch64`), which
saves x16-x30 and d8-d15 around the whole JIT-compiled body. Since this function is invoked via a
plain `bl` from other JIT-compiled code (the main program's light-mode body), not a C-ABI
boundary, and its wrapper deliberately only protects x0-x13, the corrected safe set is **x14, x15,
x19-x28** (12 registers) — genuinely unused anywhere in the surrounding glue, confirmed by reading
every instruction in the wrapper, not just skimming the doc's claim.

## Implementation

A pre-pass was added in `generateSuperscalarHash()`, right after `scheduleSuperscalarProgram()`
computes `emit_order` and before the main per-instruction emission loop: walk `emit_order`, and
for the first (in schedule order) up to 12 `IMUL_RCP` instructions found, emit a preload
`LDR literal_reg[k], [literal_pos]` using the same sequential `literal_pos` consumption the
existing code already relies on (the scheduler's own IMUL_RCP-relative-order-preservation
guarantee is what makes this safe to consume in a separate pre-pass and have it line up with the
main loop's later consumption of the same sequence). The main loop's `IMUL_RCP` case was changed
to check a counter and, for the first 12, skip the `LDR_LITERAL` and emit only
`MUL dst, dst, literal_reg[k]`; instructions beyond 12 kept the original indirect-load path
unchanged.

## Result: failed test_jit_equivalence on the first case

```
FAIL: JIT/interpreter mismatch for seed="jit_equiv_seed_0" input="equivalence input 0_0"
```

Same failure signature as `docs/experiments/memory-op-scheduler-attempt.md`'s earlier scheduler
extension: wrong on the very first, most basic seed/input pair, not an edge case. `test_jit_encodings`
and `test_jit_determinism` both passed, so this isn't a gross encoding error — the divergence is
specific to actual dataset-item content in at least one of the exercised programs.

**Mechanism not identified.** Register selection was independently re-verified against the actual
`.S` wrapper (not just the doc's claim) and appears correct; the literal-pool consumption ordering
relies on the same scheduler guarantee the existing indirect-load path already depends on. Given
the failure mode is silent wrong hashes and no specific root cause was isolated in the time spent,
the change was fully reverted (`git checkout -- src/jit_compiler_a64.cpp`) rather than shipped
with an undemonstrated fix, matching this project's standing rule for this risk class. Re-verified
clean (`test_jit_equivalence` passing) on the reverted code before moving on.

## If revisited

Candidate next steps, not attempted here:
- Bisect with a smaller reproduction (single seed, single program) and dump both JIT and
  interpreter intermediate register state after the superscalar phase specifically, rather than
  only the final hash, to localize which register/value first diverges.
- Double check whether `emit_order`'s IMUL_RCP-relative-order guarantee, which
  `scheduleSuperscalarProgram()`'s own doc comment states as *"no two IMUL_RCP instructions ever
  have their relative emission order changed,"* actually holds when the *first* instruction in a
  swapped triple (`fp[i].is_long_latency`) is itself scheduled early via the `i+2`/`i+1` swap
  pattern in a way that could put a later program's independent instruction between two IMUL_RCPs
  that this pre-pass assumed were contiguous in traversal order — not confirmed as the actual bug,
  but the most specific lead for a future attempt.
