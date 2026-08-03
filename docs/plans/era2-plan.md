# Era II Plan — From Mirage to Measurement

Companion to `docs/STRATEGY.md` and `docs/TESTING.md`. This is the executable plan. Each phase
has an exact command and a pass/fail criterion. Nothing is committed as a "real optimization"
until it clears the gate sequence in TESTING.md §5.

---

## Phase 0 — Resolve the 1w/8w contradiction (MEASUREMENT ONLY, ~1–2h)

**Goal:** decide whether the lever is IPC or instruction-count before any code change.

**Command (on-device, cross-built `bench_armrx`):**

```sh
# pristine rebuild (TESTING.md §4.4 — always, after any E2x session)
rm -rf build-cross && cmake -S . -B build-cross \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DARMRX_DISABLE_LTO=ON
cmake --build build-cross -j$(nproc) --target bench_armrx

# ship + run the gated 500-hash window at 8 workers
scp build-cross/bench_armrx mechres@192.168.10.156:/tmp/b_armrx
ssh mechres@192.168.10.156 'pgrep -af b_armrx | grep -v pgrep | xargs -r kill -9
  taskset -c 0-7 /tmp/b_armrx --full-hash-only --perf-ready --workers=8' \
  > /tmp/p0_8w.out 2>&1
```

Also capture 1w with the same window to confirm against the w11 census (89.5M):

```sh
ssh mechres@192.168.10.156 'taskset -c 3 /tmp/b_armrx --full-hash-only --perf-ready --workers=1' \
  > /tmp/p0_1w.out 2>&1
```

**Decision (RESOLVED by the run):** armrx = **113.8M instr/hash** at BOTH 1w and 8w (flat, 0% Δ).
The M1 103.5M was a pool-`Total` artifact; the w11 census 89.5M was a mis-divided window. Both
wrong. Compared to XMRig's 98.9M, **armrx is +15% HEAVIER per hash**. Per-worker IPC (0.662) and
stalls are also flat 1w↔8w, so the 95.2% gap is cluster-contention throughput loss (armrx scales
to 65% of linear vs XMRig 73%), which the +15% instruction count likely causes.

**→ Lever (RESOLVED pre-AES, CORRECTED 2026-08-06):** instruction count / codegen density. The
density gap is addressed by the shipped hardware-AES funnel (Item 1, `2687a2e`), but its measured
saving is **modest, not the claimed −16.7%**: the reproducible HEAD re-baseline gives **101.10M
instr/hash**, i.e. AES saves **107.36M → 101.10M = −5.8%**. armrx at 101.10M is **~7% HEAVIER
than XMRig (94.5M)** at 1w — the earlier "89.47M, below XMRig / largest win in history" claim was a
contaminated-divisor artifact. H/s parity still holds (1w 5.11 vs 5.04) via better IPC. The E3b
reframe below is correct that *the specific candidates it traced* (CBRANCH, C*, `*_M` padding) are
IPC-preserving and 8w-neutral. Do NOT re-open the "8w contention-IPC / reduce-memory-traffic" axis —
Reasonix's 2026-08-05 audit root-caused the residual `*_M` stall as L2/contention latency (25–40
cyc/op), the fixed RandomX light-mode penalty XMRig also pays; the only software attack (E26)
segfaulted and is closed. See the **Re-baseline** section for the corrected numbers.

---

## Phase 1b — INSTRUCTION-COUNT REDUCTION IS 8w-NEUTRAL — reframed (SUPERSEDED by hardware AES)
**Reframe (`e3b-reframe-instruction-count-neutral.md`):** E3b's per-opcode map found armrx
heavier than structural-min, but tracing the top candidates showed the "excess" is **deliberate
IPC-preserving padding**:
- `h_CBRANCH` (5 instr) is intentional — a naive `beq` would mispredict 99.6% (CBRANCH taken
  ~0.4%); tightening it costs IPC.
- The `*_M` / C* "over-emission" IS the E24 padding; **E24's A/B proved removing it = 0% 8w H/s**
  (instr 103.8 vs 103.5M, IPC 0.599 vs 0.598).
