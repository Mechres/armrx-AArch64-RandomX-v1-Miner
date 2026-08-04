# 2026-08-07 — Warmup "ramp" root cause, Tier 1 fix, and Tier 2 scope

## TL;DR
The ~20-minute "ramp" from ~5 H/s to ~26 H/s was **NOT a JIT/VM warmup**.
It was a measurement artifact plus a slow dataset fill:

1. **The printed rate is a cumulative average** (`total_hashes / elapsed_time`).
   During the first ~164 s the workers were (under the old behavior) hashing on
   an *empty* partial dataset while the background fill slowly populated it, so
   the cumulative average climbed slowly even though each post-fill hash was at
   full speed. The "ramp" in the *number* is the metric, not the hardware.
2. **The partial-dataset fill itself takes ~164 s** for the 512 MiB / 8.4M-item
   config (measured 8-fill-thread split: 13.8 s Argon2d cache + ~164 s dataset
   fill). XMRig's equivalent fill is ~10 s — armrx's fill is ~16× slower
   (separate issue, see Tier 2).

## Root cause of the slow fill (why 164 s vs XMRig's ~10 s)
The dataset item computation (`initialize_dataset` → `execute_superscalar_neon`)
performs, per item, 8 superscalar-program rounds (each a sequential
register-dependency chain) plus a cache-line XOR. `execute_superscalar_neon`
is **scalar-lane-extract heavy**: for every `IMUL_R`/`IROR_C`/`ISMULH_R` it does
`vgetq_lane_u64` → scalar multiply → `vcombine` (dataset.cpp). 8.4M items × 8
rounds ≈ **67M program executions** × ~2.5 µs ≈ ~167 s — matches the 164 s
measured. This is a NEON *anti-pattern* (paying SIMD load/extract/insert
overhead to do purely scalar work).

### RETRACTION (per external review)
An earlier draft claimed "AES rounds are missing from the item computation, a
divergence from spec." **This is WRONG.** In the RandomX reference algorithm,
`initDatasetItem` does NOT apply AES inside the per-item superscalar mixing loop.
AES (`Aes4R`/`Aes1R`) is used in **cache construction** (Argon2d fill + mixing)
and in **scratchpad fill** (`aes_hash.cpp`), not in `generate_dataset_item`. The
`dataset_seed_registers` + 8 superscalar rounds + cache-line XOR is exactly what
the spec requires. The "missing AES" clause was a conflation with the earlier
T-table bug (which was in the cache/scratchpad AES path, already fixed). It is
retracted and does NOT factor into the Tier 2 plan.

## Tier 1 (SHIPPED this session) — `wait_for_fill` dead-start
**What it does:** workers block in `worker_loop` on `PartialDataset::wait_for_fill()`
until the background fill completes, then mine at full speed immediately. No more
hashing on an empty dataset, no misleading cumulative-average ramp.

**Key correctness fixes (vs an earlier abandoned attempt):**
- `PartialDataset::fill_complete_` is now set by the **fill worker itself**
  (when the last chunk publishes `allocated_items_`), not only by
  `wait_for_fill()` polling `item_count_` — so the handshake fires reliably.
- `start_fill` is given **all cores** (`exclude_cores = {}`), because the mining
  workers are idle (blocked) during the fill and consume no CPU. Passing the
  miner cores as exclude left only 1 core → 1 fill thread → ~22-min dead-start
  (a bug caught on-device: first Tier 1 attempt showed 0.00 H/s for 240 s).
- `wait_for_fill()`'s fill-thread `join()` is guarded by a mutex so multiple
  concurrent worker callers don't race on `join()` of the same threads (UB).

**Verification (on-device, non-isolated device, 512 MiB / 7 workers):**
- `test_partial_dataset` + `test_mining` → ALL PASSED (regression).
- `--pool-test` fill markers: `started fill with 8 workers` → `fill complete —
  workers resuming` at t≈164 s. Post-fill **instantaneous** rate (snapshot-delta
  per 5 s window, not cumulative) jumps to **~30 H/s** at t≈185 s and holds
  28–31 H/s through t=400 s. The cumulative-average line still ramps (diluted by
  the dead-start) — that is expected; the instantaneous dump is now the honest
  number.

**Caveat:** without `isolcpus`, 7 workers spread across all 8 cores; 3 land on
the slow cluster (4–7) at ~1.6 H/s while 4 on the fast cluster (0–3) run ~2.9.
The *aggregate* instant rate was ~30 H/s. Under `isolcpus` (the user's 28.4
condition) the weak cluster isn't stolen by OS background work, so the per-worker
split is healthier. Tier 1's job — remove the ramp — is done regardless.

## Tier 2 — fill optimization (NOT started; scope agreed)
Goal: close the 164 s-vs-10 s fill gap to match XMRig's instant start.

**(a) Drop NEON from the fill interpreter (do FIRST — experimental control).**
The superscalar program is a sequential dependency chain; there is no
data-parallelism *within* an item to exploit, so the `vgetq_lane → scalar →
vcombine` dance is pure overhead. Replace `execute_superscalar_neon` in the fill
path with plain scalar GPR arithmetic. This gives the **scalar baseline** per item.

**(b) Vectorize *across* items (decide AFTER (a)).**
Process 2–4 independent items simultaneously, each in a separate NEON lane (items
have no inter-dependency). Then measure against the (a) baseline to see whether
the 2–4× parallelism is realized or eaten by memory bandwidth / register
pressure from juggling multiple item states. On this in-order A53, verify NEON
actually helps for interleaved independent items before committing (some
superscalar ops, e.g. `IMUL_R`/`IROR_C`, may still need per-lane extraction even
here). (a) alone may get most of the way to XMRig speed; (b) may be a smaller
marginal gain than expected.

**Explicit non-goal:** do NOT attempt to vectorize *within* a single item's
superscalar program — that chain is serialized by register dependencies and
cannot be parallelized. That path is a waste of effort.

## Next step decision (pending user)
- (x) Quick `--workers=4` fast-cluster (cores 0–3) sweet-spot test under
      `isolcpus` to establish the real per-core number with no weak-cluster drag.
- (a) Begin Tier 2 with the (a) drop-NEON fill baseline, measure, then decide
      on (b).
