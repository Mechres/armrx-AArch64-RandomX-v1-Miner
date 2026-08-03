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

**Expected:** 8w instr/hash ≈ 1w (89.5M) if the M1 103.5M was an artifact. The window is
self-counting (500 hashes), so no `Total` parsing needed — read `instructions:u / 500`.

**Decision:**
- If 8w ≈ 89.5M → **lever = IPC**. Go to Phase 1-IPC.
- If 8w ≈ 103.5M → **lever = instruction count**. Go to Phase 1-density.
- (Either way, record the number in `perf-tracking.md` and close the contradiction.)

---

## Phase 1a — IPC lever (if Phase 0 says IPC)

The 1w census already shows armrx LEANER (89.5M vs 101.4M) but LOWER IPC (0.547 vs 0.654).
So we emit fewer instructions but stall more. The fix is hiding latency, broadly — not just C*
immediates (E24 already did that at 1w).

### Experiment P1.1 — Clang cross-build A/B (GLM Tier 1-B, ~1h, zero risk)
GCC 16.1.0 gave +7.9% over GCC 15.2.0 with zero source change. Clang's AArch64 codegen may
differ again. Build with a Clang cross toolchain, KAT 16/16 on-device, bench 1w+8w, compare to
GCC-16-cross baseline (5.11 / 26.65). Cheapest non-zero-leverage experiment.

### Experiment P1.2 — Per-opcode emission diff vs XMRig (GLM E3b, 1–2d, medium)
M1 localized the gap to instruction *mix*, not stalls. Diff the JIT buffer (`--jit-dump`) against
XMRig's emitted code per opcode. GLM's flagged candidates:
- `*_M` consumer: armrx `add→and→ldr→op` (emitMemLoad) vs XMRig's equivalent.
- CBRANCH form (branchless `bne+b target` ~4–5 instr vs XMRig's 2–3 branchy).
- ISWAP_R (3-MOV sequence) vs XMRig.
- INEG_R (`sub dst,xzr,dst`, 1 instr — already minimal, verify).
For each delta found, implement behind an `ARMRX_*` flag (TESTING.md §6), gate, measure.

### Experiment P1.3 — Broaden stall-hiding to the main-VM body
The superscalar body is 80.5% of instructions (IPC 0.800, already good). The **main-VM JIT is
the worst region (IPC 0.405)** — the `*_M` load-stall lives here, but M1 says total stalls are
~equal to XMRig, so the issue is *how armrx interleaves*, not the absolute stall. Explore
independent-op interleaving in the main-VM emission (NOT cross-handler byte relocation — that's
the dead W3-2/E26 path). Must clear the 450+200 stress gates.

---

## Phase 1b — Instruction-count lever (if Phase 0 says density)

If armrx is genuinely heavier at 8w, the gap is codegen density. Same candidate opcodes as P1.2
(per-opcode diff), but the fix is *fewer instructions per VM opcode* (selection / constant
materialization / reg-alloc), not interleaving. Implement behind flags, gate, measure.

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

## What we will NOT do in Era II (dead, per Era I evidence)
`*_M` scheduler extension / E26 hoist, CBRANCH replay-map redesign, PRFM scratchpad hints,
hugepages tuning, PGO, dual-issue alignment, Track C, CSEL CBRANCH, FDIV/FSQRT, `-mtune=
cortex-a53`, NEON-load "second LSU" (A53 has one LSU), speculative execution past loads (A53
is in-order). See `docs/archived/` for the evidence behind each.
