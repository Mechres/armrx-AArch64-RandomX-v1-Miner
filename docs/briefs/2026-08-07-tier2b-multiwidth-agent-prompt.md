# VERBATIM AGENT PROMPT — Tier 2(b) re-attempt: 3 distinct designs

You are re-attempting the Tier 2(b) optimization lever in the `armrx` AArch64 RandomX
miner (repo: /home/mechres/Projeler/aarch64-randomx, C++ / CMake, cross-compiled for
aarch64 Cortex-A53, 8 cores). Previously a SINGLE 4-wide design regressed (165s vs
163.3s baseline) and the lever was closed. Per the new project rule, the kill criterion
is PER LEVER FAMILY: try 3 DISTINCT designs, close the lever only if ALL fail. You
implement each on its OWN branch, verify host + on-device, and leave each branch
non-reverted so the senior engineer (Hermes) can inspect. Do NOT modify Part 1
contiguous publish.

## Read first (do not skip)
- `docs/briefs/2026-08-07-tier2b-multiwidth.md` — the full brief (3 designs, gates,
  branch discipline, per-family kill criterion).
- `src/dataset.cpp` `initialize_dataset` (~81-142) — already 2-wide; you widen from here.
- `src/superscalar.cpp` `execute_superscalar_neon` (~773-845) — FAKE-SIMD: NEON
  add/sub/xor/shift, but multiplies (`IMUL_R`, `IMULH_R`, `ISMULH_R`, `IMUL_RCP`) and
  `IROR_C` are scalar extract/recombine. Design B changes this (shared by fill + JIT).
- `docs/briefs/2026-08-07-tier2b-across-item-vectorization.md` — the prior single
  attempt (context on why 4-wide regressed: serial per-access cache-line fetch doesn't
  scale with lane count).

## Branch discipline (CRITICAL — code never lost)
From main @ `41ae490`, create THREE branches:
- `try/tier2b-8wide`
- `try/tier2b-neon-mulh`
- `try/tier2b-cacheline`
Implement ONE design per branch. Commit on the branch. Do NOT merge to main. Do NOT
revert/delete the branch work — leave it committed so Hermes can `git diff main...branch`.

## Design A — 8-wide fake-SIMD  [branch: try/tier2b-8wide]
In `initialize_dataset`, hold FOUR pairs (8 items) in the `uint64x2_t vr[8]` array
(item0..item7), calling `execute_superscalar_neon` per pair, halving the outer loop
iterations vs 4-wide. Keep the fake-SIMD multiply extract/recombine; only change item
count per iteration + load/store stride for 8 items. Do NOT touch
`execute_superscalar_neon` itself. Goal: test if 8-wide amortizes the cache-line fetch
better than 4-wide did. Gate: `time_partial_fill 512` < 163.3s AND `test_partial_dataset`
byte-identical. Else REVERT on branch.

## Design B — True NEON multiply-high  [branch: try/tier2b-neon-mulh]
The hard stretch, never tried. In `execute_superscalar_neon`, replace the scalar
`IMULH_R`/`ISMULH_R` (u64×u64→u128 high 64) with REAL NEON: decompose each u64 into two
u32 limbs, use `vmull_u32` (u32×u32→u64) on the 4 cross-limb products + `vmlal`/
`vmlal_lane` to reconstruct the full 128-bit product, take the high 64 bits. `IMUL_R`
(low 64) via `vmull`+`vmlal`; `IMUL_RCP` similarly; `IROR_C` NEON-ize or keep. This
parallelizes the DOMINANT multiply cost across NEON lanes — no scalar extract. This
function is shared by fill AND JIT, so it MUST pass JIT KATs: `armrx_tests` JIT 16/16,
`test_mining` (incl. `test_light_mode_partial_dataset_matches_reference`), and
steady-state H/s must not regress. If multiply-high is correct but fill not faster, it is
STILL valuable for the JIT — report separately. If it breaks a KAT or regresses IPC,
REVERT this design only. Gate: fill < 163.3s AND JIT 16/16 AND test_mining pass AND
steady H/s no regression.

## Design C — Cache-line amortization  [branch: try/tier2b-cacheline]
In `initialize_dataset`'s inner loop, decouple the serial per-access `load_cache_line`
(2 dependent memcpys + modulo per access) from lane count. Pre-fetch a BLOCK of cache
lines (the superscalar read-set for K items) once into a local buffer, run K items'
`execute_superscalar_neon` against it, then refill. Keep 2-wide (or combine with A).
Pure restructuring of `initialize_dataset`; do NOT touch `execute_superscalar_neon`.
Attacks the per-access serial work 4-wide didn't help. Gate: `time_partial_fill 512`
< 163.3s AND `test_partial_dataset` byte-identical. Else REVERT on branch.

## Correctness gate (ALL designs)
1. Host: build + `./build/test_partial_dataset` (contiguous KAT MUST pass) +
   `test_mining` + `armrx_tests` (JIT 16/16). For Design B also confirm JIT hashes
   byte-identical to reference.
2. On-device (lenovo, mechres@192.168.10.156, aarch64): cross-build
   `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake
   && cmake --build build-cross -j$(nproc)`; scp binaries to `/tmp/cross-dag/` (NOEXEC
   tmpfs — run from there); run `time_partial_fill 512` for fill time and
   `--pool-test --dataset-mb=512 --workers=7 --seconds=200` for steady H/s. One
   test/session per branch; separate scp/ssh; never qemu; kill stale procs first.
3. Wrong hashes or byte-difference vs reference → REVERT that design and report.

## Kill criterion (PER FAMILY — all 3 must fail to close the lever)
- Adopt any design whose gate passes. Multiple may pass — adopt the best.
- CLOSE Tier 2(b) ONLY IF all 3 designs fail their gate. One failure does NOT close it.
- Report honest numbers per design either way — no fake win.

## Discipline
- Do NOT modify Part 1 contiguous publish in `partial_dataset.cpp` unless a design
  requires it (it shouldn't).
- Do NOT commit the audit docs in docs/audits/.
- Per branch, report: diff scope, host results, on-device fill time + steady H/s +
  correctness. Leave the branch non-reverted so Hermes can inspect.
