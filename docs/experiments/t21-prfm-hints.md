# T2-1: PRFM Scratchpad Prefetch Hints — Measured Regression (2026-08-01)

Gate item T2-1 from `docs/audits/combined-audit-20260731.md`: insert
`PRFM PLDL1KEEP` hints for scratchpad loads in the main-VM JIT, one
instruction before each LDR, and A/B it on-device.

## History this experiment closes

- **2026-07-24:** three `prfm pldl1keep` scratchpad hints were tried in the
  fill loop (prefetching next-hash data) and **removed as a regression**;
  removing them gained **+0.885% IPC** — the project's largest code-level
  win. See `src/jit_compiler_a64_static.S:397-404` (now a comment block)
  and `RETROSPECTIVE.md:105,159`.
- The `.S` comment mandated: *"Do not re-add without a fresh measurement."*
- **This experiment is that fresh measurement**, at a different insertion
  point: inline in the hot main-VM program (one PRFM per scratchpad LDR,
  immediately after the address computation), rather than a full-hash-ahead
  fill-loop lookahead.

## Implementation (reverted after measurement)

Two insertions in `src/jit_compiler_a64.cpp` (reverted — tree clean at HEAD):

- `emitMemLoad<tmp_reg>` — `src != dst` branch only: `PRFM PLDL1KEEP, [x2, tmp_reg]`
  between the address `and` and the `ldr`.
- `emitMemLoadFP<tmp_reg_fp>` — same position before the `ldr d...`.
- Encoding `0xF8B0C000 | (tmp << 16) | (2 << 5)` verified against the
  existing PRFMs in `jit_compiler_a64_static.S`.
- Correctness: PRFM is architecturally a hint (never faults, no state
  change) — all JIT equivalence/determinism/encoding tests and mining KATs
  passed byte-identical with the hints present.

## A/B (taskset -c 3, perf stat, --full-hash-only, 500 hashes ~105 s each)

Order B-P-B-P (PRFM lost to baseline on every metric in every pairing):

| run | tag | cycles | instructions | IPC | l1d_refill | wall s | µs/hash | H/s |
|---|---|---|---|---|---|---|---|---|
| 1 | BASELINE | 90,866,668,938 | 66,349,765,078 | 0.7302 | 351,590,859 | 117.31 | 206,536 | 4.84 |
| 2 | PRFM | 91,319,757,963 | 66,661,881,518 | 0.7300 | 351,423,211 | 117.89 | 207,683 | 4.82 |
| 3 | BASELINE | 91,064,836,949 | 66,349,822,254 | 0.7286 | 351,586,251 | 117.58 | 207,098 | 4.83 |
| 4 | PRFM | 91,299,921,872 | 66,661,915,187 | 0.7301 | 353,081,403 | 117.87 | 207,708 | 4.81 |

Deltas (PRFM vs adjacent baseline): cycles **+0.499% / +0.258%**,
wall **+0.495% / +0.248%**, instructions **+0.470% / +0.470%** (exact —
fixed added PRFMs per hash), l1d_cache_refill −0.048% / +0.425% (noise).

## Verdict: REGRESSION — T2-1 closed

- **Mechanism:** on the in-order A53, the PRFM issues in the same cycle
  window as the dependent LDR it precedes (address from the immediately
  preceding `and`), so the load stall is not hidden — the hint only adds an
  issue slot and instruction count. `l1d_cache_refill` unchanged proves no
  prefetch effect occurred.
- **Decision rule (hard gate):** win ≥0.5% in both trials → adopt; null or
  regression → close. PRFM lost both trials on cycles and wall time →
  **closed as regression, not adopted.**
- **Corroboration:** independent confirmation of the 2026-07-24 fill-loop
  regression, at a different insertion point. The `.S` comment's mandate is
  now fulfilled: the fresh measurement exists and the removal decision stands.
- Baseline-to-baseline drift (runs 1 vs 3, IPC 0.7302 vs 0.7286) confirms
  ~0.2% run noise; PRFM still separated consistently above baseline in both
  pairings, matching the historical direction.

## Files

- Implementation (reverted): `src/jit_compiler_a64.cpp`
- Prior removal record: `src/jit_compiler_a64_static.S:397-404`
- Audit row: `docs/audits/combined-audit-20260731.md` (T2-1 → closed)
- Change log: `changelogs.md` (2026-08-01)
