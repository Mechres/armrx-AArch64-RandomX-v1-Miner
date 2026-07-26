# armrx — Mid/High-Risk Performance Ideas (2026-07-26)

**Status:** tracking doc for real-correctness-risk performance work, opened after
`docs/plans/experimental-performance-ideas-20260725.md` (low-risk backlog) was worked to full
closure the same day. Everything here carries genuine silent-wrong-hash risk if done carelessly —
treat each item with the same rigor the JIT scheduler's original hazard model got (three
independent code reviews before being trusted).

Ranked by how well each is actually supported by evidence gathered so far, not by ambition.

---

## Tier 1 — most evidence-aligned

### 1. Bisect and fix the reverted superscalar `IMUL_RCP` register pre-assignment (idea #1)

**Status: CLOSED for good (2026-07-26) — root cause found, payoff too small at the safe scale.**

Full account in `docs/experiments/superscalar-imul-rcp-preassignment-attempt.md`. Bisected using
a temporary env-var cap (0-12 registers) on the original attempt: root cause is that
`randomx_calc_dataset_item_aarch64` is called via `bl` from *inside* the main program's own
light-mode JIT body, and that call site doesn't protect x14/x15 (the main program's own live r6/r7
VM registers) or x21-x28 (the main program's own pre-loaded `IMUL_RCP` literals) — the original
code has always been correct only because it never happens to write to those registers. Of the
originally-planned 12 registers, only **x19** empirically works as a sole preassigned register;
x20 (also nominally "temporary") fails too, for a reason not fully pinned down. With a real safe
budget of at most 1 register (lightly validated), the achievable win is far too small to justify
the ongoing correctness burden of a register-allocation-fragile optimization in the JIT's most
safety-critical path. **Not carried forward — closed permanently, not left open for a future
attempt.**

### 2. Widen the main-VM-program scheduler's swap window for the already-adopted long-latency-op class

**Status: not started.**

The existing scheduler only reorders within a fixed 3-instruction lookahead
(`scheduleProgram()`/`scheduleSuperscalarProgram()` in `src/jit_compiler_a64.cpp`). Step 1
(`docs/experiments/scratchpad-locality-bound-20260726.md`) showed the main VM program's ~2.2× IPC
penalty is architectural (dependency chains / in-order pipeline depth), not memory-latency —
exactly the class of problem a wider scheduling window addresses. Restricted to the same
already-reviewed, already-safe long-latency-op category (`IMUL_R`/`IMULH_R`/`ISMULH_R`/
`IMUL_RCP`) — explicitly *not* extending to memory ops, which is the specific extension that
already failed once for an unidentified reason (`docs/experiments/memory-op-scheduler-attempt.md`).

**Risk**: medium — extends an already-reviewed scheduler into new hazard combinatorics (a wider
window means more possible interleavings to reason about), but stays within a category that's
already proven safe at the 3-window scale.

---

## Tier 2 — legitimate but weaker alignment with evidence

### 3. BOLT (post-link profile-guided binary layout)

**Status: not started.**

Uses real `perf`-recorded profiles (not compile-time instrumentation like PGO) to reorder hot
functions/basic blocks for better I-cache locality. Genuinely untried in this project. **Expected
to replicate PGO's null result** — I-cache miss rate is already measured at 0.788%
(`docs/plans/experimental-performance-ideas-20260725.md` idea #8), so there isn't much
instruction-layout locality left to win back. Worth a quick try only if someone wants to close the
question definitively, not because the evidence points at a real gap here.

---

## Tier 3 — bigger, more speculative redesigns

### 4. A real dependency-graph list scheduler for the main VM program

**Status: not started.**

Replace the current fixed 3-window heuristic with genuine list scheduling over a full
per-program dependency graph. This is the "textbook correct" answer to Step 1's architectural
finding, but amounts to building a compiler backend scheduler from scratch. High effort, high
risk (silent wrong hashes), would need review rigor matching or exceeding the original scheduler
(three independent audits). Not worth starting before Tier 1/2 are resolved.

### 5. Register-allocation restructuring in the main VM program's JIT

**Status: not started.**

Reduce false WAW/WAR dependencies between virtual registers by allocating more physical registers
per virtual register (avoiding unnecessary serialization). Plausible but genuinely unexplored — no
concrete measurement yet points at this specifically as the bottleneck. Speculative.

---

## How to use this file

Work top to bottom unless a specific new measurement redirects priority. Tier 1 items have a
concrete, evidence-backed reason to try; Tier 3 items are architecturally sound but should wait
until there's a specific reason to believe the effort will pay off (e.g., if item 1 or 2 reveals
something that makes a full scheduler rewrite look more tractable or more necessary).
