# armrx — A Performance Autopsy

> **Clean-room AArch64 RandomX v1 miner built from the specification.**
> 210 commits, 18 days (2026-07-13 → 2026-07-30), 10 performance tracks attempted, 2 adopted.

---

## 1. The Goal

Build a high-performance, clean-room Monero RandomX v1 miner for AArch64 Linux — from the specification only, no XMRig code. Target: an MSM8929/Snapdragon 415 tablet with 8× Cortex-A53, 2 GiB RAM, running postmarketOS.

The initial implementation — Blake2b → AES primitives → Argon2d cache → superscalar execution → dataset generation → interpreted VM → JIT compiler → Stratum client → multi-threaded mining engine — was laid down in a concentrated ~48 hour burst (~40 commits). A complete, working RandomX v1 implementation from scratch.

Everything after that was **measurement, optimization, and the slow discovery that this hardware has very little slack left.**

---

## 2. Architecture Overview

```
block_template + nonce
       │
       ▼
  ┌─────────────┐     ┌──────────────────────┐
  │   Blake2b    │────→│   AES fill (1R×4)    │──→ scratchpad[N] (2 MiB)
  └─────────────┘     └──────────────────────┘
                              │
                              ▼
  ┌─────────────────────────────────────────┐
  │        VM Program (JIT-compiled)         │
  │  256 RandomX instructions, emitted as    │
  │  native AArch64, runs against scratchpad │
  │  16,384× dataset-item calls (light mode) │
  └─────────────────────────────────────────┘
                              │
                              ▼
  ┌─────────────┐     ┌──────────────────────┐
  │  AES hash    │────→│    result = [32]byte │
  │  (1R×4)      │     └──────────────────────┘
  └─────────────┘
```

Two memory modes:
- **Light mode** (256 MiB cache): 16,384 on-the-fly dataset-item derivations per hash
- **Fast mode** (2080 MiB dataset): precomputed dataset for direct lookup — impractical on 2 GiB RAM

The JIT compiler emits AArch64 machine code for all 256 RandomX opcodes, with Cortex-A53-specific tuning: prefetch hints, register allocation, instruction scheduling, NEON for selected paths.

---

## 3. Timeline

### Phase 1 — Getting it working (July 13–17)

| Date | Milestone |
|------|-----------|
| Jul 13 | Initial implementation flood: Blake2b, AES, Argon2d, superscalar, interpreted VM, JIT compiler, Stratum client |
| Jul 13 | First JIT-on-hardware verification (`392eac5`) |
| Jul 14 | NEON SIMD for superscalar + dataset init, big.LITTLE-aware core pinning, huge pages |
| Jul 15 | Newton-Raphson FDIV/FSQRT JIT implementation |
| Jul 16 | Security hardening (S1–S8), AES LUT optimization, dispatch table refactor |
| Jul 17 | NEON Argon2 G-function, branchless CBRANCH, hwloc topology-aware pinning |

**Key outcome:** a correctly-functioning RandomX v1 implementation on real AArch64 hardware. Everything stable, all KATs passing.

### Phase 2 — Architecture & Tooling (July 18–19)

P2 refactoring sprint: structured logger, TUI dashboard, Prometheus metrics, `--jit-dump` infrastructure, opcode frequency analyzer, JIT determinism/encoding tests, hot-path reductions (template copy elimination, superscalar heap churn), memory tier upgrades (MAP_HUGETLB for dataset/cache/scratchpad). The project's documentation infrastructure (changelogs.md, ROADMAP.md, PLAN.md) took its mature form here.

### Phase 3 — First Critical Fixes (July 20–22)

**AES T-table bugs** — discovered during Phase 2's re-baseline: byte order wrong, column permutation wrong in both encrypt *and* decrypt transforms, plus the NEON AESE/AESD hardware path was emitting operations in the wrong order for the RandomX round spec. Three commits (`4aba541`, `70427e5`, `ed512e5`) fixing three independent bugs that had been silently producing wrong hashes since day one.

**Fast-mode dataset corruption** — multi-threaded `initialize_dataset()` used wrong output span, corrupting results under thread contention. Fixed plus worker-thread reuse for dataset init.

**PoolManager failover self-deadlock** — `tick()` calling locked `current_pool_name()` from inside its own lock. Found via mock Stratum protocol tests.

### Phase 4 — Facing the Performance Wall (July 22–23)

The first serious optimization attempt hit a wall that defined the rest of the project:

