# armrx — Performance & Optimization Master Plan

**Author:** performance audit pass, 2026-07-24
**Target device:** Cortex-A53 ×8 @ ~1.2 GHz, 2 GiB single-channel LPDDR3, postmarketOS / musl / GCC 15.2
**HEAD reviewed:** `88f4122`
**Scope:** JIT hot path (`vm.cpp` → `jit_compiler_a64_static.S`), dataset/cache memory, worker threading.

> **Read this first — honest framing.** This codebase is *mature*. Five prior sessions have
> profiled it hard and closed almost every code-level lead, several with documented negative
> results (CSEL/CBRANCH, PGO, NEON-AES, Newton-Raphson div/sqrt, `memcpy` elimination,
> `--stagger-ms`). I re-verified those closures against the current source; they are correct.
> **Do not re-open them.** The genuinely-actionable remaining surface is therefore small and
> lives mostly *below* the C++ layer: memory-page backing, worker-count vs. bandwidth, and
> kernel/system tuning. Every claim below is tagged **[VERIFY]** (needs an on-device
> measurement before you trust it), **[ACT]** (do it), or **[CLOSED]** (recorded so nobody
> re-does it). Follow the house rule: *measure apples-to-apples on real hardware, KATs before
> benchmarks, document negatives.*

---

## 0. The one architectural fact that reframes everything

