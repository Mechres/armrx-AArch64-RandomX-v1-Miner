# armrx — Performance & Optimization Master Plan (2026-07-24)

**Baseline for this plan:** HEAD `88f4122`, target = 8× Cortex-A53 (in-order, dual-issue,
Snapdragon 410-class), ~2 GiB single-channel LPDDR3, Alpine/musl, GCC 15.
**Ground truth (measured, benchmark v2, `docs/performance-next-agent-handoff.md` §22):**

| Fact | Value | Source |
|---|---|---|
| Single-thread H/s (current, honest) | **4.27** (not 5.18 — that figure is stale) | PGO re-measurement 2026-07-23 |
| 8-worker pool H/s | ~22–25 | NEXT_STEPS.md telemetry |
| Time in `run()` (JIT execute) | **99.1% of hash**, 98.24% of that is generated-code execution | §22.1–22.2 |
| IPC | **0.708** (~35% of dual-issue peak) | §22.3 |
| Hot-path branch-miss rate | **2.4%** (~0.1–0.16% of cycles) — NOT 31–34% | branchless-cbranch.md |
| JIT speedup over interpreted | 12.85× | §22.3 |

**Closed leads — DO NOT re-propose without a new reason** (all measured honestly on-device):
CSEL/branchless CBRANCH (+46% misses, flat H/s — reverted); NEON vector-permute AES
(−19.4% — flag-gated OFF); hardware AESE/AESD (incompatible + slow); PGO (+0.0% on
current code, tool kept); `--stagger-ms` (+0%, hardware ceiling); Argon2 copy-elimination
(flat-to-worse); Argon2 backlog generally (closed after `perf annotate`); Newton-Raphson
FDIV/FSQRT (−1.1%, frozen).

Everything below is scoped to what is genuinely still open.

---

## 1. Hotspot & Bottleneck Analysis

### 1.1 Where the cycles actually are
```
one hash (light mode, JIT)
├─ blake2b input→seed            0.00%
├─ init_scratchpad (AES 2 MiB)   0.31%
├─ 8× program run()             ~99.0%   ← everything lives here
│   ├─ JIT compile (8 progs)     1.76%
│   └─ JIT execute               98.24%  ← generated VM code + inlined
│                                          light-mode dataset derivation
└─ final AES hash + blake2b      0.53%
```
Any optimization not touching the generated-code execution path is fighting over <1%
of the budget. C++-side "cleanup" (allocations, std::string, iostream logging, mutexes
in `MiningEngine`) is performance-irrelevant: it executes once per job/second, not per
hash iteration (2,048 iterations × 8 programs per hash).

### 1.2 The real bottleneck: in-order pipeline stalls, not branches, not I-count
- IPC 0.708 vs ~2.0 dual-issue peak. Branch misses explain only ~0.1–0.16% of cycles
  (measured). The remaining stall budget is **load-use latency, SIMD widening/convert
  chains, and dependent-instruction back-to-back issue** — classic A53 in-order hazards.
- Concrete instances already identified in code:
  - `src/jit_compiler_a64_static.S:236-263` — eight serialized
    `ldr d → sshll → scvtf` chains; each consumes its load result on the next
    instruction (3-cycle load-use + SIMD pipe latency exposed 8×/iteration ×
    16,384 iterations/hash).
  - `JitCompilerA64::emitMemLoadFP()` (`src/jit_compiler_a64.cpp:655`) — emits
    `add x19, x2, x19; ld1 {vN.2s},[x19]` where a single register-offset
    `ldr dN, [x2, x19]` exists. One extra dependent ALU op in *every* FP memory
    opcode (FADD_M/FSUB_M/FDIV_M), on the critical address path.
  - Superscalar literal pools emitted **inline between code**, jumped over with an
    always-taken `b` per program (`generateSuperscalarHash()`,
    `src/jit_compiler_a64.cpp:406-505`) — pollutes sequential I-fetch and burns one
    branch per program per dataset-item derivation, which in light mode runs inside
    the 98.24% hot region.

### 1.3 Memory system
- Light mode: per-iteration dataset-item derivation = Superscalar programs over the
  256 MiB cache → dominated by L2/DRAM latency; 2 MiB scratchpad thrashes the shared
  512 KiB L2 with ≥2 workers.
- 8-worker scaling loss (~30%) is *presumed* single-channel DRAM saturation but never
  proven with L2-refill/stall/TLB counters (handoff §4.3 explicitly flags this).
- Huge-page residency is **asserted, not verified** — `MAP_HUGETLB`/`MADV_HUGEPAGE`
  success does not prove backing (§16). On A53 a 2 MiB scratchpad on 4 KiB pages is
  512 TLB entries vs a ~512-entry unified L2 TLB: if THP is silently not backing the
  scratchpad, we're paying page walks in the hot loop.
