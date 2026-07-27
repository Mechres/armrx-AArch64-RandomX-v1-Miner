# armrx — Master Performance Plan, 2026-07-27

**Status:** synthesis document. Combines four independently-written 2026-07-27 plans into one
sequenced backlog. Nothing below has started; this file is the thing to work from going forward.
The four source documents are kept as-is for full reasoning/detail and are cited by short name
throughout:

| Short name | File | Author | Core thesis |
|---|---|---|---|
| **Opus** | `performance-plan-20260727.md` | Claude Opus 5 | Change the axis: do less work (partial dataset) or do more independent work at once (interleaving) |
| **Deepseek** | `hail-mary-ideas-20260727.md` | Deepseek V4 | Brainstorm across architecture/topology/toolchain/"out there"; ranks per-cluster cache + hybrid interpreter highest |
| **Sonnet-R2** | `hail-mary-round2-20260727.md` | Claude Sonnet 5 | Is the 94%-architectural ceiling actually proven, or just the current scheduler's ceiling? Proposes cross-hash (dual-nonce) interleaving |
| **Hermes** | `performance-plan-breakthrough-20260727.md` | Hermes (Hy3) | Pivot: the real gap vs XMRig is **instruction count** (132.93M vs 99.57M), not IPC — armrx already wins the stall war. Re-screens "IPC-closed" ideas on the count axis |
| **MidHigh** | `../mid-high-risk-performance-ideas-20260726.md` | (2026-07-26, pre-dates the four above) | Correctness-risk-ranked tiers. Tier 1 (items 1-2) already resolved — item 2 (scheduler window widening) adopted, item 1 (`IMUL_RCP` pre-assignment) closed-but-revisitable, both already reflected below. **Tier 2 (item 3, BOLT) and Tier 3 (items 4-5) were never started** and are folded into this synthesis for the first time in §Track E/J. There is no Tier 4 in the source file — it stops at Tier 3. |

All five open from the same closed state: `docs/plans/performance-plan-20260725.md` (gated plan,
closed), `docs/plans/experimental-performance-ideas-20260725.md` (backlog worked to closure),
and `docs/experiments/scratchpad-locality-bound-20260726.md` (the "+6% IPC ceiling, 94% architectural"
finding). `NEXT_STEPS.md` said no genuinely open item remained except worker-to-core placement —
the four 2026-07-27 documents were written to find out if that's actually true; MidHigh's leftover
Tier 2/3 items are the one piece of unstarted work from *before* that round that hadn't been folded
in yet.

*(Housekeeping note: Sonnet-R2 recorded that round 1's file had been moved to `~/Masaüstü/` outside
the repo. It is present at `docs/plans/20260727/hail-mary-ideas-20260727.md` now — that loose end
is resolved, all four source files live in this folder.)*

---

## 0. The load-bearing disagreement between these docs — read this before picking an item

Opus, Deepseek, and Sonnet-R2 all reason primarily in **IPC / stall-cycle** terms (the framing that
closed everything in the 2026-07-25/26 plans). Hermes points out this framing has a blind spot:
armrx's IPC (0.731) and stall rate (11.41% `ld_dep_stall`) are already *better* than XMRig's (0.612
IPC, 16.70% stall) — the entire measured ~10-12% cluster-normalized deficit is explained by armrx
emitting **33.5% more instructions per hash** (132.93M vs 99.57M) doing equivalent work.

This matters concretely: several ideas below were killed in earlier plans *because they didn't move
IPC* (peephole coalescing, `IMUL_RCP` literal-load elimination net -0.3%, superscalar literal-pool
relayout). Those closures are correct on the IPC axis but were never evaluated on the
instruction-count axis, which is the axis the actual XMRig gap lives on. **Track F below
re-opens that class of idea, but only when scored against instruction count, not IPC** — this is
the single biggest methodological correction across all four documents and should govern how
everything else here gets judged.

---

## 1. Ground truth (shared numeric baseline all four docs build on)

- Superscalar / dataset-derivation region: **72.71% of instructions, 63.42% of cycles, 1.145× IPC**
  — already efficient. Every instruction-count-reduction attempt here has regressed
  (`IMUL_RCP` literal-load elimination, superscalar literal-pool relayout, CSEL/CBRANCH).
- Main VM program region: **9.23% of instructions, 20.04% of cycles, 0.461× IPC** — the "problem
  child." `docs/experiments/scratchpad-locality-bound-20260726.md` showed **~94% of this penalty is
  architectural** (dependency chains / in-order pipeline depth on the Cortex-A53), not
  memory-latency; forcing L1 residency recovered only +6% IPC.
- C++ overhead (AES fill/hash, Argon2, JSON, etc.): **15.56% of instructions, 1.107× IPC** —
  proportionally efficient, except `hash_aes_1r_x4`/`fill_aes_1r_x4` alone at **~12.3% of all
  cycles** (Track G below).
