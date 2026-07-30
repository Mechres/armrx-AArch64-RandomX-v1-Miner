# Track D2 — Cross-Hash Boundary Pipelining

**Date:** 2026-07-30
**Status:** ✅ Implemented, verified correct — not benchmarked on-device

## Summary

Overlap the AES finalization of hash N (reads scratchpad, `hash_aes_1r_x4`)
with the AES fill of hash N+1 (writes scratchpad, `fill_aes_1r_x4`).
These two phases are independent per-hash operations (~12.3% of cycles each in
the original A/B) that run sequentially with no data dependency. By interleaving
them, we hide memory latency on the in-order Cortex-A53.

**Result: Correct on host (all KATs pass). On-device A/B not completed
(mining tests too slow in fast mode on 1.4 GiB RAM device).**

## Mechanism

Rather than the classic full dual-nonce instruction-interleave (Track D3,
contraindicated by D1's L1I result + Track A item 4's 100% register liveness
finding), D2 only overlaps the **boundary** between two consecutive hashes:

```
Before (sequential):
  Hash N:   [VM program] → [AES finalization (read scratchpad)] → output
  Hash N+1: [Blake2b → AES fill (write scratchpad)] → [VM program] → ...

After (pipelined):
  Hash N:   [VM program] → ──[AES finalization]── → output
  Hash N+1:                 ──[Blake2b → AES fill]── → [VM program] → ...
```

This is much smaller than a full interleave (~cycle share of the AES boundary
operations only) but also has zero register-pressure risk — neither the
finalization nor the fill touch the RandomX integer/float register file.

## What was implemented

### Files changed (6 files, +420/−22)

| File | Δ | What |
|------|---|------|
| `include/armrx/aes_hash.hpp` | +12 | Declare `hash_and_fill_aes_interleaved_x4` |
| `src/aes_hash.cpp` | +139 | Interleaved read/write AES function — NEON and scalar T-table paths |
| `include/armrx/vm.hpp` | +29 | `set_scratchpad()` (munmaps old owned buffer, takes external pointer), `scratchpad_span()`, `randomx_calculate_hash_pipelined` declaration; `bool scratchpad_owned_` flag |
| `src/vm.cpp` | +78 | `randomx_calculate_hash_pipelined` implementation, `set_scratchpad()` method body |
| `src/mining_engine.cpp` | +127 | Double-buffered 2 MiB scratchpads, primed pipeline, alternating hash+fill/finalize worker loop |
| `include/armrx/vm.hpp` (+1) / `src/vm.cpp` (+1) | (+1) | Destructor guard: only `munmap`s scratchpad when owned (not external buffer) |

### Core function

`hash_and_fill_aes_interleaved_x4` replaces the sequential pair:
```cpp
hash_aes_1r_x4(hash_scratchpad, ...);   // read existing scratchpad
fill_aes_1r_x4(fill_scratchpad, ...);    // write new scratchpad
```
with an interleaved version that processes one block from each at a time,
allowing the memory subsystem to overlap reads and writes.

### MiningEngine worker loop

- Allocates **two 2 MiB scratchpads** instead of one
- **Prime phase**: first hash runs standard `randomx_calculate_hash` to fill
  scratchpad[0]
- **Pipeline phase**: `randomx_calculate_hash_pipelined` hash N's data from
  `sp[N%2]` while `randomx_calculate_hash` fills `sp[(N+1)%2]` for hash N+1
  (the prime buffer already has the data for hash 0)
- On job change: pipeline resets (`pipelined_active = false`), local nonce
  reinitializes

## Bugs found and fixed

### 1. Pipeline state not reset on job change

After `set_job()`, `pipelined_active` stayed `true` with stale `block_input`
buffer content. The next pipelined hash would write the wrong nonce bytes into
`block_input`, then produce a hash from the old job's nonce values.

**Fix:** Added `pipelined_active = false; local_nonce = thread_id; current_nonce
= 0;` at the job-change site in `worker_loop()`.

### 2. Nonce tracking confusion

The pipelined path used `nonce` (the *next* nonce after advancement) in the
share callback instead of the actual nonce embedded in `block_input`. The prime
path also had a latent double-advancement bug where `block_input` nonce was
overwritten before both the hash+fill and finalization steps.

**Fix:** Added `std::uint64_t current_nonce` to the worker context, tracking
what's actually in the block buffer. The share callback reads `current_nonce`,
which is set before each hash operation and never advanced mid-pipeline.

### 3. VM destructor double-munmap (latent)

The VM destructor unconditionally calls `munmap` on `scratchpad_data_`, but the
pipelined mining engine replaces this pointer with an externally-owned buffer
(via `set_scratchpad()`). Without tracking ownership, the destructor would
either double-free (if the buffer was `new[]`-allocated) or `munmap` a pointer
not obtained from `mmap`.

**Fix:** Added `bool scratchpad_owned_` (default `true`). `set_scratchpad()` sets
it to `false`. The destructor only `munmap`s when `scratchpad_owned_` is true.

## Why not benchmarked on-device

The mining KATs (`test_mining`) take ~3-5 minutes host-side in fast mode
(2080 MiB dataset, tested via tmpfs). On the device (1.4 GiB RAM + zram), the
dataset initialization alone takes 3-5 minutes per test case, and the full test
suite attempts multiple fast-mode cycles (dataset reinit after seed change).
The process was killed after 15+ minutes still in the first lifecycle test's
initialization phase. An A/B benchmark would require:
- Mining engine test infrastructure that exercises the pipeline path without
  full dataset init (currently impossible — fast mode requires all 2080 MiB)
- Or a dedicated microbenchmark for `hash_and_fill_aes_interleaved_x4` alone

The host-side verification is strong: all KATs match, and `test_mining` (which
exercises the full worker loop including the pipelined path, job changes, and
share callbacks) passes completely on x86_64.

## Estimated impact

~0.5–1% hashrate, based on the ~12.3% cycle share of each AES boundary phase
and the overlap window. Not confirmed on-device.

## Key files

- Implementation brief: `docs/plans/d2-pipelined-hash-fill-plan-20260730.md`
- Changelog entry: `changelogs.md` (2026-07-30)
- AES hash primitives: `src/aes_hash.cpp`
- VM pipeline: `src/vm.cpp`
- Mining engine integration: `src/mining_engine.cpp`
