# Device Reference

Hardware notes for the AArch64 devices used to develop and validate `armrx`.

These files exist because the difference between two "8× Cortex-A53" phones can be
large enough to invalidate a benchmark comparison. Each file records what was
**measured on the device**, not what the spec sheet claims.

| Device | SoC | Cores | cpufreq | RAM | armrx mode | Status |
|---|---|---|---|---|---|---|
| [Lenovo Vibe K5](lenovo-vibe-k5-msm8929.md) | MSM8929 | 8× A53 | **none** (fixed ~765 MHz) | 1884 MiB | light | primary benchmark host |
| [Xiaomi Redmi 7A](xiaomi-redmi-7a-sdm439.md) | SDM439 | 8× A53 | **yes** (960–1958 MHz) | 1826 MiB | light | **69.39 H/s** (2026-08-08) |
| [Unisoc SC9863A](unisoc-sc9863a-termux.md) | SC9863A | 8× **A55** (**only 6 reachable**) | yes, **not settable** (no root) | 3865 MiB | light or **fast** | never benchmarked |

## Reading these files

Each device file follows the same layout:

- **Identity** — SoC, device tree compatible, kernel, distro
- **CPU topology** — cluster layout and the *actual* core-to-cluster mapping
- **Frequency scaling** — whether DVFS exists at all, and available governors
- **Thermal** — idle temperatures, trip points, cooling devices
- **Memory** — total/available, and which armrx memory mode it selects
- **Access** — how to reach the device
- **Gotchas** — measured failure modes, with the command that triggers them

## Cross-device warnings

**Do not assume `taskset -c 0-3` selects the fast cluster.** Core numbering does
not reliably follow cluster boundaries, and the convention is **inverted between
devices in this fleet**:

| Device | Fast/big cores | Slow/LITTLE cores |
|---|---|---|
| Redmi 7A | **0, 5, 6, 7** | 1, 2, 3, 4 |
| Unisoc SC9863A | 4, 5, 6, 7 | 0, 1, 2, 3 |
| Lenovo | 0–3 (fast) | 4–7 (weak) |

Always read `/sys/devices/system/cpu/cpu<N>/cpufreq/related_cpus`, or on devices
without cpufreq, measure it.

**Do not trust `nproc`.** On the Unisoc, Termux is confined by an Android cpuset
cgroup to cores 0–5, so `nproc` reports 6 on an 8-core device and `taskset`
cannot escape the restriction. Check `Cpus_allowed_list` in `/proc/self/status`.

**Do not compare hashrate across devices without normalising for clock, core
microarchitecture, memory mode, and toolchain.** The fleet spans Cortex-A53 and
Cortex-A55, fixed 765 MHz and scaling 1.96 GHz, musl GCC and Termux clang, and
light vs (potentially) fast memory mode. Any of those alone invalidates a naive
comparison.

**Both pmOS devices run BusyBox `ash`, not bash.** Scripts must be POSIX sh.
`pgrep` can hang; prefer `ps -eo comm | grep`.

## Scope note

Device-specific deployment tuning (`isolcpus`, DT/OPP changes, governor pinning)
is **not** an armrx optimisation lever. It belongs here as environment
documentation so measurements can be interpreted correctly, not in the
optimisation ledger. armrx targets generic, portable AArch64.
