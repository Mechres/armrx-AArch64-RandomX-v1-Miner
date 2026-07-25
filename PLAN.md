# armrx — Master Update and Improvement Plan

This document is the live master plan for the `armrx` RandomX AArch64 miner — the *open* work
only. It used to also carry the full narrative for every completed phase; that grew to 400+
lines of 100%-done history sitting in front of the actually-open work, so it was split out
2026-07-24 into **[`docs/archived/plan_completed_phases_1-5.md`](docs/archived/plan_completed_phases_1-5.md)**.
Read this file for what's still open; read the archive for the full why-and-how behind
everything already shipped. The chronological, dated record of the same history lives in
`changelogs.md`; the short-list actionable view at any point in time lives in `NEXT_STEPS.md`;
completed/remaining status tables live in `ROADMAP.md`.

---

## Completed — Phases 1–5 (2026-07-21 through 2026-07-23)

All correctness fixes, structural refactors, test-coverage gaps, and performance
investigations identified through Phase 5 are done. Summary (full detail in the archive):

- **Phase 1–2** (2026-07-21/22): `MetricsExporter` thread-join fix, Stratum nonce
  abstraction, `main.cpp` → `CommandLineParser`/`MinerApp` split, worker-thread reuse for
  dataset init (surfaced and fixed a critical fast-mode dataset-corruption bug), mock Stratum
  protocol tests (surfaced and fixed a `PoolManager` self-deadlock), JSON fuzzing (2.5M+
  executions, zero findings), Argon2 NEON `permute_block` enabled (~16% faster).
- **Phase 3** (2026-07-22/23): on-device LTO/`fortify-headers` build regression root-caused
  and fixed, both pool-failover gaps fixed, 3 constant-dedup refactors, CBRANCH/CSEL
  investigated and reverted (net regression) — **and its own investigation root-caused the
  historical 31.08% branch-miss figure to a non-representative benchmark section**; the real
  hot-path rate is 2.4%, costing ~0.1–0.16% of cycles. Argon2 diagonal-step NEON
  vectorization landed (26.8% fewer instructions / 19.0% fewer cycles for cache init);
  `memcpy` copy-elimination tried and reverted (no net win).
- **Phase 4** (2026-07-23): fresh codebase inspection found and fixed 3 more bugs — a worker
  thread permanently killed by a malformed nonce job, unguarded config-file numeric parsing,
  and a `MetricsExporter::server_fd_` data race — plus new test coverage for `cli_parser.cpp`
  (found a real broken-`--config=` bug) and `aes_hash.cpp`. JIT buffer RWX default kept,
  now disclosed at startup.
- **Phase 5** (2026-07-23): external audit's leads fact-checked and adopted — PGO devbox
  wiring shipped (tool works, but the claimed +19.3% payoff did **not** reproduce on current
  code: measured identical 4.27 H/s PGO vs. non-PGO); NEON vector-permute AES derived from
  scratch and exhaustively verified correct, measured as a real ~19.4% regression (kept,
  flag-gated OFF); `--stagger-ms` confirmed already tested and ineffective in an earlier
  session, not re-run.

**Still open from this era, low priority, tracked in backlog (not started):** QEMU AArch64
GitHub Actions CI, Stratum V2 protocol support, `ARMRX_JIT_FAST_DIV_SQRT` CMake flag
centralization, `tls_client.cpp`/`tui.cpp` test coverage (need a mock TLS server / terminal-
capture harness respectively — bigger lift than the `cli_parser.cpp`/`aes_hash.cpp` work
already done). None of these block or relate to Phase 6 below.

---

## Phase 6 — current (2026-07-24): dual master-plan synthesis, on-device verification first

Two independent performance master plans were produced against current HEAD (`88f4122`):
`docs/plans/performance-master-plan.md` (this assistant) and `docs/plans/performance-master-plan-20260724.md`
("Hermes" agent). Both post-date, and explicitly build on, Phase 3/5's closed-leads list
(CSEL/CBRANCH, NEON AES ×3, Newton-Raphson, PGO, `--stagger-ms` — all re-verified correct in
both new plans, **not re-opened**). This phase reconciles the two into one adopted plan rather
than running them in parallel.

**Where the two plans agree (adopted as-is, highest priority):**
- The device runs **light mode** (2 GiB RAM can't fit the ~2080 MiB fast-mode dataset), so the
  hot path is superscalar dataset-item derivation plus random probes into the 256 MiB Argon2
  cache — not fast-mode bandwidth. IPC is 0.708 (~35% of dual-issue peak): a memory-latency-
  stall-bound workload, not an instruction-throughput-bound one. This is *why* every closed
  instruction-count lead (CSEL, Newton-Raphson, NEON-AES) failed — none of them reduced stalls.
- **Huge-page residency for the hot-path allocations is asserted, never verified.**
  `MAP_HUGETLB`/`MADV_HUGEPAGE` "succeeding" doesn't prove the kernel actually backed the
  mapping with huge pages, especially the 256 MiB Argon2 cache's silent-fallback path
  (`argon2.cpp:270-277`). This is the single highest-EV unmeasured unknown in both plans —
  zero code risk, gates everything downstream.
- **A worker-count sweep has never actually been run** on this hardware. 8 workers deliver
  ~68% scaling efficiency vs. ideal; both plans flag that the ceiling (and whether it's DRAM
  bandwidth, thermal throttling, or TLB pressure) has never been isolated with counters.
- Both plans independently rank these two verifications above any code change, and both
  explicitly say: measured hashrate on real hardware vetoes every estimate in either document.

**Where the two plans diverge (reconciled, not run in parallel):**
- The opus plan treats IPC 0.708 as near-conclusive evidence that instruction-level JIT
  changes are low-EV until the memory-side questions are answered, and ranks peephole/
  literal-pool work last (`docs/plans/performance-master-plan.md` §2.3, §5 L1).
- The Hermes plan reaches the same IPC number but stays specific about *which* instruction-
  level changes could plausibly help an in-order core stalling on load-use latency —
  register-offset FP loads, software-pipelined load/convert groups, literal-pool relayout —
  framed as **latency-hiding**, not instruction-count reduction, so they are mechanistically
  distinct from the already-falsified CSEL/Newton-Raphson category.
- **Reconciliation:** these aren't actually in conflict. The Hermes-plan emitter items are
  reasonable *second-tier* candidates specifically because they attack stalls, not instruction
  count — but they still queue behind the shared memory-side verification (§ above), since a
  huge-page or worker-count fix could change the instruction stream's stall profile enough to
  make premature emitter surgery wasted effort. Adopt both plans' shared verification step
  first; treat the Hermes-plan emitter items as the next tier, gated on what that verification
  shows; treat opus's full peephole-JIT rewrite (`docs/plans/peephole-jit-plan.md`) as lowest-EV,
  unstarted without a fresh region-scoped measurement per its own gate.

**Adopted phased plan (supersedes running either source doc standalone):**

*Short-term (hours, zero/low code risk — do first, on-device only):*
1. **Huge-page residency check — ✅ done (2026-07-24), result: already fully coalesced, no-op.**
   While mining (8 workers, light mode, steady state): `AnonHugePages: 278,528 kB` out of
   285,432 kB total anon RSS (**97.6%**), the 256 MiB Argon2 cache mapping specifically showing
   **100%** `AnonHugePages` coverage. `Private_Hugetlb`/`Shared_Hugetlb` are both 0 kB and
   `HugePages_Total: 0` — there is **no** real hugetlbfs pool on this kernel, so every
   `MAP_HUGETLB` request must be silently failing, but THP's `always` policy (confirmed via
   `/sys/kernel/mm/transparent_hugepage/enabled`) is independently coalescing almost the entire
   working set anyway. Cross-checked with `perf stat -e dTLB-load-misses,...` attached to the
   live mining process: 78,552 dTLB-load-misses over 49.9B instructions — **~1.6 per million
   instructions**, nothing like the "500× TLB reach" worst case either master plan hypothesized.
   **This closes the lead as a no-op**: items 5 and 6 below (disclose+prefault, reserve a
   hugetlb pool) would not change anything measurable on this device — do not implement them
   without a different device showing a different residency result first.
