# W2-3 Dataset-Access Thunk Instruction Count — Static Analysis Only

Static inspection of the four 1-way dataset-item thunks in
`src/jit_compiler_a64_static.S`. **No source changes.** All four labels are
defined entirely in this `.S` file (not C); the superscalar opcode body that
the JIT splices *between* prefetch and mix is out of scope here.

Labels (exact):

| Label | Local alias | Lines |
|---|---|---|
| `randomx_calc_dataset_item_aarch64` | `rx_calc_dataset_item` | 945–993 (+ literal pool 995–1002, excluded) |
| `randomx_calc_dataset_item_aarch64_prefetch` | `rx_calc_dataset_item_prefetch` | 1006–1010 |
| `randomx_calc_dataset_item_aarch64_mix` | (falls through) | 1015–1027 |
| `randomx_calc_dataset_item_aarch64_store_result` | (falls through to `ret`) | 1029–1044 |

End sentinel: `randomx_calc_dataset_item_aarch64_end` at line 1046.

## 1. Per-thunk AArch64 instruction counts

Count = real instructions from the label up to (but not including) the next
thunk label, or through `ret` for store. Literal pool / padding excluded.

| Thunk | Instr | Breakdown |
|---|---:|---|
| `…_aarch64` (prologue) | **28** | `sub` + 7×`stp` + `ldr` Mul0 + 3×`mov` + `madd` + 7×(`ldr`+`eor`) + `b` |
| `…_prefetch` | **3** | `and` + `add` (lsl 6) + `prfm` |
| `…_mix` | **12** | 4×`ldp` + 8×`eor` |
| `…_store_result` | **13** | 4×`stp` + 7×`ldp` + `add` + `ret` |
| **Sum of the four bodies (once each)** | **56** | |

Literal pool at 995–1002 (8×`.quad`) sits between prologue’s `b` and
prefetch; excluded from the 28.

### How the JIT actually uses them (context for §3)

`generateSuperscalarHash()` (`jit_compiler_a64.cpp`) memcpy’s these ranges into
the JIT buffer once per seed. Per `bl rx_calc_dataset_item` (one dataset item):

| Piece | Times / call | Static thunk instr | Notes |
|---|---:|---:|---|
| Prologue | 1 | 28 | Includes in-copy literal pool (data, not counted) |
| Prefetch sequence | 8 (`kRandomXCacheAccesses`) | 3×8 = 24 | JIT emits the `and` itself and copies static from `prefetch+4` (`add`+`prfm` only) |
| Mix | 8 | 12×8 = 96 | |
| Store / epilogue | 1 | 13 | |
| **Thunk total / call** | | **161** | |
| JIT glue *not* in these thunks | 8×(`b` over literal pool + `mov x10`) | +16 | → census wrapper 177 = 161 + 16 |

Matches W1-1’s wrapper split: `28 + 8×(12 mix + 1 b + 1 mov + 1 and + 1 add + 1 prfm) + 13 = 177`.

## 2. Redundant / non-minimal sequences (inspection)

Concrete line numbers in `src/jit_compiler_a64_static.S`:

1. **Serial literal loads that could be paired (966–991)** — seven
   `ldr x12, superscalarAddN` / `eor` pairs. Adjacent Add1/Add2 … Add5/Add6
   are consecutive `.quad`s (996–1001); `ldp x12, x13, …` + two `eor`s would
   cut ~3–4 instructions from the prologue (Add7 stays a singleton). Mul0
   (956) stays a lone `ldr`. Constraint: literals must keep PC-relative
   layout after memcpy (today ensured by the `b` at 993 jumping the pool).

2. **Bare SP adjust vs folded pre/post-index (947, 1042)** —
   `sub sp, sp, 112` then `stp x0, x1, [sp]` / trailing `add sp, sp, 112`
   after the last `ldp`. AArch64 allows `stp …, [sp, #-112]!` and
   `ldp …, [sp], #112`, saving **1 instr in prologue + 1 in epilogue**.

3. **Placeholder `and` never copied into the live JIT path (1008)** —
   static `and x11, x10, 1` is sizing/documentation only:
   `generateSuperscalarHash` emits a real mask `and` then memcpy’s from
   `prefetch+4`. Not executed in the hash path; the executed prefetch is
   still 3 instructions (JIT `and` + static `add`/`prfm` at 1009–1010).

