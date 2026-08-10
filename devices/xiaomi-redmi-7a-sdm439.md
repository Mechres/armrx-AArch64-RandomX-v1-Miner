# Xiaomi Redmi 7A (`xiaomi-pine`)

Snapdragon 439 test device running postmarketOS. Faster than the Lenovo and the
only device in the fleet with **working frequency scaling**, which makes it the
better platform for clock-normalised measurement.

Referred to as **pine** in conversation (pmOS device codename `xiaomi-pine`).

> **First baseline measured 2026-08-08: 69.39 H/s steady-state** (8 workers,
> `performance` governor, fan-cooled, light mode, zero throttling).
> NOTE: this was measured at the uncapped 1958 MHz fast-cluster clock, which
> later proved to wedge under pool load (see [Clock-instability hazard](#-clock-instability-hazard-2026-08-09-concluded)).
> **Corrected 2026-08-10:** the stable operating point is the fast cluster
> capped to **1497 MHz (1497600)** — **~63 H/s** at 8 workers. The previously
> cited 1708 MHz is **NOT stable** (crashes after 10–15 min of sustained load);
> only 1497 holds indefinitely. Treat 69.39 H/s (measured uncapped at 1958) as a
> best-case local number, not a sustainable pool rate.

All values below were read from the running device on 2026-08-08.

## Identity

| | |
|---|---|
| Model | `Xiaomi Redmi 7A (pine)` |
| DT compatible | `xiaomi,pine qcom,sdm439` |
| SoC | Qualcomm Snapdragon 439 (SDM439) |
| Kernel | `7.1.3-msm89x7` (mainline) |
| Distro | postmarketOS edge, BusyBox `ash`, musl |
| pmOS port | generic **`qcom-msm89x7`** |
| UI | `console` |

### Installation note

Use the **generic port** codename `msm89x7` (`pmbootstrap init` → Vendor: `qcom`,
Device codename: `msm89x7`). `pmbootstrap` emits:

```
WARNING: xiaomi-pine is archived: unmaintained
```

This is **expected and harmless** — the standalone `xiaomi-pine` package was
archived precisely because it was superseded by the generic port. Flashing
`lk2nd` is a hard dependency; without it the mainline kernel will not select the
display panel.

## CPU topology

8× Cortex-A53 in two clusters. **Core numbering does not match cluster
boundaries** — this is the single most important fact on this page.

| Cluster | Cores | Max clock | OPPs (kHz) |
|---|---|---|---|
| **Fast** | **0, 5, 6, 7** | 1958 MHz | 960000, 1305600, 1497600, 1708800, 1958400 |
| **Slow** | **1, 2, 3, 4** | 1459 MHz | 768000, 998400, 1171200, 1305600, 1459200 |

```console
$ cat /sys/devices/system/cpu/cpu0/cpufreq/related_cpus
0 5 6 7
$ cat /sys/devices/system/cpu/cpu4/cpufreq/related_cpus
1 2 3 4
```

**`taskset -c 0-3` gives you three slow cores and one fast core.** For a
fast-cluster-only run use `taskset -c 0,5,6,7`.

Cluster clock ratio is only **1.34×**, notably flatter than the Lenovo's
effective ~2× throughput gap, so the slow cluster is much less of a liability
here.

## Frequency scaling

Unlike the Lenovo, **cpufreq is fully functional**.

| | |
|---|---|
| Driver | present, 2 policies (`cpu0`, `cpu4`) |
| Default governor | `schedutil` |
| Available | `userspace`, `powersave`, `performance`, `schedutil` |
| Boost | supported, currently `0` |

Pin all cores to maximum:

```sh
for p in 0 4; do
  echo performance | sudo tee /sys/devices/system/cpu/cpu$p/cpufreq/scaling_governor
done
```

Verified result — all 8 cores at their cluster maximum:

```
cpu0=1958 cpu1=1459 cpu2=1459 cpu3=1459
cpu4=1459 cpu5=1958 cpu6=1958 cpu7=1958   (MHz)
```

Remember to restore `schedutil` afterwards if the device is not dedicated to a run.

## Thermal

13 thermal zones. Idle readings **without** active cooling:

| Zone | Idle |
|---|---|
| `cpuss0-thermal` | 47 °C |
| `cpu4..7-thermal` | 47 °C |
| `aoss-thermal` | 47 °C |
| `q6-thermal` | 46 °C |
| `cpuss1-thermal` | 46 °C |

Trip points on `cpuss0-thermal`: **75 °C / 85 °C / 100 °C** — throttling begins
at 75 °C, leaving roughly 28 °C of headroom from idle.

Cooling devices (4 throttle steps per cluster):

```
cpufreq-cpu0: cur=0/max=4
cpufreq-cpu1: cur=0/max=4
```

**`cur_state > 0` is the definitive throttling signal.** Poll it during any
sustained run; do not infer throttling from hashrate alone.

This unit is **in its case** and actively cooled by a fan. The case makes peak
and sustained behaviour more likely to diverge than on the open-frame Lenovo.

## Memory

```
Mem:   1826 MiB total,  ~1386 MiB available
Swap:  2739 MiB (zram)
```

Available RAM is **below the 2080 MiB fast-mode dataset threshold**, so armrx
auto-selects **light mode (256 MiB cache)**. Light-mode results are not
comparable to fast-mode results.

## Power

**No battery** — the unit is powered by a stable external PSU.
`/sys/class/power_supply/` is empty.

This is worth stating explicitly because the pmOS wiki lists **Battery: Broken**
for this port, so an empty `power_supply` directory is ambiguous: here it is
simply *correct*. A side benefit for measurement is that there is no battery sag
or charge-state variable in the thermal/clock behaviour.

### The missing battery is a real operational hazard

A phone battery normally acts as a very-low-ESR buffer capacitor directly across
the power rail. It absorbs the microsecond-scale current transients produced
when the CPU jumps from idle to full load — far faster than a switching
regulator's feedback loop can respond.

**With no battery, this device has no local buffering.** Every load transient
propagates back through the supply wiring to the regulator.

Observed failure mode: running an 8-worker benchmark while a second device
shared the same buck-converter supply caused a **hard hang** — unresponsive on
the physical console, with the kernel still answering ICMP and TCP SYN from
softirq context while userspace was completely wedged. Removing the second
device from the shared supply made the identical workload run to completion.

Two consequences:

- **Network reachability is not proof the device is alive.** `ping` and an open
  port 22 can both persist through a hang. Check the physical console.
- **No crash log survives.** `/sys/fs/pstore` is empty (no ramoops backend),
  `/var/log/dmesg` is overwritten each boot, and `wtmp` is broken (dates to
  1970). Files written shortly before a hang can also come back **0 bytes** —
  unflushed ext4 writes are lost. Any on-device logging intended to survive a
  hang must `sync` after every line.

The comparison devices **never exhibit this failure** on the same supply, and
the wiring difference is the key asymmetry:

| Device | Power path | Hangs under load? |
|---|---|---|
| Lenovo (MSM8929) | LM2596 → battery terminals, direct | no |
| Unisoc (SC9863A) | LM2596 → battery terminals, direct | no |
| **Redmi 7A** | LM2596 → **BMS** → phone | **yes** |

The Redmi is the only device with a **BMS in series** in the power path, and the
only one that hangs. A BMS adds a protection FET (nonzero on-resistance), a
current-sense shunt, and an overcurrent/short-circuit trip with its own
threshold and blanking time. Under a step load from idle to 8 workers at
1958 MHz, either the FET + shunt drop can sag the rail below the PMIC's
undervoltage threshold, or the BMS overcurrent protection can trip outright.
A momentary cut would present exactly as observed: userspace wedged, kernel
still answering ICMP from softirq, nothing in any log, unflushed writes lost.

Because the BMS is not exposed to the kernel (`/sys/class/power_supply/` is
empty), a trip event leaves **no driver-level trace** — there is nothing to log
even in principle.

Note that the Lenovo's `pm8916-bms-vm` node reports a 4.17 V "battery" despite
having **no battery connected**; that reading is a phantom from the BMS driver
against an unpopulated input and is not evidence of buffering.

A secondary contributing factor is clock and DVFS behaviour: the Lenovo has **no
cpufreq at all** and runs at a fixed 765 MHz, whereas this device scales to
1958 MHz and changes OPP dynamically. Dynamic power scales with frequency and
roughly with the square of voltage, and OPP transitions are themselves current
steps — so this device's peak and transient demand is substantially higher.

**The mechanism is not confirmed.** The BMS is the strongest suspect on
elimination grounds, not on direct measurement.

### Discriminating test

Bypass the BMS and wire the LM2596 directly to the phone's terminals, matching
the other two devices. This is a single-variable change against the one
component unique to this device. If the 8-worker hang disappears, the BMS is the
cause.

Worth doing **before** adding bulk capacitance upstream: capacitors on the
regulator side of a tripping BMS would not help much. Capacitance placed
*downstream* of the BMS, at the phone end with short leads, helps with both
failure modes.

Capping peak current via `scaling_max_freq` is a second zero-cost, reversible
test:

```sh
echo 1497600 | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
```

If the hangs stop at a lower OPP, the cause is power delivery, not software.

## Access

| Path | Address |
|---|---|
| WiFi (primary) | `ssh mechres@192.168.10.200` |
| USB fallback | `ssh mechres@172.16.42.1` |

WiFi uses a static IP with autoconnect priority 100, and survives reboot.

`sudo` has no TTY over SSH and `sudo -S` is blocked, so use an askpass helper:

```sh
printf '#!/bin/sh\necho <password>\n' > /tmp/ap.sh && chmod 700 /tmp/ap.sh
SUDO_ASKPASS=/tmp/ap.sh sudo -A <command>
```

## Gotchas

### `iw` scan/set commands crash the device

**Do not run `iw dev wlan0 scan`, `iw ... scan -u`, `iw set bitrates`, or
`iw phy ... set` on this device.** A full active scan wedges the WCNSS
remoteproc firmware and takes the kernel with it — hard reboot, sometimes a
bootloop. This caused roughly four reboots and one bootloop in a single session.

Use `nmcli` for **all** wireless operations (`nmcli dev wifi list`,
`nmcli c modify`, `nmcli c up`). Everything needed for configuration and
diagnosis is reachable without `iw` write or scan commands.

Read-only `iw` calls are safe: `iw dev wlan0 link`, `iw dev wlan0 station dump`,
`iw reg get`.

### wcn36xx cannot handle a TKIP group cipher

Symptom on a WPA/WPA2-mixed AP with a **TKIP group cipher**:

- 60–95 % packet loss, **receive side only**
- `tx retries: 0`, `tx failed: 0` — transmit is clean
- `beacon loss: 0` — beacons and broadcast management frames arrive perfectly
- Signal excellent (−27 dBm), so it looks nothing like a range problem
- Loss is **worse when idle** (93 % at 1 s ping interval vs 75 % at 0.05 s)
- RX bitrate collapses to 1–26 Mbit/s while TX negotiates 52–72 Mbit/s

Broadcast/multicast frames — including ARP — are mishandled, which cascades into
unicast failure.

**Forcing the client to CCMP does not help.** A client cannot override the AP's
group cipher; only the AP can. Fix it at the access point:

| AP configuration | Result |
|---|---|
| WPA/WPA2 mixed, TKIP group cipher | 60–95 % loss |
| WPA2/WPA3 transition, AES | 45 % loss |
| **WPA2-only, AES/CCMP** | **0 % loss, 135 Mbit/s RX** |

### No WPA3 and no PMF — hardware limitation

Supported ciphers are WEP40, WEP104, TKIP, CCMP-128. There is **no BIP/GMAC
cipher**, so the chipset cannot do WPA3/SAE or 802.11w protected management
frames. An AP with **PMF set to "Required" will lock this device out entirely**.
Leave PMF at *Optional* or *Disabled*.

### `debug_mask` defaults to maximum verbosity

On this image `wcn36xx` ships with `debug_mask=65535`, which hex-dumps **every
network frame** to the kernel log. This is a real CPU drain and will contaminate
benchmark results. Pin it off:

```sh
echo 'options wcn36xx debug_mask=0' | sudo tee /etc/modprobe.d/wcn36xx-debug.conf
```

### Display backlight cannot be turned off

`/sys/class/backlight/` is **empty** — no brightness control is exposed, matching
the wiki's "Screen: Partial" rating. Both software blanking paths report success
while the panel stays lit:

```console
$ cat /sys/class/graphics/fb0/blank        # 1  (claims blanked)
$ cat /sys/class/drm/card0-Unknown-1/dpms  # Off (claims off)
```

The backlight regulator is not driven by the display driver, so there is
currently **no known software method** to switch the panel off. For a
permanently headless unit the remaining option is physical.

Note that the `console` UI choice is not what enables a local TTY — pmOS
provides a TTY on the physical display with `none` as well.
## Benchmarking notes

### First measured baseline (2026-08-08)

First `armrx` run on this device. Cross-compiled `armrx 0.2.0` (`a1ea83c`),
`performance` governor on both policies, actively fan-cooled, in its case.

**Correctness gate first:** `test_jit_equivalence` — **16/16 pairs
byte-identical**.

| Metric | Value |
|---|---|
| **Steady-state** | **69.39 H/s** (mean of last 60 samples) |
| Stability | σ = 0.078 H/s |
| Peak | 69.54 H/s |
| Workers | 8 |
| Valid shares | 121 |
| Memory mode | light — **528 MiB** including reserve |
| Throttle events | **0** (`cooling_device*/cur_state` never left 0) |
| Clock | `1958,1958,1459,1459` MHz — never varied |
| Zone temps | 37 °C idle → **49 °C peak** (trip is 75 °C) |
| Reported CPU temp | 45 → 50 °C |

Hashrate *rose* +1.58 H/s from the early window to the late window and then held
flat — that is warmup completing, not thermal decay. **69.39 H/s is the
steady-state figure.**

Two things this establishes:

- **Thermals are a non-issue with active cooling.** Peak 49 °C against a 75 °C
  trip point is 26 °C of headroom, with zero throttling, in a case.
- **Memory mode is light at 528 MiB**, not the ~2 GiB a naive
  "256 MiB × 8 workers" estimate would suggest — workers share the cache.

For scale, the Lenovo's best recorded figure is ~28.4 H/s (under `isolcpus`), so
this device is roughly **2.4×**. The clock ratio alone is 1958/765 ≈ 2.56×, so
the gain is essentially clock, not microarchitecture — as expected for the same
A53 core.

### Real-pool run (2026-08-08)

A second run against a CryptoNote Stratum pool (`--pool`, 8 workers, light mode
with `--dataset-mb=512`).

- Login succeeded; partial dataset (512 MiB) filled in **~46 s** before mining
  started.
- Steady-state **~80–83 H/s** — **higher than the local benchmark's 69.39 H/s**,
  consistent with the local benchmark's artificial 30 s warmup and the pool run
  reaching full steady state.
- CPU 39 → 50 °C, no throttling, no hang.
- **0 shares** submitted over a ~145 s run (no share-found at pool difficulty in
  that window) — expected at this hashrate; not a fault.

This run confirms the `--dataset-mb` partial-dataset path works end to end
(login → seed-key cache init → background fill → resume → mining) and that the
Redmi 7A is stable on real pool traffic, not just the local benchmark.

### ⚠️ Clock-instability hazard (2026-08-09, CONFIRMED)

Running at the **uncapped fast-cluster clock (1958 MHz)** under 8-worker pool
load causes a **device-wedging fault** shortly after the first seed-key
rotation (typically job 3, ~170–195 s): the process emits `Segmentation fault`
and the **whole device hangs** (SSH → "No route to host", unrecoverable without
a power cycle). No core dump or kernel log survives — consistent with an ARM
SError / async external abort, i.e. a **hardware-level** fault, not a recoverable
userspace crash.

This is **NOT a software bug** — the identical `a1ea83c` binary runs cleanly
through 9+ seed rotations on the Lenovo (~765 MHz) and on pine itself once the
clock is capped. Confirmed by a controlled test:

- Uncapped (1958 MHz): segfault + wedge at job 3 / ~173 s.
- Capped fast cluster to its 960000 floor (slow cluster → 768000): **survived
  781 s / 9 job rotations / 50.8 H/s, zero crashes** — matching the Lenovo.

**Operational rule (CORRECTED 2026-08-10):** do **not** mine pine at 1958 MHz
(the fast cluster's top step) **and do not run sustained loads at 1708 MHz** —
both wedge/crash. Cap the fast cluster to **1497 MHz (1497600)**, the only step
that survives indefinitely, for safe operation:

```sh
# stable pine: fast cluster pinned to 1497 MHz (userspace), slow cluster at full 1459
echo userspace | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
echo 1497600  | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed
echo performance | sudo tee /sys/devices/system/cpu/cpufreq/policy1/scaling_governor
echo 1459200  | sudo tee /sys/devices/system/cpu/cpufreq/policy1/scaling_max_freq
```

- Stable throughput at 1497/1459 MHz: **~63 H/s** light mode, 8 workers
  (measured 63.11 H/s, 2026-08-10, local `--mine` benchmark, no crash, CPU ≤49°C,
  run duration ~19 min). The earlier ~80 H/s figure was at 1708 MHz, which is
  now known to be unstable (see below) — do not use it as a target.
- Clock-sweep results, each run several job rotations under pool load:
  - **2026-08-09:** 960 ✓, 1497 ✓, 1708 ✓ (short-window), 1958 ✗ (wedges @ job 3 / ~173–210s).
  - **2026-08-10 (CORRECTION):** 1708 MHz is **NOT stable under sustained load** —
    it crashes after **10–15 minutes** (well beyond the short 2026-08-09 windows
    that passed). **1497 MHz (1497600) is the highest step that survives a long
    run.** So the stable ceiling was overstated on 2026-08-09; use 1497, not 1708.
- The fault at 1958 is a hardware V/F margin (SoC not stable at that clock under
  the JIT-recompile current spike), confirmed NOT a supply issue (bench PSU swap
  still wedged) and NOT a software bug. Out of scope for armrx code.
- The 86 H/s briefly seen at 1958 before wedging is **not safely attainable**;
  ~63 H/s at 1497 is the practical, sustainable max.

### Permanent clock pin (OpenRC `local.d`, 2026-08-09)

To keep pine off the unstable 1958 step across reboots, the stable pin is
installed as a boot hook (pmOS uses OpenRC, not systemd):

- File: `/etc/local.d/cpufreq.start` (mode 0755), already in the `default`
  runlevel via the `local` service.
- Contents: fast cluster (policy0) → `userspace` + `scaling_setspeed=1497600`;
  slow cluster (policy1) → `performance` + `scaling_max_freq=1459200`; re-assert
  after 1 s (the cpufreq driver on this kernel occasionally rounds a setspeed
  up to the next table step, so the re-assert guards against that).
- Verify after any reboot: `cat /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq`
  must read `1497600 1459200`. If it reads `1708800` or `1958400 ...`, the hook
  did not fire (or a setspeed rounding slipped through) — re-run
  `sudo /etc/local.d/cpufreq.start` and confirm it reads `1497600`.
- **Note (CORRECTED 2026-08-10):** the cpufreq driver silently rounds any
  off-table frequency request UP to the nearest table step (e.g. writing 1830000
  yields 1958400). Only the five table steps stick: 960 / 1305 / 1497 / 1708 /
  1958. **1708 is NOT safe for sustained runs (crashes 10–15 min); 1497 is the
  highest stable step for a long pool run.**
- **PSU swap (2026-08-09) ruled out the supply:** replacing the LM2596 with a
  clean regulated bench/desk PSU at the same voltage, clock left uncapped, still
  wedged at ~job 3 / ~210s. The fault is the **SoC's V/F margin at 1958 MHz**,
  not the supply. No PSU change can fix it — the 960 MHz cap is the only stable
  answer. See `docs/briefs/2026-08-08-pine-pool-segfault.md`.
- Root-cause analysis and the discriminating experiment are in
  `docs/briefs/2026-08-08-pine-pool-segfault.md` (with the rejected JIT-race
  hypothesis, kept for record).

### Topology / worker-count matrix (2026-08-10)

Measured with the fast cluster clamped to **1497 MHz (1497600)** — the stable
ceiling — slow cluster at full 1459 MHz. Local `--mine` benchmark (pool `:1111`
is BLOCKED from pine's network egress, so no real-pool run possible here).
200 s per config, 30 s warmup, steady-state over the post-warmup window.

| Mode | Workers | **Steady H/s** | worker spread (w0 / w4) |
|---|---|---|---|
| light | 1 | **10.08** | 10.08 |
| light | 2 | **19.14** | 9.39 |
| light | 4 | **35.10** | 8.95 |
| light | 8 | **63.11** | 8.42 / 7.49 |
| auto (8w) | 8 | **63.04** | 7.97 / 7.45 |

**Findings:**
- **Near-linear scaling** with worker count (≈ +87% per doubling): 10 / 19 / 35
  / 63 H/s at 1 / 2 / 4 / 8 workers. 8 workers = 63.11 H/s.
- The 2026-08-08 **69.39 H/s** baseline was at the uncapped 1958 MHz fast cluster;
  at the stable 1497 MHz ceiling it is **63.11 H/s** — the ~9% drop is the clock
  reduction (1958 → 1497), not a regression.
- `auto` selected **light** (requires only ~528 MiB) and matched the manual
  light/8w figure (63.04 vs 63.11) — confirms auto mode + the matrix are
  consistent.
- **No crash** over the ~19 min total run; CPU peaked at 49°C (vs the 1708
  crash zone). The 1497 clamp held end-to-end.
- **Fast mode not run on this build:** the 2026-08-08 pine binary lacks
  `--dataset-mb`, and free RAM (~1.39 GiB) is below the 2080 MiB fast threshold,
  so `auto` correctly stays light. A fast-mode matrix is a separate task (needs a
  build with `--dataset-mb` + more free RAM).

### Procedure for subsequent runs

1. Pin both policies to `performance` (see above).
2. Log `scaling_cur_freq` **and** `cooling_device*/cur_state` every 5–10 s.
3. Run until temperatures plateau — the case means peak ≠ sustained.
4. Only record hashrate **after** thermal steady state; treat any run with
   `cur_state > 0` as throttled and label it as such.

```sh
# thermal + clock sampler — sync after every line so the log survives a hang
while :; do
  L="$(date +%T) T="
  for z in 2 5 6; do L="$L$(( $(cat /sys/class/thermal/thermal_zone$z/temp) / 1000 )),"; done
  L="$L F="
  for i in 0 5 1 4; do L="$L$(( $(cat /sys/devices/system/cpu/cpu$i/cpufreq/scaling_cur_freq) / 1000 )),"; done
  L="$L thr="
  for c in /sys/class/thermal/cooling_device*; do L="$L$(cat $c/cur_state),"; done
  echo "$L" >> /tmp/thermal.log
  sync
  sleep 5
done
```

Because steady-state throughput is what matters, a slower start that reaches a
higher plateau beats a fast start that settles lower.

### Do not process logs on-device

Running `tr`, `awk`, or `strings` over log files **on this device** has
repeatedly preceded a hang. `scp` the logs to a host and parse them there — the
data survives even when the device does not.