- Prefetch hints (`prfm` at `jit_compiler_a64_static.S:381-383`, three per iteration)
  were never A/B-tested; on A53 they consume issue slots and may hurt 8-worker
  aggregate while helping 1-thread.

### 1.4 Data layout / allocation — audited, largely clean
- Scratchpad and dataset use dedicated mapped memory (`src/memory.cpp`), 64-byte-aligned
  items; no per-hash heap allocation on the hot path (verified: `worker_loop` reuses
  `block_input`, VM, scratchpad across iterations).
- No AoS/SoA problem: RandomX register file layout is dictated by spec and mapped to
  fixed native registers (handoff §14.5 — there is no allocator producing spills).
- One residual micro-nit: `VirtualMachine::hash_and_fill()` exists (`src/vm.cpp:892`)
  but production mining doesn't use it — mining does hash-then-fill as two full 2 MiB
  scratchpad traversals where one fused traversal would halve that memory sweep
  (worth ~0.8% total; see §2.3).

---

## 2. Low-Level & Algorithmic Optimization Strategies

Algorithmic headroom is ~zero by design: RandomX outputs must be bit-identical, the
work is spec-mandated, and Argon2/AES/Blake2b are closed. All remaining leverage is
micro-architectural, inside the JIT emitter and static template.

### 2.1 Static FP load/convert scheduling (open, low risk, est. 1–4%)
Target: `src/jit_compiler_a64_static.S:236-263`.
Before (serialized, per chain: load-use stall + convert stall):
```asm
ldr   d16, [x17]        ; sshll v16.2d, v16.2s, #0 ; scvtf v16.2d, v16.2d
ldr   d17, [x17, #8]    ; sshll v17.2d, ...        ; scvtf ...
```
After (software-pipelined groups of 4 — hide load latency under independent work):
```asm
ldr d16,[x17] ; ldr d17,[x17,#8] ; ldr d18,[x17,#16] ; ldr d19,[x17,#24]
sshll v16.2d,v16.2s,#0 ; sshll v17.2d,... ; sshll v18.2d,... ; sshll v19.2d,...
scvtf v16.2d,v16.2d    ; scvtf v17.2d,... ; ...
```
Run the 2/4/8-group matrix from handoff §6.2; also try `ldp dN,dM` for the adjacent
fixed offsets (safe here — static layout, unlike VM opcodes, handoff §14.6).

