# Argon2 Performance Backlog Closeout — Copy Elimination (Reverted) + Driver-Code Attribution

## Problem

The same `perf record -e cycles` pass (212K samples, `bench_armrx
--argon2-only`, full 256 MiB light-mode cache init) that found and fixed the
`permute_16_neon` diagonal-step bottleneck (see
`docs/experiments/argon2-neon-diagonal-vectorization.md`) also attributed **23.17% of
cycles to `Argon2dCache::initialize`'s own driver code** and **5.54% to
`memcpy`** — tracked as an open lead in `NEXT_STEPS.md` §5 and `PLAN.md` Phase
3 item C, not investigated further at the time.

## Root cause found

`argon2_compress()` (`src/argon2.cpp`), called once per cache block —
262,144 times for a 256 MiB light-mode init:

```cpp
Argon2Block result{};
for (i) result[i] = previous[i] ^ reference[i];
auto permuted = result;        // full 1024-byte copy
permute_block(permuted);       // mutates the copy in place
for (i) { result[i] ^= permuted[i]; if (dest) result[i] ^= (*dest)[i]; }
return result;
```

`Argon2Block` is `std::array<std::uint64_t, 128>` = 1024 bytes.
`permute_block` mutates its argument **in place** (row step permutes each row
directly within the block's own memory, then the column step gathers/
permutes/scatters columns back into the same block). The `auto permuted =
result;` line exists because the algorithm needs both the original `R =
previous^reference` (kept in `result`) and the fully permuted `Z =
permute(R)` (built in `permuted`) to compute the final XOR, and an in-place
permute destroys its input as it works — so something has to duplicate `R`
into a second buffer first. That's the 1024-byte copy the profiler was
attributing real `memcpy()` call time to, once per block, every pass.

