# Track C Phase A Retry — Light-Mode Dataset-Item Helper: Reduced Register Preservation

## Context

The light-mode hot path calls `bl rx_calc_dataset_item` **16,384× per hash** (2048 iterations × 8 programs). Each call pays for register preservation the light-mode caller doesn't need.

**The caller** (`randomx_program_aarch64_vm_instructions_end_light`, `jit_compiler_a64_static.S:528-581`):
- Saves x0, x1, x2, x30 to its 96-byte frame (lines 544-545)
- After `bl rx_calc_dataset_item` returns (line 574), it **discards** x0, x1, x2 unconditionally — reloads its own copies from its own stack frame (lines 577-578)
- **Never uses x3 at all**

**The callee** (`rx_calc_dataset_item`, `jit_compiler_a64_static.S:942-1043`):
- Saves/restores **x0-x13** (7 `stp`/`ldp` pairs), a 112-byte frame
- x0/x1/x2/x3 preservation is redundant for the light-mode caller

**Expected payoff**: hypothesized 3-8% (never measured — previous attempt hung before reaching measurable state). This attacks the **instruction-count axis** (the real XMRig gap), not IPC/stalls.

## Previous attempt (2026-07-27 — fully reverted)

A conservative version (drop x0-x3 only, keep x4-x13) was:
1. Implemented: new `_light` prologue/epilogue labels in the `.S` file, new declarations in the `.hpp` file, `generateSuperscalarHash()` memcpy redirected to light labels
2. Verified byte-correct by both static `objdump` and dynamic `/proc/<pid>/mem` disassembly
3. **Hung indefinitely on the first JIT-mode hash** — no crash, CPU spinning, not a lock

Root cause not identified. Reverted cleanly. Full account:
`docs/experiments/light-mode-dataset-item-prologue-attempt.md`

The experiment doc includes the **complete diff** of everything that was tried (lines 43-276 in that doc) — do NOT re-derive from scratch; reuse that design.

## Already ruled out — do not re-check

1. **Literal-pool typo** — the light prologue hand-copies `superscalarMul0`/`superscalarAdd1..7` into new labels; diffed against the originals, they're byte-for-byte identical.
2. **Stack offset in shared mix code** — the shared `prefetch`→`mix` block (`jit_compiler_a64_static.S:1004-1041`) does not reference `sp` anywhere; only x0-x13. The 112-vs-80 frame size difference cannot affect this block.

## What to do

### Step 0 — Read the prior experiment doc

`docs/experiments/light-mode-dataset-item-prologue-attempt.md`. It has the full reconstructed diff. Do not re-invent. The labels to add are `randomx_calc_dataset_item_aarch64_light`, `_light_store_result`, `_light_end`.

### Step 1 — Data-flow diff harness (host-side, no devbox needed)

Write a small standalone C++ program that computes `rl[0..7]` for fixed `(cache, itemNumber)` using the prologue's arithmetic:
```
rl[0] = (itemNumber + 1) * superscalarMul0
rl[i] = rl[0] ^ superscalarAdd_i   for i = 1..7
```
Compare against `generate_dataset_item()`'s equivalent pre-mix values from `src/dataset.cpp`. If they match, the prologue arithmetic is correct. If they diverge, you have a host-side repro.

### Step 2 — Frame-consistent bisection (only if Step 1 passes)

Build two variants, each internally self-consistent (same frame size in prologue and epilogue):

**Variant A — epilogue-only change.** Keep the *original* prologue (full x0-x13, 112-byte frame). Write a new epilogue that restores the same 112-byte frame but has a different structure (e.g., reorder restore instructions, or throw x0-x3 into scratch regs instead of skipping them). Tests whether merely altering the epilogue's shape reproduces the hang.

**Variant B — prologue-only change.** Apply the light prologue (80-byte frame, saves x4-x13 only) exactly as the prior attempt designed it. Pair it with a matching 80-byte epilogue that is a **mechanical** transcription of the original restore logic (same instruction ordering/style, just fewer pairs). This isolates whether the *specific* epilogue from the first attempt had its own independent bug.

For each variant: verify with static `objdump` + dynamic `/proc/<pid>/mem` disassembly (same method as first attempt) before running anything on-device.

### Step 3 — If both variants hang

