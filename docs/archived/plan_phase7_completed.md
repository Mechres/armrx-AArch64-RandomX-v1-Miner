# armrx — Plan Phase 7 (completed items), 2026-07-25

Full narrative for Phase 7's closed-out items, split from `PLAN.md` 2026-07-25 once the phase's
completed history grew large enough to bury the one remaining open item (`--rt-priority`,
still tracked live in `PLAN.md`). Read `PLAN.md` for what's still open; read this for the
full why-and-how behind everything closed this phase.

---

## Item 2 — Peephole JIT coalescing: closed, deprioritized on evidence

([`docs/plans/peephole-jit-plan.md`](../plans/peephole-jit-plan.md)) — **closed 2026-07-25,
deprioritized on evidence, not just deferred.** The ~22% of cycles left unreconciled by an
earlier opcode-level correlation ("inside a worker buffer but outside any superscalar entry")
was narrowed by extending `tools/jit_correlate.py` to use the `code_size` boundary it already
parsed but never applied — splitting that bucket cleanly by whether an address falls before or
after the superscalar region starts. Two live `perf record` captures on the same 8-worker
mining workload (one `-e cycles`, one `-e instructions`) show the split precisely:

| Region | % of instructions | % of cycles | relative IPC |
|---|---|---|---|
| Main per-hash VM program (offset < CodeSize) | 9.23% | 20.04% | **0.461×** avg |
| Superscalar, opcode-attributed | 72.71% | 63.52% | 1.145× avg |
| Superscalar, unattributed (fixed wrapper chunks) | 2.49% | 2.38% | 1.046× avg |
| Outside any JIT buffer (named C++) | 15.56% | 14.05% | 1.107× avg |

**The main VM program region carries ~9% of dynamic instructions but ~20% of cycles — a
~2.2× IPC penalty relative to the rest of the pipeline.** This is a stall signature, not an
instruction-count signature: this region is where the memory-operand opcodes (`*_M`,
`ISTORE` — ~48% of this region's code bytes per the original static breakdown) live, doing
genuinely random 64-byte reads/writes into the 2 MiB scratchpad. Branch misprediction is
already ruled out separately (2.4% hot-path miss rate, ~0.1-0.16% of cycles). The superscalar
unattributed slice, once isolated, turned out proportionate (~1.0× IPC) — not a real lead at
all, just measurement noise from the old lumped-together bucket.

**Conclusion: this is evidence against peephole JIT coalescing, not just an unmet gate.**
Peephole's entire premise is code-density/instruction-count reduction; the one remaining
unexplained slice of cycles is disproportionately expensive *because of memory-latency
stalls*, which code-density reduction cannot fix. This is also the same conclusion every
single instruction-count-reduction attempt this project has tried has independently reached
(CSEL, Newton-Raphson, NEON-AES ×3, superscalar literal-pool relayout, `IMUL_RCP`
literal-load elimination — all implemented, measured, and reverted for exactly this reason).
Not starting the 3-6 week clean-room rewrite against evidence that specifically points away
from it. `tools/jit_correlate.py`'s region-split extension is kept as reusable diagnostic
infrastructure regardless of this outcome.

---

## Item 3 — `-frounding-math` compile option: done

Flagged by the Deepseek audit (Phase 6 item 19), confirmed missing via direct grep, added to
`CMakeLists.txt`. Verified: local x86 full suite 7/7, on-device targeted suite 5/5, no new
warning categories. Committed `059b6fe`.

---

## Item 4 — Defensive `ARMRX_ASSERT` for CBRANCH-with-unwritten-target-register: added, then removed

Also from the Deepseek audit (Phase 6 item 19), initially believed accurate by tracing the code
(`register_usage_[creg] == -1` wraps `pc` to `0` via `int16_t` truncation + `++pc`). Added to
`h_CBRANCH` in `src/vm.cpp`, committed `059b6fe`. Superseded by item 5's investigation below,
which found the audit's "theoretical only" premise was factually wrong — removed in `4da77f0`.

---

## Item 5 — Extend the emitter lookahead scheduler to hide memory-op latency in the main VM program: tried, reverted

**Tried 2026-07-25, caused a real JIT/interpreter divergence, reverted.** The natural follow-on
to item 2's finding above: the same *mechanism* that already produced a real, measured win for
the superscalar region's `IMUL_R`/`IMUL_RCP` stalls (reordering to hide long-latency-op stalls,
not reducing instruction count) was a plausible, specific, falsifiable hypothesis for the main
VM program's 2.2× IPC penalty too. Implemented: flagged the memory-load opcodes (`*_M`) as
`is_long_latency` in `computeFootprint()`, making them eligible as swap triggers (`P`) — no new
hazard-model change was believed necessary, since the existing memory-memory-always-hazard rule
already prevents a `*_M` op from ever swapping past another `*_M` op.

