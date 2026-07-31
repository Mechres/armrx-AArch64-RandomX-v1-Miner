# W2-2 / N1 — Scratchpad LDR/STR adjacency analysis (static, host-only)

**Date:** 2026-07-31  
**Scope:** Source analysis of `src/jit_compiler_a64.cpp` / `include/armrx/jit_compiler_a64.hpp`
(and the fixed main-loop glue in `src/jit_compiler_a64_static.S`). No AArch64 build; no
runtime measurement. Closes the N1 gate posed in
`docs/audits/performance-audit-work-ideas-20260801.md` §W2-2.

**Verdict up front: NO — never emit-adjacent (and wrong addressing mode for classical
`ldp`/`stp` `#imm` fusion). Close N1 as not actionable by a simple LDP/STP emission pass.**

---

## 1. Scratchpad load/store emission helpers

There is **no** `emitMemStore` helper. Stores are inlined in `h_ISTORE`.

### Declarations (`include/armrx/jit_compiler_a64.hpp`)

```172:176:include/armrx/jit_compiler_a64.hpp
		template<uint32_t tmp_reg>
		void emitMemLoad(uint32_t dst, uint32_t src, Instruction& instr, uint8_t* code, uint32_t& codePos);

		template<uint32_t tmp_reg_fp>
		void emitMemLoadFP(uint32_t src, Instruction& instr, uint8_t* code, uint32_t& codePos);
```

`h_ISTORE` is a separate handler (`void h_ISTORE(Instruction&, uint32_t&);`, line 206).

### `emitMemLoad` (`src/jit_compiler_a64.cpp:1303–1334`)

Address formation, then a **register-indexed** 64-bit load from scratchpad base `x2`:

| Path | Address setup | Emitted load |
|---|---|---|
| `src != dst` (L1/L2) | `emitAddImmediate(tmp, src, imm_masked)` + `and tmp, tmp, #mask` | `ldr tmp_reg, [x2, tmp_reg]` — encoding `0xf8606840 \| tmp \| (tmp << 16)` (line 1321–1322) |
| `src == dst` (L3) | `emitMovImmediate(tmp, (imm & L3Mask) >> 3)` | `ldr tmp_reg, [x2, tmp_reg, lsl #3]` — encoding `0xf8607840 \| tmp \| (tmp << 16)` (line 1329–1330) |

**Not** `ldr Xt, [x2, #imm]`. Offset is always in a GPR, data-dependent at runtime.

### `emitMemLoadFP` (`src/jit_compiler_a64.cpp:1336–1363`)

Same L1/L2 address formation into `x19`, then:

1. `ldr d<tmp_reg_fp>, [x2, x19]` — encoding `0xfc606800 \| …` (line 1353–1354)
2. `sxtl` + `scvtf` (convert packed s32 → f64)

Again register-indexed, not `#imm`.

### `h_ISTORE` (`src/jit_compiler_a64.cpp:1947–1975`)

Address: `emitAddImmediate(x20, dst, imm_masked)` + L1/L2/L3 `and`, then:

- `str src, [x2, x20]` — encoding `0xF8206840 \| src \| (tmp << 16)` (line 1971–1972)

Call sites for loads: all `*_M` integer handlers (`h_IADD_M` … `h_IXOR_M`) and FP
`h_FADD_M` / `h_FSUB_M` / `h_FDIV_M` (grep hits at lines 1391, 1438, 1475, 1506, 1537,
1626, 1723, 1746, 1776).

---

## 2. Adjacency analysis

### 2a. Superscalar path — zero scratchpad traffic

`generateSuperscalarHash` (`src/jit_compiler_a64.cpp:1060–1198`) emits only register ALU
ops plus `LDR` **literal** for `IMUL_RCP` (line 1166–1167). The opcode switch has no
calls to `emitMemLoad` / `emitMemLoadFP` / `h_ISTORE`.

Cache-line traffic lives in the **static wrapper** memcpy'd around each program
(`randomx_calc_dataset_item_aarch64_{prefetch,mix,store_result}`) and already uses
`ldp`/`stp` of adjacent 16-byte pairs (`jit_compiler_a64_static.S` ~1016–1033). That is
dataset/cache memory, not RandomX scratchpad, and is already fused.

**Consequence for the 95.77 M superscalar-region instr/hash (W1-1):** fraction that are
scratchpad `LDR`/`STR` = **0%**. N1 cannot move the superscalar needle at all.

### 2b. Main-VM JIT program — loads/stores never emit-adjacent

Each memory RandomX opcode expands to a **handler block**:

