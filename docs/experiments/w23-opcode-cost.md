# W2-3 Superscalar Opcode Emission Cost — Static Analysis Only

Static cost analysis of every `SuperscalarInstructionType` case in
`JitCompilerA64::generateSuperscalarHash()` (`src/jit_compiler_a64.cpp`
~1060–1198) and the helpers it calls. **No source changes.**

Companion: wrapper/thunk frame costs are in `docs/experiments/w23-thunk-count.md`
(out of scope here). Region context: `docs/experiments/w11-instruction-census.md`
(measured opcode body **5,224 A64 instr/call**).

## Scope notes

- Superscalar ISA is **14 opcodes** (`include/armrx/superscalar.hpp:13–31`).
  There is **no** `IADD_R` and **no** `ISWAP` in this set (those exist only on
  the main RandomX VM program). Register ops present: `ISUB_R`, `IXOR_R`,
  `IADD_RS`, `IMUL_R`, `IMULH_R`, `ISMULH_R`.
- Counts below are **executed AArch64 instructions** in the opcode switch
  body. `emit64` for `IMUL_RCP` writes **data** into a per-program literal
  pool that is jumped over (`B` at lines 1107–1109) — not retired as ALU/load
  ops in the opcode stream (the pool is still part of the buffer extent).
- Superscalar path pins `num32bitLiterals = 64` (lines 1069–1080) so
  `emitMovImmediate` **never** takes the shared NEON literal-pool branch;
  C7/C8/C9 always use the self-contained `MOVZ`/`MOVN`+`MOVK` path.

## 1. Helper expansion facts (with line numbers)

### `emit64` — `include/armrx/jit_compiler_a64.hpp:156–160`

Writes 8 bytes, advances `codePos` by 8. Used only in the IMUL_RCP pre-pass
(line 1104) to populate the literal pool. **0 executed instructions** in the
opcode body (data only).

### `emitMovImmediate` — `src/jit_compiler_a64.cpp:1215–1261`

| Immediate | Path (with `num32bitLiterals ≥ 64`, superscalar) | A64 instr |
|---|---|---:|
| `imm < 2^16` | one `MOVZ` (1219–1222) | **1** |
| `imm ≥ 2^16`, `imm` as `int32_t` ≥ 0 | `MOVZ` high + `MOVK` low (1251–1256) | **2** |
| `imm ≥ 2^16`, `imm` as `int32_t` < 0 | `MOVN` high + `MOVK` low (1244–1256) | **2** |
| (dead here) `num32bitLiterals < 64` | one `UMOV`/`SMOV` from NEON pool (1226–1240) | 1 |

**Typical full 32-bit random immediate** (`gen.get_uint32()` for C7/C8/C9,
`superscalar.cpp:383/392`): `P(imm < 2^16) = 2^−16 ≈ 0`, so **2 instructions**
(`MOVZ`/`MOVN` + `MOVK`).

### `emitAddImmediate` (5-arg, tmp=`x13` in superscalar) — lines 1269–1301

| Immediate | Path | A64 instr |
|---|---|---:|
| `imm < 2^24`, only lo or only hi 12-bit half nonzero | one `ADD_IMM_LO` or `ADD_IMM_HI` (1283–1290) | **1** |
| `imm < 2^24`, both halves nonzero | `ADD_IMM_LO` + `ADD_IMM_HI` (1278–1281) | **2** |
| `imm ≥ 2^24` | `emitMovImmediate(tmp)` + `ADD` reg (1294–1297) | **2 or 3** (almost always **3**) |

For uniform random `uint32_t`: `P(imm < 2^24) = 2^−8 ≈ 0.39%`, so the fast
path almost never hits. **Expected cost ≈ 2.996 ≈ 3 instructions.**

## 2. Per-opcode A64 instruction count (switch lines 1120–1175)

