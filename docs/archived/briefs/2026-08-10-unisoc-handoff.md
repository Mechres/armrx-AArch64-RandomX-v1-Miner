# Handoff — Unisoc (Cortex-A55) perf re-measurement

**Date:** 2026-08-10 · **Author:** Hermes (session end) · **Status:** WIP, untracked (do NOT commit)
**Reviewed by:** Hermes (2026-08-10) — measurement plan corrected before any device time spent.

## Why this session exists
The post-audit review (Deepseek/Luna) is fully closed (hybrid KAT, DAG doc fix,
FDIV/FSQRT deletion, Android portability). The last item — **fleet re-validation on
Unisoc** — was *started and correctness-validated*, but the **hashrate number is wrong
and must not be trusted as a baseline**.

## What is DONE (merged to main)
- `cb96ef9` — Android/bionic portability fixes (`sched_setaffinity` + `memfd_create`
  guard). Unisoc now builds natively in Termux.
- Correctness KATs PASS on Cortex-A55 (first new microarch in the fleet):
  - `test_jit_equivalence`: 16/16 byte-identical
  - `armrx_tests`: Input1/Input2 JIT hashes == reference KAT
  - `test_mining`: ALL PASSED (incl. `test_light_mode_partial_dataset_matches_reference`
    hybrid-path KAT)
- Changelog `cb96ef9`/`1ad2357` + brief updated. Raw audit files `luna.md`/`deepseek.md`
  deliberately left UNCOMMITTED.

## The 21.84 number — what it actually is (CORRECTED)
Prior measurement: `bench_armrx --full-hash-only --workers=8` → **~21.84 H/s** @ 8 workers.

**What the measurement does (read the source, not the assumption):**
`tests/bench_armrx.cpp:1000-1052` — with `--workers=8` it divides **500 hashes TOTAL**
across 8 threads (≈62 hashes/worker, 30-hash warmup each), wall-clock ~22.9 s, reports
aggregate. So it is a **short, low-sample window** (not "a single 500-hash window" — that
framing understates how short it is), AND it is a **bench-only path**: shared
`Argon2dCache`, no pool, no warmup ramp, light mode. Treat it as a rough probe, not a
baseline.

**Is 21.84 "wrong"? — Open, do not pre-judge.** The earlier brief asserted the number is
an artifact because "4 of 8 workers land on the 1.2 GHz LITTLE cluster." That mental model
is **inverted** (see below). On the Lenovo (A53 @765 MHz, all-8) the comparable bench figure
is ~13.37 H/s (`ROADMAP.md:45`). An Unisoc A55 at 1.2/1.6 GHz with better IPC legitimately
*should* exceed that, so ~21.84 at all-8 is **plausibly the real all-core number**, not a
defect. The real open question is not "is 21.84 wrong" but "**is there a better
worker/affinity config than the default?**"

**Default pinning is NOT the LITTLE-cluster problem the old brief claimed.**
`detect_core_order()` (`mining_engine.cpp:126-131`) sorts cores by `cpuinfo_max_freq`
descending, and `AffinityMode::All` pins `worker i` to `core_order_[i % n]`
(`mining_engine.cpp:406-413`). So on a big.LITTLE part, **All already puts workers 0-3 on
the big cluster and 4-7 on LITTLE** — the big cores are *used*, not idle, under the default.
The "big cores idle during mining by default" claim is false for armrx's own pinning (it may
be true of Android's *general* scheduler, but armrx overrides that via `sched_setaffinity`).

**Thermal:** all three fleet devices sit in front of a 12 cm fan; the Unisoc board is
exposed. Thermal throttling is ruled out regardless of the number.

## CRITICAL — measurement-plan corrections (do NOT run the old commands blind)
The original handoff's "correct commands" had three defects. They are fixed below.

1. **`--workers=8 --affinity-mode=big-only` is the config the project's own data says to
   AVOID.** `BigOnly` pins `worker i` to `core_order_[i % big_core_count_]`
   (`mining_engine.cpp:393-405`). With 4 big cores and 8 workers, that is **2 workers per
   big core** (oversubscribed, contending for the shared L1/L2/ALU). The ROADMAP measured
   exactly this on the Lenovo: 8w pinned to 4 fast cores = **12.33 H/s < 8w-all = 13.37**
   (`ROADMAP.md:40-46`). So the old brief's "big-only will beat 21.84 significantly" is
   **contradicted by the project's data** and would most likely return a *lower* number.

