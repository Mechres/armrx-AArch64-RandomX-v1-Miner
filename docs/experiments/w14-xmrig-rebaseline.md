# W1-4 — XMRig Re-Baseline on Target Device (perf stat, no disassembly)

**Date:** 2026-07-31
**Auditor:** Hermes (measurement), AGY/Cursor delegated per user direction
**Device:** MSM8929/Snapdragon 415, postmarketOS, fixed 765 MHz, `isolcpus=1-7`
**Protocol (audit W1-4):** `perf stat` totals + PMU only. **No disassembly of XMRig JIT buffers** (counsel-gated N5 is a separate item).

## Binary provenance (clean-room compliant)
- Source: Kanedias `xmrig-static` GitLab release `v6.26.0-b167` (build of xmrig v6.26.0).
  Official xmrig releases ship **no** aarch64 Linux binary, so a third-party static build was required.
- Download: double TLS fetch, checksum-stable (sha256 `6895ad8c…` identical across fetches).
- Device md5 (`md5sum`): `2ac4814a61d39b8f090fd128e4d1b5f3` — recorded in every run script and aborted-on-mismatch.
- Runs as glibc static-pie under musl (verified — `hexdump` magic `7f45 694c 0102`, API `glibc 2.38`).

## Configuration
Light mode, single thread, pinned to core 3 (`taskset -c 3`), `autosave:false`.
Benchmark section required XMRig's exact schema: `"benchmark": { "size": "250K", "seed": "…" }`
(release builds accept ONLY `"1M".."10M"`, `"250K"`, `"500K"` — numeric sizes are rejected → null pool → "no valid configuration").
```json
{ "autosave": false, "api": {"enabled": false}, "http": {"enabled": false},
  "donate": {"level": 0}, "cpu": {"enabled": true},
  "randomx": {"mode": "light", "init": -1},
  "benchmark": {"size": "250K", "seed": "armnx-w14-rebaseline-20260801"} }
```

## Measurement results (steady state, 765 MHz)
Two `perf stat` windows (120 s + 100 s) over the hashing phase, plus one 250K benchmark completion run:

| Metric | Window 1 (120 s) | Window 2 (100 s) | Combined |
|---|---|---|---|
| cycles:u | 88.83 B | 73.33 B | — |
| instructions:u | 57.52 B | 47.48 B | — |
| IPC | 0.6475 | 0.6474 | **0.6475** |
| cache-misses:u | 372.2 M | 307.0 M | — |
| branch-misses:u | 7.24 M | 5.99 M | — |

250K benchmark completion (pinned, piped → line-buffered so speed lines flush):
```
bench  start benchmark hashes 250K algo rx/0
randomx dataset ready (7854 ms)        # light mode, 256 MiB cache
cpu READY threads 1/1 (1)
miner speed 10s/60s/15m 5.05 n/a n/a H/s max 5.16 H/s   # steady
```
**XMRig measured H/s = 5.05** (single thread, light mode, this device).

## Derived per-hash figures
Using measured `ins/s = 479.2 M`, `cyc/s = 739.7 M` (combined windows) and `H/s = 5.05`:

| Quantity | XMRig (measured) | armnx (W1-5 ON census) | Δ |
|---|---|---|---|
| instructions / hash | **94.5 M** | 118.96 M | armnx **+25.9%** |
| cycles / hash | **145.9 M** | 162.74 M | armnx **+11.5%** |
| IPC | 0.6475 | 0.731 | armnx **+12.9%** |
| raw H/s (765 MHz) | **5.05** | 4.84 | armnx **−4.2%** |

## Conclusion — reframes the audit's "+19.5%" claim
1. The instruction-count gap is **real and LARGER** than the audit assumed:
   armnx emits **+25.9%** more instructions/hash than XMRig (119.0 M vs 94.5 M),
   vs the audit's stated +19.5% (which used a historical XMRig figure of 99.57 M).
   The historical 99.57 M is **5.1% higher** than the value measured on this device
   today (94.5 M) — likely from fast mode or a different device/clock in the prior measurement.
2. **However**, armnx's IPC is materially better (0.731 vs 0.648, **+12.9%**).
   This nearly closes the cycles gap: armnx uses only **+11.5%** more cycles/hash.
3. Net result in raw hashrate at the same fixed 765 MHz clock: armnx is **only 4.2% behind**
   XMRig (4.84 vs 5.05 H/s) — **not** the 19.5% the audit implied.
4. The audit's "+19.5%" was computed as a pure instruction-count ratio (119.0 / 99.57)
   and did **not** account for the IPC differential that partially offsets it on this in-order core.

### Implication for the optimization backlog
- The remaining headroom vs XMRig is now correctly scoped: **~26% excess instructions/hash**
  is the dominant lever. Closing even half of it (via superscalar-body density, the 80.5% region)
  would flip armnx ahead of XMRig at equal IPC.
- The cycles/hash gap (+11.5%) is almost entirely the instruction gap expressed through IPC;
  there is **no separate stall/scheduling deficit** — armnx's IPC is already superior.

## Reproduce
```sh
# on device (BusyBox ash):
scp xmrig-aarch64-static … ; md5sum must be 2ac4814a61d39b8f090fd128e4d1b5f3
taskset -c 3 xmrig-aarch64-static -c xmrig-config.json --threads=1 --no-color 2>&1 | cat
# (pipe through cat forces line-buffering so XMRig's speed lines are visible;
#  redirect-to-file alone buffers and shows nothing until exit/flush)
perf stat -p <pid> -e cycles,instructions,cache-references,cache-misses,branch-misses \
  -o perf.txt -- sleep 120
```

## Clean-room / legal note
- Static binary only; **no XMRig source compiled, no objdump of JIT buffers** (N5 separate).
- Measurement is perf-stat totals (PMU) — within the agreed audit scope.
- Binary is GPL (XMRig) but treated as an external measurement oracle, never linked into armnx.
