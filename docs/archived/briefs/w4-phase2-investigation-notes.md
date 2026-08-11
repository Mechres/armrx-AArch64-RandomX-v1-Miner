# W4 Phase-2 — Investigation Log & SOLVED (2026-08-02)

## Objective
Close the instruction-count gap vs the BSD reference JIT (armrx 132.7M/1.369 → reference
104.8M/1.54 on Cortex-A53 @ 765 MHz). Technique: pool superscalar C* (`IADD_C7..9` /
`IXOR_C7..9`) immediates into a DEDICATED inline literal pool inside the dataset-item function,
loaded via one `LDR (literal)` instead of 2–3 `MOVZ/MOVN+MOVK+ALU`. Mirrors IMUL_RCP's proven
PC-relative LDR pool, but in its OWN per-program region (phase-1 failed because it tried to share
the main-VM `ImulRcpLiteralsEnd` region and overwrote the main VM's literals).

## STATUS: SOLVED ✅ (2026-08-02)
Full device gate set PASS:
- `test_jit_equivalence` 16/16 byte-identical (EQ_EXIT=0)
- `test_jit_dataset_2way` (D2W_EXIT=0)
- `test_jit_determinism` (DET_EXIT=0)
- `test_jit_scheduler_stress` (450 pairs, STRESS_EXIT=0)
- `test_jit_superscalar_scheduler_stress` (200 pairs, SSTRESS_EXIT=0)

Pool: 128 slots (1 KiB) per program, **703/703 C* ops pooled** across the 8 programs (all imm ≥ 1<<16).

## ROOT CAUSE (the bug that cost 15+ iterations)
The offset formula carried a spurious `- 8`:
```cpp
int32_t offset = static_cast<int32_t>(cpoolLiteralPos_ - k - 8) / 4;  // WRONG
```
A64 `LDR (literal)` computes `target = k + off*4`, where `k` is the address of the LDR instruction
itself. **There is NO `+8` — that is an A32/T32 (ARMv7) PC+8 quirk, NOT A64.** So the `-8` made
every pooled C* op load from `litpos - 8` = the PREVIOUS slot (or pre-pool garbage for slot 0) →
silent hash mismatch (not SIGILL), exactly matching the observed symptom.

The `k + 8` convention had been "proven" by a **runtime self-check that was CIRCULAR**:
```cpp
int32_t soff = (offset & 0x40000) ? (offset - (1<<19)) : offset;
uint32_t target = (k + 8) + soff * 4;          // <-- uses the SAME formula under test
uint32_t target_val = ((uint32_t*)(code + target))[0];
fprintf(stderr, "match=%d\n", target_val == imm);  // always 1
```
It recomputed the check target with the same (wrong) offset, so `match=1` was guaranteed regardless
of what the hardware would actually load. This self-check "validated" every version, including the
broken one, for 15+ iterations.

**Fix** (mirror the proven IMUL_RCP loader `off = (literal_pos - codePos)/4`):
```cpp
int32_t offset = static_cast<int32_t>(cpoolLiteralPos_ - k) / 4;  // CORRECT
offset &= (1 << 19) - 1;
emit32(ARMV8A::LDR_LITERAL | dst | (offset << 5), code, k);
```

## SECOND BUG (also required): sign-extension of C* constants
RandomX C* constants are SIGNED. The pool entry MUST hold the sign-extended 64-bit value
(`memcpy` of `int64_t(imm)`). A zero-extended pool makes `LDR Xt` (which reads all 8 bytes) load
the wrong value for negative constants. Gpt-5.6 Luna fixed this. Both the `-8` and the
sign-extension had to be correct together — fixing only one left the hash failing.

## HOW IT WAS CONFIRMED (no more guessing)
1. **Hardware micro-test** (Kimi K3): mmap RWX buffer, hand-encode `0x58000000 | (off<<5)`, execute
   under qemu-aarch64-static AND on the real Cortex-A53. `off=5` loaded `k+20`; `off=3` loaded `k+12`
   → `target = k + off*4` (c=0), settle empirically. IMUL_RCP's existing code independently confirms
   c=0 (it mines valid mainnet shares with `off = (literal_pos - codePos)/4`, no `-8`).
2. **Loader audit**: every `LDR (literal)` in the emitted region was decoded; all 703 pooled C* ops
   (rt ∈ {x12,x13}) resolve to valid sign-extended 32-bit constants inside the pool. The 4 "OOB"
   hits were false positives (data words in the pool whose top byte happened to be 0x58).
3. **Byte-stream diff** (fixed vs forced-fallback): structure-aware word diff confirms the ONLY
   divergence is the intended C* substitution (3-instr MOVx2 → 1-instr LDR) + pool contents
   (constants vs zeros) + downstream shift. **No OTHER divergence** — the "deeper structural effect"
   hypothesis from the earlier investigation was WRONG; the bug was entirely in the LDR target.

## CORRECTED PROVEN FACTS (supersedes the pre-solve notes)
1. **Forced-fallback passes** (every C* → MOVZ/MOVN+MOVK): 16/16 PASS on the current code. The
   pool reservation, B-jump, reset, and emission sites are structurally sound.
2. `ARMV8A::LDR_LITERAL = 0x58000000` = 64-bit `LDR Xt` (loads all 8 bytes; "zero-extend" in old
   notes is a misnomer). `0x18000000` = 32-bit `ldr w0`. `0x90000000`/`0xD0000000` = `adrp` (WRONG).
3. **A64 `LDR (literal)` target = `k + off*4` — NO `+8`.** `offset = (litpos - k)/4` (signed div,
   mask `& 0x7FFFF`). Mirror IMUL_RCP exactly. This is the corrected, empirically-verified formula.
4. C* constants are SIGNED → pool entry = sign-extended 64-bit value (Luna fix).
5. The 1 KiB pool shifts IMUL_RCP LDR encodings by 0x200 (512B at 64-slot) / 0x400 (1 KiB at
   128-slot) but they resolve to the correct reciprocals. IMUL_RCP is unaffected.

## PERF (honest, since `perf` is blocked on-device: `perf_event_paranoid=2`, no root)
PC-relative inline pool: pooled `--mine` = **4.32 H/s** vs documented baseline **4.27 H/s**
(non-isolated, 1 worker, same device/mode). **No regression, possibly marginal improvement.** The
instruction-count win (C* sites 2–3 → 1 `LDR`) is latency-neutral on the in-order A53 — consistent
with the project's repeated finding (W3, T2-1, T2-2) that *fewer instructions ≠ faster here*. This is
NOT the earlier W3 NEON-vector-pool attempt (which scattered ~714 loads and thrashed the cache,
−16% to −20% H/s): the PC-relative inline pool sits adjacent to the code and stays cache-clean.

## CODE (current, uncommitted on d978d05)
- `emitCpoolImmediate` (~1310): imm<(1<<16)→MOVZ; else if `cpoolBase_!=0 && cpoolSlot_<128` →
  sign-extend imm to int64, memcpy 8 bytes to `code+cpoolLiteralPos_`, `off=(litpos-k)/4 & 0x7FFFF`,
  emit `LDR_LITERAL|dst|(off<<5)`, `cpoolLiteralPos_ += 8`, `++cpoolSlot_`; else →
  MOVN/MOVZ+MOVK fallback. Ends `codePos = k;` (correct).
- `generateSuperscalarHash` (~1136): after IMUL_RCP pool, reserve 128×`emit64(0)`; set
  `cpoolBase_ = cpoolLiteralPos_ = cpool_pos`; `cpoolSlot_ = 0`; B jumps over both pools.
- C* sites (~1184/1190): `IADD_C*`→`emitCpoolImmediate(13, imm)`; `IXOR_C*`→`emitCpoolImmediate(tmp_reg, imm)`.
- `generateProgram` (~773/800): `cpoolBase_ = 0; cpoolSlot_ = 0`.
- Members `cpoolBase_`/`cpoolLiteralPos_`/`cpoolSlot_` in `include/armrx/jit_compiler_a64.hpp`.

## LESSONS (for future optimizers)
- **A64 PC = address of the current instruction.** No `+8`. Don't trust a self-check that derives
  its expected target from the same formula it's testing. Settle PC semantics ONCE with a hardware
  micro-test, then trust it.
- The `aarch64-ldr-literal-pool` skill was corrected (the `k+8` formula + circular self-check are
  now flagged as the trap). Reuse the corrected skill.
- The phase-1 shared-region collision is real; the fix is a dedicated per-program region, not
  un-pinning `num32bitLiterals`.
- Full session transcript (Kimi K3): `session-ses_0416.md` at repo root.

## Build/deploy/test (host x86_64 → AArch64 musl)
```
cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake
cmake --build build-cross -j$(nproc) --target test_jit_equivalence test_jit_dataset_2way \
  test_jit_determinism test_jit_scheduler_stress test_jit_superscalar_scheduler_stress bench_armrx
scp build-cross/test_* build-cross/armrx mechres@192.168.10.156:/tmp/cross/
# device: run each under setsid; gate = EXIT=0, no FAIL lines.
```