### 2.2 Register-offset FP loads in the emitter (open, low risk, est. 0.5–2%)
Target: `JitCompilerA64::emitMemLoadFP()` `src/jit_compiler_a64.cpp:655` and its three
call sites (lines 1038/1061/1091 — FADD_M/FSUB_M/FDIV_M).
Replace `add x19,x2,x19` + `ld1 {vN.2s},[x19]` with `ldr dN,[x2,x19]`
(LDR SIMD&FP register-offset encoding, size=10, opc=01, option=011 LSL #0).
Add an encoding-decode unit test in `tests/test_jit_encodings.cpp` before enabling.

### 2.3 Fused hash-and-fill in production mining (open, medium risk, est. 1–5%)
`hash_and_fill_aes_1r_x4` is implemented, KAT-pinned (`tests/test_aes_hash.cpp`
proves decomposition equivalence), and **unused by mining**. Pipeline nonces:
first/next/last as designed in handoff §9.2, converting two 2 MiB scratchpad sweeps
into one per nonce. Hazards are all bookkeeping (one-result latency, job-change flush,
nonce/share association) — implement behind a benchmark first, integrate only on a
measured win, with a long deterministic-nonce equivalence test vs
`randomx_calculate_hash()`.

### 2.4 Superscalar literal-pool relayout (open, medium risk, est. 1–4%)
Target: `generateSuperscalarHash()` `src/jit_compiler_a64.cpp:406-505`. Move all
reciprocal literals to a single aligned pool *after* all eight programs' code; record
pending `LDR literal` sites and patch imm19 afterwards. Removes one always-taken
branch per program per dataset-item derivation and keeps literals out of I-fetch.
This runs inside the light-mode hot region, so unlike the dataset-helper-ABI idea
(compile-path only, deprioritized per §22.4) it touches real cycles. Guard with
imm19 range asserts + byte-level determinism test (handoff §18).

### 2.5 Conservative generated-VM scheduling (open, HIGH risk, est. 2–6%)
The big one behind the IPC-0.708 number: a 2–3-instruction emitter lookahead that
reorders provably independent emitted ops (disjoint integer regs, independent mul
chains, address generation hoisting), with hard barriers at CBRANCH/CFROUND and
all scratchpad memory ops treated as aliasing (handoff §13). Do this only after
2.1/2.2/2.4 land, because they change the instruction stream it would schedule.
Prototype ≠ DAG scheduler; preserve `reg_changed_offset` semantics exactly.

### 2.6 Explicitly NOT recommended
- Branchless/CSEL CBRANCH — measured regression, closed.
- Any NEON/hardware AES path — measured regressions ×3, A53 NEON ld/st overhead is
  the structural cause, not the technique.
- Peephole vs XMRig disassembly (3–6 wk) — the 33% I-count "gap" was never shown to
  live in the generated region; re-run region-scoped instruction attribution first
  and only open this if generated-code I-count per hash is materially above XMRig's
  *for the same region* (validation gate, handoff §4.4).
- Custom allocators / memory pooling — no hot-path allocation exists to pool.

---

## 3. Concurrency, I/O & System Resource Optimization

### 3.1 Current state (audited — mostly correct already)
- Workers are persistent, affinity-pinned (freq-sorted via hwloc/sysfs,
  `mining_engine.cpp:26-109`), nonce-space partitioned (`thread_id + k*N`, no shared
  nonce counter → no atomic contention on the hot path). Per-worker hash counters are
  relaxed atomics, read-side aggregated. `job_generation_`/`dataset_init_generation_`
  counters avoid flag races. This is the right shape; don't refactor it.
- `set_job()` takes `job_mutex_` but runs once per pool job (seconds apart) — not a
  contention source. Stratum/TLS I/O is on its own threads; syscall overhead is noise
  at 4–25 H/s share rates.

### 3.2 Open items, in order of expected value
1. **Prove or disprove the DRAM-ceiling hypothesis** (diagnostic, gates everything
   multi-worker): 1/2/4/6/8-worker `perf stat` with L1D/L2 refills, backend stalls,
   dTLB refills/walks, plus per-second cpufreq + thermal-zone logging (handoff §4.3,
   §12). If scaling loss is thermal/frequency rather than DRAM, the fix is worker
   count/placement, not memory.
2. **Huge-page residency verification** (`/proc/<pid>/smaps` AnonHugePages for the
   scratchpad and cache mappings during mining, §16). If not resident: pre-reserved
   hugetlbfs pages for the 2 MiB scratchpads specifically — 1 TLB entry vs 512 each,
   direct hot-loop payoff, near-zero code change (`src/memory.cpp` already has the
   MAP_HUGETLB path — verify it actually wins the fallback race).
3. **Prefetch A/B matrix** (`jit_compiler_a64_static.S:381-383`): none / spAddr0-only /
   current-three / L1STRM-vs-KEEP, judged on 8-worker aggregate, not 1-thread.
4. **Worker/thermal sweep** (handoff §12): 5/6/7/8 workers, one core reserved for
   housekeeping, 15–30 min steady-state each. On a passively cooled A53, 7 workers
   at sustained frequency can beat 8 throttled ones — this is potentially a free
   0–10% and costs zero code.
5. Leave `SCHED_FIFO` (`--rt-priority`) default-off; continuously runnable FIFO
   miners can starve thermal management and reduce sustained H/s.

---

## 4. Benchmarking & Profiling Strategy

Protocol v2 exists and is deployed; keep using it. Non-negotiables (learned the hard
way, five times, this codebase — see HANDOFF_CLAUDE.md §5):

1. **Correctness gate first, always:** full KATs + `ctest` 12/12 on-device before any
   timing. RandomX has zero numerical tolerance.
2. **Apples-to-apples:** old code rebuilt fresh, back-to-back runs, same core mask,
   governor, huge-page state, starting temperature. Thermal variance on this device
   is real — alternate/randomize variant order, run each ≥3×, report median + spread.
3. **Metrics:** cycles/hash and instructions/hash (PMU, normalized by completed
   hashes), sustained H/s (steady-state window, first minute excluded), 1-thread AND
   8-worker. H/s on real hardware is the veto metric; instruction count is only a proxy
   (copy-elimination: −2.86% instructions, +0.35% cycles → reverted).
4. **Region attribution before opcode work:** `ARMRX_JIT_PROFILE` phases + `perf record`
   / `perf annotate` with debug symbols (this is what killed the false 31% branch-miss
   narrative). For flamegraphs on-device: `perf record -g --call-graph fp` (frame
   pointers; DWARF unwinding is too heavy on A53) → `perf script | stackcollapse-perf |
   flamegraph.pl` off-device.
5. **Tooling:** `devbox_perf_stat` / `devbox_bench` / `devbox_pgo_build` (all fixed and
   tracked as of this session). `bench_armrx --full-hash-only` = representative hot
   path; `--attribution-only` is NOT (it's the section that produced the misleading
   31% figure — never aggregate it into hot-path claims).
6. **Encoding changes** additionally require decode-level unit tests
   (`tests/test_jit_encodings.cpp`) and byte-level JIT determinism checks before any
   benchmark is trusted.
7. **Document negative results** in `docs/` — this repo's closed-leads list is its
   most valuable perf asset; keep feeding it.

Latency percentiles (p95/p99) are meaningful only for share round-trip / pool I/O,
which is not a bottleneck; for mining, median + σ of μs/hash is the right statistic.

---

## 5. Phased Optimization Roadmap

### Short-term (days; low risk; measurement + free wins)
| # | Item | Target | Expected outcome | Why now |
|---|---|---|---|---|
| S1 | Multi-worker PMU attribution (L2 refills, stalls, dTLB, freq/thermal log, 1–8 workers) | devbox scripts, `devbox_perf_stat` | Proves whether 8-worker loss is DRAM vs thermal vs TLB | Gates S3, M3; pure diagnostic, zero risk |
| S2 | Huge-page residency check; fix scratchpad backing if absent | `src/memory.cpp`, `/proc/smaps` | If THP absent: large TLB win in the 98% region for ~0 code | Cheapest possible hot-loop lever |
| S3 | Worker-count/thermal steady-state sweep (5–8 workers, core-reserve variant) | run config only, no code | 0–10% sustained 8-worker H/s | Zero code risk, possibly the biggest single sustained gain |
| S4 | Register-offset FP load (`ldr dN,[x2,x19]`) | `jit_compiler_a64.cpp:655` + encoding test | −1 dependent ALU op per FP mem opcode; est. 0.5–2% | Smallest emitter change with a clear mechanism |

### Medium-term (1–2 weeks; moderate risk; pipeline & layout)
| # | Item | Target | Expected outcome | Why this order |
|---|---|---|---|---|
| M1 | Static FP load/convert schedule matrix (groups of 2/4/8, `ldp` variants) | `jit_compiler_a64_static.S:236-263` | Hide load-use + convert latency; est. 1–4% | In-order A53's textbook weakness; static code, fully testable |
| M2 | Superscalar literal-pool relayout (pool-after-code, imm19 patching) | `jit_compiler_a64.cpp:406-505` | Remove 8 taken branches + I-fetch pollution per dataset item; est. 1–4% | Lives inside light-mode hot region, bounded blast radius with byte-determinism tests |
| M3 | Prefetch A/B matrix (contingent on S1 results) | `jit_compiler_a64_static.S:381-383` | −2% to +3%; pick per 8-worker aggregate | Needs S1's memory data to interpret |
| M4 | Fused hash-and-fill nonce pipeline (bench first, then mining integration) | `vm.cpp:892`, `mining_engine.cpp::worker_loop`, new tests | One 2 MiB sweep instead of two per nonce; est. 1–5% | Code exists & is KAT-pinned; risk is job-lifecycle bookkeeping, hence after the pure-emitter items |

### Long-term (weeks; high risk; only if medium-term stalls out)
| # | Item | Target | Expected outcome | Gate |
|---|---|---|---|---|
| L1 | Conservative 2–3-insn emitter lookahead scheduler (barriers at CBRANCH/CFROUND, mem ops alias) | `jit_compiler_a64.cpp` handlers/`generateProgram*` | Attack the IPC-0.708 stall budget directly; est. 2–6% | Only after M1/M2/S4 (they change the stream); byte-determinism + JIT/interp differential tests mandatory |
| L2 | Region-scoped armrx-vs-XMRig generated-code comparison → peephole pass only if a real gap shows | `--jit-dump` + objdump workflow (`docs/peephole-jit-plan.md` Phase 1) | Either closes the "33% gap" myth for good, or yields a data-ranked peephole list | Do NOT start the 3–6-week peephole effort on the old aggregate numbers |
| L3 | PGO re-measurement after any M/L item lands meaningfully | `devbox_pgo_build` (one command) | PGO nulled out on today's code; a reshaped binary may reopen it | Free to re-check; never re-build tooling |
| L4 | Clang vs GCC + `-mcpu=cortex-a53` flag matrix on the C++ side (AES init, Blake2b, superscalar C++ only) | `CMakeLists.txt` | Bounded: affects the <2% non-generated share + compile path | Cheap but capped upside; batch with L3 |

### Priority rationale in one line
Everything is ranked by (share of the 98%-hot generated-execution region touched) ×
(mechanistic confidence) ÷ (blast radius), with the standing rule that
**measured hashrate on the device vetoes every estimate in this document.**
