# Unisoc SC9863A (Termux / Android)

Third device in the `armrx` test fleet, and the only one running **Android with
Termux** rather than postmarketOS. It is also the only **big.LITTLE** device and
the only one with 4 GB of RAM.

Its defining constraint is that **Termux is confined by an Android cpuset cgroup
to cores 0–5**, so only 2 of the 4 big cores are reachable without root. That
single fact shapes every measurement taken here.

All values below were read from the running device on 2026-08-08.

## Identity

| | |
|---|---|
| Host | `reeder P13 Blue MaxL 2022` |
| Model | `P13 Blue MaxL 2022` |
| SoC | Unisoc **SC9863A** (`ro.board.platform` = `sp9863a`) |
| Core type | **Cortex-A55** (`CPU part 0xd05`) |
| Kernel | `4.14.193` (Android vendor kernel, SMP PREEMPT) |
| OS | Android 11 |
| Environment | Termux (`$PREFIX=/data/data/com.termux/files/usr`) |
| Root | **No** — `su` is the unrooted stub binary |

Note the core type: these are **Cortex-A55**, not the Cortex-A53 found in the
other two fleet devices. A55 is the ARMv8.2 successor to A53 and is a genuinely
different microarchitecture, so per-clock comparisons across the fleet are not
apples-to-apples.

The CPU feature list includes **hardware AES**:

```
fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp
cpuid asimdrdm lrcpc dcpop asimddp
```

`aes` + `pmull` means the `__ARM_FEATURE_AES` hardware AES funnel is available,
as on the pmOS devices.

## CPU topology

8× Cortex-A55, all **present and online**, in two clusters of four.

| Cluster | Cores | Max clock | OPPs (kHz) |
|---|---|---|---|
| LITTLE | 0, 1, 2, 3 | 1200 MHz | 768000, 884000, 1000000, 1100000, 1200000 |
| **big** | **4, 5, 6, 7** | **1600 MHz** | 768000, 1050000, 1225000, 1400000, 1500000, 1600000 |

```console
$ cat /sys/devices/system/cpu/present
0-7
$ cat /sys/devices/system/cpu/online
0-7
```

Unlike the Redmi 7A, core numbering here **does** follow cluster boundaries, and
the ordering is the conventional one: **low cores are the LITTLE cluster**, high
cores are big. This is the inverse of the Redmi, where core 0 is a *fast* core —
do not carry an assumption from one device to the other.

Cluster clock ratio is **1.33×**.

## The cpuset restriction

This is the most important operational fact about this device.

```console
$ grep Cpus_allowed_list /proc/self/status
Cpus_allowed_list:      0-5
$ nproc
6
```

Termux runs inside Android's `cpuset:/foreground` cgroup:

```
7:schedtune:/foreground
4:cpuset:/foreground
3:cpuacct:/uid_10203/pid_5627
```

Consequences:

- **`nproc` reports 6, not 8.** Any build or benchmark that trusts `nproc` will
  silently under-provision.
- Only **cores 4 and 5** of the big cluster are reachable — **half the big
  cluster is inaccessible**.
- **`taskset` cannot escape it.** Verified: `taskset -c 0-7` still yields
  `Cpus_allowed_list: 0-5`. A cgroup cpuset is a hard ceiling, not an affinity
  hint.
- Escaping would require root, and this device is **not rooted**.

So the usable configuration is **4 LITTLE cores @ 1.2 GHz + 2 big cores @
1.6 GHz**, not 8 cores.

Android may also migrate the app between cpuset groups (`foreground` →
`background`) depending on screen state and app lifecycle, which can change the
allowed set mid-run. Re-read `Cpus_allowed_list` at the start *and* end of any
long measurement.

## Frequency scaling

cpufreq is present with two policies and, unusually for an unrooted Android
device, the governor list is readable:

| | |
|---|---|
| Policies | `policy0` (LITTLE), `policy4` (big) |
| Default governor | `schedutil` |
| Available | `userspace`, `powersave`, `performance`, `schedutil` |

