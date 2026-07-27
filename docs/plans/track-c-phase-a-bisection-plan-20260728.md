# Track C Phase A Retry: Bisection Plan for the Next Agent

## Status this plan assumes

Track C Phase A (light-mode dataset-item helper: drop unneeded x0-x3 register
preservation in `rx_calc_dataset_item`'s light-mode entry point) was implemented once,
verified byte-correct by both static and dynamic disassembly, and then hung
indefinitely on the very first JIT-mode hash. It was fully reverted
(`git checkout -- include/armrx/jit_compiler_a64_static.hpp src/jit_compiler_a64.cpp
src/jit_compiler_a64_static.S`), and the revert was confirmed clean via a full
`ctest` pass (14/14, timings matching the historical baseline).

Full background, the complete reconstructed diff of everything that was tried, and
the investigation already performed are in
`docs/experiments/light-mode-dataset-item-prologue-attempt.md` — **read that file
first.** This plan does not repeat that context; it only adds what's needed to retry
more surgically than the first attempt did.

Repo state right now: clean, matching HEAD, no Track C changes present. You are
starting from the known-good original.

## What is already ruled out — do not re-check these

Two hypotheses were checked against the actual (reverted) source after the first
attempt's postmortem was written, specifically to avoid wasting devbox time on them
again:

1. **A transcription typo in the duplicated literal pool.** The light prologue
   hand-copies `superscalarMul0`/`superscalarAdd1..7` into new labels
   (`superscalarMul0_light` etc.) so the light entry point's copy range is
   self-contained. Diffed against the original's literal pool
   (`src/jit_compiler_a64_static.S:896-903`) — byte-for-byte identical. Not the bug.
2. **A hardcoded stack offset in the shared prefetch/mix code.** The unmodified
   `prefetch`→`mix` block (`src/jit_compiler_a64_static.S:907-929`, shared between
   both variants, never touched by the Phase A change) does not reference `sp`
   anywhere — only x0-x13. The 112-byte-vs-80-byte frame size difference between the
   original and light prologues cannot affect this block. Not the bug.

If you find yourself re-deriving either of these from scratch, stop — they're closed.

## The flaw in the previously-proposed "hybrid" bisection — avoid repeating it

A prior review suggested localizing the fault by pairing the new light prologue with
the *original* full-size epilogue ("if that works, the bug is in the new epilogue;
if it still hangs, the bug is in the new prologue"). **This specific pairing is
invalid and must not be used as written.** The original epilogue restores x0-x13
from a 112-byte frame at fixed offsets (`ldp x0,x1,[sp]` at offset 0, ...,
`ldp x12,x13,[sp,96]` at offset 96, `add sp,sp,112`). The light prologue only
allocates 80 bytes and saves x4-x13 at different offsets (x4,x5 at offset 0, ...,
x12,x13 at offset 64). Splicing the two together means the epilogue reads stack
slots the prologue never wrote and deallocates 32 bytes more than were ever
allocated — a guaranteed `sp` drift on every call, for a reason that has nothing to
do with which half's *logic* is actually wrong. If that hybrid hangs (it likely
would), it proves nothing except that mismatched frame sizes are broken, which
nobody doubts.

**Any hybrid variant you construct must keep the frame size and save/restore
offsets internally self-consistent.** Vary only the specific thing under suspicion
within one matched, correctly-paired prologue/epilogue — never splice across two
different frame layouts.

## Step-by-step plan

### Step 1 — Data-flow diff (do this first; no devbox JIT run required to start)

This is the highest-value, lowest-risk next step, and does not depend on resolving
the bisection question below at all.

- Write a small standalone harness (host-side C++, can run on the x86_64 dev
  sandbox — this does not need AArch64 hardware) that computes `rl[0..7]` for a
  handful of fixed `(cache, itemNumber)` inputs using the exact same arithmetic as
  the prologue: `rl[0] = (itemNumber + 1) * superscalarMul0`, `rl[i] = rl[0] ^
  superscalarAdd_i` for i = 1..7.
- Compare against `generate_dataset_item()` / the equivalent reference path in
  `src/dataset.cpp` for the *same* fixed inputs, before the mix loop runs (i.e.
  compare the pre-mix `rl[]` values, not the final dataset item after 8 rounds of
  superscalar+mix — the goal here is to check the prologue's arithmetic in
  isolation, not the whole pipeline).
- If these match for the reference C++ implementation, the prologue's *arithmetic*
  is confirmed correct in isolation, and the bug — if there is one distinct from
  scheduling/register allocation — lives somewhere in how the JIT-generated
  superscalar program or the mix loop interacts with the reduced register set, not
  in the rl[] formula itself.
- If they diverge, you have a concrete, reproducible, host-side repro that doesn't
  need the devbox at all to keep debugging.

### Step 2 — Valid, frame-consistent bisection (only if Step 1 doesn't find it)

Construct two variants, each internally self-consistent (same frame size in its own
prologue and epilogue), changing only one axis at a time relative to the *original*
(not relative to each other):

- **Variant A — epilogue-only change.** Keep the *original* prologue completely
  unchanged (full x0-x13 save, 112-byte frame). Write a new epilogue that restores
  the same 112-byte frame but is otherwise structured like the light epilogue
  (e.g., reorder the restore, or drop the redundant x0-x3 restore into throwaway
  registers instead of skipping it structurally) — the point is to test whether
  merely disturbing the epilogue's shape, independent of the prologue's frame size,
  reproduces the hang. If this variant does NOT hang, the epilogue's *shape* is not
  the problem by itself.
- **Variant B — prologue-only change.** Keep the light prologue's actual 80-byte
  frame and computation exactly as attempted, but restore it with a *matching*
  80-byte epilogue that is otherwise as close as possible to a mechanical,
  unmodified transcription of the original's restore logic (same instruction
  ordering/style, just fewer pairs). This isolates whether the *specific* epilogue
  written in the first attempt had its own independent bug, separate from the
  prologue's register-dropping logic.
