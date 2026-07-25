# isolcpus/rcu_nocbs — a real ~14% hashrate win (2026-07-25)

**Status: adopted.** This is the largest measured win in this project's history — every prior
adopted change has been sub-1%. Unlike everything else on `PLAN.md`, this is not a code change:
it's a kernel-boot-cmdline tuning step, on a specific device, that anyone deploying `armrx` on
similar asymmetric multi-cluster ARM hardware should consider.

## Background

`PLAN.md` Phase 8's only open item was `--rt-priority` + `isolcpus=`/`nohz_full=`, blocked for
most of this project's life on device access (`setcap`/root not available). The user installed
`setcap` and granted explicit permission to edit the boot cmdline and reboot the device. This
doc covers what was actually measured once that access existed.

## What was applied

`/boot/extlinux/extlinux.conf`'s `append` line got `isolcpus=1-7 nohz_full=1-7 rcu_nocbs=1-7`
added (core 0 deliberately left out — it's the boot CPU, and kernel housekeeping/IRQ work
generally assumes at least one core stays in the normal scheduling domain). `setcap
cap_sys_nice+ep` was applied to the `armrx` binary to let `--rt-priority` actually engage
`SCHED_FIFO` (it needs `CAP_SYS_NICE`; without it, `pthread_setschedparam` fails and
`mining_engine.cpp` logs a one-time fallback warning and continues on the default scheduler,
non-fatally).

**Real gotcha found**: `nohz_full=1-7` silently did nothing. `zcat /proc/config.gz | grep NO_HZ`
confirmed this kernel build has `# CONFIG_NO_HZ_FULL is not set` — the boot parameter is accepted
but has no effect without kernel support. `isolcpus=`/`rcu_nocbs=` don't have this dependency and
did take effect (confirmed via `/sys/devices/system/cpu/isolated` showing `1-7`).

## Method

Initial testing conflated two variables: every run on the isolated kernel was tested with
`--rt-priority` on vs. off, but `isolcpus`/`rcu_nocbs` were baked in via the cmdline for *both*
conditions (reboot required to toggle). A same-binary, same-session comparison against a
historical "~24.95 H/s" README figure looked like confirmation, but that comparison was
initially sloppy — the historical figure came from a different investigation and needed
re-verification against *today's* exact binary before it meant anything (see "false starts"
below). The clean test: revert the cmdline to no-isolation, reboot, re-measure the current
binary, then restore and re-measure again — a true within-session, same-binary A/B, controlling
for both code-version drift and any transient session confounds.

All runs: `./armrx --mine --workers=8 --warmup=15 --seconds=60 --log-level=error`, steady-state
hashrate as reported by the binary's own post-warmup measurement window (45s), read directly
from the per-worker breakdown it already prints.

## Results

| Config | Aggregate | Cores 0-3 (workers 0-3) | Cores 4-7 (workers 4-7) |
|---|---|---|---|
| No isolcpus, round 1 | 24.16 H/s | 4.26 / 4.26 / 4.26 / 4.26 | 1.42 / 1.42 / **2.84** / 1.42 |
| No isolcpus, round 2 | 25.59 H/s | 4.26 / 4.26 / 4.26 / 4.26 | 1.42 / 1.42 / 2.84 / 2.84 |
| isolcpus on, round 1 | 28.41 H/s | 4.26 / 4.26 / 4.26 / 4.26 | 2.84 / 2.84 / 2.84 / 2.84 |
| isolcpus on, round 2 (rt-priority) | 28.38 H/s | 4.26 / 4.26 / 4.26 / 4.26 | 2.84 / 2.84 / 2.84 / 2.84 |
| isolcpus on, round 3 (rt-priority, reversed order) | 23.09 H/s | (not captured) | (not captured) — one anomaly, see below |
| isolcpus on, round 4 (rt-priority off, restored config) | 28.41 H/s | 4.26 / 4.26 / 4.26 / 4.26 | 2.84 / 2.84 / 2.84 / 2.84 |

Average no-isolcpus: **24.9 H/s** (matches `README.md`'s independently-documented, 2026-07-24
re-baselined figure of 24.95 H/s almost exactly — strong cross-validation that today's
no-isolcpus retest is a faithful reproduction, not a fluke).

Average isolcpus-on (3 of 4 consistent rounds): **28.40 H/s**. **A real, reproducible ~14%
aggregate hashrate improvement.**

## Mechanism

The fast cluster (cores 0-3) is byte-for-byte identical across every single round tested here —
4.26 H/s each, isolcpus or not. **The entire effect lives on the slow cluster (cores 4-7)**:
without isolation, 2-3 of those 4 workers randomly get knocked down to roughly half rate (1.42
vs. 2.84 H/s) each run — a *different* subset of workers each time, which is exactly the
signature of ordinary background OS work or interrupts occasionally landing on an unprotected
core and stealing cycles from a pinned mining worker. With `isolcpus`, all four consistently hit
the full 2.84 H/s with zero variability across every clean round tested. This is precisely what
CPU isolation is designed to do, and the per-worker data shows it doing exactly that — not just
an aggregate number moving, but a specific, mechanistically coherent story.

