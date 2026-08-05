# VERBATIM AGENT PROMPT — Tier 2(b): widen across-item vectorization of the dataset fill

You are implementing a performance optimization in the `armrx` AArch64 RandomX miner
(repo: /home/mechres/Projeler/aarch64-randomx). Scope is NARROW: speed up the background
partial-dataset fill. Do NOT touch the mining JIT, the hybrid `_end_hybrid` consumption path,
or seed-rotation logic.

## Context (read the actual files before editing)
The hybrid partial dataset caches a prefix of the fast-mode dataset and fills it in the
background. The fill takes ~164 s on the target device (`lenovo`, aarch64, 8× Cortex-A53 @
fixed 765 MHz) for a 512-MiB / ~8.4M-item dataset. A previous attempt to drop NEON and use the
scalar fill REGRESSED to 207 s (reverted). Baseline to beat: **NEON 164 s**. Dual-hash
interleaving was measured and is DEAD. So this is the only remaining code lever for fill speed.

The fill calls `armrx::initialize_dataset()` (src/dataset.cpp). Under `#ifdef __aarch64__`
(lines 81-142) it ALREADY processes 2 items per iteration using `uint64x2_t vr[8]` (item0 in
lane 0, item1 in lane 1), calling `execute_superscalar_neon()` then XORing two cache lines.
There is a single-item tail loop for odd counts.

KEY FACT you must verify by reading src/superscalar.cpp `execute_superscalar_neon()`
(lines 773-845): it is a FAKE-SIMD. ADD/SUB/XOR/SHIFT are true NEON (`v*_u64`), but every
MULTIPLY (`IMUL_R`, `IMULH_R`, `ISMULH_R`, `IMUL_RCP`) and `IROR_C` extracts both lanes,
computes SCALAR, and recombines with `vcombine_u64`. So the ~35% of the 2048-instruction
program that is multiplies runs twice (per lane), scalar, on the in-order A53. The 2-wide
speedup only helps the non-multiply ~65%.

## The actual lever
Widen to 4 (or 8) items to AMORTIZE THE SERIAL PER-ACCESS WORK. Inside the 8-access inner loop
(dataset.cpp:103-126), per access it does `load_cache_line` ×2 (each = 1 modulo + 2 dependent
memcpy) then an 8×8 byte XOR — this runs ONCE PER ACCESS regardless of lane width. Holding two
`uint64x2_t[8]` arrays (`vr_a` for items 0,1; `vr_b` for items 2,3) and calling
`execute_superscalar_neon` for BOTH per access halves the outer-loop iterations and amortizes
those 8 serial cache-line fetches/XORs across 4 items.

Sketch: loop `offset += 4`; build `vr_a[8]` and `vr_b[8]` via `vcombine_u64`; in the 8-access
inner loop, for each access load `line0/1/2/3`, call `execute_superscalar_neon(vr_a,...)`
AND `execute_superscalar_neon(vr_b,...)`, XOR `line0/1` into `vr_a` and `line2/3` into `vr_b`,
advance both `reg_val`; store 4 outputs (`vgetq_lane_u64` lanes 0/1 from each array). Keep a
correct tail for counts not divisible by 4 (or pad to a multiple of 4).

A true NEON multiply-HIGH ((b2)) is HARD: AArch64 NEON has no u64×u64→u128 multiply, so
`IMULH_R`/`ISMULH_R` need scalar `__uint128_t` per lane regardless. ONLY attempt (b2) if (b1)
alone does not beat 164 s AND you can demonstrate NEON high-mul beats scalar on in-order A53.
Otherwise report (b1)'s number honestly.

## Correctness is non-negotiable
The fill output MUST be byte-identical to `armrx::generate_dataset_item(cache, i)` for every
cached item (that is exactly what `tests/test_partial_dataset.cpp` asserts). The widening must
not change any computed value — only process more items per iteration.

## What to do
1. Implement (b1) in src/dataset.cpp `initialize_dataset()` (aarch64 path). Keep the existing
   2-wide path working as a fallback if you prefer, but the goal is 4-wide.
2. Host build + run: `ctest --test-dir build --output-on-failure` — `test_partial_dataset`,
   `test_mining`, `armrx_tests` (JIT 16/16) MUST all PASS. (Build:
   `cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON && cmake --build build -j`.)
3. Cross-build: `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake && cmake --build build-cross -j$(nproc)`. If the binary mtime doesn't advance after build, `find build-cross -name '*.o' -delete` and rebuild (stale dep tracking).
4. Ship `build-cross/time_partial_fill` to `mechres@192.168.10.156:/tmp/cross-dag/` (512M NOEXEC
   tmpfs). Run on-device: `ssh mechres@192.168.10.156 'cd /tmp/cross-dag && time ./time_partial_fill 512'`.
   Compare against the NEON baseline of **163.3 s**. Report the number honestly.
5. Also run on-device `test_partial_dataset` + `test_mining` to confirm byte-identical (the host
   pass is necessary but the device NEON path is the real target).

## Kill criterion
Adopt ONLY if `time_partial_fill 512` on-device is < 164 s AND `test_partial_dataset` stays
byte-identical. If it regresses or is within noise (e.g. 160 s flat), REVERT and write a short
note explaining whether the per-lane scalar multiply remained the floor (→ widening further is
futile) or the serial fetch/XOR was NOT actually the bottleneck. Report the honest measurement
either way — do NOT claim a win you didn't measure.

## Report back
- The diff (files changed).
- Host ctest result.
- On-device `time_partial_fill 512` number vs 163.3 s.
- Whether (b1) amortized the serial fetch/XOR or the scalar-multiply floor dominated.
- Adopt or revert, with the measured reason.
