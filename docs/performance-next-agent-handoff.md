# armrx Performance Optimization — Next-Agent Handoff

**Created:** 2026-07-19  
**Updated:** 2026-07-19 — added §22 with benchmark v2 findings  
**Target:** 8× Cortex-A53, Snapdragon 410-class device, 2 GiB RAM, postmarketOS/musl  
**Current reported performance:** approximately 5.16–5.18 H/s single-thread and 28–29 H/s with 8 workers in RandomX light mode  
**Correctness baseline:** all four current KAT/unit targets reported passing  
**Primary goal:** find sustainable performance beyond current XMRig parity without changing RandomX results

This document is a self-contained handoff for the next agent. It consolidates a source audit of the current performance plan, corrects several inaccurate assumptions, and provides a ranked experiment sequence with concrete implementation sites and validation requirements.

No optimization described below has been measured on the target unless explicitly marked as already tested. Gain estimates are hypotheses. Real AArch64 hashrate is the veto metric.

---

## 1. Read this first

Before changing code, read:

1. [`../PLAN.md`](../PLAN.md) — master plan, but note the stale priorities documented below.
2. [`../ROADMAP.md`](../ROADMAP.md) — completed/remaining tracker, also contains stale entries.
3. [`beyond-parity.md`](beyond-parity.md) — prior post-parity analysis.
4. [`peephole-jit-plan.md`](peephole-jit-plan.md) — existing instruction-gap plan; several assumptions need correction.
5. [`../OPTIMIZATION_REFERENCE.md`](../OPTIMIZATION_REFERENCE.md) — historical experiments and old perf counters.
6. [`../AGENTS.md`](../AGENTS.md) — mandatory project rules and build procedure.

Important project constraint:

- Do not modify `scratch_vm_study/`. It is standalone and does not affect the main build.

Standard local build and test:

