# What breaks the `*_M` path and why (consolidated failure analysis)

**Date:** 2026-08-05
**Purpose:** single source of truth for planning agents. Read this BEFORE proposing any
`*_M` / main-VM scratchpad optimization. Two independent attempts failed on the exact
same on-device gate; the mechanism is the same root cause class.

---

## 0. The goal we were chasing

armrx is at parity with XMRig at 1 worker (5.12 vs 5.04 H/s) and **95.2% at 8 workers**
(26.65 vs 28 H/s). The remaining ~4.8% (1.35 H/s) is a single PMU stall class,
confirmed by a 1w→8w growth-delta measurement:

| stall/hash | 1w | 8w | Δ |
|---|---:|---:|---:|
| `other_interlock_stall` (E24 multiply) | 10.3 M | 10.5 M | flat (E24 holds) |
| `ld_dep_stall` (`*_M` load-use) | 15.9 M | **25.9 M** | **+63%** under 8w contention |

|Load-dependency stalls at 8w: 25.9M/hash × 26 H/s ≈ **690M/s total across 8 workers**.
Dividing by aggregate core cycles (8 × 765 MHz = 6,120M/s) gives **≈0.11 (≈11% of all
core cycles)** — a real but MODEST tax, NOT "saturating 88%" (that earlier 0.88 figure was
an 8× error: aggregate stalls were divided by one core's cycles). The +63% growth
(15.9→25.9M/hash) is a per-hash count delta and is correct. Whether the stall is
L1-bubble-accumulated or L2/contention cannot be settled from the committed numbers (they
came from 60 s `--mine` windows, which this project's own discipline flags as
non-authoritative). The stall is real; its magnitude-per-op is not precisely characterized.

The lever: hide the 3-cycle load-use stall (or reduce `*_M` memory traffic) WITHOUT
breaking correctness.

---

## 1. The two failures (same gate, same class)

### W3-2 (earlier, scheduler reorder)
- Marked `*_M` as `is_long_latency` so `scheduleProgram` reordered it as a distance-N
  anchor, interleaving independent instructions between the load and its consumer.
- **Result:** `test_jit_scheduler_stress` divergence — JIT hash ≠ interpreter hash.
  Reverted unidentified.

### E26 (2026-08-05, Reasonix — hoist)
- Hoisted each `*_M` op's address `add`/`and` **into the slack of the preceding VM
  instruction** (new `emitMemLoadAddr<>`; `memAddrHoisted_` flag; backward scan with
  CBRANCH-anchor / replay-domain guards; `ldr` never moves).
- Gated on device (cross build):
  - `test_jit_equivalence` → **16/16 byte-identical PASS**
  - `test_jit_scheduler_stress` (450 pairs) → **SEGFAULT (exit 139)** ~5 min in, crash
    inside the RWX JIT code buffer (generated-code corruption, stack unwalkable).
  - Pre-E26 baseline passes the same 450-pair gate → crash is E26, not the environment.
  - Reverted (`git checkout`); tree clean.

**Pattern:** both changes pass the 16-pair KAT but die on the **450-pair
`test_jit_scheduler_stress`** (and the 200-pair superscalar suite). That gate is the
real correctness oracle for this region. Any new proposal MUST be gated on it (plus the
200-pair suite), not just KAT.

---

## 2. The root-cause class (NOT memory aliasing)

Both audits and both failures point at **CBRANCH replay-domain equivalence**, not
memory hazards. Mechanism:

- armrx is a **branchless** RandomX VM: `CBRANCH`/`CFROUND` are implemented as
  *branchless* code, but on a **taken** branch the JIT must **re-execute a slice of
  already-emitted code** (because RandomX's CBRANCH is a conditional *modify-register*
  that the spec defines as re-running the branch target's computation).
- The replay target is **code-offset based**, not instruction-index based. The JIT
  records, per destination register, `reg_changed_offset[instr.dst]` = the byte offset
  in the emitted code where the **last writer** of that register started. See
  `h_CBRANCH` at `src/jit_compiler_a64.cpp:1991` (`int32_t offset =
  reg_changed_offset[instr.dst];`) and the anchor bookkeeping in
  `emitPrologueMix` (~lines 822, 841) inside the `scheduleProgram` emit loop.
- On a taken CBRANCH, the JIT branches back to `reg_changed_offset[dst]` and re-runs
  the code from that anchor up to the `CBRANCH`. **The interpreter replays the same
  slice by *instruction index*.** So the JIT and interpreter agree ONLY if the emitted
  byte-range `[anchor_offset, cbranch_offset]` corresponds exactly to the interpreter's
  instruction-index slice — i.e. the *set of ops and their order* in that range is
  unchanged.

**Why `*_M` changes break this:**
- Moving emitted bytes across a CBRANCH replay boundary (or changing the byte-offset of
  the `[anchor, CBRANCH]` range) silently desynchronizes JIT replay from interpreter
  replay. The op *semantics* may be identical, but the *replay window* now covers
  different bytes → wrong re-execution → divergence (W3-2) or, if the hoist lands bytes
  such that the re-executed slice reads uninitialized/garbage registers, a **crash in
  the JIT buffer** (E26 segfault).
- The hazard model is **register-only** (`hasHazard` blocks memory-memory via
  `writesX20()`-style conservative checks), but it **misses the replay-domain semantic
  class entirely**. A change can be "safe by register analysis" and still break replay.

---

## 3. Why the "safe" guards failed anyway

E26 added a CBRANCH-anchor stop + replay-domain guard (refuse to hoist across a
CBRANCH/CFROUND barrier; land the add/and at-or-after the domain anchor). It still
segfaulted. Likely reasons (not root-caused — the task forbade blind iteration):

1. **Sub-boundary liveness:** the anchor marks the *last writer's start offset*, but the
   hoisted `add`/`and` may need a register (e.g. `x20`, the scratchpad base temp) that
   is *written by an op inside* the replay range. If the hoist emits bytes before that
   writer, the re-executed slice's first read of `x20` is stale → crash.
2. **`writesX20()` false negative:** the conservative check may miss a path that writes
   the scratchpad temp, allowing a hoist that corrupts the replay slice.
3. **Multiple CBRANCH anchors / CFROUND:** the "anchor" is per-register; a hoist that
   spans two different registers' anchors lands in a mixed replay domain.

The point for planners: **register/anchor analysis is necessary but NOT sufficient.**
The invariant that must be preserved is *byte-offset replay-window equivalence*, which
is stricter than "no register hazard."

---

## 4. What is KNOWN-GOOD (don't regress or discard)

- **E24** (C* immediate → 3-instr `MOVZ/MOVN+MOVK`): shipped, +7.1% H/s, interlocks
  below XMRig. This closed the 1w gap.
- **E25** (skip imm==0 `ADD` in `emitMemLoad`): shipped, equivalence-safe (16/16 + both
  stress suites), null H/s — confirmed the gap is NOT instruction count.
- `emitMemLoad` current safe form (lines 1386–1420): `add`/`and` → `ldr` → op, with
  E25's imm==0 fast path. Any new approach must preserve this for the KAT/stress gates.

---

## 5. What a NEW approach must satisfy (hard constraints)

1. **Zero change to the `[anchor_offset, cbranch_offset]` replay window semantics.**
   If you move emitted bytes, the set of ops + their order in every replay window must
   be byte-for-byte the same as before *for the interpreter's replay index slice*. The
   safest way: **don't move bytes across CBRANCH/CFROUND boundaries at all.**
2. **Don't mark `*_M` long-latency / don't extend scheduler distance-N reorder to
   memory ops.** That's W3-2; it diverged.
3. **The `ldr` (memory op) never moves** relative to its consumer — memory order must
   be preserved exactly.
4. **Gate on device:** `test_jit_equivalence` (16/16) + `test_jit_scheduler_stress`
   (450 pairs) + `test_jit_superscalar_scheduler_stress` (200 pairs). The 450-pair
   suite is the oracle — KAT alone is insufficient (both failures passed KAT).
5. **If any gate fails: REVERT, record failing seed/input + first-differing hash stage,
   do not iterate blindly.** Report and stop.
6. **Clean-room:** technique only from XMRig; re-implement in armrx conventions. No
   XMRig source in tree.

---

## 6. Angles that are NOT yet exhausted (for planners)

These are *shapes*, not prescriptions — evaluate each against §2/§5 before committing.

- **A. Root-cause first (highest value).** Trace exactly why CBRANCH replay breaks when
  bytes move: what invariant does `reg_changed_offset` actually require? Find the
  precise condition under which a byte relocation preserves replay equivalence. Then any
  fix designed to *preserve that invariant* is safe by construction. (Removes the blind
  spot; both prior failures were "try a change, see if replay still holds.")
- **B. Reduce stall without moving bytes.** Emit *more independent work* between a load
  and its consumer using *already-emitted* adjacent ops (VM-level op fusion / local
  unrolling) with **zero emission-order change** — i.e. change what's computed, not
  where bytes sit. Only safe if the replay window is untouched.
- **C. Different ISA lever.** Load the scratchpad value via NEON (`ldr` into a vector
  reg + `ins` into a scalar) — the NEON LSU may not contend with the integer LSU the
  same way, potentially hiding the integer load-use bubble on a different pipe.
  Speculative; clean-room re-impl required.
- **D. Accept-and-document as a hard limit.** If the evidence shows the in-order A53
  simply cannot hide the load-use bubble without breaking the replay invariant, that is
  a valid, evidence-backed conclusion (parity = 95.2%). Document it; stop.

---

## 7. Files / references

- `src/jit_compiler_a64.cpp`: `emitMemLoad` (1386), `h_CBRANCH` (1976, replay anchor
  `reg_changed_offset` at 1991), `emitPrologueMix` anchor bookkeeping (~822, 841),
  `scheduleProgram` (640–744), `hasHazard` (~471), `*_M` handlers
  `h_IADD_M`/`h_ISUB_M` (1465–1527), `h_IMUL_M`/`h_IMULH_M`/`h_ISMULH_M` (1549–1625),
  `h_IXOR_M` (1700–1715).
- `docs/audits/opencode_20260803_audit.md`, `docs/audits/github_copilot_2026-08-03_audit.md`
  — the two post-E24 audits (Hypothesis #1 = `*_M` load-use stall; root-cause = CBRANCH
  replay, not aliasing).
- `docs/briefs/2026-08-04-star-m-fix.md` — original brief; §8 records E26 exhaustion.
- `docs/experiments/perf-tracking.md` — §0 (parity numbers), confirmed-measurement
  (1w→8w ld_dep_stall +63%), E25 soak, E26 record + stall-arithmetic correction.
- Build/test: cross `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake && cmake --build build-cross -j$(nproc)`. Tests run on device
  `mechres@192.168.10.156`, pinned `taskset -c 1-3`. Stress suites are SLOW (~20–35 min
  each) — budget for it.
