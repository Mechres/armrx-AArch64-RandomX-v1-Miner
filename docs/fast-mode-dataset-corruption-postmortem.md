# Fast-Mode Dataset Corruption Postmortem

**Date:** 2026-07-21
**Status:** Fixed, verified on both x86_64 (interpreted) and AArch64 (Cortex-A53, JIT, on-device)
**Discovered while:** implementing PLAN.md §2.1 (worker-thread reuse for dataset init) and writing its correctness test

---

## Symptoms

`MiningEngine`'s fast mode (`RandomXMode::fast`, the ~2080 MiB dataset path) has silently produced a dataset that is almost entirely uninitialized (zero-filled) whenever more than one thread participates in building it — which is the normal case, since dataset construction has always been parallelized across `std::thread::hardware_concurrency()` threads. Only the first thread's assigned item range (`start_item == 0`) ever landed in the correct location; every other thread's output overwrote the same starting bytes instead of writing to its own region.

Practical impact: any fast-mode hash computed against such a dataset does not match the RandomX specification's output for that seed/nonce. A real pool independently verifying the hash would reject essentially every fast-mode share. This is not a performance bug — it is a correctness bug that made `--mode=fast` non-functional for its intended purpose whenever the machine had more than one core (i.e., always, in practice).

## Root Cause

`initialize_dataset()` (`src/dataset.cpp`) has a simple, self-consistent, and *correctly implemented* contract: given `output`, `cache`, `start_item`, `item_count`, it computes items `[start_item, start_item + item_count)` and writes them **relative to `output[0]`** — i.e., it expects `output` to be a buffer sized exactly for `item_count` items, not a full dataset buffer indexed at an absolute offset. This is confirmed by its own existing test coverage in `tests/test_blake2b.cpp:125-135`, which allocates a buffer sized for exactly 3 items and calls `initialize_dataset(buf, cache, 5, 3)`, checking `buf[0]`/`buf[64]` against `generate_dataset_item(cache, 5)`/`generate_dataset_item(cache, 6)` — i.e., relative placement is the documented, tested behavior.

`MiningEngine::set_job()`'s multi-threaded dataset build (both the original temporary-thread code and the new worker-reuse code added for §2.1) called it incorrectly:

```cpp
// Wrong — passes the FULL dataset buffer regardless of start_item
initialize_dataset(std::span<std::byte>(new_dataset->data(), new_dataset->size()),
                    *shared_cache_, start_item, count);
```

Every thread computed the *correct item content* for its assigned range (`start_item + offset`), but wrote it to `output[offset]` instead of `output[start_item + offset]`. With N threads partitioning the full ~34M-item dataset:
- Thread 0 (`start_item == 0`): writes to `output[0, count)` — happens to be correct, since offset 0 coincides with absolute position 0.
- Every other thread: writes to `output[0, count)` **again**, racing thread 0 (and each other) for the same bytes, with content for a completely different item range.
- The rest of the buffer (`output[count, full_size)` onward) is never written by anyone — left at its zero-initialized `mmap` default.

### Why existing tests didn't catch it