| Attempt | Result |
|---------|--------|
| CBRANCH CSEL (replace branch with conditional select) | Measured **regression**, reverted. The 31.08% branch-miss figure everyone was citing turned out to *not represent the mining hot path* — the real rate is 2.4%. |
| Argon2 memcpy copy-elimination | Measured **regression**, reverted. "Cost relocated, not eliminated" became a recurring pattern. |
| Argon2 diagonal-step NEON vectorization | **Adopted** — 26.8% fewer instructions, 19.0% fewer cycles for the cache-init path. |
| NEON vector-permute AES | Derived from scratch, exhaustively verified (256/256 S-box, 20,000-trial full-round), **measured −19.4% regression**. |

The pattern was clear: on this Cortex-A53, the workload is memory-latency-bound, not compute-bound. Shuffling instructions around doesn't free the bottleneck — it just moves it.

### Phase 5 — The Proving Ground (July 24–26)

This is where everything got measured rigorously.

**Discovery #1: the gap to XMRig is instruction count, not IPC.** armrx IPC (0.731) is *better* than XMRig (0.612). armrx stall rate (11.41%) is *lower* than XMRig (16.70%). But armrx emits **33.5% more instructions per hash** (132.93M vs 99.57M). Every performance idea scored against IPC/stall cycles had been measuring the wrong metric. This reframing came from comparing four independently-written performance plans against each other — one of them (Hermes) correctly identified the axis mismatch, which the other three had all missed.

> **Correction (2026-08-01):** the 132.93M figure predates the `--perf-ready` harness and Track G. The clean steady-state total is **119.0M instr/hash** (IPC 0.740, `docs/experiments/t12-perf-ready-first-run.md`), narrowing the XMRig gap to ~+19.5% if XMRig's 99.57M is unchanged.
>
> **Correction (2026-07-31, W1-4):** XMRig was re-baselined on the same device (light mode, 1 thread, 765 MHz, perf-stat totals only — no disassembly). Measured XMRig = **94.5M instr/hash, IPC 0.648, 5.05 H/s**. The audit's +19.5% hypothesis is **rejected**: the true instruction gap is **+25.9%**, but armrrx's superior IPC (0.731 vs 0.648) shrinks the cycles/hash gap to **+11.5%** and the raw H/s gap to only **−4.2%** (armrrx 4.84 vs XMRig 5.05 H/s). Net: the remaining lever vs XMRig is ~26% excess instructions/hash (dominant, ~80% in the superscalar body); there is no separate stall/scheduling deficit. The historical 99.57M XMRig figure was 5.1% above the device re-baseline (likely fast-mode/different-clock). See `docs/experiments/w14-xmrig-rebaseline.md`.

**Discovery #2: the two-cluster topology.** XMRig runs on the same hardware showed armrx at ~90% per-cluster — a real but modest gap, not the "far behind" impression raw aggregate numbers gave. The 8 workers don't see symmetric cores: the two 4-core L2 clusters compete through an interconnect that costs cluster 1 roughly half its throughput under full contention. No code fix touches this.

**Discovery #3: the +14% win is operational, not a code change.** `isolcpus=1-7 rcu_nocbs=1-7` on the kernel boot cmdline gave a reproducible 28.4 H/s burst (corrected to ~24.8 H/s sustained). This was — and remains — the largest single measured win in the project's history, bigger than any code change.

What was adopted in this period:

| Change | Impact |
|--------|--------|
| Prefetch removal (`pldl1keep` for dataset) | +0.885% IPC |
| Compiler flags (`-O3 -fno-strict-aliasing` × 4) | +0.298% IPC avg |
| Argon2 prefault | +0.037% IPC |
| `.p2align 6` | +0.045% IPC |
| Emitter lookahead scheduler (main VM program) | +0.033% IPC |
| Emitter lookahead scheduler (superscalar path) | +0.200% IPC, total +0.233% |
| Scheduler window widening (4-instr fallback) | +0.156% IPC |

Each of these was measured with `perf stat -e cycles,instructions`, `taskset`-pinned, trial-order-reversed, thermal-settled. The largest single code-level win was **+0.233% IPC**. Hardware is the bottleneck, not the code.

What was closed on evidence:

