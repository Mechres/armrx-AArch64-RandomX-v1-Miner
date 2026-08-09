# Superscalar JIT Disassembly — Emission-Density Findings (2026-08-09)

**Goal:** Resolve Luna's F1 hypothesis ("no source-verified redundant instruction remains in the
direct opcode cases; need opcode-level disassembly to confirm"). Method: capture the actual emitted
superscalar body on Lenovo (MSM8929, Cortex-A53), disassemble it host-side, compare per-opcode
emission against the *minimum legal A64* for each RandomX operation.

**Verdict up front:** Luna's F1 hypothesis is CONFIRMED. The superscalar emitter already emits the
minimum legal A64 sequence for every direct-register op. The only "extra" instructions are the
C* `IADD_C*`/`IXOR_C*` 3-instruction form — which is the **deliberate, measured E24 interlock pad**
(+7.1% H/s on A53), NOT redundancy. **There is no safe instruction-redundancy lever in the JIT
emitter.** Source-of-truth: the disassembly below, gated against `src/jit_compiler_a64.cpp`
emitters and the E24 changelog entry.

---

## How the data was obtained (reproducible, no live patching)

1. `try/jit-superscalar-rawdump` branch adds a guarded raw-byte dump of the superscalar region
   (`jit_dump_enabled_`) to `JitCompilerA64::dumpJitCode()` — reads the in-memory `code` buffer,
   so it sidesteps the device's I-cache coherence anomaly (w11-census caveat 5).
2. Cross-built (`build-cross`), `scp` to Lenovo `/tmp/cross/`, ran `./armrx --jit-dump`.
   - Raw dump: `docs/briefs/2026-08-09-jitdump-lenovo.txt` (first run, sizes only)
   - Raw + superscalar bytes: `docs/briefs/2026-08-09-jitdump2-lenovo.txt` (after branch)
3. Host-side `aarch64-linux-musl-objdump` of the captured raw bytes, keyed by the
   `--jit-dump` opcode boundary table (offset + size per emitted op).

(Branch left non-merged as evidence, per project rule.)

---

## Measured static body (this dump, single seed)

- 3,563 instructions / 20,920 bytes per superscalar body.
- Opcode mix (count | bytes | avg bytes):

| opcode | count | bytes | avg | note |
|---|---:|---:|---:|---|
| IMUL_R | 775 | 3100 | 4 | 1 instr |
| ISUB_R | 400 | 1600 | 4 | 1 instr |
| IXOR_R | 398 | 1592 | 4 | 1 instr |
| IADD_RS | 395 | 1580 | 4 | 1 instr |
| IROR_C | 402 | 1608 | 4 | 1 instr |
| IMULH_R | 116 | 464 | 4 | 1 instr |
| ISMULH_R | 124 | 496 | 4 | 1 instr |
| IMUL_RCP | 239 | 1912 | 8 | 2 instr (ldr+mul) |
| IADD_C7 | 135 | 1620 | 12 | 3 instr (E24 pad) |
| IXOR_C7 | 137 | 1644 | 12 | 3 instr |
| IADD_C8 | 92 | 1104 | 12 | 3 instr |
| IXOR_C8 | 128 | 1536 | 12 | 3 instr |
| IADD_C9 | 100 | 1200 | 12 | 3 instr |
| IXOR_C9 | 122 | 1464 | 12 | 3 instr |
| **C\* total** | **714** | **8568** | **12** | **41% of body bytes** |

(Static 3,563 vs census dynamic ~5,845: the documented static-vs-dynamic gap — the dump is one
seed's program; the live body's C* ops scale ~1.64× to ~1,170 ops/call.)

---

## Actual disassembly (representative ops, host objdump)

