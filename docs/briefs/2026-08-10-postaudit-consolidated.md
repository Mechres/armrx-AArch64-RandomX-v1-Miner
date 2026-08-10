# armrx — Post-Audit Consolidated Review (2026-08-10)

> **Scope.** Combines the Luna (GPT-5.6) and Deepseek post-audit reviews. The
> Tiers 1–7 adoption work from `2026-08-09-audit-consolidated.md` is already
> merged/closed; its per-item adoption is **not re-verified here** (skipped per
> request). This document covers the *forward-looking* material only:
>
> - Tier 8 status (remaining perf probes)
> - Closure-record hygiene — checks on levers labeled "closed on evidence"
> - Experimental / new ideas (deferred, not evidence-backed wins)
> - The new high-severity findings
>
> **Hermes source-check note.** The three load-bearing new claims (hybrid
> `--dataset-mb` correctness, ledger "worker-local buffer reuse" staleness,
> FDIV/FSQRT register clobber) were verified against source — see §4. Even
> where Deepseek/Luna agree, claims are tagged by source and by my own check.

---

## 0. Status going in

- **Tiers 1–7:** shipped and merged (separate doc). Not re-litigated here.
- **Tier 8:** the only remaining audit area — gated, deferred, low-priority.
- **Fleet:** Lenovo (primary, fixed 765 MHz), Redmi 7A/pine (DVFS, ~1708 MHz,
  ~80 H/s — best measurement platform), Unisoc SC9863A (A55, 6 reachable cores,
  3.86 GiB RAM — never benchmarked; only device capable of fast mode).
- **Perf ceiling:** armrx is ~90% of XMRig per-cluster (cluster-normalized);
  remaining ~10–12% gap is an **instruction-count** gap, not stalls/scheduling.
  IPC is *better* than XMRig (0.731 vs 0.612). Superscalar body = 5,224 A64
  instr/call (W1-1 census).

---

## 1. Tier 8 — remaining perf probes (status)

| ID | Probe | Verdict (both audits agree) |
|----|-------|------------------------------|
| T8-1 | **LTO probe** | Stale for the *target* (musl cross) build: `cmake/toolchain-aarch64-musl.cmake:21-27,61` force-disables LTO because the cross toolchain lacks `liblto_plugin.so`. On-device **native** LTO A/B already measured **+1.9%** (4.32 vs 4.24 H/s, `docs/experiments/perf-tracking.md:176`). Remaining work = acquire a cross toolchain with a working AArch64 LTO plugin, not a source optimization. |
| T8-2 | **Superscalar timing-model retune** | **Remain closed.** `src/superscalar.cpp:85-110` (x86-derived MacroOp model), `:497-546` (port scheduling), `:568-639` (program generation) produce *consensus-critical* programs. Changing the schedule changes generated programs → changes hashes. If forced to generate byte-identical programs, it cannot improve the model. Closed by **consensus constraint**, not merely "one more design remains." Three designs tried: two broke reference hashes, one was a no-op. |
| T8-3 | **Fleet re-validation** | Valid operational/portability work, **not** an H/s lever. Highest info-per-effort, **zero code risk**. Detected by `src/mining_engine.cpp` (hwloc + `cpuinfo_max_freq` ordering, `:49-169`; `include/armrx/cpu_features.hpp:16-24` online-CPU count from sysfs). Risk: frequency sorting is a heuristic; equal/missing freq → treated as one cluster. |
| T8-4 | **AES generator scalar XOR** | Only plausible small *code* probe. `include/armrx/aes.hpp:350-369` byte-wise XOR loops; reached via `src/aes_generator.cpp:44-60`; `src/vm.cpp:763-772` fills the generator per VM run. Census attributes only 359 samples to `AesGenerator4R::next` vs 101,556 total. **Disassemble the AArch64 binary once**: if the compiler already vectorized the 16-byte loop, close it; else max whole-hash effect < 0.5%. |
| T8-5 | **Fresh weighted JIT census** | Diagnostic, not an optimization lead. `docs/briefs/2026-08-09-superscalar-disassembly-findings.md:32-128` already confirms direct ops = 1 A64 instr, IMUL_RCP = min 2-instr (ldr+mul), C* padding deliberate (removing it = −7.1% H/s), no safe redundancy lever. Weighted multi-seed census improves doc confidence only. |
| T8-6 | **Argon2 / Blake2b** | Remain closed for steady-state. `src/argon2.cpp:100-219` NEON, `:300-364` cache init is seed setup not steady-state. `src/blake2b.cpp:69-183` NEON compression. Blake2b parity is an armrx *win* (`perf-tracking.md:167-173`). Do not rewrite in asm for H/s. |