| Opcode | Lines | Emission | Typical A64 count |
|---|---|---|---:|
| `ISUB_R` | 1122–1124 | one `SUB` | **1** |
| `IXOR_R` | 1125–1127 | one `EOR` | **1** |
| `IADD_RS` | 1128–1130 | one shifted `ADD` | **1** |
| `IMUL_R` | 1131–1133 | one `MUL` | **1** |
| `IROR_C` | 1134–1136 | one `ROR` imm | **1** |
| `IADD_C7` / `IADD_C8` / `IADD_C9` | 1137–1147 | `emitAddImmediate(dst,dst,imm,13,…)` | **3** (rarely 1–2) |
| `IXOR_C7` / `IXOR_C8` / `IXOR_C9` | 1148–1153 | `emitMovImmediate(x12,imm)` + `EOR` | **3** (rarely 2) |
| `IMULH_R` | 1154–1156 | one `UMULH` | **1** |
| `ISMULH_R` | 1157–1159 | one `SMULH` | **1** |
| `IMUL_RCP` | 1160–1172 | `LDR` literal + `MUL` (`emit64` = pool data only) | **2** |
| `default` | 1173–1174 | nothing | 0 |

Register ops are already minimal (1). `IROR_C` is 1 (`ROR` imm). `IMUL_RCP`
is 2 (architecturally: materialize 64-bit reciprocal + multiply). All
multi-instruction cost above 1.0 instr/op lives in **IADD_C\*** / **IXOR_C\***
(imm materialization) and the extra `LDR` of **IMUL_RCP**.

## 3. Opcode distribution — no `RANDOMX_FREQ_*` table

`src/instruction_weights.hpp` / `RANDOMX_FREQ_*` apply to the **main VM**
256-byte opcode map, **not** superscalar generation. Superscalar opcodes are
chosen by the decoder-buffer / slot model in `src/superscalar.cpp`
(buffers at 185–248, slots at 252–258, `createForSlot` at 300–335). There is
**no constant-weight table** for the 14 superscalar types.

Use the measured `--jit-dump` aggregate from
`docs/plans/20260727/master-plan-20260727.md` (2026-07-27, one
`generateSuperscalarHash()` compile) instead of uniform 1/14:

| Opcode group | Count (RL ops) | Assumed A64/op | A64 contrib |
|---|---:|---:|---:|
| 1-instr register / rotate (`ISUB_R`…`IROR_C`, `IMULH_R`, `ISMULH_R`, …) | 2,610 | 1 | 2,610 |
| `IMUL_RCP` | 239 | 2 | 478 |
| `IXOR_C7`+`C8`+`C9` (137+128+122) | 387 | ≈3.000 | ≈1,161 |
| `IADD_C7`+`C8`+`C9` (135+92+100) | 327 | ≈2.996 | ≈980 |
| **Total** | **3,563 RL ops** | | **≈5,229 A64** |

