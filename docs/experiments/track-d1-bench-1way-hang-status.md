# Track D1 gate tool: bench_dataset_2way 1-way mode hang — night-shift status (2026-07-28/29)

**Status: open.** The D1 *implementation* is unaffected (ctest 16/16 green, incl. the
exhaustive 2-way differential test). What hangs is the *gate benchmark harness*'s
1-way baseline mode, which blocks running the L1I decision gate.

## Facts established (Hermes ssh probes + an agy/opus debug session that hit its
## print timeout before concluding; session transcript at /tmp/d1_bench_debug_result.log)

- `bench_dataset_2way 1way N` hangs (state R, full CPU, utime climbing — spinning,
  not blocked) even at N=10, under `taskset -c 3` with isolcpus on. >10 min, no exit.
- **`bench_dataset_2way 2way N` COMPLETES successfully** (EXIT 0, checksum printed)
  on the same device, same build. The new 2-way path works; the *1-way baseline*
  loop is what hangs.
- An -O2 debug rebuild of the same source's 1-way path **works**. The hanging binary
  is the CMake Release (-O3) build. agy was mid-way through an -O2 vs -O3
  miscompilation/UB comparison when its session timed out.
- `test_jit_dataset_2way` (ctest, passed in 193s) only exercises fn2way — it never
  calls `getCalcDatasetItemFunc()` the way the bench's 1-way mode does, so ctest
  green and bench hang are consistent.
- perf sampling of the hung process shows execution in main + JIT regions + RNG —
  consistent with the loop running with a wrong/looping callee, or a miscompiled
  loop, not a deadlock.

## Leading hypotheses (narrowed from the above)
1. **-O3 miscompilation or UB in the bench harness's 1-way loop** (works at -O2,
   hangs at -O3, same source). Candidate UB: none obvious in the 95-line file, but
   the fn1way pointer-call pattern + strict-aliasing at -O3 deserves a look.
2. **The bench's 1-way setup misuses the JitCompilerA64 API** in a way the test
   never does — e.g. `getCalcDatasetItemFunc()` on an instance that only ran
   `generateSuperscalarHash()`, if that function's contract expects more state
   initialized. (The mining path constructs this differently.)

## Next steps (for whichever agent resumes)
- Diff the -O2-works / -O3-hangs binaries' disassembly of main()'s loop (the agy
  transcript was in the middle of exactly this).
- Check how production code obtains and calls the single-stream entry
  (`getCalcDatasetItemFunc` usage in src/ vs the bench's usage).
- If it's harness UB/miscompile: fix the bench, rerun; the gate protocol itself is
  unchanged. If it's an API-contract misuse: fix the bench's setup to mirror
  production usage.
- Device left clean (no stray processes). isolcpus is ON — leave it on; the gate
  runs under it anyway, and the 2way-completes datapoint was collected under it.
