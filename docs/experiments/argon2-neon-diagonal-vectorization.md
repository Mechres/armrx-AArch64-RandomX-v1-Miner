# Argon2 NEON Diagonal-Step Vectorization

**Date:** 2026-07-23
**Status:** Implemented, measured, kept — a real, evidence-backed improvement.

## Background

Following the CBRANCH investigation (`docs/experiments/branchless-cbranch.md`), which
found CBRANCH misprediction was never a meaningful real-world lever once
properly isolated, `Argon2dCache::initialize()` was flagged as a comparably-
sized, never-independently-investigated contributor to the earlier profiling
pass (12.44% of `--full-hash-only` samples for the function itself, plus
3.23%/2.95% for `permute_block_neon`/`permute_16_neon` — 18.62% combined,
larger than CBRANCH's 10.54%). This investigation follows the same "profile
first" discipline before touching any code.

## Profiling

Isolated the workload with a new `bench_armrx --argon2-only` benchmark
(`tests/bench_armrx.cpp`'s `bench_argon2_cache_init()`) that constructs one
full light-mode-sized (256 MiB, 262,144 blocks, 3 passes) `Argon2dCache` and
calls `initialize()` repeatedly with different keys, isolated from every
other benchmark section — the same lesson from CBRANCH's investigation about
not measuring a mixed workload.

**Step 1 — multi-event `perf stat`** (learned from CBRANCH: check memory-
boundedness and branch behavior *together*, don't assume one is the
bottleneck):
```
perf stat -e instructions,cycles,branches,branch-misses,cache-references,cache-misses ./bench_armrx --argon2-only
```
Result: IPC 0.65, branch-miss rate 2.0%, cache-miss rate 0.3%. Both ruled
out — this is not a branch-misprediction or memory-latency story, despite
Argon2's deliberate memory-hardness design making the latter a very
reasonable prior hypothesis going in.

**Step 2 — `perf record -e cycles`** with symbol attribution (212K samples,
full run to completion):

| Symbol | % of cycles |
|---|---|
| `gb()` (scalar mixing function) | **38.09%** |
| `permute_16_neon` | 26.10% |
| `Argon2dCache::initialize` | 23.17% |
| `memcpy` | 5.54% |
| `permute_block_neon` | 5.24% |

38% of *all* cycles spent initializing the cache go to one scalar function.

## Root cause

`permute_16_neon()` (`src/argon2.cpp`) does 8 mixing rounds per 16-word
block: 4 "column" rounds (indices `(0,4,8,12)`, `(2,6,10,14)`, processed 2 at
a time via the existing `gb_neon()`, which runs 2 G-functions in parallel
NEON lanes) and 4 "diagonal" rounds (`(0,5,10,15)`, `(1,6,11,12)`,
`(2,7,8,13)`, `(3,4,9,14)`). The diagonal rounds fell back to 4 *sequential
scalar* `gb()` calls, per a comment already in the code explaining why:
their register-pairs aren't at consecutive memory positions, so the naive
`vld1q_u64`-based load `gb_neon()` relies on doesn't work for them directly.
Each scalar `gb()` call is also a long, fully-sequential dependency chain
(`blamka_add → rotr → blamka_add → rotr → blamka_add → rotr → blamka_add →
rotr`) with no room for the CPU to overlap work. So half of
`permute_16_neon`'s work got 2x NEON parallelism and the other half got none
— and the unparallelized half dominated the cost.

## The fix

Checking memory adjacency for the 4 operands each diagonal round needs,
grouped into 2 pairs (lane 0 = first G-function, lane 1 = second) the same
way the column step already pairs them:

- Group A (round 1 + round 2): `a`=`words[0],[1]` (adjacent), `b`=`words[5],[6]`
  (adjacent), `c`=`words[10],[11]` (adjacent), `d`=`words[15],[12]` **(not
  adjacent)**.
- Group B (round 3 + round 4): `a`=`words[2],[3]` (adjacent), `b`=`words[7],[4]`
  **(not adjacent)**, `c`=`words[8],[9]` (adjacent), `d`=`words[13],[14]`
  (adjacent).

Only **one** of the four operands per group is non-adjacent. The other three
load/store with plain `vld1q_u64`/`vst1q_u64`, exactly like the column step.
The one scattered pair is gathered via `vcombine_u64(vld1_u64(&words[i]),
vld1_u64(&words[j]))` (two independent 64-bit scalar loads assembled into
one `uint64x2_t`, lane 0 = first arg) and scattered back via
`vget_low_u64`/`vget_high_u64` + `vst1_u64`. `gb_neon()` itself is
unchanged — only the load/store glue around it differs from the column step.
This is the same gather/scatter idea `permute_block_neon()`'s outer loop
already uses for non-adjacent *columns*, just applied one level deeper.

Lane consistency (correctness, not just performance) holds by construction:
every `vld1q_u64(words + i)` naturally puts the lower-index word in lane 0,
and the gather is written to match (`vcombine_u64(low, high)` → lane 0 =
`low`), so whichever G-function occupies lane 0 is consistent across all 4
loads for a group and matches on the way back out.

## Verification

- **Correctness**: `permute_16_neon` computes the exact same mathematical
  permutation, just via a different instruction sequence.
  `tests/test_blake2b.cpp`'s existing reference-value checks
  (`assert_reference_item0`, known first-words of specific dataset items)
  produce byte-identical output before and after. Full RandomX KAT hashes
  (both interpreted and JIT) also unchanged. Full `ctest` green: 8/8
  on-device, 4/4 on x86_64 (the scalar `permute_16()` path there is
  untouched by this change).
- **Performance** — apples-to-apples comparison (old code rebuilt fresh in a
  separate directory, both runs back-to-back to minimize thermal-state drift
  on this passively-cooled device, which showed real run-to-run wall-clock
  variance — e.g. the same old code measured ~8.8s and ~17.0s per cache init
  on two different occasions during this session, so wall-time alone isn't
  trustworthy here; instruction/cycle counts are clock-frequency-independent
  and far more reliable):

| | Old (scalar diagonal) | New (vectorized diagonal) | Δ |
|---|---|---|---|
| Instructions | 18.31B | 13.40B | **−26.8%** |
| Cycles | 26.81B | 21.71B | **−19.0%** |
| Branches | 741.79M | 386.32M | −47.9% |
| Cycles per `argon2_compress` call | 11,364 | 9,204 | −19.0% |

IPC actually drops slightly (0.68 → 0.62) — expected and not a concern: NEON
instructions do more work per instruction (128-bit-wide, 2 lanes at once),
so fewer instructions are needed for the same work even if each one isn't
necessarily faster individually. What matters is total cycles, which drop
by a clear, real 19%.

## Scope of the real-world impact — being honest about what this does and doesn't change

`Argon2dCache::initialize()` runs once per seed-key rotation (roughly every
~2048 Monero blocks), **not** per hash. This optimization reduces **seed-
change latency** (how long the miner spends rebuilding its cache and
therefore not producing hashes, at each rotation) — it does **not** change
sustained steady-state hashrate, which is dominated by the JIT'd per-hash
execution loop this change doesn't touch. This is a real, valuable
improvement, but shouldn't be conflated with a hashrate gain — the same
discipline `docs/experiments/branchless-cbranch.md` insists on for its own numbers.
