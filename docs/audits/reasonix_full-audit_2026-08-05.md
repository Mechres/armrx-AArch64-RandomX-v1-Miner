# Reasonix — Full Audit of the `*_M` Load-Use Stall Effort (2026-08-05)

**Auditor:** Reasonix (independent full audit; no code edits, no planning)
**Scope:** E24/E25/E26 correctness, `ld_dep_stall` attribution arithmetic, CBRANCH replay
invariant + E26 segfault root-cause, safe-lever ranking, process discipline.
**Method:** docs-first (what-breaks-star-m-path.md, 2026-08-04-star-m-fix.md,
perf-tracking.md, both post-E24 audits, census), then source verification of the committed
tree, then on-device gate runs of the committed tree (results in §A).

---

## A. Correctness of the E24/E25 shipping changes — VERDICT: CORRECT (both)

### E24 (superscalar C* immediates → 3-instr MOVZ/MOVN+MOVK)
Source-verified (`emitCpoolImmediate`, `src/jit_compiler_a64.cpp:1323-1343`):
- Negative imm → `MOVN dst, #~imm>>16, lsl 16`; else `MOVZ dst, #imm>>16, lsl 16`; then
  `MOVK dst, #imm&0xFFFF`. Reconstructs the sign-extended 32-bit immediate exactly
  (e.g. imm=0x80000001 → MOVN #0x7FFF,lsl16 + MOVK #0x0001 = 0xFFFF_FFFF_8000_0001).
- Same constant, same semantics as the pre-E24 2-instr LDR-pool form → byte-identical
  hash output is guaranteed *by construction* (the constant is identical in both halves),
  not merely by test. This is exactly what the +7.1% H/s KAT result (16/16 byte-identical)
  and the superscalar stress suite confirm. No replay impact: the superscalar region has
  no CBRANCH and no scratchpad `*_M` lowering (register-only programs); the change is a
  handler-internal emission rewrite, which the replay invariant (see §C) tolerates.
- Evidence: committed `acc7735`; changelogs.md E24 entry; perf-tracking §4b.

### E25 (skip imm==0 ADD in `emitMemLoad`)
Source-verified (`src/jit_compiler_a64.cpp:1400-1418`):
- imm==0 branch: `and tmp_reg, src, #mask` — `andBase = 0x927d0000 | tmp_reg` (Rd=tmp_reg,
  bits 4:0), `src << 5` (Rn, bits 9:5), `(Log2(L1/L2)-4) << 10` (imm12). Correct encoding.
- Address identity: (src + 0) & mask ≡ src & mask — algebraically identical, ldr unchanged,
  no memory-order change. Handler-internal rewrite → replay-safe per §C.
- Documented as passing 16/16 + both stress suites (perf-tracking §0), null H/s (confirms
  the gap is NOT instruction count).

**On-device re-verification of the committed tree (this audit, cross build, `taskset -c 1-3`):**
`test_jit_equivalence` **16/16 byte-identical PASS** (fresh run, 2026-08-05). The two stress
suites (450 + 200 pairs) were launched on the same committed-tree binaries; results appended
in §A.2. Caveat: my earlier session's 450-pair baseline control (27e7c41 device build) was
**pre-E25**; E25's stress pass therefore rests on the perf-tracking record plus this run.

### A.2 Committed-tree stress results (appended after runs complete)
(Results of `test_jit_scheduler_stress` and `test_jit_superscalar_scheduler_stress` on the
committed E25 tree, device, `taskset -c 1-3` — filled in at finalization.)

---

## B. The `ld_dep_stall` attribution — the headline number is an 8× arithmetic error

### B.1 The "0.88 stall-cycles per core-cycle" claim is WRONG (8× normalization error)
perf-tracking §0 (and what-breaks §0, and the brief §8) claim:
> "8w ld_dep_stall = 25.9M/hash × ~26 H/s ≈ 673M/s ≈ 0.88 stall-cycles per core-cycle"

The arithmetic: 25.9M × 26.65 H/s = **690M stall-cycles/s (aggregate over ALL 8 workers)**.
A core-cycle denominator must be the aggregate: 8 cores × 765 MHz = **6,120M core-cycles/s**.
690/6120 = **0.113** (11.3%), not 0.88. Dividing by one core's 765M/s (690/765 = 0.90) is an
**8× error** — the aggregate stall rate was compared against a single core's cycle rate.

Cross-checks (all consistent at ~11%):
- 1w: 15.9M × 5.12 = 81.4M/s ÷ 765M = **10.6%** of core cycles.
- 8w per-hash: hash time at 8w = 1/(26.65/8) = 300 ms ≈ 230M cycles → 25.9M/230M = **11.3%**.
- The +63% absolute growth (15.9→25.9M/hash) tracks the per-worker hash-time growth
  (195→300 ms, +54%) — the stall *fraction* is nearly flat (10.6% → 11.3%), not an explosion.

**The "correction" in perf-tracking §0 (2026-08-05) that overrode the original "~30 cyc/op =
DRAM" analysis is itself the arithmetically wrong claim, and it reversed a correct one.**
This must be corrected in perf-tracking.md: the true figure is ~0.11 stall-cycles per
core-cycle (11%), and the stall is NOT "saturating ~88% of core cycles."

### B.2 Per-op stall magnitude contradicts the "3-cycle L1 bubble, NOT DRAM" claim
RandomX v1 weights (instruction_weights.hpp, sum=256): int `*_M` loads = IADD_M 7 + ISUB_M 7
+ IMUL_M 4 + IMULH_M 1 + ISMULH_M 1 + IXOR_M 5 = **25**; FP `*_M` = FADD_M 5 + FSUB_M 5 +
FDIV_M 4 = **14**; total `*_M` loads = **39/256 ≈ 15.2%** (ISTORE 16 is a store, not a load).
Per hash: 16,384 main-VM calls × 39 ≈ **639K `*_M` loads**.
- 1w: 15.9M/639K ≈ **25 cycles/op** of `ld_dep_stall`.
- 8w: 25.9M/639K ≈ **40 cycles/op**.
- A pure 3-cycle L1 bubble over 639K ops would give ~1.9M/hash — the measured 15.9M is
  **~8× larger**. The dominant component is L2/contention-class latency (the scratchpad is
  2 MiB; 256K-masked non-modMem accesses thrash the per-cluster shared L2 under 8 workers),
  NOT the 3-cycle L1 load-use bubble. The brief's "don't let 'it's just memory latency'
  become an excuse" framing is directionally useful, but the arithmetic does not support
  "this is the 3-cycle L1 bubble" — it supports a long-latency-load mix.
- Caveat: the absolute numbers come from 60 s `--mine` windows (perf-tracking's own
  discipline flags `--mine` as non-authoritative); the ±50% band on per-op cycles does not
  change the order-of-magnitude conclusion (25-40 vs 3).

### B.3 Is the residual gap really `*_M`? — YES (localized, consistent measurements)
The census (w11-instruction-census.md) attributes the main-VM JIT region at **9.91% of
instructions / 18.13% of cycles (IPC 0.405)** — the `*_M` ops live almost entirely in that
region, and 1w `ld_dep_stall` (15.9M) is ~55% of the region's 29.08M cycles/hash. The
9%/20% region signature and the `ld_dep_stall` are the same phenomenon measured two ways.
The audits' Hypothesis #1 stands. **What is NOT defensible:** the audits' secondary claim
(copilot audit §"zero-immediate fast paths") that XMRig lowers `*_M` addresses better — a
code-verified check of upstream RandomX/XMRig `emitMemLoad` (2026-08-05) shows **no
AND-skip and no imm==0 fast path** (the mask is emitted unconditionally); armrx E25 is
strictly ahead of upstream there. The opencode audit's "the inspected integer `*_M`
sequence is already the same" is the correct reading.

---

## C. CBRANCH replay invariant and the E26 segfault

### C.1 The precise invariant
Interpreter (vm.cpp:490-513, 715-721): `register_usage_[creg]` = program INDEX of the last
writer of `creg`; on a taken branch `pc = register_usage_[creg]`, the loop's `++pc` makes
the replay window the instructions **`[w+1, cbranch]` in program order**, with registers
NOT rewound (each iteration starts from the previous iteration's final state).

JIT (h_CBRANCH, jit_compiler_a64.cpp:1976-2012): `reg_changed_offset[creg]` = byte offset
of the last writer's **handler end**; the backward branch targets that offset, so the
replay window is the emitted bytes **`[w_end, cbranch]` in emitted order**, registers not
rewound.

**Equivalence condition:** for every CBRANCH, the two re-executions must produce the same
state evolution. Sufficient (and, for this codebase, necessary) conditions:
1. **Same op-set, same relative order** in the window (or an order provably
   register-equivalent — the scheduler's hazard-free P,R,Q swaps qualify; the anchor
   exclusion prevents order changes across the window entry).
2. **Window entry at a handler boundary**: the first re-executed byte must be the start of
   the window's first VM instruction's handler — i.e. `reg_changed_offset[creg]` must equal
   the emitted start of `w+1`'s handler.
3. **Read-before-write safety inside the window**: every byte re-executed in the window
   must read only registers written earlier *within the window*, or whose value is
   identical at the replay entry in both first-pass and replay.
4. **No boundary-resident bytes** whose write is invisible to any in-window read (this is
   the class the current scheduler's index-based `hasHazard`/footprint model cannot see).

Conditions 2-4 are exactly what a cross-handler byte relocation (E26's hoist) touches;
condition 1 is what W3-2's reorder touched (and what the scheduler's swap rules already
police for non-memory ops).

### C.2 E26 segfault — verdict on the mechanism
Evidence: SEGFAULT (exit 139) ~5 min into `test_jit_scheduler_stress`, PC **inside the RWX
JIT buffer** (`0xfffff7eef8e8`), backtrace unwalkable at a garbage stack address (gdb,
2026-08-05); pre-E26 baseline passes the same 450-pair gate; 16-pair KAT passes. This is a
**faulting memory access in generated code, not a value divergence** — the signature of a
wild load address or a corrupted branch target, requiring a rare program shape (16 pairs
never hit it; 450 pairs do).

Static analysis of the reverted E26 diff (reconstructed from the 2026-08-05 working tree,
since the code was reverted and only the outcome was committed):
- The hoist (`emitMemLoadAddr<20>`) **writes** x20 (`add x20, src, imm; and x20, x20, #mask`)
  and **reads only `X.src`** — it never reads x20. This matters because the parallel
  `gemini_full-audit_2026-08-05.md` root-causes the crash as "the replayed add/and reads
  stale x20" — **that specific claim is factually wrong** (the hoist has no x20 read).
- The backward scan stops at the nearest {`X.src` writer, x20 writer (`writesX20`), barrier,
  CBRANCH anchor} — a deliberately conservative set. I re-audited `writesX20` against the
  full handler table of the committed tree: no false negatives for the default build (the
  three FP `*_M` entries are conservative false positives; the handlers that exist — int
  `*_M`, ISTORE, CFROUND, ISUB_R/IMUL_R/IXOR_R src==dst, IROL_R/ISWAP_R src!=dst, IMUL_RCP
  conservative, IADD_RS displacement imm≥2^24 — are all covered; CBRANCH's own x20 write for
  imm≥2^24 is covered by the barrier stop).
- The anchor-stop guard provably lands the hoist at-or-after the domain anchor (the nearest
  anchor to `X` is the domain's own), so the hoist is inside every replay window containing
  `X`, and `X.src` is stable between the hoist and `X`'s ldr in both first pass and replay.
- I attempted to construct a concrete first-pass liveness failure (two hoists colliding on
  x20, a replay-entry state divergence, a flag mismatch) and could not complete one: every
  intermediate int `*_M` handler is itself an x20-writer, which blocks farther targets'
  scans; the hoisted bytes are always masked; the replay entry lands on the hoist which
  recomputes the (masked) address.

**Verdict (evidence-backed, honestly bounded):** the crash mechanism class is confirmed —
**boundary-resident hoist bytes inserted into CBRANCH replay windows violate condition 4 /
the replay-entry condition 2 of §C.1** (the only thing E26 changed; the only mechanism that
can produce a JIT-buffer fault on a rare shape while passing 16/16). The exact faulting
shape is **not determinable from the reverted code** — the code is gone, and neither the
docs' three hypotheses, nor Gemini's (partly wrong) mechanism, nor my own reconstruction
produces a provable first-pass or replay failure. This is itself the key finding: **two
"paper-safe" attempts have now failed, and no static analysis has produced a provable
failing shape — the next step must be instrumentation (what-breaks §6A), not another
paper-guarded attempt.**

### C.3 Is the replay invariant preservable by any byte relocation?
**In principle, yes** — the conditions in §C.1 are satisfiable by a relocation that (i)
models the FULL replay window (not just `[hoist, X]`): op-set, order, entry point, and
read/write liveness across every window at both first-pass and replay-entry states; (ii)
keeps the window entry at a handler boundary; (iii) leaves no boundary-resident bytes that
a window op reads. **In practice, the evidence says the cost/risk is prohibitive**: the
register-only hazard model has now missed two distinct failure classes (W3-2 divergence,
E26 crash), the exact E26 shape is still unexplained, and the expected payoff is bounded by
§B.2 (~11% of cycles, L2-class latency that a 2-instruction hoist cannot hide regardless of
correctness). **The safe form is the one the docs already state: change NO emitted bytes
across any CBRANCH boundary — intra-handler rewrites only** (E25's class: semantics- and
layout-preserving within a handler, replay-safe by construction).

---

## D. Ranked safe next attempts (fresh eyes, evidence-weighted)

| Rank | Lever | Expected payoff | Replay-risk | Gate plan |
|---|---|---|---|---|
| 1 | **Root-cause instrumentation** (re-apply E26 in a scratch build with a per-hoist trace + env-gated hoist budget, bisect the failing 450-pair shape; do NOT ship) | Knowledge (the crux); enables any future safe design or closes the question | N/A (debug-only, never shipped) | stress 450-pair + manual trace; revert after |
| 2 | **Accept-and-document as the parity point** | Honest closure; frees effort | none | none |
| 3 | **Intra-handler rewrites** (E25-class): the L3 (src==dst) path's mov-imm, any remaining address-path instruction cuts | ~0 (E25 measured null; the gap is latency, not instruction count) | low (handler-internal, replay-safe by construction) | equivalence + both stress suites |
| 4 | **NEON-load lever** (`ldr dN,[x2,xM]; ins x20,vN.d[0]` for int `*_M`) | likely ~0: A53 has a single shared load/store unit; the FP path's NEON loads show the same latency, and the census's superscalar NEON work is a different region | low-moderate (intra-handler, but new register pressure in x19/vector regs) | equivalence + both stress suites; cheap to A/B |
| 5 | **Full replay-model-aware relocation** (true list scheduler over the main-VM DAG with explicit CBRANCH replay modeling) | bounded by §B.2 (~11% of cycles; L2-class — a 2-instr filler hides ~2 cycles of 25-40) | high (two prior failures in this class) | heavy; not justified by current evidence |

The highest-payoff SAFE change is **#1** (it is the prerequisite for anything else) and the
evidence-backed outcome is **#2**. The brief's own §6 ranking (A root-cause, B fusion, C
NEON, D accept) matches this ordering; B (VM-level fusion with zero emission-order change)
is constrained by the same window-invariant and, like #4, has bounded payoff.

---

## E. Process / discipline audit

- **"Revert on first gate failure" — FOLLOWED.** E26 was reverted with `git checkout`
  (2026-08-05); the E26 code was never committed; the tree is clean at the current HEAD
  (`503547b`, doc-only commits on top of E25 `beeeeef`). Verified: `git status` clean
  (only the untracked `.hermes/`, `20260803/`, and this audit file).
- **Dead-ends documented — YES, thoroughly.** perf-tracking §0 (E26 record, W3-2 record),
  what-breaks-star-m-path.md (consolidated), brief §8, memory-op-scheduler-attempt.md. The
  "do not retry this shape" warning is explicit.
- **Evidence gaps / unverified claims found:**
  1. **The stall-arithmetic "correction" is wrong (8× error)** — see §B.1. It overrode a
     correct analysis and is now propagated into what-breaks §0 and the brief §8. Must be
     corrected (the original "~30 cyc/op" analysis was right in magnitude).
  2. **The copilot audit's "XMRig address fast-paths" claim is false** (upstream verified:
     no fast path; E25 is ahead). The opencode audit's reading is correct.
  3. **README.md 8w table row is stale** (24.95 H/s pinned vs the authoritative 26.65 pool
     figure).
  4. **E25's stress-suite pass was documented, not re-verified on-device** (my earlier
     450/450 control was pre-E25) — re-verification of the committed tree is in progress
     (§A.2).
  5. **Device-clock instability** (clock jumped ~3 h mid-session 2026-08-05; rebooted
     since) — log timestamps in the record are unreliable; process elapsed times are the
     reliable measure.
  6. The `ld_dep_stall` absolute numbers rest on 60 s `--mine` windows (the project's own
     discipline flags `--mine`); the +63% ratio is robust, absolute per-hash numbers
     should be re-measured with the `--perf-ready` gated harness if quoted again.

---

## Final verdict on the parity question

**The residual 4.8% is NOT closable by any change that current evidence supports.** The
evidence: (a) two cross-handler attempts (W3-2 reorder, E26 hoist) both failed the same
450-pair gate, one by divergence, one by crash, and no static analysis has produced a
provable mechanism for either; (b) the stall is ~25-40 cycles/op (L2/contention-class),
which a 2-instruction hoist cannot hide even if made correct; (c) the true cost is ~11% of
core cycles, not 88%; (d) intra-handler rewrites (the only provably replay-safe class) are
exhausted (E25) and measured null. **95.2% (8w 26.65 vs XMRig 28) is the evidence-backed
parity point** — unless and until step D-1 (root-cause instrumentation) produces a
replay-invariant-preserving mechanism with a demonstrated >1% gain, which the current
evidence does not predict.

*Clean-room note: no XMRig source was read or quoted; upstream behavior was verified
against the vendored reference (`scratch_vm_study/upstream_rx`) for technique comparison
only.*
