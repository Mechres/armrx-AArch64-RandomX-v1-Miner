# armrx — Performance Plan (2026-07-25)

**Status: closed, 2026-07-26.** Step 1 ran and gave a small-effect result, which per this plan's
own gate closes Steps 2 and 3 without needing to attempt either. See "Step 1 — result" below.
The rest of this document is kept as-written (including the original, not-yet-run framing) so
the reasoning that led to each step is legible after the fact; only the "result" callouts are new.

**Status (as originally written):** honest, evidence-gated plan. No aspirational estimates —
every item below states what's known, what's unknown, and what evidence would green-light or
kill the next step.

**Background:** after six phases of profiling, three independent code reviews, three external
audits, a worker-count sweep, a two-cluster interconnect discovery, opcode-level cycle
attribution down to individual JIT regions, and a working (if modest) emitter scheduler win
(+0.233% IPC), the surface of genuinely unexplored performance leads is small. This plan
captures what's left, ranked by evidence quality, not by ambition.

> **This is the gated, prioritized plan.** For the full speculative backlog — ideas targeting
> regions *other* than the lead below, unmeasured and unscheduled — see
> [`docs/plans/experimental-performance-ideas-20260725.md`](experimental-performance-ideas-20260725.md).
> Nothing here duplicates that doc. (An earlier, now-superseded first pass at this material lives
> at [`docs/archived/future-performance-ideas-20260725.md`](../archived/future-performance-ideas-20260725.md).)

---

## The one quantified, still-open lead

From `PLAN.md` Phase 7 item 2 (the `jit_correlate.py` region-split extension):

| Region | % of instructions | % of cycles | IPC vs. avg |
|---|---|---|---|
| Main per-hash VM program | 9.23% | 20.04% | **0.461×** |
| Superscalar, opcode-attributed | 72.71% | 63.52% | 1.145× |
| Superscalar unattributed wrapper | 2.49% | 2.38% | 1.046× |
| Outside JIT buffer (C++) | 15.56% | 14.05% | 1.107× |

The main VM program region has ~2.2× worse IPC than the rest of the pipeline. This region
contains the memory-operand opcodes (`*_M`, `ISTORE` — ~48% of its code bytes) doing genuinely
random 64-byte reads/writes into the 2 MiB scratchpad. Branch misprediction is ruled out
(2.4% hot-path miss rate, ~0.1% cycle cost). This is a **stall signature**, not an
instruction-count signature — consistent with memory latency dominating this narrow region.

**What's been tried against this lead:**
- Memory-op scheduler extension — caused `test_jit_equivalence` divergence, reverted. Mechanism
  not identified ([`docs/experiments/memory-op-scheduler-attempt.md`](docs/experiments/memory-op-scheduler-attempt.md)).
- Every instruction-count-reduction approach (CSEL, Newton-Raphson, NEON AES ×3, superscalar
  literal-pool relayout, `IMUL_RCP` literal-load elimination) — all implemented, measured, and
  reverted or rejected because they reduce instructions but not stalls.

---

## Plan: gated, evidence-first, each step measures before committing to the next

### Step 1 — Bound the achievable win (low effort, no code change, gates everything downstream)