`ROADMAP.md` says "~5.2 H/s single-thread." The device has **2 GiB RAM**. Fast mode needs a
**~2080 MiB dataset** (`RANDOMX_DATASET_BASE_SIZE` = 2 GiB + 32 MiB), which **cannot coexist
with the OS in 2 GiB**. `MiningEngine` therefore runs in **light mode** on this hardware
(`mining_engine.cpp:169` only builds a dataset `if (mode_ == RandomXMode::fast)`; the memory
picker in `memory.cpp` reserves 256 MiB and won't select fast).

Consequences that dominate the whole performance profile:

```
                       fast mode (won't fit)          light mode (what actually runs)
 per dataset read   →  1× 64-byte load from 2 GiB   vs  full SuperScalarHash recompute:
 (2048× per hash)      dataset (pure bandwidth)         rx_calc_dataset_item →
                                                        RANDOMX_CACHE_ACCESSES(8) ×
                                                        [random 64B load from 256 MiB cache
                                                         + a ~450-instr superscalar program]
```

So on this box the hot loop is **not** bandwidth-bound on a 2 GiB dataset — it is bound by
(a) the **superscalar compute** injected inline by `generateProgramLight()` /
`randomx_calc_dataset_item_aarch64`, and (b) **random 64-byte reads scattered across the
256 MiB Argon2 cache** — 8 per dataset read × 2048 reads × = **~16 K random cache probes per
hash**. That cache, not the 2 MiB scratchpad, is the TLB- and latency-critical working set.
**This is the single most important thing to internalize; several "obvious" optimizations
(dataset prefetch tuning, fast-mode bandwidth tricks) are simply not on this device's path.**

---

## 1. Hotspot & Bottleneck Analysis

### 1.1 Where the cycles actually are (measured, prior sessions + confirmed by code)

| Region | Share of hash | Code | Nature |
|---|---:|---|---|
| JIT main-loop execution | **98.24%** | `randomx_program_aarch64` (static.S) | memory-latency + FP/ALU |
| — of which, light-mode dataset derivation | large fraction of the 98% | `rx_calc_dataset_item` + inlined superscalar | random 256 MiB cache loads + superscalar ALU |
| JIT program compile | 1.76% | `generateProgramLight()` (per run, 8×/hash) | code emit + `__builtin___clear_cache` |
| AES scratchpad fill/hash | 0.3% + 0.5% | `aes_hash.cpp` | T-table software AES |
| Blake2b chaining | ~0.0% | `blake2b.cpp` | 8× 256-byte hashes/hash |

IPC is **0.708** on the A53 (in-order, dual-issue). That number is the tell: this is a
**latency/memory-stall-bound** workload, not an instruction-throughput-bound one. Shaving
instructions off the JIT body (the peephole-coalescing idea) fights the wrong bottleneck —
which is exactly why CSEL and Newton-Raphson *added* instructions "for free" and still lost.

### 1.2 Cache / TLB — the real pressure point

- **Scratchpad**: 2 MiB/worker, huge-page backed (`vm.cpp:132` `allocLargePagesMemory` →
  MAP_HUGETLB, fallback THP). 2 MiB = exactly one huge page. This part is done right.
- **Argon2 cache**: 256 MiB, `allocLargePagesMemory` with a **silent fallback** to plain
  anonymous mmap + `MADV_HUGEPAGE` (`argon2.cpp:270-277`). If the hugetlb pool is empty (the
  default on a stock 2 GiB postmarketOS kernel), the request falls through to **THP**, which
  on a memory-pressured 2 GiB device may only *partially* coalesce. Worst case: 256 MiB on
  4 KiB pages = **65 536 page mappings** probed randomly. The A53's L1 dTLB is ~10 entries and
  the L2 TLB ~512 entries — i.e. the cache is **500× larger than the TLB reach**. Every one of
  the ~16 K cache probes per hash risks a page-table walk. **[VERIFY]** — this is lead #1
  below and is currently *unmeasured*.

### 1.3 Branch behavior — [CLOSED]

The "31.08%" branch-miss figure is a benchmark artifact; real hot-path miss rate is **2.4%**,
costing ~0.1–0.16% of cycles (`docs/branchless-cbranch.md`). The `bne +8 / b target` CBRANCH
form in `h_CBRANCH` (`jit_compiler_a64.cpp:1217`) is already the right shape. **Do not touch.**

### 1.4 Allocations / pointer-chasing — clean

Per-hash steady state is allocation-free: scratchpad and JIT buffer are per-worker and
persistent, block template is copied into a reused per-worker `std::vector` only on job change
(`mining_engine.cpp:394`), nonce partitioning is lock-free (`local_nonce += num_threads_`). No
leaks or per-hash heap churn found. The one interpreter-only `const_cast` in `ISWAP_R`
(`vm.cpp:627`) is off the device path.

### 1.5 Data layout — already tuned

`RegisterFile` is `alignas(16)` (O1). Group-A/E/F registers map to NEON v16–v27 with a
hand-scheduled interleave in the main loop (static.S:222-286) that hides `ldp` load-use
latency behind `sshll`/`scvtf`. This is upstream-grade and matches tevador/SChernykh. No
struct-padding or AoS→SoA win is available here.

---

## 2. Low-Level & Algorithmic Optimization Strategies

### 2.1 What is genuinely still open

**[ACT] Huge-page backing for the Argon2 cache — make the fallback observable and forceable.**
Right now `argon2.cpp:270` cannot tell you whether it got real huge pages or fell back. Add:
1. a one-time log line reporting which path won (mirror the RWX/W^X disclosure pattern already
   in `JitCompilerA64`'s ctor), and
2. on fallback, an explicit `MADV_HUGEPAGE` **plus** a `MADV_POPULATE_WRITE` prefault (the
   scratchpad already does this at `vm.cpp:147`) so the kernel coalesces *before* mining
   starts rather than under steady-state pressure.

   *Before:*
   ```c
   void* ptr = allocLargePagesMemory(allocated_size_);
   if (!ptr) {
       ptr = ::mmap(nullptr, allocated_size_, PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
       ::madvise(ptr, allocated_size_, MADV_HUGEPAGE);   // hope THP coalesces later
   }
   ```
   *After (sketch):*
   ```c
   void* ptr = allocLargePagesMemory(allocated_size_);
   bool hugetlb = (ptr != nullptr);
   if (!ptr) {
       ptr = ::mmap(...MAP_PRIVATE|MAP_ANONYMOUS...);
       ::madvise(ptr, allocated_size_, MADV_HUGEPAGE);
   #if defined(MADV_POPULATE_WRITE)
       ::madvise(ptr, allocated_size_, MADV_POPULATE_WRITE); // fault+coalesce up-front
   #endif
   }
   log_once(hugetlb ? "Argon2 cache: hugetlb (256 MiB, 128 pages)"
                    : "Argon2 cache: THP fallback — check /proc/meminfo AnonHugePages");
   ```
   Expected upside **only if** the device is currently on 4 KiB pages: light-mode hashrate is
   directly throttled by cache-probe TLB walks, so moving 256 MiB from 65 536 → 128 mappings
   could be a double-digit % win. **This is the highest-EV lead in the whole plan.** If
   `/proc/meminfo` already shows the cache fully in `AnonHugePages`, it's a no-op — hence
   **[VERIFY] first** (§4).

**[ACT] Reserve a persistent hugetlb pool at boot (system config, not code).** If §2.1's
verification shows the hugetlb path failing, the fix may be purely operational:
`echo 160 > /proc/sys/vm/nr_hugepages` (128 for the cache + 8 for scratchpads + slack) or a
`hugepagesz=2M hugepages=160` kernel cmdline. Cheap, reversible, potentially the same
double-digit win as §2.1 without touching code. Document the exact value in the run guide.

### 2.2 What is closed — do not re-attempt (verified against current source)

| Idea | Result | Evidence |
|---|---|---|
| Branchless CBRANCH (CSEL) | +46% branch-miss, flat hashrate → reverted | `docs/branchless-cbranch.md` |
| Newton–Raphson FDIV/FSQRT | −1.1% (FPU pressure on in-order A53) | flag `ARMRX_ENABLE_JIT_FAST_DIV_SQRT`, default OFF |
| NEON vector-permute AES | −19.4% (per-block NEON load/store overhead) | `docs/neon-vector-permute-aes.md` |
| Hardware AESE/AESD | wrong round order per spec | `docs/aes-ttable-bug-postmortem.md` |
| Argon2 `memcpy` copy-elimination | cost relocated, +0.35% cycles → reverted | `docs/argon2-compress-copy-elimination.md` |
| PGO | identical 4.27 H/s on current code | `NEXT_STEPS.md` §5a |

The reason these all failed is the same and worth stating once: **the workload is
memory-latency-bound (IPC 0.708), so instruction-level cleverness has almost no headroom.**
Any future micro-opt proposal should be met with "does it reduce cache/DRAM stalls?" — if not,
predict a null result and demand a measurement before spending device time.

### 2.3 One real win already banked (for context)

Argon2 diagonal-step NEON vectorization: **−26.8% instructions / −19.0% cycles** for
`Argon2dCache::initialize()` (`docs/argon2-neon-diagonal-vectorization.md`). Note this is
**seed-rotation latency**, not steady-state hashrate — it makes the miner recover faster when
the pool pushes a new seed, invisible in H/s. Correctly scoped; nothing more to do there.

---

## 3. Concurrency, I/O & System Resource Optimization

### 3.1 The 8-worker scaling cliff — [VERIFY], best remaining throughput lever

8 workers deliver ~28–29 H/s vs. 8 × 5.2 = 41.6 ideal → **~68% scaling efficiency**. The docs
attribute this to single-channel LPDDR3 saturation, and `--stagger-ms` (a startup-only desync)
correctly did nothing (`docs/archived/beyond-parity_v2.md`). **But a worker-count sweep has
never actually been run.** In light mode the bottleneck mix is different (more compute per
dataset read, relatively less raw bandwidth), so the bandwidth-saturation point may not be at
8. Action:

**[ACT] Sweep aggregate H/s at N = 4,5,6,7,8 workers** (`armrx --mine --seconds=60 --workers=N`,
back-to-back, thermal-settled). Two possible honest outcomes, both useful:
- If aggregate is flat from ~6→8: run **6 workers**, bank the same hashrate at lower power/heat
  (an A53 phone throttles — sustained clock at 6 busy cores may even *exceed* 8-core throttled
  clock, a real net win). Make it the documented default for this device.
- If it still climbs to 8: confirms the ceiling, close the lead with a number instead of an
  assumption.

This is a few minutes of device time for a potentially real sustained-throughput gain. Highest
EV after the huge-page verification.

### 3.2 Thread affinity — already good, one refinement

`worker_loop()` pins by frequency-sorted core order (`detect_core_order()` via hwloc/sysfs).
On a homogeneous 8×A53 (no big.LITTLE) this is a no-op ordering, which is correct. **[VERIFY]**
whether the device is truly homogeneous; if `AffinityMode` is left `Unpinned`, pin to `All`
so the scheduler stops migrating hot scratchpad working sets between cores (cache-warmth loss
on migration is real and free to avoid).

### 3.3 Scheduler jitter — [ACT], low effort

For a dedicated miner:
- `--rt-priority` (SCHED_FIFO, already implemented at `mining_engine.cpp:300`) needs
  `CAP_SYS_NICE`; grant it (`setcap cap_sys_nice+ep ./armrx`) and measure p95 per-hash jitter.
- Kernel cmdline `isolcpus=` / `nohz_full=` on the mining cores removes timer-tick and
  scheduler interference. System-level, reversible, no code. Document in the run guide.

### 3.4 Locking / atomics — clean

Steady-state mining touches no locks: job pickup is a generation-counter compare
(`job_generation_`), stats use relaxed atomics flushed every 64 hashes
(`mining_engine.cpp:443`). The dataset-reinit handshake (`dataset_init_generation_` +
`dataset_init_cv_`) is off the hot path (fires only on seed rotation) and was already
race-audited last session. No contention lead here.

### 3.5 Per-run JIT recompile — [VERIFY], probably not worth it

`randomx_calculate_hash` calls `run()` 8× per hash, and each `run_jit()` re-emits the program
via `generateProgramLight()` + `__builtin___clear_cache` (1.76% of time). The 8 programs within
one hash are *different* (each chained on the previous), so they genuinely differ — no caching
possible. The `enableWriting`/`enableExecution` calls are no-ops under RWX (default). This is
near the floor already; only revisit if a profile ever shows compile share rising.

---

## 4. Benchmarking & Profiling Strategy

Use the existing devbox MCP bridge (`tools/devbox/`, `devbox_full` = sync→build→test→bench) or
direct SSH. Thermal variance on this device is **real** — always settle and run back-to-back.

### 4.1 Standard measurement protocol (house rule, don't deviate)
1. KATs green **before** any benchmark (`ctest --test-dir build`).
2. Rebuild the baseline fresh from the same tree (never compare against a stale binary).
3. `armrx --mine --seconds=60 --workers=N`, ≥2 runs, report both (thermal spread).
4. Trust **instruction count over wall-clock** for micro-changes (`perf stat`), wall-clock
   H/s for end-to-end.

### 4.2 The specific commands this plan needs

```sh
# Lead #1 — is the 256 MiB cache actually on huge pages? (run WHILE mining)
grep -E 'HugePages_Total|HugePages_Free|AnonHugePages|Hugetlb' /proc/meminfo
cat /proc/$(pgrep -n armrx)/smaps_rollup | grep -E 'AnonHugePages|Private_Hugetlb'
#   → AnonHugePages ~= 256 MiB  → THP already coalesced (lead is a no-op)
#   → AnonHugePages ~= 0        → running on 4 KiB pages → §2.1/§2.2 is a real win

# TLB-walk cost — the direct evidence for/against the huge-page lead
perf stat -e dTLB-load-misses,iTLB-load-misses,cache-misses,cycles,instructions \
    ./armrx --mine --seconds=30 --workers=1
#   high dTLB-load-misses/instructions ratio → confirms TLB-bound → huge pages will help

# Lead §3.1 — worker-count sweep
for n in 4 5 6 7 8; do ./armrx --mine --seconds=60 --workers=$n; done

# Where the light-mode cycles land (symbol attribution)
perf record -g ./armrx --mine --seconds=30 --workers=8 && perf report --stdio | head -40
#   expect rx_calc_dataset_item / superscalar-derived code dominant, confirming §0
```

### 4.3 Metrics to track
- **Primary:** aggregate H/s (throughput). This is a miner — percentile latency is not a KPI.
- **Diagnostic:** dTLB-load-miss rate (huge-page lead), IPC (should stay ~0.7; a rise means
  you unblocked a stall), cache-miss rate, per-worker H/s spread (affinity/thermal balance).
- **Regression guard:** the 5.2 H/s single-thread and ~28 H/s 8-worker figures in `README.md`
  are **stale/unverified** per the PGO finding — re-baseline them honestly before trusting any
  Δ against them.

---

## 5. Phased Optimization Roadmap

Ordered by **expected value = (probability it helps) × (size if it does) ÷ effort**. Given the
maturity, "effort" includes *the risk of reproducing a known negative*.

### Short-term — verification & config (hours, no/low code risk)

| # | Action | File / surface | Expected outcome | Why this priority |
|---|---|---|---|---|
| S1 | **[VERIFY] huge-page backing of the 256 MiB cache** (§4.2 grep + perf dTLB) | on-device only | Decides whether S2/S3 are real or no-ops | Highest EV; zero risk; gates everything below |
| S2 | **[ACT] Disclose + prefault the cache huge-page path** | `argon2.cpp:270` | Observability + up-front THP coalesce; potential double-digit % if S1 shows 4 KiB pages | Cheap code, mirrors existing RWX-disclosure pattern |
| S3 | **[ACT] Reserve hugetlb pool** `vm.nr_hugepages` / kernel cmdline | run guide / boot config | Same win as S2 via the guaranteed (hugetlb) path | No code; reversible |
| S4 | **[ACT] Worker-count sweep 4→8** | `--workers=N` | Either 6-worker parity (power/thermal win) or a real ceiling number | Minutes of device time; best throughput lever left |
| S5 | **[ACT] `--rt-priority` + isolcpus/nohz_full** on dedicated miner | run guide, `setcap` | Lower per-hash jitter, steadier sustained H/s | Low effort, system-level |

### Medium-term — structural, only if S1 says TLB-bound (days)

| # | Action | File / surface | Expected outcome | Rationale |
|---|---|---|---|---|
| M1 | If THP proves unreliable, allocate the cache from an explicit **hugetlbfs** mmap with a hard-fail-to-log path (not silent fallback) | `argon2.cpp`, `virtual_memory.c` | Deterministic 128-page mapping for the hottest working set | Removes the §1.2 worst case entirely |
| M2 | Prefetch tuning **for the light-mode cache probe** specifically — the `prfm pldl1keep` at `static.S:905` (`rx_calc_dataset_item_prefetch`) targets a *random* 256 MiB line; test `pldl2keep`/`pstl1strm` variants | `jit_compiler_a64_static.S` | Possibly a few % if L1 is being thrashed by cold random lines | Distinct from the *scratchpad* prefetch tuning already done (O10); this one is on the light-mode path that actually runs here |

### Long-term — high effort, uncertain, do not start without a measured hypothesis

| # | Action | File / surface | Expected outcome | Rationale / caveat |
|---|---|---|---|---|
| L1 | **Peephole JIT coalescing** (`docs/peephole-jit-plan.md`, ROADMAP "P3") | `jit_compiler_a64.cpp`, `static.S` | ~+5–10% *claimed* | **Re-scope before starting.** Estimate was framed against the debunked 31% branch-miss baseline; with IPC 0.708 the workload is stall-bound, so instruction coalescing likely underperforms the estimate. **Note (2026-07-24): the plan's original XMRig-disassembly methodology is now permanently out of scope (clean-room boundary, `PLAN.md` Phase 6 item 14) — any revival must use self-directed analysis of armrx's own generated code only.** **Lowest EV item here — S1–S4 must be exhausted first.** |
| L2 | Bigger-RAM device evaluation (fast mode) | hardware | Unlocks the pure-bandwidth fast path; different optimization regime entirely | Not an armrx change — but note the *entire* light-mode compute bottleneck (§0) vanishes with ≥3 GiB RAM. If the deployment target can be a 4 GiB SBC, that dwarfs every software lead in this document. Worth raising as a deployment decision. |

---

## 6. TL;DR for the next agent

1. **The device runs light mode** (2 GiB can't hold the dataset). The hot path is
   superscalar-hash recompute + **random 256 MiB Argon2-cache probes**, not fast-mode
   bandwidth. Optimize *that*, ignore fast-mode ideas.
2. **First action, before any code:** verify the 256 MiB cache is actually on huge pages
   (§4.2). It's the highest-EV unknown and currently unmeasured. If it's on 4 KiB pages, S2/S3
   are a likely double-digit win; if not, they're no-ops — either way you learn something real.
3. **Second action:** sweep worker count 4→8 (§3.1) — never actually done; may bank free
   power/thermal headroom at equal hashrate.
4. **Everything at the instruction level is closed** and closed *correctly* (IPC 0.708 = the
   machine is stalling on memory, not starving for ALU). Re-opening CSEL/PGO/NEON-AES/Newton-
   Raphson will reproduce a documented null. The peephole-JIT idea (L1) is the only surviving
   code lead and its estimate is stale — re-scope with a measurement first.
5. **Re-baseline `README.md`'s 5.2 / 28 H/s honestly** before trusting any delta; the PGO
   finding showed those numbers are stale.