2. **Worker-count sweep — ✅ done (2026-07-24), result: no plateau, smooth monotonic decline.**
   4/5/6/7/8 workers, `--seconds=60` each, two full interleaved passes, 90s cooldown between
   runs (thermal zones confirmed reset to the ~38–41°C idle baseline before every run). Both
   passes agreed to within a few hashes at every point (very low noise):

   | Workers | Avg H/s | H/s/worker | Efficiency vs. 4.27 H/s baseline |
   |---|---|---|---|
   | 4 | 16.82 | 4.20 | 98.5% |
   | 5 | 19.04 | 3.81 | 89.2% |
   | 6 | 21.13 | 3.52 | 82.5% |
   | 7 | 23.21 | 3.32 | 77.7% |
   | 8 | 24.95 | 3.12 | **73.0%** |

   **Neither of the two anticipated outcomes happened** — efficiency doesn't plateau from
   ~6→8, it declines smoothly and continuously across the whole range, and 8 workers gives the
   highest absolute hashrate at every step (each additional worker still contributes ~41–52% of
   a full thread's rate, never zero). **The "bank a 6-worker default at equal hashrate" idea is
   rejected** — there is no free lunch in this range; fewer workers trades real throughput for
   lower heat, it doesn't recover the same aggregate rate. 8 workers remains the right default
   for throughput. `README.md`'s table re-baselined with these numbers (was item 15, done early
   since the data was already in hand). **Scope caveat**: each data point is a 60s window: this
   does not test long-duration (15–30 min) sustained thermal throttling — post-run thermal-zone
   temps rose to ~47–55°C within 60s across all worker counts (idle 38–41°C), but whether that
   keeps climbing over a much longer run and eventually throttles is a distinct, still-open
   question (see item 3 immediately below, and the "thermal/worker" sweep this item's own
   `NEXT_STEPS.md`/`ROADMAP.md` entries still track as a longer-duration follow-up).
