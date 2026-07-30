# armrx — Hail Mary Round 2: Concurrency, Scheduler-Optimality, and Dead-End Records (2026-07-27)


*** Written by:*** Claude Sonnet 5

**Status: brainstorm only, not gated, not prioritized, not promised.** This is a *second* batch,
written to not overlap the first. Round 1 (`hail-mary-ideas-20260727.md`, 15 ideas across
architectural/topology/toolchain/"out there" categories — hybrid JIT/interpreter, per-cluster
cache replication, partial fast mode, conservative load hoisting, etc.).

**Ground truth this doc builds on** (full derivation in `docs/experiments/scratchpad-locality-bound-20260726.md`
and `docs/plans/hail-mary-ideas-20260727.md`, not re-derived here):
- Main VM program region: 9.23% of instructions, 20.04% of cycles, 0.461× IPC. ~94% of that
  penalty was shown to be architectural (dependency chains / in-order pipeline depth), not
  memory latency. Superscalar region (72.71% of instructions) is already efficient at 1.145× IPC.
- The emitter lookahead scheduler (`jit_compiler_a64.cpp` ~L220-670) reorders emission within a
  window to hide multiply latency; widened once already for +0.156% IPC
  (`docs/experiments/main-scheduler-window-widening-20260726.md`). Extending it to memory-load
  opcodes was tried and reverted (unexplained JIT/interpreter divergence).
- `IntRegMap[8] = {4, 5, 6, 7, 12, 13, 14, 15}` (jit_compiler_a64.cpp:133) maps RandomX's 8
  integer registers to x4-x7, x12-x15. Plus x9-x11 (address/temp scratch), x19/x20 (shared
  scratch, src==dst hazard case), x10 (scratchpad address), x2 (memory base pointer) are all
  live at various points. That's already 12-14 of the 31 GPRs committed before touching the
  float register groups (a/e/f, held in vector registers). This number matters for Category E below.

The two questions round 1 didn't really ask: **(1) is the "94% architectural" ceiling actually
proven optimal, or just "the current heuristic scheduler can't do better"?** and **(2) if a
single hash's dependency chain is truly unschedulable, is there independent work sitting right
next to it — a *different* hash — that could fill the same bubbles?** Everything below follows
from those two questions.

---

## Category E: Concurrency — filling in-order bubbles with a *second* hash

Round 1's D9/A2 tried to find independent instructions *within* one hash's dependency chain to
fill pipeline bubbles, and kept hitting the wall that the chain is genuinely serial (that's the
94% architectural finding). But two different nonces' hash computations are **already fully
independent of each other** — no hazard analysis needed between them, only within each one
(which is already correctness-verified). If the JIT interleaved the native code of two hashes
instead of one, an in-order dual-issue core could execute one hash's ALU op and the other hash's
independent ALU/load op in the same cycle, exactly filling the bubbles that a single hash leaves
empty. This is "poor man's SMT" — done by the compiler at emission time, not by hardware.

### E1. Full dual-nonce interleaved JIT emission

**The idea:** For a worker processing nonces N and N+1 back to back anyway, generate both
programs (they're regenerated per-hash regardless — RandomX's program bytecode itself depends
on nonce-derived state, not just the data it operates on), then **co-emit** their native
instruction streams into one buffer, interleaved instruction-for-instruction (or in small
groups), each stream keeping its own internal order intact. Two independent instruction streams
scheduled together let the A53's 2-wide in-order issue actually reach 2-wide, instead of stalling
on stream A's dependency while stream B sits uncompiled and unavailable.

**Why this is a genuinely different risk profile than A2/D9 in round 1:** Those ideas needed to
prove properties *about* a single dependency chain (address equivalence, hazard-free reordering)
— exactly the class of proof that broke the memory-op scheduler silently. Here, no such proof is
needed: each stream's internal instruction order is untouched (still whatever the existing
verified scheduler produced), so each stream in isolation still computes the exact hash it always
did. The only new mechanical requirement is that the two streams don't clobber each other's
registers or memory — a resource-allocation problem, not a correctness proof about semantics.
Correctness reduces to "did we allocate registers/scratchpad without collision," which is
checkable exhaustively, not "did we prove a subtle equivalence."

