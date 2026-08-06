# Full Project Audit — 2026-08-07

**Scope:** ground-truth review of current state. Read end-to-end: README.md,
TESTING.md, CMakeLists.txt, STRATEGY.md, AGENTS.md, ROADMAP.md, both
2026-08-07 handoffs (briefs/2026-08-07-handoff.md, briefs/2026-08-07-handoff-fill-optimization.md),
audits/residual-gap-optimization-roadmap.md, src/partial_dataset.cpp. Cross-checked
against `git log --all` and `devbox_status`. No code changes made; this file is NOT committed.

**Headline:** the code/perf program is effectively **closed** (H/s parity earned by
measurement: 1w armrx 5.11 vs XMRig 5.04; 8w 26.65 vs 28 = 95.2%). The remaining gaps
are **doc drift** (several docs contradict each other and current reality) plus a small
number of latent code risks and one untried perf probe.

---

## 🔴 DOC CONTRADICTIONS (high — actively mislead a reader)

### A1. Three different 8-worker numbers; README table is stale
- README "Performance Baseline" table: **8w = 24.95 H/s**, near-linear (73% scaling efficiency).
- ROADMAP.md (Lever-3 multi-worker bench, 2026-08-07, labelled authoritative): **8w all-cores = 13.37 H/s** (2.6× per-core; weak-cluster 50% drag).
- STRATEGY.md: **8w real-pool = 26.65 H/s** (95.2% of XMRig 28.0).
- README line 44 already admits "5.18 / 25.28 H/s, linear scaling" were *stale and didn't
  reproduce* — yet the table still shows **24.95**, essentially the same retracted figure.
  The table predates Lever 3 (`bench_armrx --workers=N` did not exist) and was never re-measured.
- **Action:** replace README table or explicitly relabel as pre-Lever-3. Recommend keeping the
  real-pool 26.65 H/s (8w) + 5.11 H/s (1w) figures and dropping the synthetic bench 13.37 number
  from the user-facing table, or showing both with provenance.

### A2. README states the wrong device clock
- README line 40: "8× Cortex-A53 (**4×1.1 GHz + 4×1.4 GHz**)".
- TESTING.md line 7, both 2026-08-07 handoffs, and `devbox_status` (`maxfreq` empty) confirm the
  device runs a **fixed 765 MHz firmware clock, no cpufreq** (no OPP table in DT). The 1.1/1.4 GHz
  is the rated part speed, not operating speed. Overstates the absolute ceiling ~1.5×.
- **Action:** correct to "MSM8929, 8× Cortex-A53, fixed 765 MHz (no cpufreq)".

---

## 🟠 STALE DOCS (medium)

### B1. STRATEGY.md "Known bugs" describes a project that no longer exists
STRATEGY.md is treated as canonical but lists bugs already fixed at HEAD (`2899f12`):
- "PartialDataset 512 MiB fill **stalls** on-device (OPEN… never completes)" → **FIXED**:
  Tier 1 `wait_for_fill` shipped (5e63942); fill completes in ~164 s clean dead-start
  (handoff verified on `lenovo`).
- TUI segfault with `ARMRX_DAG_SCHED=1` (OPEN) → **FIXED**: dangling `std::string_view pool_name`
  UAF → owned `std::string` (e347514), plus cout/mutex race (c664195).
- TUI garbage control bytes / overlapping lines (OPEN) → **FIXED** (c664195).
- SIGINT on `--pool` may not exit cleanly (OPEN) → **FIXED/VERIFIED** (1e5fc52 + 53fc420).
- "out-of-order publish (LATENT)… masked by the stall" framing is obsolete now that the fill
  completes (see 🟡 C1 for the still-real residue).
- **Action:** refresh the "Known bugs" section from the two 2026-08-07 handoffs; keep only the
  genuinely-open items (see 🟢 D section).

### B2. AGENTS.md Architecture contradicts its own Gotchas on AES
- Architecture (≈line 65): "NEON AES paths were removed… **All AES uses software T-table path**."
- Gotchas (≈line 154): correctly states hardware AESE/AESD is the **default aarch64 funnel** +
  NEON T-table default ON; T-table is the x86/non-crypto fallback.
- Hardware AESE/AESD adopted 2026-08-03 (Item 1). The Architecture bullet is stale.
- **Action:** update Architecture bullet to: "Hardware AESE/AESD zero-key funnel is default on
  aarch64+crypto; NEON T-table (Track G) default ON; scalar T-table is the x86_64 / non-crypto
  fallback and KAT oracle."

### B3. README "Core Features" overstates hugepages
- Line 27: "Automated huge-pages mapping (**MAP_HUGETLB** + MADV_HUGEPAGE) to eliminate TLB miss penalties."
- E9 (README line 134) documents MAP_HUGETLB **FAILED** (`HugePages_Total: 0`) and it falls back to
  anonymous + THP, which collapsed nothing. On this device MAP_HUGETLB is never actually used.
- **Action:** change to "THP-backed (MADV_HUGEPAGE); MAP_HUGETLB attempted but unavailable on
  device, falls back to anonymous + THP."

