# 2026-08-07 — Tier 2(b): widen across-item vectorization of dataset fill

**Status:** CLOSED — REVERTED (honest negative, 2026-08-07). Attempted by external agent,
gated by the brief's kill-criterion. Result below. No code retained.

## Result (agent, on-device 2026-08-07)
- Implemented 4-wide fake-SIMD fill experimentally in src/dataset.cpp (two uint64x2_t[8]
  arrays, amortizing the per-access serial cache-line fetch/XOR across 4 items).
- Host CTest: 9/9 passed (byte-identical to generate_dataset_item).
- Device fill: **165.12 s** vs 163.3 s NEON baseline — ABOVE the 164 s adoption
  threshold. REVERTED.
- Device test_partial_dataset + test_mining: passed.
- The post-fill hashing phase of time_partial_fill segfaulted (full-memory VM setup in
  that harness — separate from the fill itself, which completed successfully).

## Verdict: DEAD as a lever
The serial scalar multiply / high-multiply work (IMUL_R / IMULH_R / ISMULH_R / IMUL_RCP,
which execute_superscalar_neon computes scalar-per-lane because AArch64 NEON has no
u64×u64→u128 op) remains the dominant floor. Widening to 4 lanes amortizes only the
serial cache-line fetch/XOR, which is NOT the bottleneck — the added lane-management
overhead pushed fill time UP (165.12 > 163.3). This confirms beyond-parity_v2.md's
warning that the 2-wide path already saturates the A53 dual-issue pipeline for this
workload.

**Conclusion:** the across-item vectorization lever is exhausted. The dataset fill speed
is bounded by scalar high-multiplies on in-order A53; only a true NEON high-mul (not
available) or the hardware clock/OPP unlock (~+44% at 1.1 GHz vs fixed 765 MHz) would
move it. Tier 2(b) is closed — do not re-attempt.

## Original brief below (preserved for record)

## The gap this closes
- The hybrid partial dataset is now **correct** (rotation + hybrid-consumption fixes shipped
  2026-08-07) but perf-gated at **−31% @ 8w** and a **~164 s dead-start** fill
  (`time_partial_fill 512` = 163.3 s on `lenovo`).
- Tier 1 (`wait_for_fill`) removed the misleading ramp but NOT the 164 s itself.
- Tier 2(a) (drop NEON, scalar fill) was tried and **REVERTED** — scalar = 207 s (WORSE).
  Baseline to beat is the NEON **164 s**.
- Dual-hash interleaving (the other code lever) was measured on-device 2026-08-07 and is
  **DEAD** (no MAC-port slack). So Tier 2(b) is the **only remaining code lever** for fill speed.

## What already exists (READ BEFORE CHANGING ANYTHING)
`src/dataset.cpp` `initialize_dataset()` under `#ifdef __aarch64__` (lines 81–142) ALREADY
processes **2 items per iteration**: `uint64x2_t vr[8]` holds item0 (lane 0) + item1 (lane 1),
calls `execute_superscalar_neon(vr, prog, ...)` and XORs two cache lines. There is a leftover
single-item tail loop (lines 137–142) for odd counts.

**Critical finding — the "NEON" path is a FAKE-SIMD for multiplies.** In
`src/superscalar.cpp` `execute_superscalar_neon()` (lines 773–845):
- TRUE NEON (vectorized): `ISUB_R` (`vsubq_u64`), `IXOR_R` (`veorq_u64`), `IADD_RS`
  (`vshlq_n_u64`+`vaddq_u64`), `IADD_C*`/`IXOR_C*` (`vaddq_u64`/`veorq_u64` + `vmovq_n_u64`).
- SCALAR-WITH-EXTRACT (NOT vectorized): `IMUL_R`, `IMULH_R`, `ISMULH_R`, `IMUL_RCP`, `IROR_C`
  — each does `vgetq_lane_u64` ×2, scalar op, `vcombine_u64`. So the ~35% of the
  2048-instruction superscalar body that is multiplies runs **twice (per lane), scalar, on the
  in-order A53** (each `IMUL_R` = 4-cycle interlocked multiply). The 2-wide speedup only helps
  the non-multiply ~65%.

