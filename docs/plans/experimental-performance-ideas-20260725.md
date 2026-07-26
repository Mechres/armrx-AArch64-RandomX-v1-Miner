# armrx — Experimental Performance Ideas (2026-07-25)

**Status:** speculative, unmeasured, not scheduled. This is an idea file — nothing here is
gated, planned, or promised. If someone wants to pick up performance work after
`docs/plans/performance-plan-20260725.md`'s gated steps are exhausted, start here.

**Update 2026-07-26: worked through this list directly** (implement-and-measure, not gate-on-
diagnostic-alone — see `[[feedback_perf_work_pace]]` memory). Outcome per item, in the tables and
sections below: **#6+#12 adopted** (compiler flags, real small win, +0.298% IPC avg over two
on-device `perf stat` samples), **#9 closed** (scratchpad already 2 MiB-aligned, far beyond the
64-byte requirement), **#8 closed** (I-cache miss rate 0.788%, below the doc's own ~1% threshold),
**#4 done** (AES `hash_aes_1r_x4`+`fill_aes_1r_x4` is the single biggest named-C++ cost at ~12.3%
of all cycles, bigger than Argon2 or NEON permute combined — both known optimization avenues
already tried and failed, so not newly actionable, but now precisely quantified), **#5 closed**
(BLAKE2b doesn't register in the profile at all), **#10 adopted** (correctness-verified, latency-
only so not independently quantified — see reasoning in the item below), **#11 adopted** (measured
noise-level as the doc itself predicted, kept anyway — zero risk), **#1 tried, reverted** (real
JIT/interpreter divergence, see `docs/experiments/superscalar-imul-rcp-preassignment-attempt.md`).

**Sources:** brainstormed after the main-VM-program ~2.2× IPC lead was identified,
superscalar region was characterized (72.71% of instructions, 1.145× IPC), and the C++
overhead slice was measured (15.56% of instructions, 1.107× IPC). These ideas are the
*complement* of the main plan — they target regions OTHER than the main VM program's stall
signature, or propose changes orthogonal to the existing scheduling/prefetch approaches.

---

## Superscalar / dataset-derivation region (72.71% of instructions)

### 1. Superscalar IMUL_RCP literal register pre-assignment — TRIED, REVERTED (2026-07-26)

**Real JIT/interpreter divergence, `test_jit_equivalence` failed on its first case.** The doc's
own register-availability claim below (x9..x15 available) was also wrong — x9 holds the output
pointer, live across the whole call. Corrected set (x14,x15,x19-x28) was implemented and still
failed. Mechanism not identified; fully reverted rather than ship an undemonstrated fix. Full
account: `docs/experiments/superscalar-imul-rcp-preassignment-attempt.md`.

**The idea:** the superscalar path's `IMUL_RCP` emits `LDR_LITERAL x12, [pool]` + `MUL dst, dst, x12` (two instructions). The main VM program pre-assigns physical registers (x30, x29, ..., x11, x0) for its first 12 IMUL_RCP literals, avoiding LDR_LITERAL entirely for those — a single `MUL dst, dst, xN`. Do the same for the superscalar path.

**Why it might work:** the superscalar path uses x0..x7 for VM registers, leaving x9..x15 and x19..x28 as available literal registers. Even with the scheduler reordering IMUL_RCP relative to other opcodes, the scheduler's existing IMUL_RCP exclusion preserves relative IMUL_RCP order, so register assignment can be deterministic given the schedule.

**Caveats:** the pre-pass populates the literal pool in original order; register pre-loading needs to happen in emission order (post-scheduling). This means either: (a) schedule first, then pre-load, then emit (two-pass), or (b) use a register-indexed LDR with a base register instead of PC-relative LDR_LITERAL. Either approach needs a code restructure but no new hazard model.

**Correctness risk:** low. Existing IMUL_RCP exclusion handles ordering. Register assignment is deterministic once the schedule is known.

**Expected payoff:** saves one instruction per IMUL_RCP in the superscalar path, which dominates instruction volume. Each saved LDR_LITERAL is one fewer issue slot. Whether this translates to saved cycles depends on whether the in-order pipeline was stalling on the LDR latency anyway — worth measuring on exactly one program before committing.

