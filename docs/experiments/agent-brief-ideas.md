# Agent brief — armrx AArch64 RandomX: help localize the remaining perf gap

You are being asked to read a codebase and propose **concrete, microarchitecture-grounded
hypotheses** for a performance gap we have *measured but not localized*. This is a research/ideas
task — do **not** edit code unless explicitly asked in a follow-up. Read first, then reason.

---

## 1. What the project is

`armrx` is a clean-room reimplementation of the RandomX proof-of-work hash (the one Monero uses),
with a JIT code generator for **AArch64**. It is a from-scratch implementation — **no code may be
copied from XMRig** (the reference miner); reading XMRig's *technique* is allowed, re-implementing
in armrx's own conventions is required. The goal is to be as fast as XMRig on the same ARM silicon.

Target device: **Qualcomm MSM8929 / Snapdragon 415**, 2×4 **Cortex-A53** @ fixed **765 MHz**
(no cpufreq, firmware clock). In-order, **dual-issue**, **single load/store port**. Integer
`MUL`/`UMULH`/`SMULH` are **multi-cycle and NOT fully pipelined** — dependent back-to-back
multiplies interlock. `LD` is 3-cycle, `LDR` (literal) similar, NEON `umov`/`smov` are
register-to-register.

Mode under test: **light** (256 MiB dataset, hugepages). Both miners use identical mode.

---

## 2. The measured situation (all numbers live-verified on device, 2026-08-04)

Per hash, same core (core 3), both CPU-saturated, clock-consistency-checked (cycles÷elapsed ≈ 765 MHz):

| metric | armrx | XMRig | ratio |
|---|---:|---:|---:|
| instructions / hash | **89.5 M** | 101.4 M | 0.88× (armrx LEANER) |
| cycles / hash | **163.5 M** | 155.0 M | 1.05× |
| IPC | **0.547** | 0.654 | 0.84× |
| H/s (1 worker) | 4.77 | 5.04 | 94.6% |
| H/s (8 workers, projected) | ~24.1 | 28 | ~86% (all of it = SoC weak-cluster asymmetry) |

**Key finding (E19):** the gap is ONE PMU stall class. A53 `other_interlock_stall` (cycles stalled
in the Wr stage for a reason other than load/store/AGU/SIMD — overwhelmingly the integer
multiplier):

| stall class (per hash) | armrx | XMRig | ratio |
|---|---:|---:|---:|
| **other_interlock_stall** | **23.27 M** | 10.96 M | **2.12×** |
| ld_dep_stall | 17.63 M | 19.33 M | 0.91× (armrx BETTER) |
| l1i_cache_refill | 0.19 M | 0.52 M | 0.36× (armrx 2.8× BETTER) |
| l1d_cache_refill | 0.83 M | 0.65 M | 1.28× (+0.18 M, 2% of gap) |
| br_mis_pred | 0.031 M | 0.015 M | 1.99× (+0.015 M, 0.2% of gap) |

`other_interlock_stall` excess = **+12.30 M cycles/hash = 154% of the entire 7.99 M cycle gap**.
Everything else is exonerated (armrx is *better* on load stalls and I-cache). Total interlocks:
armrx 26.7% of all cycles vs XMRig 21.2%.

The superscalar body is ~80% of the work and is dependency-dense by RandomX design:
**1,254 of 3,563 instructions are multiplies (35%)** — `IMUL_R` 775 (21.8%), `IMUL_RCP` 239,
`IMULH_R` 116, `ISMULH_R` 124 (from `--jit-dump`).

---

## 3. The puzzle you are being asked to solve

## 3. The puzzle you are being asked to solve