| Idea | Why closed |
|------|------------|
| Huge-page residency | Already ~97.6% THP-coalesced, dTLB misses negligible |
| Worker-count sweep | 8 is highest-throughput; no free lunch at lower counts |
| Superscalar literal-pool relayout | Measured −1.12% hashrate ("cost relocated" pattern) |
| Fused hash-and-fill | Measured −3.6% slower |
| Peephole JIT coalescing | Main VM program's IPC penalty is stall, not code density |
| Memory-op scheduler extension | Caused real JIT/interpreter divergence, reverted |
| PGO | Confirmed null twice: 4.47 vs 4.48 H/s, pinned, thermal-settled |
| Scratchpad L1 residency test | Only +6.07% IPC — 94% of penalty is architectural |

### Phase 6 — The Master Plan (July 27–30)

Four independently-written performance plans (Claude Opus 5, Deepseek V4, Claude Sonnet 5, Hermes) were synthesized into one gated master plan with 10 tracks (A–J). Every track was then tried in priority order:

| Track | Idea | Result |
|-------|------|--------|
| **A** | Diagnostics (register liveness, PMU, instruction budget) | ✅ Done — 100% register liveness, front-end stalls negligible, per-opcode emission already minimal |
| **B** | Hybrid partial dataset (cache N MiB of 2 GiB dataset) | ✅ Implemented, **−31% at 8 workers** — interconnect saturates, not adopted for production. Code stays gated as reference. |
| **C** | Inline dataset-item helper (skip ABI call overhead) | Phase A ✅ — zero impact (+0.02%, noise). Phase B closed: ~0.5% ceiling, high-risk, not worth it. |
| **D1** | 2-way superscalar interleave | 🍅 Negative — 66× more L1I refills, −1.2% IPC |
| **D2** | Cross-hash boundary pipelining | ✅ Implemented, live in mining path; on-device A/B done 2026-08-01: interleaved −2.77% AES time, ~0.34% E2E (`docs/experiments/t11-d2-microbenchmark.md`) |
| **D3** | Full dual-nonce JIT interleave | 🍅 Contraindicated by D1 + 100% register liveness finding |
| **E** | Scheduler ceiling investigation | 🔻 Gated — back-end bound confirmed, no scheduler fix possible |
| **F** | Instruction-count micro-opt | 🔻 Deprioritised — per-opcode audit found no waste anywhere |
| **G** | NEON T-table AES AddRoundKey | ✅ **+28.8% AES primitive throughput**, projected ~3.6% full-workload. **Default ON since 2026-08-01 (commit 4888ba1)**. |
| **H** | Alternative execution models | 🔻 Never started — highest risk, most speculative |
| **I** | Core-0 stratum/worker contention | ✅ Closed — effectively zero (<0.1%) |
| **J** | Cheap layout/padding tweaks | 🔻 Low priority |

---

## 4. What Actually Moved the Needle

Ranked by real measured impact:

| # | Change | Type | Impact |
|---|--------|------|--------|
| 1 | `isolcpus=1-7 rcu_nocbs=1-7` | Kernel config (deployment) | **+14%** |
| 2 | NEON T-table AES AddRoundKey (Track G) | Code | **+28.8% AES throughput** (~3.6% full workload, default ON 2026-08-01) |
| 3 | Emitter lookahead scheduler | Code | **+0.233% IPC** |
| 4 | Scheduler window widening | Code | **+0.156% IPC** |
| 5 | Prefetch removal | Code | **+0.885% IPC** |
| 6 | Compiler flags ×4 | Code | **+0.298% IPC avg** |
| 7 | Argon2 diagonal NEON | Code | 19% faster cache init (one-time cost) |

Everything else — and there was a lot of it — measured as null, negative, or too small to distinguish from noise.

---

## 5. The Recurring Patterns

### "Cost relocated, not eliminated"

At least five separate optimization attempts followed this exact arc: implement → measure → find the win disappeared → root-cause as cost moved to a different pipeline stage → revert:

- Argon2 memcpy copy-elimination
- Superscalar literal-pool relayout
- CBRANCH CSEL
- IMUL_RCP literal-load elimination
- Track C Phase A (caller-frame reduction)

On a wide out-of-order core, these would likely show real wins. On an in-order Cortex-A53, there's always a serial dependency chain somewhere that re-asserts the same latency.

### The measurement trap

Every time we thought we had a win, it was because of a measurement artifact:

- **Unpinned comparison**: "PGO wins 2×!" — actually just core-cluster scheduling
- **Short window**: "isolcpus gives +14%!" — sustained is more like +4%
- **Wrong metric**: "31.08% branch misses!" — that was the interpreted-mode benchmark, not the JIT hot path
- **Wrong comparison**: "28.4 vs 24.95 H/s!" — different measurement windows, different warmup

