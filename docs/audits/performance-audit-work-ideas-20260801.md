# armrx — Fresh Performance Audit & Work-Ideas Brief (2026-08-01)

> **Supersedes:** `docs/audits/combined-audit-20260731.md` (AGY + Reasonix).
> **Scope:** read-only audit of docs + code; prioritized performance work ideas with
> falsifiable measurement protocols. No source changes in this deliverable.
> **Hardware:** MSM8929 / 8× Cortex-A53 @ fixed 765 MHz, isolcpus=1-7, light mode,
> ~4.84 H/s single-core / ~18 H/s cross-built 7-worker / ~28.4 H/s native 8-worker isolated.

---

## 0. Executive verdict

The microarchitectural (IPC) axis is closed. armrx already beats XMRig on IPC
(0.740 clean steady-state vs XMRig's historical 0.612). Every low-risk IPC idea from
the 2026-07-31 combined audit is either **done** or **closed as a measured regression**.

What remains is almost entirely an **instruction-count / ABI-overhead / measurement-
rebaseline** problem — and several of the numbers the project still quotes are stale
or internally inconsistent. The highest-value next work is diagnostic, not clever JIT
surgery.

**Do not re-propose:** PRFM hints (T2-1), dual-issue NOP padding (T2-2), NEON AESE/AESD,
Track C Phase A as previously structured, memory-op scheduler extension, fill-loop PRFM,
IMUL_RCP preassignment, CSEL CBRANCH, Argon2 copy-elim / diagonal (already adopted where
it helped), scratchpad-locality chase, peephole coalescing, cpufreq/governor fixes.

---

## 1. Current ground truth (verified 2026-08-01)

| Metric | Value | Source |
|---|---|---|
| Clean single-core light-mode | **4.83 H/s median, 207 ms/hash** | `docs/experiments/t12-perf-ready-first-run.md` |
| Instructions / hash (steady-state) | **119.0 M** | same (`--perf-ready`, 500 hashes) |
| IPC | **0.740** | same |
| Branch miss | 3.11% | same |
| Core clock | **fixed 765 MHz** (no OPP table → no cpufreq) | same; AGENTS.md |
| Track G (NEON T-table AES) | **default ON** (`CMakeLists.txt:23`) | code + changelog |
| D2 interleaved hash+fill | **live in mining path**; microbench −2.77% AES time, ~0.34% E2E | `docs/experiments/t11-d2-microbenchmark.md` |
| Emitter scheduler | adopted, +0.233% IPC (superscalar) | RETROSPECTIVE / AGENTS |
| isolcpus pinning | workers 1–7, main on 0 | T2-3 done |

### Historical vs clean totals — treat carefully

| Figure | Value | Status |
|---|---|---|
| Historical armrx instr/hash | 132.93 M | Pre-`--perf-ready`, pre-Track-G-default; derived from sustained mining H/s |
| Clean armrx instr/hash | **119.0 M** | Post-Track-G-ON, `--perf-ready` gated; **prefer this** |
| Historical XMRig instr/hash | 99.57 M | Same-device light-mode, **not remeasured** against current armrx |
| Old gap | +33.5% instr vs XMRig | Stale if Track G moved the total |
| Implied gap if XMRig unchanged | **+19.5%** (119/99.57) | **Hypothesis only — re-baseline required** |

Delta 132.93 → 119.0 = **−13.9 M (−10.5%)**. Plausible contributors: Track G denser AES path,
measurement methodology change, other post-2026-07-24 work. **Not attributed.**

---

## 2. Doc / code status-marker audit (first-class findings)

The project already caught one false "✅ gate check done" on T3-3 (corrected in
`c29dd89`). This pass found more mismatches:

| # | Claim | Reality | Severity |
|---|---|---|---|
| D1 | `RETROSPECTIVE.md` Track D2: "On-device A/B not completed" | Done 2026-08-01; `t11-d2-microbenchmark.md` | **Stale doc** |
| D2 | `RETROSPECTIVE.md` Track G: "default OFF" | `CMakeLists.txt` default **ON** since 4888ba1 | **Stale doc** |
| D3 | `master-plan-20260727.md` Track G: "default OFF" | Same — ON | **Stale doc** |
| D4 | Master plan Track A: clean remeasure "tooling gap" / trust 132.93 M | `--perf-ready` closed the gap; clean total is **119.0 M** | **Stale plan number** |
| D5 | Master plan Track C Phase A: "DONE, code stays" | Reverted again in `27e7c41` (hang); symbols absent from tree | **Contradicts tree** |
| D6 | `NEXT_STEPS.md` Track C Phase A "code stays" | Same revert | **Stale archived tracker** |
| D7 | Combined audit T3-1 pitches "custom ABI retry ~1–3%" | Master plan Phase B/C already closed at **≤0.5% ceiling**, high risk; Phase A measured **zero** | **Overstates ROI / understates closure** |
| D8 | Combined audit T0-2 body has no ✅ while ranking says Done | Code is done (`online_cpu_count`, isolcpus cap) | Cosmetic inconsistency |
| D9 | Combined audit N3 "verify Blake2b uses NEON" | `src/blake2b.cpp:71+` already has full AArch64 NEON compress path | **Already implemented** — reframes to share/density check |
| D10 | Combined audit N7 cpufreq tuning | Device has **no** cpufreq policies (no OPP) | **Closed / impossible** |
| D11 | `CMakeLists.txt` NEON_AES help text cites `docs/neon-vector-permute-aes.md` | Real path: `docs/experiments/neon-vector-permute-aes.md` | Broken path |
| D12 | AGENTS.md "Experimental flags (all default OFF…)" | Omits that `ARMRX_ENABLE_NEON_TTABLE_AES` is **ON** | Misleading |
| D13 | Region table quotes "% of instructions" from `jit_correlate` | Tool counts **perf samples** (`perf record -e instructions` ≈ instr share, but absolute M/hash via ×132.93 is inconsistent with static 58.4 M ss body — see §3) | **Open reconciliation** |
| D14 | T3-3 "✅ gate check done" (historical) | Corrected; sampler was in progress | **RESOLVED** — real gate run **FAIL** (0.003% ≪ 30%) on 2026-08-01; `docs/experiments/t33-imul-magnitude-gate.md` |

**Recommendation:** treat `combined-audit-20260731.md` ranking table as historical; use
**this document** as the live backlog. Patch RETROSPECTIVE / master-plan Track G+D2 lines
and AGENTS experimental-flags blurb in a docs-only follow-up (out of scope here).

---

## 3. Instruction-count landscape (the real gap)

### 3.1 What is known tightly

From Track A / `--jit-dump` (master plan, 2026-07-27), **per-opcode JIT emission is already
minimal** for every high-frequency opcode checked (superscalar integer ops = 1 instr;
`IMUL_RCP` = 2 required; immediates at architectural encoding limits; FP memory ops pay
spec-mandated convert/mask). **Peephole density is not the remaining gap.**

Static superscalar body (exact):

\[
3563\ \text{AArch64 instr/call} \times 16384\ \text{calls/hash} = \mathbf{58.4\,M\ instr/hash}
\]

≈ **49% of the clean 119 M total**.

### 3.2 What is *not* reconciled

| Bucket | Estimate | Confidence |
|---|---|---|
| Superscalar scheduled body | 58.4 M | **Exact** (static × call count) |
| Main VM dynamic execution | historically ~9.23% → ~11 M @ 119 M | Medium — CBRANCH makes static≠dynamic; needs `--perf-ready` + region split |
| C++ outside JIT (AES/Blake/fill harness) | historically ~15.56% → ~18.5 M | Medium — Track G may have shrunk this |
| SS fixed wrapper (prologue/prefetch/mix/store) | historically ~2.49% samples | Medium |
| **Unaccounted / inconsistent** | historical "72.71% instr" × 132.93 M = **96.7 M** ss-attributed vs static **58.4 M** | **Open** — 1.65× mismatch |

The 58.4 M vs ~96.7 M superscalar attribution gap was flagged as "unverified ~96.6 M" in the
master-plan Hermes verification pass and **never closed**. With a clean total now available,
closing this is the single highest-leverage audit deliverable:

**Hypothesis A:** `perf record -e instructions` sample shares are biased vs true retired counts
on this PMU/kernel.
**Hypothesis B:** 3563 under-counts what lands in "opcode-attributed" samples (boundary table
sizing / multi-program structure).
**Hypothesis C:** call count or buffer layout assumptions are wrong for dynamic execution.

Until A/B/C are distinguished, **"33.5% more instructions than XMRig"** and any ROI claim
that assumes a known region owner for the gap are soft.

### 3.3 Main-VM 2.2× IPC penalty — status

Still real (9% instr / 20% cycles historically). Scratchpad L1-alias experiment recovered only
**+6% IPC** → **~94% architectural** (`docs/experiments/scratchpad-locality-bound-20260726.md`).
PMU split on main-VM-execute-only: back-end stalls ~26:1 over front-end
(`ld_dep_stall` 11.1%, `other_interlock_stall` 6.7%, `simd_dep_stall` 3.6% —
master plan Track A item 2).

Open (narrow): whether residual `ld_dep_stall` is **true DRAM/L2 latency**, **register
dependency depth after the load**, or **AGU interlock** — the L1-alias result already caps
pure-latency recovery. A PMU drilldown is informative; a code fix is unlikely to pay.

---

## 4. Status of prior audit tiers (verified)

| ID | Idea | Combined-audit status | Verified status (this audit) |
|---|---|---|---|
| T0-1 | Track G default ON | Done | **Done** — `CMakeLists.txt:23` ON |
| T0-2 | `hardware_concurrency` / online count | Done (ranking) | **Done** — `online_cpu_count()` + isolcpus worker cap |
| T1-1 | D2 on-device bench | Done | **Done** — live path; −2.77% AES micro, ~0.34% E2E |
| T1-2 | `--perf-ready` | Done | **Done** — first clean 119 M / 0.740 IPC |
| T2-1 | PRFM hints | Closed regression | **Closed** — do not reopen |
| T2-2 | Dual-issue alignment | Closed regression | **Closed** — do not reopen |
| T2-3 | isolcpus worker remap | Done | **Done** — `filter_to_isolated` all paths |
| T3-1 | Track C custom ABI | Blocked | **Reclassified:** Phase A hung×2 + zero when it ran; B/C closed ≤0.5%. See W3-1 |
| T3-2 | Load-address hoisting | Design only | **Still design-only / high risk** — see W3-2 |
| T3-3 | NEON mul lane-pack gate | Gate NOT done | **Sampler WIP** (uncommitted) — do not duplicate; see W1-2 |

### Novel angles N1–N8

| ID | Angle | Status after verification |
|---|---|---|
| N1 | `ldp`/`stp` fusion of adjacent scratchpad ops | **Open, speculative** — adjacency not statically obvious (`emitMemLoad` uses masked register+imm; RandomX addresses are data-dependent). Needs frequency study first. → W2-2 |
| N2 | Monolithic no-ABI register pinning | **Open, extreme risk/effort** — overlaps Track C moonshot; only if census proves frame overhead is the XMRig gap. → W3-3 |
| N3 | Blake2b NEON check | **Mostly closed as "missing NEON"** — NEON path exists. Residual: measure Blake2b's share of the 119 M and whether scalar fallback ever runs on device. → W1-3 |
| N4 | AArch32/Thumb-2 | **Deprioritize** — loses 64-bit GPRs RandomX needs; I-cache win speculative; front-end already <1% of cycles |
| N5 | XMRig black-box binary instr census | **Open, legal-gated** — highest direct answer to the gap; clean-room counsel before any disassembly. Prefer self-census (W1-1) + same-device XMRig `perf stat` totals first (no disasm). → W1-4 |
| N6 | BOLT | **Still never run** — expected null (JIT dominates). Cheap close-the-question. → W2-1 |
| N7 | Per-cluster cpufreq | **Closed** — no OPP / no policies |
| N8 | Hybrid JIT/interpreter main VM | **Educational only** — contradicts 94% architectural finding |

---

## 5. Prioritized work ideas (this audit)

Tiers follow the project's culture: **T0** trivial/docs, **T1** cheap measurements first,
**T2** medium A/B, **T3** design-first gated. ROI is honest and often diagnostic.

### Tier 0 — Docs / bookkeeping (no binary change)

#### W0-1. Sync stale performance status docs
- **What:** Update RETROSPECTIVE D2/G lines; master-plan Track G default + A remeasure note;
  AGENTS experimental-flags blurb; fix CMake NEON_AES help path.
- **ROI:** Prevents the next agent from re-opening closed work or citing 132.93 M as current.
- **Risk / effort:** Zero / 30–60 min.
- **Protocol:** Docs-only PR; no bench.

#### W0-2. Mark combined-audit-20260731 as superseded
- **What:** Banner at top pointing here; leave file as historical.
- **ROI:** Single source of truth.
- **Risk / effort:** Zero / 5 min.

---

### Tier 1 — Measurements that unblock everything else

#### W1-1. Region instruction census on clean `--perf-ready` total ★ TOP PRIORITY
- **What:** Attribute the **119.0 M instr/hash** into: main-VM JIT region, superscalar
  opcode body, superscalar wrapper, named C++ (AES fill/hash, Blake2b, Argon2 amortized≈0,
  other). Reconcile against static 58.4 M.
- **Why:** Per-opcode emission is already minimal; the XMRig gap must live in *structure*
  (call wrappers, main-loop glue, AES/Blake density, dynamic CBRANCH expansion) — nobody has
  a closed budget against the clean total.
- **Expected ROI:** 0% hashrate directly; **unlocks** every subsequent idea's ROI estimate.
  Without this, T3 ABI work and N2 are shots in the dark.
- **Risk:** Low (read-only tooling).
- **Effort:** 1–2 days on device.
- **Files:** `tools/jit_correlate.py` (extend or sibling), `tools/perf_ready_bench.sh`,
  possibly `dumpJitCode()` for wrapper symbol ranges.
- **Protocol:**
  1. Host: build + `ctest` smoke (interpreter paths).
  2. Cross-build, deploy.
  3. `taskset -c 3 tools/perf_ready_bench.sh` with `perf stat -e cycles,instructions` → confirm
     ~119 M ±2%.
  4. Same binary: `perf record -e instructions:u -c <large_period>` **and** separately
     `-e cycles:u` during a `--perf-ready` window only (attach after PERF_READY).
  5. Snapshot `/proc/<pid>/maps`, `--jit-dump`, run correlator; report M/hash per bucket
     (`share × 119.0 M`) **and** compare ss-opcode bucket to 58.4 M.
  6. Falsify: if ss-opcode attributed M/hash is within 5% of 58.4 M, Hypothesis B/C die and
     Hypothesis A (sample bias in older runs) or total-change explains the old 96.7 M figure.
  7. Write `docs/experiments/w11-instruction-census.md`.

#### W1-2. T3-3 IMUL magnitude gate — **DONE, FAIL (2026-08-01)**
- **Result:** `bench_imul_magnitudes` implemented (VM hook + self-checking sampler,
  committed) and run: **0.003% of 74.1M genuine IMUL_R executions have one operand
  ≤ 2^32** (gate ≥30%; both-operands 0.0003%; per-seed max 0.03%; cross-confirmed
  at 367.8M samples). **Gate FAIL — close F3 follow-on permanently; no NEON
  lane-pack design (W3-4 closed).** Writeup: `docs/experiments/t33-imul-magnitude-gate.md`.
- RCP-lowered correctly excluded (pointer identity, `isrc == &imm`). Runtime
  register values are effectively full-width random — 0.003% is ~65,000× above
  uniform-random expectation but ~10⁴× below feasibility.

#### W1-3. Blake2b NEON share check (N3 residual)
- **What:** Confirm device mining hits the NEON compress path; measure Blake2b's fraction of
  the 119 M / cycles.
- **ROI:** Likely null (historical named-symbol time ~0%); closes N3 properly.
- **Risk / effort:** Low / half day.
- **Protocol:** `perf record -g` during `--perf-ready` window; `perf report` for `blake2b` /
  compress symbols; if <0.5% cycles, close. Optional: force-compile without NEON and A/B
  (expect large init/hash regression only if Blake were hot — it shouldn't be).

#### W1-4. Same-device XMRig `perf stat` re-baseline (no disassembly)
- **What:** Remeasure XMRig instructions/hash and H/s on the **same** device, isolcpus,
  light mode, pinned core, long window — update the gap vs armrx 119.0 M / 4.83 H/s.
- **ROI:** Recalibrates the "10–12% / 33.5%" narrative; may shrink or grow the target.
- **Risk:** Legal — **totals and PMU only**, no `objdump` of XMRig JIT buffers (see N5).
- **Effort:** half–1 day.
- **Protocol:** Document job seed/template if possible; `perf stat -e cycles,instructions`
  on steady-state mining; compute instr/hash = instructions / hashes_completed; compare to
  armrx `--perf-ready`. Write experiment note. **Do not** proceed to N5 without counsel.

#### W1-5. Track G full-workload A/B (never measured E2E)
- **What:** `neon-ttable-aes.md` projected ~3.6% from 12.3%×28.8% but states **no full-workload
  A/B**. Default is now ON — still worth a one-time OFF vs ON hashrate confirmation.
- **Expected:** ~2–4% if cycle share held; could be less if Track G already shrank AES's share.
- **Risk:** Low (flag flip).
- **Effort:** half day device.
- **Protocol:** Two cross builds (ON/OFF), `taskset`-pinned `--perf-ready` or 180s mine,
  reversed order, thermal settle; report ΔH/s, Δinstr/hash, ΔIPC. Doc under
  `docs/experiments/track-g-e2e-ab.md`.

---

### Tier 2 — Cheap-to-fail experiments

#### W2-1. BOLT null-check (N6)
- **What:** One `llvm-bolt` profile-guided layout pass on `armrx` (+ `--emit-relocs`).
- **Expected ROI:** **~0%** (84%+ cycles in JIT W^X buffers BOLT cannot see). Purpose: close
  the lingering MidHigh Tier-2 item.
- **Risk:** Low correctness if A/B on KATs; build complexity medium on musl cross.
- **Effort:** ~1 day if BOLT available for aarch64; else **close as blocked-on-toolchain**.
- **Protocol:** Baseline `--perf-ready` → BOLT binary → same bench; adopt only if
  instr/hash or H/s improves ≥1% with KATs green. Else document null and close.

#### W2-2. Scratchpad-op adjacency frequency study (N1 gate)
- **What:** Before any `ldp`/`stp` fusion, measure how often consecutive main-VM memory ops
  target addresses that are statically or dynamically 8-byte-adjacent.
- **Expected ROI:** Unknown; fusion only pays if adjacency rate × issue savings beats
  "added instructions cost cycles" law (T2-1/T2-2/fill-PRFM).
- **Risk:** Analysis-only first.
- **Effort:** 1 day.
- **Protocol:** Interpreter hook or JIT dump pass counting consecutive `*_M`/`ISTORE` with
  same base register and imm differing by 8 within same scratchpad mask class; report rate
  over many seeds. **Gate:** if adjacent pairs <5% of memory ops, close N1. If ≥15%, design
  a correctness-preserving fusion prototype (separate T3).

#### W2-3. Main-VM stall taxonomy PMU drilldown (optional)
- **What:** On `bench_armrx --scratchpad-real` vs `--scratchpad-l1`, split
  `ld_dep_stall` / `agu_dep_stall` / `other_interlock_stall` to see what the +6% IPC recovery
  actually removed.
- **ROI:** Diagnostic; unlikely to unlock a code fix given 94% ceiling.
- **Risk / effort:** Low / half day.
- **Protocol:** Three grouped `perf stat` passes (A53 counter limit), both scratchpad
  conditions, `taskset -c 3`. Close further memory-latency ideas if `ld_dep_stall` barely
  moves under L1 alias (consistent with architectural interlock).

---

### Tier 3 — Design-first / gated (do not start cold)

#### W3-1. Track C — status correction (no retry without new diagnostic)
- **Combined-audit T3-1 reframed:** Phase A (drop x0–x3 save) measured **~0%** when it
  briefly worked, then **hung again** and was fully reverted (`27e7c41`). Phases B/C closed
  at **≤0.5%** ceiling with high register-conflict cost
  (`master-plan-20260727.md` Track C).
- **Do not** retry the same light-mode prologue memcpy splice.
- **Only reopen if W1-1 shows** dataset-item call/frame/wrapper instructions are a
  **multi-percent** slice of the XMRig gap (not the historical 0.05–0.1% Phase A math).
- **Mandatory diagnostic before any code:** full-VM call-site hang bisect plan from
  `docs/experiments/light-mode-dataset-item-prologue-attempt.md` + `docs/archived/plans/track-c-phase-a-bisection-plan-20260728.md` — the 2500-pair data-flow test alone is **insufficient**
  (it passed while the full VM hung).
- **ROI if somehow successful:** ≤0.5–1% on current evidence, not 1–3%.

#### W3-2. Conservative load-address hoisting (old T3-2)
- **What:** Hoist only address LEA/mask for `*_M`, keep LDR/STR in place.
- **Risk:** **High** — sibling of failed memory-op scheduler; x2 scratchpad base is shared.
- **Gates (all required):** (a) written hazard model for x2/tmp_reg; (b) swap-count budget
  bisection harness; (c) abandon on first `test_jit_equivalence` failure; (d) W2-3 suggests
  remaining stall is address-generation dominated (else don't bother).
- **ROI:** 0.2–1% speculative; prior art says null/negative.

#### W3-3. Monolithic mining trampoline (N2)
- **What:** Custom ABI for entire hash lifecycle — eliminate AAPCS save/restore around
  hot calls.
- **Risk / effort:** Extreme / weeks.
- **Gate:** Only if W1-1 + W1-4 show the majority of the armrx−XMRig instr gap is in
  **non-opcode frame/glue**, not AES/Blake/superscalar body.
- **Protocol:** Design doc + register map review before any `.S` landing; differential KATs
  per phase.

#### W3-4. NEON lane-pack IMUL (F3 follow-on) — **CLOSED (gate FAIL, 2026-08-01)**
- **Blocked on W1-2 PASS** → W1-2 **FAILED**: 0.003% of genuine IMUL_R have a
  narrow operand. Lane-packing premise falsified on runtime data distribution;
  do not reopen without new evidence (e.g. a different transform target).
- Additional design gate (historical): packing ≥2 independent IMUL_R into `mul v.4s`
  without GPR↔NEON moves on the critical path (F3 showed moves erase the win).
- ROI: 0–2% speculative; many ways to regress instruction count.

---

## 6. Explicitly rejected / do-not-file

| Idea | Why |
|---|---|
| PRFM in main VM or fill loops | T2-1 + 2026-07-24 removal: pure overhead on in-order A53 |
| Dual-issue NOP / AND xzr padding | T2-2: IPC↑ but cycles↑ |
| NEON AESE/AESD permute AES | Spec AddRoundKey order incompatible |
| Track C Phase A re-apply as before | Hung twice; zero impact when measured |
| Memory-op emitter scheduler | JIT/interpreter divergence, unreproduced mechanism |
| Peephole coalescing for main-VM IPC | Stall-bound; Track A says emission already minimal |
| Scheduler window widening beyond adopted | Already adopted where it helped; further widening null |
| cpufreq / governor / 1.1 GHz | No OPP table |
| PGO | Null twice |
| Track B hybrid partial dataset @ 8 workers | −31% interconnect |
| Track D1/D3 interleave | L1I thrash / contraindicated |
| N4 AArch32 | Wrong trade for RandomX 64-bit state |
| N8 hybrid interpreter | Educational only |
| Duplicating `bench_imul_magnitudes` | WIP elsewhere |

---

## 7. Suggested execution order

```
W0-1/W0-2 (docs sync)
    │
    ├─► W1-1 instruction census ★
    ├─► W1-2 T3-3 gate (parallel; other agent)
    ├─► W1-4 XMRig total re-baseline (parallel; no disasm)
    └─► W1-5 Track G E2E A/B (parallel; cheap)
         │
         ├─► W1-3 Blake share (quick close)
         ├─► W2-1 BOLT null-check
         └─► W2-2 N1 adjacency gate
              │
              └─► only then consider W3-* gated by census numbers
```

---

## 8. Measurement protocol template (all perf ideas)

1. **Host first:** `cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure` (interpreter / non-JIT tests).
2. **Cross:** `cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake -DARMRX_DISABLE_LTO=ON && cmake --build build-cross -j$(nproc)`.
3. **Device:** pin to fast cluster (`taskset -c 3` single-thread; workers 1–7 for mine); never expect cpufreq changes.
4. **Steady-state only:** `bench_armrx --full-hash-only --perf-ready` + `tools/perf_ready_bench.sh` (or device fifo equivalent).
5. **A/B:** reversed trial order, thermal settle, report cycles + instructions + IPC + H/s; md5/JIT equivalence when touching JIT.
6. **Adopt iff** hashrate improves **or** (for diagnostics) the falsifiable question is answered; instruction-count wins that regress H/s are rejects (project standing rule).

---

## 9. Files / docs consulted

- `AGENTS.md`, `RETROSPECTIVE.md`, `ROADMAP.md`, `changelogs.md`
- `docs/audits/combined-audit-20260731.md`
- `docs/plans/20260727/master-plan-20260727.md`
- `docs/experiments/*` (all 20 experiment writeups)
- `docs/archived/plan_phase7_completed.md`, `docs/archived/NEXT_STEPS.md`,
  `docs/archived/plans/performance-plan-20260725.md`
- Code: `CMakeLists.txt`, `src/blake2b.cpp`, `src/cpu_features.cpp`, `src/cli_parser.cpp`,
  `src/mining_engine.cpp`, `src/jit_compiler_a64.cpp` (`emitMemLoad*`),
  `src/vm.cpp` / `include/armrx/vm.hpp` (uncommitted T3-3 hook),
  `tests/bench_imul_magnitudes.cpp` (uncommitted), `tools/jit_correlate.py`,
  `tests/bench_armrx.cpp`

---

## 10. One-page ranking (live backlog only)

| Rank | ID | Idea | Likely gain | Risk | Ready? |
|---|---|---|---|---|---|
| 1 | W1-1 | Clean instruction census vs 119 M | Diagnostic ★ | Low | **Ready** |
| 2 | W1-2 | T3-3 IMUL magnitude gate | Gate only | Low | **DONE — FAIL** (0.003%; closed) |
| 3 | W1-4 | XMRig total re-baseline | Recalibrates gap | Low+legal | **Ready** |
| 4 | W1-5 | Track G E2E A/B | Confirm ~2–4% | Low | **Ready** |
| 5 | W0-1 | Stale-doc sync | Process | None | **Ready** |
| 6 | W1-3 | Blake2b share close | ~0% | Low | **Ready** |
| 7 | W2-1 | BOLT null-check | ~0% | Low | Ready if toolchain |
| 8 | W2-2 | N1 adjacency frequency | Gate | Low | **Ready** |
| 9 | W2-3 | Stall taxonomy PMU | Diagnostic | Low | Optional |
| 10 | W3-4 | NEON lane IMUL | 0–2% | High | **Closed — gate FAIL** |
| 11 | W3-1 | Track C reopen | ≤0.5–1% | **Very high** | **Blocked on W1-1 + hang diagnostic** |
| 12 | W3-2 | Address hoisting | 0.2–1% | **High** | Design + gates |
| 13 | W3-3 | No-ABI trampoline | Unknown | Extreme | Blocked on census |
| — | N4/N7/N8 | — | — | — | **Closed / skip** |
| — | N5 disasm | XMRig JIT compare | Large info | Legal | Counsel first |
