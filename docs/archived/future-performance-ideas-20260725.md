# armrx — Future Performance Ideas (2026-07-25, superseded)

**Superseded same-day** by a more detailed pair of docs written independently on top of this
one: [`docs/plans/performance-plan-20260725.md`](../plans/performance-plan-20260725.md) (gated,
prioritized plan — a fleshed-out version of Section 1 below) and
[`docs/plans/experimental-performance-ideas-20260725.md`](../plans/experimental-performance-ideas-20260725.md)
(speculative backlog covering regions this doc didn't touch). This file's unique content
(Section 3's lower-confidence items) was merged into the experimental-ideas doc as items 13-15.
Kept here for historical reference only — **do not treat as current**, read the two docs above
instead.

**Status:** speculative backlog, not an active plan. Nothing here is scheduled or gated the way
`PLAN.md`'s Phase 7 items are — this is "if someone wants to keep pushing on performance, here's
where the evidence points," written at the point where the two most promising leads (peephole
JIT coalescing, the memory-op scheduler extension) were both closed out the same day, one on
evidence and one after a real regression.

> **Read this first — honest framing, same spirit as `docs/plans/performance-master-plan.md`'s
> own opening note, now doubly true.** By this point the codebase has been through six phases of
> profiling, three independent code reviews, three external audits, a worker-count sweep, a
> two-cluster interconnect discovery, opcode-level cycle attribution down to individual JIT
> regions, and a working (if modest) scheduler win. Nearly every code-level lead anyone has
> proposed has been tried and closed, several with hard-won negative results. **The remaining
> surface is genuinely small.** Don't re-open anything in `docs/archived/plan_phase6_completed.md`
> or `docs/experiments/`. If you're not sure whether something's already been tried, grep
> `changelogs.md` before spending device time on it — it almost certainly has.

---

## 1. The one concrete, quantified, still-open lead

The main per-hash VM program region carries ~9% of dynamic instructions but ~20% of mining
cycles — a **~2.2× IPC penalty** relative to the rest of the pipeline (`tools/jit_correlate.py`,
2026-07-25, see `PLAN.md` Phase 7 item 2). This is a real, measured, precisely-located stall
signature, almost certainly from the memory-operand opcodes' (`*_M`) genuinely random 64-byte
scratchpad reads. It is the single most concrete unaddressed number this project has produced.

**What's already been tried against it and failed:**
- Extending the emitter scheduler to treat `*_M` opcodes as swap triggers — caused a real
  JIT/interpreter divergence, reverted, mechanism not identified (see
  `docs/experiments/memory-op-scheduler-attempt.md`).

**What hasn't been tried:**

### 1a. Proper bisection of the memory-op scheduler hazard
The fastest path back into this lead if someone wants the scheduler win specifically. Use the
same technique that solved the *original* `src==dst` hazard: a global swap-count budget
(`ARMRX_MAX_SWAPS`-style env var, temporary/debug-only, never shipped) to binary-search which
exact swap first causes `test_jit_equivalence`'s `jit_equiv_seed_0`/`equivalence input 0_0` to
diverge. Budget real time for this — the two prior successful hazard discoveries both needed
real bisection, not just reasoning. A narrower variant (e.g. integer `*_M` only, excluding float
`*_M`; or excluding swaps where `Q`/`R` share any register with a nearby CBRANCH anchor even when
not required to) might dodge the hazard without needing to fully understand it, but this is a
guess, not a plan — verify empirically either way, given the failure mode is silent wrong hashes.

### 1b. Explicit software prefetch for scratchpad memory ops (different mechanism, not scheduling)
Instead of reordering *which* instruction executes when, emit an explicit `PRFM` at the point a
memory op's address becomes computable, ahead of the actual `LDR`. This is mechanistically
distinct from the scheduler (no emission reordering, no new hazard-model surface — a `PRFM` is a
hint, not a data dependency) and might sidestep whatever the reverted scheduler attempt hit
entirely. Caveat: RandomX's scratchpad addresses are only known at *runtime* (computed from a
register value, not compile-time-constant), so there's no "prefetch N iterations ahead" pattern
like a classic loop prefetch — the address is available at most a couple of instructions before
the load regardless. Whether there's enough of a window between address computation and use to
make a `PRFM` insertion worth the extra instruction is unverified; this needs a from-scratch
measurement, not an assumption. Item 10's prefetch tuning (`.Lmain_loop`'s `pldl1keep` hints) is
a different, already-closed lead (the *main loop's* own dataset-line prefetch, not per-instruction
`*_M` opcode prefetch) — don't conflate the two when picking this up.