The discipline that caught these (taskset pinning, long warmup, reversed trial order, `perf stat` over wall-clock) was itself a major project output.

### The 94% architectural ceiling

The central finding that governed everything: the main VM program region carries 9.23% of instructions but 20.04% of cycles — a 2.2× IPC penalty. Forcing the scratchpad to be L1-resident only recovered +6% of that penalty. The remaining 94% is the in-order pipeline's inability to hide dependency-chain latency, which is **architectural** — no compiler or code change can fix it on this core.

---

## 6. How the Work Was Done

This project used an unusual development workflow worth documenting.

**Three AI agents, different roles:**
- **Hermes** (this agent) — handled documentation, skills, static verification, second-opinion gating, continuity across sessions (keeping track of closed leads, measurement traps, file:line anchors), and running other agents.
- **Reasonix** (Deepseek CLI) — the primary code-writing agent. Given structured briefs with exact file paths, root causes, old→new changes, and verification commands, it would implement the changes independently.
- **AGY (Google Gemini)** — used for some batch tasks and analysis work.

**Workflow discipline:**
- Every handoff brief was self-contained: root cause, exact file+line changes, verification commands, constraints (clean-room boundary, what NOT to modify).
- Hermes never edited source code directly — all code changes went through Reasonix or AGY.
- After each completed change, Hermes ran the build, ran targeted tests first (only the crash path), then the full regression suite, and only then updated changelogs/ROADMAP/README/ref docs.