**Hypothesis:** not all of the ~2.2× IPC penalty is recoverable. The scratchpad accesses are
genuinely data-random (RandomX's ASIC-resistance design) — some of this may be a hardware floor
on this Cortex-A53, not a code-quality gap worth spending implementation time on.

**What to measure:** the *best-case* IPC for this region if latency were fully hidden. A
synthetic microbenchmark: run the existing main VM program binary, but with the scratchpad
replaced by a small preloaded buffer (fully L1-cache-resident), and compare IPC. The difference
between L1-resident IPC and real scratchpad IPC is the *recoverable* portion of the 2.2× gap.

**Gate:** the L1-resident IPC gives an upper bound on recoverable stall cycles. If the
difference is small (e.g., L1-resident IPC is still only 0.55× avg), there is very little room
to improve — the penalty is architectural (pipeline depth × memory latency), not fixable by
code changes. That's useful information: it means stop here, don't spend weeks on Steps 2-3.
If the difference is large (e.g., L1-resident IPC near 1.0× avg), proceed to Step 2.

**Effort:** ~1 hour. One devbox session, one code change (hoist scratchpad into L1 for the
benchmark only), one `perf stat` capture.

**Decision:** if recoverable gap is small → close this lead, document as hardware floor. If
large → proceed to Step 2.

**Result (2026-07-26): small — gate closed here, Steps 2-3 not attempted.**

Built the synthetic microbenchmark exactly as scoped: `bench_armrx --scratchpad-real` /
`--scratchpad-l1` (new flags; see `tests/bench_armrx.cpp`'s `bench_scratchpad_locality()` and
`AliasedScratchpad`). Rather than shrinking the scratchpad buffer itself (which would break the
JIT's compile-time address masks, which range up to the full 2 MiB), a small 16 KiB `memfd` is
tiled 128× across the same 2 MiB virtual range the JIT already computes offsets into — every
address the compiled program can produce lands on the same 16 KiB of physical memory, small
enough to be L1-resident, with zero change to the JIT compiler or its masking logic. A new
`VirtualMachine::run_execute_only()` (`src/vm.cpp`) re-invokes the last-compiled program directly,
isolating pure execute-against-scratchpad cycles from the JIT-compile step measured separately in
`ARMRX_JIT_PROFILE` builds. `perf stat -e cycles,instructions`, 2000 iterations each, `taskset -c 0`
(on-device, real hardware, not the interpreted x86 dev sandbox):

| Condition | Cycles | Instructions | IPC |
|---|---|---|---|
| Real 2 MiB scratchpad | 44,694,130,884 | 29,373,692,608 | 0.6572 |
| L1-aliased (16 KiB) | 42,135,298,579 | 29,373,699,356 | 0.6971 |

Instruction counts match to 5 decimal places (same compiled program, same iteration count) — a
clean, well-controlled comparison. Forcing the scratchpad to be effectively latency-free bought
only **+6.07% IPC** (cycles −5.73%). Per this step's own gate ("if the difference is small...
there is very little room to improve — the penalty is architectural, not fixable by code
changes"), **6% is small** against the ~2.2× IPC penalty this region carries overall. **The main
VM program's stall is therefore mostly not a memory-latency problem** — both Step 2 (`PRFM`,
which only helps if latency is the bottleneck) and Step 3 (bisecting the reverted memory-op
scheduler, which reorders emission to hide latency) target the wrong mechanism and are closed
without being attempted. The residual penalty looks architectural — in-order pipeline /
dependency-chain-bound on this Cortex-A53 — not something a code change can chase further.
**Caveat worth recording**: per-instruction `*_M` addresses in the real condition are already
masked to a 16 KiB (`kScratchpadL1Mask`) or 256 KiB (`kScratchpadL2Mask`) window depending on the
opcode's mod-mem bit (only the once-per-program `mx`/`ma` accumulator address uses the full 2 MiB
mask) — so the real baseline already has more locality than a naively "fully random 2 MiB" mental
model would suggest, which is consistent with (and likely part of why) the recoverable gap turned
out to be small rather than a methodology artifact.

---

### Step 2 — Prefetch-based approach: `PRFM` insertion for `*_M` opcodes (medium effort, mechanistically distinct from the reverted scheduler extension) — closed, not attempted (2026-07-26)

**Closed by Step 1's result**: `PRFM` only pays off if the stall is memory-latency-bound. Step 1
showed forcing near-zero latency (full L1 residency) recovers just 6% of the region's IPC
penalty — there isn't enough latency-bound stall left for a prefetch hint to hide. Not attempted.

**Hypothesis (gated on Step 1 showing a real recoverable gap):** explicit software prefetch
(`PRFM`) for scratchpad memory operands, inserted at the point an opcode's address register
value becomes available, might hide part of the stall without reordering instruction emission.
This avoids the scheduler's hazard-model surface entirely — a `PRFM` is a hint, not a data
dependency, so no correctness argument is needed beyond "the address register value is live."

**Caveats:** RandomX scratchpad addresses are only known at *runtime* (computed from a VM
register, not compile-time-constant). The address is typically available at most 1-2
instructions before the load. Whether there's enough of a gap between address computation and
the actual `LDR` for a `PRFM` to make a difference is unverified — this is a measurement
question, not a design question. On an in-order Cortex-A53, the `PRFM` itself costs an issue
slot, so the net benefit is `(stall reduction) − (issue slot cost)`. This may be zero or
negative.