**The catch — register pressure:** Per the ground truth above, one stream already occupies
~12-14 GPRs. Two full streams need ~24-28, leaving almost nothing for the interleaving logic
itself (loop counters, base pointers for two separate scratchpads/memory regions). This is tight
but maybe not impossible:
- x0-x3, x21-x30 are mostly still free (base pointers, LR, FP) — call it ~10-12 spare GPRs, not
  quite enough for a second full RandomX register file (8 int regs + address temps).
  A real prototype would need to either (a) spill some of stream B's registers to a small stack
  frame between uses (adds loads/stores, eating into the gain), or (b) find that RandomX
  programs rarely use all 8 integer registers live-simultaneously in practice (register
  liveness analysis on real compiled programs would answer this — check before building
  anything).
- The vector register file (32× 128-bit) holds the float groups (a/e/f = 12 registers per
  stream). Two streams need 24 of 32 — tighter, but survivable since NEON isn't otherwise
  contended in this region.
- Two scratchpads (2 MiB each) and two dataset-cache read streams double the DRAM/L2 bandwidth
  demand per worker. On a device with 8 workers already contending, this could just move the
  bottleneck from ALU stalls to memory bandwidth — the exact failure mode B1 in round 1 worried
  about, but self-inflicted instead of interconnect-inflicted.

**Expected effect:** If it works, this is the only idea across both rounds that could plausibly
close a meaningful fraction of the "94% architectural" gap, because it's the only one that
doesn't need the single-stream dependency chain to become less serial — it hides the serial
chain's bubbles with *unrelated* work instead. Call the ceiling "up to most of the main VM
program's 20% cycle share" in the best case, likely far less once register-spill and memory-
bandwidth costs are counted.

**Why it might fail:** Register spilling could add enough instructions to erase the IPC gain
(this is the same trap that sank round 1's A1 hybrid-interpreter idea — recompilation/dispatch
overhead eating a real IPC improvement). Also, this only helps throughput *per worker*; if a
worker is already only bound by 1 core, doubling its work-in-flight without doubling issue slots
just changes which resource saturates first.

**Effort:** This is the largest item in either round — realistically 1-2 weeks for a prototype
(needs: dual program generation already exists trivially, a register-collision-free dual
allocator, a two-scratchpad/two-memory-base-pointer calling convention, and full KAT + JIT-vs-
interpreter equivalence testing doubled up). Not a "measure in an afternoon" item.

**Risk:** Medium, structurally lower than round 1's riskiest items (no semantic proof needed),
but high in a different way — this is a substantial rewrite of the JIT's calling convention and
register allocator, which is the most load-bearing and best-tested part of the codebase. A bug
here risks silent wrong hashes for a completely different reason (register collision between the
two streams), which is just as bad as the memory-op scheduler's failure mode even if the *cause*
is easier to reason about in advance.

**First thing to check before writing any code:** register liveness in real compiled RandomX
main-program instances — dump how many of the 8 int + 4×3 float registers are simultaneously
live at each instruction across a sample of real programs. If it's routinely close to 8/8 (no
slack), E1 is dead on arrival for register-pressure reasons alone and shouldn't be prototyped.
This check is cheap (a script over `jit_correlate.py`'s existing instrumentation or a one-off
liveness pass) and should gate everything else in this section.

### E2. Cross-hash pipelining at the boundary only (lighter-weight version of E1)

**The idea:** Instead of interleaving two *entire* hash programs (E1's register-pressure
problem), only overlap the **tail of hash N** (AES finalization / result compression — a short,
fixed, low-register-pressure sequence) with the **head of hash N+1** (scratchpad fill from the
new tempHash / blake2b — also short and mostly load/store, low ALU register pressure). These are
the two places in the per-hash pipeline that are shortest and least register-hungry, so
interleaving just this boundary needs far fewer spare registers than a full E1 interleave, at
the cost of only hiding bubbles in a smaller fraction of the pipeline.

**Why this might be worth doing even if E1 is dead:** If the liveness check above kills E1, this
is the fallback that asks the same question at a scale where the register math might actually
close. The finalization sequence (AES rounds over the register file, no data-dependent branches)
and the scratchpad-fill sequence (blake2b-derived writes, no VM register dependencies at all) are
both close to embarrassingly parallel with the *next* hash's corresponding start/end, since
neither touches the RandomX integer/float register file the main program uses.