- `h_CFROUND` is negligible (rare).
**The E3b *premise* (trimming instructions won't help) is half-right and half-wrong:** the specific
candidates it traced are genuinely IPC-preserving and 8w-neutral, but the broader claim "instruction
count cuts are 8w-NEUTRAL" was **qualified by the shipped hardware-AES funnel** — a density cut in
`aes_hash.cpp` (not the JIT) that *did* move H/s, though by **−5.8% (107.36M → 101.10M)**, not the
erroneous −16.7% first reported (89.47M was a contaminated-divisor artifact). armrx remains ~7% heavier
than XMRig on instructions at 1w; H/s parity holds via better IPC. The AES path is the one density
region E3b's per-opcode map did not reach (it lives in `aes_hash.cpp`/`aes.hpp`, not the JIT), and it
was the actual lever. P1.1/P1.2 below are therefore **moot for the residual gap** and
kept only as optional cheap probes. The "8w contention-IPC / reduce-memory-traffic" axis is **closed**
(see Reasonix 2026-08-05 audit): the `*_M` stall is L2/contention latency (25–40 cyc/op), a fixed
RandomX light-mode penalty XMRig also pays; E26's only software attack segfaulted.

### Experiment P1.1 — Per-opcode emission diff vs XMRig (GLM E3b) — SUPERSEDED
M1 localized the gap to instruction *mix*, not stalls. Diff the JIT buffer (`--jit-dump`) against
XMRig's emitted code per opcode. Candidates (CBRANCH, `*_M` consumer, ISWAP_R, INEG_R) are all
IPC-preserving padding per E24's A/B — no 8w H/s movement expected. Not worth re-running now that the
density gap is closed by AES.

### Experiment P1.2 — Clang cross-build A/B (GLM Tier 1-B) — optional cheap probe
GCC 16.1.0 gave +7.9% over GCC 15.2.0 with zero source change. Clang's AArch64 codegen may reduce
instruction count differently. Build with a Clang cross toolchain, KAT 16/16 on-device, bench 1w+8w,
compare to GCC-16-cross baseline. Zero-risk; only worth it if a future session wants to re-verify
codegen density — no longer the critical path.

---

## Re-baseline — clean gated `--perf-ready` at HEAD (DONE, 2026-08-06, non-isolated)

`isolcpus` is an operational deployment knob, NOT an armrx feature, and is not assumed on real
devices — so the baseline is measured **non-isolated** (the device's normal state). `bench_armrx
--full-hash-only` is single-threaded (it ignores `--workers`), so it yields the authoritative 1w
number; the 8w number comes from the **real-pool long-run** (26.65 vs XMRig 28 = 95.2%).

**Result (1w, core 3, non-isolated, two runs identical to 0.00006%):**
instr/hash **101.10M**, IPC **0.667**, median **195.5 ms**, clock 763 MHz (valid).
This supersedes the changelog's erroneous 89.47M (−16.7%) — that was a contaminated-divisor
artifact; the reproducible AES delta is **107.36M → 101.10M = −5.8%**, and armrx is **~7% HEAVIER
than XMRig (94.5M)** at 1w. H/s parity still holds (1w 5.11 vs 5.04) via better IPC. Full detail in
`measurements/2026-08-06-head-rebaseline.md`.

**Build (pristine, AGENTS.md §4.4):**
```sh
rm -rf build-cross && cmake -S . -B build-cross \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DARMRX_DISABLE_LTO=ON
cmake --build build-cross -j$(nproc) --target bench_armrx
```

**Device — 1w (core 3, non-isolated), gated 500-hash window:**
```sh
scp build-cross/bench_armrx mechres@192.168.10.156:/tmp/b_armrx
ssh mechres@192.168.10.156 'pgrep -af b_armrx | grep -v pgrep | xargs -r kill -9
  taskset -c 3 /tmp/b_armrx --full-hash-only --perf-ready --workers=1' > /tmp/reb_1w.out 2>&1
```

**Conclusion:** the instruction-count gap is NOT closed (armrx ~7% heavier at 1w); the
**H/s parity goal IS met** (1w ahead, 8w 95.2%) and is the meaningful outcome. No remaining
software lever on instruction count — the residual is the SoC's two-cluster asymmetry (weak cluster
0.53× fast), which XMRig bears identically. The Clang A/B (P1.2) remains an optional cheap probe.

---

## Phase 1a — IPC lever (DEPRIORITIZED — not the differentiator)
Phase 0 showed per-worker IPC (0.662) and stalls are FLAT 1w↔8w. The 95.2% gap is cluster
contention, not per-worker IPC. Do NOT pursue stall-hiding/scheduling for IPC — it cannot close a
gap that isn't there per-worker. (If P1b's density fix also improves 8w scaling, revisit.)


---

## Phase 2 — Gate, keep, or revert (every experiment)

1. KAT 16/16 byte-identical (on-device).
2. `test_jit_scheduler_stress` 450 pairs.
3. `test_jit_superscalar_scheduler_stress` 200 pairs.
4. 1w + 8w H/s via TESTING.md §1/§3 + PMU.

Keep only if it improves the Phase-0-locked metric without regression. Archive the dead attempt
with evidence (Era I discipline). Revert-on-failure always.

---

## Open items carried from GLM audit (non-perf, do alongside)
- **SIGINT/Ctrl-C bug**: `sigaction(SIGINT, SA_RESTART=0)` + unblock in stratum reader thread.
- **MetricsExporter data race**: `std::atomic<int>` on `server_fd_`.

## What we will NOT do in Era II (dead, per Era I + 2026-08-05 audit evidence)
`*_M` scheduler extension / E26 hoist, CBRANCH replay-map redesign, PRFM scratchpad hints,
hugepages tuning, PGO, dual-issue alignment, Track C, CSEL CBRANCH, FDIV/FSQRT, `-mtune=
cortex-a53`, NEON-load "second LSU" (A53 has one LSU), speculative execution past loads (A53
is in-order), **and the "8w contention-IPC / reduce-memory-traffic" axis** (the `*_M` stall is
L2/contention latency, the fixed RandomX light-mode penalty XMRig also pays; no software attack
survives — E26 segfaulted, scratchpad pattern is spec-fixed). The density gap is CLOSED by the
shipped hardware-AES funnel; no remaining code lever. See `docs/archived/` for the evidence
behind each.