---

### 2. Superscalar IXOR_C* immediate materialization

**The idea:** `IXOR_C7`/`C8`/`C9` handlers do `emitMovImmediate(x12, imm)` (MOVZ+MOVK, 2 instructions) + `EOR dst, dst, x12` (1 instruction) = 3 instructions total. If the immediate fits in AArch64's replicated-bit encoding, use single-instruction `EOR dst, dst, #imm` instead. If not, pre-load into registers during a pre-pass (same pattern as idea #1).

**Expected payoff:** smaller than #1 (IXOR_C* frequency is lower than IMUL_RCP). Quick measurement → adopt or discard.

---

### 3. Superscalar emission: eliminate the per-IMUL_RCP LDR_LITERAL entirely via a base+offset pool

**The idea:** instead of PC-relative `LDR_LITERAL` for each IMUL_RCP (which requires computing a signed offset, masking to 19 bits, and emitting LDR_LITERAL + MUL), load the pool base address into a register once (per superscalar program, or even once per `generateSuperscalarHash` call), then use `LDR x12, [base, #offset]` with a pre-computed offset in emission order. Same instruction count as the current approach (LDR base+offset is still one instruction), but the offset is a load-time immediate rather than a compile-time-relative one — might enable easier pre-assignment or wider encoding flexibility.

**Why it's different from #1:** #1 eliminates the load entirely by pre-loading into registers. This idea keeps the load but makes it a base+immediate form, which might be easier to integrate with the two-pass approach (pre-pass stores literals into pool at known emission-order offsets, main pass does base+offset LDR).

**Expected payoff:** maybe zero. The current approach already works. Only worth it if #1 hits an implementation wall and this intermediate step helps.

---

## C++ overhead region (15.56% of instructions, 1.107× IPC)

### 4. Profile the C++ slice to find the dominant function — DONE (2026-07-26)

`perf report --sort=overhead,dso` on-device (`bench_armrx --full-hash-only`): 81.07% of cycles in
JIT-generated code, 18.24% in named C++ (`bench_armrx` binary), 0.69% elsewhere. Within the C++
slice: `hash_aes_1r_x4` 6.21%, `fill_aes_1r_x4` 6.08%, `permute_16_neon` 3.23%,
`Argon2dCache::initialize` 1.59%, `permute_block_neon` 0.34%, `VirtualMachine::run` 0.31%. AES
scratchpad fill/hash is the single biggest named-C++ cost at ~12.3% of *all* cycles — bigger than
Argon2 or the NEON permute functions combined. Both known optimization avenues for it were already
tried and failed (NEON hardware AES — spec-incompatible; NEON vector-permute AES — measured
−19.4%; fused hash+fill — measured 3.6% slower), so not newly actionable, but now precisely
quantified rather than estimated.

**The idea:** `tools/jit_correlate.py` already attributes samples to named C++ functions. Run a `perf record -e cycles` session, feed it through the correlator, and inspect which function(s) dominate the 15.56% slice. Candidates: `compile_program()` (256-bytecode linear scan), `execute_superscalar()` (the interpreted superscalar path, called once per dataset item), `generateSuperscalarHash()` (JIT compilation for the superscalar path), AES generators (`fillAes1Rx4`/`hashAes1Rx4`), BLAKE2b finalization.

**Why this is step zero:** before optimizing anything in the C++ region, find out what's actually there. Could be one function at 8% or ten functions at 1.5% each — different strategies apply.

**Effort:** 15 minutes. One `perf record`, one `jit_correlate.py` run, read the table.

---

### 5. BLAKE2b NEON vectorization — CLOSED (2026-07-26)

Per idea #4's profile, BLAKE2b doesn't register in the top-cycle-consumer list at all (below the
noise floor) — exactly the condition this idea's own caveat said would close it. Not pursued.