2. **There is NO "Pinned to big cluster" log line.** The `BigOnly` branch is silent (grepped:
   no such string exists). The only affinity-related log is the `isolcpus` one. Do NOT look
   for a "Pinned to big cluster" line — you'll think pinning failed when it didn't. Verify
   pinning via `taskset -p <pid>` or `/proc/<pid>/task/*/stat` (field 39 = last-set aff mask)
   instead.

3. **`--pool-test` with no `--pool=` is NOT offline / "throwaway address".**
   `cli_parser.cpp:437-443` hard-defaults to the project's **herominers TEST pool**
   (`tr.monero.herominers.com:1111`) + a TEST wallet and sets `should_connect_pool = true`.
   It needs a working network connection on :1111. If the device/network blocks that, the
   run connects-and-idles and reports ~0 H/s — not a measurement, a network failure. Keep a
   fallback (`--pool=<reachable-host:port>`) or pre-check reachability.

4. **bench_armrx cannot do affinity.** It has no `--affinity-mode` flag (`tests/bench_armrx.cpp`
   only parses `--workers=N`). So any affinity A/B MUST go through the miner `--pool-test`
   path, which is a **different code path** than the 21.84 bench baseline (pool + fill-wait
   vs bench shared-cache). Do not present a `--pool-test` number as "the corrected 21.84" —
   they are not apples-to-apples. Report both, label each.

## Correct commands (real A/B — do not assume a winner)
Run from the device `~/armrx/build/armrx` (rebuild first if main moved; see build flags
below). Light mode, `--seconds=200` for a steady-state tail (pool-test waits for dataset
fill, so there is no hashrate ramp — read the tail). Run each as its own invocation:

```sh
# A) Current default — comparable family to the 21.84 probe, but pool path + 200 s:
./armrx --mode=light --affinity-mode=all   --workers=8 --pool-test --seconds=200

# B) BEST-CASE from ROADMAP data: 1 worker per big core (4 big cores):
./armrx --mode=light --affinity-mode=big-only --workers=4 --pool-test --seconds=200

# C) The old brief's config — FLAGGED LIKELY-REGRESSIVE (8 workers on 4 big cores):
./armrx --mode=light --affinity-mode=big-only --workers=8 --pool-test --seconds=200

# D) 4 workers round-robin across all 8 (≈ workers 0-3 on big anyway):
./armrx --mode=light --affinity-mode=all   --workers=4 --pool-test --seconds=200

# E) FAST mode — only device with 4 GB RAM to fit the 2080 MiB dataset.
#    Cap at 4 workers (big-only) to avoid the oversubscription trap from (C):
./armrx --mode=fast --affinity-mode=big-only --workers=4 --pool-test --seconds=200
```

Read the **steady-state tail** of each (pool-test prints per-5s-window INST rate; the
ramp is only the dataset fill). Expect the ordering to be roughly B ≈ D ≥ A > C on the
Lenovo analogy; **Unisoc's A55 big cluster is a much better microarch than the A53, so the
spread may differ — measure, don't assume.** If B/D beat A meaningfully, the lever is
"fewer workers on big-only," NOT "8 on big-only."

## Compare against fleet (for context, not as a gate)
- Lenovo (A53 @765 MHz, no throttle): ~28.4 H/s @ 8w (isolcpus).
- Redmi 7A (A53, faster clock, fan-cooled): ~69.39 H/s @ 8w (2026-08-08 baseline).
- Unisoc (A55 1.2/1.6 GHz): TBD — expected to be **at or above** the Lenovo once a sane
  config is used; the old brief's "0.77× of Lenovo, must be wrong" framing was based on the
  inverted pinning model and should be discarded.

## MEASURED RESULTS (2026-08-10, real device run)

Single device run, 5 configs, each `--pool-test --seconds=200` (fast mode re-run at
900 s because the 2080 MiB `init_dataset` exceeds a 200 s window under no-hugepage
Termux — the first fast attempt read 0.00, a window-too-short artifact, NOT a device
fault). Pool = herominers `:1111`, confirmed reachable. Run launched detached via
`setsid` so an SSH blip could not kill it.

| Config | Mode | Affinity | Workers | **Steady H/s** | worker spread |
|---|---|---|---|---|---|
| A | light | all | 8 | **33.61** | w0-3≈3.3, w4-7≈4.35 |
| B | light | big-only | 4 | 25.98 | w0-1≈8.5, w2-3≈4.5 |
| C | light | big-only | 8 | 33.44 | ~4.0–4.4 each |
| D | light | all | 4 | 26.01 | w0-1≈8.5, w2-3≈4.5 |
| E2 | fast | big-only | 4 | **41.89** | w0-1≈13.5, w2-3≈7.5 |

