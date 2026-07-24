# Peephole JIT Coalescing — Plan (v2, incorporating review)

> **⚠️ Superseded in part by the clean-room boundary (2026-07-24, permanent — see `PLAN.md`
> Phase 6, note after item 13).** This plan's methodology of disassembling XMRig's
> JIT-generated output and diffing it against armrx's (Guiding principle 2, Phase 1's
> "reference dump" steps, and every "compare against XMRig's emitted code" step below) is
> **no longer permitted** — inspecting another miner's compiled output is ruled out, not just
> deprioritized. Black-box comparison (hashrate, `perf stat` counters) remains fine. If this
> plan is ever revived, it must be rebuilt around self-directed analysis of armrx's own
> generated code plus first-principles ARM64 reasoning (the approach `PLAN.md` item 14 now
> prescribes). The text below is kept as historical record, unedited.

**Goal**: Close the ~33% instruction-count gap between armrx and XMRig's
AArch64 JIT output (measured at 64.3B vs 48.2B instructions per benchmark).

**Current state**: The static ASM template (`jit_compiler_a64_static.S`) and
JIT compiler (`jit_compiler_a64.cpp`) are nearly identical to XMRig's — the gap
comes from years of XMRig's incremental peephole optimizations across hundreds
of small emitted sequences. No single optimization will close the gap; it
requires systematic comparison of per-opcode emitted code.

---

## Guiding principles

1. **Hashrate vetoes** — instruction count is a proxy metric. A change that
   reduces instructions but increases cycles (pipeline stalls, cache pressure)
   is a loss. Hashrate on real hardware is the final arbiter.
2. **Clean-room** — study XMRig's *output* (disassembly), not its source.
   All implementations must be original.
3. **KAT parity before every commit** — no exceptions for a hash function.
4. **Frequency data before optimization** — don't optimize opcodes that
   rarely appear in real programs.

---

## Phase 1 — Infrastructure & Data Gathering

### 1.1 Code dumping with boundary markers
Add a `--jit-dump` flag to `armrx` that:
- Runs one hash with a given seed/key
- Dumps the JIT code buffer to stdout as hex, with **opcode boundary markers**
  (a side-channel offset table written alongside the raw bytes, so each emitted
  instruction sequence can be sliced per opcode)
- Exits without executing

**File**: `src/main.cpp`, new `--jit-dump` flag
**Output**: raw bytes + offset table (start position, opcode ID, length for each)

### 1.2 Instruction-level benchmark with opcode frequency histograms
Add micro-benchmarks that:
- Run a batch of real RandomX programs from random seeds
- Record opcode frequency histograms (count per opcode across all programs)
- Use `perf stat -e instructions` per opcode to measure instruction retired cost

**Key insight from review**: Don't prioritize optimizations by intuition.
IMUL_RCP savings only matter if IMUL_RCP is common. Frequency data stops
you spending a week polishing opcodes that turn out to be rare.

**File**: `tests/bench_opcodes.cpp`

### 1.3 Spot-check: register allocation vs peephole dominance
Pick 2-3 opcodes and compare not just their local instruction sequence but
whether GPR/NEON spill/reload patterns around them differ between armrx and
XMRig. If allocation differences dominate the instruction-count gap, we need
a register allocator overhaul rather than per-opcode peepholing.

**File**: Manual investigation using Phase 1.1 dumps + objdump comparison.

### 1.4 CBRANCH encoding unit test
Add a test that decodes the emitted `bne`/`b` bytes for a CBRANCH sequence
and asserts the computed target address matches the `reg_changed_offset` value.
This turns a "wait 120s for timeout" failure mode into an instant, localized
assertion for any future branch-encoding change. The `extr` instruction for
IROR_R/IROL_R and load-pair coalescing in Phase 3 will also touch encodings.

**File**: `tests/test_jit_encodings.cpp`

### 1.5 Seed-program determinism test
Add a test that compiles the same program twice and asserts identical JIT output.
Catches non-determinism in the JIT compiler.

