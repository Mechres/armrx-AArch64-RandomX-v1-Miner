**Written by:** Hermes (hy3)
# Review: conservative emitter lookahead scheduler (commits be94b1f, 6712479)

Reviewer: independent pass over `src/jit_compiler_a64.cpp` /
`include/armrx/jit_compiler_a64.hpp`, plus the CBRANCH/loop-body machinery in
`src/vm.cpp` and the literal-pool logic in `src/jit_compiler_a64.cpp` +
`generateSuperscalarHash`.

Scope: correctness only (consensus-critical silent-wrong-hash risk). Perf is out of scope.

Method: static tracing of the emitted-code dataflow vs. the interpreter's
index-based loop semantics. I did **not** execute the stress tests — the AArch64
JIT cannot run on this x86_64 host (`ARMRX_HAVE_JIT` is defined only on AArch64,
per AGENTS.md), so a local run would be interpreter-vs-interpreter and would not
exercise the scheduler at all. Findings below are read from the code; where I
say "the pass/fail evidence says X," that is your on-device result, not mine.

---

## TL;DR

- The two schedulers' *own* logic (greedy 3-window single swap, hazard model,
  index bounds, `i += 3` stride) is **correct**. I could not construct a wrong
  result from the swap mechanics themselves.
- The **CBRANCH anchor logic is correct and robust** — including the
  back-to-back / empty-domain / first-last-in-domain boundary cases you flagged.
  No live bug.
- The **superscalar IMUL_RCP exclusion is correct**, and the "can IMUL_RCP be P
  and still break two swaps later?" question resolves to **no** — verification below.
- The **src==dst exclusion is the one place I cannot sign off on mechanically.**
  The doc's stated reason (a cross-instruction x20 race) does **not** hold up
  under code inspection. The exclusion is empirically necessary (per your
  bisection), so I am *not* recommending removing it — but the stated mechanism
  is wrong/unknown, which means the exclusion may be either (a) more conservative
  than necessary, or (b) an incomplete band-aid over a *different* still-unknown
  hazard. This is the highest-priority open question.
- One genuine **maintenance hazard**: `resolveInstructionType()` is a hand-written
  if-chain that must stay in sync with the macro-derived `engine[256]` table;
  they are not mechanically linked. A future opcode added to `engine` but not to
  `resolveInstructionType` silently collapses that opcode's footprint to NOP
  (no read/write/barrier bits) and could permit an unsafe swap.

---

## 1. CBRANCH anchor logic — VERDICT: correct

### What the JIT actually does
`h_CBRANCH` (jit_compiler_a64.cpp:1750) emits, for a backward branch, a target
taken from `reg_changed_offset[instr.dst]` (line 1765), which is the **code
position of the last instruction that wrote `creg`, in emission order**. After
emitting, it resets `reg_changed_offset[i] = k` for all 8 registers (lines
1782-1783) — the JIT analogue of the interpreter's `register_usage_` reset.

The interpreter loop is index-based: `ibc.target = register_usage_[creg]`
(vm.cpp:485) and `pc = ibc.target` (vm.cpp:695), so its loop body is the index
range `[target, branch)`.

For the JIT and interpreter to re-execute the *same set* of VM instructions,
the JIT's emitted loop body `[anchor_code_pos, branch_code_pos]` must correspond
to program indices `[anchor_index, branch_index)`.

### Why the anchor constraint is sufficient
The scheduler marks, for each CBRANCH at program index `i`, the **last writer of
`creg` in the domain `[domain_start, i)`** as `is_anchor` and refuses to let that
index become Q or R of any swap (`!is_anchor[i+1] && !is_anchor[i+2]`,
scheduleProgram:620).

Trace the consequences of "the anchor never moves and nothing may cross it":
- The anchor sits at program index `a`. Because it is an anchor, no swap may have
  it as Q or R. The only windows that could move index `a` are those with P = a-1
  (Q=a, blocked) or P = a-2 (R=a, blocked). So `a` is **immovable**.
- The swap primitive is `(i, i+1, i+2) → (i, i+2, i+1)`: R moves *left* by one,
  Q moves *right* by one (scheduleProgram:625-627). To move any instruction
  *across* the anchor `a`, some swap would need Q=a or R=a — forbidden. So nothing
  crosses `a`.