**The idea:** BLAKE2b's G-function operates on a 4×4 state matrix of uint64 values, doing two columns per round. The G-function does additions + rotations on 64-bit values. On AArch64 NEON, 2× 64-bit operations can be packed into one 128-bit NEON register — the G-function is essentially 2-wide SIMD already in its design (it processes two independent columns). NEON would let you do 2 columns at once with `add.2d` + `shl.2d`/`usra.2d` (rotate via shift-left + shift-right-and-insert).

**Caveats:** BLAKE2b might be a negligible fraction of the 15.56% slice (see idea #4). Only worth pursuing if `perf` shows BLAKE2b as a significant cost. Also: BLAKE2b is consensus-critical — any NEON implementation must produce byte-identical output to the current scalar implementation. The existing `test_blake2b.cpp` KATs would catch any divergence immediately.

**Expected payoff:** unknown until profiled. BLAKE2b runs 8 times per hash (7 chain + 1 final). If it's even 2% of hash time, a 2× speedup on BLAKE2b is worth ~1%.

---

### 6. `-fno-semantic-interposition` / `-fvisibility=hidden` — ADOPTED (2026-07-26)

Added to `armrx_core`'s compile options in `CMakeLists.txt`, alongside idea #12 (they were
measured together). Two on-device `perf stat -e cycles,instructions` samples of
`bench_armrx --full-hash-only`, `taskset -c 0`, vs. the pre-flags baseline (cycles 97,907,028,597 /
instructions 73,741,319,475, IPC 0.7532): run 1 cycles −0.403%, run 2 −0.198%, average −0.301% /
IPC +0.298%. Small but consistent in direction across both runs, zero correctness risk (pure
compiler flags, full test suite unaffected). Kept.

**The idea:** add `-fvisibility=hidden` and `-fno-semantic-interposition` to `armrx_core`'s compile options. On PIE builds (default on many Linux distros), this lets the compiler inline across translation units more aggressively and eliminates PLT indirection for intra-library calls. Standard optimization for performance-sensitive shared libraries and static archives.

**Effort:** 2 lines in `CMakeLists.txt`. Measure before/after hashrate on the devbox.

**Expected payoff:** small (1-3% depending on call density). Zero correctness risk. Pure compiler-flag change.

**Already checked?** Confirmed by direct grep of `CMakeLists.txt` (2026-07-25) — neither flag is set anywhere in the build. Genuinely untried.

---

## Orthogonal / cross-cutting

### 7. Double-buffered JIT compilation (overlap compile with execute)

**The idea:** JIT compile time is measured at ~1.76% of hash time. Each `run()` call inside `randomx_calculate_hash` is: AES entropy → generate program → compile JIT → execute. These are sequential. But run N+1's program entropy is known as soon as run N's hash completes — start compiling run N+1's JIT code while run N executes, using a second code buffer.

**Maximum gain:** ~1.76% if fully overlapped — this is an *estimated upper bound derived from the compile-time-share measurement*, not a measured result of double-buffering itself (no double-buffering has been implemented or benchmarked). Treat it as a ceiling to decide if the idea is worth prototyping, not a promised number. Same order of magnitude as the scheduler win (+0.233% IPC). Clean mechanism, no hazard model, no scheduler interaction.

**Complexity:** needs `VirtualMachine` to own or coordinate with a second `JitCompilerA64` buffer. The `randomx_calculate_hash` outer loop restructures to pipeline compile/execute.

**Caveat:** the gain is bounded and small. For 1.76%, the complexity may not justify it. But if combined with other small wins (ideas #5, #6, #1), the cumulative effect crosses a psychologically meaningful threshold.

---

### 8. Instruction cache pressure measurement — CLOSED (2026-07-26)

`perf stat -e l1i_cache,l1i_cache_refill` on-device during `bench_armrx --full-hash-only`:
298,174,032 refills / 37,833,635,673 accesses = **0.788% miss rate**, below the doc's own ~1%
threshold ("if it's low, close this lead in 5 minutes"). Not pursued further.

**The idea:** the Cortex-A53 has a 16 KiB L1 I-cache. The static JIT template is `CodeSize` bytes. The JIT-filled VM instructions slot is `RANDOMX_PROGRAM_MAX_SIZE × 32 × 4` bytes. The superscalar compiled code is at `CodeSize`+ offset, up to `CalcDatasetItemSize` bytes. If the superscalar code doesn't fit in I-cache alongside the main VM program region, there's thrashing every time the main loop transitions between them (2048 times per hash × 8 superscalar programs per iteration = 16,384 transitions).

**What to measure:** `perf stat -e L1-icache-load-misses` during a real mining run. If I-cache miss rate is above ~1%, there's room. (The main loop's code size is known; the superscalar code size is variable per program.)

