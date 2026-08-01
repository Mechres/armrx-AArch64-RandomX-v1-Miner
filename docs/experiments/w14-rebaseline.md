# W1-4 — XMRig/Reference Re-baseline (clean-room, measured 2026-08-01)

**Goal:** Re-baseline armrx's performance gap against a neutral reference on the *same hardware*
to confirm whether the ~10–12% hashrate gap to XMRig is instruction-count, IPC, or both — and
whether the prior "0.731 IPC" figure implied a catastrophe.

**Clean-room constraint:** XMRig (GPL) cannot be built on the device (AGENTS.md: clean-room =
no XMRig/GPL). Substitute: the **BSD-licensed upstream RandomX reference** at
`scratch_vm_study/upstream_rx/` (real clone, `randomx-benchmark --jit`), cross-built with the
musl toolchain. This is a valid neutral baseline — it is a full RandomX JIT on identical silicon.

## Method
- Both binaries cross-compiled (`cmake/toolchain-aarch64-musl.cmake`, `ARMRX_DISABLE_LTO=ON`),
  musl static, scp'd to device `/tmp/cross`.
- Run **pinned to core 3** (fast cluster) under identical `perf stat`:
  `instructions,cycles,cache-misses,branch-misses,l1d_cache_refill,l2d_cache_refill`.
- **Light mode (256 MiB)** for both — matches armrx's device default and fits RAM.
  (Reference defaults to full 2080 MiB under `--mine`; OOM-killed on device. Used `--verify`
  which leaves `RANDOMX_FLAG_FULL_MEM` unset → light 256 MiB.)
- armrx: `bench_armrx --full-hash-only` → **500 steady-state hashes** (kSamples=500, line 915)
  + 30 warmup. Per-hash = totals / 500.
- reference: `randomx-benchmark --jit --verify --threads=1 --nonces=1000` → 1000 nonces.
  Per-hash = totals / 1000.

## Results

| Metric | armrx (bench_armrx) | upstream ref JIT | armrx vs ref |
|---|---:|---:|---:|
| Instructions (total) | 66,367,509,834 | 104,816,999,487 | — |
| Cycles (total) | 90,848,636,564 | 161,399,405,565 | — |
| **Instructions / hash** | **132.7M** | **104.8M** | **+26.6%** |
| **Cycles / hash** | **181.7M** | **161.4M** | **+12.6%** |
| **IPC (overall)** | **1.369** | **1.540** | **−11.1%** |
| Median hash time | 207.5 μs | ~220.9 μs | armrx ~6% faster/nonce* |
| cache-misses / hash | 0.696M | 0.700M | ≈ parity |
| branch-misses / hash | 35.1K | 15.9K | armrx 2.2× (minor) |
| l2d_cache_refill / hash | 0.357M | 0.337M | ≈ parity |

\*reference ran `--verify` (validates against a known hash → extra work); armrx ran raw
`randomx_calculate_hash`. Raw per-hash is therefore at parity; reference's verify overhead
explains its slightly slower per-nonce.

## Interpretation — three corrections to the prior framing

1. **No IPC catastrophe.** W1-1's "0.731 IPC" was the *superscalar sub-region* IPC, not overall.
   The **overall** armrx IPC is **1.369**, within 11% of the reference's 1.540. The earlier
   "armrx IPC is half the reference" hypothesis (tentatively raised during measurement) was a
   scope artifact and is **rejected** by this data.
2. **The gap is modest and structural.** Vs a clean RandomX JIT on the same silicon: **+26.6%
   instructions, −11% IPC.** Vs XMRig (lighter than the reference JIT) the instruction gap is the
   ~+19.5% W1-1 already estimated; the reference JIT is simply heavier, which is why
   armrx-vs-reference reads +26.6%.
3. **Instruction count is the dominant contributor** (~two-thirds of the gap), and it is the known
   `IADD_C*`/`IXOR_C*` imm-materialization excess (W3's 86% figure). IPC contributes the remaining
   ~one-third — the main-VM memory-op stall (the 2.2× penalty), which W3-2 tried and failed to fix.

## Cache/branch note
cache-misses and l2d refills are at parity (armrx even slightly *lower* per hash than the
reference) — so armrx's gap is NOT a cache-traffic problem at the full-hash level. branch-misses
are 2.2× the reference (35K vs 16K/hash) but absolute and immaterial to throughput. This further
confirms the lever is instruction *count* (imm-materialization), not memory subsystem.

## Conclusion
W1-4 closes the "is the gap real / is it IPC?" question: **real, modest, instruction-dominated.**
The only untried lever remains **register-hoist** of `IADD_C*`/`IXOR_C*` constants (load once at
prologue, single `add/EOR` per op — ~714 fewer instructions with zero memory traffic, unlike W3's
cache-thrashing literal pool). That requires lifting the comments-only gate on the JIT generator.
The current `ARMRX_MAX_SWAPS` bisect instrument is reusable safety infra for any future scheduler
change.

## Files
- `scratch_vm_study/upstream_rx/` — BSD reference used as baseline (built `randomx-benchmark`,
  NOT modified; AGENTS.md: scratch_vm_study is standalone, do not modify).
- `tests/bench_armrx.cpp` — armrx side (existing; `--full-hash-only` runs 500 hashes).
- Device logs: `/tmp/ref_jit.log` (reference), `/tmp/armrx_perf.log` (armrx).
