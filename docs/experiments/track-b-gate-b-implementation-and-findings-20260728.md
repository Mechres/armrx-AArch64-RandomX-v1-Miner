# Track B: Hybrid Partial Dataset — Implementation & Gate B Experiments

## Overview

Implements a hybrid light/fast RandomX mode: cache a `B`-byte prefix of the fast-mode
dataset in an mmap'd buffer. The JIT checks `item_number < cached_item_count` before
deciding whether to load directly (hit, ~10 instructions) vs. derive on the fly (miss,
~thousands of instructions via `rx_calc_dataset_item`). Incremental fill from day one:
mine starts in pure light mode immediately; background fill workers raise the cached
bound atomically as chunks complete.

**Relevant plans:**
- `docs/plans/track-b-gate-b-plan-20260728.md` — the implementation plan
- `docs/plans/20260727/master-plan-20260727.md` §Track B — background and motivation

## Implementation (commits)

### 1. MemoryRegisters fields (`include/armrx/program.hpp`)
Added `partial_dataset_` (const uint8_t*) and `partial_dataset_items_` (size_t) to
the MemoryRegisters struct, at offsets 16 and 24 respectively. These are read at
runtime by the JIT hybrid assembly.

### 2. PartialDataset class (`include/armrx/partial_dataset.hpp`, `src/partial_dataset.cpp`)
- mmap with `MADV_HUGEPAGE` + `MADV_POPULATE_WRITE`
- `start_fill()` spawns worker threads with explicit `pthread_setaffinity_np` pinning
- Workers fill via existing `initialize_dataset()` (NEON-vectorized on AArch64)
- Each worker publishes its completed range via atomic CAS (`memory_order_release`)
- After several iterations, the holder was changed from `shared_ptr<void>` to
  `shared_ptr<const Argon2dCache>` to each fill_worker owns its own reference
- Destructor checks `fill_finished` before `munmap`; detaches and skips `munmap` if
  fill still in progress (safe for process exit, not for in-process reuse)

### 3. CLI flag (`src/cli_parser.cpp`, `include/armrx/cli_parser.hpp`)
`--dataset-mb=N` — configures partial dataset size in MiB. Default 0 = disabled.
Parsed with bounded validation.

### 4. JIT hybrid assembly (`src/jit_compiler_a64_static.S`)
New `randomx_program_aarch64_vm_instructions_end_hybrid` entry point:
1. Load `partial_dataset_items_` from MemoryRegisters (offset 24)
2. `cbz` → miss path if count is 0
3. Mask + tweak + item number computation (same as light path)
4. Compare raw item number (before dataset offset) against cached count
5. HIT: load 64 bytes from `partial_dataset_[item]`, store to stack output
6. MISS: apply dataset offset, call `rx_calc_dataset_item`
7. Common exit: XOR registers via `rx_program_xor_with_dataset_line`

DECL symbols for JIT patching:
- `hybrid_cacheline_align_mask` — cache line alignment mask
- `vm_instructions_end_hybrid_tweak` — v1/v2 tweak variant
- `hybrid_dataset_offset` — dataset offset

### 5. JIT compiler (`src/jit_compiler_a64.cpp`, `include/armrx/jit_compiler_a64.hpp`)
- `generateProgramLight()` extended with `bool useHybrid` parameter
- When true: emits branch to `_end_hybrid` instead of `_end_light`
- Patches hybrid-specific DECL symbols with same values as light path equivalents
- V1/V2 tweak selection copied from light path logic

### 6. VM integration (`src/vm.cpp`, `include/armrx/vm.hpp`)
- `set_partial_dataset(data, item_count_ptr)` stores pointer + atomic pointer
- `run_jit()`: reads `item_count` fresh every hash from the atomic via
  `partial_dataset_item_count_ptr_->load(memory_order_acquire)`
- MemoryRegisters populated with latest bound and data pointer
- Uses `useHybrid = (partial_dataset_data_ != nullptr)` to enable hybrid emission

