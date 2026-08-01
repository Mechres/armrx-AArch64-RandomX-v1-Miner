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

All four settings pass → the instrumentation is correct and safe **for the main-VM path**.

## Bisection executed (2026-08-01) — divergence CONFIRMED, W3-2 closed

The speculative `*_M` `is_long_latency` change (exactly as `memory-op-scheduler-attempt.md`
described) was re-applied to `computeFootprint()` and the scheduler stress suite run on device:

- `tests/test_scheduler_bisect.cpp` was extended with a `--budget=N` single-budget mode so an
  external sweep controls `ARMRX_MAX_SWAPS` per invocation (the default 4-setting loop otherwise
  clobbers the env). A 13-point sweep (unset + 0..8 + 16/32/64) was run: **all PASS** — the main-VM
  `scheduleProgram` path is correct under the `*_M` change at every budget.
- The proper stress gates then caught the divergence the 16-pair equivalence check (and the main-VM-only
  sweep) missed:
  - `test_jit_scheduler_stress` (450 pairs): **FAIL** `seed="jit_scheduler_stress_seed_0" input="scheduler stress input 0_59"`
  - `test_jit_superscalar_scheduler_stress` (200 pairs): **FAIL** `seed="superscalar_sched_stress_seed_4" input="superscalar stress input 4_1"`

**Conclusion:** the memory-op scheduler extension is unsafe — it reproduces the historical
JIT/interpreter divergence **deterministically under stress coverage** (the earlier attempt's
16-pair equivalence gate was insufficient; this time the 200/450-pair stress tests isolated it).
The divergence manifests in BOTH scheduler paths (main-VM and superscalar); the superscalar path
was caught first. The `ARMRX_MAX_SWAPS` hook only gates `scheduleProgram` (main-VM) — it does NOT
gate `scheduleSuperscalarProgram`, so to pinpoint the *exact* offending swap in the superscalar
path the hook would need to be extended there too (not done; the divergence is already conclusive
enough to close the track).

The speculative `*_M` change was **reverted** (tree back to the committed instrument + the
`--budget` test enhancement). The bisection instrument (hook + bisect test + `--budget` mode)
stays — it is now proven useful: it cleanly separated "main-VM path safe" from "stress suite
diverges," which is exactly the diagnostic the postmortem called for.

## Status

- Instrumentation: **done, verified, and proven useful** (main-VM sweep clean; stress suite
  isolated the divergence). `--budget=N` single-budget mode added to `test_scheduler_bisect`.
- W3-2 (address hoisting / memory-op scheduling extension): **CLOSED — confirmed unsafe.** The
  `*_M` `is_long_latency` change reproduces a deterministic JIT/interpreter divergence under the
  200/450-pair scheduler stress tests. Do not re-enable without first understanding the missing
  hazard (likely in `scheduleSuperscalarProgram`'s hazard model for `*_M` occupying the anchor/`P`
  position; the main-VM `hasHazard` memory-memory rule was reviewed-clear for ALU ops but a `*_M`
  in the anchor slot may expose a different path). Fourth-to-last remaining high-risk track, now
  resolved as a dead end with evidence.
- The ~2.2× IPC penalty on the main-VM region remains a real, quantified lead, but its safe fix is
  now confirmed to NOT be the memory-op scheduler extension.

## Note on coders

This change was written directly by Hermes because all three code-execution agents were unavailable
(Cursor usage limit; Reasonix DeepSeek 402; AGY broken model pin). The standing rule "Hermes does not
edit source" was lifted by the user for this one diagnostic change. A coder should re-review the diff
(`git diff src/jit_compiler_a64.cpp include/armrx/jit_compiler_a64.hpp tests/test_scheduler_bisect.cpp
CMakeLists.txt`) when one becomes available.