- Therefore, in emission order, `a` remains the **last writer of `creg`** (nothing
  after it in program order can be emitted before it). So `reg_changed_offset[creg]`
  at the CBRANCH's emit time equals `a`'s code position — exactly the interpreter's
  `target`. The emitted loop body is the emitted code for program indices
  `[a, branch)` in some hazard-checked permutation. **Match.**

### Boundary cases you flagged
- **First instruction of a domain is the anchor:** anchor scan is
  `for (j = i; j-- > domain_start; )` (scheduleProgram:589). The body runs with
  `j` already decremented, so the *first* index inspected is `i-1` and the *last*
  is `domain_start` (inclusive). So the entire domain `[domain_start, i)` is
  covered, including its first instruction. No off-by-one. (I initially mis-counted
  this; re-deriving operand-by-operand shows it is correct.)
- **Back-to-back CBRANCHs / empty domain:** domain for CBRANCH at `i2`, when
  `i2 = i1+1`, is `[i1+1, i1+1)` = empty. Scan finds nothing; `is_anchor` stays
  false. Correct — and harmless, because `h_CBRANCH(i1)` already reset
  `reg_changed_offset[creg]` to `i1`'s code position, so the JIT target for `i2`
  is `i1`'s code pos, matching the interpreter's `target = i1`. Consistent.