- vs. XMRig, same device/job/light-mode: armrx **132.93M instructions/hash** (IPC 0.731) vs XMRig
  **99.57M instructions/hash** (IPC 0.612). armrx wins on IPC and stall rate; loses on raw
  instruction count. Net: ~90% of XMRig per-cluster.
- The only large *measured* win in this project's history is operational, not code-level:
  `isolcpus=1-7 rcu_nocbs=1-7` on the kernel boot cmdline, **+14% aggregate hashrate**
  (`docs/experiments/isolcpus-rt-priority-win.md`).
- Device: 2 GiB RAM, light mode forced, ~1.2-1.3 GiB `MemAvailable` while idle. Two 4-core L2
  clusters (MSM8929/Snapdragon 415) with measured throughput asymmetry — cluster 0
  (cores 0-3) ~4.26 H/s/core, cluster 1 (cores 4-7) ~2.13-2.84 H/s/core depending on isolation and
  measurement window (long-window/sustained numbers are the trustworthy ones — see §6).

---

## 2. Deduplication map

Three ideas were independently proposed by more than one document. Treat these as **one item**
each, not two:

- **Partial/hybrid dataset in light mode** = Opus Item 1 = Deepseek D2. Nearly identical mechanism
  (precompute a prefix of the 2 GiB dataset, hit rate ≈ `bytes_cached / 2 GiB`, fall back to
  on-the-fly derivation on miss). Opus's writeup is more implementation-complete (exact register/
  JIT-emission sketch, gates A/B/C); Deepseek's is the same idea with a rougher instruction-count
  estimate. **Track B below uses Opus's version as primary, Deepseek D2 as corroborating estimate.**
- **Cross-hash / dual-nonce interleaving** — Opus Items 3 (superscalar-only 2-way interleave, cheap)
  and 4 (full dual-nonce, the "moonshot") sit at opposite ends of the same idea Sonnet-R2 develops
  independently as Category E (E1 full dual-nonce interleave, E2 lighter boundary-only version).
  These compose into one graduated track, not three separate ones. **Track D below merges them by
  cost, cheapest first: Opus Item 3 → Sonnet-R2 E2 → Sonnet-R2 E1 / Opus Item 4.**
- **Instruction-budget audit** = Opus Item 5 ("instruction-budget reconciliation") = Hermes Item 1
  ("per-opcode AArch64 instruction-budget audit"). Same diagnostic: account for where
  `132.93M instructions/hash` actually goes, per-opcode, against a theoretical minimum. Hermes's
  version is more actionable (ties directly into `kCompileHandlers`, proposes the concrete tool
  extension). **Track A uses Hermes's framing.**
- **Worker/main-thread core-0 cost** = Opus Item 6 = Hermes Item 7. Both point at the same
  `NEXT_STEPS.md` open item: worker 0 shares core 0 with the stratum reader / JSON / per-second
  console print, and under `isolcpus` it can't be relocated (no sysfs `cpufreq` data →
  `detect_core_order()` falls back to sequential placement). **One item, Track I.**

---

## Track A — Diagnostics (cheap, parallel, zero/near-zero risk — run first)

Everything downstream should be scored against these, not against intuition.

