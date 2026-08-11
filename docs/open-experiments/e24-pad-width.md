# Experiment: E24 C*-padding width per uarch

**Status:** 🟡 Open — needs more device tests
**Consensus-safe:** ✅ Yes (both variants hash byte-identically; only instruction
density/timing differs)
**Related ideas:** Deepseek #1 (startup JIT autotune), Luna #3 (A55 E24 validation)

## Hypothesis

The RandomX superscalar body is ~35% integer multiplies. On the **Cortex-A53**
(single 4-cycle MAC), `emitCpoolImmediate` pads the multiply gaps with an extra
independent ALU op (the "E24" 3-instruction `MOVZ/MOVN+MOVK` form) to hide the
MAC interlock. That gave **+7.1% H/s on A53** (4.77→5.11).

But that padding is tuned for *one* microarch. On a core with a different
pipeline (different interlock behavior, wider issue, deeper bypass), the
3-instruction pad may be *too dense* or *too sparse*. If some device is faster
with the unpadded (denser) form, then a per-device JIT autotuner (pick pad width
at startup) would be worth building. **We can't tell without data from other
uarchs.**

## What we measured so far

| Device | uarch | PAD-ON (default) | PAD-OFF | Δ |
|--------|-------|------------------|---------|---|
| Lenovo (MSM8929) | 8× A53 @765 MHz | 5.11 (known-optimal) | — | E24 wins |
| Redmi 7A (SDM439) | 8× A53 | 5.11-class | — | E24 wins |
| Unisoc SC9863A | 8× A55 | **33.87 H/s** | **33.78 H/s** | −0.3% (tie) |
| **your device?** | | | | |

A53 is already E24-optimal; A55 is a tie. **No win observed in the current
fleet** — but we have no A7x, no Neoverse, no Apple/Ampere data. If you have a
device with a *different* pipeline behavior, this is the experiment that finds
out whether padding matters there.

## How to test (any AArch64 device)

You build two binaries (pad ON vs pad OFF) and run a short benchmark of each.
No code changes — just two compile flags.

### A. Build both variants

#### Option 1 — native build on the device (recommended; works on Linux/pmOS/Termux)

```sh
# clone if you haven't
git clone <armrx-repo-url> armrx && cd armrx

# Variant A: E24 pad ON (the default)
cmake -S . -B build_pad_on  -DARMRX_ENABLE_NATIVE=ON -DARMRX_BUILD_TESTS=OFF
cmake --build build_pad_on  -j"$(nproc)"

# Variant B: E24 pad OFF (denser code, the autotune alternative)
cmake -S . -B build_pad_off -DARMRX_ENABLE_NATIVE=ON -DARMRX_BUILD_TESTS=OFF \
      -DCMAKE_CXX_FLAGS="-DARMRX_NO_E24_PAD"
cmake --build build_pad_off -j"$(nproc)"
```

#### Option 2 — cross compile from a host (musl toolchain, for Linux targets)

```sh
cmake -S . -B build_pad_on  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake \
      -DARMRX_ENABLE_NATIVE=ON -DARMRX_BUILD_TESTS=OFF
cmake --build build_pad_on  -j"$(nproc)"

cmake -S . -B build_pad_off -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake \
      -DARMRX_ENABLE_NATIVE=ON -DARMRX_BUILD_TESTS=OFF -DCMAKE_CXX_FLAGS="-DARMRX_NO_E24_PAD"
cmake --build build_pad_off -j"$(nproc)"
```

> Android/Termux note: the **musl** cross binaries will NOT run on Bionic
> (Termux/Android) — there is no `/lib/ld-musl-aarch64.so.1`. On Termux, use
> **Option 1 (native clang build)**. Also: Termux has no `/tmp` — don't redirect
> logs there; write to your home dir.

### B. Run the A/B

Pick a worker count that saturates your device (8 is a good default for
8-core; use `--workers=N` to match your core count). Use a long-ish run so the
steady-state number is stable:

```sh
BIN=build_pad_on/armrx
echo "=== PAD ON ==="
timeout 240 "$BIN" --mode=light --workers=8 --mine --difficulty=100 \
        --seconds=200 --warmup=40 --no-tui --no-color \
  | tr '\r' '\n' | grep -E "Steady-state hashrate|Selected mode"

BIN=build_pad_off/armrx
echo "=== PAD OFF ==="
timeout 240 "$BIN" --mode=light --workers=8 --mine --difficulty=100 \
        --seconds=200 --warmup=40 --no-tui --no-color \
  | tr '\r' '\n' | grep -E "Steady-state hashrate|Selected mode"
```

Read the `Steady-state hashrate:` line (measured over the post-warmup window).
That's your A and B.

### C. (Optional but valuable) confirm hashes are unchanged

If you want to be rigorous, run the equivalence KAT on both binaries — they
should both report byte-identical:

```sh
# build with tests on instead, then:
./build_pad_on/test_jit_equivalence   # expect: 16 pairs, all byte-identical
./build_pad_off/test_jit_equivalence  # expect: 16 pairs, all byte-identical
```

## How to report

Copy this into an issue / discussion titled `[open-exp] E24 pad width — <device>`:

```
Experiment: E24 pad width
Device:     <SoC / board>
uarch:      <e.g. "4×A76 + 4×A55">
Freq:       <fixed GHz / DVFS governor>
OS:         <distro / Android+Termux / pmOS>
Workers:    <N>
PAD-ON:  <H/s>
PAD-OFF: <H/s>
Notes:    <thermal / background load / anything odd>
```

**Most useful result:** a device where PAD-OFF is clearly faster (or clearly
slower in an unexpected direction). That's the signal that pad width is
uarch-dependent and the autotune idea is worth building. A tie on yet another
A53/A55 device just confirms the current default is fine there.

## What happens with the data

- If a device shows a real pad-width delta → we reopen Deepseek #1 and build
  the autotuner (runtime pad-mode param + startup microbench + cached choice).
- If every device tested is a tie or matches its known-optimal → the idea stays
  parked; the default E24 padding stands.