Both policies were observed sitting at their **maximum** OPP at idle
(1200 MHz / 1600 MHz).

Writing `scaling_governor` requires root and is therefore **not available** here.
Clock behaviour cannot be pinned; `schedutil` will do what it wants during a run.

## Memory

```
Mem:   3865 MiB total,  ~2429 MiB available
Swap:  2396 MiB  (340 MiB in use)
```

**This is the only fleet device that may clear the 2080 MiB fast-mode dataset
threshold.** At the time of reading, `available` was 2429 MiB — above the
threshold, but not by a wide margin, and Android's memory pressure varies with
what else is resident.

Treat fast mode as **plausible but not guaranteed**. Confirm `MemAvailable`
immediately before a run, and record which mode armrx actually selected rather
than assuming.

## Thermal

**Not observable.** `/sys/class/thermal/` is not readable by the Termux user:

```console
$ ls /sys/class/thermal/
ls: cannot open directory '/sys/class/thermal/': Permission denied
```

`/proc/loadavg` is likewise `Permission denied`.

This is a significant measurement limitation: **thermal throttling cannot be
detected directly on this device.** Unlike the Redmi, where
`cooling_device*/cur_state` gives a definitive throttling signal, here throttling
can only be *inferred* from `scaling_cur_freq` drift and hashrate decay.

Android's own thermal management is additionally more aggressive and more opaque
than the pmOS devices'.

## Toolchain

Termux provides a complete native build environment:

```
clang  gcc  cmake  git
```

This is **Android NDK / Termux clang against Bionic libc**, not musl GCC as on
the pmOS devices. Binaries are not interchangeable between the pmOS devices and
this one, and codegen differences are a real confounder when comparing hashrate.

## Access

```sh
ssh u0_a203@192.168.10.206 -p 8022
```

Note the non-standard port **8022** (Termux `sshd` default). The user is
`u0_a203` (Android app UID 10203).

## Why this device matters

### Documented re-attempt target for co-tenant fill

The hybrid hash-during-fill family (Part 2+3) is closed **for the Lenovo only**,
on the grounds that an 8-core single-tier device has no spare cores and cannot
reach the steady-state ceiling.

This device is the documented legitimate re-attempt target: co-tenant
`SCHED_IDLE`/nice fill could win here **if** the big cores idle during mining.
Re-attempt via the existing `try/hybrid-cotenant-fill` branch (already reverted
to main-equivalent, retained as evidence).

That is a re-attempt on a different device class — **not** a re-opening of the
Lenovo closure.

**However**, the cpuset restriction weakens the premise: only 2 big cores are
reachable, so the "spare big core" headroom the experiment assumes is halved.
Whether the big cores actually idle during a mining run **still needs
measuring** — it has not been confirmed.

### Only possible fast-mode measurement

With ~2.4 GiB available it is the sole fleet candidate for a fast-mode
(2080 MiB dataset) run, which the two sub-2 GB pmOS devices cannot do.

## Remaining unknowns

- [ ] Do the big cores actually idle during a mining run? (co-tenant premise)
- [ ] Does armrx select fast mode in practice, under real Android memory pressure?
- [ ] Sustained vs peak behaviour, given thermal state is unobservable
- [ ] Whether Android migrates the process out of `cpuset:/foreground` mid-run
- [ ] Cooling arrangement (tablet form factor, passive)

## Benchmarking notes

Any hashrate from this device must be labelled with:

1. **Effective core count** — 6 reachable, not 8, and the mix is 4 LITTLE + 2 big
2. **Memory mode actually selected** — light or fast, confirmed not assumed
3. **Toolchain** — Termux clang / Bionic, not musl GCC
4. **Core microarchitecture** — Cortex-A55, not A53

Do not compare raw numbers against the pmOS devices without stating all four.
Because governor pinning and thermal observation are both unavailable, run-to-run
variance is expected to be the highest in the fleet — prefer several runs and a
steady-state figure over any single peak reading.