- **No explicit anchor found in a non-empty domain:** only happens when the
  last writer of `creg` in `(prev_cbranch, i)` is itself a CBRANCH on `creg`
  (outside the scan, since the scan excludes the domain start's predecessor). The
  CBRANCH reset again makes `reg_changed_offset[creg]` equal that earlier CBRANCH's
  code pos, matching the interpreter. Safe.
- **`creg` normalization:** the anchor scan reads `program(i).dst` (scheduleProgram:588),
  but `emitPrologueMix` already ran `instr.dst %= RegistersCount` (line 690)
  *before* `scheduleProgram` (line 693), so `creg` is normalized and matches
  `computeFootprint`'s normalized `dst8` bit. Consistent.

**Conclusion: anchor logic is sound. No failure scenario constructible here.**

---

## 2. src==dst exclusion — VERDICT: necessary per your evidence, but the stated
   mechanism is wrong/unknown (open question)

### The doc's claim
Lines 281-304 claim several handlers (h_ISUB_R, h_IMUL_R, h_IXOR_R, h_IROL_R)
materialize the immediate into shared scratch **x20** when `src==dst`, and that
this x20 use is "invisible to the register/memory hazard model ... x20 is a
physical ARM64 register outside that space," causing a cross-instruction race.

### What the code actually shows
Every src==dst special case writes x20 and consumes it **within the same
handler's own emission**, with no other instruction between:

- h_IMUL_R src==dst (jit:1327-1334): `emitMovImmediate(x20=src, imm)` then
  `mul dst,dst,x20`. Adjacent.
- h_IXOR_R src==dst (jit:1478-1485): `emitMovImmediate(x20, imm)` then
  `eor dst,dst,x20`. Adjacent.
- h_ISUB_R src==dst (jit:1285-1297): MOVZ x20 / `add dst,dst,x20` (or
  emitAddImmediate which itself writes x20 then uses it adjacently). Adjacent.
- h_IROL_R src==dst (jit:1544-1547): takes the `ROR_IMM` path — **no x20 at all.**

A swap reorders *whole handlers* in emission. Inside handler A, x20 is written
and read back-to-back; the very next emitted instruction is handler B's *first*
emit, which may clobber x20, but A's critical section is already complete. There
is **no observable cross-instruction race on x20**: the read always immediately
follows the write within one handler.

### The doc is also factually wrong about IROL_R
Lines 283 and 302-304 list `h_IROL_R` as using x20 on the src==dst path. The code
does the opposite: `h_IROL_R` uses x20 **only when `src != dst`**
(jit:1534-1543); when `src == dst` it uses `ROR_IMM` with no x20. That the
mechanistic write-up is wrong about a listed instruction is itself a signal that
the x20 theory was never nailed down (which the doc concedes at lines 296-301).

### What the swap actually does to a src==dst instruction
For IMUL_R/IXOR_R/ISUB_R with src==dst, `computeFootprint` still records
`int_read = (1<<dst)|(1<<src) = (1<<dst)`, `int_write = (1<<dst)` — i.e. the same
footprint as `src != dst`. So the hazard model already blocks any swap where the
neighbor shares register `dst`. The *only* case where a src==dst instruction moves
relative to an unrelated neighbor is when it shares **no** register with P/Q — and
in that case there is no x20 dependency on the neighbor either. So the hazard model
already covers the dataflow; the additional `q_src_eq_dst`/`r_src_eq_dst` exclusion
(jit:616-617, 621) is **redundant with respect to the x20 story** and cannot be
explained by it.

### So why does your bisection say it's needed?
You isolated (lines 291-295) that excluding src==dst from Q/R removes the
divergence. Two possibilities, and I cannot tell them apart from the code:

1. **The exclusion is more conservative than necessary.** Removing it would be
   safe, but the true trigger of your original divergence was something else
   (e.g. a shape the 450-pair test happened to hit only when a src==dst
   instruction was also present), and the src==dst exclusion was a coincidental
   mask. **If this is true, the exclusion is over-broad and costs some legitimate
   stall-hiding opportunities — acceptable, but worth knowing.**
2. **There is a genuine hazard that the src==dst flag happens to correlate with,**
   which the hazard model misses for a reason I haven't found. If so, the exclusion
   might be an *incomplete* band-aid: there could be sibling swap shapes (with
   `src != dst`) that hit the same underlying bug and are **not** excluded.

### Recommendation (highest priority)
Do not remove the exclusion. But **re-derive the true mechanism** before treating
the scheduler as fully understood. Concrete next steps:
- Build a synthetic micro-test that drives `scheduleProgram()` directly (not via
  blake2b-derived programs) with an *adversarial* fixed program: a long-latency P,
  then a src==dst IMUL_R (with a non-power-of-2 immediate so it's not NOP'd), then
  an independent R; force the swap to fire; and compare JIT vs interpreter for that
  exact program. If it **does not** diverge, the exclusion is (1) above.
- If it **does** diverge, instrument the exact emitted code to see what differs —
  that will reveal the real cause (e.g. a `reg_changed_offset`/`literalPos` side
  effect the footprint model misses) and let you tighten the exclusion to its true
  necessary-and-sufficient condition, closing possibility (2).

Note: `h_IMUL_RCP` main-path (jit:1419) also special-cases on `getImm32()` and is
*not* covered by the src==dst exclusion, yet the doc and your tests treat it as
safe. I confirmed it is safe (see §3) — but it is a sibling "immediate-derived /
position-sensitive" path, so whatever the true src==dst cause is, verify it does
not also apply to `h_IMUL_RCP` main-path under swapping (my analysis says no,
because its reciprocal is written self-contained at emit time).

---

## 3. Superscalar IMUL_RCP exclusion — VERDICT: correct

### The hazard (lines 465-492)
`generateSuperscalarHash` (jit:988-994) lays IMUL_RCP reciprocals into a pool in
**program order**, then consumes them with a sequential `literal_pos += 8` pointer
per IMUL_RCP in **emission order** (jit:1045-1047). If two IMUL_RCPs swap relative
to each other, the k-th emitted reads the k-th *program-order* reciprocal → wrong
multiplier.

### Why excluding IMUL_RCP from Q or R is sufficient (and P is fine)
The swap primitive only ever reorders Q and R. The exclusion
(`!q_is_imul_rcp && !r_is_imul_rcp`, jit:657) blocks any swap where Q or R is an
IMUL_RCP. Therefore:
- No two IMUL_RCPs can ever swap with each other (that would require one of them to
  be Q or R — blocked).
- An IMUL_RCP as **P** never moves (scheduleProgram/scheduleSuperscalarProgram both
  only move Q and R; P is emitted first and stays). The doc's worry "can IMUL_RCP
  end up as P and still break two swaps down the line?" — **no**: if P is IMUL_RCP,
  Q and R are non-IMUL_RCP (else blocked), so the swap only reorders two
  non-IMUL_RCP instructions; the IMUL_RCP subsequence in emission order is
  byte-for-byte the same as in program order. Its relative order is preserved, so
  `literal_pos` consumption matches the program-order pool. **Safe.**
- Chained swaps: after a firing swap the window advances by 3 and never re-touches
  the just-placed positions, so no IMUL_RCP can be nudged across another IMUL_RCP
  over multiple swaps (any such nudge would require a blocked Q/R=IMUL_RCP window).

### Why the *main-path* h_IMUL_RCP has no such problem (confirmed)
Main-path `h_IMUL_RCP` (jit:1419-1458) does **not** use a pre-laid pool. It
computes `literal_id` from its own call count, **writes its own reciprocal** into
`code + literalPos` at emit time (jit:1431-1435), and uses the fixed
`literal_regs[literal_id]` (x30, x29, …). Because the reciprocal value is written
at emit time *in emit order*, and the assigned physical literal register is also a
function of emit-order count, each IMUL_RCP always multiplies by its *own*
reciprocal regardless of where it lands in emission order. So even swapping two
main-path IMUL_RCPs is self-consistent. This matches the doc's claim (lines 478-484)
and I verified it independently.

**Conclusion: IMUL_RCP superscalar exclusion is correct and complete. No failure
scenario.**

---

## 4. Other transient / pool / pre-pass patterns — VERDICT: checked, no hazard,
   one maintenance hazard

### 32-bit immediate literal pool (`emitMovImmediate`, jit:1093)
The pool at `ImulRcpLiteralsEnd` is indexed by `num32bitLiterals`, written at emit
time, and read via `ldr`/`smov`/`umov` with the *current* `num32bitLiterals` baked
in at emit time. Because both write and read positions are computed from the live
counter at emit time, emission order is irrelevant — the pool is self-consistent
under reordering. No hazard.

### Superscalar path defuses a latent pool hazard by construction
`generateSuperscalarHash` sets `num32bitLiterals = 64` (jit:969) before emitting.
`emitMovImmediate`'s pool branch is `if (num32bitLiterals < 64)` (jit:1104), which
is therefore **never taken** on the superscalar path — IXOR_C7..9 etc. always use
the `MOVN/MOVZ/MOVK` via `tmp_reg = x12` (jit:1029-1035), self-contained per
instruction. So there is no pool-ordering hazard in the superscalar region. Note
this only works *because* `num32bitLiterals` starts at 64 there; if that line
regresses to 0, the superscalar immediate loads would suddenly use the shared pool
and become order-sensitive. Worth a comment guarding it.

### `resolveInstructionType()` — single point of failure (maintenance hazard)
`scheduleProgram` resolves each opcode to an `InstructionType` via a **hand-written
if-chain** (jit:539-571) by comparing `engine[opcode]` pointers, while `engine[256]`
is built by the macro `INST_HANDLE` (jit:1857+) from `instruction_weights.hpp` —
the *same* source as `vm.cpp`'s `kCompileHandlers`. Today they are in sync (all 30
main opcodes + 14 superscalar opcodes covered), so the hazard model is accurate.
But the two are **not mechanically linked**: if someone adds a new opcode to
`engine[]` but forgets the matching `if (h == &JitCompilerA64::h_X)` line,
`resolveInstructionType` returns `NOP` for it → `computeFootprint` yields an all-zero
footprint (no read/write/barrier bits) → the scheduler would treat that instruction
as having **no hazards** and could permit an unsafe swap past it. This is exactly
the "silent wrong hash" class. Recommend either (a) deriving `resolveInstructionType`
from the same `instruction_weights.hpp` table as `engine[]`, or (b) a
compile-time/assert check that every non-null `engine[opcode]` maps to a known type.

