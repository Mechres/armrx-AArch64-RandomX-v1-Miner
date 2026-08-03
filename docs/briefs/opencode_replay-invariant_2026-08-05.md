# Plan: the `*_M` load-use stall — replay-invariant-first attack, and when to stop

**Date:** 2026-08-05
**Author:** opencode (planning only — no source modified)
**Status:** PLAN. Supersedes nothing; consumes `what-breaks-star-m-path.md` §6 and ranks
its angles A–D against source-verified replay semantics.
**Prerequisites read in full:** `what-breaks-star-m-path.md`, `2026-08-04-star-m-fix.md`,
`perf-tracking.md`, both 2026-08-03 audits, `memory-op-scheduler-attempt.md`,
`w3-2-swap-budget-bisect.md`, `w11-instruction-census.md`, and the source regions listed
in §7 of the failure analysis.

---

## 0. TL;DR and ranking

| # | Approach | Class | Payoff ceiling | Correctness risk | Verdict |
|---|----------|-------|---------------:|------------------|---------|
| **M1** | Miner-to-miner 8w PMU diff (armrx vs XMRig, same window) | measurement | decision-critical information | zero | **DO FIRST** |
| **V** | Compile-time replay-invariant verifier (INV-1..4) as an env-gated check inside `emitPrologueMix` | Angle A (root-cause, mechanized) | converts 20-min crash-oracle gambles into seconds-scale proofs; root-causes W3-2 for the record | zero (test-only, no production behavior change) | **DO FIRST (parallel with M1)** |
| **C1** | Identity main-VM emit order A/B (disable `scheduleProgram` reorder) | Angle B (whole-instruction order only) | unknown; could be the whole residual or nothing | near-zero by construction (identity == interpreter index order) | **DO after V lands** |
| **C2** | Split-handler software pipeline done *correctly* (INV-4-safe, V-verified) | E26 shape, repaired | **≤ ~0.7% H/s** (arithmetic, §5) — capped, not speculative | high (the exact class that failed twice) | **NO-GO unless M1 changes the exposure arithmetic** |
| **C3** | NEON/vector-domain load for `*_M` | Angle C | negative (A53 FP load-use + FP→INT transfer > 3-cycle integer bubble; single shared LSU) | n/a | **KILL on paper, no experiment** |
| **D** | Accept 95.2% and document the hard in-order-A53 limit | Angle D | — | — | **probable terminal state**; evidence chain in §8 |