Extend Step 1's harness to run the full 8-round superscalar+mix loop (not just prologue rl[] formula) against `generate_dataset_item()` reference — directly tests the "wrong final data, not wrong instructions" hypothesis end-to-end.

## Files to modify

### `include/armrx/jit_compiler_a64_static.hpp` (line 68, after `_end`)
Add three declarations:
```cpp
void randomx_calc_dataset_item_aarch64_light();
void randomx_calc_dataset_item_aarch64_light_store_result();
void randomx_calc_dataset_item_aarch64_light_end();
```

### `src/jit_compiler_a64_static.S` (around line 1000)
Insert new prologue **immediately before** `DECL(randomx_calc_dataset_item_aarch64_prefetch):` (line 1003). Must textually precede `_prefetch` because the trailing `b rx_calc_dataset_item_prefetch` branch offset (link-time computed) must remain valid after memcpy to a different runtime address.

Design:
- `sub sp, sp, 80` (not 112)
- Save x4-x13 only (5 stp pairs: x4,x5 at [sp]; x6,x7 at [16]; x8,x9 at [32]; x10,x11 at [48]; x12,x13 at [64])
- Same rl[0..7] computation as original (madd/ldr/eor sequence with superscalarMul0_light..Add7_light literals)
- `b rx_calc_dataset_item_prefetch`
- New literal pool with the same 8 constants (superscalarMul0_light etc.)

Insert new epilogue **after** `DECL(randomx_calc_dataset_item_aarch64_end):` (line 1043):
- `stp x0,x1,[x9]` through `stp x6,x7,[x9,48]` (same store pattern, stores computed result)
- Restore x4-x13 from matching offsets, `add sp, sp, 80`, `ret`

### `src/jit_compiler_a64.cpp`

**`CalcDatasetItemSize`** (lines 118-131): Update the prologue term to use `_light` → `_prefetch` range, and the epilogue term to use `_light_store_result` → `_light_end` range. The loop terms (prefetch+4→mix, mix→store_result) stay unchanged.

**`generateSuperscalarHash()`** (lines 1060-1193):
- Line 1064: change `p1` source from `randomx_calc_dataset_item_aarch64` to `_light`
- Lines 1190-1191: change both `p1`, `p2` sources from `_store_result`/`_end` to `_light_store_result`/`_light_end`
- The loop-internal memcpy calls (prefetch+4→mix at line 1088-1090, the superscalar instruction emission, and the implicit loop via codePos placement) stay **completely unchanged**

### Files NOT to touch
- `src/jit_compiler_a64_static.S`: the original `randomx_calc_dataset_item_aarch64` function (lines 942-1043), the shared prefetch/mix/store_result blocks, and the dead-code fast-mode path `randomx_init_dataset_aarch64_main_loop` — leave all completely unmodified
- Any other file

## DO NOT

1. **Splice mismatched frame sizes**: pairing the light prologue (80-byte frame) with the original epilogue (112-byte restore + `add sp,sp,112`) is invalid — guaranteed `sp` drift, proves nothing. Always keep frame size consistent within each variant.
2. **Touch the shared prefetch/mix code**: it's genuinely shared, uses only x0-x13, no stack references, and was correct in both the original and the prior attempt.
3. **Skip the static+dynamic verification step** before running on-device — the prior attempt found instruction-level bugs this way that would have caused silent wrong hashes if run.

## Verification commands (on-device, after build)

Run in this order — stop at the first failure:

```sh
# 1. Static verification: confirm new labels exist and offsets look right
armrx --jit-dump 2>&1 | grep -c "CodeSize"  # should show positive result

# 2. Single-hash smoke test (must complete promptly, < 60s, not hang)
armrx --mine --seconds=1 --warmup=0 2>&1 | grep -E "Steady-state|Speed|hash"

# 3. Full test suite
ctest --test-dir build --output-on-failure -j2
# Key individual tests:
build/test_mining --kat-only             # KATs (interpreter + JIT)
build/test_jit_equivalence               # JIT = interpreter equivalence
build/test_jit_determinism               # JIT determinism
build/test_jit_dataset_2way              # 2-way dataset (regression check)
build/test_partial_dataset               # Track B dataset test
```

## If it fails again

Revert cleanly (`git checkout --` the 3 changed files). Update `docs/experiments/light-mode-dataset-item-prologue-attempt.md` with what this attempt additionally ruled out (don't create a second competing doc). Track C remains blocked.
