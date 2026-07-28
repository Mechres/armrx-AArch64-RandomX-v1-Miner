# Track B: Inline Hit-Path Attempt — Report

## Validated Headline Finding

**The concept works.** A ~25% dataset-hit rate reduces instructions/hash by **~9%**
(from 138M to 125.5M) and cycles/hash by **~10%** (from 181.1M to 162.5M) at 1 worker,
measured on the earlier assembly-based hybrid path (which did work when the activation
guard was manually bypassed).

The flat H/s (4.28 vs 4.28 baseline) was **thermal-DVFS masking**: the 3-minute fill
phase heated the SoC by ~10°C, reducing CPU frequency by ~10%, cancelling the cycle
savings. Instructions/hash and cycles/hash are frequency-independent and show the real
benefit. In production, seed rotation happens every ~2.8 days, amortizing the fill heat
to nothing — the frequency-equalized number is the deployment-relevant one.

**The implementation failed.** Two separate JIT-boundary bugs prevented the hit path
from running in production: the template-based assembly path and the inline-emitted
path both crashed. This doc records what was tried, what failed, and what the next
attempt should know.

## Implementation Attempts

### Attempt 1: Template-based _end_hybrid entry in static.S

A new `_end_hybrid` entry was added to `jit_compiler_a64_static.S`, mirroring
`_end_light` but adding a bound check before the `bl rx_calc_dataset_item`. The bound
check read `partial_dataset_items_` from `MemoryRegisters` using a saved register.

**Root cause of crash (x21 clobber):** The MemoryRegisters* pointer was saved to x21
in the prologue (`mov x21, x1`). The prologue then executes `ldp x9, x1, [x1]` which
loads `mx` into x9 and `memory` into x1. Later, the main loop executes
`ldr x21, <literal_x21>` which OVERWRITES x21 with a literal pool value. When the
JIT program's B instruction branches to `_end_hybrid`, x21 contains garbage, the
bound check reads invalid values, and the program crashes.

**Cross-reference:** This is the same class of undocumented-register-contract issue
that the project's audit flagged for `rx_calc_dataset_item`. The prologue's register
usage (which registers it preserves) is not documented, and the emitted code (the 256
VM instructions + the main loop template) is not checked for register conflicts with
new template entries.

**Reverted in commit 7429537.** The fix was to always jump to `_end_light` from the
emitted program, disabling the hybrid path entirely. The `_end_hybrid` entry is kept
as a fallback identical to `_end_light` but is never branched to.

### Attempt 2: Inline-emitted bound check in JIT compiler C++ code

Instead of using a template entry, the bound check was emitted directly in
`generateProgramLight()` as ARM64 instructions before the `B _end_light` branch.
The items count and partial dataset pointer were embedded as MOVZ/MOVK immediates
at compile time (every hash in light mode = fresh values).

**Two bugs:**

1. **LDP/STP encoding error** — the LDP/STP base encoding was wrong, causing an
   "Illegal instruction" (SIGILL) at the first hash where the bound check activated
   (~30s in, after fill chunk completed). Fixed by using exact encodings from the
   template (`0xA9400620` for LDP with x17 base, `0xA90003E0` for STP with sp base).

