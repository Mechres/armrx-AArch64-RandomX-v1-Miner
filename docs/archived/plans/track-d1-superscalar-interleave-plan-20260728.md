# Track D1: 2-Way Interleaved Superscalar Dataset-Item Derivation — **CONCLUDED: NEGATIVE**

**Status: concluded (2026-07-29). Clean negative result — the 2-way path
causes ~66× more L1I refills than the single-stream baseline on this
Cortex-A53. Do not wire into production mining. D3 is also contraindicated.**

Full measurement data and analysis: `docs/experiments/track-d1-bench-1way-hang-status.md`.

## Retrospective

What happened: the 2-way JIT path was implemented (as `JitDataset2Way` in
`src/jit_dataset_2way.cpp` + `src/jit_compiler_a64_static.S`), verified
by exhaustive differential KAT (`test_jit_dataset_2way`, 40020 item
derivations across 20 seeds), and benchmarked against the 1-way baseline
to measure the L1I cache impact. The gate measurement showed a ~66×
increase in `l1i_cache_refill` (0.023% → 1.51% miss rate) with a −1.2% IPC
regression.

The core insight was correct — two independent nonces are fully independent
and interleaving them at emission time cannot create new hazards — but the
I-cache locality tradeoff is worse, not better, on the in-order Cortex-A53
with its 16 KB L1I. Each branch in the interleaved code causes the next
stream's instructions to displace the previous stream's cache lines,
creating an access-pattern problem that code-size reduction cannot fix.

**This is a clean negative result that directly informs D3: if the cheap,
simple superscalar interleave can't clear this bar, full dual-nonce
interleave of the entire VM program (which is larger and even more
branch-heavy) is extremely unlikely to.**

## Original plan (retained for reference)

The text below is the working plan as written before implementation.
It is kept for traceability but should not be followed.

## What makes this categorically safer than the reverted memory-op scheduler attempt

`docs/experiments/memory-op-scheduler-attempt.md` failed because it *reordered*
instructions within a single dependency chain, which requires proving new hazards
don't exist across the reordering window — exactly the kind of proof that broke
silently and was never root-caused. **D1 does not reorder anything.** It emits
each superscalar instruction *twice*, once per disjoint register set, for two
already-independent nonces. Each stream's internal order is left exactly as the
existing (already-verified) scheduler produces it. The only new correctness
question is "do the two streams' register/memory footprints actually stay
disjoint" — a static, checkable-by-construction property, not a dynamic hazard
proof.

## Precedent already in this repo

`src/dataset.cpp:88-135` already does the same 2-way interleave in C++/NEON for
fast-mode dataset initialization. Read that code first — it's a working,
KAT-verified example of the exact interleaving pattern this item asks for, just in
a different execution model (C++/NEON vs. hand-emitted AArch64 JIT).

## Where this lives and the register budget

The light-mode loop that currently does one dataset-item derivation per iteration
is at `src/jit_compiler_a64_static.S:528-560`. RandomX's one-iteration lookahead
(the current and next dataset-item address are already simultaneously live in
this loop) exists precisely so implementations can do exactly this.

Register budget: the existing single-stream derivation already commits x0-x13 (14
GPRs: the 8-register output/working set x0-x7, plus x8-x13 as cache-pointer/
output-pointer/registerValue/mixBlock/scratch — see the header comment at
`src/jit_compiler_a64_static.S:836-844` for the exact role of each). A second,
fully disjoint stream needs its own ~8-10 GPRs. AArch64 gives you x19-x28 as
additional callee-saved registers not currently used by this function at all —
budget ~20 total registers for two streams, fitting by extending the callee-saved
set used by a new, dedicated 2-way entry point (do not touch the existing
single-stream entry point; add a new one, so the single-stream path stays
provably unchanged for anything that doesn't opt in).

## The gate that actually decides this

**Code size roughly doubles**: the current derivation code is part of a ~20,916-byte
JIT buffer; a 2-way version is expected to land around ~42 KB, against this A53's
16 KiB L1 I-cache. This is the single most likely failure mode — not a
correctness bug, a locality regression. **Measure `l1i_cache_refill` before and
after** (same PMU-event methodology Track A item 2 already used on this device for
front-end/back-end stall attribution — reuse that measurement setup rather than
inventing a new one). If I-cache refills rise enough to erase the gain from
filling pipeline bubbles, that's a real, valid negative result — don't chase
further code-size reduction hoping to rescue it without first understanding why
the interleave and the I-cache cost trade off the way they do.

## Step-by-step plan

### Step 1 — Design the 2-way entry point

- Add a new static entry point (parallel to, not replacing,
  `randomx_calc_dataset_item_aarch64`) that takes two `(cache, itemNumber)` pairs
  and produces two independent 64-byte dataset items.
- Emit each superscalar-program instruction twice — once targeting stream A's
  register set, once targeting stream B's (extended into x19-x28) — interleaved at
  emission time so the in-order A53 pipeline sees alternating independent
  instructions instead of one long serial chain.
- Keep each stream's own internal instruction order exactly as the existing
  scheduler already emits it for the single-stream case — this item changes
  *which* instructions are interleaved with which, not the order within either
  stream.

### Step 2 — Static verification before running anything

- `objdump`/`nm` the compiled binary: confirm both streams' register sets never
  overlap anywhere in the emitted range, and that any PC-relative branches this
  new entry point contains (if it needs its own literal pool, following the same
  copy-and-splice mechanism documented in
  `docs/experiments/light-mode-dataset-item-prologue-attempt.md`'s Context
  section) resolve correctly under the same textual-adjacency constraint that
  documents already explains for the light-mode entry point.

### Step 3 — Correctness: exhaustive differential test

- The derivation is a pure function of item number — write a differential test
  comparing the 2-way path's output for many `(itemNumber_A, itemNumber_B)` pairs
  against the existing, proven single-stream path run twice. This oracle is total
  and cheap; there's no reason to ship this without running it exhaustively (in
  practice: thousands of pairs, not a handful).

### Step 4 — The actual gate: L1 I-cache refill rate

- On the devbox, `perf stat -e l1i_cache_refill` (or whatever this A53's exact
  event name resolves to — confirm against the same PMU event list Track A item 2
  already validated works on this hardware) before and after, `taskset`-pinned,
  long window, reversed trial order — full measurement discipline per the master
  plan's §4.
- If refills increase enough to offset the interleaving win, record that as a
  clean negative result and stop — do not proceed to wiring this into production
  mining code on a regression.

### Step 5 — Wire into the mining path only if Step 4 is a net win

- Only if the I-cache gate clears: wire the new 2-way entry point into actual
  2-nonce-at-a-time mining (this requires the caller to actually process two
  nonces concurrently, which is a separate integration question from the JIT
  emission itself — scope that as a follow-on once the core hypothesis is proven,
  don't build the integration before the gate clears).

## Validation

1. Differential test vs. single-stream path, exhaustive, green.
2. Full `ctest` suite green.
3. `l1i_cache_refill` measured before/after, `taskset`-pinned, long window,
   reversed trial order, written up with real numbers.

## Success criterion

A measured net hashrate improvement from processing two nonces' dataset-item
derivation together, with the I-cache refill rate either flat or improved enough
that it doesn't erase the gain. This result also directly informs whether D3 (the
much larger, much riskier full dual-nonce interleave) is worth attempting at all —
report that implication explicitly regardless of which way D1 comes out.