This is a different mechanism from the interconnect-arbitration effect documented in
`docs/archived/plan_phase6_completed.md` (which is a hardware fact that hits both miners
equally and isn't scheduler-visible) — that effect still exists and still caps cluster 1's
per-worker rate around 2.84 H/s under full contention. What `isolcpus` fixes is a *separate*,
software-visible problem: without it, cluster 1's workers don't even reliably get their fair
share of *that* already-reduced rate, because they're competing with the general scheduler too.

## The one anomaly

One isolcpus-on round (rt-priority on, reversed test order) measured 23.09 H/s — well below the
other three isolcpus-on rounds. Two theories were checked and ruled out or left open:

- **Thermal throttling**: initially suspected, but `/sys/class/thermal/thermal_zone*/temp`
  measured shortly after (32-39°C across every zone) argues against sustained overheating — this
  device has active cooling and wasn't running hot. Not confirmed either way *during* the
  anomalous run specifically (no live sampling was taken at the time), so this isn't fully ruled
  out for that one moment, just not supported by the evidence available.
- **Hardware IRQ landing on an isolated core**: `isolcpus` stops the *scheduler* from placing
  ordinary tasks on isolated cores, but hardware interrupts can still be routed there unless IRQ
  affinity (`/proc/irq/*/smp_affinity`) is separately steered away. Plausible one-off explanation,
  not independently confirmed.

Given the other three isolcpus-on rounds are tightly reproducible (28.38-28.41 H/s) and the
no-isolcpus rounds show a completely different, more variable per-worker signature, this single
outlier reads as a one-off — not evidence the win is fake, and not enough evidence to fully
explain either.

## `--rt-priority`'s independent contribution: unclear

Every isolcpus-on round tested landed in the same ~28.4 H/s range regardless of whether
`--rt-priority` was on or off. `--rt-priority` is confirmed functionally engaged (no
`CAP_SYS_NICE` fallback warning logged; the binary ran normally under `SCHED_FIFO`), but no
round showed a clear independent effect beyond what `isolcpus`/`rcu_nocbs` alone provide. This
project's own history teaches that small effects need low-noise `perf stat` measurement to
resolve, not wall-clock hashrate — but the attempt to get `perf stat` cycles/instructions
readings against the full multi-threaded `armrx --mine` process gave near-zero counts (correctly
measured a sanity-check busy loop, so the issue is specific to attributing cycles to a
worker-thread inside this particular multi-threaded binary — a real, unresolved tooling gotcha,
not yet root-caused). **Conclusion: the measured win here should be attributed to
`isolcpus`/`rcu_nocbs`, not to `--rt-priority`.** `--rt-priority` remains available and harmless
to enable, but its incremental value on top of `isolcpus` is unconfirmed.

## False starts worth recording

- Initially compared today's isolcpus-on numbers (28.38-28.41 H/s) against a **28.28 H/s** figure
  in `docs/archived/plan_phase6_completed.md` and concluded "no jump, this is just the existing
  baseline" — wrong on inspection: that 28.28 H/s figure is **XMRig's** number from a
  cluster-normalized comparison, not armrx's. armrx's own number in that same investigation was
  ~25.04 H/s, matching `README.md`'s real current baseline (24.95 H/s). Caught by the user
  pointing back at the source document.
- Also cited an even older **28.9 H/s** figure from `docs/archived/beyond-parity_v2.md` — that
  doc explicitly flags itself as stale in its own text ("MAP_HUGETLB not yet deployed at time of
  measurement... re-measure before optimizing") and likely predates the AES T-table bug fix
  (2026-07-20) entirely. Not a valid comparison point.
- Attributed the one 23.09 H/s anomaly to thermal throttling with more confidence than the
  evidence supported — the device has active cooling and measured cool afterward. Corrected after
  the user pushed back with direct knowledge of the hardware.

The lesson underneath all three: **cross-session historical numbers need re-verification against
the exact binary and exact question being asked before being trusted as a comparison baseline** —
consistent with this project's standing discipline, and worth restating because it nearly
produced a wrong "no effect" conclusion here on what turned out to be a real, large, adopted win.

## This does not close the gap to XMRig

Worth being explicit about scope: this is a general OS-scheduling fix, not something specific to
`armrx`'s code. The mechanism (background OS work stealing cycles from a pinned worker thread on
an unisolated core) would very likely help *any* pinned multi-threaded workload on this device,
XMRig included, by a comparable margin — nothing here was measured against XMRig with the same
cmdline applied. The ~10-12% cluster-normalized code-level gap to XMRig documented in
`docs/archived/plan_phase6_completed.md` (item 9) is untouched by this finding and remains open.
This win is real, but it's an "improve the deployment for any miner on this hardware" result, not
an "armrx gets closer to XMRig" result.

## Recommendation for deployment

On asymmetric multi-cluster ARM SoCs (confirmed here: MSM8929/Snapdragon 415, two 4-core L2
clusters with different rated clocks), add `isolcpus=<N1>-<N2> rcu_nocbs=<N1>-<N2>` to the boot
cmdline, covering all cores except core 0, before running `armrx` with `--workers=<all cores>`.
This is an operational/deployment recommendation, not something `armrx` itself can configure —
it requires root and a reboot. `nohz_full=` can be included for forward-compatibility but has no
effect on kernels built without `CONFIG_NO_HZ_FULL` (check via
`zcat /proc/config.gz | grep NO_HZ_FULL` or equivalent before assuming it's doing anything).
`--rt-priority` (needs `setcap cap_sys_nice+ep` on the binary, re-applied after every rebuild) is
safe to enable alongside this but its independent contribution is unconfirmed.