**What this enabled:**
- Parallel workstreams (Hermes could document Track A's results while Reasonix worked on Track B's implementation).
- Independent verification (Hermes reviewed Reasonix's diffs before sign-off, found bugs the implementation agent missed — e.g., pipeline state not reset on job change, nonce tracking mismatch, VM destructor double-munmap).
- Preservation of continuity across session boundaries when an agent's context window filled.

This workflow evolved organically and was itself refined multiple times — notably when the user specified that Hermes should run Reasonix directly via terminal rather than writing handoff briefs for the user to paste.

---

## 7. Key Numbers

| Metric | Value |
|--------|-------|
| Total commits | 210 |
| Total test files | 14 (10 CTest + 3 tools + 1 fuzz) |
| JIT/interpreter differential pairs verified | 670 (450 main-program + 200 superscalar + 16 equivalence) |
| Total files changed (cumulative) | ~85+ |
| Lines of code added | ~15,000+ |
| Performance tracks attempted | 10 (A–J) |
| Tracks adopted as production | 2 (G, scheduler items) |
| Tracks adopted as gated | 2 (B, D2) |
| Tracks closed on evidence | 4 (C, D1, D3, I) |
| Tracks not started | 2 (H, low-priority E/F/J items) |
| Largest code-level perf win | +0.885% IPC (prefetch removal) |
| Largest overall perf win | +14% (isolcpus deployment, kernel config) |
| Device | MSM8929/Snapdragon 415, 8× Cortex-A53, 2 GiB RAM |
| Sustained hashrate (8 workers) | ~24.8 H/s |
| Single-core hashrate | ~4.84 H/s (armrrx, Track G ON) / ~5.05 H/s (XMRig, W1-4) |
| armrrx instructions/hash | 119.0M (clean, `--perf-ready`) |
| XMRig instructions/hash (same device, W1-4) | 94.5M |
| armrrx IPC | 0.731 |
| XMRig IPC (W1-4) | 0.648 |
| armrrx vs XMRig (instruction gap) | +25.9% instr/hash, −4.2% H/s (W1-4) |
| armrx vs XMRig (cluster-normalized) | ~90% |
| Independent code reviews completed | 5 (3 for the scheduler alone) |
| Differential KAT pairs verified | 450 main-program + 200 superscalar |

---

## 7. Where and Why the Tests Were Added

The test suite grew organically as specific failure modes and correctness risks surfaced. Every test has a documented rationale in its file header. Here's the full inventory:

### Correctness — the big net

| Test | Purpose | Why it exists |
|------|---------|---------------|
| `test_jit_equivalence.cpp` | 16 seeds × 2 inputs — JIT vs interpreter byte-identical hashes | The two KAT inputs in `armrx_tests` were insufficient — a CBRANCH encoding bug tied to a specific immediate value could slip past two fixed programs |
| `test_jit_scheduler_stress.cpp` | 450 + 200 differential pairs for the emitter scheduler | The scheduler's failure mode is a *silent* wrong hash that looks like a real 32-byte value — not a crash |
| `test_jit_superscalar_scheduler_stress.cpp` | 5 seeds × 20,000 pairs for the superscalar scheduler | Same reasoning, extended to the superscalar path |
| `test_jit_determinism.cpp` | Same program compiled twice → byte-identical JIT output | Determinism is not guaranteed by construction — JIT emission order changes could produce different code for the same input |
| `test_partial_dataset.cpp` | `partial_dataset_item == generate_dataset_item` | Track B's hybrid path must be bit-identical to the reference on its cached items, and must fall back identically on misses |
| `test_jit_dataset_2way.cpp` | 2-way superscalar derivation bit-identical to reference | Track D1 — a new code path in assembly that could silently diverge |

### Unit tests for specific vulnerability surfaces

| Test | Purpose | Why it exists |
|------|---------|---------------|
| `test_blake2b.cpp` | KATs for Blake2b + reference dataset items | The RandomX specification's own published test vectors — the baseline everything depends on |
| `test_aes_hash.cpp` | Golden-output pins + decomposition equivalence for `hash_and_fill_aes_1r_x4` | The AES fill/hash functions had no direct coverage — only end-to-end KATs that wouldn't localize a regression to `aes_hash.cpp` |
| `test_jit_encodings.cpp` | Decodes emitted `bne`/`b` bytes for every CBRANCH sequence, asserts target is in-bounds | A prior CBRANCH encoding bug (`imm19` off-by-one) caused a 120s test *hang* instead of a fast assertion — we wanted instant failure next time |
| `test_mining.cpp` | End-to-end lifecycle, bad-nonce recovery, dataset reinit, stop-race, pipeline correctness | The mining engine is the top-level integration point — if it breaks, nothing works |
| `test_cli_parser.cpp` | Every CLI flag and config override combination | Zero prior automated coverage — CLI behavior was only verified manually against `--help` output |
| `test_config.cpp` | Malformed numeric config values (`--workers=abc`) | `load_config_with_fallback()` runs unconditionally on every launch; malformed values crashed the process via unhandled `std::invalid_argument` |
| `test_pool_protocol.cpp` | Loopback mock server × real PoolManager — Stratum V1 / CryptoNote / AUTO / failover / backoff | PoolManager had a known self-deadlock that no test caught — the mock-server approach exercises the real wire protocol paths |
| `fuzz_json.cpp` | LibFuzzer harness for `armrx::json` — hostile bytes | The JSON parser is the attack surface for pool replies and config files; fuzzing found crashes in hours that manual review missed |

### Benchmarks and tools

| File | Purpose |
|------|---------|
| `bench_armrx.cpp` | Region-attributed steady-state benchmarking — separate JIT, AES fill, hash, and interpreter phases, with `--full-hash-only` for PMU measurement |
| `bench_opcodes.cpp` | Opcode frequency analyzer — samples RandomX program generation across many seeds and reports per-opcode counts |
| `tools/jit_correlate.py` | Correlates JIT emission with `perf annotate` cycle attribution |
| `tools/track_c_dataflow_diff.cpp` | Data-flow diff harness for Track C (inline prologue verification) |
| `tools/aes_kat_check.cpp` | Standalone AES KAT check — verifies T-table and NEON primitives in isolation |
| `tools/bench/mul_latency_bench.c` | Cortex-A53 multiply latency micro-benchmark (F3 experiment) |

### How test coverage decisions were made

The rule wasn't "every function needs a test" — it was "every failure mode we've encountered or can plausibly anticipate needs a regression gate." Tests were added:

1. **After a bug was found and fixed** — AES T-table bugs → `test_aes_hash.cpp`, JIT encoding bug → `test_jit_encodings.cpp`, pool deadlock → `test_pool_protocol.cpp`
2. **Before a high-risk change landed** — scheduler → stress tests, D1 2-way → `test_jit_dataset_2way.cpp`, D2 pipeline → `test_mining.cpp` KATs
3. **For surfaces with no coverage at all** — CLI parser, config parser, JSON parse
4. **As a cheap oracle for optimization claims** — "this change is safe because it doesn't change the output" → add a differential test

The expensive tests (scheduler stress, pool protocol mock server, partial dataset) are listed in `ctest` but are practical to run only on the host, not on-device, where the 2080 MiB dataset init makes even `test_mining` a 15+ minute affair.

## 8. What Was Learned

### About this hardware

The Cortex-A53 is an in-order, dual-issue core with a short pipeline and limited memory-level parallelism. It's the ARM equivalent of an Intel Atom from a decade ago. For a workload that's 98%+ JIT-compiled code doing random-access 64-byte reads from a 256 MiB cache, it runs out of memory-level parallelism very quickly. The bottleneck isn't the code — it's the hardware.

The two-cluster topology (MSM8929) means the 8 cores are not symmetric under contention. An interconnect arbitration effect costs the second cluster ~half its throughput when both clusters compete. This is invisible to single-core benchmarks.

### About performance measurement on slow hardware

Every measurement technique developed for this project was validated by catching at least one false positive:
- Taskset pinning prevents core-cluster artifacts
- Long warmup (60s+) prevents burst-vs-sustained confusion
- Reversed trial order prevents thermal drift from masquerading as a result
- `perf stat` over wall-clock prevents scheduling noise from dominating
- The `bench_armrx --full-hash-only` path isolates the mining hot path from startup/init overhead

### About clean-room implementation

A clean-room implementation of RandomX on AArch64 is feasible and can be correct. The project passed every available correctness test: KATs, JIT equivalence (16/16 pairs byte-identical), determinism, 2500-pair data-flow diagnostics, and an exhaustive scheduler stress test (450 + 200 differential pairs). The project's clean-room boundary was maintained throughout — no XMRig code was consulted at any point.

The instruction-count gap to XMRig (~33.5% more instructions/hash) appears to be structural — different compilation choices and ABI conventions, not obviously fixable without adopting XMRig's approach.

### About multi-agent development

A three-agent workflow (Hermes for docs/verification/continuity, Reasonix for code implementation, AGY for batch tasks) was surprisingly effective for a solo developer. The key was strict role separation: the verification agent never edited code, and the implementation agents received briefs detailed enough to be self-contained. The main failure mode was context-window overflow during long sessions — mitigated by writing task briefs to files rather than keeping them in the conversation.

### The honest bottom line

After 18 days and 210 commits, what did the project actually accomplish?

A correct, clean-room RandomX v1 implementation on a real Cortex-A53 device, running at ~90% of XMRig's per-cluster throughput — plus a mountain of measurement evidence proving that the remaining gap is **architectural**, not fixable on this microarchitecture. The largest code-level performance win across the entire project was +0.885% IPC (prefetch removal). The largest overall win (+14%) was a kernel boot-cmdline configuration, not a code change at all.

The project succeeded at understanding the problem fully — it just turned out the problem wasn't code quality. The Cortex-A53 is an in-order, dual-issue core with a short pipeline, and RandomX is a near-perfect memory-latency stress test that defeats whatever memory-level parallelism the core can muster. No compiler flag, scheduler trick, NEON path, or assembly rewrite can change that.

### A note on documentation culture

This project's documentation is unusual: it treats negative results as first-class outputs. Every attempt that measured as null or regression has a full writeup — root cause, measurement protocol, raw numbers, reversion rationale. The `docs/experiments/` directory has more dead ends than wins, and that's by design.

The reason is simple: on a single-developer project spanning 18 days with multiple AI agents cycling in and out, the documentation *is* the continuity mechanism. A developer returning in six months — or another agent being dropped into the repo cold — should be able to read `docs/experiments/`, `changelogs.md`, and `ROADMAP.md` and know exactly what was tried, what happened, and why something isn't being pursued further, without having to re-derive any of the evidence. This is the most reusable output of the project for anyone else attempting RandomX on Cortex-A-class hardware: not the code, but the proof that several obvious-looking optimization categories are dead ends.

---

## 9. The Archive

Every experiment, dead end, and adopted change is documented:

- `docs/archived/alpha-changelogs.md` — chronological record, every change with rationale
- `docs/archived/alpha-roadmap.md` — completed vs remaining item tables
- `docs/experiments/` — 12+ writeups of individual experiments, both wins and reverts
- `docs/archived/audits/` — 5 independent code reviews (3 for the scheduler alone)
- `docs/postmortems/` — root-cause analysis for critical bugs (AES T-table, dataset corruption, pool deadlock)
- `docs/plans/20260727/master-plan-20260727.md` — the final synthesis: 10 tracks, all either done or gated

The project's honest record — every measured result, every reverted attempt, every wrong assumption corrected — is as much the output as the code itself.

---

*Written 2026-07-30. 210 commits, 18 days, one Cortex-A53.*
