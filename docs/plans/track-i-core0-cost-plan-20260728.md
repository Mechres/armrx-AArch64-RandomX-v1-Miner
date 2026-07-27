# Track I: Reduce Main-Thread Cost on Shared Core 0 — Plan for the Next Agent

## Status this plan assumes

Under `isolcpus=1-7`, `detect_core_order()` (`src/mining_engine.cpp:27`, with a
second definition at line 83 — check which one is actually compiled in for this
device's config before assuming either) has no `cpufreq` sysfs data to work from
and falls back to sequential `[0..7]` placement, landing worker 0 on core 0 — the
only core *not* isolated by the kernel cmdline, which also hosts the stratum
reader, JSON/job handling, and the per-second console/TUI render. This measurably
costs worker 0 real throughput under actual sustained pool mining (~24.76 H/s vs.
the ~28.4 H/s burst figure measured elsewhere). Full background:
`docs/plans/20260727/master-plan-20260727.md`, `## Track I`.

**Already confirmed wrong, don't re-try it**: removing worker 0 from core 0
entirely nets -0.6 H/s — the master plan already measured this, it is not an
option. This item is about reducing the *other* work sharing that core with
worker 0, not about relocating worker 0 itself.

This is **not a JIT change** — no correctness risk to the hash pipeline at all.
It can land independently, any time, in parallel with any of the other tracks.
Confirmed-open, i.e. this is not speculative — the cost is real and measured, the
fix just hasn't been designed or attempted yet.

## What actually shares core 0 with worker 0

From `src/miner_app.cpp`'s pool-mining loop (~line 355 onward): the stratum share
Submission logging, and a per-iteration status block (~lines 399-440) that either
renders a full TUI frame (`tui->render(snap)`, line 421) or prints a
`std::cout`-based one-line status update (lines 423-440+) — including reading
`armrx::max_cpu_temperature()` and iterating `engine.worker_hash_rate(w)` for
every configured worker. None of this currently has any affinity pinning or
throttling — it runs on whatever thread drives the main loop, which per the
above lands on core 0 alongside worker 0.

## Step-by-step plan

### Step 1 — Profile before designing anything

Do not skip straight to a fix. Spend real time (the master plan estimates about
an hour) profiling the main thread specifically under sustained pool mining:

- Confirm via `/proc/<tid>/stat` field 39 (`psr`) which threads actually land on
  core 0 during a real run — main thread, worker 0, and check whether metrics
  (`include/armrx/metrics.hpp`'s background thread) or TUI threads also land
  there. Don't assume the master plan's description is exhaustive; verify it
  live on this specific build/config.
- `perf record -p <main-thread-tid>` (or a system-wide `perf record` filtered to
  core 0) during sustained mining to see what the main thread is actually
  spending time on: JSON parsing, string formatting for the status line, TUI
  rendering, or something else entirely.
- Get a real per-second cost figure for the status/render path specifically —
  don't guess; time it directly (a simple high-resolution timer around the
  block at `src/miner_app.cpp:399-440` during a live run is enough).

### Step 2 — Candidate fixes, in order of how directly Step 1's data points to them

Only pursue the ones Step 1's profile actually implicates — this is explicitly
not a "try all of these" list:

- **Throttle the per-second render.** If the status/TUI block itself is
  measurably expensive per call, render less often (e.g. every 2-3 seconds)
  without changing the underlying hash-rate accounting, which should stay
  per-second internally regardless of render cadence.
- **Move JSON parsing off the critical path.** If stratum job/share JSON
  handling (`src/json.cpp`, `src/stratum_client.cpp`) is contributing measurable
  cost on core 0, consider whether it can run on a dedicated non-core-0 thread
  instead of inline on the main loop — but only if Step 1 shows this actually
  matters; don't restructure threading speculatively.
- **Check metrics/TUI thread placement.** If `MetricsExporter`'s background
  thread (`include/armrx/metrics.hpp`) or the TUI's own thread (if any) also land
  on core 0 without you having deliberately placed them there, that's likely an
  easy, low-risk win — pin them elsewhere explicitly, following the same
  `pthread_setaffinity_np` pattern `worker_loop()` already uses
  (`src/mining_engine.cpp:318-335`).

### Step 3 — Implement only the fix(es) Step 1's data actually supports

- Keep the change small and targeted at whatever Step 1 measured as the real
  cost — this is meant to be a quick, low-risk win, not a rearchitecture of the
  main loop.

## Validation

1. No correctness risk to the hash pipeline itself — existing KATs/`ctest` should
   be unaffected, but run the full suite anyway as a matter of course.
2. Re-measure worker 0's *sustained* hash rate specifically (long window, not
   burst — this device has a documented burst-vs-sustained gap, always use
   `--warmup=60 --seconds=180` or longer) before vs. after, `taskset`-pinned per
   the master plan's §4 discipline.
3. Confirm via `/proc/<tid>/stat` `psr` that the fix didn't inadvertently move
   worker 0 itself, or change which cores anything lands on beyond what was
   intended.

## Success criterion

A measured increase in worker 0's sustained hash rate under real, live pool
mining conditions (not a synthetic single-worker benchmark), with no change to
any other worker's throughput and no correctness regression. Given worker 0's
current ~24.76 H/s sustained figure sits well below the ~28.4 H/s burst number
partly attributable to this shared-core cost, even a partial recovery here is a
real, measurable aggregate win across the whole 8-worker deployment.