---

## 🟡 LATENT CODE RISK (real, low priority)

### C1. Partial-dataset out-of-order publish still in the code
`src/partial_dataset.cpp:197-203` publishes `item_count_ = max(completed)` — the per-worker END
bound — so a *lagging* chunk's items are advertised as ready before they are actually filled.
STRATEGY.md names the proper fix "contiguous publish" (advance only over the completed prefix), but
it was **never implemented**.
- Currently masked because `wait_for_fill()` (line 218) blocks all mining workers until
  `fill_complete_`, so nobody hashes during the fill window.
- Reopening risk: the JIT reads `item_count_` **directly, per hash** (per changelogs ca41554 /
  ff9b7f3) to decide hit/miss, so any relaxation of the `wait_for_fill` block reopens the
  wrong-hash window.
- **Action:** either implement contiguous publish (advance a monotonic contiguous bound in
  `fill_worker`) + add a KAT that stresses a lagging chunk, or add a code comment marking the
  `max()` publish as load-bearing-on-`wait_for_fill`.

### C2. bench_armrx `--workers` 2× discrepancy vs real pool
Lever 3 was built to give "TRUE N-worker aggregate H/s," but:
- `--perf-ready --workers=8` → **13.37 H/s** (ROADMAP, labelled "REPLICATE before trusting").
- real-pool mining `@8w` → **26.65 H/s** (authoritative per ROADMAP/STRATEGY).
The shared-cache / `--perf-ready` model is not representative of pool scaling on this device.
- **Action:** either investigate the bench methodology for an overhead bug, or explicitly relabel
  the `--workers` bench number as "not representative of pool throughput; pool number is authoritative."

### C3. TESTING.md §1 "pool idles ~41%" figure is suspect
TESTING.md line 31-33 states the pool miner idles ~41% of the time (not hashing), which is the
stated reason `--mine` is invalid for measurement. But pool H/s (26.65) is **2× higher** than bench
H/s (13.37), so relative to compute the pool is clearly not idling 41%. The 5-agent sweep (2026-08-07
handoff) already flagged this as "stale, verify before believing" — never re-validated.
- The "don't measure with `--mine`" rule is still correct (idle confound exists), but the 41%
  quantitative claim should be re-measured or softened to "pooled mining has stratum/network idle
  bursts that corrupt the cycles/elapsed ratio."

---

## 🟢 GENUINELY OPEN WORK

### D1. Lever 4 — cross-toolchain LTO A/B (the one untried perf probe)
- CMakeLists already fixed the fortify-headers/LTO crash (lines 402-413). GCC-16 cross-LTO was never
  actually A/B'd on-device; prior on-device LTO link measured +1.9%. Positive prior, crash risk
  (revert, one session), KATs gate correctness.
- **Action:** after `devbox_sync` (HEAD→device), run the LTO cross-build + KATs + a perf A/B as one
  gated device session.

### D2. Worker-local buffer reuse
- Kill the per-hash `std::vector` alloc in `worker_loop` (5-agent sweep). Free A/B, no correctness risk.
- **Action:** small refactor, host ctest + cross-build gate.

### D3. 2nd target (Unisoc / Termux)
- For build/test execution only, **explicitly NOT for metrics** (different silicon → numbers not
  comparable to MSM8929). No code work; just note it exists and is excluded from perf A/B.

### D4. Device is 6 commits stale
- Deployed `27e7c41` vs local HEAD `2899f12` (Tier-1 fill + seed-rotation fix + hybrid-consumption
  fix + doc corrections). `devbox_sync` required before any device test.

---

## CLEANUP (trivial)
- `feature/dag-scheduler` branch still exists locally + remote after the fast-forward merge to
  `main`. Safe to delete (`git branch -d feature/dag-scheduler` + `git push origin --delete`).
- `bench_armrx` is registered as a ctest (CMakeLists:221) — fine, just noting.

---

## RECOMMENDED NEXT SESSIONS (priority order)
1. **Doc reconciliation pass (zero-risk, no device needed):** fix A1 (README perf table + clock),
   A2, B1 (STRATEGY Known bugs), B2 (AGENTS AES), B3 (README hugepages), and add the C2/C3
   "not representative / to be re-validated" notes. These are all doc-only and user-approved.
2. **C1 decision:** implement contiguous publish + KAT, or annotate. (Correctness-adjacent; do
   before any relaxation of `wait_for_fill`.)
3. **D1 — cross-LTO A/B** after `devbox_sync`. The single real perf experiment remaining.
4. **D2** worker-local buffer reuse (cheap win, optional).
5. **D3** bring up 2nd target for build/test only.

**Bottom line:** no code-level perf lever remains untried (every JIT-reordering and fill
vectorization path is closed on evidence; H/s parity is achieved). The project is in a
"finish + document" phase. The audit's highest-value items are the doc contradictions (A1/A2)
and the stale STRATEGY "Known bugs" (B1), which currently misrepresent a shipped project as
bug-ridden and 2× slower than it is.
