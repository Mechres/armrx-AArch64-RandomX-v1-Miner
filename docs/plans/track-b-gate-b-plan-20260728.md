# Track B Gate B/C: Partial Dataset Memory-Contention Test — Plan for the Next Agent

## Status this plan assumes

Track B (hybrid partial dataset: cache a `B`-byte prefix of the fast-mode dataset,
hit it directly instead of deriving on the fly for any item within the cached
range) is the highest-expected-value item in the current backlog (+16-32%
*estimated*, not yet measured). Gate A (init cost) is done: a 512 MiB fill takes
~187s (~3.1 min) total (`cache_init_ms≈8000` + `fill_ms≈179393`), confirmed
genuinely 8-core-parallel once fill worker threads are explicitly pinned. Gate B
(memory contention under real 8-worker load) has not been measured — that's this
plan's job. Full background: `docs/plans/20260727/master-plan-20260727.md`,
Track B section (`## Track B`).

No code for this exists yet. You are building it, not retrying a prior attempt.

## Why Gate B is the whole risk of this item

This device's known 8-worker bottleneck is shared memory-path arbitration between
its two 4-core L2 clusters. Track B trades ALU work for random DRAM traffic
(~2.5M extra 64-byte reads/sec aggregate at 50% hit rate against a 512 MiB cache)
— exactly the contended resource. **A single-core `taskset` measurement will
systematically overstate the win** — this is the same class of mistake as the
historical false "PGO wins 2×" read on this device
(`bench_armrx needs core pinning` in memory / prior sessions), just on a
different axis (memory contention instead of core-cluster placement). The 8-worker
number is the only one that gets to decide whether this item ships.

## Implementation to build (concrete, from Opus's sketch in the master plan)