(Also checked and ruled out as *not* wasteful: the function's `return
result;` plus the caller's `blocks_[current] = argon2_compress(...)` looks
like a second full-block copy, but `result` has a single `return` statement —
a textbook NRVO case — so that's one unavoidable write of the final block
into its home slot, not extra work.)

## Attempted fix

Gave `permute_block` an out-of-place sibling, `permute_block_into(src, dst)`,
for both the NEON and scalar paths: the row step reads 16 words from `src`
into a small stack-local scratch array, runs the unchanged `permute_16`/
`permute_16_neon` on that scratch, and writes the result into `dst` directly
— fusing "copy source into the working buffer" into the row step's existing
load/store instead of doing a separate whole-block `memcpy` first. The
column step was unchanged (it already stages through its own local `words`
array), just reading/writing `dst` instead of a shared in-place buffer.
`argon2_compress` then called `permute_block_into(result, permuted)` instead
of `auto permuted = result; permute_block(permuted);`, and dropped the
redundant `Argon2Block result{};` zero-init (every index is unconditionally
overwritten in the next line anyway).

## Validation

**Correctness (before any benchmarking, per this project's standing
discipline):** `tests/test_blake2b.cpp`'s existing guards for this exact code
path — `argon2_compress(zero_block, zero_block) == zero_block`, a
determinism check, and full-scale `assert_reference_item0` reference-dataset-
item checks against a real `Argon2dCache::initialize()` run — all passed
byte-identical, both the scalar path (x86_64, `armrx_tests`) and the NEON
path (on-device). Full KAT hashes (`"This is a test"`, `"Lorem ipsum dolor
sit amet"`) matched the official RandomX test vectors exactly, interpreted
and JIT mode both. Full `ctest` was green locally (7/7) before proceeding to
perf measurement.

**Apples-to-apples `perf stat`** (old code rebuilt fresh in a separate
directory on the devbox, `bench_armrx --argon2-only`, back-to-back on the
same device):

| | Old (`bne`-era diagonal fix, current) | New (copy-elimination) | Δ |
|---|---|---|---|
| Instructions | 13,416,159,584 | 13,032,942,537 | **-2.86%** |
| Cycles | 20,985,200,430 | 21,059,284,331 | **+0.35%** (worse) |
| Branches | 389,470,675 | 266,000,801 | -31.7% |
| Branch-misses | 15,122,431 | 9,829,150 | -35.0% |
| Cache-references | 6,947,797,676 | 7,310,633,034 | +5.2% |
| Wall-clock (3 runs) | 13,254,807 μs | 13,357,067 μs | flat (within noise) |

Fewer instructions and far fewer branches/branch-misses (removing the
`memcpy()` call also removes its internal size-classification branching),
but **cycles came out flat to very slightly worse** — IPC actually dropped.

**Symbol-attributed `perf record -e cycles`** on both trees explained why:
the `memcpy` share did drop, from 6.77% (old) to 3.56% (new) — but a brand-
new `permute_block_into_neon` symbol appeared at **12.74%**, while the old
`permute_block_neon` (6.68%) disappeared. Old total "supporting" overhead
(`permute_block_neon` + `memcpy` + `memset`) = 15.63%; new total
(`permute_block_into_neon` + `memcpy`) = 16.30%. **The copy work didn't go
away — it relocated** from an explicit, highly-optimized `memcpy()` library
call (which uses wide NEON loads on this hardware) into a hand-written
per-row scratch-copy loop that apparently didn't get fully register-allocated
under `-O2`/multiple locals across a function-call boundary, and ended up
costing about the same, or marginally more, than the call it replaced.

## Conclusion: reverted, no net win

Same standard applied to the CBRANCH/CSEL investigation: implement, measure
honestly, keep only if it's a real improvement. This one wasn't — cycles and
wall-clock came out flat-to-worse despite fewer instructions, so the change
was reverted (`git checkout -- src/argon2.cpp`, back to the diagonal-step-fix
state). **Lesson learned:** eliminating an explicit `memcpy()` call doesn't
automatically save cycles if the replacement code ends up doing materially
the same memory traffic by hand — glibc's `memcpy` on this Cortex-A53 is
already close to as fast as a naive manual copy for a 1024-byte block.

## Follow-up: what *is* `Argon2dCache::initialize`'s own 23-25%? (2026-07-23)

Rebuilt `bench_armrx` with `-g` added (same `-O3`/optimization flags,
just debug symbols) in a separate `build_profile` directory so `perf annotate`
could attribute cycles to individual instructions within
`Argon2dCache::initialize`'s disassembly, not just the function as a whole.

**Finding: it's not separate driver overhead at all.** Of ~3600 sampled
instructions inside the function, 3315 (92%) carry ≈0% attributed cost. The
actual per-block address/reference computation (`j1`, `square = j1*j1`,
`x`, `y`, `relative`, `reference` — the `umull`/`add`/`sub`/`lsr` scalar
arithmetic) tops out at 0.16% per instruction, negligible in aggregate.
**Every hot instruction is a NEON `eor v.16b` (vector XOR), `ldr q`/`str q`/
`ldur q`/`stur q` (128-bit load/store)** — the top offender alone (`eor
v1.16b, v5.16b, v27.16b`) is 9.90% of *all* cycles in the entire benchmark.

This is `argon2_compress()`'s own XOR-combine math — `result[i] =
previous[i] ^ reference[i]`, then `result[i] ^= permuted[i]` (and `^=
(*destination)[i]` for pass > 0), three full 1024-byte XOR passes per block
— auto-vectorized by GCC into 128-bit NEON operations and **inlined directly
into `Argon2dCache::initialize`'s body** (small function, simple non-
recursive loop call site, comfortably within GCC's inlining threshold at
`-O3`). It shows up under `initialize`'s symbol name only because of that
inlining decision, not because there's a distinct, fixable inefficiency in
the driver loop itself. `permute_block_neon`/`permute_16_neon` remain
separate, non-inlined symbols (58.88% + 6.76% of the same profile) — GCC
inlined one level (`argon2_compress` into `initialize`) but not two
(`permute_block_neon` into the now-inlined `argon2_compress`).

**Conclusion: this is inherent, unavoidable compression work, already
compiler-vectorized — not a bug or missed optimization.** The XORs are
required by the Argon2d spec and can't be eliminated; the compiler is
already doing them as efficiently as a straightforward NEON codegen allows.
**This closes out the Argon2 performance backlog** (`NEXT_STEPS.md` §5,
`PLAN.md` Phase 3 item C) — there is no remaining actionable lead from the
original profiling pass. Any further Argon2 speedup would require either a
fundamentally different memory-traffic strategy (e.g. restructuring how
blocks are laid out to improve cache behavior — unexplored, higher risk,
no evidence yet that cache behavior is even the constraint here given the
0.3% cache-miss rate measured earlier) or accepting diminishing returns on
this function.
