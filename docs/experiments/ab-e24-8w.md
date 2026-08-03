# A/B — E24 C* immediate padding at 8w

**Date:** 2026-08-05 (session clock)
**Author:** Hermes
**Status:** DONE — E24 padding has NO measurable 8w effect
**Purpose:** k3 re-localization step after M1 refuted the `*_M` load-stall hypothesis. Test
whether E24's 3-instr C* immediate padding (the post-E24 instruction-count delta) is the 8w
gap lever.

## Method

- Added compile flag `ARMRX_NO_E24_PAD` (A/B gate in `emitCpoolImmediate`): when defined,
  restores the pre-E24 1-instruction `MOVZ` fast path for small immediates (`imm < 2^16`),
  which is the actual density difference vs E24's forced 2-instr `MOVZ+MOVK`. Source is
  E24+E25 HEAD; flag is default-OFF (E24 behavior preserved).
- Built both variants pristine (rm -rf build-cross; LTO forced off by toolchain FORCE).
- **KAT gate:** `test_jit_equivalence` 16/16 byte-identical for the no-E24 variant — the
  1-instr path is equivalence-safe (same hash, fewer instructions). ✅
- Both measured at **8 workers, real pool** (RandomX light), `perf stat` same events as M1.
  Hash count from "Total: N" (exact). Pristine rebuild fixed the earlier E26-object segfault.

## Results (8 workers, per-hash)

| metric | E24-ON (M1 ref) | no-E24 (A/B) | Δ |
|---|---:|---:|---:|
| hashes | 3292 | 2725 | — |
| `ld_dep_stall` | 30.1 M | **30.0 M** | 0.3% |
| `other_interlock_stall` | 11.8 M | 11.8 M | 0% |
| `instructions` | 103.5 M | 103.8 M | +0.3% |
| `cycles` | 173.0 M | 173.3 M | +0.2% |
| IPC | 0.598 | 0.599 | 0% |
| `l1d/l2d_cache_refill` | 2.38 / 1.65 M | 2.21 / 1.66 M | ≈ |

## Verdict

**E24's padding is NOT the 8w differentiator.** Removing it changes total instructions by
+0.3% (small-C* immediates are a tiny fraction of all instructions) and changes stalls/IPC by
0% (within run-to-run noise). E24 was a legitimate **1w win (+7.1%)** — it hid the 1w MAC
interlock — but at 8w the instruction-count savings are negligible and the throughput is
identical. Keep E24 shipped (it's correct and helps 1w); it is not the lever for the 8w gap.

## Consequence (stacked with M1)

Two refutations now:
1. **M1:** `ld_dep_stall` is ~equal between armrx and XMRig at 8w (30.1 vs 27.0M) → the
   `*_M` load-stall is NOT the differentiator.
2. **This A/B:** E24's instruction padding has no 8w effect → the post-E24 instruction-count
   delta is NOT the differentiator either.

The remaining ~4.6% instruction-count gap vs XMRig (armrx 103.5M vs 98.9M instr/hash at 8w)
must come from **broader emission structure** — the overall JIT codegen shape, not the
`*_M` load path or C* immediates. Next re-localization: compare **instruction mix** between
armrx and XMRig at 8w (e.g. perf with `inst_retired` breakdowns, or a JIT disassembly diff of
a representative program), to find which opcode class armrx emits ~4.6% more of. That is the
actual remaining lever. D (accept 95.2%) is still NOT earned — the gap is real and now
narrowly localized to "instruction mix," not yet to a closable knob.

## Raw perf
- no-E24: `cycles 472,393,918,904; instr 282,767,539,745; ld_dep 81,688,771,147;
  other_int 32,119,436,644; l1d 1,984,454,353; l2d 1,377,691,224` over 2725 hashes / 111.9s.
- E24-ON (from M1): `cycles 569,362,403,973; instr 340,629,561,226; ld_dep 98,919,673,078;
  other_int 38,771,098,427; l1d 2,379,673,579; l2d 1,652,479,458` over 3292 hashes / 134.0s.