1. `PartialDataset`: an `mmap`/`MADV_HUGEPAGE` buffer of `N` items, filled via the
   existing `initialize_dataset()` (`src/dataset.cpp:66`, already NEON-vectorized
   and parallelized across `std::thread`s) — reuse verbatim with `item_count = N`.
   **Do not skip the affinity fix found during Gate A**: the fill worker threads
   must get explicit `pthread_setaffinity_np` pinning (mirroring
   `worker_loop()`'s own `AffinityMode::All` pattern at `src/mining_engine.cpp:318-335`),
   or under this device's `isolcpus=1-7` config the fill silently serializes onto
   one core — confirmed during Gate A to cost ~4-5× (835s vs. 179s observed).
2. JIT emission: in `randomx_program_aarch64_vm_instructions_end_light`
   (`src/jit_compiler_a64_static.S:528`), add a bound check before
   `bl rx_calc_dataset_item` — hit (item index < `N`) → direct load via
   `rx_program_xor_with_dataset_line`; miss → existing derivation, unchanged.
   ~4 extra instructions on a ~3,563-instruction path.
3. **Incremental fill from day one — not a later add-on.** A blocking
   implementation would stall mining for minutes at every seed rotation (a ~2.8-day
   cycle on this device). Mine in pure light mode immediately; let a background
   fill raise the cached-item bound as it progresses. The bound is re-readable at
   each of the JIT's 8 per-hash recompiles, so this costs nothing at runtime beyond
   the bound check already in step 2.
4. CLI: `--dataset-mb=N` (0 = off), plus an `auto` policy sized from
   `MemAvailable` (leave headroom — see Gate C below, no swap on this device).

## Correctness oracle (why this item is low risk despite being big-ticket)

This item is total and cheap to verify: `partial[i] == generate_dataset_item(cache, i)`
for every `i` in the cached range. Both branches of the resulting hybrid (fast
mode's direct load, light mode's on-the-fly derivation) are already independently
KAT-verified — this item only chooses between two already-proven computations, it
invents no new arithmetic. Write a differential test that fills a small partial
dataset (a few thousand items is enough) and checks every cached item against
`generate_dataset_item()` output, plus a handful of out-of-range indices to confirm
the miss path is untouched.

## Step-by-step plan

### Step 1 — Build the plumbing, verify correctness first, off the devbox where possible

- Implement `PartialDataset`, the bound check, and the CLI flag.
- Run the differential test above (host-side/x86_64 dev sandbox is fine for pure
  correctness — this doesn't need AArch64 hardware yet).
- Get `armrx_tests`/`test_jit_equivalence`/KATs green with `--dataset-mb` both at 0
  (fully off, must be bit-identical to today) and at some nonzero value, on
  whatever host builds this repo, before spending devbox time.

### Step 2 — Gate A re-confirmation with the real incremental-fill code path

- On the devbox, confirm the incremental fill actually behaves as designed: mining
  starts immediately in pure light mode, the cached bound visibly rises over the
  ~187s/512MiB fill window, and per-thread `psr` (via `/proc/<tid>/stat` field 39)
  shows the fill spread across all 8 cores, not serialized onto one — the exact
  check that caught the affinity bug during Gate A itself.

### Step 3 — Gate B: the actual memory-contention decision (this is the point of this plan)

- Measure hashrate with `--dataset-mb=0` (baseline) vs. a representative nonzero
  value (start with 512 MiB, matching the Gate A timing data) **at both 1 worker
  and 8 workers**, `taskset`-pinned, using `perf stat -e cycles,instructions` in
  addition to wall-clock (per the project's standing measurement discipline —
  wall-clock alone has already been shown insufficiently sensitive at comparable
  effect sizes on this device).
- **The 8-worker number decides. Full stop.** A 1-worker-only win with an 8-worker
  regression means Gate B has failed and this item does not ship as designed — do
  not rationalize a 1-worker win into a recommendation.
- Use the long measurement window (`--warmup=60 --seconds=180` or longer) — this
  device has a documented burst-vs-sustained gap (2.84 H/s at 15s/60s warmup vs.
  2.13 H/s at 60s/180s on the slow cluster). Reverse trial order at least once to
  rule out thermal drift (real, measured on this device).

### Step 4 — Gate C: memory pressure

- No swap on this device. Watch `MemAvailable` while mining (not idle) over a
  multi-hour run at whatever `--dataset-mb` value Step 3 lands on. Watch for
  OOM-killer activity (`dmesg`, `journalctl -k`). Size the `auto` policy
  conservatively — leave real headroom, don't chase the theoretical maximum.

### Step 5 — TLB check

- 512-768+ MiB of random access needs THP or this could become page-walk-dominated.
  Verify via `/proc/<pid>/smaps` that the partial dataset's mapping is actually
  THP-backed during a live run (the Argon2 cache huge-page precedent is good
  evidence THP works on this device, but the Argon2 working set is smaller than
  this one — don't assume it transfers without checking).

## If Gate B kills the idea

Don't discard the underlying hypothesis — promote the cheaper, narrower test of
the same "is the slow cluster's penalty interconnect-bound or controller-bound"
question instead: **per-cluster cache replication** (give cluster 1, cores 4-7,
its own physical 256 MiB Argon2 cache copy instead of one shared cache — see the
master plan's "Related but distinct" note under Track B, ~1-2 days, no
correctness risk since it's identical data just duplicated, ~12.5% estimated *if*
the penalty is interconnect-bound). Record the Gate B result either way in a new
`docs/experiments/` doc before moving on — a clean negative result here is exactly
as valuable to record as a positive one, per this project's established
discipline.

## Validation

1. Differential test (`partial[i] == generate_dataset_item(...)`) green.
2. Full `ctest` suite green, no regression in existing JIT/interpreter equivalence
   tests.
3. Gate B's 8-worker `perf stat` comparison, both trial orders, written up with
   real numbers (not estimates) before recommending ship/no-ship.

## Success criterion

A measured (not estimated) net hashrate improvement at 8 workers, sustained
window, with `MemAvailable` and THP backing confirmed stable over a multi-hour
run. If the 8-worker number doesn't show a net win, that's a valid, complete
result — write it up and stop; do not keep tuning `--dataset-mb` looking for a
sign flip without new evidence of what changed.