1. **Per-opcode instruction-budget audit** *(Hermes #1 = Opus #5)*. For every emitted VM opcode
   (`kCompileHandlers` in `src/jit_compiler_a64.cpp`) and superscalar opcode (`src/superscalar.cpp`),
   compute a theoretical-minimum AArch64 instruction count and diff against `--jit-dump`'s actual
   emission. This is the map to the whole remaining instruction-count gap and the gating input for
   Track F. Also reconciles a standing discrepancy Opus flagged: two prior counts of the
   superscalar path's cost (58.4M/hash vs ≈96.6M/hash) don't agree — ~38M instructions/call
   unaccounted for. ~1 day. No correctness risk (read-only tooling).
2. **PMU `STALL_FRONTEND` vs `STALL_BACKEND` breakdown on the main VM program region**
   *(Sonnet-R2 F1)*. The "94% architectural" finding was derived from memory-latency experiments,
   not a direct front-end/back-end stall split. If a meaningful share turns out to be front-end
   (I-cache miss, fetch bubbles, branch-recovery) rather than back-end (waiting on ALU/MUL), that
   redirects effort toward code-layout fixes (cheap) instead of concurrency (expensive) — determines
   whether Track D/E are even pointed at the right problem. ~0.5 day, pure measurement. **Do this
   before committing to Track D or Track E.**
3. **NEON cross-domain move latency measurement** *(Sonnet-R2 F3, cheap-to-falsify)*. Measure
   GPR↔NEON transfer latency (`FMOV`/`INS`/`UMOV`) on the devbox in isolation. If it's as expensive
   as the A53 SOG implies, it kills the NEON-multiply-offload idea on paper before anyone prototypes
   it. ~0.5 day.
4. **Register-liveness check for dual-nonce interleaving** *(Sonnet-R2, gates E1/Item 4 specifically)*.
   Dump how many of the 8 int + 12 float registers are simultaneously live at each instruction
   across real compiled main-program instances. If routinely near 8/8 with no slack, Track D's
   expensive end (Opus Item 4 / Sonnet-R2 E1) is dead on arrival and shouldn't be prototyped — only
   the cheap end (Opus Item 3) remains viable. Cheap script, not a feature.

---

## Track B — Do less work: hybrid partial dataset *(highest expected value, correctness risk genuinely low)*

*Opus Item 1 = Deepseek D2; Opus Item 2 / Deepseek B1 as follow-ons.*

RandomX's fast/light split is a spec convenience, not a hard requirement — the dataset is a pure
function of the cache, so any precomputed prefix is valid and bit-identical to on-the-fly
derivation. This device has ~950 MiB free while mining. Caching a `B`-byte prefix gives a hit rate
of `B / 2 GiB`; Opus estimates +16% to +32% hashrate at 512-896 MiB cached (treat as directional,
not promised — see gates).

**Why this is unusually safe for a big-ticket item:** it has a **total, cheap correctness oracle**
— `partial[i] == generate_dataset_item(cache, i)` for all `i` in the cached range, and both branches
of the resulting hybrid (fast-mode's direct load, light-mode's derivation) are already independently
KAT-verified. This item chooses between two proven computations; it invents nothing new.

**Implementation** (Opus's sketch, already concrete):
1. `PartialDataset`: `mmap`/`MADV_HUGEPAGE` buffer of `N` items, filled via the existing
   `initialize_dataset()` (`src/dataset.cpp:66`) — already NEON-vectorized and parallelized, reuse
   verbatim with `item_count = N`.
2. JIT emission: in `randomx_program_aarch64_vm_instructions_end_light`
   (`jit_compiler_a64_static.S:528`), a bound check before `bl rx_calc_dataset_item` — hit → direct
   load via `rx_program_xor_with_dataset_line`; miss → existing derivation, unchanged. ~4 extra
   instructions on a ~3,563-instruction path.
3. Incremental fill (do second, not first): the bound is re-readable at each of the JIT's 8
   per-hash recompiles, so mining can start in pure light mode and the threshold can rise as a
   background fill progresses, at zero extra runtime risk once the blocking version is validated.
4. CLI: `--dataset-mb=N` (0 = off) plus an `auto` policy sized from `MemAvailable`.

**Gates, in order:**
- **Gate A — init cost.** Measure fill wall-clock for 512 MiB before wiring into the JIT. If
  seconds, negligible against a ~2.8-day seed rotation; if minutes, incremental fill (step 3)
  becomes mandatory, not optional.
- **Gate B — the memory-path caveat (real, not theoretical).** This device's known 8-worker
  bottleneck is shared memory-path arbitration between the two L2 clusters. This item trades ALU
  work for random DRAM traffic (~2.5M extra 64-byte reads/sec aggregate at 50% hit rate) — exactly
  the contended resource. **A single-core `taskset` measurement will overstate the win; measure at
  1 worker AND 8 workers, and let the 8-worker number decide.**
- **Gate C — memory pressure.** No swap on this device. Size conservatively; watch for OOM-killer
  activity over a multi-hour run; re-check `MemAvailable` while mining, not idle.
- **TLB.** 768 MiB of random access needs THP or this could be page-walk-dominated. Verify via
  `smaps` (the Argon2 cache precedent is good, but its working set is smaller).

**Follow-on, contingent on Track B landing — asymmetric per-cluster memory mode**
*(Opus Item 2)*: cluster 0 already wins interconnect arbitration (~4.26 H/s) — give it the
memory-heavy partial-dataset path; keep cluster 1 on pure ALU-heavy derivation, since its ALUs sit
idle waiting on the bus anyway. Hours of effort once Track B is measured; per-worker boolean
threaded into VM construction.

**Related but distinct — per-cluster cache *replication*** *(Deepseek B1)*: instead of one shared
256 MiB Argon2 cache, give cluster 1 (cores 4-7) its own physical 256 MiB copy, eliminating
interconnect arbitration on cache reads specifically (as opposed to Track B's dataset reads).
~1-2 days, no correctness risk (identical cache data, just duplicated), ~12.5% aggregate estimated
*if* the slow-cluster penalty is interconnect-bound rather than memory-controller-bound — genuinely
unknown until measured. Independent of Track B; can run in parallel or be tried first since it's
cheaper. If Track B's Gate B measurement shows severe cross-cluster memory contention, B1 is worth
promoting ahead of Track B's main item as a smaller, faster test of the same underlying hypothesis
(is the slow cluster's penalty interconnect- or controller-bound).

---

## Track C — Structural ABI/call overhead: inline the light-mode dataset-item helper

*Hermes Item 2. Traces to an abandoned 2026-07-19 handoff priority, never executed. Independent of
Track B — orthogonal axis (reduces the cost of every derivation call rather than reducing the
number of calls) and composes with it.*

Per light-mode iteration (16,384×/hash: 2048 iterations × 8 programs), the current path pays a
`bl`/`ret`, a 96-byte outer frame, a 112-byte inner frame, 8 saved+restored GPRs, a 64-byte store to
a temp buffer, then an immediate 64-byte reload of that same buffer to XOR into VM registers — a
full round-trip the data never needed to make (`jit_compiler_a64_static.S:504,536,824-919,338-349`).
The item is computed in x0-x7 and could be XORed **directly** into the live VM registers.

**Phased, each independently revertible:**
- Phase A — remove duplicate register preservation.
- Phase B — direct result mixing (skip the store/reload relay entirely).
- Phase C — only then consider inlining the `bl` away (this is what Track E's "monolithic JIT"
  extends to).

**Why it's different from everything IPC-framing closed:** this attacks call/ABI overhead and
redundant memory traffic, not instruction-level stalls — it reduces raw instruction count (the axis
§0 says is the real XMRig gap) *and* removes latency the scheduler structurally can't hide because
it's outside the JIT body (across a `bl`).

**Correctness risk: high** — this is the JIT's most safety-critical path; a register-contract bug
here is a silent wrong hash. Mitigate with full JIT+interpreted KATs per phase and a new
differential test comparing light-mode dataset-item results before/after across many deterministic
indices. Treat each phase as a separate, individually-revertible landing.

**Effort:** ~3-5 days phased. **Expected payoff:** hypothesized 3-8% (never measured, because never
built) — potentially the largest single win in this entire combined backlog, on the axis that
matters most per §0. **Gate:** if instruction count drops but hashrate regresses, that's a hidden
hazard signal — stop and investigate, don't push through.

---

## Track D — Concurrency: filling in-order pipeline bubbles with a second, independent hash

*Merged from Opus Items 3-4 and Sonnet-R2 Category E, cheapest-to-most-expensive.*

The shared insight: the main VM program's dependency chain is ~94% architecturally serial (proven,
§1) — nothing *within* one hash's chain can fill its own stalls. But two different nonces are
**already fully independent** with zero new hazard analysis required *between* them (only within
each, which is already verified). Interleaving two streams at emission time is "poor-man's SMT,"
done by the compiler instead of hardware.

**D1 — 2-way interleaved superscalar dataset-item derivation** *(Opus Item 3, do this first — cheapest, best-understood)*.
Both the current and next dataset-item address are already simultaneously live in the light-mode
loop (`jit_compiler_a64_static.S:528-560`); RandomX's one-iteration lookahead exists precisely so
implementations can do this. Emission is mechanically simple — emit each superscalar instruction
twice, once per disjoint register set, **no hazard analysis at all** (categorically unlike the
reverted memory-op scheduler: no reordering within a chain, so no new hazard class). Precedent
already exists in-repo: `src/dataset.cpp:88-135` does the same 2-way interleave in C++/NEON for
fast-mode init. Register budget: ~20 total for two streams, fits by extending the callee-saved set
(x19-x28) in a new 2-way entry point. **Gate: code size roughly doubles (20,916 → ~42 KB) against a
16 KiB L1 I-cache — measure `l1i_cache_refill` before/after; this is the most likely failure mode.**
Correctness oracle is total and cheap (derivation is a pure function of item number; differential
test against the existing 1-way path is exhaustive in practice). Interacts with Track B
sub-additively — if Track B lands, there's less to interleave; re-estimate after.

**D2 — Cross-hash boundary-only pipelining** *(Sonnet-R2 E2, the fallback if D3's liveness check is
bad)*. Overlap only the *tail* of hash N (AES finalization/result compression — short, fixed,
low-register-pressure) with the *head* of hash N+1 (scratchpad fill from blake2b — also short,
load/store-heavy, no VM register-file dependency). Much smaller ceiling than D3 (a few percent of
the main-program region's cycle share) but much smaller register-pressure risk, since neither
touched sequence uses the RandomX integer/float register file. ~3-5 days, low-medium risk.

**D3 — Full dual-nonce interleaved JIT emission (the moonshot)** *(Opus Item 4 = Sonnet-R2 E1)*.
Interleave the *entire* native instruction stream of two hashes, instruction-for-instruction or in
small groups, each stream's internal order left untouched (still whatever the verified scheduler
produced) — correctness reduces to "no register/memory collision between streams" (checkable
exhaustively) rather than a semantic reordering proof (the class of proof that broke the memory-op
scheduler). Register pressure is the central open question: one stream already occupies ~12-14 of
31 GPRs; two need ~24-28, leaving little room for loop/base-pointer bookkeeping. **Gated entirely on
Track A item 4 (the liveness check) — do not prototype until that returns.** If it clears, ceiling
is "up to most of the main VM program's 20% cycle share," likely much less once register-spill and
doubled memory-bandwidth-per-worker costs are counted (this could just move the bottleneck from ALU
stalls to memory bandwidth, mirroring Track B's Gate B risk, self-inflicted instead of
interconnect-inflicted). Effort: 1-2 weeks, the largest single item across the combined backlog.
**Do not start before D1 is measured** — D1 tests the same underlying hypothesis (independent-stream
interleaving pays off on this core) for a tenth of the cost, with a trivial oracle. If D1 doesn't
pay off, D3 won't either.

---

## Track E — Is the scheduler actually at its ceiling? *(gated on Track A item 2)*

*Sonnet-R2 Category F, minus F1/F3 which live in Track A as diagnostics.*

**F2 — exact A53 dual-issue-slot scheduler** *(Sonnet-R2)*. The current emitter scheduler reorders
on a register-hazard model only — it doesn't know which specific instruction-type pairs can actually
co-issue on the A53's two pipes (branch+simple-ALU vs ALU+MUL+DIV+NEON, per the A53 Software
Optimization Guide). A hazard-clean reorder can still be issue-slot-suboptimal. Building a real
list scheduler with a pipe-occupancy model is strictly a scheduling change (same hazard-safety net
as today) — low correctness risk, real effort risk (1-2 weeks; getting dual-issue pairing rules
wrong risks a *slower* schedule, not a wrong one). **Only pursue if Track A item 2 (PMU breakdown)
confirms back-end/dependency attribution** — if it's front-end-bound instead, this effort is pointed
at the wrong problem.

**D9 / conservative load hoisting retry** *(Deepseek D9)*. The memory-op scheduler extension was
tried and reverted for an unexplained JIT/interpreter divergence. A narrower retry — widen the
hazard-check window from 2 to 5 instructions, or use the existing `computeFootprint()` liveness
tracking to hoist only loads whose consumer is provably far enough away — might avoid whatever the
first attempt missed. Medium risk (same failure class as the original attempt); only worth it if F2
above shows real remaining slack, since both compete for the same "is there schedulable room left"
budget. Ceiling bounded by the same ~6% latency-recoverable gap Step 1 already found.

**Escalation — full dependency-graph list scheduler** *(MidHigh Tier 3, item 4, never started)*.
F2 above still keeps the existing fixed-window heuristic and just teaches it about A53 issue-slot
pairing. This item goes further: throw the heuristic away and replace it with genuine list
scheduling over the main VM program's *full* per-program dependency graph — the "textbook correct"
answer to the scratchpad-locality experiment's architectural finding, rather than a bounded-window
approximation of it. High effort (amounts to building a compiler-backend scheduler from scratch),
high correctness risk (silent wrong hashes), and per the source doc would need review rigor
matching or exceeding the *original* emitter scheduler's three independent audits before being
trusted. **Not worth starting before F2 is measured** — F2 answers essentially the same question
(is there schedulable room the current heuristic is leaving on the table) far more cheaply; only
escalate to a full graph scheduler if F2 finds real remaining slack that a wider-but-still-windowed
model can't capture.

**Escalation — register-allocation restructuring in the main VM program's JIT** *(MidHigh Tier 3,
item 5, never started)*. Distinct axis from both F2 and the list-scheduler escalation above:
instead of reordering *emission*, reduce false WAW/WAR dependencies between virtual registers by
allocating more physical registers per virtual register in the first place, so the scheduler (of
whichever kind) has fewer artificial serialization points to work around. Plausible given the main
VM program's register pressure (§ Track D's liveness-check discussion notes ~12-14 of 31 GPRs
already committed in a single stream), but genuinely unexplored — no concrete measurement yet
isolates false-dependency-driven serialization as *the* bottleneck specifically, as opposed to true
dependency-chain latency (which the 94%-architectural finding already attributes most of the
penalty to). Speculative; park behind Track A's diagnostics and the two items above rather than
prioritizing it on its own.