- Run each variant through the same verification the first attempt used (static
  `objdump`/`nm` check on the built binary, then dynamic disassembly of the live
  JIT buffer via `/proc/<pid>/mem` for one `--jit-dump` run) before running the full
  test suite, exactly as documented in the experiment doc — don't skip straight to
  `ctest` on an unverified build.

### Step 3 — If both variants hang

That would suggest the bug isn't in either half's internal logic at all, but in
something about the *interaction* between the reduced-frame light entry point and
the *shared* prefetch/mix/superscalar-program-insertion code that Step 1's static
check (which only covered the rl[0..7] formula, not the full 8-round mix
interaction) wouldn't catch. At that point, extend Step 1's harness to run the full
8-round superscalar+mix loop (not just the prologue's rl[] formula) against
`src/dataset.cpp`'s reference for the same fixed inputs — this directly tests the
"wrong final dataset item, not wrong instructions" hypothesis end-to-end instead of
only at the prologue boundary.

## Validation

Whichever step finds and fixes the actual cause, validate with:

1. Static + dynamic disassembly re-check (same method as the first attempt).
2. `armrx --jit-dump` for a single hash — must complete promptly (baseline: well
   under a minute, not 45+ minutes).
3. Full `ctest` suite (`ctest --test-dir build --output-on-failure`) — 100% pass,
   timings consistent with the historical baseline in
   `light-mode-dataset-item-prologue-attempt.md`'s revert-verification section.

## Success criterion

The light-mode entry point produces bit-identical dataset items to the original,
unmodified `rx_calc_dataset_item`, with a genuinely reduced register-preservation
footprint (fewer than the original's 7 `stp`/`ldp` pairs), and the full test suite
passes with no hang and no performance regression outside of Track C's own expected
win.

## If this attempt also fails

Revert cleanly (`git checkout --` the same three files), update
`docs/experiments/light-mode-dataset-item-prologue-attempt.md` with what this
attempt additionally ruled out (don't create a second competing doc), and leave
Track C blocked in `docs/plans/20260727/master-plan-20260727.md` rather than forcing
a partial or unverified fix through.