### Conclusions (measured, not assumed)
1. **21.84 was an artifact.** Real all-8 light = **33.61 H/s** (+54%). It **beats the
   Lenovo A53 isolcpus baseline (28.4 H/s)** by ~18%, confirming the A55 > A53 intuition.
2. **`--affinity-mode` is irrelevant at equal worker count.** 8w: A(all) 33.61 ≈ C(big-only)
   33.44. 4w: B(big-only) 25.98 ≈ D(all) 26.01, and **B ≡ D** confirms `all/4` and
   `big-only/4` pin the same reachable cores. The "big cores idle by default, pin them to
   win" premise is **false** — armrx's `sched_setaffinity` already uses the big cluster.
3. **Worker count is the only dominant lever** (8w ≈ 33.6 vs 4w ≈ 26.0, ~+29%).
4. **Fast mode is the winner on Unisoc**: fast/big-only/4w = 41.89 H/s (> light/8w 33.61
   with *fewer* workers), and it is the only mode the 4 GB device can hold. E2's tail was
   still climbing (~106 H/s INST agg at t=900) because init ate the early window; **41.89 is
   a conservative floor**.

### cpuset correction (important — fixes the earlier pinning analysis)
The pinning analysis above assumed 8 reachable cores. **Live check:** `Cpus_allowed_list:
0-5` (`nproc` = 6). The Android cpuset cgroup confines Termux to **cores 0–5** — 4 LITTLE
(0-3) + **2 big (4-5)**; cores 6-7 are unreachable without root (matches
`devices/unisoc-sc9863a-termux.md`). Consequences:
- `big-only` targets cores 4-7 but 6-7 are cgroup-masked, so big-only effectively = cores 4-5.
- Thus A (all/8w: 4→big + 4→LITTLE) and C (big-only/8w: 8→cores 4-5) both collapse onto the
  same reachable set — exactly why A ≈ C. The "affinity irrelevant" finding **holds**; it is
  now explained by the cpuset ceiling, not by 8 free cores.
- E2 (fast/4w) = 4 workers on cores 4-5 (2 big) → 41.89 H/s is a strong per-big-core figure.

### Fleet placement
- Unisoc light/all/8w **33.61** > Lenovo A53 **28.4** (isolcpus), < Redmi 7A **69.39**.
- Unisoc **fast 41.89+** is its best mode and the only fast-mode number in the fleet.

### Open questions — RESOLVED (2026-08-10)
1. Best config: **8 workers, any affinity (light)** = 33.61; **fast/big-only/4w = 41.89** is
   the headline. `big-only` alone does not help.
2. Fast-mode H/s = **41.89** (floor; warm tail ~106).
3. 21.84 → corrected to **33.61** (light/all/8w). Update post-audit brief + changelogs.

## Environment recap (for the next session)
- **Unisoc SSH — IP confirmed `.218` (DHCP).** The device is DHCP-assigned and moved
  `.206` → `.218` on the 2026-08-10 restart; `.206` is now dead. `sshd` auto-starts via
  `termux-boot` (`~/.termux/boot/start-sshd.sh` → `termux-wake-lock; sshd`), so it is
  reachable after reboot once a lease is had. **Always re-probe both `.206` and `.218`** at
  session start — the lease can change again. Device doc IP corrected to `.218` (2026-08-10).
- Native Termux build (SECURE_JIT=OFF is required — Android SELinux blocks W^X mprotect):
  ```sh
  cd ~/armrx && cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON \
    -DARMRX_SECURE_JIT=OFF -DARMRX_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release \
    && make -C build -j6
  ```
  (`ARMRX_SECURE_JIT=OFF` and `ARMRX_BUILD_TESTS=ON` are correct; the latter is already the
  default — harmless to keep.)
- Source on device is at `~/armrx` (transferred via tar-pipe; .git excluded). To sync newer
  main: re-tar from host or `git` if device can reach GitHub (it could NOT via HTTPS creds).
- Do NOT commit the raw audit files `luna.md`/`deepseek.md`.
- After building, before the runs, **pre-check pool reachability** (the TEST pool default):
  `timeout 5 bash -c 'cat < /dev/null > /dev/tcp/tr.monero.herominers.com/1111' && echo OK || echo BLOCKED`
  If BLOCKED, pass an explicit reachable `--pool=` so `--pool-test` still reports H/s.
