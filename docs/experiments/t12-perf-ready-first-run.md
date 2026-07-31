# T1-2 First Steady-State perf stat Run — Device-Verified (2026-08-01)

The `--perf-ready` hook was implemented and host-verified earlier; on-device
verification was blocked by the Track C / GCC-11 JIT hang. After the GCC-16
cross-toolchain upgrade (both JIT stress suites now byte-identical on device),
the first clean steady-state measurement ran successfully.

## How it ran

- `tools/perf_ready_bench.sh` logic, device-adapted (`/tmp/cross/perf_ready_bench_device.sh`):
  binary path, PERF_READY wait 100s → 1200s, `--perf-ready-timeout=900`, pinned to
  fast-cluster core 3 (`taskset -c 3`, isolcpus=1-7 active).
- Command: `bench_armrx --full-hash-only --perf-ready`
- Flow: 256 MiB Argon2 cache init → 30-hash warmup → `PERF_READY` printed →
  perf attaches (`-p`) → `go` line on FIFO → 500 measured hashes.
- Binary: cross-built GCC 16.1.0 (commit 3563047), deployed to `/tmp/cross/`.

## Raw output

```
 Performance counter stats for process id '18909':

       80425068854      cycles:u
       59478960617      instructions:u
         393637977      branches:u
          12262948      branch-misses:u

     105.132056588 seconds time elapsed

PERF_READY
full hash (light, JIT)                       207001.80 μs median      4.83 hash/s
  min:       203732.14 μs
  1st pctl:  204577.92 μs
  5th pctl:  205202.61 μs
  25th pctl: 206303.99 μs
  50th pctl: 206999.87 μs (median)
  75th pctl: 207880.22 μs
  95th pctl: 215792.86 μs
  99th pctl: 222555.80 μs
  max:       226816.32 μs
  mean:      207788.10 μs
```

## Derived metrics (500-hash steady-state window)

| Metric | Value |
|---|---|
| Hash rate (perf window, 500 / 105.13s) | 4.76 H/s |
| Hash rate (bench median) | 4.83 H/s |
| Median hash time | 207.0 ms |
| Cycles / hash | 160.9 M |
| Instructions / hash | 119.0 M |
| **IPC** | **0.740** |
| Branches / hash | 787 k |
| Branch miss rate | 3.11% |
| Effective core frequency | 765 MHz (of 1.1 GHz max) |
| p50 → p99 spread | +7.5% (tight distribution) |

## Notes / caveats

1. **IPC 0.740 is consistent with the retrospective's cluster-normalized 0.731**
   — the measurement methodology now matches, closing the master-plan 2.77× pitfall.
2. **Core ran at 765 MHz — that is the device's FIXED clock.** Correction to
   the earlier "governor" claim: MSM8929 on this postmarketOS kernel has NO
   cpufreq support — `CONFIG_CPUFREQ_DT=y` is built in, but the device tree
   contains no CPU OPP table (`operating-points-v2` absent from all cpu nodes),
   so no cpufreq policies exist and `scaling_governor` does not exist under
   `/sys/devices/system/cpu/cpu*/`. The CPU is fixed at the firmware-set
   frequency (perf-derived 765 MHz; not thermal — 36°C idle, 48-50°C under
   load, far below A53 throttle). ~1.1 GHz is NOT achievable on this kernel
   without a DT/kernel change (add MSM8929 OPP table + reflash) — out of scope.
   This number (207 ms/hash @ 765 MHz) is therefore the device's true
   steady-state single-core figure, and the 7-worker ~18 H/s mining rate is
   consistent with it (3 fast + 4 weak-cluster cores, ~50% throughput).
3. First clean **light-mode, single-core** steady-state number on MSM8929.
   The earlier "248 ms/hash" figure predates this methodology (and the JIT hang).
4. Cross-built (GCC 16) binary — identical results expected from device-native
   builds (JIT byte-identical across both toolchains, verified same day).
