# armrx AArch64 RandomX: Remaining 8-Worker Gap Audit

**Date:** 2026-08-03  
**Scope:** post-E24, MSM8929 / Cortex-A53, clean-room comparison  
**Constraint:** source and technique audit only; no source changes

## Executive Finding

The remaining code-level suspect is still the main-VM scratchpad path, but the
W3-2 failure is not root-caused by the evidence currently in the tree. The
strongest conclusion is narrower:

- `emitMemLoad()` makes every integer scratchpad operation a dependent
  `address add -> mask -> LDR -> integer operation` chain
  (`src/jit_compiler_a64.cpp:1386-1416`). The consumer is immediately after the
  load in `h_IADD_M`, `h_ISUB_M`, `h_IMUL_M`, `h_IMULH_M`, `h_ISMULH_M`, and
  `h_IXOR_M` (`1465-1715`). This is a direct A53 load-use exposure and remains
  the highest-value code hypothesis.
- XMRig's AArch64 reference lowering inspected in
  `scratch_vm_study/upstream_rx/src/jit_compiler_a64.cpp:525-553` has the same
  integer shape. Therefore, “XMRig hoists the address calculation” is not a
  supported explanation. Any advantage must come from surrounding instruction
  order, not a different integer `*_M` sequence.
- The W3-2 result does **not** establish a superscalar-path hazard. The
  superscalar generator has no memory opcodes (`src/jit_compiler_a64.cpp:487-497`),
  and `test_jit_superscalar_scheduler_stress` still executes the main VM JIT
  before comparing hashes (`tests/test_jit_superscalar_scheduler_stress.cpp:45-53`).
  Its seed-4 failure is additional main-VM coverage, not proof that
  `scheduleSuperscalarProgram()` diverged.

At eight workers the same per-worker deficit is magnified by the fixed fast/
weak cluster mix. The available evidence does not show a separate software
scaling defect, so the cheapest useful measurement remains a one-worker
opcode/PMU attribution, not another eight-worker run.

## Ranked Hypotheses

### 1. Immediate scratchpad load-use serialization

**Confidence: high that this is real; medium that it explains the residual
armrx/XMRig delta.**

For `src != dst`, `emitMemLoad<20>()` emits an immediate address calculation,
mask, and register-indexed `ldr x20, [x2, x20]`; the handler then consumes x20
on the next instruction. On this A53, the LDR result has about three cycles of
latency and there is one load/store port. The independent work available to
fill that gap is not inside the handler. Integer multiply variants make the
next instruction a multi-cycle `MUL`, `UMULH`, or `SMULH`, but the same
load-to-use issue exists for ADD/SUB/XOR.

Relevant locations:

- `emitMemLoad()` at `src/jit_compiler_a64.cpp:1386-1416`
- `h_IADD_M()` / `h_ISUB_M()` at `1465-1527`
- `h_IMUL_M()` / `h_IMULH_M()` / `h_ISMULH_M()` at `1549-1625`
- `h_IXOR_M()` at `1700-1715`
- FP analogue `emitMemLoadFP()` at `1419-1445` and `h_FADD_M()` /
  `h_FSUB_M()` / `h_FDIV_M()` at `1797-1899`

The existing 64.3% immediate-consumer classification and main-VM IPC penalty
are exactly the expected signature. This is a structural emission problem,
not an instruction-count problem.

### 2. Surrounding-order difference, not a different `*_M` lowering

**Confidence: medium.**

XMRig's integer lowering is structurally identical, so compare the instruction
before each `*_M` address calculation and the instruction after its consumer.
armrx's `scheduleProgram()` can reorder whole VM handlers using a register-only
model (`640-738`), while XMRig's inspected AArch64 path emits the program in
original order (`upstream_rx:141-150`). A different neighboring sequence can
change how much unrelated work lies between a preceding operation and the
scratchpad consumer, even when both emit the same four-instruction core.

This is the only plausible XMRig technique difference found in the supplied
reference. Do not infer that plain program order is automatically better: E22
and the E24 result show that spacing effects are target- and region-specific.

### 3. Address-generation issue-slot pressure

**Confidence: medium-low, but cheap to test.**

`emitAddImmediate()` may emit one or two ADD-immediate instructions, or a
materialization plus ADD for a large immediate (`1351-1383`). Together with
the mask and register-indexed load, this consumes ALU and the sole memory port
before the dependent operation can issue. A non-scheduler structural change
could help only if it reduces address instructions or moves address work into
slack created by a preceding independent operation. A generic reorder is
unsafe until the W3 issue is understood.

### 4. FP scratchpad lowering as a smaller separate component

**Confidence: low for the dominant gap.**

The FP path uses x19 for address formation, loads `d28`, then performs sign
extension and conversion before the FP consumer. It is longer than the integer
path, but the recorded hot signature specifically identifies scratchpad loads,
not necessarily integer-only loads. It should be opcode-tagged rather than
assumed to be a major contributor.