Cross-check: same dump reported **20,916 bytes** → `20916/4 = 5,229` A64
words. (The dump line `Total instructions: 3563` counts **RandomX-level**
ops via `ss_total_instr++` per dump entry at lines 1005–1007 — not A64
instructions. That naming mismatch is why the old 3,563→58.4M figure
under-counted the body; W1-1's live 5,224 is the A64 truth.)

Uniform 1/14 over `8×512 = 4,096` ops would predict ≈7,896 A64
(`E[cost]≈1.93`) — **far above** measurement, because the decoder model
heavily favors 3-/4-byte slots (1-instr ops), not equal weight on C7/C8/C9.

## 4. Estimate vs measured 5,224

| Model | A64 instr / `rx_calc_dataset_item` call |
|---|---:|
| Static helper model × dump mix (§3) | **≈5,229** |
| Dump bytes/4 | **5,229** |
| W1-1 live buffer (this seed) | **5,224** |
| Uniform 1/14 × 4096 ops | ≈7,896 (reject) |
| Scale dump mix to hard 8×512 cap | ≈6,008 (programs usually finish under cap; ~445 RL ops/program in the dump) |

**Verdict:** estimate and measurement agree to ≪1%. The ~1.28–1.47 A64/RL-op
average is real, not a counting artifact.

### Gap decomposition (excess over 1.0 A64 / RL op)

For the dump mix: 5,229 − 3,563 = **1,666 extra A64 instructions**.

| Source | Extra A64 | Share of extras |
|---|---:|---:|
| `IXOR_C*` (+2 each) | ≈774 | ~46% |
| `IADD_C*` (+≈2 each) | ≈653 | ~39% |
| `IMUL_RCP` (+1 each) | 239 | ~14% |
| **Imm-constant ops (`IADD_C*`+`IXOR_C*`)** | **≈1,427** | **~86%** |

Imm-constant ops are also **~41% of the entire opcode body** by instruction
volume (~2,141 / 5,229), despite being only ~20% of RL ops (714 / 3,563).

**(a) Immediate-constant ops dominate** the excess over 1 instr/op.
Register ops and `IROR_C` are already 1; `IMUL_RCP` is a smaller fixed +1.

## 5. Ranked PROPOSAL ideas (not implemented)

Savings are rough fractions of the **opcode body** (~5,224/call ≈ 85.6 M/hash
at 16,384 calls). Cycle/hashrate impact may be lower if IPC falls.

1. **Pool-load materialization for `IADD_C*` / `IXOR_C*` (replace `MOVZ`+`MOVK` with one `LDR`/`UMOV`)**  
   Today: 2 mov + 1 ALU = 3. Target: 1 load + 1 ALU = 2 (−1 per imm op).  
   ~714 ops × 1 ≈ **714 A64/call ≈ 13.7% of body ≈ 11.7 M instr/hash (~10% of
   total ~119 M)**. Requires lifting the `num32bitLiterals = 64` pin
   (lines 1069–1080) with a scheduler-safe pool ordering story (the pin exists
   specifically because independent review flagged order sensitivity under
   `scheduleSuperscalarProgram`). Prior superscalar `IMUL_RCP` *literal-load
   elimination* went the opposite direction (more instr, net −0.3%) — this
   proposal reduces count; still needs a controlled `perf` veto.

2. **Shared per-program constant pool + amortized `ADRP`, then `LDR`+ALU**  
   Same asymptotic −1/imm-op as (1), with one `ADRP` (or base pointer) per
   program instead of PC-relative literal `LDR`s scattered through the body.  
   Extra win is I-cache density / fewer literal islands, not a large further
   instruction cut. Rough body save still **~12–14%**; secondary density win
   unquantified. Same scheduler/pool-ordering hazard class as (1).

3. **Opportunistic single-instruction forms where the ISA allows**  
   - `EOR` bitmask-immediate when the 32-bit constant is encodable (prior
     audit: ~0/20k encodable — ≈**0%**).  
   - `ADD_IMM`/`SUB_IMM` when `imm` or `−imm` fits 24-bit (already mostly
     handled for `imm < 2^24`; random hit rate ~0.4–0.8%) ≈ **≪1% of body**.  
   Keep as a cheap peephole behind (1)/(2), not a primary lever.

## Files / cross-references

- `src/jit_compiler_a64.cpp` — `generateSuperscalarHash` 1060–1198;
  `emitMovImmediate` 1215–1261; `emitAddImmediate` 1269–1301
- `include/armrx/jit_compiler_a64.hpp` — `emit64` 156–160
- `include/armrx/superscalar.hpp` — 14-opcode enum
- `src/superscalar.cpp` — decoder slots / `get_uint32()` immediates for C*
- `docs/experiments/w11-instruction-census.md` — measured 5,224 body
- `docs/plans/20260727/master-plan-20260727.md` — 2026-07-27 dump counts
  (3563 RL ops / 20916 bytes)
