# Peephole JIT Coalescing — Plan

**Goal**: Close the ~33% instruction-count gap between armrx and XMRig's
AArch64 JIT output (measured at 64.3B vs 48.2B instructions per benchmark).

**Current state**: The static ASM template (`jit_compiler_a64_static.S`) and
JIT compiler (`jit_compiler_a64.cpp`) are nearly identical to XMRig's — the gap
comes from years of XMRig's incremental peephole optimizations across hundreds
of small emitted sequences. No single optimization will close the gap; it
requires systematic comparison of per-opcode emitted code.

---

## Methodology

1. **Dump the generated code** from both armrx and XMRig for the same seed/key.
2. **Disassemble** both dumps and align by opcode boundary.
3. For each opcode handler, **compare instruction sequences** and identify
   differences.
4. For each difference, **verify correctness** (KAT parity) and **measure
   impact** (instructions retired).
5. **Merge** winning patterns into armrx's JIT compiler.

---

## Phase 1 — Infrastructure (tooling to enable comparison)

### 1.1 Code dumping tool
Add a `--jit-dump` flag to `armrx` that:
- Runs one hash with a given seed/key
- Dumps the JIT code buffer (`code` array from `JitCompilerA64`) to stdout as hex
- Exits without executing (or executes, then dumps)

**File**: `src/main.cpp`, new `--jit-dump` flag
**Output format**: raw bytes (pipe to `xxd` or `objdump`)

### 1.2 Seed-program determinism test
Add a test that compiles the same program twice and asserts identical
JIT output. Catches non-determinism in the JIT compiler.

**File**: `tests/test_jit_determinism.cpp`

### 1.3 Instruction-level benchmark
Add micro-benchmarks for individual opcode handlers that measure
instructions retired per opcode dispatch (via `perf stat -e instructions`).

**File**: `tests/bench_opcodes.cpp`

---

## Phase 2 — Per-opcode audit

For each of the 30 opcodes, extract the emitted AArch64 sequence and
compare against XMRig's output. The XMRig source is at `xmrig-dev/src/`.

### Known gaps to investigate first

Based on the OPTIMIZATION_REFERENCE "33% more instructions" finding:

| Priority | Opcode | armrx issue | Est. savings |
|----------|--------|-------------|--------------|
| P0 | **IMUL_RCP** | Emits `mov xN, #imm; umulh` — XMRig may fold constant into `movk` sequence | −1–2% |
| P0 | **CBRANCH** | New `bne+b` path is correct but adds 1 instruction vs old `beq`; XMRig may use different add+test combination | −0.5% |
| P1 | **IROR_R / IROL_R** | armrx uses `rotr`/`rotl` (function call or intrinsics); XMRig may use `extr` (AArch64 rotate-insert) | −1–3% |
| P1 | **FDIV_M / FSQRT_R** | armrx emits software divide/sqrt calls; XMRig may use inline Newton-Raphson sequences | −2–5% |
| P2 | **All memory ops** | getScratchpadAddress pattern (`add + and`) — XMRig may fold addressing modes | −1–2% |
| P2 | **ADD/SUB immediate** | `emitAddImmediate` emits 1-2 `add` instructions; XMRig may use `adds`/`subs` to save flags | −0.5% |

### Audit process for each opcode

1. Find the `h_` handler in `jit_compiler_a64.cpp`
2. Trace the emitted instructions via `emit32` calls
3. Find the corresponding handler in XMRig's source
4. Diff the emitted sequences
5. If different: implement the XMRig pattern → KAT verify → benchmark

---

## Phase 3 — Cross-opcode optimizations

These span multiple opcodes and require deeper analysis:

| # | Optimization | Description | Est. savings |
|---|-------------|-------------|--------------|
| 3.1 | **Register allocation reuse** | Track which GPRs/NEON regs are "dead" after each opcode and avoid spilling | −2–3% |
| 3.2 | **Constant folding** | Pre-compute immediates that can be expressed as single `mov` + shifted operand | −1% |
| 3.3 | **Dead instruction elimination** | Skip emitting instructions whose results are overwritten before use | −1–2% |
| 3.4 | **Load-pair coalescing** | Merge adjacent single loads into `ldp` where register pairing allows | −1–2% |

---

## Phase 4 — Validation

Each change requires:

1. **KAT parity**: All existing KAT vectors produce identical hashes
2. **Determinism**: Same seed → same JIT output (test from Phase 1.2)
3. **Instruction count**: `perf stat` shows reduction for the affected opcode
4. **Hashrate**: No regression on real hardware (>3 runs, average)

---

## Estimated Timeline

| Phase | Effort | Expected gain |
|-------|--------|---------------|
| Phase 1 (tooling) | 1-2 days | — |
| Phase 2 (opcode audit) | 1-2 weeks | −5–10% |
| Phase 3 (cross-opcode) | 2-4 weeks | −5–10% |
| Phase 4 (validation) | Ongoing | — |
| **Total** | **3-6 weeks** | **−15–20% instructions** (~matching XMRig) |

---

## Risks

- **KAT regressions**: Every instruction sequence change risks producing
  different hashes. Mitigation: run full KAT suite before every commit.
- **XMRig code is not clean-room**: We can compare outputs and study
  techniques, but cannot copy XMRig source. All implementations must be
  original.
- **Diminishing returns**: The first few optimizations may yield large gains;
  later ones may be marginal. Stop when the gap is ≤5%.