3. **Multi-worker PMU attribution — ✅ done (2026-07-24), result: two layered mechanisms, not one; TLB definitively rejected.**
   Cortex-A53-specific PMU events (`l1d_cache_refill`, `l2d_cache_refill`, `ld_dep_stall` — cycles
   stalled specifically on a load-miss dependency), sampled via `perf stat -p <pid>` attached to a
   live 15s steady-state window at each of 1/2/4/6/8 workers, no multiplexing (fit within the A53's
   counter budget). Per-core effective clock isn't exposed via `scaling_cur_freq` on this device, so
   it's derived from `cycles / wall_seconds / worker_count`:

   | Workers | Est. per-core clock | IPC | L1D refill /1K instr | L2D refill /1K instr | dTLB miss /1M instr | `ld_dep_stall` (% cycles) |
   |---|---|---|---|---|---|---|
   | 1 | 774.9 MHz | 0.765 | 4.805 | 2.142 | 0.545 | 7.61% |
   | 2 | 774.4 MHz | 0.743 | 4.807 | 2.679 | 0.414 | 10.10% |
   | 4 | 772.2 MHz | 0.723 | 4.818 | 3.098 | 0.333 | 12.25% |
   | 6 | 641.4 MHz | 0.730 | 4.809 | 3.132 | 0.580 | 11.49% |
   | 8 | 567.4 MHz | 0.730 | 4.795 | 3.274 | 1.398 | 11.41% |

   **TLB-bound: definitively rejected.** dTLB misses stay below 1.4 per *million* instructions at
   every worker count — consistent with item 1's huge-page finding, TLB pressure is not a factor
   at any core count tested.

   **Not one mechanism, two, with different onset points:**
   - **L2/DRAM-adjacent contention is real but front-loaded.** L2 refill rate per instruction
     climbs steeply from 1→4 workers (2.14→3.10 per 1K instr, +45%), then nearly flattens 4→8
     (3.10→3.27, +5.6% total) — L1D refill rate stays essentially flat throughout (~4.8/1K instr
     at every count), so this is specifically an L2/beyond-L2 effect, not an L1 one. This tracks
     `ld_dep_stall`'s own rise (7.6%→12.25% of cycles) across the same 1→4 range.
   - **A per-core clock reduction onsets specifically at 6+ workers and is not explained by the
     L2/stall data**, which is nearly flat by then. Effective clock holds flat at ~772–775 MHz for
     1/2/4 workers, then drops to ~641 MHz at 6 (−17%) and ~567 MHz at 8 (a further −12%, −27%
     total from the 1–4-worker baseline). Post-run thermal-zone temps stayed mild throughout
     (36–50°C, idle baseline ~36–41°C) — well below where junction-temperature throttling
     typically first engages on this SoC class — so classic thermal throttling is not a fully
     satisfying explanation by itself. ~~This looks more consistent with a core-count-triggered
     multi-core power/current cap.~~ **Superseded — see the corrected explanation below (found
     later the same day, §"REVISED").** The "power cap" framing was wrong; the real cause is a
     two-cluster interconnect-arbitration asymmetry, not a frequency change of any kind.
   - **Practical read (superseded, see REVISED below):** ~~the item-2 hashrate sweep's smooth
     efficiency decline (98.5%→73.0%) is the sum of both effects — front-loaded memory contention
     explains most of the 1→4-worker decline, the clock cap explains most of the additional
     6→8-worker decline.~~
   - **Consequence for item 4 (superseded, see REVISED below):** ~~if the second mechanism really
     is a power/current cap, `isolcpus=`/`nohz_full=` is unlikely to touch it.~~

   ---
   **REVISED (2026-07-24, later the same day) — the real mechanism is a two-L2-cluster
   interconnect-arbitration asymmetry, found while investigating a completely different question
   ("why can't armrx match XMRig's hashrate on this device") with a real XMRig run on the same
   hardware.** Full chain of evidence, each step directly measured, not inferred:

   1. **XMRig's own per-core table on this device** (`armrx --mine` vs `./xmrig-dev`, same pool
      job, `rx/0`, slow/light mode — XMRig hit the same "not enough memory for dataset" fallback
      armrx does) showed a striking, non-uniform split: cores 0-3 at 4.6-4.7 H/s each, cores 4-7
      at only 2.3-2.5 H/s each — roughly 2:1, aggregate 28.28 H/s. Not something either master
      plan or five prior sessions of profiling had ever looked for (all prior measurement was
      aggregate-only).
   2. **armrx, tested with 8 isolated sequential single-core runs** (`taskset -c N --workers=1`,
      no contention): all 8 cores identical, 67-68 hashes/15s each (~4.47-4.53 H/s) — no
      asymmetry at all in isolation. Ruled out "cores 4-7 are just weaker."
   3. **armrx, tested with a real 8-worker concurrent run**, per-worker breakdown (`run_local_benchmark()`
      already prints this via `--warmup`/steady-state snapshot delta — previously missed because
      every earlier command in this session grepped it away): first attempts at 25-40s
      steady-state windows showed an exact 2:1 split too, but the *absolute* hash counts were
      suspiciously identical between two differently-sized windows (128 vs 64 hashes, both
      times) — a red flag for measurement-quantization artifact, since per-worker counters are
      flushed in batches of 64 (`mining_engine.cpp`) and a batch takes ~14s to fill at ~4.5 H/s,
      so a 25-40s window only captures 1-2 batches, mostly noise. **Re-ran with a 300s/60s-warmup
      window (240s steady-state, ~15-16 batches/worker)** to average out that noise: the split
      *held* — worker[0-3] at 4.0-4.26 H/s, worker[4-7] at a rock-steady 2.13 H/s. Confirmed real,
      not an artifact.
   4. **Confirmed the physical core mapping** via `/proc/<pid>/task/*/stat`'s processor field:
      armrx's worker[i] pins directly to physical core i (no reordering) — so worker[0-3] = cores
      0-3, worker[4-7] = cores 4-7, an **exact match** to XMRig's own core-ID split. Two
      independently-implemented miners agreeing on the same core-group boundary is strong
      cross-validation that this is a real device characteristic, not a bug in either miner.
   5. **Found the root cause directly in kernel cache-topology sysfs**:
      `/sys/devices/system/cpu/cpu0/cache/index2/shared_cpu_list` = `0-3`, and
      `cpu4/cache/index2/shared_cpu_list` = `4-7`. **This device has two separate 4-core L2
      cache domains, not one shared 8-core cluster** — directly contradicting `lscpu`'s own
      "Cluster(s): 1, Core(s) per cluster: 8" self-report (wrong, or at least not reflecting real
      cache topology; the "Lenovo MSM8916" identification this whole project has used since its
      earliest docs was wrong — **confirmed (per postmarketOS wiki, 2026-07-24): the real SoC
      is MSM8929 / Snapdragon 415**, a genuine big.LITTLE-shaped octa-core (4× Cortex-A53 @
      1.1 GHz + 4× Cortex-A53 @ 1.4 GHz), which explains the two-cluster L2 topology outright).
   6. **Ruled out a static per-cluster frequency/throughput difference directly**: pinned 4
      workers exclusively to cluster 0 (`taskset -c 0-3`) and, separately, 4 workers exclusively
      to cluster 1 (`taskset -c 4-7`), each running *alone* with zero cross-cluster contention,
      same PMU events as the item-3 sweep. Result: **virtually identical** — 46.394B vs 46.398B
      cycles, 33.535B vs 33.540B instructions, 103.89M vs 103.80M L2 refills (<0.01% apart).
      **There is no inherent clock or cache difference between the two clusters** *under these
      isolated test conditions*. Both are identical when either one has exclusive access to the
      shared downstream memory path. **Caveat added 2026-07-24 after confirming the real chip
      (MSM8929/Snapdragon 415) has genuinely different rated clocks per cluster (1.1 vs 1.4 GHz)**:
      the measured ~772 MHz effective clock in this isolated test is *below both* rated maxes —
      this specific memory-bound workload doesn't push either cluster to its ceiling alone under
      the tested conditions, so this test doesn't rule out the real 1.1/1.4 GHz asymmetry mattering
      more once both clusters compete under full 8-worker thermal/power pressure. The "dynamic
      arbitration, not frequency" conclusion still holds for *this* isolated comparison (both
      clusters equal to each other), but the real frequency difference is a separate, not-yet-
      isolated factor that may compound with the arbitration effect under full contention.
   7. **Conclusion**: the 2:1 split is a **dynamic interconnect-arbitration effect that only
      appears when both clusters compete for the shared memory path simultaneously** — not a
      static hardware asymmetry, not thermal throttling, not a "power cap." Cluster 0 (cores 0-3)
      wins that arbitration under contention; cluster 1 (cores 4-7) loses roughly half its
      throughput. This is a genuine SoC interconnect characteristic affecting both miners equally
      — not a code-quality gap in either one.
   8. **This fully explains item 2's worker-sweep efficiency curve mechanistically, not just
      empirically**: workers 1-4 map to cluster 0 alone (confirmed core mapping) — zero
      cross-cluster contention, hence 98.5% efficiency at 4 workers, essentially the isolated
      peak. Worker 5 is the *first* worker on cluster 1, immediately hitting the arbitration
      penalty — matching the sweep's own step-down (98.5%→89.2% between 4 and 5 workers, a
      bigger single-step drop than any other point in the curve). Workers 6-8 add the rest of
      cluster 1, each at roughly half rate, producing exactly the smooth-looking decline
      through 8 that item 2 measured. **Item 3's original "core-count-triggered power/current
      cap" hypothesis is retracted** — the apparent per-core clock drop at 6/8 workers was
      simply the *average* of a full-rate cluster and a half-rate (arbitration-losing, not
      slower-clocked) cluster once the worker count started spanning both.
   9. **A real, quantified, cluster-normalized answer to "why can't armrx match XMRig"**: comparing
      like-for-like (fast cluster to fast cluster, slow cluster to slow cluster) instead of raw
      aggregates — XMRig cluster-0 sum 18.62 H/s vs. armrx 16.78 H/s (armrx at **90.1%** of
      XMRig); XMRig cluster-1 sum 9.67 H/s vs. armrx 8.52 H/s (armrx at **88.1%** of XMRig).
      **armrx is consistently ~10-12% behind XMRig on both clusters independently** — a real,
      modest, now-precisely-quantified gap, not the vague "far behind" impression the raw
      28.28-vs-24.95 aggregate comparison gave (which was conflating a real ~10-12% code-level
      gap with this interconnect effect that hits both miners identically).
   - **Consequence for item 4**: since this isn't thermal or a power cap, `isolcpus=`/
     `nohz_full=` were never going to touch it anyway — correctly still blocked/deferred for
     unrelated reasons (§ below), but now for the right reason: this is an interconnect-hardware
     fact, not a scheduler-visible one.
   - **Not immediately actionable in software**: RandomX's per-hash workload is inherently
     symmetric across workers, so there's no obvious way to "protect" cluster 0 from cluster 1's
     presence without simply not running workers there — which would forfeit real throughput
     (cluster 1 still nets +8.5 H/s even at half rate). This is a documented hardware
     characteristic to design around in future analysis, not a bug to fix.

   **Follow-up: what accounts for the remaining ~10-12% code-level gap to XMRig?** Attached the
   same PMU event set to XMRig's own live process (`perf stat -p <pid>`, 8 threads, real pool job,
   steady state after 5:51 runtime) for a direct comparison against armrx's 8-worker numbers above:

   | Metric | armrx (8w) | XMRig (8t) | |
   |---|---|---|---|
   | IPC | **0.731** | 0.612 | armrx is *better* per-instruction |
   | `ld_dep_stall` (% cycles) | **11.41%** | 16.70% | armrx stalls *less* per-instruction |
   | L2D refill /1K instr | **3.274** | 4.488 | XMRig touches L2 more *densely* per instruction |
   | dTLB miss /1M instr | **1.40** | 9.26 | XMRig ~6.6× higher, still tiny in absolute terms |
   | Instructions/hash (derived from measured H/s) | 132.93M | **99.57M** | armrx uses **33.5% more** |
   | Cycles/hash (derived from measured H/s) | 181.96M | **162.59M** | armrx uses **11.9% more** — matches the ~10-12% cluster-normalized gap above almost exactly |

   **The gap is not a stall/scheduling problem — armrx is already ahead of XMRig on IPC and
   stall rate.** It's an **instruction-count/code-density problem**: armrx needs ~33.5% more
   total instructions to do the same hash computation. That surplus is individually "cheap"
   (armrx's per-instruction efficiency is better), but it adds up to the measured ~11.9%
   cycle-level gap, which is the real, final number.
   **This satisfies item 14's gate in spirit** — a real, measured instruction-count gap now
   exists, not the old debunked 31%-branch-miss-era estimate item 14 explicitly required before
   reopening the peephole-JIT idea. It is **not yet region-scoped** (item 14's literal ask) —
   this is a whole-process comparison; which specific opcodes/pipeline phases (VM opcode
   emission vs. superscalar-hash vs. AES/scratchpad-fill) concentrate the 33.5% surplus is still
   unknown. Recommended next step, bounded (hours, not the 3-6 week full rewrite): use
   `ARMRX_JIT_PROFILE`/`--jit-dump` to break armrx's own instruction count down by
   opcode/phase first, before committing to anything larger — narrow the target before
   investing, per this project's standing discipline (and per items 7-9's lesson that guessing
   at generic "coalescing"-shaped fixes without a specific target has a poor hit rate: 2 of 3
   were already-fixed, 1 was a regression).

   **Region-scoped breakdown, done (2026-07-24).** `./armrx --jit-dump` compiles one program and
   dumps a full opcode boundary table (offset + size per emitted instruction); the dump chains
   all 8 RandomX programs (256 instructions each, ~2047 total entries) — the recorded `size` per
   entry is accurate at compile time regardless of later JIT-buffer reuse (the byte-reuse caveat
   from `test_jit_encodings.cpp` only affects re-reading *current* memory contents, not these
   already-recorded sizes), so aggregating all ~2047 entries gives a statistically solid,
   frequency-weighted opcode breakdown for the main per-hash VM program path (does **not**
   include the separate superscalar-hash/dataset-item-derivation generator, item 9's target):

   | Category | Share of code bytes | Notes |
   |---|---|---|
   | **Memory-operand opcodes** (`*_M`: `IADD_M`, `ISUB_M`, `IMUL_M`, `IXOR_M`, `IMULH_M`, `ISMULH_M`, `FADD_M`, `FSUB_M`, `FDIV_M`) | **37.13%** | Each needs an address-compute preamble (`emitAddImmediate` + AND-mask for scratchpad wraparound + `LDR` [+ `SXTL`+`SCVTF` for the FP variants]) — 4.5-7.9 ARM instructions per opcode just for the address+load, before the actual operation. `FDIV_M` is the single most expensive opcode emitted, avg 31.4 bytes (~7.9 instructions). |
   | **CBRANCH** | **19.34%** | Single largest individual opcode by code volume — avg 20 bytes (5 ARM instructions) per occurrence, 9.14% of all emitted instructions. |
   | **ISTORE** | 11.19% | avg 15.24 bytes (~3.8 instructions). |
   | Register-only opcodes (`*_R` + `IMUL_RCP`) | 28.51% | Already lean — most average exactly 4 bytes (1 instruction): `FADD_R`, `FSUB_R`, `FSQRT_R`, `FSCAL_R`, `FSWAP_R`, `IMULH_R`, `ISMULH_R`, `INEG_R`, `IROR_R`, `IMUL_RCP` all hit the 1-instruction floor already. |

   **Initial read (below, struck through) turned out wrong on both counts — corrected the same
   day after actually reading the relevant code instead of just the byte-count table.**
   ~~Two concrete, quantified candidates for the instruction-count gap: memory-operand address
   computation (37%) and CBRANCH's per-occurrence instruction count (19%).~~

   **Correction 1 — wrong region entirely.** This table only covers the main per-hash VM
   program: a *fixed* 8×256 = 2047 instructions, executed once per hash. Measured
   instructions/hash is ~132.93M (§ above) — meaning **>99.998% of armrx's actual instruction
   volume is not in this table at all**. In light mode, each of the 2048 VM-program memory
   reads can trigger a full on-demand dataset-item derivation
   (`generateSuperscalarHash()` — item 9's target), invoked far more often than the fixed
   main program runs. The dominant instruction volume almost certainly lives there, not here.
   This breakdown is still accurate for what it measures, just not the right region to explain
   the XMRig gap.
   **Correction 2 — neither identified candidate survives closer reading.**
   - *Memory-operand address computation*: re-reading `emitAddImmediate` (`jit_compiler_a64.cpp:586-619`)
     shows it already uses the tightest available encoding for this immediate range — 1-2
     `ADD`-immediate instructions for anything under 2²⁴, which covers the whole scratchpad
     wraparound-masked range unconditionally. No slack found.
   - *CBRANCH's `tst`+branch preamble*: the idea was fusing the bit-test and forward-branch into
     a single `TBZ`/`TBNZ`. Doesn't apply — RandomX's CBRANCH condition tests an **8-bit-wide**
     field (`static_assert(ConditionMask == 0xFF)`, `jit_compiler_a64.cpp:1204`), and
     `TBZ`/`TBNZ` only test a single bit each.
   - Both `emitAddImmediate` calls in these paths are doing genuine spec-required work (the
     scratchpad wraparound mask; CBRANCH's `reg[dst] += imm`), not padding — there's no
     "obviously wasteful" ARM64 encoding choice to trim here. This, plus the superscalar-hash
     generator's own opcode handlers (read in full during item 9) already being mostly
     1-ARM-instruction-per-VM-opcode, suggests the remaining ~10-12%/33.5%-instruction gap is
     **not concentrated in easy, bounded, opcode-encoding-level fixes** the way a short peephole
     pass could catch.
   **Net conclusion**: finding the real source of the gap needs proper instrumentation of the
   superscalar/dataset-derivation path specifically (no existing `--jit-dump`-equivalent covered
   it — item 13 built this). A second option considered here — actual binary-level comparison
   against XMRig's generated code — is **deliberately not pursued**; see the clean-room boundary
   note after item 13 for why. **No code was changed as a result of this investigation** — a
   negative result, documented rather than acted on, same as items 7/8's
   stale-claim closures and item 9's reverted regression. See `NEXT_STEPS.md`/`changelogs.md`
   for the same correction and the discussion of what's actually worth pursuing next.
   ---
4. **`--rt-priority` + `isolcpus=`/`nohz_full=`** — **blocked on manual device access (2026-07-24), deferred.**
   `--rt-priority` currently falls back silently to the default scheduler
   (`[WARN] --rt-priority requires CAP_SYS_NICE; falling back to default scheduler.`) — granting
   it needs `setcap`, which isn't installed on this device (`libcap-utils` missing), and there's
   no passwordless `sudo` to install it. `isolcpus=`/`nohz_full=` need a kernel-cmdline edit +
   reboot of the physical device, which requires explicit user sign-off regardless of privilege
   availability. Left open, not attempted further without the user granting device access
   directly. Per item 3's finding, tempered expectations either way: if the 6→8-worker
   degradation really is a firmware power/current cap, CPU isolation won't touch it — it might
   help the front-loaded memory-contention component marginally at best.

*Contingent on step 1 above — **not applicable, step 1 came back "already coalesced, no-op" (see above). Do not implement 5/6 unless step 1 is re-run on different hardware and shows a different result.***
5. ~~**[ACT] Disclose + prefault the Argon2 cache's huge-page fallback path** (`argon2.cpp:270`)~~ — **closed as a no-op, 2026-07-24.**
6. ~~**[ACT] Reserve a persistent hugetlb pool at boot** (`vm.nr_hugepages`, system config, no code)~~ — **closed as a no-op, 2026-07-24.**

*Medium-term (1-2 weeks, moderate risk — only after short-term verification lands, per the reconciliation above):*
7. ~~Register-offset FP loads~~ — **closed 2026-07-24: stale claim, already implemented.**
   Read `JitCompilerA64::emitMemLoadFP()` (`src/jit_compiler_a64.cpp:654-681`) before touching
   it: it already emits `ldr d<Rt>, [x2, tmp_reg]` directly (line 671-672) — decoded the raw
   encoding (`0xfc606800 | (tmp_reg<<16) | (2<<5) | tmp_reg_fp`) by hand to confirm it's genuinely
   `LDR Dt, [Xn, Xm, LSL #0]`, the exact register-offset form this item proposed introducing, not
   just a stale comment. `grep -n "ld1"` across the file returns nothing — the `add x19,x2,x19` +
   `ld1 {vN.2s},[x19]` pattern the Hermes plan described does not exist anywhere in this codebase.
   Matches `ROADMAP.md`'s own Phase 1 log: `O13 | JIT register-offset FP loads | ✅`, done in an
   earlier session, well before this plan was written. **No code change made or needed.**
8. ~~Static FP load/convert software-pipelining~~ — **closed 2026-07-24: stale claim, already
   implemented.** Read `src/jit_compiler_a64_static.S:218-287` before touching it: the prologue's
   own comment says "Interleaved loading of FP registers (d16-d23) and integer registers (F0-F3)
   to hide load latencies and FP execution delays," and the code does exactly that — independent
   `ldr` batches (5+ at a time) followed by `sshll` batches followed by `scvtf` batches, further
   interleaved with independent integer XOR work to fill the gaps, more sophisticated than the
   plain 2/4/8-group pipelining this item proposed. Matches `ROADMAP.md`'s Phase 1 log:
   `O12 | JIT prologue instruction scheduling | ✅`. **No code change made or needed.**
9. ~~Superscalar literal-pool relayout~~ — **implemented, measured, reverted (2026-07-24): a
   real but small regression, not an improvement.** Premise verified accurate first (unlike
   items 7/8): `generateSuperscalarHash()` did emit a literal pool + always-taken `B` jump per
   program. Implemented the relayout as specified: one leading always-taken branch, a single
   consolidated pool sized by an exact up-front count of `IMUL_RCP` instructions across all
   programs (no heap allocation — a plain counting pass, consistent with this codebase's
   existing no-per-hash-allocation discipline), each `LDR literal` site's imm19 offset computed
   directly (no two-pass backpatch needed, since the pool's address is known before any code
   emits) and guarded with a real `ARMRX_ASSERT` range check (the original code had none —
   silent truncation on overflow — this closes that dormant gap regardless of the outcome
   below). Correctness: KATs + `test_jit_determinism`/`test_jit_equivalence`/`test_jit_encodings`
   all green, 12/12 on-device.
   **Measured apples-to-apples** (old code rebuilt fresh via `git stash`, same tool,
   `bench_armrx --full-hash-only` under `perf stat`, back-to-back): branches dropped **12.1%**
   (501.5M → 440.8M) exactly as the mechanism intended — but **cycles rose +1.65%** (96.64B →
   98.24B) and IPC fell (0.761 → 0.748), both real regressions. A direct `--mine --seconds=60
   --workers=1` hashrate check corroborated it: 268 → 265 hashes (**−1.12%**). Instructions and
   branch-misses were both essentially flat. **Same pattern as this session's Argon2 `memcpy`
   copy-elimination attempt**: the targeted overhead (branches, I-fetch pollution) really did
   go down, but the cost didn't disappear — moving every program's literal loads to one distant,
   shared pool at the front of the function plausibly trades better I-fetch/branch behavior for
   worse D-cache locality on the `LDR`-literal accesses themselves (a later program's load now
   reaches back across the entire preceding programs' code instead of a few bytes away), which
   costs more on this stall-bound core than the branches saved. **Reverted**
   (`git checkout -- src/jit_compiler_a64.cpp`), verified device rebuilt clean and back to
   12/12. This closes item 9 — no further superscalar literal-pool work planned without a new
   mechanism/hypothesis.
10. ~~Prefetch A/B matrix~~ — **adopted 2026-07-24: the "none" variant is a real, small,
    confirmed win — removed the three `prfm` hints permanently.** Target confirmed first:
    `jit_compiler_a64_static.S:381-383`'s three hints (`pldl1keep`/`pldl1strm`/`pldl1keep+32`)
    sit inside `.Lmain_loop`, the main VM execution loop — up to 16,384 executions/hash (2048
    iterations × 8 chained programs), distinct from the already-tuned `O10` dataset-item
    prefetch. Correctness verified first (12/12: KATs + `test_jit_determinism`/
    `test_jit_equivalence`/`test_jit_encodings`), both before and after the final adopted edit.

    **This took two rounds of measurement to get right, and the user's own skepticism of the
    first round is why the second round happened — worth recording as a methodology lesson:**
    - **Round 1 (wall-clock hashrate, 2 passes/condition, non-interleaved)**: with-prefetch
      24.94 H/s avg vs. no-prefetch 25.05 H/s avg (+0.44%). Read at the time as "within this
      device's ~0.2-0.3% pass-to-pass noise, a null result" — **but the two conditions were run
      in separate time blocks (all no-prefetch first, then a rebuild, then all with-prefetch)**,
      an unaddressed confound: any monotonic time-based drift over that ~10+ minute span could
      manufacture an apparent difference independent of the code. Challenged (correctly) before
      accepting this conclusion.
    - **Round 2 (wall-clock hashrate, interleaved A/B/A/B/A/B, 3 passes/condition)**: built both
      binaries once, ran alternating to distribute any drift across both conditions equally.
      With-prefetch mean 1498.67 vs. no-prefetch mean 1504.67 (+0.40%, same direction/magnitude
      as round 1 — reassuring against pure drift) but individual samples now *overlapped*
      (one with-prefetch run beat one no-prefetch run), landing at only borderline significance
      (p≈0.07 on a 3-vs-3 t-test) — genuinely inconclusive, not confidently either way.
    - **Round 3 (perf stat cycles/instructions, interleaved, 2 samples/condition, 20s each)**:
      switched to a lower-noise metric instead of just adding more wall-clock passes.
      Instructions completed in a fixed 20s window: with-prefetch 66,224,771,994 avg
      (samples 66,267,402,630 / 66,182,141,357) vs. no-prefetch 66,810,942,939 avg (samples
      66,798,047,203 / 66,823,838,674) — **+0.885%, zero overlap between conditions, and
      within-condition spread of only 0.04-0.13%** (a 7-20× signal-to-noise ratio). Cycles
      showed the same direction (+0.497%) with somewhat more spread (consistent with cycles
      being more thermal/DVFS-sensitive than raw instruction throughput). IPC was marginally
      *higher* without the hints too (0.7325 vs. 0.7296) — this is not a stall-hiding-vs-more-
      stalls tradeoff, the no-prefetch version is cleanly better on every axis measured.
    - **Conclusion**: real, small (~0.4-0.9% depending on metric), confidently-measured win.
      **Adopted** — the three `prfm` lines are permanently commented out with a dated
      explanation in the source (not deleted, so the exact removed instructions and the
      reasoning stay visible for anyone revisiting this). Full numeric account in
      `changelogs.md`.
    - **Why this matters beyond the ~0.5% itself**: round 1's initial "null" read would have
      been wrong, and wasn't caught by adding more of the same (wall-clock) measurement — it
      needed a *different, more sensitive* measurement to resolve. Worth remembering for any
      future small-effect-size test on this device: wall-clock hashrate noise (~0.2-0.5%) can
      swamp real effects of similar or smaller magnitude; `perf stat` cycle/instruction counts
      have roughly an order of magnitude less noise and should be the default tool for anything
      expected to be a single-digit-percent change, not a fallback after wall-clock is
      ambiguous.
    - Remaining matrix cells (single-hint variants, `L1STRM`/`KEEP` swaps vs. the now-adopted
      "none") not tried — lower priority now that the coarsest cut (all-or-nothing) already
      landed on a real win; could still be explored if someone wants to see whether a *partial*
      prefetch configuration beats "none" outright, but "none" is now the shipped baseline to
      beat, not the original three-hint version.
11. ~~Fused hash-and-fill nonce pipeline~~ — **closed 2026-07-24: benchmarked, failed its own
    gate, no mining-engine integration attempted.** Per the design notes' explicit prerequisite
    ("bench first, integrate only on a measured win"), added a direct primitive-level benchmark
    to `tests/bench_armrx.cpp` (`bench_aes_primitives()`) comparing `hash_and_fill_aes_1r_x4`
    (one fused 2 MiB traversal) against `hash_aes_1r_x4` + `fill_aes_1r_x4` run back-to-back
    (two separate 2 MiB traversals — exactly what the mining hot path does today at a nonce
    boundary). Result, 30 samples each: separate = 57,084.78 μs; **fused = 59,124.84 μs — ~3.6%
    *slower***, not faster. Consistent with this session's other findings on this in-order core:
    the fused loop keeps both hash-accumulation and fill-generation state live every iteration
    (more register pressure, more interleaved instruction types per 64-byte stride), while the
    separate version gets two tight, specialized, uniform passes — touching each byte once
    didn't beat touching it twice with simpler per-pass structure. **No `mining_engine.cpp`/
    `vm.cpp` changes made** — the nonce-pipelining integration (worker-loop restructuring,
    first/next/last state machine, job-change/shutdown flush logic, new equivalence tests) was
    never attempted, since its premise failed at the cheap, low-risk primitive-benchmark stage
    before any of that real implementation risk would have been incurred. Benchmark code kept
    in `bench_armrx.cpp` as reusable reference (same treatment as the NEON-AES experiment).

    **Follow-up, resolved same day**: the same benchmark run's separate `--full-hash-only` pass
    showed an anomalous 2.24 H/s (half the ~4.27 H/s baseline). Traced to a `bench_armrx.cpp`-
    specific artifact on cluster 1 (cores 4-7) — confirmed *not* present in real mining: `armrx
    --mine`, verified via `/proc/<pid>/task/*/stat` to be genuinely pinned to physical core 4
    (ruling out `MiningEngine`'s internal `hwloc` affinity silently overriding external
    `taskset`), measured the full expected 4.27 H/s on that same core over a sustained 90s run.
    Both binaries call identical `randomx_calculate_hash()` with identical flags and identical
    huge-page allocation paths, so this is isolated to the standalone benchmark harness, not a
    real mining-relevant finding — today's earlier per-core/per-cluster measurements (which all
    used the real `armrx --mine` path) are unaffected and stand as correctly measured. Full
    account in `changelogs.md`'s item 11 entry.

*Long-term (weeks, high risk — do not start without a measured hypothesis from the medium-term items):*
12. **Conservative 2-3-instruction emitter lookahead scheduler — implemented, measured, adopted, 2026-07-25.** Barriers at CBRANCH/CFROUND, all scratchpad memory ops treated as aliasing, targets item 14's measured dominant cost (`IMUL_R`/`IMULH_R`/`ISMULH_R`/`IMUL_RCP`, >35% of mining cycles). Full design in `src/jit_compiler_a64.cpp` above `scheduleProgram()`: RAW/WAR/WAW hazard analysis across the int/f/e register files, a CBRANCH "anchor" constraint (the last writer of a CBRANCH's target register must never move relative to its neighbors, since the JIT's generated loop body is a physical code range, not an index range like the interpreter's), and a fourth hazard found only by stress-testing — several ALU handlers materialize a compile-time immediate into a shared physical scratch register (x20) when `src==dst`, invisible to the VM-register-level hazard model, fixed by excluding any `src==dst` instruction from the swap's Q/R positions. Verified via a new dedicated differential stress test (`tests/test_jit_scheduler_stress.cpp`, 450 (seed, input) JIT-vs-interpreter pairs across 3 Argon2 caches, not just the standard 16-seed suite).

    **First measurement (main VM program only): a clean null.** IPC +0.016% (noise-level, 3 interleaved trials/condition, zero overlap on other counters) plus a real, reproducible **+24.9% branch-misses** with no compensating benefit. Root cause: `scheduleProgram()` was only wired into `emitPrologueMix()` (the main, once-per-hash VM program), but item 14's own correlation found the dominant IMUL cost lives in `generateSuperscalarHash()`'s output (the dataset-derivation region, executed 16,384x/hash in light mode) — a completely separate emission path the scheduler never touched. User chose to extend rather than revert.

    **Extension to the superscalar/dataset-derivation path — implemented, measured, adopted.** This instruction set (`SuperscalarInstructionType`, 14 opcodes) is structurally simpler: no CBRANCH/CFROUND (straight-line only, no anchors needed), no memory ops, a single flat 8-register file. A new hazard specific to this path was found by *reading* `generateSuperscalarHash()` (not stress-test bisection this time): `IMUL_RCP`'s reciprocal literals are populated by a pre-pass in original program order, then consumed by the main emission loop via a simple incrementing pointer — safe only if no two `IMUL_RCP` instructions ever have their relative emission order changed. Fixed by excluding `IMUL_RCP` from a swap's Q/R positions (the main path's own `h_IMUL_RCP` was checked and does *not* have this problem — it computes its literal slot from its own call count self-contained per call, unlike the superscalar pre-pass design). `scheduleSuperscalarProgram()` in `jit_compiler_a64.cpp`; declared in `jit_compiler_a64.hpp`. Verified via a second dedicated stress test, `tests/test_jit_superscalar_scheduler_stress.cpp` — deliberately shaped around **many distinct seeds** (100 seeds x 2 inputs = 200 pairs across 800 individual superscalar programs) rather than many inputs per seed, since `generateSuperscalarHash()` compiles once per seed rotation, unlike the main program which recompiles every hash. Both stress tests plus the full existing suite (`armrx_tests`, `test_mining`, `test_jit_encodings`, `test_jit_determinism`, `test_jit_equivalence`) all green on-device with both schedulers active together.

    **Final performance measurement**: 3-way `perf stat` comparison (baseline `906b96e` / main-only `be94b1f` / full, `taskset -c 0`-pinned single-worker steady-state windows, 2 independent rounds with reversed run order to rule out thermal drift, 6 samples/condition total). Combined result: **full vs baseline: IPC +0.233%, cycles −0.036%** (consistent direction both rounds: +0.183%/+0.284%); **full vs main-only: IPC +0.318%** (also consistent: +0.285%/+0.352%) — the superscalar extension is what makes this a net positive; main-only alone remained a small, consistent regression vs baseline (−0.085% IPC) even with the extension's other correctness fixes in place. Branch-miss rate rose from 2.54% (baseline) to 3.14% (full, +23.8% relative) — a real cost, but net cycles/IPC still improved, meaning the stall-hiding benefit outweighs it. Smaller than the original "2-6%" estimate, but a genuine, reproducibly-measured win (signal clearly exceeds the ~0.02-0.23% spreads seen at each condition) — **adopted**.
13. **Build real instrumentation for the superscalar/dataset-derivation path — done 2026-07-24,
    tooling built and validated, partial region-scoped data obtained.** `--jit-dump` only
    covered the fixed, 2047-instruction main VM program (executed once per hash); measured
    instructions/hash is ~132.93M, so the dominant instruction volume needed its own tooling.

    **First, confirmed the exact mechanism by reading the assembly**, not assuming it:
    `generateSuperscalarHash()` is called from `VirtualMachine::set_cache()`
    (`vm.cpp:175`) — **once per seed rotation, not once per hash** — and JIT-compiles the
    dataset-item-derivation code once. That compiled code is then *executed* (not
    recompiled) via `bl rx_calc_dataset_item` inside
    `randomx_program_aarch64_vm_instructions_end_light`
    (`jit_compiler_a64_static.S:557`), reached from **every iteration of the main VM loop**
    in light mode — `RANDOMX_PROGRAM_ITERATIONS` (2048) × 8 chained programs = **16,384
    calls per hash**. This is the concrete mechanism behind the instruction-count dominance,
    not just an inference from subtraction.

    **Built the instrumentation**: extended the existing `JitDumpEntry`/`--jit-dump`
    mechanism (same idea as `--jit-dump`'s main-program table) to cover
    `generateSuperscalarHash()`'s emitted code — new `superscalar_jit_dump_` vector +
    `getSuperscalarJitDump()` accessor (`jit_compiler_a64.hpp`), instrumented the
    per-instruction emission loop in `generateSuperscalarHash()`
    (`jit_compiler_a64.cpp`), and extended `dumpJitCode()` to print a per-opcode aggregate
    table for this region. Verified 12/12 on-device (KATs + `test_jit_determinism`/
    `test_jit_equivalence`/`test_jit_encodings`) — the instrumentation only records
    metadata about already-emitted bytes, doesn't change codegen, but this is
    correctness-critical tooling touching the JIT compiler so it got the same verification
    discipline as any other change here.

    **Real data from `--jit-dump`**: one `generateSuperscalarHash()` call (= one
    `rx_calc_dataset_item` compile) emits **3,563 instructions, 20,916 bytes** of variable
    superscalar-opcode code (`IMUL_R` dominates at 14.82% of bytes; the three `_C7`/`_C8`/
    `_C9` immediate-constant variants of `IADD`/`IXOR` average 12 bytes/instruction — the
    `emitMovImmediate`+`EOR`/`emitAddImmediate` sequences already discussed for the main
    program; everything else is a lean 4 bytes/instruction). Scaled: 3,563 × 16,384 calls
    ≈ **58.4M instructions/hash — ~44% of the ~132.93M total**, from this region alone.

    **Honest scope of what this does and doesn't establish**: the 58.4M figure is a *lower
    bound* — it covers only the variable superscalar-opcode portion my instrumentation
    tracks, not the fixed prefetch/mix/store-result wrapper chunks copied around each of
    the 8 cache-access rounds inside one call, nor the main VM loop's *own* fixed
    per-iteration wrapper code (interleaved FP/int loads, `FE_mix` AES tweak, the
    `xor_with_dataset_line` step itself, `spMix` update, prefetch, store) that executes on
    all 16,384 iterations regardless of the small variable VM-instruction region. Those
    remain uninstrumented — this closes the *tooling* gap item 13 was created for, and
    gives the first real, code-confirmed number for the dominant region, but does not yet
    fully reconcile the remaining ~55% of instructions/hash. That reconciliation is item
    14's job now that it has real tooling to work with instead of none — done entirely
    against armrx's own code, per the clean-room boundary below.

    > **Clean-room boundary — decided 2026-07-24, permanent.** This project is a clean-room
    > implementation built independently against the RandomX spec, explicitly *not* derived
    > from an existing mining client (see `CLAUDE.md`). Comparing aggregate, black-box
    > behavior against XMRig — hashrate, `perf stat` counters, whole-process instruction
    > counts — is fine and has already produced real findings (item 3's two-cluster
    > topology discovery, the ~10-12% cluster-normalized gap, the ~33.5% instruction/hash
    > gap). Inspecting XMRig's *internals* — disassembling its JIT-generated machine code,
    > diffing it region-by-region against armrx's own codegen — is a different thing:
    > legally fine, but in real tension with this project's own stated identity, so it's
    > ruled out going forward. This isn't an oversight or a scope cut for lack of time; it's
    > a deliberate boundary. Item 14 (below) was reframed on this date to drop the
    > `--jit-dump` + objdump XMRig-comparison framing it originally had, in favor of
    > self-directed analysis of armrx's own generated code. Any future task that would
    > require reading or disassembling another miner's compiled output should be rejected on
    > sight, not just deprioritized.
14. **Self-directed instruction-count reconciliation for the superscalar/dataset-derivation
    region** — item 13's instrumentation accounts for ~44% of instructions/hash (58.4M of
    ~132.93M); the remaining ~55% is the *fixed* per-call wrapper chunks inside
    `generateSuperscalarHash()` (prefetch/mix/store-result code repeated across the 8
    cache-access rounds, not yet counted by the variable-opcode instrumentation) and the
    main VM loop's own fixed per-iteration overhead in
    `randomx_program_aarch64_vm_instructions_end_light`
    (`jit_compiler_a64_static.S`) — interleaved FP/int loads, the `FE_mix` AES tweak,
    `xor_with_dataset_line`, `spMix` update, prefetch, store. Extend the instrumentation (or
    just count emitted bytes/instructions directly from the `.S` source and the fixed-chunk
    emission code) to get a real number for these regions, using only armrx's own code and
    first-principles ARM64 ISA reasoning — the same method items 7-10 already used
    successfully. Only after this reconciliation is complete does it make sense to judge
    whether `docs/plans/peephole-jit-plan.md`'s full peephole-JIT rewrite (3-6 week clean-room
    effort) is worth starting; do **not** start it on the old, now-debunked 31%-branch-miss-era
    estimate. No XMRig-side comparison is in scope for this item — see the boundary note
    above.

    **First static count toward this reconciliation (2026-07-24, self-analysis only):**
    counting instructions per labeled region directly from `jit_compiler_a64_static.S` and
    the `generateSuperscalarHash()` emission code gives, per `rx_calc_dataset_item` call:
    entry 36 + 8 rounds × (AND 1 + prefetch 3 + jump 1 + mix 12 + reg-update 1) + store 13
    ≈ **~185 fixed insns/call → ~3.0M/hash (~2.3%)**. The light-mode main VM loop's fixed
    per-iteration overhead (main_loop 40 + xor_with_dataset_line 12 + update_spMix1 9 +
    end_light chunks ~11 + light_dataset_offset 8 + `v2_FE_mix_soft_aes` 189 ≈ ~270/iter)
    adds ≈ **4.4M/hash**. So *all* fixed JIT-region wrappers together explain only ~7.4M of
    the missing ~74.5M — item 13's 58.4M + these ~7.4M + the ~4-5M main VM program body
    still leaves **~60M insns/hash unaccounted in emitted code entirely**. The remainder
    almost certainly lives in the C++ side (software T-table AES scratchpad fill/hash,
    Blake2b, superscalar-adjacent C++). Next step for this item is therefore *not* more JIT
    instrumentation but `perf record`/self-attribution of armrx's own binary by symbol —
    fully inside the boundary.

    **`perf record` self-attribution (2026-07-24, self-analysis only) — corrects the above
    hypothesis.** Ran `perf record -F 999 -g -e cycles` directly on `./armrx --mine
    --workers=8` under a representative light-mode run (20s warmup + 60s steady-state, all 8
    workers confirmed contributing — worker[0-3] 3.20 H/s, worker[4-7] 1.60 H/s, matching
    item 3's known ~2:1 cluster-arbitration split almost exactly, a good consistency check),
    486K samples. This is the first *live system-profiler* capture of the real mining hot
    path in this project — everything before this was static (`--jit-dump`, `.S` source
    counting).

    Result: only **~14-15% of self-time samples resolve to a named C++ symbol** at all —
    dominated by `randomx_calculate_hash` (~12%, the C++ entry point wrapping the whole
    per-hash call) and `permute_16_neon`/`Argon2dCache::initialize` (~1-2% combined). The
    remaining **~85%** doesn't resolve to any named function: ~44% shows as thousands of
    distinct raw hex addresses (`[.] 0x0000ffff...`), and a further ~41% doesn't even resolve
    a leaf frame in the call-graph view at all (consistent with `-O3` frame-pointer omission
    plus zero unwind/CFI info existing for a raw JIT buffer). **Directly verified, not
    inferred**: dumped `/proc/<pid>/maps` during a live mining run and found multiple
    (~1/worker) small **150-370 KiB `rwxp` anonymous mappings** sitting in the exact same
    `0xffffXXXXXXXX` address range as the unresolved perf samples — i.e. these samples
    provably land inside the runtime-JIT-compiled code, which has no symbol table and is
    invisible to any generic profiler or disassembler by construction, not a profiling
    artifact.

    **This corrects the prior hypothesis, not confirms it.** The remaining ~60M
    instructions/hash do *not* primarily live in the C++ side — `ROADMAP.md`'s own region
    breakdown already measured AES scratchpad fill/hash at only 0.3% and Blake2b at 0.0% of
    hash *time*, and this session's named-symbol total (~14-15%) is consistent with that
    being small. The missing instructions are overwhelmingly still inside JIT-generated code
    — just not the parts item 13/14's static counting has covered so far (the variable
    superscalar-opcode region, the fixed per-call wrapper chunks, and the main VM program's
    fixed per-iteration overhead). Something in the JIT-generated code is bigger than any of
    those three static counts accounted for.

    **Concrete next step, still fully self-directed**: correlate perf's raw sample addresses
    against item 13's own `JitDumpEntry`/`getSuperscalarJitDump()` offset tables — subtract
    each worker's JIT-buffer base address (readable from `/proc/<pid>/maps` at capture time,
    or exposed via a small diagnostic hook) from each sampled IP, then look up which
    RandomX-opcode byte range that offset falls in. This gives real opcode-level cycle
    attribution *inside* the JIT code, which neither `perf report`'s generic symbolization
    nor the static instruction count alone can produce — a small, self-contained correlation
    script using only armrx's own tooling, not yet built.

    **Correlation script built and run (2026-07-24) — real opcode-level cycle attribution
    obtained.** `dumpJitCode()` extended to print (a) the per-entry superscalar boundary
    table (previously only an aggregate was printed, matching the main table's existing
    per-entry format) and (b) the buffer's true allocated size (`CodeSize +
    CalcDatasetItemSize`) and `CodeSize` alone, as ground truth for matching against
    `/proc/<pid>/maps`. New `tools/jit_correlate.py`: parses `--jit-dump` output, a
    `/proc/<pid>/maps` snapshot, and `perf script -F ip` output; matches worker JIT buffers
    by size (rounded to the page boundary mmap/mprotect actually use); **critically, THP
    (`always` policy on this device) was directly observed merging adjacent worker buffers
    into single VMAs of 1×, 2×, and 4× the per-buffer size in the same snapshot** — handled
    by splitting any region whose size is a whole multiple of the per-buffer size into that
    many equal sub-regions, otherwise workers sharing a merged region would be attributed
    against the wrong (region-start, not buffer-start) base. Two real gotchas hit and fixed
    along the way: `pgrep -f './armrx --mine'` matched the *invoking shell's own* command
    line (since the whole capture sequence is itself passed to `sh -c '...'`), not the real
    process — fixed by matching `/proc/<pid>/comm` exactly instead; and `perf script -F ip`
    still emits the full call chain per sample as a blank-line-separated block (leaf frame
    first, then unwound callers), not one address per line — counting every frame as an
    independent sample would have inflated/skewed the result, fixed by taking only each
    block's first line.

    **Result** (486K-ish sample run, same methodology as the `perf report` pass above):
    **14.25%** of samples fall outside any worker JIT buffer — matching the earlier
    `perf report` finding of ~14-15% named-C++ almost exactly, a strong cross-check between
    two independently-built methods. Of the **85.75%** inside a JIT buffer, **73.96%**
    (**63.42% of total samples**) matched a specific superscalar opcode via the boundary
    table — far more of the profile explained than the ~44%-of-instructions static count
    alone gave. Per-opcode breakdown of cycles (not just instruction count): **`IMUL_R`
    (20.98% of all samples) and `IMUL_RCP` (14.30%) together account for over 35% of every
    cycle spent mining** — by a wide margin the two most expensive superscalar opcodes,
    consistent with integer multiply's longer pipeline latency on an in-order A53 and this
    project's own established "memory/latency-stall-bound, not instruction-count-bound"
    framing. The remaining ~22% of total samples land inside a worker buffer but outside any
    superscalar entry (the static per-hash VM program/fixed-wrapper region, not attributable
    per-opcode by this method since it's regenerated every hash). New tool committed at
    `tools/jit_correlate.py`, fully self-contained (armrx's own `--jit-dump`/`perf`/`/proc`
    only, no external miner involved, per the clean-room boundary above).

    **`IMUL_RCP` literal-load elimination — implemented, measured, reverted (2026-07-25),
    small but real regression.** Given the correlation script's finding, tried the obvious
    next step: `IMUL_RCP`'s reciprocal is known at JIT-compile time, so replaced its
    literal-pool `LDR` + `MUL` (2 instructions, 16 bytes incl. the 8-byte literal) with a
    direct 64-bit immediate materialization (`MOVZ`/`MOVN` + up to 3× `MOVK`, new
    `emitMovImmediate64()`) + `MUL` — eliminating the load-to-use stall entirely, at the cost
    of up to 5 instructions instead of 2. Removed the now-dead literal-pool mechanism for
    this opcode (nothing else populated or read it). Correctness: no fixed per-instruction
    "slots" exist in this buffer — `codePos` is a plain sequential bump allocator across the
    whole `CalcDatasetItemSize` region, so the only real constraint is aggregate buffer size,
    not a per-occurrence boundary; verified via the full KAT/`test_jit_determinism`/
    `test_jit_equivalence`/`test_jit_encodings` suite (5/5 green) rather than ASan, which
    turned out to be unavailable on this device's toolchain entirely (`cannot find -lasan` —
    Alpine/musl doesn't ship a libasan; confirmed via `find / -name libasan*` after fixing an
    unrelated, real `CMakeLists.txt` bug found along the way — `ARMRX_ENABLE_ASAN`'s
    compile/link options were `PRIVATE` on the `armrx_core` static library, which never
    propagates to the executables that actually need to link the sanitizer runtime; fixed to
    `PUBLIC`, kept independent of this item's outcome).

    **Measured via three properly-controlled `perf stat` trials** (old vs. new binaries
    preserved side-by-side, `timeout`-wrapped to a truly fixed wall-clock window — the first
    attempt without an external `timeout` wrapper produced mismatched elapsed times between
    runs and had to be discarded), including one with reversed run order to rule out thermal
    drift: **IPC improved consistently and substantially as hypothesized (~0.729→~0.789,
    +8.2%)**, confirming the literal-pool load really was costing stall cycles — but
    **instruction count rose ~8.5-8.6%**, and for the identical amount of completed work
    (matched hash counts across trials), **total cycles needed rose ~0.25-0.34%** — a small,
    consistent net regression, the stall-elimination benefit almost exactly cancelled by the
    extra instructions' own cost. Same "cost relocated, not eliminated" pattern as item 9's
    superscalar literal-pool relayout and the Argon2 `memcpy` attempt. Reverted
    (`git checkout -- src/jit_compiler_a64.cpp include/armrx/jit_compiler_a64.hpp`), KATs
    re-confirmed 5/5 on the reverted build. The `CMakeLists.txt` ASan-propagation fix was
    kept (real, independent bug). **Diagnostic value, not wasted**: `IMUL_R`/`IMUL_RCP`'s
    cost is now understood to be genuinely latency-bound, not merely a memory-access
    artifact — any future attempt here needs to hide the latency (scheduling/reordering, item
    L1) rather than trade it for more instructions on this specific in-order core.
15. Re-run `devbox_pgo_build` (tool already exists, kept from Phase 5) after any medium/long-term item lands meaningfully — PGO nulled out on today's code shape, but a reshaped binary may reopen it. Free to re-check, never rebuild the tooling.
16. ~~Re-baseline `README.md`'s stale 5.2 H/s single-thread / ~28 H/s 8-worker figures honestly~~ — **✅ done (2026-07-24)**, using item 2's sweep data: `README.md`'s performance table now shows 4.27 H/s (1 worker), and 16.82/21.13/24.95 H/s (4/6/8 workers) with per-point efficiency, replacing the stale "linear scaling" claim.
17. **Two independent full-codebase audits (Gemini, Hermes) cross-verified and acted on — 2026-07-25.** User supplied `docs/audits/PROJECT_AUDIT_REPORT_Gemini_25072026.md` and `docs/audits/PROJECT_AUDIT_REPORT_Hermes_25072026.md`. Per this project's standing "verify external claims before trusting them" discipline, every concrete claim was checked against the actual source before any fix, not implemented on the auditors' say-so.

    **Confirmed real, fixed:**
    - *Hermes 1.1* — `ARMRX_ASSERT`'s release-build semantics (log-and-continue, by design, documented in `assert.hpp`) meant `vm.cpp`'s `dataset_read()` OOB guard was a no-op in release builds, with the actual out-of-bounds read executing right after. Verified the exact bound math (`readPtr = dataset_offset_ + (ma_ & 0x7fffffc0)`, both terms bounded by construction) — spec-guaranteed in-bounds today, so this was **latent, not currently exploitable**, but a real defense-in-depth gap. Fixed with a targeted early-return at this one call site (not a macro-wide behavior change, which would've been much higher risk).
    - *Hermes 1.4* — `AffinityMode::BigOnly` hardcoded `thread_id % 4` for "the big cluster," completely ignoring `core_order_` (the already-detected, frequency-sorted topology) sitting in the very next branch. Real correctness-of-intent bug on any topology where the big cluster isn't cores 0-3. Fixed: new `count_top_frequency_cores()` helper derives the actual big-cluster size from the same `cpuinfo_max_freq` data `detect_core_order()` already reads, `BigOnly` now pins to `core_order_[thread_id % big_core_count_]`.
    - *Both audits, `worker_hashes_` false sharing* — confirmed a densely-packed `std::atomic<uint64_t>[]`, up to 8 workers' counters per 64-byte line. Fixed with a cache-line-padded `PaddedCounter` wrapper struct (`alignas(64)` forces `sizeof` up to 64 too, so this holds for arrays, not just the first element).
    - *Hermes 3.1* — stale comment in `h_IMUL_M` said `// sub dst, dst, tmp_reg` next to an `ARMV8A::MUL` emit (copy-paste artifact, no functional bug, but a maintenance trap). Fixed the comment; grepped for other op/comment mismatches nearby, found none.

    **Checked and refuted** (would have wasted effort or introduced unnecessary risk if acted on blindly):
    - *Gemini's FPCR-leakage claim* directly contradicted Hermes's own assessment of the same code. Read `randomx_calculate_hash()` directly: it's straight-line, `fesetenv` always runs at the end, no early-return or exception path exists between the `fegetenv`/`fesetenv` pair. Hermes was right; Gemini's claim doesn't hold.
    - *Gemini's "missing `isb` barrier after `__builtin___clear_cache`"* — very likely a false positive. GCC/Clang's AArch64 implementation of that builtin is specifically documented to already emit the `dc cvau`/`ic ivau`/`dsb ish`/`isb` sequence; that's the whole reason JIT authors use the builtin instead of hand-rolled asm.
    - *Gemini's `munmap` vs `freePagedMemory` abstraction mismatch* — real as a code-hygiene nit, but checked `freePagedMemory`'s actual Linux implementation: it's a literal null-checked `munmap(ptr, bytes)`, functionally identical to what `vm.cpp`/`mining_engine.hpp` already do directly. This project targets Linux/AArch64 only (`CLAUDE.md`), so the "cross-platform compatibility" impact Gemini claims doesn't apply here. Not fixed — correctly low priority.

    **Not yet acted on, needs measurement not blind adoption** (per this phase's own standing protocol below): `-mtune=cortex-a53` default tuning (Hermes 2.1, Gemini 4-ish overlap), interpreted-path dataset prefetch (Hermes 2.2), SIGSEGV/SIGBUS JIT-fault handler (Hermes 1.2), RWX-JIT-by-default reconsideration (Hermes 1.3 — already a known, deliberate, documented tradeoff per Phase 4, not a miss), windowed hash-rate reporting (Hermes 3.2), oversubscription/forced-light warnings (Hermes 3.3/4.1), BOLT (Hermes 2.5). Left for a future pass; none are correctness bugs, all are speculative-until-measured per this project's own rule.

    Verified: local x86 full suite 7/7, on-device (AArch64) targeted suite (KATs/`test_mining`/`test_jit_encodings`/`test_jit_determinism`/`test_jit_equivalence`) 5/5, both 100%. Build warning count unchanged from baseline (56).

**Not adopted / explicitly deprioritized:** full peephole-JIT coalescing without a fresh region-scoped gap measurement (item 14's gate); any further NEON/hardware-AES attempt (three independent measured regressions is enough evidence the per-block NEON ld/st overhead is structural on this A53); custom allocators/memory pooling (no hot-path allocation exists to pool, confirmed independently by both plans and Phase 1/4's own audits).

**Standing protocol for this phase** (from both source docs, consistent with every prior phase's discipline): KATs green before any benchmark; old code rebuilt fresh for every comparison; back-to-back runs, thermal variance on this device is real; trust instruction/cycle counts over wall-clock for micro-changes, wall-clock H/s for end-to-end; document negative results in `docs/` whether or not a lead pans out.

**Side-fix while running this phase's verification (2026-07-24): devbox MCP tooling had two real bugs, both fixed.**
Executing items 1–2 above surfaced the same class of tooling problem the Phase 5 PGO work found
in this same file last session (`tools/devbox/devbox_mcp.py`):
1. `tool_test()` called `cfg.timeout()` (the 120s **`"default"`** bucket) instead of a bench-scale
   one, so any unfiltered `devbox_test` call — the full 12-test suite, `bench_armrx` alone ~300s —
   was essentially guaranteed to time out. Fixed by adding a dedicated `"test"` timeout bucket
   (900s) and using it at that call site; added to `devbox.json`/`devbox.example.json`.
2. The MCP server itself is a single-threaded, synchronous stdin-read loop that ran tool handlers
   inline — while a long call (test/build/bench) was executing, the server couldn't read or
   answer *anything* else on stdin (including the host's own liveness `ping`), so the host
   concluded the server had hung and force-reconnected mid-call, losing in-flight work twice
   during this session before being root-caused. Fixed by running each `tools/call` dispatch in
   a worker thread (keeping the main loop free to answer `ping`), with a `_device_lock` around
   the actual remote SSH/rsync invocations so this doesn't let two device operations race each
   other — it only frees the JSON-RPC loop, it doesn't parallelize device access.
   **Verified**: a real single `devbox_test` call completed cleanly in 612.72s (12/12 passed) —
   past both the old 120s bucket and the 600s bench bucket, with the connection staying up the
   whole time unattended. (One invalid test methodology along the way: manually firing a *second*
   concurrent tool call while the first was in flight reliably killed the connection immediately —
   this MCP host's client appears to only tolerate one in-flight request per connection; that is
   separate from, and not fixed by, the threading change, and is not a supported usage pattern.)