---

## Track F — Instruction-count-driven micro-optimization *(re-opened under §0's reframing; gated on Track A item 1)*

*Hermes Items 3 and 6. This explicitly re-litigates ideas closed under the IPC framing — legitimate
per §0, because instruction count is a different axis from IPC and was never separately screened.*

- **Fusion/peephole, re-scored.** `docs/plans/peephole-jit-plan.md` and the 2025-07-25 closure were
  right that the main-program region isn't stall-dominated — but that's an IPC-axis conclusion, not
  an instruction-count one. Re-run candidate search using Track A item 1's budget table as the
  source (e.g. an `IADD_RS` shift+add the emitter currently splits, or adjacent `ISTORE`+`IADD_M`
  sharing an address computation), and **gate purely on `instructions/hash` delta**, not IPC. Adopt
  only if instruction count drops *and* hashrate holds or rises.
- **Multiply-width reduction in the superscalar path.** `IMULH_R`/`ISMULH_R` (128-bit-product high
  half) and `IMUL_RCP`'s reciprocal-correction sequence are candidates for non-minimal emission —
  Track A item 1 will flag this directly if the emitter isn't using the minimal `umulh`/`smulh` +
  merge sequence. This targets the >35%-of-cycles multiply region the scheduler has already
  squeezed (+0.233%, then +0.156% on widening) with essentially nothing left to schedule — the
  remaining lever here is reducing the multiply's *own* instruction cost, not hiding its latency.
  Low correctness risk (emission reshaping within the already-reviewed superscalar path).

