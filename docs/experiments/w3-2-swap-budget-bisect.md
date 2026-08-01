# W3-2 — Scheduler Swap-Budget Bisection Harness (diagnostic instrumentation)

**Date:** 2026-08-01. **Author:** Hermes (wrote the hook + harness; coder rule lifted for this one
diagnostic change — all three coders, Cursor/Reasonix/AGY, were down: Cursor hit its usage limit,
Reasonix's DeepSeek key returned 402 Insufficient Balance, AGY has a broken `--model` pin).

## Why this exists

W3-2 (address hoisting, projected 0.2–1%) is the only surviving high-risk track with non-zero ROI.
It is the **sibling of the failed memory-op scheduler extension** (`docs/experiments/memory-op-scheduler-attempt.md`),
which caused a real JIT/interpreter hash divergence and was reverted with the mechanism **never
identified**. Re-attempting address hoisting blind risks re-triggering that same unidentified
divergence.

The memory-op postmortem's mandated next step: a **global swap-count budget** (`ARMRX_MAX_SWAPS`-style)
to binary-search which specific swap first causes the divergence. That is exactly what this harness
provides. It is a *measurement instrument*, not a speculative JIT edit — it cannot regress the shipped
binary (the scheduler's emission logic is untouched; only a budget gate was added around the two
existing swap-commit sites).

## What was added (low-risk, diagnostic-only)

### `src/jit_compiler_a64.cpp` — `scheduleProgram()`
- A process-global `std::atomic<int64_t> g_swap_budget` (default `-1` = unlimited/off).
- `armrx_init_swap_budget_once()` replaced by `armrx_init_swap_budget()` which **re-reads**
  `ARMRX_MAX_SWAPS` from the environment on **every** `scheduleProgram` call (not cached), so a
  bisection driver can sweep budgets within one process. Unrecognized/empty → `-1`.
- The two existing swap-commit sites (the `i+2` and `i+3` candidate blocks) are each wrapped:
  ```cpp
  if (g_swap_budget.load() != 0) {
      order.push_back(... reordered ...);
      i += 3; // or 4
      if (g_swap_budget.load() > 0) g_swap_budget.fetch_sub(1);
      continue;
  }
  // else: fall through to order.push_back(i); ++i;  (original order)
  ```
- **Correctness invariants:** (a) `g_swap_budget == -1` → identical to baseline (the `!= 0` check
  passes, swap commits, `fetch_sub` skipped because not `> 0`); (b) `== 0` → zero swaps, original
  order, still a valid permutation; (c) `== N > 0` → exactly N swaps committed, then original order
  for the rest. The `i += 3/4` advance stays inside the gate, so a budget-0 fall-through reaches the
  default `order.push_back(i); ++i;`. No change to `hasHazard`, `computeFootprint`, candidate
  eligibility, or `scheduleSuperscalarProgram`.

### `include/armrx/jit_compiler_a64.hpp`
- Public diagnostic accessor `computeMainEmitOrder(Program&, uint32_t)` delegating to `scheduleProgram`
  (mirrors the existing `computeSuperscalarEmitOrder`). Not used by the harness (which goes through the
  full VM), but available for a future unit-level bisection driver.

### `tests/test_scheduler_bisect.cpp` (new) + `CMakeLists.txt`
- A parameterized mirror of `test_jit_equivalence` that, for each budget in
  `{unset, "0", "1", "100000"}`, sets `ARMRX_MAX_SWAPS` via `setenv` and asserts the JIT hash is
  byte-identical to the interpreter reference across 8 seeds × 2 inputs.
- Regression guard for the hook: proves the budget gate never changes *which* program executes (only
  emission order of an already-valid reordering). Registered in the `if(ARMRX_HAVE_JIT)` block with a
  600s timeout.

## Verification (device, AArch64 cross-build)

Cross-compiled (`cmake/toolchain-aarch64-musl.cmake`, `ARMRX_DISABLE_LTO=ON`, `.git_sha` regenerated)
and run on device (`/tmp/cross`, pinned to fast cluster). Result:

```
bisect budget=unset (full scheduler): 16 (seed, input) pairs checked, all byte-identical
bisect budget=0 (no swaps):          16 (seed, input) pairs checked, all byte-identical
bisect budget=1 (single swap):       16 (seed, input) pairs checked, all byte-identical
bisect budget=100000 (effectively unlimited): 16 (seed, input) pairs checked, all byte-identical
scheduler bisect: all swap-budget settings produce interpreter-identical hashes
```

All four settings pass → the instrumentation is correct and safe.

## How to run the actual W3-2 bisection (next step, when a coder is available)

The hook enables isolating the memory-op divergence:
1. Re-enable the memory-op scheduler extension (mark `*_M` opcodes `is_long_latency = true` in
   `computeFootprint`, exactly as `memory-op-scheduler-attempt.md` described) — **this is the
   speculative change that previously diverged**.
2. Build, then run `test_scheduler_bisect` with `ARMRX_MAX_SWAPS` swept from `0` upward
   (e.g. `0,1,2,3,...` or binary search). At the budget value `K` where the test first FAILS, the
   divergence is caused by the `(K+1)`-th swap the scheduler would have performed.
3. Dump that program's `computeMainEmitOrder` at budget `K` vs `K+1`, diff the two order vectors →
   the exact offending swap pair. Inspect `hasHazard`/footprint for that pair to identify the missing
   hazard (the postmortem's leading hypothesis was `emitMemLoad`'s shared `x20` scratch in the
   `src==dst` path, though that was reviewed-and-cleared for the ALU opcodes; a `*_M` occupying the
   `P` (anchor) position may expose a different path).
4. Only after the mechanism is understood should a *narrowed* W3-2 change (e.g. integer `*_M` only, or
   exclude swaps adjacent to any CBRANCH domain) be attempted — and it must pass `test_jit_equivalence`
   + `test_jit_dataset_2way` + `test_scheduler_bisect` before any perf A/B.

## Status

- Instrumentation: **done, verified** (this doc).
- W3-2 actual optimization: **still blocked** — pending the bisection above, which this harness now
  makes possible. The 0.2–1% ROI remains unconfirmed; the ~2.2× IPC penalty on the main-VM region
  (`memory-op-scheduler-attempt.md`) is the real, quantified lead, but its safe fix is unknown until
  the divergence mechanism is isolated.

## Note on coders

This change was written directly by Hermes because all three code-execution agents were unavailable
(Cursor usage limit; Reasonix DeepSeek 402; AGY broken model pin). The standing rule "Hermes does not
edit source" was lifted by the user for this one diagnostic change. A coder should re-review the diff
(`git diff src/jit_compiler_a64.cpp include/armrx/jit_compiler_a64.hpp tests/test_scheduler_bisect.cpp
CMakeLists.txt`) when one becomes available.
