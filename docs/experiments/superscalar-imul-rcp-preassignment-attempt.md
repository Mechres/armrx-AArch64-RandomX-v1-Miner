# Superscalar IMUL_RCP register pre-assignment — tried, root-caused, closed for now (2026-07-26)

**Status: closed for now, root cause understood, explicitly revisitable.** This is
`docs/plans/experimental-performance-ideas-20260725.md` idea #1, and
`docs/plans/mid-high-risk-performance-ideas-20260726.md` Tier 1 item 1. First attempt implemented,
failed the very first differential test case, reverted with the mechanism unidentified. Revisited
via bisection the same day: root cause found and confirmed, but the safe register budget turned
out to be at most 1 (not 12), making the achievable win too small to be worth the ongoing
correctness burden *right now*. Not left as an active open item, but not proven impossible either
— see "Revisit path" in `docs/plans/mid-high-risk-performance-ideas-20260726.md` item 1 for what
would need to change to make this worth another look (mainly: understanding why x20 specifically
fails, which could double the safe budget if the current analysis turns out to be wrong).

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

**Mechanism not identified at the time.** Register selection was re-verified against the actual
`.S` wrapper (not just the doc's claim) and appeared correct within that scope. Given the failure
mode is silent wrong hashes and no specific root cause was isolated in the time spent, the change
was fully reverted (`git checkout -- src/jit_compiler_a64.cpp`) rather than shipped with an
undemonstrated fix, matching this project's standing rule for this risk class. Re-verified clean
(`test_jit_equivalence` passing) on the reverted code before moving on.

## Root cause found on revisit (2026-07-26, same day)

The register-availability analysis above was scoped only to the superscalar wrapper
(`randomx_calc_dataset_item_aarch64`) — it never checked what the *caller* of that function has
live in those same physical registers. That's the actual gap.

`randomx_calc_dataset_item_aarch64` is invoked via a plain `bl` from *inside* the main VM
program's own JIT-compiled body, in light mode, once per main-loop iteration (2048×/hash) — not
as a standalone call. Reading the main program's own register table at the top of
`jit_compiler_a64_static.S`:

```
# x14 -> "r6"
# x15 -> "r7"
...
# x19 -> temporary
# x20 -> temporary
# x21 -> literal for IMUL_RCP
...
# x28 -> literal for IMUL_RCP
```

**x14 and x15 hold two of the main program's own live VM registers (r6, r7)** throughout its
entire execution, and **x21-x28 hold the main program's own pre-loaded `IMUL_RCP` literals**
(the exact same optimization already applied to the main path, `h_IMUL_RCP`) — loaded once at
program start and expected to survive for the program's full 2048-iteration lifetime. Neither the
light-mode call site (`randomx_program_aarch64_vm_instructions_end_light`, saves only x0/x1/x2/x30
around the `bl`) nor the superscalar wrapper's own entry/exit (saves x0-x13) protects x14, x15, or
x21-x28 across this call. **The only reason the original, unmodified code has always been correct
is that it never writes to any of those registers** — safety by non-interference, not by an
explicit contract. The moment new code writes into x14 (the first register in the original
12-register plan), it silently corrupts the main program's live r6 for the rest of that hash.

**Confirmed empirically via bisection**, using a temporary `ARMRX_IMUL_RCP_MAX_PREASSIGN` env var
(0-12, capping how many registers the fast path used) to test hypotheses without rebuilding:

| Registers used (in order) | Result |
|---|---|
| (none, cap=0) | Pass — sanity check, matches baseline |
| x19 alone (cap=1) | **Pass** — 16/16 pairs |
| x19, x20 (cap=2) | **Fail** — same seed_0/input_0 divergence |
| x19, x20, x14 (cap=3) | Fail |

x19 and x20 are the *only* two registers the main program's own table marks "temporary" (implying
genuinely free) rather than holding persistent state — and x19 alone does work. But **x20 also
fails**, for a reason not fully traced to a single instruction (the main-loop body reuses x19/x20
for "next iteration's scratchpad address" bookkeeping earlier in the same iteration that the `bl`
call happens in, computed *after* the call and consumed at the very end of the loop body — the
exact liveness window across the call wasn't fully pinned down for x20 specifically). No other
untested candidate registers remain: everything else in the original 12 has an independently
understood reason to be unsafe (x14/x15 = live VM registers, x21-x28 = the main program's own
`IMUL_RCP` literals, x0-x13 = used by the superscalar wrapper itself, x16-x18/x29-x30 = procedure-
call-reserved / frame pointer / return address).

## Why this is closed for now, not carried forward as an active item

The realistic safe register budget is **at most 1** (x19, lightly validated — only against the
16-pair `test_jit_equivalence` sweep, not the full 100-seed stress test), not the 12 originally
planned. At that scale the achievable win shrinks to "maybe eliminate one `LDR_LITERAL` for
whichever `IMUL_RCP` instruction happens to be scheduled first, in whichever of the 8 chained
per-hash superscalar programs has one early enough" — a payoff far below what would justify adding
a special-cased, register-allocation-fragile optimization to the JIT's most safety-critical code
path, one that would need to be re-verified any time the surrounding `.S` template or scheduler
changes what it keeps live across this call. **Not worth continuing right now** — but genuinely
revisitable, not proven impossible. See the "Revisit path" note in
`docs/plans/mid-high-risk-performance-ideas-20260726.md` item 1 for the two concrete next steps
(root-cause x20's failure; consider explicit save/restore at the call site) that would need to
happen before this is worth another attempt.

## Lesson for any future work touching this call boundary

Any future JIT change that emits code inside `generateSuperscalarHash()` (or anything else reached
via `bl rx_calc_dataset_item`) must treat **x14, x15, and x21-x28 as live and must not clobber
them** in light mode, even though nothing in the superscalar wrapper's own local save/restore
suggests they're in use. The safety of the existing code is contingent on this register set never
being touched — that contract isn't enforced anywhere, only true by inspection today.
