# Xiaomi Redmi 7A (`xiaomi-pine`)

Snapdragon 439 test device running postmarketOS. Faster than the Lenovo and the
only device in the fleet with **working frequency scaling**, which makes it the
better platform for clock-normalised measurement.

> **Status: never benchmarked.** Treat the first `armrx` run as a
> characterisation run, not a data point. See [Benchmarking notes](#benchmarking-notes).

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

This device has never been benchmarked. Suggested first run:

1. Pin both policies to `performance` (see above).
2. Log `scaling_cur_freq` **and** `cooling_device*/cur_state` every 10 s.
3. Run until temperatures plateau — the case means peak ≠ sustained.
4. Only record hashrate **after** thermal steady state; treat any run with
   `cur_state > 0` as throttled and label it as such.

```sh
# thermal + clock sampler
while :; do
  printf '%s ' "$(date +%T)"
  for z in /sys/class/thermal/thermal_zone*; do
    printf '%s=%sC ' "$(cat $z/type)" "$(( $(cat $z/temp) / 1000 ))"
  done
  for c in /sys/class/thermal/cooling_device*; do
    printf '%s:%s ' "$(cat $c/type)" "$(cat $c/cur_state)"
  done
  echo
  sleep 10
done
```

Because steady-state throughput is what matters, a slower start that reaches a
higher plateau beats a fast start that settles lower.