**One-paragraph recommendation:** Run **M1** (the missing XMRig-side 8w measurement —
the current "ld_dep_stall is the gap" claim rests on an armrx-only growth delta, and
XMRig had *more* ld_dep_stall than armrx at 1w yet still won). In parallel build **V**
(the verifier: the precise replay invariant `reg_changed_offset[]` requires, mechanized
as a compile-time check). Then run **C1** (identity-order A/B — the only emission change
that is replay-safe *by construction* and tests the audits' "surrounding-order"
hypothesis #2). If M1 confirms the stall is the real differentiator AND C1 is null, the
arithmetic in §5 says no INV-safe change can recover more than ~0.7% H/s — go to **D**
and document. Do **not** attempt C2 on current numbers.

---

## 1. The replay mechanism, exactly as implemented (both sides, from source)

**JIT side** (`src/jit_compiler_a64.cpp`):
- Every register-writing handler ends with `reg_changed_offset[instr.dst] = k;` where
  `k` is the offset *after* the handler's emitted code — i.e. the **start of the next
  emitted instruction** (e.g. `h_IADD_M`:1495, `h_IXOR_M`:1730).
- `h_CBRANCH` (:1976) emits the register modify + test, then reads
  `int32_t offset = reg_changed_offset[instr.dst];` (:1991) and emits the
  branchless-taken backward branch `b target` to that byte offset (:2005-2006). Finally
  it resets **all 8** entries to `k` (:2008-2009), i.e. "the last writer of every
  register is now this CBRANCH."
- `emitPrologueMix` (:782-783) resets all entries to `PrologueSize` (== offset of VM
  instruction 0's code) before emitting — matching the interpreter's
  `std::fill(register_usage_, -1)` + `-1 → pc=0` wrap (vm.cpp:585, :494-502).

**Interpreter side** (`src/vm.cpp`):
- At *compile* time each handler sets `register_usage_[dst] = i` (instruction **index**).
  `h_CBRANCH` (:490-513) snapshots `ibc.target = register_usage_[creg]` then sets all 8
  to its own index `i`.
- At *run* time a taken CBRANCH does `pc = ibc.target` (:718); the execute loop's
  `++pc` then lands on `target+1` — the instruction **immediately after** the last
  index-order writer of `creg`.

**So both replay "everything after the last writer of `dst`, up to and including the
CBRANCH" — the interpreter by index slice in original program order, the JIT by byte
range in emission order.** Equivalence holds iff the two always describe the same
computation. That is the entire game.

---

## 2. The invariant `reg_changed_offset[]` actually requires (INV-1..4)

From §1, for **every** CBRANCH `C` at original index `c` with target register `d`, let
`W` = the last writer of `d` before `c` in original index order within the domain
(earlier CBRANCHes reset the domain on both sides). Four conditions are jointly
sufficient for JIT-replay ≡ interpreter-replay:

- **INV-1 (anchor identity).** The last writer of `d` *emitted* before `C`'s code is
  exactly `W` (same instruction), so the JIT's branch target `end(W)` corresponds to the
  interpreter's replay start `W+1`.
- **INV-2 (window membership).** The set of VM instructions whose emitted bytes lie in
  `[end(W), start(C))` equals the index set `{W+1, …, c-1}`. No instruction's bytes
  enter the window from outside; none leave it.
- **INV-3 (window order equivalence).** For every inversion — instructions `A, B` in the
  window with `index(A) < index(B)` but `B` emitted before `A` — the pair is independent:
  no RAW/WAR/WAW register hazard in either direction and not both memory ops.
  (Sufficient by the adjacent-transposition argument: bubble-sorting the emitted order
  back to index order only ever swaps independent adjacent pairs, each of which
  commutes, so every replay iteration computes identical register and memory state.)
- **INV-4 (physical scratch-register liveness).** No emitted code inside the window
  reads a physical scratch register (x20 address/load temp, x19 FP-address temp, d28,
  x8 FPCR temp, IMUL_RCP literal regs x21-x30/x11) whose producing write is *outside*
  the window. Replay re-executes only the window bytes; a scratch value written before
  the window is not re-written, and window instructions clobber those regs freely.

**The shipped scheduler satisfies INV-1..4 (proof sketch).** Swaps move whole handlers
(bytes contiguous) ⇒ INV-4 holds: every handler writes and reads its scratch regs within
its own contiguous byte range, so a scratch read is never separated from its write
across a window boundary. INV-1: two writers of the same register are WAW-hazardous, and
every swap requires `!hasHazard` between the two instructions whose relative order
changes (`P,R` and `Q,R` / distance-3 analogues, :702-704, :720-723), so writers of `d`
are never inverted; the anchor itself is barred from the moving positions
(`is_anchor[i+1..i+3]`, :700, :718), and because swaps are local (±1, ±2), nothing can
cross the anchor without the anchor occupying a checked moving position. INV-2: window
entry/exit would require crossing the anchor (checked) or the CBRANCH/CFROUND barrier
(`is_barrier` is a hard stop on both sides, :691-695, :699, :717). INV-3: the only
inversions the scheduler creates are the swapped pairs themselves, each explicitly
`!hasHazard`-checked (register files ×3 plus the memory-memory rule). ∎

**Corollary (the design rule for anything new):** any emission change that (a) keeps
every VM instruction's bytes contiguous, (b) never moves bytes across a
CBRANCH/CFROUND boundary or an anchor, and (c) only reorders mutually independent
whole instructions, is replay-safe *by construction*. Both failures violated (a)–(c)
in ways the VM-register hazard model cannot see.

---

## 3. Root causes

### 3.1 E26 — identified: INV-4 violation, mechanism matches the crash signature exactly

E26 hoisted each `*_M` op's address `add`/`and` (which **write x20**) into the preceding
instruction's emission, leaving `ldr x20, [x2, x20]` and the consumer in place. Whenever
the hoisted bytes landed before some replay window's start while the `ldr` remained
inside it: first (linear) pass is fine; on a **taken** CBRANCH the window re-executes,
the re-executed `ldr` reads an x20 last written by *some other handler inside the
window* (x20 is clobbered by most handlers — `emitAddImmediate`, `emitMovImmediate`,
`emitMemLoad`, `h_ISTORE`, `h_CFROUND`…), producing a garbage 64-bit offset.
`ldr x20, [x2, garbage]` is an unbounded wild load → **SIGSEGV inside the RWX buffer**.

Every element of the observed signature is explained: a **crash** (exit 139), not a hash
divergence; stack unwalkable (wild address in generated code); 16/16 KAT pass (the
trigger shape — a taken CBRANCH whose window contains a split `*_M` whose x20-write
landed outside — needs a specific rare program shape × a taken branch, ~0.4% per
execution); 450-pair stress catches it ~5 min in (enough shapes × iterations). And it
explains why the register/anchor guards missed it: **x20 is not a VM register** — the
footprint model (`computeFootprint`/`hasHazard`) and the per-target-register anchor
guard reason about r[0..7]/f/e only. E26's guards also anchored on *one* register's
domain; an instruction sits inside **many overlapping windows simultaneously** (one per
later CBRANCH), so "after the domain anchor" for one window is "before the anchor" for
another — the hoist must clear `max` over *all* windows containing the instruction, and
even then it splits the handler (INV-4 requires the write to travel with the read).

**W3-2 also becomes explicable in class, though not yet in instance (§3.2):** E26 shows
the model has a blind spot exactly where physical (non-VM) state crosses a window edge.

### 3.2 W3-2 — still not proven; V will name it in one run

Marking `*_M` `is_long_latency` keeps handlers atomic (INV-4 holds) and the scheduler's
existing checks preserve INV-1/INV-2 by the §2 argument — so by INV-1..4 the break must
be an **INV-3** hole: some pair the hazard model calls independent that is not, under
replay, when a memory op occupies the `P` (anchor) position. Candidates the two audits
already ruled out: memory aliasing (memory-memory swaps are blocked), x20 lifetime,
AES state, superscalar-pool order. What remains unexcluded is subtle (e.g. an
interaction between the `hasHazard(P,Q)`-required "dependency" being satisfied by the
memory-memory rule rather than a true RAW, combined with window membership of the
deferred `Q`). **Honest status: unknown.** The verifier (§4, V) turns this into a
mechanical answer: re-apply the W3-2 one-liner under `ARMRX_VERIFY_REPLAY=1` on the
known failing pair (`jit_scheduler_stress_seed_0` / `scheduler stress input 0_59`) and
it prints the exact CBRANCH window and inversion pair that violates INV-1..4 — root
cause for the record, in seconds, no 20-minute segfault oracle. (The existing
`ARMRX_MAX_SWAPS` hook cannot do this: it gates `scheduleProgram` but the bisect sweep
was only ever run against the 16-pair equivalence test, which passes; the divergence
only shows under the 450/200-pair stress oracles.)

---

## 4. The recommended program (ordered)

### M1 — Miner-to-miner 8w PMU diff (measurement; DO FIRST; zero risk)

**Gap in the evidence chain:** the confirmed measurement is an *armrx-only* 1w→8w
growth delta (ld_dep_stall 15.9M → 25.9M/hash). XMRig's 8w ld_dep_stall was **never
measured**. E19's own table shows XMRig at 1w has *more* ld_dep_stall than armrx
(19.33M vs 17.63M/hash) and still wins. Both miners run identical scratchpad working
sets on the same SoC/L2/DRAM — if XMRig's ld_dep_stall grows by a similar factor at
8w, then ld_dep_stall is **not the armrx-vs-XMRig differentiator** and the entire
`*_M` chase is another denominator error (this project's third: the `--mine` census,
the phantom 8w gap, the stripped-binary attribution). The Copilot audit recommended
exactly this miner-to-miner growth delta; only the armrx half was ever run.
Measurement-discipline rule #2 exists for this: *miner-to-miner only.*

**Protocol** (per discipline rules 1, 9, 10): same device state, fan on, report temp;
`bench_armrx --full-hash-only --perf-ready`-style saturated 500-hash window for armrx
at 8 workers; XMRig 8-thread long window (its `--http-port` summary for live rate);
`perf stat` events `cycles, instructions, ld_dep_stall, agu_dep_stall,
other_interlock_stall, l1d_cache_refill, l2d_cache_refill`; clock-check
`cycles ÷ elapsed ≈ 765 MHz` on every window; per-hash normalization on both sides;
≥3 repeats, median.

**Decision rule:**
- If armrx 8w ld_dep_stall/hash ≈ XMRig's (within ~15%): **the stall is not the
  differentiator** — stop all `*_M` work, re-localize the 4.8% (prime suspect becomes
  the post-E24 +12% instruction count interacting with 8w contention; that is a
  *different* experiment, out of scope here). Go to D with this evidence.
- If armrx ≫ XMRig: the stall is confirmed as the differentiator; proceed to C1 with
  a real target.

### V — Replay-invariant verifier (Angle A, mechanized; DO in parallel; zero production risk)

**What it is:** an env-gated (`ARMRX_VERIFY_REPLAY=1`) compile-time check run at the end
of `emitPrologueMix` (after the emission loop, :799-808) on **every** compiled main-VM
program. On violation it prints the program seed/index, the CBRANCH index, the window
endpoints, the offending instruction(s)/pair, and aborts — instead of a segfault or a
silent wrong hash 20 minutes into a stress run. Unset ⇒ zero cost, zero behavior change.
Test-only instrumentation in the spirit of the existing `ARMRX_MAX_SWAPS` hook and
`--jit-dump`.

**Checks (mechanical, per program; all data already available — program, emission order,
per-instruction byte ranges as recorded for `jit_dump_`, footprints):**
- **V1 (contiguity):** the emitted bytes partition into exactly one contiguous interval
  per VM instruction, in emission order. (E26 fails this mechanically — a split handler
  is directly visible.)
- **V2 (window membership / INV-1+INV-2):** re-simulate the `reg_changed_offset`
  bookkeeping over the emission order and the `register_usage_` bookkeeping over index
  order; for each CBRANCH require (a) JIT anchor instruction == interpreter's `W` and
  (b) the instruction set inside `[end(W), start(C))` == `{W+1,…,c-1}`.
- **V3 (inversion independence / INV-3):** within each window, for every index-order
  inversion `(A,B)` require `!hasHazard(fp[A], fp[B])` (which subsumes the
  memory-memory and barrier rules).
- **V4 (target alignment):** every CBRANCH's computed branch target equals the *end
  offset* of the identified anchor instruction (a real instruction boundary, never
  mid-handler).

Cost is O(Σ window²) per program — trivial against JIT-compile cost. **Self-validation
step (mandatory):** run the verifier over the *stock* scheduler across all stress seeds
first; it must pass everywhere. Any flag there means the invariant (not the scheduler)
is wrong — fix INV-1..4 before trusting V as a gate.

**Why this is the highest-value code artifact on the board:** both prior attempts were
"try a change, see if replay still holds" against a 20-35-minute crash/divergence
oracle. V makes the invariant a **compile-time proof check with a named culprit**. Every
future emission idea — C1, any resurrected hoist, any scheduler change — gets gated in
seconds, and W3-2 gets root-caused for the record (§3.2). It also permanently answers
"is the current scheduler replay-sound?" with a machine check instead of three paper
reviews.

**Integration:** new file `tests/test_replay_invariant.cpp` (or env-gated block inside
`emitPrologueMix` plus a driver); registered in the `ARMRX_HAVE_JIT` test block;
the stress suites are then run once with the var set as part of the gate sequence (§6).
No production-code path changes when the var is unset.

### C1 — Identity main-VM emit order A/B (Angle B; DO after V; near-zero risk, unknown payoff)

**What changes:** `scheduleProgram` returns the identity permutation `[0..n-1]` under an
A/B guard (mirror E22's `#if` approach, but for the **main-VM** scheduler — E22 only
ever disabled the *superscalar* one; the main-VM scheduler has never been A/B'd. Its own
adoption note says the main-VM-only version "measured null at first (wrong target
region)" — so disabling it can cost at most the ~nothing it earns, while testing whether
it actively *hurts* the `*_M` spacing).

**What does NOT change:** every handler's bytes, sizes, and content; `reg_changed_offset`
bookkeeping (each handler still records its own end offset — in identity order this is
exactly the interpreter's index order); memory-op order; CBRANCH/CFROUND positions.

**Replay-invariant argument (the crux, and this time it's trivial):** identity emission
order == original program order == the interpreter's index order. INV-1: last emitted
writer of `d` before `C` is by construction the last index-order writer. INV-2: window
byte-range contains exactly `{W+1,…,c-1}`. INV-3: no inversions exist. INV-4: handlers
are contiguous (untouched). ∎ — replay-safe **by construction**, and V will confirm it
mechanically on all stress seeds as a self-check.

**Why it might win:** both audits' hypothesis #2 — XMRig emits the main VM in plain
program order and wins; armrx reorders around long-latency multiplies. The reorder only
ever *moves independent work earlier*, which can pull a future consumer *closer* to the
`*_M` load that feeds it, shrinking exactly the slack the in-order core needs. At 1w
this is invisible (armrx beats XMRig); at 8w, with memory latency up, lost slack
compounds. This has never been measured because every scheduler A/B to date targeted
the superscalar path.

**Gate:** full §6 sequence + H/s at 1w *and* 8w, median-of-3, miner-to-miner.
Ship only if 8w improves without a 1w regression. Null or negative ⇒ revert and record
— that permanently closes the "surrounding order" hypothesis too.

### C2 — Split-handler software pipeline, INV-4-safe (E26 repaired; NO-GO on current arithmetic)

The only shape that can put work *between* a `*_M` load and its consumer (they share one
atomic handler — no whole-instruction reorder can ever separate them; this is why the
stall is structural). The correct version is now specifiable: split `N`'s handler into
`[addr+ldr]` … `[op]`; fill the gap only with a *later* instruction `M` that is
x20/x19-free, register-independent of `N`, non-memory-store, inside **every** replay
window containing `N` (computable from the anchor simulation V already builds), with `N`
not itself an anchor and not the last instruction before a CBRANCH; re-target
`reg_changed_offset` bookkeeping to the end of `N`'s op half. V (V1 amended to admit
*verified* splits, V2/V3/V4) gates it mechanically.

**Why it is still a no-go today — payoff ceiling arithmetic (§5):** the gap can hold at
most ~1 filler instruction realistically (dependency-dense code; x20-free independent
neighbors are scarce), hiding ≤ ~1 of the ~6.3 average exposed cycles ⇒ ≤ ~15% of the
stall ⇒ **≤ ~0.7% H/s**, against the highest correctness-risk class in the project's
history (two failures, one still unexplained). Even with V, the expected value is
negative. **Revisit only if M1 shows the exposure is ~3-cycle L1-scale** (then 1-2
fillers hide most of it) **or** a multi-instruction fill source appears. Otherwise
permanently closed, with the mechanism this time *understood* rather than merely feared.

### C3 — NEON load (Angle C): killed on paper

`ldr d20, [x2, x20]` + `fmov x20, d20` moves the value through the FP domain: on A53 the
FP/vector load-to-use latency is *longer* than integer (≈4 cycles) and the FP→INT
transfer adds further multi-cycle latency — ≥6 cycles total against the 3-cycle integer
bubble being attacked. The A53 has a single shared LSU per core, so there is no
port-contention relief to buy. Consumers are scalar integer ops with no vector-domain
equivalent that avoids the transfer. Strictly worse; no experiment warranted.

---

## 5. Exposure arithmetic (why the payoff ceiling is low)

Source-verified numbers: main-VM JIT region = **11.79M instr/hash, 29.08M cycles/hash,
IPC 0.405** (W1-1 census, device-verified); `*_M` (int + FP) ≈ 35% of main-VM ops
(failure-analysis §0) ≈ **~4.1M ops/hash**; ld_dep_stall = 15.9M/hash (1w) → 25.9M/hash
(8w) ≈ 0.88 stall-cycles per core-cycle at 8w.

- Average per-`*_M` exposure ≈ 25.9M / 4.1M ≈ **6.3 cycles at 8w** (1w: ~3.9). That is
  L2-scale latency, not the 3-cycle L1 bubble — consistent with the +63% growth being
  contention-driven (L1 is per-core and cannot contend; L2/DRAM can).
- Hiding *N* cycles on an in-order core requires *N* independent issue slots between
  load and use. The realistic fill via C2 is ~1 slot ⇒ ≤15% of the stall ⇒ ≤0.7% H/s.
- Chain-*shortening* (E25 family) is already proven null on device — and the remaining
  address algebra is closed: `(src+imm)&mask` cannot be reassociated into fewer ops
  ((a+b)&M == ((a&M)+b)&M only swaps which op is first; the mask cannot fold into the
  register-offset `ldr`; the L1/L2 masks differ by `modMem` so no uniform pre-mask of
  `src` is legal). XMRig's integer `*_M` lowering is confirmed structurally identical
  (opencode audit, upstream reference) — there is no shorter shape to adopt.

**Consequence:** the residual is *memory-latency*-bound on an *in-order* core whose only
latency-hiding tool (independent work between load and use) is structurally unavailable
inside the atomic `*_M` handler and capped at ~1 slot outside it. That is the
hard-limit evidence chain, should M1/C1 confirm it.

---

## 6. Gate sequence (mandatory for C1 and any future emission change)

Per the brief's hard constraints — KAT alone is insufficient; both failures passed KAT.

1. Cross-build: `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake && cmake --build build-cross -j$(nproc)`; deploy to device `/tmp/cross/`.
2. `test_jit_equivalence` — **16/16 byte-identical**.
3. **V gate (new, once V lands):** stress seeds run with `ARMRX_VERIFY_REPLAY=1` —
   must pass with zero violations (seconds; a violation names the culprit and aborts).
4. `test_jit_scheduler_stress` — **450/450 pairs** (device, `taskset -c 1-3`, ~20-35 min).
5. `test_jit_superscalar_scheduler_stress` — **200/200 pairs** (~35 min).
6. H/s: `bench_armrx --full-hash-only` 1w (core 3, idle, cool, median-of-3) **and** 8w
   (all cores; long converged run or real-pool soak per the 1209s/1263s precedent);
   8w ld_dep_stall/hash must DROP vs the 15.9M→25.9M baseline for any stall claim.
   Miner-to-miner vs XMRig under identical conditions.
7. **Failure protocol (non-negotiable):** any gate failure ⇒ REVERT immediately; record
   the failing seed/input, the first-differing hash stage (or V's printed culprit), and
   stop. No blind iteration — that rule is what both postmortems demand.

---

## 7. Risk ranking — "likely safe but small" vs "high-payoff but replay-risky"

- **Likely safe, small/unknown payoff:** M1 (pure measurement), V (test-only), C1
  (identity order — replay-safe by construction, INV-1..4 trivially, V-verifiable).
  These three are the entire recommended program.
- **High-payoff-looking but replay-risky:** C2 (split-handler pipeline) — and its
  payoff is *arithmetically capped* at ≤0.7% H/s anyway. Not worth it unless M1
  rewrites the exposure numbers. C3 (NEON) — not replay-risky, just microarchitecturally
  backwards. Any re-attempt of W3-2 (`*_M` as scheduler anchor) or E26 (hoist) without
  V green **and** a named root cause — forbidden by the brief, re-affirmed here.

---

## 8. Verdict (expected terminal state)

**Most likely outcome is D — accept and document — and that is a legitimate, evidence-
backed result.** The chain: (i) the `*_M` load and its only consumer share one atomic
handler, so no whole-instruction scheduler can hide the bubble (structural); (ii) the
exposure at 8w is ~6.3 cycles/op — L2-scale memory latency, not an issue-slot bubble,
so 1-slot fills cap at ~15% of the stall; (iii) chain-shortening is already null (E25)
and the address algebra admits no shorter legal form; (iv) the only shape that could
help (C2) is the exact class that failed twice and is payoff-capped below the risk
threshold; (v) XMRig bears the same SoC, the same scratchpad working sets, and had
*more* load stalls at 1w while winning. Parity = **95.2% (8w 26.65 vs 28), 101.6%
per-core (5.11 vs 5.04)**.

**But D must be *earned* by M1 and C1, not assumed:** if M1 shows XMRig does *not*
suffer the 8w stall equally, the gap is mis-localized and a different (non-`*_M`)
lever exists — likely the post-E24 +12% instruction count under 8w contention; if C1
wins, ship it. Either way the loop closes with evidence, V stays in the tree as the
permanent mechanical guard for this region, and W3-2's open mechanism gets named.

---

## 9. Exact code regions (for whoever executes)

- Verifier hook point: `src/jit_compiler_a64.cpp` `emitPrologueMix` emission loop
  (:799-808) — run checks after the loop, before returning; needs the program, the
  `emit_order` (already local, :797), per-instruction `[pos_before, codePos)` pairs
  (already captured for `jit_dump_`, :805-807), and footprints (recompute via the
  existing `computeFootprint`/`resolveInstructionType`; both in the same TU).
- C1 A/B point: `scheduleProgram` (:638-739) — early-return identity under the guard;
  nothing else touched.
- Invariant reference points: `h_CBRANCH` (:1976-2012, anchor read :1991, reset
  :2008-2009); anchor precompute (:649-664); `hasHazard` (:465-472);
  `reg_changed_offset` init (:782-783); interpreter mirror: vm.cpp `h_CBRANCH`
  (:490-513), execute (:715-720), `register_usage_` writers (:267-421).
- Known failing oracles for V validation: `jit_scheduler_stress_seed_0` /
  `scheduler stress input 0_59` (450 suite); `superscalar_sched_stress_seed_4` /
  `superscalar stress input 4_1` (200 suite) — W3-2 re-applied must be flagged here.
- Measurement precedent: `tools/perf_ready_bench.sh` handshake, `bench_armrx
  --full-hash-only --perf-ready` 500-hash window; XMRig live rate via `--http-port`
  `/2/summary`; clock-check every window (≈765 MHz).
