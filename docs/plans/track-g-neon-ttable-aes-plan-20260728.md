# Track G: NEON T-Table AES Lookup Vectorization — Plan for the Next Agent

## Status this plan assumes

`hash_aes_1r_x4`/`fill_aes_1r_x4` (`src/aes_hash.cpp:97` /
`src/aes_hash.cpp:43`; `hash_and_fill_aes_1r_x4` at line 132 does both in one
pass) cost ~12.3% of all cycles — the single largest named C++ cost in this
project's profiling. This is unimplemented; full background:
`docs/plans/20260727/master-plan-20260727.md`, `## Track G`.

**Read `docs/experiments/neon-vector-permute-aes.md` before touching any of this
code.** A structurally different NEON AES approach (vector-permute `vtbl`-based,
plus a fused hash+fill variant) was already tried and measured **-19.4%** —
regressed, not a win — and is flag-gated off. Hardware AESE/AESD is separately
ruled out for a spec-incompatibility reason (wrong `AddRoundKey` instruction
ordering relative to the RandomX round spec — see
`docs/postmortems/aes-ttable-bug-postmortem.md`), not a performance one. **This
plan's variant is different from both of those** — it keeps the scalar T-table
*algorithm* exactly as-is (bit-identical by construction, since it's the same
table lookups in the same order) and only widens the *lookup* mechanism from
1-at-a-time scalar to 4-at-a-time NEON `tbl`/`tbx` gather, since
`hash_aes_1r_x4` already processes 4 independent 16-byte blocks concurrently
per call. Do not confuse this with the reverted `vtbl` attempt — that one changed
the round *structure*; this one only changes lookup *throughput* within the
existing structure.

## Why the existing 4-lane structure matters

`hash_aes_1r_x4`/`fill_aes_1r_x4` already process 4 independent blocks per call
(the "x4" in the name) using scalar T-table lookups, one lane at a time. NEON's
`tbl`/`tbx` instructions can gather across a 128-bit register — i.e., across
exactly this function's existing 4-lane structure — in one instruction where the
scalar version currently issues 4 separate table-index+load operations. The
XOR-accumulate step that currently happens per-lane in scalar registers can move
into NEON registers alongside the gather. This is a genuinely different
mechanism from the failed attempt: same round math, same table contents, same
number of rounds, only the *lookup* is vectorized.

## Correctness risk: medium, and non-negotiable

Bit-exactness is mandatory — this function sits directly in the RandomX hash
pipeline, and any deviation is a silent wrong hash, not a crash. Existing
golden-pin tests (`tests/test_aes_hash.cpp`) and the hash/fill
decomposition-equivalence check must stay green throughout development, not just
at the end. Ship behind a **new** flag (`ARMRX_ENABLE_NEON_TTABLE_AES`, following
the existing `ARMRX_ENABLE_NEON_AES` option's pattern in `CMakeLists.txt:22` —
do not reuse that flag, since it currently means the different,
spec-incompatible hardware-AES path), default OFF, and never let it reach the
production dispatch path until independently verified on real hardware.

## Step-by-step plan

### Step 1 — Establish the bit-exact baseline test harness first

- Before writing any NEON code, build (or confirm) a test that runs the existing
  scalar `hash_aes_1r_x4`/`fill_aes_1r_x4` against a fixed set of inputs and
  records the exact output bytes. This is your ground truth — every subsequent
  step must match it exactly, not approximately.

### Step 2 — Implement the vectorized lookup, gated behind the new flag

- Add `ARMRX_ENABLE_NEON_TTABLE_AES` to `CMakeLists.txt` (default OFF), following
  the existing `ARMRX_ENABLE_NEON_AES` option's structure.
- Implement the NEON `tbl`/`tbx`-based 4-lane gather version of the T-table
  lookup inside `src/aes_hash.cpp`, behind `#ifdef`/runtime dispatch consistent
  with how `ARMRX_ENABLE_NEON_AES` is currently wired (check
  `src/aes_hash.cpp` and `src/cpu_features.cpp` for the existing dispatch
  pattern before inventing a new one).
- Keep the round *structure* (number of rounds, `AddRoundKey` ordering, table
  contents) untouched — this step only changes how the table lookup for each of
  the 4 lanes is performed, not what is looked up or in what order.

### Step 3 — Bit-exact verification against Step 1's baseline

- Run the new path against the exact same fixed inputs from Step 1 and diff byte
  for byte. Any mismatch means the gather logic doesn't match scalar semantics
  (a common `tbl`/`tbx` pitfall: out-of-range table indices in `tbl` produce zero
  by AArch64 spec, while `tbx` leaves the destination lane unchanged — pick
  whichever matches the T-table's actual index range and verify it explicitly,
  don't assume).
- Extend `tests/test_aes_hash.cpp` with the new path's own golden-pin cases,
  gated behind the new flag, rather than only relying on ad hoc comparison during
  development.

### Step 4 — Full test suite, flag on

- Build with `-DARMRX_ENABLE_NEON_TTABLE_AES=ON` and run the full `ctest` suite,
  including `test_jit_equivalence` and the full KAT set — this function sits
  upstream of everything else in the hash pipeline, so a subtle bug here could
  show up as failures anywhere downstream, not just in `test_aes_hash.cpp`
  itself.

### Step 5 — Measure on real hardware, `taskset`-pinned

- `bench_armrx` full-hash comparison, flag off vs. flag on, pinned per the
  master plan's §4 measurement discipline (reversed trial order, long window).
  Given the sibling `vtbl` attempt regressed by -19.4%, do not assume this
  variant wins just because the mechanism differs — measure before drawing any
  conclusion, and be prepared for a null or negative result here too.

## Validation

1. `tests/test_aes_hash.cpp`, both flag-off (regression-free) and flag-on
   (new golden-pin cases), green.
2. Full `ctest` suite green with the flag on.
3. `bench_armrx` A/B, pinned, reversed trial order, written up with real numbers.

## Success criterion

A measured net hashrate improvement with the flag on, full bit-exactness
maintained across all existing and new tests, and no regression with the flag
off (the default state must remain byte-for-byte identical to today's behavior).
Given this targets the single largest named C++ cost in the project (12.3% of
cycles), this is potentially the largest single win in the backlog — but per the
sibling attempt's outcome, it must be benchmarked, not assumed, before it's
adopted. A clean negative result is also a complete, valid outcome — write it up
in `docs/experiments/` either way, following the existing
`neon-vector-permute-aes.md` document's structure so future sessions can compare
the two attempts side by side.