2. **Offset-applied comparison** — the bound check compared x2 (which has the dataset
   offset applied from the light path's item-number computation) against
   `partial_dataset_items_` directly. For items near the upper end of the cached
   range with a non-zero dataset offset, the comparison could say HIT while the
   address calculation `ptr + x2 * 64` went beyond the 512 MiB buffer, causing a
   SIGSEGV. Fixed by comparing against `items + ds_offset_items` instead.

**Despite both fixes, the 112s crash remained —** a repeatable SIGSEGV ~112s into a
long benchmark (`--seconds=360 --warmup=240`). This was not fixed within the 2-hour
budget.

**Key untested hypothesis for the 112s crash:** The inline path baked `items` and
`dataPtr` as MOVZ/MOVK immediates at compile time. The deleted assembly path read
the bound LIVE from `MemoryRegisters` with acquire/release ordering. Baking the
values changes the concurrency story:

- **Torn capture:** The items count is read from the PartialDataset's atomic while
  a fill worker is concurrently writing items and publishing a new count. Even with
  acquire/release ordering, the atomic is read ONCE per hash (at JIT compile time),
  and the baked value is used for all 2048 loop iterations. If the fill publishes
  a new count between recompiles, the baked value is stale.
- **Dangling pointer:** The baked `dataPtr` could become invalid if the PartialDataset
  buffer is ever replaced (seed rotation, destructor detach path). A crash at ~112s
  of sustained running, not immediately, fits this class — the pointer is valid
  initially but becomes dangling after a specific event.

This is **hypothesis, not conclusion**. The next diagnostic should reproduce the
crash and correlate the timing with fill completion / job events / recompiles, then
consider returning to the live-load design — the pre-fix assembly path's memory
access pattern never crashed; its bug was the activation guard, not the loads.

### Bound-Check Convention

The item number x2 at the emission point (after the light-path computation) has
**the dataset offset applied**. The correct comparison for the bound check is:

```
item_with_offset < cached_count + dataset_offset / CacheLineSize
```

Both the raw and offset-applied approaches are valid IF the comparison threshold is
adjusted accordingly. The assembly `_end_light` path applies: `&dataset[item + ds_off]`
where `item = (mx >> 32) & mask >> 6`. For the partial dataset, the comparison is:
`item_with_ds_offset < items + ds_off_in_items`. If using the raw item (before ds_off):
`item_raw < items`.

## Kept Fixes (not reverted)

The following are independently verified and kept in the codebase:

1. **PoolManager self-deadlock** — `pool_name_nolock()` helper for use inside
   `tick()`'s locked region; `connect_to_current()` config calls under lock with
   captured raw pointer. (Commit range: Part 2 fixups)
2. **Fill-thread pinning fix** — `exclude_cores` parameter in `start_fill()` prevents
   fill workers from pinning to mining-occupied cores. `avail_cores` = `core_order`
   minus `exclude_cores`. (Commit e84752c)
3. **Cache lifetime holder** — `shared_ptr<const Argon2dCache>` per fill worker,
   each owning an independent reference. Destructor checks fill completion before
   `munmap`; detaches and skips `munmap` if fill still in progress. (Commit ff9b7f3)
4. **PartialDataset class** — mmap/MADV_HUGEPAGE, CLI `--dataset-mb=N`, differential
   test. Feature is inert without the hit path; kept for when work resumes.
5. **Always-jump-to-`_end_light` safety state** — hybrid path is disabled; all JIT
   light-mode programs branch to `_end_light` regardless of `useHybrid`. No template
   or inline hit-path code is emitted. (Commit 7429537)
6. **Pre-existing -Wconversion fixes** — ~20+ explicit casts in `jit_compiler_a64.cpp`
   and `vm.cpp` for GCC 15 on musl (all pre-existing code, not Track B).

## Files Changed

| File | Change | Status |
|------|--------|--------|
| `include/armrx/program.hpp` | MemoryRegisters fields | KEPT |
| `include/armrx/partial_dataset.hpp` | New: PartialDataset class | KEPT |
| `src/partial_dataset.cpp` | New: PartialDataset impl | KEPT |
| `include/armrx/vm.hpp` | `set_partial_dataset()`, atomic ptr | KEPT |
| `src/vm.cpp` | `run_jit()` hybrid setup | KEPT (inert) |
| `src/mining_engine.cpp` | Fill start, core exclusion | KEPT |
| `include/armrx/mining_engine.hpp` | PartialDataset pointer, flag | KEPT |
| `src/miner_app.cpp` | PartialDataset creation | KEPT |
| `src/cli_parser.cpp` | `--dataset-mb=N` flag | KEPT |
| `src/jit_compiler_a64_static.S` | `_end_hybrid` entry (fallback only) | KEPT (inert) |
| `src/jit_compiler_a64.cpp` | Inline hit path emission | **REVERTED** |
| `include/armrx/jit_compiler_a64.hpp` | Signature change | **REVERTED** |
| `tests/test_partial_dataset.cpp` | Differential test | KEPT |

## Gate B Status

**Not run.** The Gate B matrix (0 vs 512 MiB, 1 and 8 workers, warmup=60/seconds=180,
perf stat, reversed trial order, MemAvailable logging) was not executed because the
hit path must work for the 512 MiB numbers to be meaningful.

**When work resumes, the protocol is:**
- Primary metric: cycles/hash and instructions/hash (frequency-independent)
- Equalize thermal state: insert cooldown after fill completion until temperature
  returns to within 2°C of baseline start temp; log start/end temps per trial
- 0 vs 512 MiB, 1 and 8 workers, warmup=60/seconds=180 (baseline), warmup=240+
  for 512 MiB (fill takes ~180s), perf stat, reversed trial order
- The 8-worker number decides ship/no-ship

## Changelog Correction

Commit 7429537 was titled "Fix segfault" but it **disabled the hybrid path** — the
segfault was avoided by no longer executing the buggy code, not by fixing the bug.
The feature remains disabled in the current state (always jump to `_end_light`).