**If it's high:** consider restructuring the execution flow to keep one region I-cache-hot (e.g., inline the superscalar compilation inline in the main loop rather than calling `bl rx_calc_dataset_item`). Major restructuring, but a genuine I-cache thrashing problem would be one of the larger remaining wins.

**If it's low:** close this lead in 5 minutes.

---

### 9. Cache-line-aligned scratchpad allocation — CLOSED (2026-07-26)

Inspected `/proc/<pid>/smaps` for a live `bench_armrx` process: the scratchpad merges with the
Argon2 cache into one VMA (`Size: 264192 kB` = exactly 256 MiB + 2 MiB, no padding) starting at
`0xffff75e00000` — 2 MiB-aligned, far exceeding the 64-byte requirement. Guaranteed by
construction (mmap always returns page-aligned addresses; the cache size itself is a multiple of
64), not incidental. No code change needed.

**The idea:** the 2 MiB per-worker scratchpad is `mmap`'d without explicit alignment beyond page size. If scratchpad addresses computed in the main loop happen to cross 64-byte cache-line boundaries, each 64-byte `LDR`/`LDP`/`STR` sequence spans two cache lines, doubling the L1 data-cache access cost. Ensuring the scratchpad starts on a 64-byte-aligned boundary is trivial (already likely from huge-page allocations, but worth verifying).

**How to check:** inspect `/proc/<pid>/smaps` for the scratchpad mapping, compute `base_address % 64`. If already aligned, close. If not, add `MAP_ALIGNED(64)` or a manual offset adjustment.

**Effort:** 5 minutes to check, zero code change if already aligned.

---

### 10. Argon2 cache explicit MAP_POPULATE for seed-rotation latency — ADOPTED (2026-07-26)

Added `MADV_POPULATE_WRITE` (with a `memset` fallback, mirroring `src/vm.cpp`'s existing scratchpad
pattern) to `Argon2dCache`'s constructor in `src/argon2.cpp`. Confirmed `MiningEngine::tick()`
(`src/mining_engine.cpp:200`) constructs a **fresh** `Argon2dCache` on every seed rotation
(`std::make_shared<Argon2dCache>()`), so this fix genuinely applies each rotation, not just at
process startup. Correctness-verified (full on-device JIT/differential test suite green). Latency-
only by design — doesn't touch the steady-state hashing loop, so not independently quantified
with a benchmark number; adopted on sound reasoning (moves lazy page-fault cost to an explicit,
batched call) plus zero measured correctness risk, matching the existing scratchpad precedent this
mirrors.

**The idea:** the 256 MiB Argon2 cache uses `MADV_HUGEPAGE` (passive THP hint) but not `MAP_POPULATE` on the fallback path. Adding `MAP_POPULATE` or `MADV_POPULATE_WRITE` to the THP path would prefault all pages at allocation time rather than on first access. Doesn't help steady-state hashrate, but reduces seed-rotation latency (the pause when a new block arrives and the cache must be rebuilt).

**Already checked?** Confirmed by direct code read (2026-07-25) — `src/vm.cpp:147` has `MADV_POPULATE_WRITE` for the scratchpad; `src/argon2.cpp:277` only has `MADV_HUGEPAGE`, no populate call at all (not even a `memset` fallback). This is a real, verified asymmetry, not a hypothesis. Deliberate? Might just be an oversight. Worth checking whether seed-rotation latency matters to the user — if mining on a pool with frequent block changes, every second of rotation latency is lost hashrate.

**Effort:** 1 line change + measurement on a live pool.

---

### 11. Static template `.p2align` tuning for I-cache lines — ADOPTED (2026-07-26)