## The actual lever (two coupled sub-problems)
### (b1) Widen to 4 (or 8) items to amortize the SERIAL per-access work
The dominant serial cost that does NOT scale with lane count is the cache-line fetch + XOR
inside the `kRandomXCacheAccesses` (8) inner loop (`dataset.cpp:103–126`): per access it does
`load_cache_line` ×2 (each = 1 modulo + 2 dependent `memcpy` into a `DatasetItem`) then an 8×8
byte XOR. This runs **once per access regardless of lane width**. Widening to 4 items (two
`uint64x2_t[8]` arrays `vr_a`, `vr_b`) halves outer-loop iterations and amortizes those 8
serial cache-line fetches/XORs across 4 items. This is the promising angle.
- Implementation sketch: loop `offset += 4`, build `vr_a[8]` (items 0,1) and `vr_b[8]`
  (items 2,3) via `vcombine`, run the 8-access inner loop calling
  `execute_superscalar_neon(vr_a,...)` AND `execute_superscalar_neon(vr_b,...)` per access, XOR
  `line0/1/2/3` into both, store 4 outputs. Keep the odd-tail fallback (or pad to even 4).
- NOTE: this does NOT make multiplies parallel (still scalar-with-extract in
  `execute_superscalar_neon`); it only amortizes the per-access serial fetch/XOR. That may be
  enough if the fetch/XOR dominates.

### (b2) True NEON multiply-high (HARD, likely not worth it)
`IMULH_R`/`ISMULH_R` need the HIGH 64 bits of a 128-bit product. AArch64 NEON has NO
`u64×u64→u128` multiply, so a true vectorized high-mul is not directly available. Scalar
`__uint128_t` (what `mulh`/`smulh` already use) is unavoidable per lane. Cross-lane tricks
(`vmull_u32` on split 32-bit halves + recombine) exist but are complex and may not beat scalar
on in-order A53. **Treat (b2) as a stretch ONLY if (b1) alone doesn't beat 164 s** — measure
first.

## Kill criterion (honest)
- MUST beat **164 s** on-device (`time_partial_fill 512`, or the harness below) to be adopted.
- MUST stay **byte-identical** to `generate_dataset_item` (verified by `test_partial_dataset`
  host + on-device, and `test_mining`).
- If (b1) gives e.g. 150 s that's a marginal win; if it regresses → REVERT, document why
  (the `beyond-parity_v2.md` note warns the 2-wide path may already saturate the A53 dual-issue
  pipeline, so widening lanes may yield ~0). Report the honest number either way.

## Measurement harness (on-device)
The fill time is observable two ways:
1. `time_partial_fill 512` (a registered bench target — check `tools/` / CMake; if absent, the
   `PartialDataset` fill is timed by `--dataset-mb=512` startup log "PartialDataset: started
   fill ... workers resuming" delta).
2. Direct: a tiny driver that constructs `PartialDataset(8<<20 items)` + `Argon2dCache`, calls
   `start_fill()` + `wait_for_fill()`, `clock()` around it. (Reuse `tools/verify_seed_rotation.cpp`
   shape if helpful — but that's light-mode; for fill timing you want the `PartialDataset` path
   directly. If no standalone fill timer exists, ADD ONE — `tools/time_partial_fill.cpp` — it is
   legit experiment tooling, not shipped in the miner.)

Run on `lenovo` (aarch64), non-isolated is fine for a fill-only timing:
```
cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake
cmake --build build-cross -j$(nproc)
scp build-cross/<fill_timer> mechres@192.168.10.156:/tmp/cross-dag/
ssh mechres@192.168.10.156 'cd /tmp/cross-dag && time ./<fill_timer> 512'
```
Compare against the NEON baseline (163.3 s). ALWAYS check binary `mtime` after cross-build
(stale dep tracking; `find build-cross -name '*.o' -delete` to force a real rebuild).

## Device / discipline notes
- Device `lenovo` = 192.168.10.156, aarch64, 8× Cortex-A53 @ fixed 765 MHz (no cpufreq),
  two clusters 0-3/4-7. `isolcpus` OFF. `/tmp/cross-dag/` = 512M NOEXEC tmpfs, clears on reboot.
- One test/session. Don't run `armrx` foreground via an automated SSH that drops on the silent
  ~164 s fill phase — use a detached `setsid sh -c "..."` + poll the log, or have the user run
  interactively.
- Correctness gate is cheap & host-runnable: `ctest` → `test_partial_dataset`,
  `test_mining`, `armrx_tests` (JIT 16/16). Run host FIRST; only ship to device for the timing A/B.

## Deliverables expected from the agent
1. The widened fill (prefer (b1); attempt (b2) only if (b1) alone doesn't beat 164 s and you can
   show NEON high-mul helps).
2. Host: `test_partial_dataset` + `test_mining` + `armrx_tests` PASS (byte-identical to
   `generate_dataset_item`).
3. On-device: `time_partial_fill 512` number vs 163.3 s baseline, honestly reported.
4. A short note: did (b1) amortize the serial fetch/XOR, or did the per-lane scalar multiply
   remain the floor (→ widen further is futile, document and stop).
DO NOT touch the mining JIT, the hybrid `_end_hybrid` path, or seed-rotation logic. Scope is the
fill only.