**What to measure:** before integrating anything into the JIT compiler, measure the raw effect
of a `PRFM` before a scratchpad load on this core with a synthetic microbenchmark. If `PRFM`
reduces effective load-use latency by ≥2 cycles on this Cortex-A53, proceed to JIT integration.

**Gate:** if a single `PRFM` before a single random scratchpad load shows measurable latency
reduction (≥2 cycles), proceed to JIT integration. If not, close this approach and proceed to
Step 3.

**Effort:** ~2-3 hours. Synthetic microbenchmark first, JIT integration only if it shows a win.

**Correctness risk:** near-zero. `PRFM` is architecturally a hint — it never changes
architectural state. No hazard model needed. No scheduler interaction. `test_jit_equivalence`
should pass unchanged.

**Decision:** if `PRFM` shows measurable latency reduction → integrate into JIT for `*_M` ops,
re-measure `test_jit_equivalence` + `bench_armrx`. If not → proceed to Step 3.

---

### Step 3 — Bisect the memory-op scheduler hazard (high effort, high correctness risk, only if Steps 1-2 both show real room) — closed, not attempted (2026-07-26)

**Closed by Step 1's result**, same reasoning as Step 2: this step exists to let the emitter
scheduler reorder emission to hide memory latency. Step 1 showed there's only ~6% of IPC penalty
attributable to latency in the first place, which doesn't justify the high correctness risk
(silent wrong hashes) this step accepted in exchange for recovering it. Not attempted — the
already-small measured payoff from the existing (non-memory-op) scheduler (+0.233% IPC) was
already the practical ceiling reference point; this would have needed to clear a much higher bar
than 6% of a small region's penalty to be worth the risk.