**XMRig's AArch64 superscalar emitter does NO scheduling** (plain program order) yet has **half** armrx's multiplier interlocks — and armrx *does* schedule and still loses. Same opcodes, same multiply count, 2.12× the interlocks. So ordering alone isn't it, and C* immediate emission choice isn't it (both emit the same MOVZ/MOVN+MOVK). **The excess must be in how multiply/multiply-high chains are sequenced relative to their sources, or in another region armrx emits differently** (hot-temp register serializing independent chains; main-VM body's ~10%; Blake2b; scratch reads). We named the stall class but not the place.

**IMPORTANT — do NOT re-propose "disable / turn off the superscalar scheduler" (E22, 2026-08-04):**
three external agents independently ranked "the scheduler clusters multiplies (P,R,Q hoists an
independent MUL into the multiply's slot)" as hypothesis #1, and it was tested and **falsified**.
Disabling `scheduleSuperscalarProgram` (identity order) made things **worse**: instructions rose
89.5→101.4M, `other_interlock_stall` 23.3→24.7M, cycles +20.4M. The scheduler is *helping*; the brief's
own framing ("XMRig schedules nothing yet wins → scheduling is the bug") was a red herring — XMRig's
unscheduled order is the RandomX *generator's natural* spacing, a different distribution from
"armrx minus its scheduler." **The live hypotheses are upstream of emission:** (a) armrx's
*generated* superscalar program order differs from XMRig's — see `src/superscalar.cpp`
(`mulCount`, `fetchNext`, `allowChainedMul`) — this is the top untested lead; (b) main-VM multiply
handlers / `emitMemLoad`; (c) C* density stripped "free" MAC-latency padding; (d) `x12` WAR (the
footprint tracks only VM regs r0–r7, blind to the shared `x12` temp).

---

## 4. Files to read (in this order)

1. `docs/experiments/perf-tracking.md` — the full lead tracker. §0 standing facts, §1 the
   CLOSED/DEAD lead table, §2 the open-lead detail (E3c, E19, E20, E21). **This brief is a
   summary; the tracker is the source of truth.** Do not re-propose anything in the dead table.
2. `docs/experiments/next-iteration-plan.md` — the open-leads framing and what "localize the
   `other_interlock_stall` excess to an emitter region" means concretely.
3. `src/jit_compiler_a64.cpp` — the JIT. Key regions:
   - `scheduleProgram()` (main VM, ~line 640-735) — has distance-2 AND distance-3 reorder rules.
   - `scheduleSuperscalarProgram()` (~line 746-775) — only distance-2 (E20 tried adding distance-3,
     NULL, reverted).
   - `emitCpoolImmediate()` (~line 1318-1366) — C* immediate via inline literal pool (`LDR`+ALU).
   - `emitMovImmediate()` / `emitAddImmediate()` — register moves and add-imm.
   - the superscalar opcode handlers (the big `switch` that emits `IMUL_R`, `IMULH_R`, `ISMULH_R`,
     `IADD_R*`, `IXOR_R*`, `IROR_C`, `IMUL_RCP`, etc.).
4. `include/armrx/jit_compiler_a64.hpp` — scheduler interface (`scheduleSuperscalarProgram` is
   virtual; `ScheduleConfig` toggles extensions).
5. (Technique reference only, clean-room — **DO NOT copy**) `~/xmrig/src/crypto/randomx/jit_compiler_a64.cpp`
   on the device, especially `generateSuperscalarHash`, `emitMovImmediate` (note `num32bitLiterals=64`
   pre-fill at line 337), and the superscalar `switch`.
6. `src/blake2b.cpp` — Blake2b (armrx is NEON; XMRig is scalar C on AArch64, so armrx already wins here).

---

## 5. CLOSED / DEAD leads — do NOT re-propose

- **Instruction-count / density gap (E3c, W1-4 "+26%", session "+14%"/"+35%").** RETIRED. armrx
  emits **12% FEWER** instructions and loses on IPC. Not a density problem.
- **C* immediate density (E3c).** CLOSED — armrx's `LDR`-pool form is 2 instr, actually *denser*
  than XMRig's 3-instr MOVZ+MOVK fallback. No gap here.
- **`umov`/NEON vs `LDR` pool (E21).** CLOSED as false premise — XMRig's `umov` branch is
  unreachable on the superscalar path (64-slot pre-fill). Neither miner uses `umov` there.
- **C++ driver overhead / `worker_loop` (E17).** Artifact of profiling a stripped binary. 97.9%
  of cycles are in the JIT buffer. Nothing to remove.
- **Distance-3 superscalar peephole (E20).** IMPLEMENTED, measured NULL (−0.2%), REVERTED. Fires
  only 0.73% of slots. **Do not widen the peephole (distance-4/5) — same ceiling.**
- **LTO off, `-mtune=cortex-a53`, PGO, PRFM, dual-issue NOP padding, memory-op (`*_M`) reordering.**
  All measured null or forbidden (`*_M` reordering caused a real JIT/interpreter divergence once).
- **8-worker "unexplained 2.4 H/s".** Arithmetic error (compared 1 fast core to 8-worker avg).
  Reconciled to ~86%, same as per-core 95% through a different core mix. No second gap.

---

## 6. Constraints & conventions (respect these)

- **Clean-room:** read XMRig for *technique* only. No XMRig code in the armrx tree, ever.
- **A53-specific:** this is an in-order core. Out-of-order reasoning (register renaming hiding
  hazards, deep reorder buffers) does not apply. Think in terms of *static* dependency distance
  between a multiply and its consumer, port conflicts on the single memory pipe, and hot-temp
  register reuse serializing otherwise-independent chains.
- **Don't edit `src/` or `include/`** in this ideas pass. Propose; don't implement.
- **The shipped build is cross-compiled** with `cmake/toolchain-aarch64-musl.cmake` (GCC 16.1.0,
  +7.9% over the device GCC 15.2.0 build — a toolchain effect, not code). Don't propose changing
  the build just to chase perf; that's already settled.
- **Measurement harness if you want to verify a hypothesis:** `bench_armrx --full-hash-only
  --perf-ready` gates a 500-hash window (no counter quantization, no init contamination);
  `--jit-dump` gives the per-opcode byte/instruction census; the A53 PMU exposes `other_interlock_stall`,
  `ld_dep_stall`, `l1d_cache_refill`, `l1i_cache_refill`, `br_mis_pred`, `l2d_cache_refill`.
  `perf record -g` + `perf annotate` works on a `-g` cross build.

---

## 7. What a good answer contains

1. **A ranked list of hypotheses** for the 2.12× multiplier-interlock excess, each tied to a
   specific A53 pipeline behavior (e.g. "armrx's `IMUL_R` result is consumed 1 instruction later
   than XMRig's because of how `emitAddImmediate` sequences the temp register", or "register
   allocator reuses x12/x13 for C* immediates, forcing a 3-cycle stall on independent chains that
   XMRig's per-lane `umov`... wait, XMRig doesn't use umov — so what DOES XMRig do that we don't?").
2. **Exact emitter locations** to inspect (function + approximate line, or the opcode handler).
3. **The cheapest single measurement** that would confirm or kill the top hypothesis (e.g. a
   region-tagged `perf annotate` per opcode class, or a `--jit-dump` diff isolating where the two
   miners diverge in multiply-to-consumer distance).
4. **A clear statement of what is NOT worth trying** (and why), so we don't burn device time.

Novelty matters more than completeness. We have exhausted the obvious; we need a fresh read of the
*structure* of the emitted code, not another micro-tuning suggestion.
