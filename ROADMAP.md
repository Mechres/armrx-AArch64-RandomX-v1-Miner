# armrx — Status Tracker

> **Status tracker for completed and remaining work.**
> For the strategic master plan with ranked priorities, see [`PLAN.md`](PLAN.md) (current: Phase 6).
> For the chronological record, see [`changelogs.md`](changelogs.md).
> For the full narrative behind everything already completed (Phases 1–5), see
> [`docs/archived/plan_completed_phases_1-5.md`](docs/archived/plan_completed_phases_1-5.md).

> **Current status (2026-07-24):** Phases 1–5 are fully resolved (correctness fixes, structural
> refactors, test coverage, and the CBRANCH/Argon2/PGO/NEON-AES/`--stagger-ms` performance
> investigations — see the archive linked above). **Current work is Phase 6**: two independent
> performance master plans (`docs/plans/performance-master-plan.md`, `docs/plans/performance-master-plan-20260724.md`)
> were reconciled into one adopted plan. Both agree the device runs **light mode** (2 GiB RAM
> can't fit the fast-mode dataset) and IPC 0.708 means the workload is memory-latency-stall-
> bound, not instruction-throughput-bound. **Items 1–2 (the two highest-EV short-term
> verifications) are now done, both closed as no-ops/negative-but-useful results**: huge-page
> residency is already ~97.6% THP-coalesced (no hugetlbfs pool exists, but THP's `always` policy
> covers it anyway — dTLB misses negligible), and the worker-count sweep shows a smooth,
> monotonic efficiency decline from 98.5% (4 workers) to 73.0% (8 workers) with **no plateau** —
> 8 workers remains the highest-throughput default, there is no free-lunch lower-worker-count
> option. `README.md` re-baselined with the real numbers. Also fixed two real bugs in the devbox
> MCP tooling found while running this verification (timeout bucket + a blocking-server/
> disconnect issue) — see `PLAN.md` Phase 6 for the full account of all of the above.
>
> **Major finding, same day, later session:** this device actually has **two separate 4-core L2
> cache clusters** (cores 0-3 / cores 4-7, confirmed via kernel cache-topology sysfs), not one
> homogeneous 8-core cluster as `lscpu` claims — the "Lenovo MSM8916/Snapdragon 410" hardware ID
> this project assumed since its earliest docs was wrong; **confirmed correct ID (per postmarketOS
> wiki) is MSM8929/Snapdragon 415**, a genuine big.LITTLE-shaped part (4×1.1 GHz + 4×1.4 GHz
> Cortex-A53), explaining the two-cluster topology outright. Under full 8-way contention, cluster 1
> (cores 4-7) loses roughly half its throughput to cluster 0 in a dynamic interconnect-
> arbitration effect (not a static frequency/cache difference — confirmed identical when either
> cluster runs alone). This **retracts item 3's "power/current cap" hypothesis** and fully
> explains item 2's efficiency curve (workers 1-4 = cluster 0 alone = 98.5% efficiency; worker 5
> is the first to hit cluster 1's arbitration penalty). Found via a real head-to-head XMRig run
> on the same device, which independently shows the identical core-0-3-vs-4-7 split — once
> normalized per-cluster, armrx is at ~90%/88% of XMRig on clusters 0/1 respectively (a real,
> modest ~10-12% gap, not the "far behind" impression the raw aggregate numbers gave). See
> `PLAN.md` Phase 6 item 3's "REVISED" section for the full evidence chain.
> See `docs/postmortems/aes-ttable-bug-postmortem.md` for the AES fix analysis, and `docs/postmortems/fast-mode-dataset-corruption-postmortem.md` / `docs/postmortems/pool-failover-deadlock-postmortem.md` for Phase 2/3's critical fixes.

## Baseline

- **Hardware (corrected 2026-07-24, per postmarketOS wiki):** actually **MSM8929 / Snapdragon
  415** — genuine big.LITTLE-shaped octa-core, **4× Cortex-A53 @ 1.1 GHz + 4× Cortex-A53 @
  1.4 GHz** (two differently-clocked clusters, same microarchitecture), 2 GiB RAM (postmarketOS,
  Linux 6.12, GCC 15.2 / musl). The earlier "Lenovo MSM8916 / Snapdragon 410" ID used since this
  project's earliest docs was wrong (MSM8916 is a quad-core part) — this explains the two
  separate 4-core L2 clusters found via cache-topology sysfs (item 3's "REVISED" section). One
  nuance worth noting: pinning 4 workers exclusively to *either* cluster in isolation measured
  virtually identical cycles/instructions (~772 MHz effective, `PLAN.md` Phase 6 item 3) — below
  *both* clusters' rated max (1.1/1.4 GHz), meaning this specific memory-bound RandomX workload
  doesn't let either cluster reach its official ceiling alone under the tested conditions. That
  doesn't contradict the "dynamic interconnect-arbitration, not a static frequency difference"
  finding (both were still equal to each other), but the *real* 1.1/1.4 GHz asymmetry may
  become a bigger factor than previously modeled once both clusters compete under full 8-worker
  thermal/power pressure — not yet separately isolated from the arbitration effect.
- **Hashrate (re-baselined 2026-07-24, `PLAN.md` Phase 6 item 2):** single-thread **4.27 H/s**;
  8-worker pinned **24.95 H/s** (73.0% scaling efficiency vs. ideal linear — a smooth, monotonic
  decline across 4→8 workers with no plateau, so 8 remains the highest-throughput choice). The
  historical 5.18 H/s / 25.28 H/s "linear scaling" figures were stale and did not reproduce.
- **Perf profile:** **98.24% of hash time is JIT execution**, 1.76% JIT compile. IPC **0.708** on A53 (~35% of dual-issue peak) — this is Phase 6's central fact: a memory-latency-stall-bound workload, not an instruction-throughput-bound one. The widely-cited **31.08%** aggregate branch-miss rate does **not** represent the mining hot path — isolating `bench_armrx --full-hash-only` (2026-07-22) shows only **2.4%** there; the aggregate is 94.93% driven by `--attribution-only`'s non-representative interpreted-mode comparison run. See `docs/experiments/branchless-cbranch.md`'s "The 31.08% figure does not represent the mining hot path" section.
- **Region breakdown:** chain/final `run()` = **99%** of hash; AES scratchpad = 0.3%; Blake2b = 0.0%; get_final_result = 0.5%.
- **Light-mode hot path (Phase 6 framing):** per-hash cost is dominated by superscalar dataset-item derivation plus ~16K random 64-byte probes into the 256 MiB Argon2 cache — not fast-mode bandwidth. Huge-page residency for this cache and the 2 MiB scratchpad is asserted (`MAP_HUGETLB`/`MADV_HUGEPAGE` "succeeding") but never actually verified on-device — the top open lead.

---

## ✅ Completed — Phase 0 (Security & Correctness)

| Task | Status |
|------|--------|
| 0.0 — Green baseline recorded (`PERF_BASELINE.txt`) | ✅ |
| 0.1 — S3: `set_dataset` size validation + bounds assert | ✅ |
| 0.2 — S1: JSON escape on TX fields | ✅ |
| 0.3 — S2/S4: `RANDOMX_FORCE_SECURE` honored, `enableAll()` deleted | ✅ |
| 0.4 — `static_assert` linking Program size invariants | ✅ |
| 0.5 — S5: `ARMRX_ASSERT` macro replacing plain `assert()` | ✅ |
| 0.6 — JIT dispatch null guard | ✅ |
| 0.7 — `.gitignore` hygiene | ✅ |
| S6 — TLS peer verification (`SSL_VERIFY_PEER`, `--no-verify-tls`) | ✅ |
| S7 — `emit32` UB (memcpy fix) | ✅ |
| S8 — Dangling pointer contract | ➡️ Moved to Phase 3 (JIT encapsulation) |

## ✅ Completed — Phase 1 (Performance & Tooling)

| O# | Optimization | Status |
|----|-------------|--------|
| O1 | `alignas(16)` on `RegisterFile` | ✅ |
| O2 | Thread-local block template reuse | ✅ |
| O3 | Rounding mode cache | ✅ |
| O4 | Hot path uses span-output `blake2b` overload | ✅ |
| O5 | Dataset huge pages (via `MappedMemory` + `MADV_HUGEPAGE`) | ✅ |
| O6 | NEON Argon2 G-function (2× SIMD, 128→64 `gb` calls) | ✅ |
| O7 | T-table AES fallback (replaces runtime `gf_inverse`) | ✅ |
| — | AES encrypt_transform fix: correct byte order + column permutation | ✅ |
| — | AES decrypt_transform fix: different column permutation from encrypt | ✅ |
| — | NEON AES paths removed (AESE/AESD operation order ≠ RandomX spec) | ✅ |
| O8 | Per-hash `mprotect` skip via `rwx_` flag | ✅ |
| O12 | JIT prologue instruction scheduling | ✅ |
| O13 | JIT register-offset FP loads | ✅ |
| — | `bench_armrx` registered in CTest (3 tests) | ✅ |
| — | ASan/UBSan CMake options | ✅ |
| — | `.clang-format` / `.clang-tidy` baseline configs | ✅ |

## ✅ Completed — Phase 2 (Architecture & Structure)

| P# | Refactor | Status |
|----|----------|--------|
| P0 | `armrx::json` module (escape + tokenizer + `get_array_element`) | ✅ |
| P1 | `run()` split into `run_jit()` / `run_interpreted()` | ✅ |
| P1 | `is_fast_mode()` helper (single source of truth) | ✅ |
| P1 | `compile_instruction` dispatch table (`kCompileHandlers[256]`) | ✅ |
| P2 | `PoolManager` extraction | ✅ |
| P2 | Dead code cleanup (`CodeBuffer`/`CompilerState` removed) | ✅ |
| P2 | Flag constant de-duplication (4 values aliased to `armrx::kRandomX*`) | ✅ |
| P2 | `const_cast` abuse eliminated (8 casts → `mutable` members) | ✅ |
| — | `handle_notify` positional scanner → `get_array_element()` | ✅ |
| O9 | Load interleaving (NEON direct FP loads via `ldr dN` + `sshll`) | ✅ |
| O10 | Prefetch hint tuning (`pldl2keep` → `pldl1keep` for dataset) | ✅ |
| O11 | Branchless CBRANCH (`bne .Lskip; b target` — fixes 99.6% mispredict rate) | ✅ |
| — | `emit32` UB fix (pointer cast → `memcpy`) | ✅ |
| — | hwloc CPU pinning (optional, v2.12.2) | ✅ |
| — | TLS hostname verification | ✅ |
| — | `stratum_` mutex (use-after-free fix) | ✅ |
| — | `session_id_` escape (submit + keepalive) | ✅ |
| — | `get_array_first` dead-code fix (512-byte truncation) | ✅ |
| — | `find_key` scope fix (matches inside string values) | ✅ |
| — | `generateProgram`/`generateProgramLight` dedup (v2 AES-tweak) | ✅ |
| — | `read_buf_` cap at 1 MiB (OOM prevention) | ✅ |
| — | `setPagesRW`/`setPagesRX` return `int` (error propagation) | ✅ |
| — | `mining_engine` silent-swallow fix (log + deactivate) | ✅ |
| — | `json::escape` handles all U+0000–U+001F control chars | ✅ |
| — | CLI numeric arg validation (`try`/`catch` wrappers) | ✅ |
| — | SIGTERM handler (graceful shutdown) | ✅ |
| — | `reconnect_attempts_` → `std::atomic<unsigned>` | ✅ |
| — | `handshake_req_id_` / `authorize_req_id_` → `std::atomic` | ✅ |
| — | `rx_set_rounding_mode` static → per-instance member | ✅ |
| — | `ARMRX_ENABLE_TSAN` CMake option | ✅ |
| — | `vm.hpp` comments (flag divergence, `register_usage_` note) | ✅ |
| — | `jit_compiler_a64_static.S` stale comment fix (12→17) | ✅ |
| — | `ceil_*` constants deleted, `allocate()` comment fixed | ✅ |
| — | `reg_.a` init gated behind `if (!jit_)` | ✅ |
| — | `[DEBUG]` log line removed from `main.cpp` | ✅ |
| — | `main.cpp` SIGTERM handler | ✅ |
| — | `--jit-dump` flag with opcode boundary markers | ✅ |
| — | `bench_opcodes` frequency/byte-cost analyzer | ✅ |
| — | JIT determinism test (`test_jit_determinism`) | ✅ |
| — | CBRANCH encoding unit test (`test_jit_encodings`) | ✅ |
| — | Per-opcode audit (all 30 handlers reviewed) | ✅ |
| — | MAP_HUGETLB for dataset (MappedMemory) | ✅ |
| — | MAP_HUGETLB for cache (Argon2dCache) | ✅ |
| — | MAP_HUGETLB + MADV_POPULATE_WRITE for scratchpad | ✅ |
| — | Structured logger (`include/armrx/log.hpp`) | ✅ |
| — | Cross-thread log sites migrated to logger | ✅ |
| — | Template copy per-hash eliminated (P2.5) | ✅ |
| — | Superscalar heap churn eliminated (P2.5) | ✅ |
| — | TUI: TuiSnapshot + injectable ostream (U1.1-1.2) | ✅ |
| — | TUI: terminal-width + NO_COLOR (U1.3-1.4) | ✅ |
| — | TUI: EMA baseline + atexit cursor (U1.5-1.6) | ✅ |
| — | Share accept/reject counters (U3.1) | ✅ |
| — | `--version` flag with git SHA (U3.5) | ✅ |
| — | Dead config parser removed (U3.7) | ✅ |
| — | Prometheus metrics endpoint | ✅ |
| P2.6 | Profile-Guided Optimization (PGO) unblocked | ✅ |
| P2.7 | Software AES header inlining & register-passing | ✅ |
| P2.8 | big.LITTLE worker thread scheduling | ✅ |
| P2.9 | JIT instruction buffer size expansion (fixes literal pool corruption) | ✅ |
| P2.10| Fast Newton-Raphson division/sqrt math evaluation | ✅ |

### Docs
| Doc | Description |
|-----|-------------|
| [`docs/experiments/branchless-cbranch.md`](docs/experiments/branchless-cbranch.md) | CBRANCH misprediction analysis, imm19 bug root cause, BTB aliasing caveat |
| [`docs/plans/peephole-jit-plan.md`](docs/plans/peephole-jit-plan.md) | Detailed Phase 3 plan: frequency data, allocation spot-check, per-opcode audit, hashrate veto |
| [`docs/archived/next_phase_v3.md`](docs/archived/next_phase_v3.md) | Archived next-phase improvement plan (v3) — superseded by PLAN.md |
| [`docs/archived/next_phase_v2.md`](docs/archived/next_phase_v2.md) | Archived next-phase improvement plan (v2) |

## ✅ Completed — Phase 3 (This Session, 2026-07-22)

| Item | Status |
|------|--------|
| `MiningEngine` worker-thread reuse for dataset init (barrier via mutex/counter/condition_variable) | ✅ |
| **Critical fix:** fast-mode dataset corruption — wrong output span in multi-threaded `initialize_dataset()` calls (`docs/postmortems/fast-mode-dataset-corruption-postmortem.md`) | ✅ |
| Mock Stratum protocol test suite (`tests/test_pool_protocol.cpp`, 7 scenarios) | ✅ |
| **Critical fix:** `PoolManager::tick()` self-deadlock on real multi-pool failover (`docs/postmortems/pool-failover-deadlock-postmortem.md`) | ✅ |
| Pool-failover gap: pool dead from process startup never triggering failover | ✅ |
| Pool-failover gap: up to ~30s stale-reconnect-thread-join delay | ✅ |
| LibFuzzer harness for `armrx::json` (2.5M+ executions, zero findings) | ✅ |
| Argon2d NEON permutation benchmarked and enabled (~16% faster) | ✅ |
| `main.cpp` split into `CommandLineParser` + `MinerApp` | ✅ |
| On-device LTO build regression root-caused (Alpine `fortify-headers` + GCC LTO) and fixed | ✅ |
| AES round-key constants consolidated (`include/armrx/aes_keys.hpp`) | ✅ |
| Scratchpad L3 mask constants unified (`include/armrx/randomx_config.hpp`) | ✅ |
| `kCompileHandlers[256]` derived from `instruction_weights.hpp` instead of hand-maintained | ✅ |
| CBRANCH investigation: CSEL implemented, measured, reverted (net regression); root-caused the 31.08% branch-miss figure to a non-representative benchmark section | ✅ |
| Argon2 NEON diagonal-step vectorization: 26.8% fewer instructions, 19.0% fewer cycles for `Argon2dCache::initialize` (`docs/experiments/argon2-neon-diagonal-vectorization.md`) | ✅ |
| Argon2 `memcpy`/copy-elimination investigation: implemented, measured, reverted (no net win — cost relocated, didn't disappear) + follow-up `perf annotate` closed out the remaining "driver code" lead as inherent, already-vectorized XOR-combine work, not a bug (`docs/experiments/argon2-compress-copy-elimination.md`) | ✅ |
| `devbox_pgo_build` tool added (GENERATE→train→USE orchestration) + fixed a real pre-existing devbox-tooling bug (tilde-expansion defeated by `shlex.quote()`, silently breaking log stashing and `devbox_status`'s revision check) — but the claimed +19.3% PGO payoff does **not** reproduce on the current codebase (measured identical 4.27 H/s PGO vs non-PGO, apples-to-apples) | ✅ |
| NEON vector-permute AES: derived from scratch, exhaustively verified (256/256 S-box match, 20,000-trial full-round parity, first-try-correct on real hardware), gated behind `ARMRX_ENABLE_NEON_AES` (default OFF) — measured as a real ~19.4% regression, same root cause as the earlier hardware-AES finding (`docs/experiments/neon-vector-permute-aes.md`) | ✅ |
| `--stagger-ms` default — found already tested in an earlier session (`docs/archived/beyond-parity_v2.md`, 4 stagger values, "+0%, hardware ceiling"), not re-run; default (0) left unchanged | ✅ |
| `MiningEngine::worker_loop()` permanently killed a worker thread on a bad nonce offset/size — fixed (`active = false; continue;`), regression test added | ✅ |
| `config.cpp` numeric config-file fields unguarded against parse failure — fixed (try/catch, matching `cli_parser.cpp`), regression test added | ✅ |
| `MetricsExporter::server_fd_` data race (plain `int` across threads) — fixed (`std::atomic<int>`) | ✅ |
| `cli_parser.cpp` test coverage added (`tests/test_cli_parser.cpp`) — found and fixed a real bug: `--config=` always exited with "Unknown argument" (code 64) | ✅ |
| `aes_hash.cpp` direct helper test coverage added (`tests/test_aes_hash.cpp`) — golden pins + `hash_and_fill_aes_1r_x4` decomposition-equivalence check | ✅ |

## ✅ Completed — Phase 6 (in progress, 2026-07-24)

| Item | Status |
|------|--------|
| Huge-page residency check: cache/scratchpad already ~97.6% THP-coalesced (100% for the Argon2 cache mapping specifically), despite no real hugetlbfs pool existing; dTLB-load-misses negligible (~1.6/million instructions) — closed as a no-op | ✅ |
| Worker-count sweep, 4→8 workers, two passes: smooth monotonic efficiency decline (98.5%→73.0%), no plateau — 8 workers confirmed as the highest-throughput default, no free-lunch lower-worker-count option | ✅ |
| Multi-worker PMU attribution (Cortex-A53 `l1d_cache_refill`/`l2d_cache_refill`/`ld_dep_stall`, 1/2/4/6/8 workers): TLB definitively rejected; two layered mechanisms found — front-loaded L2/DRAM contention (1→4 workers) plus a separate per-core clock reduction onsetting at 6+ workers (~772→641→567 MHz) more consistent with a core-count-triggered power cap than thermal-junction throttling (temps stayed mild, 36-50°C) | ✅ |
| `README.md` performance table re-baselined with the sweep's real numbers, replacing the stale "linear scaling" claim | ✅ |
| devbox MCP tooling: `tool_test()`'s 120s default-bucket timeout (always too short for the ~400s+ full suite) and the single-threaded blocking server (couldn't answer `ping` during long calls, causing mid-call disconnects) both found and fixed — see `PLAN.md` Phase 6 | ✅ |
| Register-offset FP loads (item 7) and static FP load/convert software-pipelining (item 8): both verified as stale claims, already implemented in Phase 1 (`O13`/`O12`) — no code change made or needed | ✅ |
| Superscalar literal-pool relayout (item 9): implemented, measured, reverted — cycles +1.65%, IPC down, hashrate −1.12%, despite branches dropping 12.1% exactly as designed; same "cost relocated, not eliminated" pattern as the Argon2 `memcpy` attempt | ✅ |
| Region-scoped instruction-count breakdown attempt (`--jit-dump`): both candidates (memory-op address computation, CBRANCH preamble) fell apart on closer reading; dump also targeted the wrong region (main VM program, not the dominant superscalar/dataset-derivation path) — a documented negative result, no code changed | ✅ |
| SoC identity corrected: MSM8929/Snapdragon 415 (4×1.1 GHz + 4×1.4 GHz Cortex-A53), not MSM8916/410 — explains the two-cluster L2 topology outright; new Phase 6 item 13 added (build instrumentation for the superscalar/dataset-derivation path) | ✅ |
| Fused hash-and-fill nonce pipeline (item 11): new primitive-level benchmark shows the fused call is ~3.6% *slower* than two separate calls — closed, no mining-engine integration attempted | ✅ |
| Prefetch A/B matrix (item 10), "none" variant: **adopted** — removed all three `.Lmain_loop` `prfm` hints permanently after `perf stat` confirmed a real +0.885% instruction-throughput gain (zero overlap, 7-20× signal-to-noise), settling what wall-clock hashrate alone (2 then 3 samples, +0.4%, p≈0.07) couldn't confirm | ✅ |
| **Major finding**: this device has two separate 4-core L2 clusters (cores 0-3 / 4-7), confirmed via kernel cache-topology sysfs and cross-validated against a real XMRig run on the same hardware; a dynamic interconnect-arbitration effect (not a static clock/cache difference) costs cluster 1 ~half its throughput under full contention — retracts item 3's "power cap" hypothesis, fully explains item 2's efficiency curve, and gives a real cluster-normalized gap to XMRig of ~10-12% (not the "far behind" impression raw aggregates gave) | ✅ |
| Item 13: built real instrumentation for the superscalar/dataset-derivation path — confirmed via assembly reading that `generateSuperscalarHash()` compiles once per seed rotation but its output executes 16,384×/hash (`bl rx_calc_dataset_item` on every light-mode main-loop iteration); extended `JitDumpEntry`/`--jit-dump` to cover this region, verified 12/12; real data shows ~58.4M instructions/hash from this region alone (~44% of the ~132.93M total, a lower bound — fixed wrapper chunks and main-loop overhead still untracked) | ✅ |

---

## 🔴 Remaining — Action List

### Correctness

_All items found in the `PLAN.md` Phase 4 fresh-codebase inspection are now fixed — see Completed table above._

### Performance

| # | Item | Site | Est. impact | Risk | Notes |
|---|------|------|-------------|------|-------|
| ~~P4~~ | ~~Reduce JIT execution branch-misprediction cost (CSEL for CBRANCH)~~ | `jit_compiler_a64.cpp` | — | — | **Closed 2026-07-22 — implemented, measured, reverted.** CSEL gave +46% branch-misses and flat hashrate vs. the existing `bne`/`b`, not an improvement (BTB-aliasing: the JIT buffer regenerates every hash, so no encoding trick fixes the predictor-history problem). Separately found the 31.08% figure this item's "~8.6–11.9% of cycles" estimate was based on doesn't represent the mining hot path at all — the isolated hot path's real miss rate is 2.4%, costing ~0.1–0.16% of cycles. See `docs/experiments/branchless-cbranch.md`. No further CBRANCH JIT work planned. |
| **P3** | **Peephole JIT coalescing** — [`docs/plans/peephole-jit-plan.md`](docs/plans/peephole-jit-plan.md) | `jit_compiler_a64.cpp`, `static.S` | ~+5–10% | 🟡 Medium | **Re-scoped by Phase 6 (2026-07-24): lowest-EV surviving code lead.** A real whole-process instruction-count gap vs. XMRig was measured once as a black-box reference point (~33.5%/hash, `PLAN.md` item 3) — a region-scoped attempt at the main VM program (`--jit-dump`) initially targeted the wrong region, but item 13's new instrumentation now covers the actual dominant region (superscalar/dataset-derivation, ~44% of instructions/hash explained). **The region-scoped gate item 14 requires is still not fully met** — ~55% of instructions/hash remains unreconciled, via armrx's own code/instrumentation only (no XMRig-side comparison — a deliberate clean-room boundary, see `PLAN.md` after item 13). Do not start P3 without closing that gap. |
| ~~P6.1~~ | ~~Huge-page residency check~~ | — | — | — | **Closed 2026-07-24 — done, no-op.** ~97.6% of anon RSS already THP-coalesced (100% for the Argon2 cache mapping), dTLB misses negligible, despite no real hugetlbfs pool existing. `PLAN.md` Phase 6 item 1. |
| ~~P6.2~~ | ~~Worker-count sweep, 4→8 workers~~ | — | — | — | **Closed 2026-07-24 — done.** Smooth monotonic decline 98.5%→73.0% efficiency, no plateau; 8 workers confirmed highest-throughput. `README.md` re-baselined. `PLAN.md` Phase 6 item 2. |
| ~~P6.3~~ | ~~Multi-worker PMU attribution~~ | — | — | — | **Closed 2026-07-24 — done, revised later same day.** TLB definitively rejected (dTLB misses <1.4/million instructions at every worker count). The original "core-count-triggered power/current cap" hypothesis for the apparent 6-8-worker clock drop is **retracted** — this device has two separate 4-core L2 clusters (cores 0-3, cores 4-7; see P6.11), and the drop was just the average of a full-rate cluster and an arbitration-losing cluster once the worker count spanned both. `PLAN.md` Phase 6 item 3's "REVISED" section. |
| P6.4 | `--rt-priority` + `isolcpus=`/`nohz_full=` experiment | system config, no code | lower jitter | 🟢 None | **Blocked on manual device access (2026-07-24)** — needs `setcap`/root (not available) and a kernel-cmdline edit + reboot (needs explicit user sign-off). Deferred. Also now known not to touch the cluster-arbitration effect (P6.11) even if unblocked — this is an interconnect-hardware fact, not scheduler-visible. `PLAN.md` Phase 6 item 4. |
| ~~P6.11~~ | ~~Two-L2-cluster interconnect-arbitration discovery~~ | — | — | — | **Closed 2026-07-24 — major finding, not originally in either master plan.** Found via a real head-to-head XMRig run on this device. Kernel cache-topology sysfs confirms two separate 4-core L2 clusters (cores 0-3, cores 4-7), not one 8-core cluster as `lscpu` claims. Under full 8-way contention, cluster 1 loses ~half its throughput to cluster 0 — a dynamic interconnect-arbitration effect (confirmed *not* a static frequency/cache difference: identical when either cluster runs alone). Fully explains P2's efficiency curve. Cluster-normalized comparison against XMRig: armrx at ~90%/88% of XMRig on clusters 0/1 — a real ~10-12% gap, not the "far behind" impression raw aggregates gave. See `PLAN.md` Phase 6 item 3's "REVISED" section. |
| ~~P6.5~~ | ~~Disclose + prefault Argon2 cache huge-page fallback~~ | — | — | — | **Closed 2026-07-24 — no-op**, per P6.1's result (already coalesced). `PLAN.md` Phase 6 item 5. |
| ~~P6.6~~ | ~~Register-offset FP loads~~ | — | — | — | **Closed 2026-07-24 — stale claim, already implemented** (`emitMemLoadFP()` already emits `ldr dN,[x2,tmp_reg]` directly; decoded the raw instruction encoding by hand to confirm). Matches Phase 1's `O13`. No code change made. `PLAN.md` Phase 6 item 7. |
| ~~P6.7~~ | ~~Static FP load/convert software-pipelining~~ | — | — | — | **Closed 2026-07-24 — stale claim, already implemented** (static prologue is already interleaved per its own comment). Matches Phase 1's `O12`. No code change made. `PLAN.md` Phase 6 item 8. |
| ~~P6.8~~ | ~~Superscalar literal-pool relayout~~ | — | — | — | **Closed 2026-07-24 — implemented, measured, reverted.** Branches down 12.1% as designed, but cycles up +1.65%, IPC down (0.761→0.748), hashrate down 1.12% (268→265 hashes/60s) — a real regression, same "cost relocated" pattern as the Argon2 `memcpy` attempt. Reverted, KATs re-confirmed 12/12. `PLAN.md` Phase 6 item 9. |
| ~~P6.9~~ | ~~Fused hash-and-fill nonce pipeline~~ | — | — | — | **Closed 2026-07-24 — benchmarked, failed its own gate.** Fused `hash_and_fill_aes_1r_x4` measured ~3.6% *slower* (59,124.84 μs) than the two separate calls it would replace (57,084.78 μs) — new benchmark in `bench_armrx.cpp`. No `mining_engine.cpp` integration attempted. `PLAN.md` Phase 6 item 11. |
| P6.10 | Conservative emitter lookahead scheduler | `jit_compiler_a64.cpp` handlers | 2–6% | 🔴 High | **Implemented and correctness-verified 2026-07-25** (450-pair differential stress test + full existing suite, all green on-device) — a `src==dst` shared-scratch-register hazard invisible to the register-level hazard model was found and fixed via bisection. **Performance not yet measured** — stays in Remaining until a `perf stat`/hashrate comparison decides keep-vs-revert. `PLAN.md` Phase 6 item 12. |

### Open decision (not a bug)

| # | Item | Site | Notes |
|---|------|------|-------|
| — | JIT buffer W^X vs RWX default | `virtual_memory.c` | **Default kept RWX (explicit user direction, 2026-07-23), now disclosed via a startup log line in `jit_compiler_a64.cpp` instead of being silent.** Revisit the actual default later if wanted. |

### Features

| # | Item | Effort | Notes |
|---|------|--------|-------|
| — | Stratum V2 protocol support | 🔴 Major | Next-gen pool compatibility |
| — | HTTP Prometheus metrics endpoint | ✅ Completed | Serving GET /metrics, loopback-only |
| — | Newton-Raphson FDIV/FSQRT (O12) | ⏸️ Frozen | **Stale entry corrected 2026-07-23**: this used to describe an early segfault/`x29` register-corruption bug, which was since root-caused and fixed (`docs/audits/WX_Alignment_and_LITTLE_Core_Profiling.md` §5). Newton-Raphson was then fully re-evaluated cleanly: 100% correctness/determinism pass in CTest, but measured **5.12 H/s vs 5.18 H/s for native hardware `fdiv`/`fsqrt` (−1.1%)** — kept OFF because it's slower on the in-order Cortex-A53 (FPU pipeline pressure from the 12-17 instruction approximation), not because of any remaining correctness risk. See `OPTIMIZATION_REFERENCE.md` and `changelogs.md` (2026-07-21). Do not re-enable by default; a future attempt would need to beat −1.1%, not just prove correctness. |

### Maintenance

| # | Item | Effort | Notes |
|---|------|--------|-------|
| — | Cross-compile CI (GitHub Actions + qemu-user) | 🟡 Medium | Optional — you test on real hardware |
| — | Test coverage: `tls_client.cpp`/`tui.cpp` remain fully untested | 🟡 Medium | Need a mock TLS server / terminal-capture harness respectively. `cli_parser.cpp`/`aes_hash.cpp` gaps closed 2026-07-23 — see `PLAN.md` Phase 4 item E. |

---

## Reference Docs

| Doc | Description |
|-----|-------------|
| [`docs/plans/performance-master-plan.md`](docs/plans/performance-master-plan.md) | Phase 6 source doc (this assistant, 2026-07-24): light-mode/IPC-0.708 framing, huge-page + worker-sweep verification plan |
| [`docs/plans/performance-master-plan-20260724.md`](docs/plans/performance-master-plan-20260724.md) | Phase 6 source doc (Hermes agent, 2026-07-24): concrete JIT-emitter latency-hiding proposals |
| [`docs/archived/plan_completed_phases_1-5.md`](docs/archived/plan_completed_phases_1-5.md) | Full narrative for every completed Phase 1–5 item, split out of `PLAN.md` 2026-07-24 |
| [`docs/experiments/branchless-cbranch.md`](docs/experiments/branchless-cbranch.md) | CBRANCH misprediction analysis, imm19 bug root cause, BTB aliasing caveat |
| [`docs/plans/peephole-jit-plan.md`](docs/plans/peephole-jit-plan.md) | Detailed peephole-JIT plan: frequency data, allocation spot-check, per-opcode audit, hashrate veto — re-scoped by Phase 6, gated on a fresh region-scoped gap measurement |
| [`docs/archived/next_phase_v3.md`](docs/archived/next_phase_v3.md) | Archived next-phase improvement plan (v3) — superseded by PLAN.md |
| [`docs/audits/jit-buffer-size-audit.md`](docs/audits/jit-buffer-size-audit.md) | JIT buffer size analysis and security audit |
| [`docs/archived/next_phase_v2.md`](docs/archived/next_phase_v2.md) | Archived next-phase improvement plan (v2) |