**Hypothesis (gated on Steps 1-2 confirming a real recoverable gap that `PRFM` can't reach):**
the reverted scheduler extension's `test_jit_equivalence` failure has a specific, isolatable
cause — one exact swap that triggers divergence. Bisecting to find it, then writing a targeted
exclusion rule (similar to the `src==dst` and superscalar `IMUL_RCP` exclusions that worked
for the existing scheduler), would allow the memory-op extension to ship safely.

**Why this is last:** the two previous scheduler hazard discoveries (`src==dst`, superscalar
`IMUL_RCP` ordering) both required real bisection time with an `ARMRX_MAX_SWAPS`-style env
var — finding one exact swap that triggers divergence across a 2047-instruction program. This
is not a reasoning exercise; it's a binary-search session. The failure mode is silent wrong
hashes, so any fix must be *demonstrated* safe, not just argued safe on paper.

**Method:** use the same technique that solved the original `src==dst` hazard — a global
swap-count budget (temporary/dev-only `ARMRX_MAX_SWAPS` env var, never shipped) to
binary-search which exact swap first causes `test_jit_equivalence`'s `jit_equiv_seed_0` /
`equivalence input 0_0` to diverge. Once the specific swap is identified, design a targeted
exclusion rule, verify it eliminates the divergence, and integrate it into `scheduleProgram()`.

**Variants to try before full bisection** (each independently testable, may dodge the hazard
without needing to understand it):
- Integer `*_M` only (exclude float `*_M`: `FADD_M`/`FSUB_M`/`FDIV_M`).
- Only when the swap window has zero register overlap with any nearby CBRANCH anchor.
- Only when the swap window boundary (`i`/`i+3`) aligns with existing CBRANCH domain
  boundaries (i.e., never cross a domain even indirectly via the `i+=3` stride straddling it).

**Effort:** unknown — could be 2 hours (easy bisection) or 2 days (stubborn hazard pattern).

**Correctness risk:** high. The existing scheduler's hazard model was exhaustively reviewed by
three independent auditors and the existing stress tests pass. The memory-op extension broke
that. Any new exclusion rule extends the hazard surface — it needs the same review rigor as the
original design, plus a new dedicated stress test.

**Decision:** if bisection finds a working exclusion → adopt, with a new stress test and a new
review. If bisection fails or the hazard proves intractable → close this lead permanently, as
not worth the correctness risk given the already-small measured payoff from the existing
scheduler (+0.233% IPC).

---

### Step 4 — Blocked, not abandoned: `--rt-priority` + `isolcpus=`/`nohz_full=`

**Status:** blocked on manual device access (`setcap`/root not available, kernel-cmdline edit
needs reboot + explicit user sign-off).

**Hypothesis:** real-time scheduling + CPU isolation reduces OS-induced jitter (timer
interrupts, scheduler ticks, RCU callbacks) that occasionally stalls worker threads during
scratchpad access. On a memory-latency-bound workload on an in-order core, even occasional
interrupt-driven evictions of the micro-TLB or L1 cache lines could have disproportionate cost.

**Tempered expectations:** the device's 6→8-worker degradation is a hardware interconnect-
arbitration effect (confirmed in Phase 6 item 3), not scheduler-visible. CPU isolation was
never going to touch that mechanism. It *might* help the front-loaded 1→4-worker memory-
contention component marginally. **Correction (2026-07-26, per Step 1's result above): the 2.2×
IPC penalty in the main VM region is mostly *not* a data-latency problem after all** — this
paragraph's original assumption was wrong; forcing the scratchpad to be effectively latency-free
only recovered 6% of IPC. `isolcpus=` doesn't make DRAM
faster.

**Effort:** ~1 hour once device access is available (one `setcap`, one `cmdline` edit, one
reboot, one `bench_armrx` run with `--rt-priority`). Do not block other work waiting for this.

**Decision:** do when device access is available, with tempered expectations. The upside is
small; the main value is closing the question definitively.

---

## What this plan deliberately does NOT include

- **Peephole JIT coalescing** — closed on evidence, not deferred. The region-split IPC finding
  shows the remaining gap is stall-dominated, not instruction-count-dominated. Peephole's
  entire premise is code-density reduction; it can't fix stalls.

- **Any instruction-count reduction** (CSEL, Newton-Raphson, NEON AES variants, literal-pool
  relayouts, `IMUL_RCP` load elimination) — all implemented, measured, and either reverted or
  rejected. The pattern is consistent: fewer instructions don't help a stall-bound workload.
  Don't re-open any of these without a specific new measurement showing the stall profile has
  changed.

- **PGO re-check** — re-measured twice (Phase 5, Phase 6), both times null on current code.
  The tooling is kept (`devbox_pgo_build`); re-run only if a substantial code change (e.g.,
  Step 2's `PRFM` integration) reshapes the binary enough to make the profile data non-
  redundant.

- **Any approach that requires reordering memory ops past each other** — the existing
  memory-memory-always-hazard rule exists for a reason (dynamic scratchpad addresses,
  non-provable aliasing at compile time). Loosening it would need a fundamentally different
  safety argument that doesn't currently exist.

---

## Overall recommendation

**Start with Step 1.** It's the cheapest way to bound expectations — if the recoverable gap
is small, the entire rest of this plan shrinks to "document the hardware floor and move on."
That's a completely legitimate outcome after six phases of profiling.

If Step 1 shows real room, **Step 2 (`PRFM`) is the safer next move** — mechanistically
distinct from the reverted scheduler extension, near-zero correctness risk, and directly
measurable without integration first.

**Step 3 (bisect the scheduler hazard) should be the last resort** — high effort, high
correctness risk, and only makes sense if Steps 1-2 have already demonstrated real recoverable
gains that a pure `PRFM` approach can't reach.

Performance work reaching a natural stopping point after this much investigation — even at
Step 1 — is a legitimate outcome, not a failure. The codebase has already had more profiling
depth than most production mining software ever gets; closing a lead with "hardware floor" is
useful knowledge for anyone reading the logs later.

## Outcome (2026-07-26)

This is exactly what happened. Step 1 ran, showed a small (6%) recoverable gap, and per its own
gate that closed Steps 2 and 3 without needing to attempt either — the natural stopping point
this section anticipated. No genuinely open performance lead remains as of this writing. See
`PLAN.md`'s Phase 9 entry and `NEXT_STEPS.md` for the pointer into this result from the
project-level trackers.
