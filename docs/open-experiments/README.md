# Open Experiments

armrx is a RandomX miner for **any** AArch64 device. We can only validate
performance hypotheses on the handful of devices we own (currently a couple of
Cortex-A53 boards, one A55 tablet). Many performance ideas depend on microarch
behavior we *cannot* observe here — pipeline depth, MAC-interlock latency,
cache/memory latency profiles that vary wildly across Cortex-A5x/A7x and vendor
implementations.

**Open Experiments** is how we crowdsource that missing data. Each experiment is
a concrete, reproducible test that anyone with an AArch64 device can run in a
few minutes. You run it, send back the numbers, and we use the aggregate to
decide whether an idea is worth building.

This is intentionally lightweight — no code changes required to participate.
Every experiment compares two already-built binaries; you just compile and run.

## How an experiment works

1. **Hypothesis** — what we think might be true (and why it could move H/s).
2. **Test** — exact build + run commands. Two binaries (A/B), one short
   benchmark each.
3. **Report** — a small results template to paste back.

## How to contribute a result

You do **not** need to be a developer. If you have an AArch64 device (SBC,
phone, TV box, server) and can run a terminal:

1. Pick an experiment from this folder.
2. Build + run it (commands are inside each experiment file).
3. Report your numbers. The preferred channel is to **open an issue or start a
   discussion in the armrx repository** with the experiment name in the title
   (e.g. `[open-exp] E24 pad width — RockPro64`). Include the results template
   from the experiment file. If the repo isn't public yet, send the same
   template to the maintainer by whatever channel is available.

### What to report (general template)

```
Experiment: <name>
Device:     <SoC / board, e.g. "Rockchip RK3399", "Raspberry Pi 4">
uarch:      <core types, e.g. "2×A72 + 4×A53", "8×A55">
Cluster:    <big.LITTLE layout if any>
Freq:       <fixed (e.g. 1.5 GHz) or DVFS (governor)>
OS:         <distro / Android+Termux / pmOS>
Workers:    <N>
A: <hashrate> H/s
B: <hashrate> H/s
Notes:      <anything unusual — thermal throttling, background load, etc.>
```

The more devices we get, the better the picture. A single data point that
*disagrees* with our fleet (e.g. a device where variant B is clearly faster) is
the most valuable result we can get — it tells us an idea is worth pursuing.

## Safety

Every experiment in this folder is **consensus-safe**: the code paths being
compared produce **byte-identical hashes** (verified by `test_jit_equivalence`
/ mining KATs). Testing never changes what you mine — only, at most, how fast.
If an experiment ever touches consensus-critical code, it will say so loudly at
the top of the file. None here do.

## Experiments

| Experiment | Hypothesis | Status |
|------------|-----------|--------|
| [`e24-pad-width.md`](./e24-pad-width.md) | E24 C*-padding may be sub-optimal on non-A53 uarchs; an autotuner could pick a better pad per device | 🟡 Open — needs more device tests |