**Deepseek prioritization of the above:** (1) fleet re-validation, (2) LTO A/B on
a non-musl build, (3) wire the on-device CI runner (T5-A's missing half), (4)
close the small open-bug backlog (TUI cursor restore, inert `subscribe_try_`
fallback, tests for TUI/metrics/TLS), (5) open-source prep (LICENSE, hardcoded
wallet removal — user-deferred), (6) low-odds perf (timing-model, AES XOR,
weighted census).

---

## 2. Closure-record review ("closed on evidence" checks)

Both audits converge: the *evidence discipline is strong*, but several closure
**records are broader than the experiments support**. Per the canonical
per-lever-family rule, a single failed design closes only that design, not the
family.

### 2A. Luna — 10 closure-overstatement findings

| # | Lever | Severity | Finding (file:line evidence) |
|---|-------|----------|-------------------------------|
| 1 | Hybrid dead-stop "three variants" | Medium | `docs/briefs/2026-08-07-hybrid-deadstop-variants.md:35-41` — Variant 2 was *untested co-located fill*; `:85-88` the branch changed only comments (`exclude_cores={}` already on main). The 178s/28.53 H/s result is the **baseline, not an experiment**. Steady-state conclusion sound; "three variants failed" is inaccurate (only 1 & 3 were behavioral). |
| 2 | Superscalar timing-model | Medium | Attempted designs correctly closed; the explicitly different "real A53 cost model" was never implemented. Not a safe perf lever (changes hashes). |
| 3 | `*_M` scheduler | Medium | `closed-levers-ledger.md:51-59` correctly records divergence; `src/jit_compiler_a64.cpp:431-440` excludes memory ops from long-latency scheduling; `next-iteration-plan.md:192-218` leaves narrower subsets untested. Exact impl closed; different mechanism could exist (high-risk, de-prioritized). |
| 4 | Argon2 copy-elimination | Medium | Failed design well-supported (`argon2-compress-copy-elimination.md:120-147`); that doc `:148-152` leaves a different memory-layout/cache strategy unexplored. Copy-elimination closed; family not proven closed (alt layout high-risk, no cache-pressure evidence). |
| 5 | N1 load-pair fusion | Low | `w22-n1-adjacency-analysis.md:110-122` proves `emitMemLoad()`/`ISTORE` can't use immediate-offset ldp/stp with current addressing; `:124-132` identifies adjacent FP loads in the *static* main loop; `jit_compiler_a64_static.S:243-297` interleaves them with integer/FP work. JIT mem-op closure correct; static FP-load pairing is a distinct tiny unmeasured cleanup. |
| 6 | Main-thread policy | Low | `closed-levers-ledger.md:101-128` — `nice=10` measured null; `SCHED_IDLE` not tested. Closing `nice=N` correct; policy family not exhaustively closed (no evidence of main-thread contention to justify more). |
| 7 | **Worker-local buffer reuse** | **Medium (doc defect)** | `closed-levers-ledger.md:137-141` says OPEN/untried, but `src/mining_engine.cpp:652-655` reuses `next_block` instead of declaring inside loop, `:690-693` swaps with `block_input`; `README.md:189-191` records it adopted. **Ledger is stale** — see §4 (Hermes confirmed). |
| 8 | LTO "open +1.9%" | Low | `CMakeLists.txt:417-455` supports LTO; musl toolchain disables it (no plugin); `perf-tracking.md:176` already records on-device +1.9%. LTO already measured positive on device-native; remaining work is toolchain, not an untested code opt. |
| 9 | Hugepages | Low | `src/argon2.cpp:269-290`, `src/partial_dataset.cpp:35-66`, `src/vm.cpp:132-142` attempt hugepages + prefault; `perf-tracking.md:161-171` supports closing generic tuning; other records show ~+0.8% when provisioning succeeds. Correct statement: "code strategy adequate; provisioning can still affect perf," not "hugepages never matter." |
| 10 | Historical C documentation | Medium (doc defect) | `perf-tracking.md:175` describes an LDR-pool form as *current*; `src/jit_compiler_a64.cpp:1492-1511` uses the E24 three-instr MOVZ/MOVK+ALU form; `2026-08-09-superscalar-disassembly-findings.md:87-111` correctly documents LDR regressed H/s. Authoritative = E24; older wording stale/misleading. |

### 2B. Deepseek — closure verdict table

| Lever | Closure quality | Verdict |
|-------|-----------------|---------|
| PRFM scratchpad hints (T2-1) | Sound (2 designs, mechanism-level) | ✅ Correctly closed |
| CBRANCH CSEL | Premise falsified (2.4% not 31%) | ✅ Correctly closed |
| `-mtune=cortex-a53` | Measured null (E18, clean A/B); already ships in toolchain | ✅ Correctly closed (08-06 brainstorm premise was false) |
| PGO | Measured null twice incl. over-training control | ✅ Correctly closed |
| Argon2 copy-elimination | Exemplar methodology (instr ↓2.86%, cycles ↑0.35% → reverted) | ✅ Correctly closed |
| Dual-hash interleaving | Binary premise, conclusive | ✅ Correctly closed |
| D2 pipelining | Not a closure (code live, E2E never A/B'd) | ⚠️ Unmeasured, not closed |
| Tier 2(b) per-family (3 designs) | Family rule respected; branches unmerged | 🟡 Correct, but Design B closed on an *implementation bug*, not measured negative |
| FDIV/FSQRT | Weakest measurement (single-pair, PGO-era) + latent correctness bug | 🔴 Closure defensible, evidence thin |
| DAG scheduler | Root cause wrong in 2 docs; family closure overreaches | 🔴 Reopenable |

### 2C. The two real 🔴 findings (must be resolved)

**🔴 (a) Hybrid `--dataset-mb>0` correctness contradiction — highest-value follow-up.**
Three records at HEAD disagree:
- `README.md:167-171` — "✅ **Correct and recommended as of 2026-08-07** ... end-to-end hashes match the light reference for any `--dataset-mb>0` job — verified on-device same-nonce."
- `tests/test_mining.cpp:438-448` — the hybrid consumption path "is currently **NOT producing correct end-to-end hashes on-device** (a single non-rotated job with `--dataset-mb>0` is also wrong vs a light reference)" — the test deliberately byte-compares generated items rather than engine hash, explicitly because the engine hash would fail.
- `commit 3272735c` message claims the fix; the same commit's test comment contradicts it.

Both cannot be true, and `--dataset-mb=512` is the README's **recommended real-world config** (~30–32 H/s). If the test comment is accurate, the README steers users into a wrong-hash path (pool rejects shares). **Gate:** write the missing same-nonce end-to-end KAT (hash with `--dataset-mb>0` vs light reference, same nonce) and settle it **on-device**.

**🔴 (b) DAG scheduler closure overreaches + stale wrong root cause — RESOLVED (doc) 2026-08-10.**
`docs/briefs/2026-08-06-dag-scheduler-attempt.md:13` and `ROADMAP.md:37` both stated "reorder forces a longer emitted sequence" — but the 2026-08-07 correction (`changelogs.md:418,429`) proved emitted length is **order-invariant**; the −14.3% was **planner runtime overhead** (O(n²) hazard matrix + ~9 heap allocs/hash, sys time tripled). Both docs corrected: "exhaustively closed" → "current implementation closed-negative; family not exhaustively disproven" (cheap planner = legitimate reopening variant, small ROI). No code change.

**Latent bug (confirmed in code, shipping-safe):** the `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` NR path (`src/jit_compiler_a64.cpp:2074` FDIV uses v0–v2, `:2118` FSQRT uses v0–v3) clobbers NEON registers that `emitMovImmediate`'s 32-bit immediate path reads (v0–v15 via smov/umov). The flag is **OFF by default** (`CMakeLists.txt:35`), so shipping is unaffected — but the "kept as opt-in" posture is unsafe, and the perf closure rests on a single-pair PGO-era A/B. **Action:** either fix the register allocation or delete the path before any adoption.

---

## 3. Experimental / new ideas (deferred — not evidence-backed wins)

### 3A. Luna — 8 remaining ideas
1. **Post-fill hybrid JIT entry** (`jit_compiler_a64_static.S:637-671` reloads PD metadata every call). Specialized entry post-fill could save ~4–6 instr × 16,384 = 65k–100k instr/hash (<0.1%). Risk: very high (prior inline-hybrid crashed on offset/register bugs). Gate listed. Only if partial-dataset becomes primary mode.
2. **AArch64 Clang matrix** — JIT `.S`/emission won't change; effects limited to Blake2b/AES wrapper/mining glue/dataset init. Unknown, probably small. Risk: FP behavior differences.
3. **A55-specific E24 validation** — E24 C* pad tuned for A53 MAC interlock; A55 differs. Compare 3-instr form vs denser literal-load on A55; keep A53 default. Device-specific; byte-identical required.
4. **Better topology detection** — use `related_cpus`/`cluster_id`/`cpu_capacity` not just `cpuinfo_max_freq` (`mining_engine.cpp:70-99,104-133`). Operational, not per-hash codegen.
5. **Adaptive partial-dataset sizing** — pick largest useful prefix by RAM/workers/fill-duration/hit-rate/interconnect. Product/runtime experiment, not hot-loop opt.
6. **Fixed-size block-ops audit** — `aes_generator.cpp:10-17,45-60`, `aes_hash.cpp:150-161`, `vm.cpp:974-987`. Disassembly only; <0.5% if scalar where NEON legal.
7. **Harden superscalar model without changing consensus** — diagnostic only (model A53 costs, compare predicted vs measured); do not generate production programs until byte identity proven.
8. **Permanent JIT dump diagnostic** — promote the guarded raw JIT-byte dump (no runtime cost when disabled). Useful for opcode accounting + regression detection.

### 3B. Deepseek — 5 genuinely-new ideas
1. **Startup JIT autotune** ⭐ (top pick) — ~1–2 s on-first-start microbench A/Bs pad width + scheduler window, caches winner (`~/.armrx/autotune.json`). Machinery cheap (ARMRX_NO_E24_PAD variant exists; timing 500 hashes is solved in `bench_armrx`). Low risk, fully portable; converts "tuned for Lenovo@765" → "tuned for whatever it runs on."
2. **Cluster-aware JIT emission variants** — Lenovo weak cluster (cores 4–7) loses ~50% to interconnect arbitration. Emit a weak-cluster variant (extra padding/interleave) for workers pinned there; fast-cluster gets current codegen. Medium risk, real potential on the 8w number.
3. **PRFM on fast-mode dataset reads** — T2-1 PRFM targeted scratchpad LDRs (regression); fast-mode dataset reads are DRAM-class (untried, different latency profile). Zero-risk to measure; gated on fast-mode bring-up (Unisoc).
4. **Cross-worker superscalar JIT dedup at seed rotation** — all 8 workers compile identical per-seed programs each rotation. Shared RO compiled-program cache (one compiles, others map) cuts rotation latency ~8×; steady-state gain small (compile = 1.76%) but low-risk (buffer immutable post-emit).
5. **Live per-worker H/s health rebalancing** — Redmi DVFS/thermal: detect stuck worker, re-pin to cooler core. Redmi-specific, operational, testable.

### 3C. Documented-but-never-tried (deepseek)
- **IMUL_RCP register pre-assignment bisect** (mid-high risk) — first attempt diverged JIT/interpreter, mechanism unidentified; bisection plan never executed. One saved LDR per IMUL_RCP in the superscalar body (80% of instructions).
- **BOLT** post-link binary layout — never attempted; toolchain-level, needs non-musl build.
- **Weighted-seed opcode census** — pure measurement.

### 3D. What neither would pursue (closed with evidence)
Blake2b/Argon2 asm rewrite, more JIT scheduler reorderings, more literal-pool density (E24 proved fewer instr → less H/s), more PRFM/alignment/NOP, generic C++→asm.

---

## 4. Hermes source verification of the three load-bearing new claims

| Claim | Source | Hermes check (file:line) | Result |
|-------|--------|---------------------------|--------|
| Hybrid `--dataset-mb>0` wrong on-device | Deepseek 🔴(a) | `README.md:167-171` ("✅ correct & recommended, verified same-nonce") vs `tests/test_mining.cpp:438-448` ("NOT producing correct end-to-end hashes on-device") | **RESOLVED on-device 2026-08-10.** The sibling test `test_light_mode_partial_dataset_matches_reference` (`:475-529`) is the same-nonce end-to-end KAT: hashes with the partial-dataset (hybrid) path and asserts `engine_hash == light-reference_hash`. Built aarch64-musl `test_mining`, ran on Lenovo (PID 1699 + foreground re-run): `TEST_MINING_RC=0`, `ALL MINING TESTS PASSED`, including `test_light_mode_partial_dataset_matches_reference passed`. **README is correct; the `:438-448` comment was STALE** (claimed the hybrid path is wrong on-device — false). Comment corrected in-tree; README unchanged. No live wrong-hash bug. |
| Ledger "worker-local buffer reuse" stale | Luna #7 / Deepseek #7 | `mining_engine.cpp:476-477` (`block_input`/`next_block` per-worker local vectors), `:554` copies template into existing buffer (no per-iter realloc) | **CONFIRMED stale.** Reuse adopted; ledger `closed-levers-ledger.md:137-141` wrong. Fix ledger (doc-only, safe). |
| FDIV/FSQRT v0–v3 clobber | Deepseek (latent) | `jit_compiler_a64.cpp:2074` (FDIV v0–v2, comment "safe — not used by other handlers"), `:2118` (FSQRT v0–v3); flag `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` OFF by default (`CMakeLists.txt:35`) | **PARTIALLY confirmed.** Code uses v0–v3 in the opt-in path; flag OFF → shipping-safe. Latent risk real; verify with flag ON + reference KAT before any adoption. |

> Note: my T7 ledger note incorrectly asserted the D2 "OPEN" line was correct.
> Luna/Deepseek are right — it is stale (the *buffer reuse* is a different lever
> from README's adopted "Cross-hash boundary pipelining (Track D2)"). Corrected here.

---

## 5. Recommended remaining work (combined)

**From Luna:**
1. One-time AArch64 disassembly check for AES generator XOR loops (T8-4).
2. Acquire/build cross toolchain with functional AArch64 LTO; run LTO A/B (T8-1).
3. Fleet topology/fast-mode validation matrix on Redmi + Unisoc (T8-3).
4. Add `PartialDataset` lifecycle stress tests (destruction during fill, repeated refill, cancellation, exception teardown, sanitizer runs).
5. Mark superscalar retuning + weighted JIT density as closed (unless new evidence).

**From Deepseek:**
1. Resolve the hybrid contradiction — one device session, same-nonce end-to-end KAT. If wrong: `--dataset-mb` mines rejected shares; if right: delete stale test comment + add KAT permanently.
2. Fix or delete the NR FDIV/FSQRT path (register allocation or removal) so the opt-in flag is safe.
3. Correct the DAG root cause in the brief + ROADMAP; downgrade "exhaustively closed" → "this implementation failed" in the ledger.

**Hermes additions:**
- Fix the ledger "worker-local buffer reuse" staleness (§4, verified safe).
- Open-source prep (LICENSE + remove hardcoded wallet at `src/cli_parser.cpp:443`) remains user-deferred.

---

## 6. Bottom line

There is **no remaining high-confidence source optimization** in Tier 8. The only
plausible code-level probe is the AES generator XOR loop, and it is likely below
the measurement threshold. The larger open items are **toolchain validation**
(LTO cross-plugin), **fleet portability** (topology/fast-mode on Redmi/Unisoc),
**lifecycle-test quality** (`PartialDataset`), and — most urgent — the **hybrid
`--dataset-mb>0` correctness contradiction**, which must be settled on-device
before the README's recommended config can be trusted.

Everything in the experimental-ideas sections (§3) is either portability work,
diagnostic tooling, or a different-device experiment — deferred by design per
the canonical "closed-on-evidence / per-lever-family" rules.