**File**: `tests/test_jit_determinism.cpp`

---

## Phase 2 — Per-opcode audit (informed by frequency data)

After Phase 1.2, re-prioritize this list based on actual opcode frequency.
The order below is the initial guess, to be replaced by data.

| Priority | Opcode | armrx issue | Est. savings |
|----------|--------|-------------|--------------|
| P0 | **IMUL_RCP** | Emits `mov xN, #imm; umulh` — may fold constant into `movk` | −1–2% |
| P0 | **CBRANCH** | Branchless `bne+b` fix is deployed (imm19=2). Compare against XMRig's approach — they may use different add+test combos | −0.5% |
| P1 | **IROR_R / IROL_R** | armrx uses `rotr`/`rotl`; XMRig may use `extr` (rotate-insert) | −1–3% |
| P1 | **FDIV_M / FSQRT_R** | armrx emits software divide/sqrt; XMRig may use Newton-Raphson | −2–5% |
| P2 | **All memory ops** | `getScratchpadAddress` pattern — XMRig may fold addressing modes | −1–2% |
| P2 | **ADD/SUB immediate** | `emitAddImmediate` emits 1-2 add; XMRig may use fused add+flags | −0.5% |

### Audit process for each opcode

1. Find the `h_` handler in `jit_compiler_a64.cpp`
2. Trace the emitted instructions via `emit32` calls
3. Locate the corresponding opcode in the --jit-dump using boundary markers
4. Disassemble and compare against XMRig's output for the same program
5. If different: implement the improvement → KAT verify → instruction-count
   verify → hashrate verify

---

## Phase 3 — Cross-opcode optimizations

| # | Optimization | Description | Est. savings |
|---|-------------|-------------|--------------|
| 3.1 | **Register allocation reuse** | Track which regs are "dead" after each opcode, avoid spilling. (Spot-check in Phase 1.3 first to see if this dominates the gap.) | −2–3% |
| 3.2 | **Constant folding** | Pre-compute immediates as single `mov` + shifted operand | −1% |
| 3.3 | **Dead instruction elimination** | Skip instructions whose results are overwritten before use | −1–2% |
| 3.4 | **Load-pair coalescing** | Merge adjacent single loads into `ldp` | −1–2% |

---

## Phase 4 — Validation

Each change requires:

1. **KAT parity** — all existing KAT vectors produce identical hashes
2. **Determinism** — same seed → same JIT output (test from Phase 1.4)
3. **Instruction count** — `perf stat` shows reduction for the affected opcode
4. **Hashrate** — **the veto metric**. No regression on real hardware
   (>3 runs, average). A change that reduces instructions but hurts hashrate
   is rejected.

---

## Estimated Timeline

| Phase | Effort | Expected gain |
|-------|--------|---------------|
| Phase 1 (tooling + data) | 2-3 days | — |
| Phase 2 (opcode audit) | 1-2 weeks | −5–10% |
| Phase 3 (cross-opcode) | 2-4 weeks | −5–10% |
| Phase 4 (validation) | Ongoing | — |
| **Total** | **3-6 weeks** | **−15–20%** |

---

## CBRANCH — status update

The earlier `bne+b` fix is already deployed and passing KATs (the hang was caused
by `imm19=1` instead of `imm19=2` in the `bne` offset — since fixed). The current
CBRANCH emits 1 extra instruction vs the original `beq`. Compare against XMRig's
approach in Phase 2 to see if they avoid this entirely with a different encoding.

See `docs/branchless-cbranch.md` for the full post-mortem.

---

## Risks

- **KAT regressions** — Mitigation: run full KAT suite before every commit.
- **Clean-room constraint** — Study XMRig's output, not source.
- **Diminishing returns** — Stop when the gap is ≤5%.
- **Instruction count ≠ throughput** — A change that reduces instruction count
  but creates longer dependency chains or pipeline stalls can hurt real
  hashrate. Hashrate vetoes all proxy metrics.
