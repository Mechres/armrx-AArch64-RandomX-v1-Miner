# Partial-dataset optimization hypotheses

Date: 2026-09-12. These are proposed experiments, not measured improvements.
The strongest practical lead is the existing light-mode partial dataset:
the repository reports roughly 30–32 H/s with 512 MiB versus 26 H/s without it.
Use `docs/closed-levers-ledger.md` to reconcile historical results.

## 1. Choose capacity by useful hashes, not peak hashrate

Measure 128/256/512/768 MiB prefixes where RAM permits, including cache,
scratchpads, OS reserve, fill time, and steady-state throughput. Select capacity
at startup from available memory and expected session duration; avoid resizing
during mining in the first design. Larger prefixes save derivation work but
cost more startup time and memory. No larger-capacity gain is established.

For a session of T seconds and fill time F, compare H × max(0, T − F),
using the same timing origin and including other startup costs consistently.
The approximate break-even against immediate light mining is
T > H_partial × F / (H_partial − H_light), when H_partial > H_light.
This differs from the closed hash-during-fill experiments: miners still wait.

## 2. Reuse a validated prefix across process restarts

Explore an optional local snapshot keyed by the exact seed, algorithm/version,
and item count. Load it only after validating its format, size, and integrity;
use atomic publication and reject incomplete snapshots. Benchmark loading plus
validation against regeneration. This targets startup losses, not steady-state
H/s. Storage bandwidth, flash wear, and cache trust may make it unattractive.
Do not trust a self-reported checksum from an untrusted snapshot producer.

## 3. Explicit hugepages for the partial prefix

The prefix currently uses anonymous mmap plus MADV_HUGEPAGE. Compare an optional
MAP_HUGETLB allocation with graceful fallback, matching the memory layer's
existing approach. Verify actual page sizes and measure fill time, dTLB events,
and H/s at equal capacity. Prior light-cache hugepage results do not establish
a benefit for the larger partial prefix. Never reserve pages automatically.

## 4. Amortize the hybrid hit check across dataset-item calls

Inspect whether a fully filled, generation-stable prefix allows the JIT to
specialize its bound/pointer setup once per program or hash. Preserve exact
dataset offsets and the miss path. Count instructions before implementing:
drop this idea if setup is already hoisted or the maximum savings are trivial.
This is distinct from reopening the previously failed inline-hit design without
new evidence; any prototype needs a documented structural difference.

## Evaluation gates

First ensure seed rotation cannot overwrite data still used by mining workers.
Require reference hashes and JIT/interpreter equivalence for every candidate.
Use pinned, thermally settled, reversed-order A/B runs at one and eight workers;
report both startup-inclusive useful hashes and steady-state H/s. Keep memory,
seed, workload, and compiler identical. Do not revive concurrent fill/mining,
generator timing changes, or memory-op scheduling on speculation alone.