Every existing caller of `initialize_dataset()` either used `start_item == 0` (`bench_dataset_helpers()`, `test_blake2b.cpp`'s buffer-sized-for-the-request calls) — which never exercises the mismatch, since offset and absolute position coincide at zero — or went through `MiningEngine::set_job()`'s buggy multi-threaded path, whose only verification was `assert()`-based in `test_mining.cpp`, and (see the companion finding below) `assert()` has been compiled out under this project's default Release build (`-DNDEBUG`) the entire time. Two independent gaps compounded: the wrong call convention, and no live assertion to catch it even in principle.

## Companion finding: `assert()` was silently disabled in the default build

`CMakeLists.txt` defaults `CMAKE_BUILD_TYPE` to `Release`, which defines `-DNDEBUG`. `tests/test_blake2b.cpp` (`armrx_tests`, the RandomX KAT suite — 41 assertions) and `tests/test_mining.cpp` use plain `assert()`, which is compiled out entirely under `NDEBUG` (confirmed: a deliberately-broken assertion produced exit code 0 under `-DNDEBUG`, and `nm` on the Release `armrx_tests` binary showed zero `__assert_fail` references). This means every "tests passed" result from the standard, documented build workflow (`cmake -S . -B build && cmake --build build && ctest --test-dir build`) was not actually checking those assertions — including the KATs. `tests/test_jit_encodings.cpp`/`test_jit_determinism.cpp` were unaffected, since they use explicit `if (!cond) return 1;` checks instead of `assert()`.

Fixed by adding `-UNDEBUG` to the `armrx_tests` and `test_mining` CMake targets specifically (`CMakeLists.txt`), forcing `assert()` to stay live regardless of overall build type. This does not affect optimization of the shipped `armrx`/`armrx_core` — it only re-enables real checking in these two test binaries.

## Fix

`src/mining_engine.cpp`, both call sites (the temp-thread fallback in `set_job()` and the new worker-reuse path added for §2.1): pass the correct sub-span instead of the full buffer.

```cpp
// Correct — pass this thread's own sub-span, not the full buffer
initialize_dataset(
    std::span<std::byte>(new_dataset->data() + start_item * kRandomXDatasetItemBytes,
                          count * kRandomXDatasetItemBytes),
    *shared_cache_, start_item, count);
```

`tests/test_mining.cpp`'s own new reference-dataset construction (added for the §2.1 correctness test) had copied the same broken call pattern from the code it was meant to verify, and needed the identical fix.

## Detection

1. Implemented PLAN.md §2.1 (worker-thread reuse for fast-mode dataset rebuilds) and wrote a correctness test that cross-checks sampled hashes from a live `MiningEngine` fast-mode run against an independently-built reference dataset for the same seed.
2. First run of the test reported "0 nonces cross-checked" — passed only because `assert(!job2_samples.empty())` was silently compiled out (`NDEBUG`). Traced this to the Release-build assert issue and fixed it with `-UNDEBUG` on the test targets.
3. Re-ran with real assertions: `assert(!job2_samples.empty())` now passed for real (workers did report job2 shares once given enough time — a second, unrelated timing fix, see below), but `assert(ref_hash == hash)` then failed for real.
4. Wrote a minimal standalone repro (`initialize_dataset` called twice into one shared buffer for sub-ranges `[0,5)` and `[5,10)`, compared against a single direct call for `[0,10)`): item 7 came back all-zero in the partitioned version instead of the correct value, confirming the wrong-destination-offset bug directly.
5. Fixed both `mining_engine.cpp` call sites and the test's own reference construction; re-ran to green.

(A third, unrelated bug was also found and fixed while building this same test: the initial 200ms wait after a live seed rotation was nowhere near enough time for interpreted — non-JIT — fast-mode hashing to produce even one sample on this x86_64 dev sandbox. Changed to a polling loop with a generous timeout instead of a fixed sleep guess.)

## Two more findings from getting this test running on real AArch64 hardware

### The devbox (~1.8 GiB RAM) can't fit fast mode at all, and the original test forced it anyway

The correctness test's first design built an *independent second full dataset* (~2080 MiB) to cross-check against, on top of the engine's own two sequential dataset builds (job1's, then job2's). Peak resident requirement: up to ~3× 2080 MiB. The on-device Cortex-A53 devbox used for hardware validation has only 1.8 GiB RAM total, and its *own* production safety check already refuses fast mode there entirely:

```
$ ./build/armrx --mode=fast --workers=1 --seconds=0
...
Available memory: 1538 MiB
Selected mode (1 workers): fast (requires 2338 MiB including reserve)
Requested fast mode does not fit in available memory.
```

`MiningEngine`'s constructor has no such check — only `MinerApp::run()` (via `choose_randomx_mode()`, `include/armrx/memory.hpp`) applies it before ever calling `set_job()`. The test bypassed that check by constructing `MiningEngine(RandomXMode::fast, ...)` directly, forcing an allocation the CLI itself would have refused on this hardware. Result: the test process was killed (almost certainly OOM, though the kernel log wasn't readable without root) partway through building the very first dataset — not a regression from this work, since fast mode had apparently never been exercised at full scale on this specific device before.

**Fix, two parts:**
1. Rewrote the verification strategy to avoid needing a third full dataset at all. RandomX "light mode" computes each dataset item on the fly via `generate_dataset_item()` (see `VirtualMachine::dataset_read()`, `src/vm.cpp:748-763`) — mathematically guaranteed to equal the materialized fast-mode value for the same seed. The test now cross-checks the engine's fast-mode hashes against a light-mode `VirtualMachine` sharing only the 256 MiB cache, which is a *stronger* check besides being cheaper: it validates the actual fast/light equivalence invariant fast mode exists to preserve, not just "was `initialize_dataset()` called with the right arguments."
2. Added a memory-availability guard (`fast_mode_fits_on_this_host()` in `tests/test_mining.cpp`, using the same `choose_randomx_mode()` the production CLI path uses) that skips — not fails — the two fast-mode tests when the host can't fit fast mode. Prints a `SKIPPED` line rather than silently doing nothing, so it's visible in test output rather than looking like reduced coverage nobody decided on.

