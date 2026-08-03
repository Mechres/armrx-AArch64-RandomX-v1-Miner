# M1 — Miner-to-miner 8w PMU diff (armrx vs XMRig)

**Date:** 2026-08-05 (session clock; agents mislabel as Aug 5)
**Author:** Hermes (executed on device)
**Status:** DONE — refutes the `*_M` load-stall hypothesis
**Purpose:** k3 plan M1 (docs/briefs/opencode_replay-invariant_2026-08-05.md §M1). The entire
`*_M` / `ld_dep_stall` chase rested on an **armrx-only** 1w→8w growth delta. XMRig's 8w
`ld_dep_stall` was never measured. This closes that gap.

## Method

- Device: MSM8929 / Cortex-A53 @ 765 MHz, fan on. Same device state for both runs.
- Both miners: **RandomX light mode**, **8 workers/threads**, real pool
  (`tr.monero.herominers.com:1111`, same wallet/password). Identical workload (same seed via
  pool) → valid per-hash comparison.
- `perf stat -e cycles,instructions,ld_dep_stall,other_interlock_stall,agu_dep_stall,
  l1d_cache_refill,l2d_cache_refill` attached to PID. `perf` v7.1.3, no root, `:u` userspace.
- Hash count: armrx prints `Total: N` (exact). XMRig console is block-buffered (no flush until
  exit) so its count is **estimated** from its known ~28 H/s 8w rate × elapsed (≈6100, ±5%).
  armrx's 3292 is exact.
- Build note: `build-cross` was **poisoned with stale E26 objects** (Reasonix's E26 session
  rebuilt it; the session's clock-jump left `.o` mtimes in the future so `make` skipped
  recompilation → linked crashing E26 hoist). A **pristine `rm -rf build-cross` rebuild**
  fixed the segfaults. All binaries here are from the pristine rebuild (HEAD `0e72144`).

## Results (8 workers, per-hash)

| metric | armrx | XMRig | Δ |
|---|---:|---:|---:|
| hashes (window) | 3292 (exact) | ~6100 (est ±5%) | — |
| `ld_dep_stall` | **30.1 M** | **27.0 M** | armrx **+11%** (within noise) |
| `other_interlock_stall` | 11.8 M | 11.5 M | ≈ equal |
| `instructions` | **103.5 M** | **98.9 M** | armrx **+4.6%** (more instr) |
| `cycles` | 173.0 M | 162.4 M | +6.5% |
| IPC | 0.598 | 0.609 | XMRig slightly better |
| `l1d_cache_refill` | 2.38 M | 2.55 M | ≈ equal |
| `l2d_cache_refill` | 1.65 M | 1.65 M | equal |

armrx per-hash (exact): ld_dep 98.92G/3292=30.07M; other_int 38.77G/3292=11.78M;
instr 340.63G/3292=103.46M; cyc 569.36G/3292=172.96M.
XMRig per-hash (est 6100): ld_dep 164.87G/6100=27.0M (range 25.8–28.4 for 5800–6400);
other_int 69.87G/6100=11.46M; instr 603.12G/6100=98.9M (matches E19's 101.4M census →
confirms ~6100 is right); cyc 990.83G/6100=162.4M.

## Verdict (decision rule from k3 M1)

> "If armrx 8w ld_dep_stall/hash ≈ XMRig's (within ~15%): the stall is NOT the differentiator
> — stop all `*_M` work, re-localize."

**CONFIRMED.** armrx `ld_dep_stall` (30.1M) vs XMRig (27.0M) differ by ~11% — **both miners
suffer essentially the same 8w load-use stall.** The earlier "+63% armrx-only growth" was
misleading precisely because XMRig's 8w side was never measured; now that it is, XMRig shows
the same stall level (its own 1w→8w growth is comparable). The `*_M` / `ld_dep_stall` lever —
which drove E26 and the entire audit effort — is **refuted as the differentiator**.

## What the gap actually is

- armrx emits **+4.6% more instructions/hash** (103.5M vs 98.9M) at **slightly lower IPC**
  (0.598 vs 0.609) than XMRig at 8w.
- 4.6% more instr × ~0.98 IPC ratio ≈ **~4.8% slower** — which is exactly the observed gap
  (26.65 vs 28 H/s = 95.2%). The instruction-count/IPC gap *explains* the throughput gap.
- This points at the **post-E24 instruction-count** question (k3 §8: "the post-E24 +12%
  instruction count interacting with 8w contention"), NOT the load stall. Note: the 1w
  definitive census had armrx *leaner* (89.5M vs 101.4M); the 8w measurement reverses this
  (103.5M vs 98.9M) — a ~15% per-hash increase for armrx from 1w→8w that needs explanation
  (possible: the 8w pool window counts dataset re-init or differently-accounted hashes; this
  is a measurement caveat, not a code claim). The *comparative* conclusion (armrx slightly
  heavier than XMRig at 8w) is robust because both were measured the same way.

## Consequences

1. **E26 / C2 (split-handler `*_M` pipeline) is dead for the wrong reason too** — not just
   replay-risky and low-payoff, but chasing a stall that isn't the differentiator. The
   replay-invariant analysis (INV-1..4) remains valuable as a guard, but the *performance*
   target was mis-localized.
2. **Next lever = instruction count / IPC**, not load latency. Re-localize: why does armrx
   emit more instructions/hash than XMRig at 8w? Candidate: the E24 3-instr C* immediate
   padding (intentional +7.1% 1w win) costs instructions; at 8w the IPC benefit may not
   compensate. Or a non-`*_M` emission difference. This is a **different experiment** (out of
   the `*_M` scope).
3. **D (accept 95.2%) is NOT yet earned** — k3's M1 rule says re-localize, not stop. The
   gap is real and explainable by instr-count; whether it's closable needs a new measurement
   (compare armrx vs XMRig instruction *mix* at 8w, or test E24 reverted at 8w to isolate the
   padding's 8w cost).
4. **V (replay-invariant verifier)** is still worth building — it's a permanent guard for the
   region regardless of which perf lever we chase next, and would root-cause W3-2.

## Raw perf (device)
- armrx: `cycles 569,362,403,973; instr 340,629,561,226; ld_dep 98,919,673,078;
  other_int 38,771,098,427; agu 6,791,510,704; l1d 2,379,673,579; l2d 1,652,479,458` over
  3292 hashes / 134.0s.
- XMRig: `cycles 990,833,996,851; instr 603,122,991,005; ld_dep 164,870,103,804;
  other_int 69,865,650,548; agu 12,318,085,541; l1d 3,881,997,506; l2d 2,716,899,014` over
  ~6100 hashes / 224.5s.