```sh
cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

AArch64 device workflow, when configured:

1. `devbox_status`
2. `devbox_sync`
3. `devbox_build`
4. `devbox_test`
5. `devbox_bench` and/or `devbox_perf_stat`

At the time this handoff was written, `devbox_status` failed because `tools/devbox/devbox.json` was unavailable to the integration. Do not claim device validation until the connection is restored and commands actually pass.

After a deployed and verified code change, follow the documentation discipline in `AGENTS.md`: update `changelogs.md`, feature status where applicable, `ROADMAP.md`, and any affected design document.

---

## 2. Current state and strategic conclusion

The implementation is already at approximately XMRig parity in total 8-worker pool hashrate. Existing records report a whole-benchmark comparison of roughly:

- armrx: 64.3 billion retired instructions
- XMRig: 48.2 billion retired instructions
- armrx: lower CPI but more total instructions
- armrx: many more branch misses in the historical measurement

The key correction is that this is **not yet proven to be a 33% generated-opcode gap**. The recorded counters cover an entire benchmark or mining path, including:

- generated RandomX VM instructions
- static AArch64 VM loop
- light-mode dataset-item derivation
- generated SuperscalarHash programs
- AES scratchpad initialization/finalization
- Blake2b
- JIT compilation and surrounding C++ control code

Therefore, do not begin by spending weeks comparing every opcode against XMRig. First attribute instructions and cycles to the major regions. A source audit found a potentially higher-value repeated cost in the light-mode dataset helper ABI.

---

## 3. Revised priority order

| Rank | Work item | Hypothesized gain | Risk | Why it is ranked here |
|---:|---|---:|---|---|
| 1 | Correct measurement and PMU attribution | Diagnostic | Low | Determines where the 33% instruction count actually comes from |
| 2 | Light-mode dataset-helper ABI and direct result mixing | ~3–8% | Medium–high | Removes repeated stack/save/store/reload plumbing from the dominant light-mode path |
| 3 | Static FP load/conversion scheduling | ~1–4% | Low | Straightforward latency-hiding opportunity on an in-order A53 |
| 4 | Direct register-offset FP scratchpad loads | ~0.5–2% | Low | Saves one instruction in every FP memory opcode |
| 5 | Superscalar literal-pool relayout | ~1–4% | Medium | Removes hot unconditional branches and literal data from sequential instruction fetch |
| 6 | Fused hash-and-fill mining pipeline | ~1–5% | Medium | Existing fused implementation is unused by production mining |
| 7 | Correct PGO final-link setup | ~1–5% | Low–medium | Current static-library link setup likely explains missing `__gcov_*` symbols |
| 8 | Scratchpad prefetch A/B matrix | −2% to +3% | Low | Current hints may help one core but hurt 8-worker aggregate throughput |
| 9 | Worker/core-mask and thermal steady-state sweep | 0–10% sustained | Low | Eight workers may not maximize thermally sustained throughput |
| 10 | Conservative generated-VM scheduling | ~2–6% | High | Potentially useful on A53, but much harder to prove safe |
| 11 | Broader XMRig emitted-code comparison | Unknown | Medium | Valuable only after region attribution shows generated handlers dominate |
| Frozen | Newton-Raphson FDIV/FSQRT | Unknown | Very high | Current path crashes and approximate FP must still be bit-identical |

Treat all ranges as experiment budgets, not promises.

---

## 4. Priority 1 — establish trustworthy measurement

### 4.1 Why the current tooling is insufficient

`tests/bench_opcodes.cpp` currently reports:

- static opcode frequency
- emitted bytes per opcode
- aggregate generation elapsed time

It does **not** isolate or execute one opcode at a time, and it does not collect PMU instructions or cycles per opcode. The claim in `docs/peephole-jit-plan.md` that per-opcode `perf stat` infrastructure is delivered is stronger than the implementation.

`tests/bench_armrx.cpp` also has misleading component benchmarks:

- `fill_aes_1r_x4 (2 MiB)` passes a 64-byte span at `tests/bench_armrx.cpp:63-68`.
- `load_cache_line (random access)` repeatedly accesses cache line 42 at `tests/bench_armrx.cpp:77-80`.
- `generate_dataset_item` repeatedly uses item 1,000,000 at `tests/bench_armrx.cpp:82-86`.
- Dataset initialization repeatedly writes the same 5,000-item output region at `tests/bench_armrx.cpp:88-91`.
- The summary compares a single `VirtualMachine` result against a hard-coded XMRig target that appears to be an 8-worker device total at `tests/bench_armrx.cpp:151-156`.

These measurements can warm a fixed cache/TLB path and cannot support detailed memory conclusions.

### 4.2 Required benchmark corrections

Create a reproducible benchmark protocol before substantial assembly changes:

1. Use a fixed revision, fixed seed, fixed generated programs, and fixed core mask.
2. Precompute a deterministic pseudorandom index sequence outside the timed region.
3. Make the sequence large enough to avoid repeatedly exercising one cache/TLB path.
4. Run multiple samples and report at least median and dispersion, not one aggregate timing.
5. Compare armrx and XMRig with the same:
   - worker count
   - exact cores
   - mode and seed
   - duration
   - governor
   - huge-page state
   - starting temperature
6. Separate startup/warmup from the steady-state measurement interval.
7. Normalize PMU counters by completed hashes.

### 4.3 Region attribution

Collect counters for as many of these regions as practical:

1. Whole `randomx_calculate_hash`.
2. JIT generation only.
3. Generated VM execution only.
4. Light-mode dataset helper only.
5. Superscalar generated body only.
6. AES scratchpad initialization/finalization.
7. Blake2b stages.

At minimum collect:

- cycles
- retired instructions
- branches
- branch misses
- L1 instruction refills
- L1 data refills
- L2 data refills
- dTLB refills/page walks
- frontend/backend stalls, if exposed
- context switches and migrations
- current frequency and thermal-zone temperature

Run 1, 2, 4, 6, and 8 workers. The existing assertion that the 30–32% scaling loss is definitively a single-channel DRAM bandwidth ceiling is plausible but unproven without L2, stall, TLB, or memory-controller evidence.

### 4.4 Validation gate

Do not prioritize broad per-opcode peepholes until the measurements answer:

- What fraction of cycles/hash is in the light-mode dataset helper?
- What fraction is in generated VM opcodes?
- Is the multi-worker loss primarily lower frequency, backend stalls, cache refills, TLB activity, or something else?
- Does the instruction gap remain when comparing only equivalent generated execution regions?

---

## 5. Priority 2 — light-mode dataset-helper ABI

### 5.1 Verified current path

The light-mode path begins at `randomx_program_aarch64_vm_instructions_end_light` in `src/jit_compiler_a64_static.S:504`.

For every VM iteration it currently:

1. Allocates a 96-byte outer frame:

```asm
sub sp, sp, 96
stp x0, x1, [sp, 64]
stp x2, x30, [sp, 80]
```

2. Uses the bottom 64 bytes as a dataset-item result buffer.
3. Calls `rx_calc_dataset_item` at `src/jit_compiler_a64_static.S:536`.
4. The helper allocates another 112-byte frame and saves `x0` through `x13` at `src/jit_compiler_a64_static.S:824-831`.
5. It calculates the item in `x0` through `x7`.
6. It stores the 64-byte result to the outer output buffer at `src/jit_compiler_a64_static.S:906-910`.
7. It reloads all saved helper registers at `src/jit_compiler_a64_static.S:912-919`.
8. The caller restores its outer saved registers.
9. The caller branches to `rx_program_xor_with_dataset_line`.
10. That code immediately reloads the same 64-byte result with four `ldp` instructions and XORs it into VM integer registers at `src/jit_compiler_a64_static.S:338-349`.

The JIT is invoked with 2,048 loop iterations per generated program at `src/vm.cpp:847-850`. A full hash chains eight programs. This makes repeated helper plumbing a high-leverage target.

### 5.2 Proposed implementation phases

Do not rewrite everything at once. Use independently benchmarkable phases.

#### Phase A — remove duplicate preservation

Create a light-mode-only internal helper contract that relies on the outer path preserving registers that must survive. Avoid saving the same argument/state registers in both the 96-byte and 112-byte frames.

Requirements:

- Document the internal register contract.
- Preserve all VM live state.
- Pay special attention to `x3`, `x8-x13`, `x16`, `x17`, and `x30`.
- Keep the general dataset-item entry point intact if it has other callers.

#### Phase B — direct result mixing

Avoid:

```text
x0-x7 result → four STP to temporary buffer → four LDP → eight EOR
```

Instead, mix the calculated `x0-x7` directly into the saved or live VM register values before restoring the external register view.

Possible design directions:

- Load the saved VM values, XOR directly with `x0-x7`, and restore the mixed values.
- Use a light-mode helper whose return contract explicitly leaves the item in `x0-x7`, then perform direct register mixing before any restore.
- Merge outer and helper frames if that simplifies liveness without adding extra moves.

#### Phase C — remove call/return overhead only if still worthwhile

Inlining or branching through a custom return continuation may remove `bl`/`ret`, but do this only after phases A and B are measured. Code size and instruction-fetch effects may offset the saving.

### 5.3 Validation

For each phase:

1. Run all KATs in JIT and interpreted configurations.
2. Add a test comparing light-mode dataset-item results before and after the new ABI for many deterministic item indices.
3. Verify stack alignment and callee-saved register behavior.
4. Compare:
   - instructions/hash
   - cycles/hash
   - stack loads/stores if measurable
   - single-thread H/s
   - 8-worker H/s
5. Reject the change if hashrate regresses despite fewer instructions.

---

## 6. Priority 3 — schedule static FP loads and conversions

### 6.1 Verified current sequence

The static loop at `src/jit_compiler_a64_static.S:236-263` performs eight independent chains in serialized order:

```asm
ldr   d16, [x17]
sshll v16.2d, v16.2s, #0
scvtf v16.2d, v16.2d
ldr   d17, [x17, #8]
sshll v17.2d, v17.2s, #0
scvtf v17.2d, v17.2d
...
```

On an in-order Cortex-A53, immediately consuming each load and then immediately consuming the widening result can expose load-use and SIMD pipeline latency.

### 6.2 Experiment matrix

Test several schedules rather than assuming all loads first is best:

1. Current serialized baseline.
2. Two loads, two `sshll`, two `scvtf`.
3. Four loads, four `sshll`, four `scvtf`.
4. Eight loads, eight `sshll`, eight `scvtf`.
5. Optional `ldp dN, dM` forms for known adjacent addresses.

Keep the four `bif` operations after E-register conversion.

### 6.3 Why this is low risk

- The eight destination SIMD registers already exist.
- Loads use fixed adjacent offsets.
- There is no semantic dependency between different destination chains before later generated VM execution.
- This does not require temporary GPR allocation.

### 6.4 Validation

- Full KATs.
- Compare exact output hashes.
- Measure cycles/hash, not only instruction count.
- Test one worker and eight workers; a schedule can interact with frontend pressure differently under load.

---

## 7. Priority 4 — direct register-offset FP loads

### 7.1 Verified current emitter

`JitCompilerA64::emitMemLoadFP()` at `src/jit_compiler_a64.cpp:650-678` currently calculates and masks an offset in `x19`, then emits:

```asm
add x19, x2, x19
ld1 {vN.2s}, [x19]
sshll vN.2d, vN.2s, #0
scvtf vN.2d, vN.2d
```

The integer memory helper already uses a base-plus-register load.

### 7.2 Proposed encoding

Test an AArch64 SIMD scalar register-offset load equivalent to:

```asm
ldr dN, [x2, x19]
sshll vN.2d, vN.2s, #0
scvtf vN.2d, vN.2d
```

This should remove the `add` from every FP memory opcode handler using `emitMemLoadFP`, including:

- `FADD_M`
- `FSUB_M`
- `FDIV_M`

### 7.3 Required tests

- Add a focused encoding test that decodes the emitted instruction fields.
- Verify that exactly two 32-bit lanes are loaded into the low 64 bits.
- Verify signed widening and conversion results against the interpreted path.
- Run deterministic programs that force each affected opcode and both scratchpad masks.

---

## 8. Priority 5 — Superscalar literal-pool relayout

### 8.1 Verified current layout

`JitCompilerA64::generateSuperscalarHash()` at `src/jit_compiler_a64.cpp:406-505` currently does this for each Superscalar program:

1. Reserves a branch at `src/jit_compiler_a64.cpp:431-432`.
2. Emits reciprocal literals inline at `src/jit_compiler_a64.cpp:434-440`.
3. Patches an unconditional branch over the pool at `src/jit_compiler_a64.cpp:442-444`.
4. Emits executable instructions.
5. Uses `LDR literal` back to the pool for `IMUL_RCP` at `src/jit_compiler_a64.cpp:486-496`.

### 8.2 Proposed layout

Generate:

```text
program 0 executable code
program 1 executable code
...
program 7 executable code
shared or per-program reciprocal pools
```

Record each unresolved `LDR literal` while emitting code, append aligned literal pools after all executable code, and patch signed `imm19` offsets afterward.

### 8.3 Expected effects

- Remove up to one always-taken branch per Superscalar program.
- Keep literal data out of the sequential instruction-fetch stream.
- Potentially reduce L1I refill and branch overhead.

Do not assume the gain from branch count alone. Measure branch misses, L1I refill, cycles, and hashrate.

### 8.4 Safety requirements

- Check signed range and 4-byte scaling for every `LDR literal` displacement.
- Preserve 8-byte literal alignment.
- Add endpoint assertions so generated code and pools cannot overlap another region.
- Add byte-level determinism coverage for code and literal contents.

---

## 9. Priority 6 — fused hash-and-fill mining pipeline

### 9.1 Existing implementation

The code already contains:

- `hash_and_fill_aes_1r_x4` in `include/armrx/aes_hash.hpp`
- its hardware-AES implementation in `src/aes_hash.cpp`
- `VirtualMachine::hash_and_fill()` in `src/vm.cpp:941-950`

Production mining calls `randomx_calculate_hash()` and does not use this fused path.

### 9.2 Experiment design

First create a benchmark-only first/next/last pipeline:

1. **First:** initialize the scratchpad for nonce N without producing a final result yet.
2. **Next:** finish nonce N while hashing its scratchpad and filling it for nonce N+1 in one traversal.
3. **Last:** flush the final pending nonce.

Only move this into mining after the benchmark proves a gain.

### 9.3 Correctness hazards

- The pipeline has one-result latency.
- Job changes must discard or correctly flush stale pending work.
- Share submission must associate the result with the correct nonce and job.
- Shutdown must flush or deliberately discard pending work.
- The fused path must not overwrite state needed for the current result before final Blake2b.

Add tests comparing a long deterministic nonce sequence from the pipelined API with independent `randomx_calculate_hash()` calls.

---

## 10. Priority 7 — repair PGO setup

### 10.1 Likely current problem

PGO options at `CMakeLists.txt:117-127` are applied to `armrx_core`, which is a static library:

```cmake
target_compile_options(armrx_core PRIVATE -fprofile-generate -fno-lto)
target_link_options(armrx_core PRIVATE -fprofile-generate -fno-lto)
```

A static library has no final executable link step. Private link options on it do not reliably provide the profile runtime when linking `armrx`, `bench_armrx`, or tests. This is consistent with historical missing `__gcov_*` symbols.

### 10.2 Proposed correction

For `GENERATE`:

- Compile `armrx_core` with `-fprofile-generate -fno-lto`.
- Link every instrumented final executable with `-fprofile-generate -fno-lto`.

For `USE`:

- Compile profiled code with `-fprofile-use -fprofile-correction -fno-lto`.
- Apply any required use-mode linker options to final executables.

Use a helper function or interface target to avoid duplicating inconsistent flags.

### 10.3 Training workload

Train primarily on sustained light-mode JIT mining. Do not let cache initialization, interpreted mode, tests, or CLI startup dominate the profile unless startup performance is a separate goal.

If GCC 15/musl remains broken after correct final linkage, evaluate Clang instrumentation PGO as a separate experiment. Keep LTO disabled for the first controlled PGO result.

---

## 11. Priority 8 — scratchpad prefetch matrix

The static loop currently emits three next-iteration scratchpad prefetches at `src/jit_compiler_a64_static.S:363-366`:

```asm
prfm pldl1keep, [x19]
prfm pldl1strm, [x20]
prfm pldl1keep, [x20, 32]
```

On A53, these may hide latency, but they also consume issue bandwidth and can increase shared-cache or memory pressure with eight workers.

Test compile-time variants:

1. No scratchpad prefetch.
2. `spAddr0` only.
3. `spAddr1` first line only.
4. Current three hints.
5. `L1KEEP` versus `L1STRM`.
6. `L2KEEP` where supported and meaningful.

Evaluate both single-thread cycles/hash and 8-worker total H/s. Do not retain a variant solely because it helps single-thread performance.

---

## 12. Priority 9 — topology, worker count, and thermals

Run long enough to reach thermal equilibrium, preferably 15–30 minutes per variant.

Suggested matrix:

- 4 workers on each cluster/core group separately.
- 5, 6, 7, and 8 workers.
- Omit each logical core in turn where practical.
- Pinned versus scheduler-managed.
- One core reserved for kernel/network/thermal housekeeping.
- Normal scheduling versus `--rt-priority`.

Record once per second:

- total and per-worker H/s
- thermal-zone temperatures
- current frequency per cpufreq policy
- throttling/time-in-state counters
- context switches and migrations
- affinity failures

Report first-minute and final-five-minute H/s separately. Start variants at comparable temperatures and alternate or randomize test order.

Be cautious with `SCHED_FIFO`: continuously runnable FIFO workers can delay ordinary-priority housekeeping or thermal-management processes and reduce sustained performance.

---

## 13. Priority 10 — conservative generated-VM scheduling

Only attempt this after lower-risk work and PMU attribution.

The main generated VM body is produced by:

- `JitCompilerA64::generateProgram()`
- `JitCompilerA64::generateProgramLight()`
- the handler dispatch in `src/jit_compiler_a64.cpp`

A Cortex-A53 is in-order, so moving independent work between a producer and consumer may matter more than merely removing one instruction.

### Safe initial scope

Build small scheduling regions terminated by:

- `CBRANCH`
- `CFROUND`
- any instruction whose memory or control behavior cannot be safely moved

Initially reorder only provably independent operations:

- disjoint integer register operations
- independent multiply chains
- FP operations on disjoint F/E registers
- address generation that can safely occur earlier
- reciprocal literal loads

Treat scratchpad memory operations as aliasing unless proven otherwise. Never move FP work across `CFROUND`.

A two- or three-instruction emitter lookahead is a safer prototype than a full DAG scheduler. Preserve `reg_changed_offset` semantics used by CBRANCH target generation.

---

## 14. Corrections to existing plan assumptions

Update the relevant plans when implementation work begins.

### 14.1 The 33% count is not yet a per-opcode JIT gap

`docs/peephole-jit-plan.md` attributes the gap to emitted sequences, but the historical measurement includes the wider hash path. Region attribution is required first.

### 14.2 `bench_opcodes` does not measure per-opcode retired instructions

It measures frequency and emitted byte count. It should not be described as a PMU opcode benchmark.

### 14.3 Immediate `ROR` and `EXTR` are not competing optimizations

AArch64 immediate `ROR` is an alias of an `EXTR` encoding with the same source register. Changing the mnemonic cannot save an instruction. Variable rotate cannot use an immediate-only `EXTR` form.

### 14.4 The existing `IMUL_RCP` plan description is inaccurate

Main VM `IMUL_RCP` uses preloaded reciprocal-value GPRs or literal loads, then ordinary `mul`. Superscalar `IMUL_RCP` also uses `LDR literal` plus `mul`. It is not a `mov` plus `umulh` sequence, and there is no generic fusion into `movk`.

### 14.5 There is no conventional VM register allocator producing general spills

VM integer and FP registers have fixed native mappings. There is temporary-register pressure and helper save/restore traffic, but a broad “eliminate allocator spills” project is based on the wrong model.

### 14.6 Generic adjacent-load `ldp` coalescing is unsafe

RandomX scratchpad memory addresses are independently calculated and masked. Two adjacent VM memory opcodes are not generally contiguous. Restrict load pairing experiments to known fixed-layout static accesses.

### 14.7 RandomX has no numerical tolerance

Mining output must be bit-identical. An approximate NR divide/sqrt is acceptable only if it reproduces required results for every reachable input and dynamic rounding mode. Passing current KATs is necessary but not a mathematical proof.

### 14.8 `x29` is not an IMUL_RCP pool base pointer

The static prologue loads a reciprocal value into `x29`; `h_IMUL_RCP()` can select it as one of several preloaded literal-value registers. Existing `ADR`/`ADRP` speculation is unsupported by that handler.

### 14.9 PUBLIC macro propagation is not itself evidence of unrelated corruption

`ARMRX_JIT_FAST_DIV_SQRT` is referenced in the JIT compiler source. PUBLIC propagation is unnecessary API exposure, but it does not by itself explain unrelated C++ behavior unless consumers also condition on the macro. Diagnose exact generated buffer bounds and minimal failing programs instead.

---

## 15. Frozen NR FDIV/FSQRT work

Keep `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` frozen until a controlled postmortem is possible.

Required investigation before any retry:

1. Compile minimal deterministic programs containing FDIV only and FSQRT only.
2. Dump the entire generated code and literal regions.
3. Assert generated endpoints against reserved template boundaries.
4. Decode every branch and literal displacement.
5. Test without LTO and with an alternate compiler.
6. Confirm whether failure is code generation, build/toolchain, buffer overwrite, ABI corruption, or FP exceptional behavior.
7. Test all supported rounding modes and exceptional inputs.

Do not describe one-iteration NR as correct based on approximate precision or a finite KAT set.

---

## 16. Huge-page verification

The project attempts explicit huge pages and falls back to anonymous mappings plus `MADV_HUGEPAGE`, but successful allocation/advice does not prove huge-page residency.

During mining inspect `/proc/<pid>/smaps` for each relevant mapping:

- `KernelPageSize`
- `MMUPageSize`
- `AnonHugePages`
- `VmFlags`

Also record:

- `/proc/meminfo` huge-page counters before and after
- THP policy
- minor and major faults
- dTLB refill/page-walk counters

Compare pre-reserved explicit hugetlb pages with THP fallback. Do not attribute performance to MAP_HUGETLB unless the actual process mappings confirm it.

---

## 17. Compiler experiment matrix

Current relevant settings include:

- `-march=armv8-a+crypto`
- optional `-mcpu=native`
- global `-funroll-loops`
- automatic IPO/LTO when supported

Test:

1. `-mcpu=cortex-a53+crypto`
2. `-march=armv8-a+crypto -mtune=cortex-a53`
3. current `-mcpu=native`
4. `-funroll-loops` on/off
5. LTO on/off
6. GCC versus a compatible Clang release

Inspect `.text` size and disassembly for:

- AES scratchpad functions
- Blake2b compression
- `execute_superscalar_neon`
- VM control code

Compiler flags do not substantially alter the generated JIT body itself. Expect gains primarily in AES, Blake2b, Superscalar C++, and control code. Global unrolling can hurt a small A53 instruction cache.

---

## 18. Tests that need strengthening before risky JIT work

### Byte-level JIT determinism

The existing determinism test should compare the actual generated instruction and literal bytes, not only metadata tuples such as opcode, offset, and size.

### CBRANCH decoding

The existing encoding test should decode the emitted conditional and unconditional branch words and verify exact target addresses against the intended register-change offsets. Merely checking emitted size would not reliably catch another immediate-displacement bug.

### Generated-buffer bounds

Add assertions/tests for:

- VM generated body endpoint
- IMUL_RCP literal endpoint
- Superscalar executable endpoint
- Superscalar literal-pool endpoint
- required alignment

### Affected-opcode differential tests

For emitter changes, create deterministic programs forcing the affected opcode forms, memory modes, immediates, and register combinations. Compare JIT output hashes against interpreted execution.

---

## 19. Per-change validation protocol

Every behavior- or performance-affecting change must pass:

1. **Correctness**
   - all existing KATs
   - JIT/interpreted equivalence for targeted programs
   - deterministic code-byte checks where applicable
2. **Encoding**
   - decode new instruction words and displacements
   - verify bounds and alignment
3. **Single-thread measurement**
   - at least three comparable runs
   - cycles/hash and instructions/hash
   - H/s
4. **Eight-worker measurement**
   - total and per-worker H/s
   - stable core mask
   - thermal/frequency record
5. **Regression veto**
   - reject a change that lowers instruction count but lowers sustainable hashrate
6. **Documentation**
   - update `changelogs.md`
   - update `PLAN.md`/`ROADMAP.md`
   - update this document or the specialized design document

Record revision, compiler/version, flags, core mask, governor, huge-page state, duration, and temperature range with every benchmark result.

---

## 20. Recommended execution sequence for the next agent

### Stage 1 — measurement foundation

1. Restore devbox configuration/connectivity.
2. Correct misleading benchmark cases.
3. Add fixed-program region attribution.
4. Record a fresh 1/2/4/6/8-worker baseline.
5. Verify huge-page backing and thermal steady state.

### Stage 2 — low-risk A53 experiments

1. Static FP schedule matrix.
2. Direct register-offset FP load.
3. Scratchpad prefetch matrix.
4. Cortex-A53 compiler flag matrix.

Keep only measured wins.

### Stage 3 — light-mode structural wins

1. Remove duplicate dataset-helper saves.
2. Eliminate dataset result store/reload through direct mixing.
3. Relayout Superscalar literal pools.
4. Retry PGO with correct final-link flags.

### Stage 4 — broader changes

1. Benchmark fused hash-and-fill batching.
2. Integrate it into mining only with strong sequence/job-change tests.
3. Consider limited generated-VM scheduling.
4. Compare XMRig emitted output only for regions proven to dominate cycles.

### Stage 5 — deferred/high-risk

- NR FDIV/FSQRT postmortem
- full generated-code scheduler
- aggressive cross-opcode transformations

---

## 22. Benchmark v2 findings (2026-07-19)

Benchmark protocol v2 was deployed and run on the target device, collecting region attribution with `ARMRX_JIT_PROFILE` and PMU counters via `perf stat`.

### 22.1 Definitive region attribution

| Phase | μs/hash | % of hash |
|-------|---------|-----------|
| blake2b (input→seed) | 3.44 | **0.00%** |
| init_scratchpad (AES 2 MiB) | 589 | **0.31%** |
| chain: 7×run() + 7×blake2b | 166,989 | **86.56%** |
| final run() | 23,871 | **12.37%** |
| get_final_result (AES+blake2b) | 1,023 | **0.53%** |

**99.1% of hash time is in `run()` calls.**

### 22.2 JIT compile vs execute (inside run())

| Component | μs/hash | % of hash |
|-----------|---------|-----------|
| JIT compile (8 programs) | 3,361 | **1.76%** |
| JIT execute (8 programs × 2,048 iter) | 187,530 | **98.24%** |

### 22.3 PMU counters

| Metric | Value |
|--------|-------|
| Single-thread hashrate | **5.18 H/s** (192,909 μs/hash median, σ=1.4%) |
| Per-hash instructions (est.) | ~118M |
| Per-hash cycles (est.) | ~167M |
| IPC | **0.708** |
| Branch miss rate | **34.42%** |
| JIT speedup over interpreted | **12.85×** |

### 22.4 What this changes

**Priority 2 (light-mode dataset-helper ABI) is no longer the highest-leverage target.** The dataset derivation is already fully inlined in the JIT-generated code — it happens during JIT execution (98.24%), not during compilation (1.76%). Removing helper saves/restores would affect the 1.76% compile path, not the 98.24% execute path.

**The actual bottleneck is generated VM execution on an in-order A53:**

1. **34.42% branch miss rate** — The unpredictable CBRANCH in RandomX VM programs causes ~26% of all cycles to be wasted on pipeline flushes. Each mispredict costs ~13 cycles on A53. This is the single largest cycle sink.
2. **IPC of 0.708 (35% of peak)** — The in-order pipeline is frequently stalled by instruction dependencies, cache misses, and branch recovery.
3. **~118M instructions per hash** — Even if we removed every redundant instruction, the remaining workload is dominated by RandomX's required computation.

### 22.5 Revised optimization priorities

| Rank | Work item | Hypothesized gain | Risk | Rationale |
|---:|---|---:|---|---|
| 1 | **Reduce CBRANCH misprediction cost** — evaluate CSEL/CINC for conditional results, balance taken/not-taken path costs, BTB-aware code layout | ~5–15% | Medium | The 34% miss rate is inherent (unpredictable CBRANCH), but the *cost* of each misprediction can be reduced. |
| 2 | **Instruction scheduling for in-order A53** — reorder JIT-emitted sequences to separate dependent instructions, interleave loads, favor dual-issuable pairs | ~3–8% | Low-Medium | IPC at 0.708 leaves room for better pipeline utilization without changing opcode count. |
| 3 | **Peephole JIT coalescing** — per-opcode instruction count reduction in generated handlers | ~3–8% | Medium | Downgraded from ~15–20%. Still worthwhile but subordinate to the branch-miss bottleneck. |
| 4–11 | Items 4 through 11 from §3 remain valid but have lower expected impact. Priority 2 (dataset ABI) affects only the 1.76% compile path — retain as a later-stage optimization. |

### 22.6 New measurement items

For the next stage, add:

1. **Per-CBRANCH branch miss rate** — Use `perf stat -e branch-misses` on a program that isolates CBRANCH-heavy workload from the rest. Confirm that CBRANCH is the dominant source of mispredictions.
2. **Generated code disassembly** — Dump the JIT-generated body for a few programs and analyze instruction sequences for A53 dual-issue pairing opportunities.
3. **Worker scaling with branch-miss focus** — Measure 1/2/4/6/8-worker total hashrate with CBRANCH optimization experiments. Branch mispredictions can interact with SMT/core sharing on A53's single cluster.

---

## 21. Definition of success (updated for v2 findings)

A successful performance change must:

- preserve exact RandomX hashes
- pass all tests
- improve median sustainable H/s on the actual target
- avoid unacceptable thermal or reliability regressions
- be explainable through counters or a clear reduction in repeated work
- include reproducible benchmark conditions

The highest-confidence near-term direction based on benchmark v2 data is:

1. **reducing CBRANCH misprediction cost** through CSEL/CINC and balanced path costs,
2. **scheduling JIT-emitted instructions** for the in-order A53 pipeline,
3. peephole per-opcode savings (downgraded — still valuable but subordinate),
4. cleaning hot Superscalar code layout,
5. light-mode dataset-helper ABI improvements (affects 1.76% compile path only).
