# armrx AArch64 Performance — Lead Tracking & Discipline Log

> Single source of truth for "what we've tried, what we're trying, what's dead."
> Goal: STOP re-running the same territory (the snake-eating-its-tail failure mode).
> Rules at bottom — read before starting any new measurement.

---

## 0. STANDING FACTS (do not re-litigate without new evidence)

- **Device:** MSM8929 (Snapdragon 415), 2×4 A53 @ fixed **765 MHz** (no cpufreq/OPP → clock
  cannot change). ~50% throughput gap between fast (0-3) and weak (4-7) clusters is the
  **SoC's own design**, not software. Both miners bear it.
- **Cooling:** fan present throughout (6cm moved 2026-08-02; 12cm added after). Steady-state
  under RandomX load ≈ **60°C**. Both armrx AND XMRig run at the same temp on the same silicon.
- **Benchmark numbers (same HW, same cooling, both ~60°C):**
  | Miner | 1w (core3) | 8w |
  |-------|-----------:|----:|
  | XMRig | 4.53 H/s | **28 H/s** |
  | armrx | ~4.0 H/s | **21.85 H/s** |
  armrx ≈ **78% of XMRig @8w**, ~88% per-core.
- **The gap is CODE, not thermal/hardware.** Both miners on identical silicon+cooling → the
  difference is instruction efficiency.
- **Instruction census (W1-4, device perf, settled):** armrx **~119M instr/hash** vs XMRig
  **~94.5M instr/hash** = **~+26% excess instructions** in armrx for the same algorithm.
  armrx IPC 0.731 > XMRig 0.648, so it's purely an *instruction-count* (not stall) gap.
- **armrx region split (W1-1 census):** superscalar 80.5% / main-VM JIT 9.9% (IPC 0.405,
  memory-stall penalty) / named C++ 9.5% (AES ~10.7M) / unattributed 0.05%.
  → the +26% gap lives mostly in the **superscalar body** (~80% of instructions).
- **armrx multi-worker scaling is LINEAR per-core** (4w = 3.8× from 1w; weak cluster = 0.53×
  fast = SoC design). No software multi-worker inefficiency. (per-worker proof via `--pool-test`.)

---

## 1. CLOSED / DEAD LEADS (do NOT re-run — evidence on file)