Both items are contingent on Track A item 1's output — there is no candidate list without it.

---

## Track G — NEON T-table AES vectorization *(independent, can run in parallel once flagged)*

*Hermes Item 4.* `hash_aes_1r_x4`/`fill_aes_1r_x4` cost ~12.3% of all cycles — the single biggest
named C++ cost. The hardware AESE/AESD path is spec-incompatible (wrong AddRoundKey order,
previously removed) and the tried `vtbl`/vector-permute NEON AES measured **-19.4%** and is
flag-gated off (`docs/experiments/neon-vector-permute-aes.md` — read before touching this again).

**The untried variant:** keep the scalar T-table *algorithm* exactly as-is (bit-identical by
construction), but vectorize the table *lookups* — `hash_aes_1r_x4` already processes 4 independent
16-byte blocks concurrently, so NEON `tbl`/`tbx` can gather across 4 lanes at once instead of 1
scalar lookup at a time, with XOR-accumulate in NEON registers. This is a throughput change to the
gather width, not a round-structure change — different mechanism from the failed attempt, different
risk profile.

**Correctness risk: medium** (bit-exactness mandatory; existing golden-pin tests
(`tests/test_aes_hash.cpp`) and the hash/fill decomposition-equivalence check must stay green).
Ship behind a new `ARMRX_ENABLE_NEON_TTABLE_AES` flag, default OFF, never on the hot path until
verified. Effort: ~2-4 days. Potentially the largest single-target upside in the whole backlog
(12.3% of cycles) or null — must be benchmarked, not assumed given the sibling attempt's outcome.