```
*_M:   [ADD/AND or MOV address] → LDR → ALU using loaded value
ISTORE: [ADD/AND address]       → STR
```

So within one RandomX instruction there is exactly one scratchpad transfer, always
preceded by ≥1 address-setup instruction and (for loads) followed by the consuming ALU.

Across two consecutive RandomX memory ops in emission order, the stream looks like:

```
… LDR/STR₁ ; [ALU₁ if load] ; ADD₂ ; AND₂ ; LDR/STR₂ …
```

The second op’s address setup always sits between the two memory transfers. Therefore
**two scratchpad `LDR`/`STR` are never adjacent in the emitted A64 stream.**

Additional constraints that reinforce this:

- The emitter scheduler (`scheduleProgram`, comments at lines 233–239) treats **any** two
  memory ops as potentially aliasing and **never reorders them relative to each other**.
  Even if a future pass tried to gather loads, the current scheduler will not create
  adjacency by reordering.
- Superscalar scheduling (`scheduleSuperscalarProgram`) is irrelevant here — that path
  has no scratchpad ops (see 2a).

### 2c. Addressing mode blocks classical `#imm` LDP/STP even under reordering

AArch64 load/store pair fusion of the form hypothesized for N1
(`ldp Xt1, Xt2, [Xn, #N]` replacing `ldr` at `#N` and `#N+8`) requires
**immediate-offset** single loads/stores with the same base.

`emitMemLoad` / `h_ISTORE` emit **`[x2, Xm]` / `[x2, Xm, lsl #3]`**. Offsets are
runtime-computed into a temp (and the temp is shared `x20` / `x19` across successive
handlers, so even consecutive address values are not simultaneously live in two
registers). Turning pairs into `ldp` would require a different address model (hoist /
dual-offset materialization), not a local peephole on the current encoding.

So: even a reordering pass that put two LDRs back-to-back would **still** not yield a
correctness-preserving one-instruction `ldp` without a larger design (closer to T3-2
load-address hoisting than to N1).

### 2d. Static main-loop glue (out of N1 scope, noted for completeness)

The fixed per-iteration scratchpad block in `jit_compiler_a64_static.S` already uses
`ldp`/`stp` for integer registers (`ldp x20, x19, [x16]` / `[x16, #16]` … and matching
stores to `[x17]`). FP loads from `spAddr1` are still eight adjacent
`ldr dN, [x17, #k]` with `#k` stepping by 8 (lines 243–254) — those **are**
imm-offset adjacent pairs, deliberately interleaved with integer `ldp` for latency
hiding. That is a separate, tiny static-glue micro-opt (main-VM region only), not the
JIT `emitMemLoad` path N1 describes, and is not counted as a superscalar win.

---

## 3. Quantitative estimate (vs W1-1 census)

| Quantity | Estimate | Basis |
|---|---|---|
| Superscalar region instr/hash | **95.77 M** | `docs/experiments/w11-instruction-census.md` |
| Of which scratchpad `LDR`/`STR` | **0 M (0%)** | `generateSuperscalarHash` emits none |
| Of those, emit-adjacent same-base offset-8 pairs | **0 (n/a)** | no candidates |
| Main-VM region instr/hash | **11.79 M** | same census |
| Expected JIT-emitted mem ops/hash | ~**0.11 M** | RandomX freqs: Σ`*_M`+`ISTORE` = 55/256 of a 256-instr program × 2048 iterations ≈ 112.6k transfers/hash; one `LDR`/`STR` each |
| Of those, emit-adjacent fusable pairs | **0%** | §2b–2c: never adjacent; wrong addressing mode |
| Gate thresholds (W2-2) | close if adjacent &lt;5% of mem ops; prototype if ≥15% | adjacent rate = **0%** → **close** |

---

## 4. Adjacency verdict

| Criterion | Result |
|---|---|
| Emit-adjacent fusable scratchpad pairs in superscalar body | **NO** (no scratchpad ops) |
| Emit-adjacent fusable pairs from `emitMemLoad` / `h_ISTORE` | **NO** (always separated by addr setup ± ALU) |
| Classical `#imm` LDP/STP applicable to current encodings | **NO** (register-indexed only) |
| Overall N1 | **NO never-adjacent** (and structurally not LDP-shaped) |

---

## 5. Recommendation

**Close N1 as not actionable** — do not implement an LDP/STP fusion pass on
`emitMemLoad`/`h_ISTORE`; simple emission cannot create pairs, and the encodings are
not `#imm`-fusable. Any future memory-op density work needs a different design
(address hoisting / dual-offset materialization), not N1.