### 1c. Just measure whether this is actually fixable at all
Given `*_M` addresses are register-dependent and genuinely data-random (that's RandomX's whole
ASIC-resistance design), it's possible this 2.2× IPC penalty is close to a hardware floor for
this workload on this core, not a code-quality gap. Before investing in 1a or 1b, it may be worth
directly measuring the *achievable* latency-hidden case (e.g. a synthetic micro-benchmark: N
independent scratchpad loads, various instruction-level parallelism the compiler/scheduler could
extract) to get a realistic upper bound on how much of this 2.2× is actually recoverable versus
inherent. If the achievable win is small, that's useful information before spending days on 1a/1b.

---

## 2. Blocked, not abandoned

**`--rt-priority` + `isolcpus=`/`nohz_full=`** (`PLAN.md` Phase 7 item 1) — needs `setcap`/root
(not currently available) and a kernel-cmdline edit + reboot (needs explicit user sign-off
regardless of privilege availability). Tempered expectations either way: this device's
6→8-worker degradation is a two-cluster interconnect-arbitration effect (`PLAN.md`'s "REVISED"
Phase 6 item 3 section), not scheduler-visible — CPU isolation was never going to touch that
specific mechanism. It might help the front-loaded 1→4-worker memory-contention component
marginally. Worth doing once device access is available, but temper the expected payoff.

---

## 3. Lower-confidence, not yet scoped

- **Re-run `devbox_pgo_build`** after any of the above lands meaningfully (`PLAN.md` Phase 6
  item 15 — already tracked, re-confirmed null twice, kept for re-evaluation on a reshaped
  binary).
- **Finer-grained instruction/cycle reconciliation.** `tools/jit_correlate.py` now explains
  ~86% of samples with high confidence (63.5% superscalar-attributed + 20% main-VM-program +
  2.4% superscalar-wrapper + 14% named C++, roughly). The remaining unattributed slice is small
  enough that further tooling investment here has a shrinking payoff — likely not worth it
  without a specific new hypothesis to test.
- **`-mtune=cortex-a53` / interpreted-path dataset prefetch / SIGSEGV-SIGBUS JIT-fault handler /
  windowed hash-rate reporting / oversubscription warnings / BOLT** — all previously flagged by
  external audits (Hermes, Gemini) as "not yet acted on, needs measurement" (`PLAN.md` Phase 6
  item 17). Still true. None have a specific evidence-backed reason to prioritize over the items
  above; pick one only if it maps to an actual pain point someone hits.

---

## 4. The honest recommendation

Given the density of investigation this codebase has already had, my actual recommendation is:
**don't chase further microarchitecture wins without a new, specific, measured hypothesis.**
Section 1's 2.2× IPC finding is the one lead with real evidence behind it, and even that comes
with a demonstrated correctness risk on the most obvious approach (scheduling). If the next
session's priority is genuinely "make armrx faster," start with 1c (bound the achievable win)
before committing to 1a or 1b's real implementation risk. If the priority is anything else —
feature work (Stratum V2), test coverage (`tls_client.cpp`/`tui.cpp`), or just shipping what's
here — that's a completely reasonable, evidence-backed choice too. Performance work reaching a
natural stopping point after this much investigation is a legitimate outcome, not a failure to
find more.