---

## Track H — Alternative execution-model experiments *(exploratory, lower priority than B/C/D)*

- **Hybrid JIT/interpreter for the main VM program only** *(Deepseek A1)*. Keep JIT for the
  superscalar path (72.71% of instructions, already efficient at 1.145× IPC); run the main VM
  program (9.23% of instructions, 0.461× IPC) through the interpreter instead, on the theory that
  the interpreter's per-opcode dispatch naturally inserts pipeline bubbles the tight JIT sequence
  doesn't. Real risk: the interpreter's `compile_instruction()` re-expands per hash (JIT compiles
  once, runs 2048×) — recompilation cost could eat any IPC gain, the same trap that would sink
  several other items here. ~2-3 days to prototype (`force_interpreted_main` flag,
  `bench_armrx` full-hash comparison). Low-medium risk — both halves are individually
  KAT-verified already.
- **Superscalar/main-program dual-issue interleaving via separate code buffers** *(Deepseek A3)*.
  Largely superseded by Track D's cleaner mechanism (same-hash-pair interleaving needs no
  cross-register-file/cross-memory-region isolation the way two *different* logical programs on one
  core would). Listed for completeness; do not pursue unless Track D fails and this offers a
  meaningfully different risk/reward — as specified it's weeks-to-months effort with very high
  correctness risk (register file isolation, memory isolation, interrupt handling across two logical
  programs on one core) for an unknown payoff.

---

## Track I — Operational: worker/main-thread cost on core 0 *(the one confirmed-open non-JIT item)*

*Opus Item 6 = Hermes Item 7, deduplicated.* Under `isolcpus=1-7`, `detect_core_order()` has no
`cpufreq` sysfs data to work from and falls back to sequential `[0..7]` placement, landing worker 0
on core 0 — the only unisolated core, which also hosts the stratum reader, JSON/job handling, and
the per-second console print. This costs worker 0 real throughput under actual pool mining
(sustained ~24.76 H/s vs. the 28.4 H/s burst figure). Removing worker 0 entirely is confirmed
*wrong* (nets -0.6 H/s). The unmeasured lever: reduce the main thread's *own* cost on that shared
core — batch/throttle the per-second render, move JSON parsing off the critical path, check whether
metrics/TUI threads also land on core 0. Worth an hour of `perf` on the main thread before designing
anything. Not a JIT change; can land independently, any time, in parallel with everything else.

---