| Lead | Result | Evidence | Why dead |
|------|--------|----------|----------|
| **Hugepages** | null (+1.5 H/s) | E15: root armrx takes 128×2MiB via `MAP_HUGETLB` (`virtual_memory.c:235` == XMRig's exact technique); HugePages_Free 256→128; only 22.9 vs 21.4 | armrx already does XMRig's method |
| **CPU affinity** | null | E15: `AffinityMode::All` (`mining_engine.cpp:168`) pins 1:1 to `core_order_` = 0..7 (correct fast/weak split by coincidence on no-cpufreq) | placement already correct |
| **Fast-div/sqrt (Newton)** | −1.1% | `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` measured regression | shipped gated; not a win |
| **`*_M` scheduler extension** | DIVERGES | `test_jit_scheduler_stress` 450 + `test_jit_superscalar_scheduler_stress` 200 FAIL deterministically | do NOT re-enable memory-op scheduling |
| **PGO** | null | `ARMRX_PGO` measured null on current code | toolchain-dependent; skip |
| **LDP/STP fusion** | structural 0% | W2-2: `*_M`/`ISTORE` are register-indexed, never adjacent `#imm` | impossible by construction |
| **C* literal-pool (W3)** | −16..20% | cache-miss tripled, IPC collapsed from 714 scattered loads | reverted; do not re-attempt |
| **Dual-issue alignment (W2-2)** | −0.1% | NOP overhead > recovery | reverted |
| **Blake2b parity (E3a)** | **armrx WINS** | armrx `blake2b.cpp` = hand-written **NEON** G; XMRig `blake2b.c` = **scalar C** on AArch64 (no NEON path) | gap is NOT here; armrx already better |
| **Thermal-throttle "cause" story** | RETRACTED | fan was always on; both miners hit same 60°C and XMRig still wins; "5.8× recovery" was 30s-vs-120s read artifact | gap is code, not temp |
| **Multi-worker scaling "inefficiency" (E15 old)** | RETRACTED | per-worker `--pool-test`: fast cluster scales 3.8× linearly, weak=0.53× (SoC design) | no software scaling loss |

---

## 2. OPEN LEADS (code-level; where the +26% instruction gap lives)

### E3c — superscalar body density vs XMRig  [PRIME SUSPECT — do this next]
- **Hypothesis:** ~80% of armrx's instructions are the superscalar body; the +26% total gap is
  mostly here. Prime suspect = **C* immediate materialization** (`IADD_C7/8/9`, `IXOR_C7/8/9`):
  armrx emits MOVZ/MOVN+MOVK+ALU (~3 A64) where XMRig uses a denser form. W2-3 static analysis
  found ~714 such ops/hash.
- **Test:** miner-to-miner `perf stat -e instructions,cycles` on identical workload, then
  **region-split** XMRig's instructions (superscalar vs main-VM vs blake2b vs argon2) to compare
  against armrx's W1-1 census. Specifically diff the superscalar opcode bodies.
- **Effort:** ~30-60 min; needs XMRig binary (present: `~/xmrig`, `~/xmrig-dev`).
- **Stop/go:** if XMRig's superscalar body is materially denser, that's the lever → design a
  code change (clean-room: re-implement technique in armrx conventions).

### E3b — main-VM memory-op emission ordering vs XMRig
- **Hypothesis:** XMRig may order/interleave scratchpad loads to hide latency better, or emit
  fewer ops for the same program. Main-VM IPC is armrx's worst region (0.405) — but that's a
  *stall* penalty, and the gap is *instruction-count* not stalls, so lower priority than E3c.
- **Test:** `perf mem` / cache-miss + instruction-count diff on the main-VM region, miner-to-miner.

### E3a — Blake2b IPC/instr vs XMRig  [EFFECTIVELY CLOSED by inspection]
- **Result:** armrx NEON vs XMRig scalar-C on AArch64 → armrx is *better*. Not the gap.
- **Only residual value:** confirm with a direct `perf` count if E3c's region split shows
  Blake2b unexpectedly large in armrx (unlikely). Low priority.

---

## 3. GitHub Copilot ideas (attached 2026-08-02) — triage

Source: user-attached `sun_aug_02_2026_performance_improvement_ideas_for_codebase.md`
(skeptical perf suggestions). Mapped against our actual state:

| # | Copilot idea | Verdict | Notes |
|---|--------------|---------|-------|
| 1 | Measure-first guardrails (pin, median+p95+MAD, separate regions, noise-floor control) | **ADOPT as discipline** | we've been burned by 1-long-run + thermal confounds; codify below |
| 2 | Split scheduler: main conservative / superscalar aggressive | **PARTIAL / already done** | emitter scheduler already reorders both; aggressive superscalar window was the real small win (+0.233% IPC). Not a new lever |
| 3 | Microarch profiles (A53 vs A55+), MIDR runtime select | **VALID but low priority** | device is fixed A53 @765MHz; only matters if we target other HW. Keep as future |
| 4 | JIT code-layout (hot-loop align 32/64B, reduce taken branches, I-cache pressure) | **WORTH A TEST** | aligns with T2-2 (dual-issue align was null) but block-alignment ≠ NOP-pad. Untested. Queue behind E3c |
| 5 | Register-pressure-aware emission | **PLAUSIBLE, untested** | small-core spill avoidance. Investigate if E3c shows spill hotspots |
| 6 | Fast-div/sqrt adaptive gating | **DEAD** | see §1: measured −1.1%, already gated |
| 7 | Prefetch sweep for dataset/cache | **LOW EXPECTATION** | RandomX accesses are random; T2-2-style nulls likely. Skip unless E3b shows miss-bound |
| 8 | Hugepages/TLB experiments | **DEAD** | see §1: armrx == XMRig method, null |
| 9 | Dynamic cluster-aware balancing | **DEAD (for gap)** | scaling is linear; weak cluster is SoC design, XMRig bears it. Only relevant for scheduling fairness, not the gap |
| 10 | PGO workflow hardening | **DEAD** | see §1: null on current code |
| 11 | Compiler-flag matrix (GCC vs Clang, -O3 vs -Ofast) | **WORTH A TEST (cheap)** | never tried Clang on AArch64; could change multiply/load scheduling. Low-risk build comparison |
| 12 | Perf-invariant docs/tests (don't schedule `*_M`, max code-size growth, min confidence) | **ADOPT** | formalize the §1 dead-lead list as enforced invariants |

**Net from Copilot:** mostly re-derives leads we already closed (6,8,9,10 dead) or already do
(2). Genuinely new + cheap: **#11 (Clang vs GCC build comparison)** and **#4 (block alignment,
distinct from the null NOP-pad test)**. Consider **#11** as a quick parallel experiment.

---

## 4. MEASUREMENT DISCIPLINE (enforced — to stop the loop)

1. **Pin + same conditions.** `taskset -c <cores>`; same cooling; report the SoC temp. Never
   compare a 30s early-cool number to a 120s steady-state number (that manufactured the fake
   "5.8× recovery").
2. **Miner-to-miner only.** The ONLY valid comparison is armrx vs XMRig on the *same device,
   same run conditions*. Don't infer XMRig behavior from memory.
3. **Region-split, not just H/s.** A single H/s number hides everything. Always get
   instruction/cycle counts per region (superscalar / main-VM / blake2b / argon2).
4. **Median over repeats, not one long run.** ≥3 repeats; report median + spread. Thermal and
   pool variance are real.
5. **Before any new code change: confirm the lead is OPEN in §1/§2.** If it's in §1, don't redo it.
6. **Clean-room:** read XMRig *technique* only; re-implement in armrx's own conventions. No XMRig
   code in our tree. (User standing rule.)

---

## 5. OPEN QUESTIONS FOR USER
- (answered 2026-08-04) XMRig = 28 H/s @8w cool? → **yes, user-confirmed, no re-baseline needed.**
- XMRig binary available for reference `perf`? → **yes (`~/xmrig`, `~/xmrig-dev`).**
- Clean-room rule still standing? → **yes (re-confirmed this session).**