4. **Full x0–x13 frame on a light-mode-only hot path (948–954, 1035–1041)** —
   7×`stp` + 7×`ldp` every call. Light-mode caller
   (`randomx_program_aarch64_vm_instructions_end_light`) reloads x0–x2 after
   return and never needs x3 preserved; x8–x11 are thunk scratch. Not
   “wrong,” but **non-minimal for the only live caller** (16,384×/hash).
   Track C / light prologue attempt already tried a reduced frame and hung
   — see `docs/experiments/light-mode-dataset-item-prologue-attempt.md`.
   Upper bound if ever made safe: on the order of the 14 save/restore
   ops × 16,384 ≈ 0.23 M instr/hash before accounting for what must stay.

5. **Not redundant (called out to avoid false positives)**
   - **958–960** `mov x8/x9/x10, x0/x1/x2` — required: x0–x2 are immediately
     overwritten by the rl[] init (963–991) but must survive as cache base /
     output / `registerValue`.
   - **963** `madd x0, x2, x12, x12` — already the compact form of
     `(itemNumber+1)*Mul0`; not a shift expansion.
   - **1016–1027** mix — `x11` set once in prefetch, reused for all four
     `ldp`s; 4×`ldp`+8×`eor` is minimal for 64-byte XOR into x0–x7.
   - **1010 `prfm` vs later mix loads** — intentional latency hide across the
     intervening superscalar body, not a dead prefetch.

## 3. Per-hash estimate vs superscalar census

| Quantity | Value |
|---|---|
| Thunk instr / dataset-item call (prologue×1 + prefetch×8 + mix×8 + store×1) | **161** |
| Calls / hash (light mode) | **16,384** |
| Thunk contribution | 161 × 16,384 = **2,637,824 ≈ 2.64 M instr/hash** |
| W1-1 superscalar region | **95.77 M instr/hash** |
| Thunk fraction of superscalar | 2.64 / 95.77 ≈ **2.76%** |
| Full wrapper (thunks + 16 JIT glue) | 177 × 16,384 ≈ **2.90 M** ≈ **3.03%** of 95.77 M (matches W1-1) |

Naive “sum the four labels once × 16,384” = 56 × 16,384 ≈ 0.92 M would
**under-count** by ignoring the 8× prefetch/mix repeat; the 161 figure is
the correct static-thunk total per item.

Opcode body (~5,224/call, ~85.6 M/hash) remains ~96–97% of the superscalar
region; thunks are a small wrapper slice.

## 4. Future ideas (proposals only — not implemented)

- **Pair AddN literal loads with `ldp`** (lines 966–991): ~−3–4 instr/call →
  ~50–65 k instr/hash ≈ **0.05–0.07%** of superscalar; tiny, low risk if pool
  layout stays PC-relative-safe after memcpy.
- **Fold `sub`/`add` sp into first/last `stp`/`ldp`** (947 / 1042): −2/call →
  ~33 k/hash (**≪0.05%**); cleanup only.
- **Revisit light-mode reduced frame** (948–954 / 1035–1041): theoretical
  ceiling low–mid tenths of a percent of total hash if a safe variant exists;
  prior attempt hung — treat as blocked until root cause is known
  (`light-mode-dataset-item-prologue-attempt.md`).
- **NEON 64-byte mix** (replace 1016–1027): speculative; need integer results
  back in x0–x7, so move traffic may erase any density win on A53.
- **Do not chase thunk density as a primary lever** — even deleting the
  entire 161-instr wrapper is only ~2.8% of superscalar / ~2.2% of the
  118.96 M total; the opcode body dominates.

## References

- `src/jit_compiler_a64_static.S` 945–1046
- `src/jit_compiler_a64.cpp` `generateSuperscalarHash()` (~1060–1198),
  `CalcDatasetItemSize` (~118–131)
- `docs/experiments/w11-instruction-census.md` (95.77 M, wrapper 177)
- `docs/experiments/light-mode-dataset-item-prologue-attempt.md`