## Track J — Cheap layout tweak *(low priority, measure-if-curious)*

**JIT buffer hot/cold reordering** *(Deepseek C3)*: reorder emission so hot superscalar code is
I-cache-line-contiguous and the cold, once-per-hash main program trails after. Current I-cache miss
rate is already 0.788% (cheap on A53, ~1-2 cycle penalty), so expected effect is small (~1% at
most). ~1 day, no correctness risk (layout only). Worth doing if Track C's Phase C /
"monolithic JIT" idea (below) is ever pursued, since that's the same lever at larger scale.

**Note — monolithic JIT (Track C's natural extension, not separately tracked):** Hermes's Item 5
("compile main program + 8 superscalar programs + the loop into ONE routine, no `bl` at all")
is explicitly framed as **Track C Phase C's natural evolution**, not a separate item — attempt it
only after Track C's Phase A/B show the ABI plumbing was the dominant cost and the `bl` itself is
next. Code size grows to ~160+ KiB against 16 KiB L1I; whether that regresses depends on whether
the *current* split is already I-cache-bound (it isn't, per the 0.788% figure) or whether removing
the call/return trampoline improves locality enough to offset it. High correctness risk (whole-hash
codegen rework), 1-2 weeks, gate on `l1i_cache_refill` + `instructions/hash` both improving.

**BOLT (post-link profile-guided binary layout)** *(MidHigh Tier 2, item 3 — genuinely never
attempted, not merely predicted-and-closed).* Uses real `perf`-recorded profiles, unlike PGO's
compile-time instrumentation, to reorder hot functions/basic blocks for I-cache locality. The
MidHigh doc's own assessment is that this is **expected** to replicate PGO's null result — I-cache
miss rate is already 0.788%, so there isn't much layout locality left to win back — but that's a
prediction, not a measurement, and it has genuinely never been run. Worth a quick try only if
someone wants to close the question definitively rather than leave it as an untested assumption;
not because any evidence currently points at a real gap here. Same low-priority bucket as the JIT
buffer hot/cold reordering above — both are cheap, correctness-risk-free, and last in line.

---

## Do NOT repeat — closed on this project's own evidence (consolidated across all four docs)

- CSEL branchless CBRANCH — +46% branch misses, reverted.
- Newton-Raphson FDIV/FSQRT — -1.1% / net regression, spec risk. Frozen.
- NEON hardware AES (AESE/AESD) — spec-incompatible AddRoundKey ordering, removed.
- NEON `vtbl` vector-permute AES / fused hash+fill — measured -19.4%, flag-gated off. (Track G's
  T-table-gather idea is a *different* mechanism and is not this.)
- Superscalar literal-pool relayout — -1.12% hashrate, reverted.
- `IMUL_RCP` literal-load elimination — +8.2% IPC, +8.5% instructions, net -0.3%.
- `IMUL_RCP` register pre-assignment — correctness failure (`test_jit_equivalence` divergence);
  safe budget ≤1 register, payoff too small. Revisit path recorded in
  `mid-high-risk-performance-ideas-20260726.md` §1 if anyone wants to reopen it.
- `IXOR_C*` logical-immediate encoding — 0 of 20,000 real immediates encodable.
- Peephole JIT coalescing **under the IPC framing** — closed on evidence there. **Reopened under the
  instruction-count framing as Track F**; that reopening is deliberate, not an oversight.
- Memory-op scheduler extension (to `*_M` opcodes) — reverted, unexplained JIT/interpreter
  divergence. (Track E's D9 is a narrower, gated retry, not a blind repeat.)
- Prefetch insertion — closed by the scratchpad-locality experiment's 6% ceiling.
- PGO — null twice. Re-measure only after a substantial binary reshape (e.g. the monolithic-JIT
  idea in Track J, if ever attempted).
- Worker-count sweep — 8 workers already confirmed as the highest-throughput choice; do not
  re-sweep without new evidence.
- Fast-cluster-only mining with frequency boost — dead end; cluster frequency is independent of the
  other cluster's load on this SoC.
- `-moutline-atomics` / LSE atomics — Cortex-A53 has no `lse` in its HWCAP; dead end.
- Custom linker script for JIT code placement — already analyzed as part of the JIT ABI review; the
  JIT's `BL`s are already within-buffer and in range. No gain.
- 1 GiB hugetlbfs backing — dTLB misses are already negligible (~1.6/million instructions) at the
  existing 2 MiB THP level; eliminating an already-negligible miss rate is itself negligible.
- Early-abort on partial hash vs. difficulty target — **not just impractical, cryptographically
  impossible.** RandomX's AES+Blake2b finalization is specifically designed so no bit of the output
  is predictable from any strict subset of final VM state without completing finalization; a
  "partial statistic" would be a break of the hash construction's avalanche property, not a mining
  optimization. Recorded so nobody re-derives this.
- Two-machine/RDMA dataset hosting, GPU (Adreno/OpenCL) offload, custom kernel module for JIT buffer
  placement, non-uniform per-cluster nonce-space width, lock-free inter-worker scratchpad sharing,
  dynamic light→fast switch at seed rotation (dataset init ~30 min vs. seed rotation ~seconds-to-
  minutes) — all dead ends for concrete, hardware- or spec-level reasons documented in
  `hail-mary-ideas-20260727.md` Category D. Not worth re-reading unless the underlying hardware
  changes.

---

## 3. Recommended sequence

```
Track A (diagnostics — items 1-4, all cheap, run first, mostly parallel)
  │
  ├─ item 2 (PMU frontend/backend) ─┬─→ gates Track E (F2, D9)
  │                                  └─→ if front-end-bound, redirect toward Track J instead
  │
  ├─ item 4 (register liveness) ────→ gates Track D's expensive end (D3)
  │
  └─ item 1 (instruction budget) ───→ gates Track F entirely (both sub-items)

Track B (partial dataset)              ── Gate A → Gate B (8-worker!) → Gate C
  │                                          │
  │                                          └─→ Track B follow-ons (asymmetric clusters / B1 cache replication)
  │
Track C (inline dataset-item helper)   ── Phase A → Phase B → (Phase C only if A/B show `bl` is next)
  │                                                                  │
  │                                                                  └─→ Track J's monolithic-JIT note
Track D1 (cheap 2-way superscalar interleave) ── l1i_cache_refill gate
  │
  ├─→ Track D2 (boundary pipelining, if D1 promising but D3's liveness check is bad)
  └─→ Track D3 (full dual-nonce, only if D1 pays off AND item 4's liveness check clears)

Track G (NEON T-table AES gather)      ── independent, start once flag scaffold exists
Track I (worker/core-0 cost)           ── independent, operational, land anytime
Track E, F, H, J                       ── each gated as noted above; lowest scheduling priority
  └─ Track E's list-scheduler / register-allocation escalations (MidHigh Tier 3) ── gated
     behind F2 specifically, not just Track A item 2 — do not promote these ahead of F2's result
```

**Suggested order of attack**, folding priority and dependency together:

1. **Track A, all four items** — half-to-one day each, run in parallel, zero/near-zero risk. These
   determine whether Track D/E point at the right problem and give Track F a candidate list.
2. **Track B (partial dataset)** — highest expected value of any single item (+16-32% *estimated*,
   caveated hard by Gate B), lowest correctness risk of any big-ticket item here (total/cheap
   oracle). Start Gate A immediately; it doesn't depend on Track A.
3. **Track C, Phases A→B (inline dataset-item helper)** — can run in parallel with Track B (different
   code path, different axis: call/ABI overhead vs. work elimination). Potentially the largest
   single win on the instruction-count axis and was simply never built before.
4. **Track D1** — cheap, well-understood, tests the interleaving hypothesis for a tenth of Track D3's
   cost. Do this regardless of whether Track A's liveness check (item 4) has landed yet, since D1
   doesn't need it — only D3 does.
5. **Track G (NEON T-table AES)** — independent axis, can be developed in parallel with any of the
   above once a flag scaffold exists; targets the single largest named C++ cost (12.3% of cycles).
6. **Track F, Track E, Track D2/D3, Track H, Track J** — in roughly that order, each strictly gated
   on its diagnostic prerequisite from Track A or on an earlier track's measured result. Do not
   promote these ahead of 1-5 without a specific reason a diagnostic surfaced.
7. **Track I (core-0 cost)** — independent, operational, no dependency on anything above; land
   whenever convenient.

---

## 4. Measurement discipline (consolidated, unchanged from prior sessions, and it has already caught real errors)

- `taskset`-pin both sides of every A/B — an unpinned comparison already produced a false "PGO wins
  2×" read on this device (`bench_armrx needs core pinning`, prior session).
- `perf stat -e cycles,instructions` over wall-clock hashrate; wall-clock has already been shown
  insufficiently sensitive at these effect sizes.
- Reverse trial order at least once per A/B to rule out thermal drift — this device has real,
  measured thermal variance between runs.
- Distinguish burst from sustained: cores 4-7 read 2.84 H/s at `--warmup=15 --seconds=60` and
  2.13 H/s at `--warmup=60 --seconds=180`. **Always use the long window.**
- **For Track B specifically: the 8-worker number decides, not the 1-worker number.** The entire
  risk of that item lives in cross-cluster memory contention a single-core measurement cannot see —
  this is the same class of mistake as the historical false "PGO wins" read, just on a different
  axis (memory contention instead of core-cluster placement).
- For any item gated on "instruction count," measure `instructions/hash` directly (`--jit-dump` /
  `bench_armrx`), not IPC — per §0, these are different axes and conflating them is exactly the
  mistake this synthesis exists to correct.