Changed `src/jit_compiler_a64_static.S`'s main loop entry from `.p2align 5` to `.p2align 6`
(64-byte, matching the Cortex-A53's actual I-cache line size). On-device `perf stat` isolated
effect vs. the #6+#12-only baseline: cycles −0.048% — noise-level, exactly as this idea's own
description predicted ("probably noise-level, but zero cost to try"). Kept anyway: zero
correctness risk, sound reasoning, no measured downside.

**The idea:** the main loop entry uses `.p2align 5` (32-byte alignment, confirmed current at `src/jit_compiler_a64_static.S:218`). The Cortex-A53 I-cache line is 64 bytes. Changing to `.p2align 6` guarantees the loop entry starts at a cache-line boundary, potentially reducing I-cache misses on the first iteration. Tiny effect, zero risk.

**Already flagged elsewhere:** this is the same item as section 2.4 of `docs/audits/PROJECT_AUDIT_REPORT_20260725_Deepseek.md`, deprioritized there as low-value. Listed here for completeness, not as a new finding.

**Effort:** 1 character change in the `.S` file. Measure with `perf stat -e L1-icache-load-misses`. Probably noise-level, but zero cost to try.

---

### 12. `-fomit-frame-pointer` / frame pointer elimination — ADOPTED (2026-07-26)

Confirmed genuinely untried, and confirmed real headroom before implementing: `objdump -d` showed
219/523 functions (42%) in a release build carry a `stp x29, x30` frame-pointer prologue. Added
`-fomit-frame-pointer` to `armrx_core`'s non-ASAN compile options (guarded `if(NOT ARMRX_ENABLE_ASAN)`,
since ASAN needs frame pointers for its own stack traces — already forces `-fno-omit-frame-pointer`
back on in that block). Measured together with idea #6 — see that item's numbers (avg cycles
−0.301%, IPC +0.298% across two on-device samples). Kept.

**The idea:** on x86_64 with `-O2`+, GCC omits frame pointers by default. On AArch64, the default may differ. Frame pointers cost one register (x29) and a store/load pair per function call. In the JIT-compiled code (which uses its own register convention), this doesn't matter. In the C++ overhead slice (15.56%), it might. Check what the current build emits and whether `-fomit-frame-pointer` is already active.

**Already checked?** Confirmed by grep (2026-07-25) — `CMakeLists.txt:153` only sets `-fno-omit-frame-pointer` inside the `ARMRX_ENABLE_ASAN` block. A normal release build has no explicit frame-pointer directive either way, so the compiler default applies and is unverified. Genuinely open.

**Effort:** `objdump -d build/armrx | grep -c 'stp.*x29.*x30'` to count frame-pointer saves. If high, add the flag. Already likely optimized away by `-O2` + LTO, but worth a 30-second check.

---

## Merged from the retired `future-performance-ideas-20260725.md` (superseded 2026-07-25)

### 13. Re-run `devbox_pgo_build` after a substantial code change