```
[IMUL_R]   9b007c63   mul   x3, x3, x0              ; 1 instr  -> MINIMUM
[IADD_RS]  8b010442   add   x2, x2, x1, lsl #1      ; 1 instr  -> MINIMUM
[IROR_C]   93c7b0e7   ror   x7, x7, #44             ; 1 instr  -> MINIMUM
[ISMULH_R] 9b457c21   smulh x1, x1, x5              ; 1 instr  -> MINIMUM
[IMULH_R]  9bc37ce7   umulh x7, x7, x3              ; 1 instr  -> MINIMUM
[IXOR_R]   ca020084   eor   x4, x4, x2              ; 1 instr  -> MINIMUM
[ISUB_R]   cb0400a5   sub   x5, x5, x4              ; 1 instr  -> MINIMUM
[IMUL_RCP] 58fff7ac   ldr   x12, [lit]             ; 2 instr -> MINIMUM
           9b0c7c00   mul   x0, x0, x12
[IXOR_C9]  d2ad1cac   mov   x12, #0x68e50000       ; 3 instr -> E24 PAD (not redundancy)
           f29d412c   movk  x12, #0xea09
           ca0c0000   eor   x0, x0, x12
[IADD_C7]  92aca2cd   mov   x13, #0xffffffff9ae9ffff
           f293a14d   movk  x13, #0x9d0a
           8b0d00c6   add   x6, x6, x13
```

Every direct-register op is **exactly one A64 instruction** — the trivial lower bound (RandomX
ops are 2-arg register transforms; `eor/add/sub/mul/ror/smulh/umulh` with two register operands
are irreducible). `IMUL_RCP` is 2 (reciprocal must be materialized; this is the documented
`ldr`+`mul` form). **No emitted sequence has a removable instruction.**

---

## The one "extra" instruction class — and why it is NOT redundancy

`IADD_C*`/`IXOR_C*` (C* = C7/C8/C9, the 32/64-bit random immediate classes) emit **3 instructions**:
`MOVZ` + `MOVK` + `ADD`/`EOR`. That is 2 more instructions than a small-immediate `MOVZ`+ALU would
need. Naively: ~2 redundant instructions × 714 ops = the single largest reducer of instruction
count in the entire miner.

**This is the E24 pad, and it is load-bearing, not redundant.** (`src/jit_compiler_a64.cpp:1476-1516`,
E24 changelog 2026-08-04): the Cortex-A53 has a **4-cycle MAC interlock**. When consecutive program
multiplies land only 2 instructions apart, `other_interlock_stall` saturates (23.3M/hash). The
3-instruction C* form inserts one extra *independent* ALU op between multiplies, hiding the
multiply latency. Measured: **+7.1% H/s (4.77→5.11)** and interlocks 23.3M→6.2M/hash (below
XMRig's 10.96M). Removing it (the `ARMRX_NO_E24_PAD` form, 1-instr for small immediates) was
*measured* to cost that 7.1%.

Quantified cost of "densifying" C* to 1-instr (hypothetical, NOT recommended):
- Saves ~5,712 bytes/static body → ~9,362 bytes/dynamic body → **~38.3M instr/hash (≈32% of
  118.96M total)**.
- But that 32% instruction reduction buys a **confirmed −7.1% H/s** on A53. It is an
  instruction-count lever that is an H/s *loss*. Not a win on this uarch.

> Note: this is exactly the instruction-count-vs-stalls tradeoff from the census reframe. armrx
> already sits on the *stall-optimal* side of it (IPC 0.731 > XMRig 0.612). Cutting instructions
> here would regress stalls harder than it helps density — which is why XMRig itself uses the
> 3-instruction form.

---

## Conclusion vs Luna's audit

- **F1 (superscalar body):** Luna said "no source-verified redundant instruction remains; needs
  disassembly to confirm." Disassembly **confirms** it. Every direct op = minimum legal A64;
  the only multi-instruction class is the deliberate, measured-good E24 pad.
- **The question "could our code be faster if written in assembly?" is now fully answered for the
  hot path:** the hot path IS assembly (hand-written `.S` template + JIT-emitted A64), and its
  emitted density is already at the A64 minimum for the operations it implements. The remaining
  gap to XMRig is **not** asm-vs-C++ and **not** emitter redundancy — it is RandomX's own
  algorithmic instruction volume per hash (the +19.5% instr/hash gap), which no asm rewrite
  changes without altering the algorithm.
- **No new `try/` lever is warranted** from this pass. The emitter is clean. Closing the
  "superscalar emission density" thread as: *measured, no safe redundancy; the one density lever
  (C* pad) is the confirmed E24 win and must stay.*

## Gate status
- KATs: N/A (analysis only; branch left non-merged as evidence).
- The dump path itself ran clean on-device (no I-cache anomaly triggered — read-only buffer dump).
- To promote the superscalar raw-byte dump to main (useful for future audits), it needs a
  `--jit-dump` flag extension + review; that is a tooling change, not a perf lever.