**Expected effect:** Much smaller ceiling than E1 (only 2 short pipeline stages, not the whole
9.23%-instruction main-program region), but also much smaller register-pressure risk. Realistic
target: a few percent of the main-program region's cycle share, i.e. a fraction of a percent of
total hashrate — worth measuring, not worth over-promising.

**Effort:** 3-5 days for a prototype restricted to just the finalization/fill boundary.

**Risk:** Low-medium. Both sequences being overlapped are already independent of the VM register
file, which is the main thing that made E1 risky.

---

## Category F: Is the scheduler actually at its ceiling, or just at *its* ceiling?

Round 1 treated "94% architectural" as settled. It was proven relative to *memory latency*
(scratchpad-locality experiment) — but that's a different claim than "the current emitter
scheduler already extracts all the schedulable slack from the true dependency chain." Nobody
has directly checked whether the *scheduler itself* (as opposed to the underlying dependency
chain) is leaving cycles on the table.

### F1. PMU STALL_FRONTEND vs STALL_BACKEND breakdown on the main VM program region

**The idea:** Cortex-A53's PMU exposes `STALL_FRONTEND` (0x23) and `STALL_BACKEND` (0x24) events
(architectural Armv8 PMU events, implemented on A53 — worth confirming against this specific
device's `perf list` output before relying on the numbers). If the "94% architectural" cycles are
actually front-end stalls (branch misprediction recovery, I-cache miss, fetch bubbles) rather
than back-end stalls (waiting on the ALU/MUL pipeline for a dependency), the entire framing of
"dependency chain bound" needs revisiting — front-end stalls point at completely different fixes
(branch prediction, code layout — closer to round 1's C3) than back-end stalls do (which is where
E1/E2 above would actually help).

**Why this wasn't done already:** The CBRANCH-31%-myth-busted work (see memory) measured branch
*miss rate*, not stall-cycle attribution — a low miss rate doesn't rule out stalls from other
front-end causes (I-cache misses specifically), and a correctly-predicted branch still costs a
bubble on an in-order core if the target isn't fetched in time.

**Expected effect:** This is a diagnostic, not an optimization — it either redirects effort
toward a front-end fix (cheap, well-trodden: code layout, alignment, branch hints) or confirms
back-end/dependency-chain attribution (which validates E1/E2's premise and rules out chasing
front-end fixes further).

**Effort:** Half a day on the devbox: `perf stat -e stall_frontend,stall_backend,cpu-cycles
-C <core>` pinned around a bench run, cross-referenced against `jit_correlate.py`'s existing
offset-to-region mapping to isolate the main-program region specifically.

**Risk:** None — pure measurement.

### F2. Exact A53 dual-issue-slot scheduler (replace the heuristic window with a real issue model)

**The idea:** The current emitter scheduler reorders within a fixed-size window based on a
register-hazard model — it doesn't model the A53's actual issue restrictions (which specific
pairs of instruction *types* can co-issue in the same cycle; Cortex-A53 is 2-wide but not every
pair of ops can go in both pipes — one pipe handles branch+simple-ALU, the other handles
ALU+MUL+DIV+NEON, per the Cortex-A53 Software Optimization Guide). A hazard-clean reordering
that's legal by the register model can still be issue-slot-illegal or simply suboptimal if it
doesn't account for which pipe each instruction needs.

**What changes:** Build a small issue-slot simulator using the A53 SOG's per-instruction
latency/pipe tables, and replace (or augment) the window-based heuristic with a real list
scheduler that tracks pipe occupancy, not just register hazards. This is strictly a scheduling
change — it doesn't alter program semantics any more than the existing scheduler does, since
both only reorder already-hazard-checked emission order.

**Expected effect:** If the current window-heuristic scheduler is already near-optimal for the
true dependency chain, this finds nothing new (a real, useful negative result — closes the
question this doc opened). If it's not, this is the most defensible remaining lever on the "94%
architectural" number, because it doesn't require finding new independent work (E1's job) — it
just uses the existing, single-stream independent work more precisely than a fixed-size register-
hazard window does.

**Effort:** 1-2 weeks (building an accurate-enough A53 pipe model is the hard part; the SOG
gives latency/throughput tables but exact dual-issue pairing rules require care to get right,
and getting them wrong risks a *slower* schedule, not a wrong one — low correctness risk, real
effort risk).

**Risk:** Low on correctness (same hazard-safety net as today, just a smarter search over legal
orderings), moderate on wasted effort if the answer turns out to be "the existing heuristic was
already close to optimal."

**Do this before E1/E2** if effort is being rationed — it's cheaper, safer, and answers whether
the whole premise of "hunt for more overlap" (which E1/E2 also rest on) has any room left in a
single stream before reaching for a second one.

### F3. NEON-pipe multiply offload for IMUL_R/IMUL_RCP (speculative, likely negative)

**The idea:** A53's NEON/FP pipe is separate from its integer ALU pipe. If `IMUL_R`/`IMUL_RCP`
could execute via a NEON integer multiply instead of the scalar `MUL`/`UMULH`, the integer ALU
pipe would be free to do address computation for the *next* instruction in the same cycle.

**Why it's probably a net loss:** Moving a value between the general-purpose and vector register
files (`FMOV`/`INS`/`UMOV`) costs real latency on A53 (on the order of several cycles each way per
the SOG's cross-domain-transfer figures) — likely more than the single-pipe stall it's trying to
avoid. This is the same "overhead eats the gain" trap as round 1's A1 and this doc's E1/E2.

**Effort to falsify:** Half a day — this is cheap enough to just measure the cross-domain-move
latency in isolation on the devbox before touching the JIT, and kill it on paper if the number is
bad (it probably is).

**Risk:** None if killed at the measurement stage; low-medium if prototyped further (same class
of correctness risk as any JIT codegen change, but a small and easily-tested one).

---

## Category G: Dead ends worth recording so nobody re-derives them

### G1. Early-abort on partial hash vs. difficulty target

**The idea that keeps coming up:** Could the VM program be aborted early if some intermediate
state already guarantees the final hash can't meet the pool's difficulty target, skipping the
remaining rounds/AES finalization?

**Why it's flatly impossible, not just impractical:** RandomX's final compression (AES rounds +
Blake2b) is specifically designed so that no bit of the output is predictable from any strict
subset of the VM's final register state without completing the full finalization — that's the
avalanche property every cryptographic hash construction targets, and RandomX's is not exempted.
There is no "partial statistic" to compute early; if one existed, it would be a break of the
hash function's core security property, not a mining optimization. This isn't a "high risk, high
effort" hail mary — it's asking for a cryptographic weakness. Recorded here so it's closed
permanently rather than being the idea someone reaches for next time the project feels stuck.

---

## Quick-comparison table

| Idea | Target | Est. gain | Correctness risk | Effort |
|------|--------|-----------|-------------------|--------|
| F1 PMU stall breakdown | Diagnosis of the 94% figure | N/A (redirects future work) | None | 0.5 day |
| F3 NEON multiply-offload measurement | Main VM program ALU pipe | Almost certainly negative | None (measurement only) | 0.5 day |
| F2 Exact A53 issue-slot scheduler | Main VM program (whatever slack exists beyond current heuristic) | Unknown, possibly null | Low | 1-2 weeks |
| E2 Cross-hash boundary pipelining | Finalization/scratchpad-fill boundary | Low single-digit % of main-program share | Low-medium | 3-5 days |
| E1 Full dual-nonce interleave | Main VM program's whole 20% cycle share (ceiling, not expectation) | Unknown, gated on register-liveness check | Medium | 1-2 weeks |
| G1 Early-abort on partial hash | N/A | Impossible (cryptographic) | N/A | N/A — closed |

**Suggested order if any of this gets picked up:**

1. **F1 (PMU breakdown)** — half a day, pure information, and it determines whether F2/E1/E2
   (all back-end-stall-oriented) are even pointed at the right problem.
2. **F3 (NEON offload measurement)** — half a day, cheap enough to kill on paper before anyone
   is tempted to prototype it properly.
3. **The E1 register-liveness check** — a script, not a feature. Answers in an afternoon whether
   E1 is worth the two weeks it would otherwise cost.
4. **F2 (issue-slot-aware scheduler)** — if F1 confirms back-end/dependency attribution, this is
   the safer of the two remaining big swings (no new independent work needed, just better use of
   what's already there).
5. **E2, then E1** — only if F2 shows real remaining slack and/or the liveness check clears E1's
   register-pressure concern. These are the two ideas in this doc with a plausible path to a
   real, measurable hashrate number; everything else here is diagnostic or a closed dead end.
