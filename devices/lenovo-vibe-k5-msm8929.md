# Lenovo Vibe K5 (`wt82918hd`)

MSM8929 device running postmarketOS. This has been the primary `armrx`
benchmark host, and most historical measurements in this repository were taken
here.

Its defining characteristic is that **it has no frequency scaling at all** — the
CPU runs at a fixed firmware-set clock. That makes it a stable measurement
platform, but also means results cannot be clock-normalised against devices that
do scale.

All values below were read from the running device on 2026-08-08.

## Identity

| | |
|---|---|
| Model | `Lenovo Vibe K5 (HD) (Wingtech WT82918)` |
| DT compatible | `wingtech,wt82918hd qcom,msm8929` |
| SoC | Qualcomm MSM8929 (Snapdragon 415) |
| Kernel | `6.12.1-msm8916` (mainline) |
| Distro | postmarketOS edge, BusyBox `ash`, musl |

**The SoC is MSM8929, not MSM8916**, despite the kernel flavour name. It has two
4-core L2 clusters.

## CPU topology

8× Cortex-A53 in two clusters of four.

| Cluster | Cores | Notes |
|---|---|---|
| Fast | 0–3 | full throughput |
| Weak | 4–7 | **~50 % throughput** under full contention |

The ~50 % penalty on cores 4–7 is **not** a frequency difference — both clusters
run the same clock. It is interconnect arbitration between the two L2 clusters.
This was measured, not inferred.

Unlike the Redmi 7A, core numbering here **does** follow the cluster boundary.

## Frequency scaling

**None.** There is no cpufreq at all:

```console
$ ls /sys/devices/system/cpu/cpu0/cpufreq/ | wc -l
0
```

The kernel has `CONFIG_CPUFREQ_DT=y`, but the device tree carries **no CPU OPP
table** (`operating-points-v2` is absent), so no policies are created and there
is no `scaling_governor` anywhere under `/sys/devices/system/cpu/`.

The CPU runs at a fixed firmware-set **~765 MHz** (derived from `perf`
measurements). This is not thermal — idle sits at ~35 °C and load only reaches
~48–50 °C.

**~1.1 GHz is not reachable** without a device-tree/kernel change and a reflash.
Governor or `cpupower` suggestions do not apply to this device. Treat 765 MHz as
the real clock for all rate arithmetic.

## Thermal

Idle readings:

| Zone | Idle |
|---|---|
| `cpu0-thermal` | 35 °C |
| `cpu1-thermal` | 35 °C |
| `modem1-thermal` | 35 °C |
| `gpu-thermal` | 34 °C |
| `camera-thermal` | 33 °C |
| `modem2-thermal` | 32 °C |

Load reaches roughly 48–50 °C. Because there is no DVFS, the device **cannot
throttle by dropping frequency** — thermal behaviour is therefore much simpler to
reason about than on the Redmi 7A.

The unit runs as an **open motherboard cooled by a 120 mm fan**, which is why
temperatures stay low.

## Memory

```
Mem:  1884 MiB total,  ~1505 MiB available
```

Below the 2080 MiB fast-mode dataset threshold, so armrx runs in
**light mode (256 MiB cache)**.

## Power

`/sys/class/power_supply/` contains `pm8916-bms-vm`.

## Access

```sh
ssh mechres@192.168.10.156
```

## Gotchas

### `isolcpus` — large but non-portable win

Adding `isolcpus=1-7 rcu_nocbs=1-7` to the kernel boot cmdline (leaving core 0
for housekeeping) measured **~28.4 H/s vs ~24.9 H/s** at 8 workers — about 14 %,
the largest single improvement recorded on this device.

Mechanism: without isolation, 2–3 of the four weak-cluster workers randomly lose
half their throughput to background OS work on each run. Isolation prevents it.

Two important caveats:

- This is a **deployment/OS change, not an `armrx` code lever.** It does not ship
  in the miner and must not appear in the optimisation ledger as a project win.
- **Hashrate figures are only comparable within the same isolation state.**
  Numbers in the 21–30 H/s range taken *without* `isolcpus` cannot be compared
  against the 28.4 H/s figure taken *with* it.

`nohz_full=1-7` silently no-ops on this kernel (`CONFIG_NO_HZ_FULL` is unset).

At time of writing `isolcpus` is **off** on this device
(`/sys/devices/system/cpu/isolated` is empty).

### Build discipline

On-device builds must be pinned and limited:

```sh
ssh mechres@192.168.10.156 \
  "cd ~/armrx/build && nohup taskset -c 1-3 cmake --build . -j2 > /tmp/build.log 2>&1 &"
```

- Cores `1-3` = fast cluster minus the housekeeping core. **Never use `1-7`** —
  cores 4–7 are the weak cluster and `-j2` can land both jobs there.
- `-j2` is **required**: 1.4 GiB RAM plus GCC LTO means `-j7` swaps and can hard-lock
  the device.
- Check for stale processes first:
  `ps -eo pid,comm | grep -E 'armr[x]|cmak[e]|gmak[e]|cc1plu[s]' | grep -v grep`

Cross-compiling from the host is roughly **10× faster** (~3 min vs ~40 min) and
is the preferred iteration path.

### CTest `BAD_COMMAND` flake

An intermittent CTest path-resolution issue occurs on-device. Treat
`BAD_COMMAND` "failures" as a flake rather than a real regression — re-run the
named tests, or execute the binaries directly from `build/`.

### Environment

BusyBox `ash`, **no bash**. `pgrep` can hang, producing a false "stuck process"
reading — poll with `ps -eo comm | grep` instead. When launching a detached
process, do it in its own SSH invocation, confirm the PID, and poll in a
*separate* call; polling inside the `nohup` launch races and false-reports
"No such file".

## Benchmarking notes

This device is the historical baseline for `armrx` measurements, including the
instruction-count comparison against XMRig. Because the clock is fixed and there
is no DVFS, run-to-run variance is low — the main confounder is the `isolcpus`
state, which must always be recorded alongside any hashrate figure.