### `emitAddImmediate` / `emitMovImmediate` x20 use on the main path
Same self-contained pattern as §2: x20 written then consumed within one call.
No cross-instruction exposure. The `num32bitLiterals` pool on the main path resets
to 0 in `emitPrologueMix` (jit:676) per `generateProgram`, decoupled from the
superscalar `=64` setting, so no cross-contamination between the two emission
passes.

---

## 5. Off-by-one / index safety — VERDICT: correct

- `scheduleProgram` (jit:618) requires `fp[i].is_long_latency && i + 2 < size`
  before any swap, so `i+1`/`i+2` are always in range when accessed.
- `is_anchor[i+1]`/`is_anchor[i+2]` are checked (jit:620) — exactly the two indices
  that can become Q/R. `is_anchor[i]` (P) is intentionally not needed (P never moves).
- `scheduleSuperscalarProgram` (jit:656) likewise guards `i + 2 < size` and checks
  `q_is_imul_rcp`/`r_is_imul_rcp` at `i+1`/`i+2`.
- The `i += 3` stride after a firing swap skips windows starting at `i+1`/`i+2`. This
  can *miss* a legitimate optimization but never performs an unsafe reorder — skipped
  windows only forgo a swap, they do not move already-placed instructions again. Safe.
- Every swapped index had its footprint computed in the preamble loop (jit:575-578
  for main; jit:645-648 for superscalar). No uninitialized footprint is ever read.