`test_jit_equivalence` failed immediately on its first (and most basic) seed/input pair — the
first time this test has failed in the project's history. Reverting the change alone (keeping
everything else) made it pass again, confirming the memory-op extension itself is the cause.
Investigated the mechanism at length: checked whether `emitMemLoad`'s own `src==dst`
shared-scratch-register special case (structurally similar to the *original* `src==dst` hazard
from the scheduler's initial design) was responsible — but that pattern was already reviewed by
three independent code reviews and confirmed self-contained regardless of position, so it
doesn't explain this. **The exact mechanism was not conclusively identified.** Given the failure
mode is silent wrong hashes and this was an explicitly speculative, "may be a null result"
experiment from the outset, the responsible choice was to fully revert rather than ship a
targeted exclusion without being able to verify it — matching this project's own standing rule
to trust empirical results over an unconfirmed theory, but here without a working fix to trust,
just a clean revert. `tools/jit_correlate.py`'s region-split extension (item 2) is unaffected
and kept. Full writeup: `docs/experiments/memory-op-scheduler-attempt.md`.

**Side fix, found and corrected while investigating**: the CBRANCH defensive `ARMRX_ASSERT`
added in item 4 fired repeatedly during this investigation on a completely normal
`test_jit_equivalence` run — its premise (that a CBRANCH targeting a never-written register is
"theoretical only") was factually wrong. Traced why the existing behavior is actually correct
and intentional (`register_usage_[creg]==-1` wraps `pc` to 0, i.e. "restart from VM instruction
0," matching the JIT's own `reg_changed_offset[]` reset to `PrologueSize` before every compile).
Removed the incorrect assert. Committed `4da77f0`.

**Performance work reached a natural stopping point this phase** — both the direct lead
(peephole JIT, item 2) and its evidence-backed follow-on (this item) were tried or ruled out.
The remaining open item is `--rt-priority` (still in `PLAN.md`, blocked on device access).

---

## Doc consolidation, same day (2026-07-25, folded in after this phase's items above closed)

Three planning docs for future performance work existed independently by end of day: this
assistant's `docs/plans/future-performance-ideas-20260725.md` (written first, short,
recommendation-focused) and a second agent's `docs/plans/performance-plan-20260725.md` (gated,
prioritized steps — a more detailed rewrite of the first doc's Section 1) plus
`docs/plans/experimental-performance-ideas-20260725.md` (speculative backlog covering regions
the first two didn't touch). Reconciled same-day: the first doc's unique content was merged
into the experimental-ideas backlog and the doc itself retired to
`docs/archived/future-performance-ideas-20260725.md`; a broken cross-reference in
`performance-plan-20260725.md` (pointed at the wrong sibling file) was fixed; a few claims in
the experimental-ideas doc were verified against source (Argon2 `MADV_POPULATE_WRITE` gap
confirmed real, visibility-flags and frame-pointer claims confirmed genuinely untried) and one
estimate (`double-buffered JIT`'s "~1.76% max") was re-labeled as an unmeasured upper bound, not
a result.