## W3-2 Divergence: What Is and Is Not Proven

The attempted change marked memory-reading opcodes `is_long_latency`. That only
made them eligible as the fixed `P` trigger in `scheduleProgram()`; the emitted
handler remained intact. The swap is `P,Q,R -> P,R,Q`, and the existing checks
require no register hazard, no barrier/anchor crossing, and no memory-memory
pair (`computeFootprint():385-457`, `hasHazard():465-471`). The ordinary source
and destination register model therefore does not expose an obvious error.

Several proposed explanations are ruled out by the code:

- **Scratchpad address aliasing between P and R:** R must be non-memory when P
  is memory, because `hasHazard()` rejects every memory-memory pair.
- **x20 lifetime:** integer `*_M` handlers write and consume x20 within one
  indivisible handler (`emitMemLoad()` followed by the operation). VM registers
  map to x4-x7 and x12-x15, so x20 is not a VM register.
- **AES state:** no AES operation is emitted by these handlers, and CFROUND is
  a hard barrier.
- **Superscalar reciprocal-pool order:** the main scheduler does not emit the
  superscalar body, and `scheduleSuperscalarProgram()` has a separate footprint
  model with no memory operations.

The exact missing hazard is therefore **not identified** by the current
repository. The stress result says the abstract “safe swap” proof is incomplete,
not which semantic resource invalidates it. In particular, the line in
`docs/experiments/w3-2-swap-budget-bisect.md:108-110` calling out a memory op in
the anchor/P position is a hypothesis, not a demonstrated mechanism.

The existing bisection did not root-cause this because `ARMRX_MAX_SWAPS` gates
only `scheduleProgram()`, while the stress failure was reported by a test whose
name suggests the superscalar scheduler. More importantly, the test has no
per-swap identity or opcode trace, and the broad stress seed that fails is not
covered by the small budget sweep. A valid root-cause experiment must log the
actual main-program swap tuple `(P,Q,R)`, emitted offsets, and the first hash
stage that differs for the failing seed/input. Until that exists, claiming
“address aliasing” or “PC coupling” as the WHY would be speculation.

## Cheapest Confirming Measurement

Use one saturated, one-worker `bench_armrx --full-hash-only --perf-ready` run
with the existing JIT dump and a region/opcode address map, and record the A53
`other_interlock_stall`, `ld_dep_stall`, and cycles events. Attribute samples
inside the main VM buffer to the `*_M` boundary entries from `--jit-dump`, with
the load and immediate consumer counted separately.

Decision rule:

- If `*_M` consumers account for most main-VM cycles and their samples carry
  the load-use/interlock events, hypothesis 1 is confirmed as the next target.
- If `*_M` is hot but the events land mainly on preceding address ADD/mask
  instructions, pursue address-generation issue pressure instead.
- If neither is concentrated, stop changing `emitMemLoad()` and compare the
  surrounding static instruction windows against XMRig; the presumed main-VM
  localization is then incomplete.

This is cheaper and safer than another scheduler experiment: it changes no
code, does not require eight-worker noise, and directly distinguishes the
consumer stall from address setup.

## Safe Follow-Up Direction

If the measurement confirms hypothesis 1, investigate a structural emitter
change that preserves original VM instruction order and memory-op order. The
candidate must either hoist only address work from an earlier independent
instruction or alter the local lowering without moving a memory operation
across another VM instruction. Every candidate must be checked against the
failing stress seed, not only the 16-pair equivalence test.

Do not re-enable generic `*_M` scheduling until a per-swap trace identifies the
missing semantic state and a minimized reproducer proves it.

## Not Worth Trying

- Distance-4/5 peepholes or another scheduler heuristic: the scheduler family
  is exhausted, and W3 already demonstrates an unproved correctness boundary.
- C* density, superscalar scheduling, or reciprocal-pool changes: E24 closed
  the measured multiplier-interlock problem; superscalar code has no scratchpad
  `*_M` lowering.
- PRFM, LDP/STP fusion, hugepages, affinity, PGO, LTO, and `-mtune`: already
  measured null, regressive, structurally impossible, or outside the stated
  parity lever.
- Copying an XMRig lowering: the inspected integer `*_M` sequence is already
  the same. The useful clean-room comparison is instruction context and
  scheduling policy, not source transplantation.

## References

- `docs/experiments/perf-tracking.md`, standing facts and W3/E24 records
- `docs/experiments/memory-op-scheduler-attempt.md`
- `docs/experiments/w3-2-swap-budget-bisect.md`
- `src/jit_compiler_a64.cpp`
- `scratch_vm_study/upstream_rx/src/jit_compiler_a64.cpp` (technique comparison only)