---

## 6. Interaction between the two schedulers — VERDICT: none

`scheduleProgram` runs inside `emitPrologueMix` (main VM program, jit:693) and
mutates `reg_changed_offset` (per-hash, reset at jit:679). `scheduleSuperscalarProgram`
runs inside `generateSuperscalarHash` (jit:1000) and mutates `literal_pos` (set at
jit:997). They are:
- different functions, called at different times (program compile vs. cache init),
- different code regions (main program loop vs. dataset-derivation stubs),
- disjoint state (`reg_changed_offset` vs `literal_pos`; `num32bitLiterals` is reset
  appropriately in each path — 0 in `emitPrologueMix`, 64 in `generateSuperscalarHash`).

No shared mutable state crosses between them. **No interaction hazard.**

---

## 7. What the existing stress tests would NOT sample (coverage gap)

The tests pass on-device (your evidence). Given §1/§3/§5 show the *logic* is sound,
the residual risk is a program shape the finite sampling missed. Concretely:

- Main stress: 3 seeds × 150 inputs = 450 programs; each input derives a distinct
  program via blake2b (vm.cpp `run` → `run(tempHash)`), so 450 distinct shapes. But
  RandomX programs are 256 instructions from a weighted opcode distribution — 450
  shapes is a thin slice of the space of *rare* shapes (e.g. multiple CBRANCHes whose
  target register is written by a src==dst instruction in a specific position, or
  back-to-back CBRANCHs with a long-latency P wedged between).
- Superscalar stress: 100 seeds × 2 inputs = 200; superscalar programs are per-seed
  (cache), so effectively ~100 distinct superscalar programs. Rarer still for
  "two IMUL_RCPs adjacent to a long-latency P" shapes.

Neither test exercises the **adversarial** program shapes most likely to expose a
subtle scheduler bug. Recommend adding a **direct unit harness** that calls
`scheduleProgram`/`scheduleSuperscalarProgram` on hand-crafted programs (not
blake2b-derived) covering: multi-CBRANCH domains with the anchor at the first/last
slot; back-to-back CBRANCHs; a src==dst IMUL_R as P with unrelated Q/R; two adjacent
IMUL_RCPs with a long-latency P; and `domain_start` exactly one past a CBRANCH with
the anchor immediately there. This decouples scheduler verification from the
(opaque, weighted) program generator.

---

## Concrete failure scenarios requested — status

| # | Concern | Constructible failure scenario? |
|---|---------|--------------------------------|
| 1 | CBRANCH anchor boundaries (first/last/back-to-back/empty domain) | **No** — verified sound, see §1 |
| 2 | src==dst x20 race | **No** — stated mechanism does not hold; true cause unknown, see §2 (highest-priority open question) |
| 3 | IMUL_RCP as P two swaps later | **No** — verified safe, see §3 |
| 4 | Other transient/pool/pre-pass patterns | **No** live hazard found; one maintenance hazard in `resolveInstructionType`, see §4 |
| 5 | Off-by-one / index safety | **No** — verified, see §5 |
| 6 | Cross-scheduler interaction | **No** — disjoint state, see §6 |

The one item I cannot close is **#2**: the exclusion is empirically required but
mechanistically unexplained (and the doc's explanation is demonstrably wrong about
IROL_R). Until the true mechanism is derived, treat the scheduler as "correct under
the src==dst exclusion, but not yet fully understood" — and prioritize the synthetic
micro-test in §2 to distinguish "over-conservative" from "incomplete band-aid."

---

## Suggested follow-ups (no code changes made)

1. Synthetic micro-test for the src==dst swap (§2) to re-derive the true mechanism.
2. Guard `num32bitLiterals = 64` in `generateSuperscalarHash` with a comment, since
   lowering it would silently make superscalar immediates order-sensitive (§4).
3. Mechanically link `resolveInstructionType` to `engine[]` (or add an assert) so a
   future opcode addition cannot silently collapse a footprint to NOP (§4).
4. Add a direct `scheduleProgram`/`scheduleSuperscalarProgram` unit harness with
   adversarial programs (§7).