### GCC 15 + musl + LTO also broke the `armrx` executable link, unrelated to any of the above

Splitting `main.cpp` into `cli_parser.cpp`/`miner_app.cpp` (PLAN.md §1.3) changed how LTO partitions the `armrx` executable's link step, and on-device this tripped the same general class of GCC 15 + musl + LTO fragility the project already has a documented escape hatch for (`ARMRX_DISABLE_LTO`, added for the Newton-Raphson `x29` corruption bug — see `docs/jit-buffer-size-audit.md`). The specific failure here was different (an `always_inline vsnprintf` inlining error inside `lto1`, not a register corruption), but the workaround was the same: `cmake -S . -B build -DARMRX_DISABLE_LTO=ON` builds and links `armrx` cleanly. This did not reproduce on the x86_64 dev sandbox (GCC there didn't hit the same LTO partitioning path) — only on-device with the Alpine/musl GCC 15.2.0 toolchain.

**Not fixed project-wide** — `ARMRX_DISABLE_LTO` is an existing opt-in CMake option, not a new default. On-device validation for this session used `-DARMRX_DISABLE_LTO=ON` explicitly; if the LTO link failure turns out to reproduce reliably (not just under this specific combination of new translation units + toolchain), it may be worth revisiting whether `ARMRX_DISABLE_LTO` should default ON for musl targets, but that's a separate decision from this fix.

## Files Changed

| File | Change |
|------|--------|
| `src/mining_engine.cpp` | Fixed both `initialize_dataset()` call sites to pass the correct sub-span |
| `tests/test_mining.cpp` | Fixed the new reference-dataset construction's identical bug (then replaced it with a light-mode-equivalence check that doesn't need a second full dataset); added `test_fast_mode_dataset_reinit_via_workers` and `test_stop_races_dataset_reinit`, both gated by `fast_mode_fits_on_this_host()` |
| `CMakeLists.txt` | Added `-UNDEBUG` to `armrx_tests`/`test_mining` targets; added a `TIMEOUT 900` ctest property to `test_mining` (it now does real dataset-building work) |

## Verification

- **x86_64 (interpreted, 31 GiB RAM):** `armrx_tests` pass with live assertions; `test_mining` passes and fully exercises both fast-mode tests (not skipped — fits easily), including 8 nonces cross-checked against the light-mode reference; full local `ctest` 3/3.
- **AArch64 (Cortex-A53, JIT, on-device, ~1.8 GiB RAM, built with `-DARMRX_DISABLE_LTO=ON`):** full `ctest` 6/6 passing — `armrx_tests` 15.5s, `test_mining` 17.6s (both fast-mode tests correctly `SKIPPED`, memory-guarded), `bench_armrx` 570.3s, `bench_opcodes` 354.0s, `test_jit_encodings` 88.9s, `test_jit_determinism` 18.2s.

## Lessons

1. **A test that "passes" isn't verification unless its assertions can actually fail.** `NDEBUG` silently stripping `assert()` in the default Release build meant the entire assert-based portion of the test suite provided no real protection — this should have been caught far earlier, and is worth checking for in any other C++ project that mixes `assert()` with a Release-by-default CMake setup.
2. **A utility function's contract should be verified against how its callers actually use it, not just against its own isolated test.** `initialize_dataset()`'s own tests were internally consistent and correct; the bug was entirely in a mismatch between its (correct, tested) relative-offset contract and callers that assumed absolute-offset semantics.
3. **Multi-threaded initialization bugs can hide in plain sight when thread 0's output happens to be correct by coincidence** (offset 0 == absolute position 0 when `start_item == 0`) — a single-threaded or `start_item == 0` test will never reveal a bug that only manifests for `start_item > 0`.
4. **A correctness test needs its own memory budget considered against real deployment targets, not just "does it fit on the dev sandbox."** This project's actual target hardware (low-RAM AArch64 SBCs) is exactly the case where a test naively assuming abundant RAM will misbehave — and where the miner's own mode-selection logic already encodes the right check to reuse.
5. **When a test needs a large reference computation to verify correctness, look for a cheaper mathematically-equivalent check before reaching for "just build a second copy of the real thing."** Light mode's on-the-fly item generation was already sitting in the codebase as an equivalence oracle for fast mode's materialized dataset — using it was both cheaper and a strictly more meaningful assertion.