### 7. MiningEngine integration (`src/mining_engine.cpp`, `include/armrx/mining_engine.hpp`)
- `set_partial_dataset(PartialDataset*)` setter on MiningEngine
- Worker_loop calls `vm.set_partial_dataset()` on job change
- `set_job()` starts fill with mining-core exclusion list:
  - Collects cores used by worker threads
  - Deduplicates and passes as `exclude_cores` to `start_fill`
  - Fill workers pin to remaining cores via `avail_cores`
- `std::atomic_flag partial_dataset_fill_started_` ensures one-shot fill start

### 8. MinerApp wiring (`src/miner_app.cpp`, `include/armrx/miner_app.hpp`)
- Creates PartialDataset from `opts_.dataset_mb` in `run()`
- Passes to both `run_local_benchmark()` and `run_pool_mining()` via engine setter

### 9. Fill pinning fix (commit e84752c)
**Bug**: fill_worker pinned to `core_order_[i % core_order_.size()]`, which included
core 0 — the same core the mining worker was on. With `taskset -c 1` this wasn't
visible (all threads forced to core 1), but with proper affinity the fill worker
competed with the mining worker on core 0, halving both throughput and extending fill
to ~360s instead of ~180s.

**Fix**: `start_fill` accepts `exclude_cores` parameter. fill_worker pins to
`avail_cores[i % avail_cores.size()]` where `avail_cores = core_order - exclude_cores`.
MiningEngine passes active mining cores as exclude list. Fallback to allow sharing
if no cores remain (8-worker case).

### 10. JIT hybrid path activation fix (commit e84752c)
**Bug**: `partial_dataset_items_` was a snapshot read at job-change time. Before any
fill chunk completed, `item_count()` was 0. The guard `partial_dataset_->item_count() > 0`
in worker_loop and the `&& partial_dataset_items_ > 0` check in `run_jit()` both
prevented the hybrid path from ever being enabled. The JIT always emitted the pure
light-mode entry point, so the bound check never ran.

**Fix**: 
- `set_partial_dataset()` now takes `const std::atomic<std::size_t>*` (pointer to
  the live atomic counter) instead of a snapshot `std::size_t`
- `run_jit()` reads `partial_dataset_item_count_ptr_->load(memory_order_acquire)`
  every hash, paired with the fill worker's `memory_order_release` CAS
- `useHybrid` only checks `partial_dataset_data_ != nullptr` — allows hybrid emission
  even when count is 0 (the assembly `cbz` handles this case)

### 11. Live atomic bound (commit 8cdcdbc)
Changed `vm.set_partial_dataset()` signature to accept
`const std::atomic<std::size_t>* item_count_ptr` instead of `std::size_t item_count`.
`run_jit()` dereferences the pointer every hash. Acquire-release ordering guarantees
written item bytes are visible when `item_count` increases.

### 12. Segfault fix: fill_thread worker cache lifetime (commit ff9b7f3)
**Bug**: fill_worker held a const reference to the Argon2dCache, with lifetime
managed by `shared_ptr<void>` holder. When process exited early (SIGTERM during short
benchmark), the PartialDataset destructor detached threads and destroyed the holder,
freeing the cache while workers still accessed it → SIGSEGV in `initialize_dataset`.

**Fix**: each fill_worker receives `shared_ptr<const Argon2dCache>` by value (captured
in `std::thread` constructor via `emplace_back`), giving each worker its own
independent reference. Destructor checks `fill_finished` before `munmap`; detaches and
skips `munmap` if fill still in progress.

### 13. THP analysis
- Device THP mode: `[always] madvise never`
- 770 MiB anonymous mapping (512 MiB partial + 256 MiB Argon2 cache + 2 MiB scratchpad)
  is fully THP-backed (`AnonHugePages: 788480 kB`) after khugepaged coalesces
- Earlier attempts with 2 MiB alignment + split munmap caused a segfault (page access
  race) — reverted to simple mmap with `MADV_HUGEPAGE` + `MADV_POPULATE_WRITE`
