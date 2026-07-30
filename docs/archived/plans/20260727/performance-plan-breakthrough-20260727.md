# armrx — Breakthrough Performance Plan (2026-07-27)

*** Written by:*** Hermes Agent (Hy3)

**Status:** new, unmeasured, deliberately contrary to the last two plans. This is the
"we're stuck" plan — it does not re-run any closed lead. It pivots the optimization axis
and opens structural plays the micro-optimization era never touched.

**Read these first (why we're stuck):**
- `docs/plans/performance-plan-20260725.md` — gated plan, all steps closed: the main VM
  program's ~2.2× IPC penalty is *architectural* (in-order depth / dependency chains), not
  memory-latency. Forcing L1 residency recovered only +6% IPC.
- `docs/plans/experimental-performance-ideas-20260725.md` — entire low-risk backlog worked to
  closure (4 adopted, the rest closed on evidence or reverted).
- `NEXT_STEPS.md` — "No genuinely open performance items remain… worker-to-core placement is
  the only real open item." The micro-optimization surface is exhausted.

---

## 0. The pivot: we optimized the wrong axis

Every plan from 2026-07-24 through 2026-07-26 measured **IPC / stall signatures** and killed
ideas that "don't improve IPC" (peephole coalescing, literal-pool relayout, CSEL, Newton-
Raphson, IMUL_RCP load elimination — all closed because *fewer instructions didn't move
stalls*).

But the **measured gap to XMRig is an instruction-count gap, not a stall gap**:
- armrx: **132.93M instructions/hash**, IPC **0.731**
- XMRig: **99.57M instructions/hash**, IPC **0.612**

armrx already has *better* IPC and *better* `ld_dep_stall`% (11.41% vs 16.70%) than XMRig. We
won the stall war. The remaining ~10–12% cluster-normalized gap (90.1% of XMRig on cluster 0,
88.1% on cluster 1) is **purely ~33.5% more instructions per hash**. Chasing IPC further is
pushing on a closed door. **The prize is instruction count.**

Consequence: every "closed because it didn't improve IPC" idea must be **re-screened for
instruction-count impact**. A change that adds 0% IPC but removes 5% of instructions is a
*~5% real hashrate win on this workload* — and we have been throwing those away.

---

## 1. Per-opcode AArch64 instruction-budget audit (NEW, diagnostic, safe)

**The idea nobody did.** Build a table: for every emitted VM opcode (the ~256-entry
`kCompileHandlers` set in `src/jit_compiler_a64.cpp`, and the superscalar opcode set in
`src/superscalar.cpp`), compute the *theoretical minimum* number of AArch64 instructions to
implement it given RandomX semantics + the emitter's register model, then diff against what
the emitter actually emits (`--jit-dump` already gives per-opcode byte/instruction counts).

Where `emitted > minimum` for a non-memory opcode, that's pure instruction-count waste — the
exact dimension of the XMRig gap. This is the targeted replacement for the broad-sweep
approaches that failed.

**Why it's different:** prior work conjectured about specific opcodes (CBRANCH, `*_M`) and was
wrong (region-scoped breakdown in `NEXT_STEPS.md` fell apart — the real volume is the
superscalar path). This *measures* the budget gap per opcode from first principles, not by
guess. No XMRig comparison needed (clean-room boundary preserved — we compare against the
AArch64 ISA lower bound, not XMRig's code).

**Expected payoff:** diagnostic, but it's the map to the whole remaining gap. If e.g. 10
opcodes each emit 1 extra instruction and together account for 5% of instruction volume,
fixing them is a real 5% win on the instruction-count axis.

**Correctness risk:** none (read-only analysis + a standalone counting tool).

**Effort:** ~1 day for the tool + table. Then each fix is its own small, KAT-gated change.

**How to measure:** extend `tools/jit_correlate.py` or add a `bench_armrx --opcode-budget`
mode that dumps emitted instruction counts per opcode class, cross-checked against a hand-derived
minimum column. Reuse the existing `--jit-dump` offset tables.

**File anchors:** `src/jit_compiler_a64.cpp` (`kCompileHandlers`, `emit*`), `src/superscalar.cpp`
(`generateProgram`/`genProgram`), `src/jit_compiler_a64_static.S`, `tools/jit_correlate.py`.

---

## 2. Inline the light-mode dataset-item helper (highest structural leverage, NEVER executed)

**This is 2026-07-19 handoff Priority 2, abandoned when the project pivoted to IPC work. It is
the single biggest un-touched structural cost.**

Re-read `src/jit_compiler_a64_static.S`:
- Light-mode path ends at `randomx_program_aarch64_vm_instructions_end_light` (~`:504`).
- Every iteration does `bl rx_calc_dataset_item` (~`:536`).
- The helper allocates a **112-byte frame** and saves x0–x13 (~`:824-831`).
- Computes the 64-byte item in x0–x7, stores it to a **96-byte outer buffer** (~`:906-910`).
- Caller reloads all saved regs (~`:912-919`), branches to
  `rx_program_xor_with_dataset_line`, which **immediately reloads the same 64 bytes with four
  `ldp` and XORs into VM integer regs** (~`:338-349`).

So per iteration (×2048/hash ×8 programs = **16,384 times/hash**) we pay: a `bl`+`ret`, a
96-byte outer frame, a 112-byte inner frame, 8 saved+restored helper GPRs, a 64-byte store to
a temp buffer, then a 64-byte load back out of that same buffer — a full round-trip the data
never needed to make. The item is computed in x0–x7 and could be XORed **directly** into the
live VM registers before any store/restore.

**The idea:** a light-mode-only ABI contract where the dataset-item helper leaves its 64-byte
result in x0–x7 (or a documented scratch set), and the caller XORs directly, eliminating the
temp-buffer STP/LDP relay and halving the frame plumbing. Phase A (remove duplicate
preservation), Phase B (direct result mixing), Phase C (only then consider inlining the `bl`
away entirely). Per the handoff's own validation gate.

**Why it's different:** it attacks *call/ABI overhead and redundant memory traffic*, not
instruction-level stalls. It reduces both instruction count (the XMRig-gap axis) and the
per-iteration latency the scheduler can't hide because it's outside the JIT body.

**Expected payoff:** potentially the largest single win available — this plumbing runs
16,384×/hash and is pure overhead with no analog in XMRig's tighter dataset path. Hypothesis
range 3–8% from the handoff, but it was never measured because it was never built.

**Correctness risk:** **high** — this is the JIT's most safety-critical path (silent wrong
hash if the register contract is wrong). Mitigated by: full JIT+interpreted KATs per phase,
and a new differential test comparing light-mode dataset-item results before/after for many
deterministic indices (handoff §5.3). Treat Phase A/B as separate, individually-revertible
landings.

**Effort:** ~3–5 days, phased.

**How to measure:** `instructions/hash` and `cycles/hash` via `bench_armrx --full-hash-only`
+ `perf stat`, pinned `taskset -c 0`; plus single-thread and 8-worker H/s. Gate: reject if
hashrate regresses despite fewer instructions (the historical trap — but here fewer
instructions *is* the goal, so a regression would mean a hidden hazard, which is the signal to
stop).

**File anchors:** `src/jit_compiler_a64_static.S:504,536,824-919,338-349`; `src/vm.cpp:847-850`
(2048 iterations).

---

## 3. Reopen instruction-fusion / peephole — but screen on INSTRUCTION COUNT, not IPC

`docs/plans/performance-plan-20260725.md` closed peephole coalescing because "the remaining gap
is stall-dominated, not instruction-count-dominated" — that conclusion was about the **main VM
program region's IPC**, which is a *different metric* from the XMRig instruction-count gap. The
closure reasoning does not transfer to the instruction-count axis.

**The idea:** re-run the fusion/coalescing candidate search (`docs/plans/peephole-jit-plan.md`
exists but its assumptions were corrected in the handoff), this time scoring candidates by
**instructions eliminated per hash**, using idea #1's budget table as the candidate source.
Pairwise emitter fusions (e.g. an `IADD_RS` shift+add that the emitter currently splits, or
adjacent `ISTORE`+`IADD_M` that share an address computation) are the kind of thing the budget
audit will surface.

**Expected payoff:** unknown until #1; could be 1–5% on the instruction axis.

**Correctness risk:** low–medium (pure emission reshaping; KATs catch divergence).

**Effort:** contingent on #1's output.

**Gate:** score on `instructions/hash` delta, not IPC. Adopt if instruction count drops and
hashrate holds or rises.

---

## 4. NEON T-table AES vectorization — a CORRECTNESS-PRESERVING variant of the #1 C++ cost

`docs/plans/experimental-performance-ideas-20260725.md` idea #4 measured `hash_aes_1r_x4` +
`fill_aes_1r_x4` at **~12.3% of all cycles** — the single biggest named-C++ cost. The hardware
AESE/AESD path is spec-incompatible (AddRoundKey order) and the tried `vtbl`/vector-permute NEON
AES measured **−19.4%** and is flag-gated off.

**The idea (genuinely untried):** keep the *correct* T-table algorithm, but vectorize the table
*lookups* with NEON. `hash_aes_1r_x4` already processes 4 independent 16-byte blocks
concurrently — the T-table indices for all 4 blocks can be gathered with NEON `tbl`/`tbx` across
four 128-bit lanes, doing 4 T-table lookups per NEON instruction instead of 1 per scalar
instruction, with the XOR-accumulate done in NEON registers. This is *not* the failed
`vtbl`-permute AES (which tried to replicate the round structure in NEON and got the ordering
wrong); it's scalar-T-table math executed with NEON gather+shuffle for throughput. The
arithmetic stays bit-identical to the scalar T-table path by construction.

**Why it might not be −19.4% this time:** the failed attempt replaced the algorithm; this one
*keeps* the algorithm and only changes the lookup gather width. Different mechanism, different
risk profile. It is still a hypothesis — NEON `tbl` gather has its own latency, and 4 parallel
T-tables may not fit well, so it could still be a wash. Measured, not assumed.

**Expected payoff:** potentially large (12.3% of cycles is the target), or null. Must be
benchmarked.

**Correctness risk:** medium — bit-exactness is mandatory. `tests/test_aes_hash.cpp` golden pins
+ the `hash_and_fill_aes_1r_x4` decomposition-equivalence check already exist and must stay
green. Ship behind a new `ARMRX_ENABLE_NEON_TTABLE_AES` flag (default OFF), never on the hot
path until verified.

**Effort:** ~2–4 days.

**File anchors:** `include/armrx/aes_hash.hpp`, `src/aes_hash.cpp`, `tests/test_aes_hash.cpp`,
`docs/experiments/neon-vector-permute-aes.md` (read first — do NOT repeat its approach).

---

## 5. Monolithic JIT: compile main program + 8 superscalar programs + the loop into ONE routine

**The crazy one.** Today the per-hash hot path is: static main loop (`jit_compiler_a64_static.S`)
→ `bl rx_calc_dataset_item` → return → XOR → next iteration. The 16,384×/hash `bl`/`ret` is the
recurring tax (see idea #2). What if, instead, the JIT emitted **one monolithic routine** per
hash: the 2048-iteration loop body with the dataset-item helper *inlined at the call site* (no
`bl`), the 8 superscalar programs already compiled once and branched-to inline, all in a single
contiguous code region?

**Trade:** code size. 8 superscalar programs × ~20 KiB ≈ 160 KiB, plus the main body — far
exceeding the 16 KiB L1I. But I-cache miss rate is already measured at **0.788%** (idea #8),
suggesting the current split isn't I-cache-bound either; a monolith might *improve* I-cache
locality by removing the call/return trampoline and the helper's separate code region. This is
exactly the kind of "crazy" structural bet that micro-opts can't reach — and it directly
eliminates idea #2's `bl` without the fragile ABI surgery.

**Expected payoff:** folds idea #2's win (no `bl`, no frame plumbing) *plus* potentially better
code locality. Or it could regress via I-cache thrash — must be measured, not theorized.

**Correctness risk:** high (whole-hash codegen rework). Prototype behind a flag; full KATs.
Stress `test_jit_equivalence` + `test_jit_determinism` + `test_mining`.

**Effort:** ~1–2 weeks if pursued.

**Gate:** measure I-cache miss rate (`perf stat -e l1i_cache,l1i_cache_refill`) and
`instructions/hash` together. Adopt only if both improve. **This is the natural evolution of
idea #2's Phase C** — do #2 first; attempt the monolith only if #2's Phase A/B already show the
ABI plumbing was the dominant cost and the `bl` itself is next.

---

## 6. Multiply-latency restructuring in the superscalar path (the >35%-of-cycles region)

`jit_correlate.py` attributed **>35% of all mining cycles** to `IMUL_R`/`IMUL_RCP`/`IMULH_R`/
`ISMULH_R`, executed via `bl rx_calc_dataset_item` 16,384×/hash. The scheduler already won a tiny
+0.233% by reordering around these; widening it won +0.156%. There's almost nothing left to
schedule.

**New angle — reduce the multiply's own cost, not its latency hiding:**
- **`IMULH_R` / `ISMULH_R` width:** these compute the high 64 bits of a 128-bit product
  (`umulh`/`smulh` on A53 — long latency). RandomX's superscalar programs use these heavily.
  Check whether the emitter emits the *minimal* sequence (a single `umulh`/`smulh` + the needed
  shift/merge) or spills to a temporary 128-bit via `mul`+`smulh`. Idea #1's budget audit will
  flag this directly.
- **Reciprocal-multiply correction strength reduction:** `IMUL_RCP` uses `reciprocal ×
  dividend` then a correction `sub`. If the correction step emits more than the minimum, the
  budget audit catches it. This is the same axis as idea #1/3 — the budget table is the gate.

**Expected payoff:** sub-1% to a few %, all on the instruction axis (multiply latency on an
in-order A53 can't be scheduled away, but the *surrounding* instruction count can).

**Correctness risk:** low (emission reshaping within the already-reviewed superscalar path).

**Effort:** contingent on #1.

---

## 7. Worker-to-core placement (the ONE real open operational item — not JIT, but real hashrate)

From `NEXT_STEPS.md`: with `isolcpus=1-7` active, `detect_core_order()` falls back to sequential
`[0..7]` (no `cpufreq` sysfs), so worker 0 lands on **core 0 — the one unisolated core**, which
also hosts the stratum reader / JSON / per-second print. Under real pool mining this costs
worker 0 its throughput for the whole run (~24.76 H/s sustained vs the 28.4 burst figure).
Excluding core 0 was tried-and-rejected (loses a fast-cluster core). The real lever: **reduce
the main/stratum thread's own CPU cost on the shared core** (lower its priority, move the
per-second print off the hot path, batch console output) rather than removing a worker.

**This is not a JIT change but it's the only confirmed open hashrate item.** Listed for
completeness; pair it with the JIT work if a "performance plan" should also close the
deployment-side gap.

---

## Do NOT repeat (closed on this project's own evidence)

- Any memory-*latency* hiding (PRFM, memory-op scheduler) — Step 1 proved the stall is
  architectural; +6% ceiling. Closed.
- NEON `vtbl` vector-permute AES — measured −19.4%. Flag-gated, do not重新use its approach (#4
  here is a different mechanism).
- Newton-Raphson FDIV/FSQRT — −1.1%, spec risk. Frozen.
- PGO — null twice. Re-measure only after a substantial binary reshape (e.g. idea #5).
- CBRANCH/CSEL — +46% branch misses. Reverted.
- Superscalar literal-pool relayout — regression. Reverted.
- IMUL_RCP register pre-assignment (idea #1 old) — `test_jit_equivalence` divergence; safe
  budget is ≤1 register, payoff too small. `mid-high-risk-performance-ideas-20260726.md` §1 has
  the revisit path if anyone wants to resurrect it.
- BLAKE2b NEON — negligible in profile. Closed.
- I-cache pressure — 0.788% miss, closed.
- Scratchpad alignment — already 2 MiB-aligned, closed.

---

## Recommended sequence (each gates the next on real measurement)

1. **Idea #1 (budget audit)** — 1 day, safe, produces the candidate list for #3/#6. Do first;
   everything downstream is scored against its output.
2. **Idea #2 (inline dataset helper), Phases A→B** — highest structural leverage, phased,
   KAT-gated. This is the "we're stuck" breaker if anything is.
3. **Idea #3 / #6 (fusion + multiply-width)** — driven by #1's table; instruction-count scored.
4. **Idea #4 (NEON T-table AES)** — independent of the above; start in parallel once a flag
   scaffold exists. Biggest single-target upside (12.3% of cycles).
5. **Idea #5 (monolith)** — only after #2 proves the `bl`/ABI was the dominant cost.
6. **Idea #7 (core placement)** — operational, can land anytime; closes the only open
   deployment-side item.

**Honest expectation:** the project's track record of "good on paper, null on silicon" is ~8-for-
8. Treat every item above as a hypothesis with even odds until `perf stat` + `bench_armrx` on
the devbox say otherwise. But unlike the last two plans, this one targets the axis where the
actual measured gap lives (instruction count, not IPC) — so even a 50% hit rate here moves the
number that matters.