PGO has been re-confirmed null twice (Phase 5, Phase 6) on the current code shape. Not worth
re-running speculatively — but if any idea above (especially #1/#7, which change code layout)
lands, that's a reshaped binary and a legitimate reason to re-check PGO once, since a stale
profile is a plausible reason for a prior null result.

### 14. Finer-grained instruction/cycle reconciliation via `tools/jit_correlate.py`

The tool now explains ~86% of samples with high confidence (63.5% superscalar-attributed + 20%
main-VM-program + 2.4% superscalar-wrapper + 14% named C++, roughly). The remaining
unattributed slice is small enough that further tooling investment here has a shrinking
payoff — likely not worth it without a specific new hypothesis to test.

### 15. Miscellaneous audit-flagged items, not yet acted on

`-mtune=cortex-a53`, interpreted-path dataset prefetch, a SIGSEGV/SIGBUS JIT-fault handler,
windowed hash-rate reporting, oversubscription warnings, and BOLT — all previously flagged by
external audits (Hermes, Gemini) as "not yet acted on, needs measurement." None have a
specific evidence-backed reason to prioritize over the ideas above; pick one only if it maps
to an actual pain point someone hits.

---

## Grouping by expected payoff and risk

| Idea | Region targeted | Expected payoff | Correctness risk | Effort to measure | Status (2026-07-26) |
|---|---|---|---|---|---|
| #1 IMUL_RCP register pre-assignment | Superscalar (72.7%) | Medium | Low | ~2 hours | **Reverted** — JIT/interpreter divergence |
| #2 IXOR_C* immediate opt | Superscalar (72.7%) | Small | Low | ~1 hour | Not yet attempted |
| #4 Profile C++ slice | C++ overhead (15.6%) | (Diagnostic) | None | 15 min | **Done** — AES fill/hash dominant (12.3%) |
| #5 BLAKE2b NEON | C++ overhead (15.6%) | Small-Medium | Low (KATs catch) | ~3 hours | **Closed** — negligible in profile |
| #6 Visibility flags | All C++ | Small | None | 30 min | **Adopted** — +0.298% IPC avg |
| #7 Double-buffered JIT | All | Small (~1.76% max) | Low | ~4 hours | Not yet attempted |
| #8 I-cache pressure measurement | Cross-cutting | (Diagnostic) | None | 15 min | **Closed** — 0.788% miss rate |
| #9 Scratchpad alignment | Main VM (9.2%) | Tiny | None | 5 min | **Closed** — already 2 MiB-aligned |
| #10 Argon2 MAP_POPULATE | Seed rotation | Small (latency) | None | 30 min | **Adopted** — verified, not quantified |
| #11 `.p2align 6` | Template | Tiny | None | 10 min | **Adopted** — noise-level as predicted |
| #12 Frame pointer check | C++ overhead | Tiny | None | 10 min | **Adopted** — +0.298% IPC avg (with #6) |
| #3 Base+offset pool | Superscalar | Small | Low | ~2 hours | Moot — contingent on #1, which reverted |

---

## What NOT to bother with

- **NEON hardware AES (AESE/AESD)** — reverted: instruction ordering incompatible with RandomX round spec.
- **NEON vector-permute AES** — implemented, measured −19.4%. Flag-gated.
- **Newton-Raphson FDIV/FSQRT** — measured −1.1%. Flag-gated.
- **PGO** — null twice. Keep tooling; re-measure only after a substantial binary change.
- **CBRANCH/CSEL** — +46% branch-misses. Reverted.
- **Argon2 copy elimination** — null. Reverted.
- **`--stagger-ms`** — previously tested, ineffective.
- **Superscalar literal-pool relayout** — measured regression. Reverted.
- **`IMUL_RCP` literal-load elimination** — measured regression. Reverted.
- **Memory-op scheduler extension** — reverted after test failure; see `docs/plans/performance-plan-20260725.md` Step 3 for the gated re-attempt path.
- **Superscalar `IMUL_RCP` register pre-assignment (idea #1)** — reverted after `test_jit_equivalence` failure, 2026-07-26; see `docs/experiments/superscalar-imul-rcp-preassignment-attempt.md`.

---

## How to use this file

1. Pick the cheapest-to-measure idea first (#4, #8, #9, #12 — diagnostic, 5-15 minutes each).
2. If a diagnostic reveals a real lead, follow it. If it's a dead end, close it in 5 minutes and move on.
3. ~~The ideas with real upside (#1, #5, #6, #7) justify implementation time ONLY if the diagnostics show room.~~
   **Revised 2026-07-26, per explicit user direction**: for low-risk ideas (compiler flags,
   alignment, small isolated refactors), implement and measure directly rather than gating on a
   diagnostic first — a weak diagnostic isn't proof there's no win, and over-gating stalls
   progress. Reserve the gate-before-you-build discipline for genuinely high-risk items (anything
   touching the JIT scheduler's hazard model, like #1 turned out to be) where the failure mode is
   silent wrong hashes, not just wasted effort.
4. Every idea should be measured on real hardware before adopting — this project's track record of "good on paper, null on silicon" (PGO, NEON AES, CBRANCH/CSEL, Argon2 copy elimination, superscalar literal-pool relayout, memory-op scheduler, and now #1) is approximately 8 for 8. The next idea has roughly even odds of being real or noise.