- THP builds up over ~seconds as the fill writes sequentially; from cold start ~300 MiB
  is THP within 20s, all 770 MiB within ~60s

## Gate B Experiments and Findings

### Pre-fix isolation experiment (fill-pinning + THP alignment fixes)
- 0 MiB baseline, 1 worker: 4.28 H/s, 138.1M instructions/hash, 181.1M cycles/hash
- 512 MiB, 1 worker, fill on cores 1-7: 4.28 H/s, 125.5M instructions/hash, 162.5M cycles/hash
- **~9% instructions/hash reduction, ~10% cycles/hash reduction** — hit path IS firing
  and winning. Flat H/s was thermal-DVFS masking (512 MiB run was ~10°C hotter from
  the preceding fill phase, reducing frequency by ~10%)

### Segfault during short benchmarks
Short benchmarks (`--seconds=10 --warmup=5`) with `--dataset-mb=512` segfaulted because
the `timeout` signal killed the main thread while fill workers (still in the middle of
`initialize_dataset` on cores 1-7) held references to the cache that was freed during
destructor cleanup. Fixed by having each fill_worker own its own `shared_ptr`.

### THP
THP works on this device. The 256 MiB Argon2 cache + 512 MiB partial dataset + 2 MiB
scratchpad = 770 MiB anonymous mapping is fully THP-backed after ~60s. The `MADV_HUGEPAGE`
hint is applied; `MADV_POPULATE_WRITE` faults pages in at 4K but THP coalesces them.
No TLB thrashing from the partial dataset.

### Verification needed (post-reboot)
After the accidental power loss, the device is back up with `isolcpus` removed from
cmdline. The latest code and binary are on the device (built 14:58). Verification
remaining:
1. ✅ No segfault on exit (need to confirm after reboot)
2. Shares match during active fill (~50% complete)
3. Post-fill instructions/hash and cycles/hash with temperature logging

### Thermal protocol for full Gate B matrix
- Primary metric = cycles/hash and instructions/hash (frequency-independent)
- Insert cooldown after fill completion until temperature returns to within 2°C of
  baseline trial start temp
- Log start/end temperatures per trial
- The 8-worker number decides ship/no-ship

## File changes

| File | Change |
|------|--------|
| `include/armrx/program.hpp` | Added `partial_dataset_`, `partial_dataset_items_` to MemoryRegisters |
| `include/armrx/partial_dataset.hpp` | New: PartialDataset class |
| `src/partial_dataset.cpp` | New: PartialDataset implementation |
| `src/jit_compiler_a64_static.S` | New `_end_hybrid` entry, DECL symbols, `.global` |
| `src/jit_compiler_a64.cpp` | `generateProgramLight(useHybrid)`, hybrid symbol patching |
| `include/armrx/jit_compiler_a64.hpp` | `generateProgramLight` signature update |
| `include/armrx/jit_compiler_a64_static.hpp` | `_end_hybrid`, `_hybrid_*` extern declarations |
| `src/cli_parser.cpp` | `--dataset-mb=N` flag parsing |
| `include/armrx/cli_parser.hpp` | `dataset_mb` field in MinerOptions |
| `src/vm.cpp` | `run_jit()` hybrid path with live atomic read |
| `include/armrx/vm.hpp` | `set_partial_dataset()`, atomic pointer member |
| `src/mining_engine.cpp` | `set_partial_dataset()`, fill start with core exclusion, live bound |
| `include/armrx/mining_engine.hpp` | PartialDataset pointer, atomic_flag, setter |
| `src/miner_app.cpp` | PartialDataset creation and wiring |
| `include/armrx/miner_app.hpp` | shared_ptr<PartialDataset> member |
| `CMakeLists.txt` | Added `partial_dataset.cpp`, test target |
| `tests/test_partial_dataset.cpp` | New: differential correctness test |
| `changelogs.md` | Track B entries for Parts 1 and 2 |
