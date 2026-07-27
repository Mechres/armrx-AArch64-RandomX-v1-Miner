# armrx — Hail Mary: Experimental Performance Ideas (2026-07-27)

*** Written by:*** Deepseek V4

**Status: brainstorm only.** Not gated, not prioritized, not promised. The gated plan
(`docs/plans/performance-plan-20260725.md`) is closed — every recoverable lead within normal
code-level optimization was chased and hit either a null or regression wall. The scratchpad-
locality experiment (`docs/experiments/scratchpad-locality-bound-20260726.md`) proved the main
VM program's 2.2× IPC penalty is ~94% architectural, not fixable by code changes. The only real
win in this project's history was operational (isolcpus: +14%). This document collects ideas
that are **experimental, high-risk, high-effort, or outright crazy** — things not worth
considering while conventional leads were open, but maybe worth a look when the alternative is
being stuck.

**Ground truth reminder (per the evidence-backed findings of 2026-07-25/26):**
- Superscalar/dataset-derivation region: **72.71% of instructions, 1.145× IPC** — already efficient.
  Instruction-count reduction here has regressed every time (IMUL_RCP literal-load elimination,
  superscalar literal-pool relayout, CSEL/CBRANCH).
- Main VM program region: **9.23% of instructions, 20.04% of cycles, 0.461× IPC** — the problem
  child. 94% of this penalty is architectural (dependency chains / in-order pipeline depth), not
  memory-latency. The 6% that IS latency is too small to chase with PRFM or any safe scheduler change.
- C++ overhead: **15.56% of instructions, 1.107× IPC** — proportionally efficient.
- Net result: armrx at ~90% of XMRig per-cluster. No conventional code-level path to 100%.

So any Hail Mary idea must either: (a) attack the architectural part of the penalty (dependency
chains, pipeline depth), (b) change the execution strategy entirely (not just tweaking the JIT),
(c) exploit the two-cluster topology in a way conventional mining doesn't, or (d) accept risk
that normal project standards would reject.

---

## Category A: Architectural / pipeline-level

### A1. Hybrid JIT/interpreter: run the main VM program in the interpreter

**The idea:** The JIT compiles everything — superscalar path AND main VM program. The superscalar
path (72.71% of instructions, 1.145× IPC) benefits hugely from JIT (no dispatch overhead, native
register allocation). The main VM program (9.23% of instructions, 0.461× IPC) does NOT benefit —
its IPC is terrible in JIT because the tight in-order pipeline couples consecutive dependent
instructions with no decode/dispatch bubbles between them.

The interpreter's dispatch loop (`run_interpreted()` at `src/vm.cpp:848`) has a per-opcode
switch with a `compile_instruction()` prefix that expands each bytecode opcode into a dense
`InstructionByteCode` struct (arithmetic + address calculation + mask + load/store in one
compiled-into-memory-block step). Each VM instruction boundary in the interpreted path is a
full function call / switch dispatch, giving the pipeline a natural **bubble** between
dependent scratchpad operations — the CPU can drain its load-to-use and multiply pipelines
between iterations in a way that the JIT's tight sequence doesn't force it to.

**What changes:**
- `vm.run()`: skip JIT compilation for the main program path only; still JIT-compile and execute
  the superscalar path (that already happens inside `generateProgramLight` / `generateProgram`
  or at dataset-derivation time, depending on mode).
- Or more surgically: every `*_M` / `ISTORE` instruction gets interpreted while everything else
  stays JIT-compiled. The JIT already has a per-opcode handler table (`engine[256]`)
  — redirecting specific opcodes to an interpreter call is ~10 lines.

**Expected effect:** The main VM program's IPC improves (fewer cycles for its 9.23% instruction
share). But its instruction share grows (interpreter overhead adds dispatch instructions).
Net effect on total cycles is hard to predict without measuring.

**Why it might be zero or negative:** The interpreter's `compile_instruction()` already expands
each opcode into a `InstructionByteCode` struct — this is a memory allocation + code-generation
step that itself costs cycles. The JIT compiles once and executes 2048 times; the interpreter
compiles per-hash. For the main VM program (compiled once per hash), the interpreter's re-
compilation cost per hash might eat any IPC gains.

**Effort to measure:** ~2-3 days. Prototype: add a `force_interpreted_main` flag to `VirtualMachine`,
short-circuit `run_jit()` to call `run_interpreted()` for the main program path (keep JIT for
superscalar). Run `bench_armrx` full-hash throughput comparison.

**Risk:** Low-medium. The interpreter is already tested (KATs, 7/7 green on x86, 5/5 on-device).
The hybrid path is novel but both halves are individually verified.

---

### A2. Per-memory-op register renaming / address precomputation

**The idea:** In the JIT, scratchpad addresses for `*_M` and `ISTORE` are computed from register
values that are the result of the previous few ALU instructions. These dependencies create a
chain: `IMUL_R r0 → IADD_RS r0 → ISUB_M (reads from scratchpad at address derived from r0)`.
The JIT emits these sequentially because the address register isn't available until the ALU
chain resolves.

But on Cortex-A53's in-order pipeline, the address-generation unit is separate from the ALU.
What if we explicitly **unroll** the address dependency by computing the scratchpad address
from the last value of r0 that was LIVE at the start of the ALU chain? This would let the
load-store unit begin the scratchpad read while the ALU is still computing r0's final value,
hiding both the ALU latency AND the scratchpad latency simultaneously.

**Why it's crazy:** It changes the program's semantics — you'd be reading from a potentially
stale scratchpad address. Unless you can prove that the address register's intermediate values
at specific program points produce the same scratchpad address as the final value (e.g., if
the ALU chain only modifies bits that are outside the address mask range).

**The crack in the wall:** Scratchpad addresses are masked to L1 (16 KiB), L2 (256 KiB), or
L3 (2 MiB) ranges. If the ALU chain only modifies bits above the mask boundary, the scratchpad
address is unchanged. This is a genuinely architectural property — on Cortex-A53, the `UBFX`
instruction that extracts the address bits from the register value might select bits that a
given ALU instruction doesn't touch.

**Expected effect:** Zero to negative, almost certainly. The analysis to prove address
equivalence across arbitrary ALU chains is speculative-compiler-grade hard, and even if it
works for some fraction of instructions, the gain is bounded by the ~6% latency-recoverable
ceiling.

**Risk:** High. Wrong hashes if the address equivalence proof is wrong. This is the same
hazard class as the reverted memory-op scheduler — silent wrong hashes from a subtle
memory-ordering violation.

---

### A3. Superscalar dataset-derivation interleaving with main program execution

**The idea:** In light mode, `generateProgramLight()` inlines the dataset-item derivation code
into the main JIT program — each dataset item fetch (8 cache reads per item, 2048 items per
hash) is emitted as inline code interleaved with the main VM program's instructions. This is
already how the JIT works.

What if we **separate** them instead? Compile the superscalar program to a separate code buffer,
execute it *while* the main VM program is running, on the same core, using the Cortex-A53's
dual-issue capability? The A53 can issue one integer ALU + one load/store per cycle. If the
main VM program is ALU-heavy and the superscalar derivation is load-heavy, they could in
principle overlap.

**Why it's crazy:** They use different register files (VM registers vs superscalar registers),
different memory regions (scratchpad vs Argon2 cache), and different program counters. You'd
need an interleaved scheduler that emits code for both programs in a ratio that matches their
pipeline-issue profiles.

**Expected effect:** Unknown. No existing RandomX implementation does this.

**Effort:** Weeks to months. Requires restructuring the JIT's code buffer management from
"one program at a time" to "two interleaved instruction streams."

**Risk:** Very high. Correctness is non-trivial (register file isolation, memory isolation,
interrupt handling across two logical programs on one core).

---

## Category B: Structural / topology exploitation

### B1. Per-cluster cache replication in light mode

**The idea:** On the MSM8929, cores 4-7 lose ~50% throughput under full contention from
interconnect arbitration. The per-hash hot path in light mode does ~16,384 random 64-byte
reads from the shared 256 MiB Argon2 cache. Every cache read from cores 4-7 traverses the
interconnect, competing with reads from cores 0-3.

Give cores 4-7 their **own physical copy** of the 256 MiB cache. 2 × 256 MiB = 512 MiB
total. The device has ~1.3 GiB MemAvailable — that fits comfortably. Each cluster reads
from its local copy with zero interconnect contention.

**Implementation:**
- `Argon2dCache` allocates two 256 MiB buffers instead of one.
- On seed rotation, initialize both buffers (same data, double the rotation time — or
  initialize one and `memcpy` to the second in parallel using both clusters).
- Each worker selects a cache pointer based on which cluster its core belongs to.
- The JIT's `mem_regs.memory` pointer (set in `vm.cpp:808`) gets the cluster-local copy.

**Expected effect:** On the slow cluster (cores 4-7), each cache read no longer arbitrates
with the fast cluster's reads. The 50% throughput loss on cores 4-7 under contention is
caused at least partially by interconnect stalls on cache reads. If even half of that 50%
penalty is interconnect arbitration for cache reads, the slow cluster goes from 2.84 H/s
to ~3.55 H/s per worker (25% improvement on the slow cluster, ~12.5% aggregate).

**Why it might be zero:** If the 50% penalty is entirely from the L2-to-L3 interconnect
(memory-side, not cache-side), a local cache copy doesn't help — the bottleneck is the
memory controller, which is shared regardless.

**Risk:** Medium. Extra memory allocation, seed rotation takes ~2× (or 1× with parallel
init + memcpy). No correctness risk — cache data is identical.

**Effort:** ~1-2 days to prototype. Add a `cluster_local_cache_` vector to `Argon2dCache`,
a cluster-affinity-aware cache selector in `VirtualMachine::run_jit()`, and wire the
allocation into `MiningEngine`'s memory setup.

---

### B2. Adaptive worker-to-cluster ratio

**The idea:** Currently workers are allocated round-robin across all cores. The fast cluster
(cores 0-3) delivers ~4.26 H/s per core; the slow cluster (cores 4-7) delivers ~2.84 H/s per
core with isolcpus, or ~1.42-2.84 H/s without. The ratio is ~1.5:1 to 3:1 in favor of the
fast cluster.

What if we allocate workers **asymmetrically** — e.g., 6 workers on the fast cluster (cores 0-3
with SMT... no SMT on A53). Dead end without SMT.

Actually, the alternative: give the fast cluster *more work per worker* by widening its nonce
partition, or give the slow cluster *fewer workers* to reduce contention. With 4 fast + 4 slow,
both clusters are saturated. With 4 fast + 2 slow (6 workers total), the fast cluster is still
saturated and the slow cluster's workers each get more interconnect bandwidth.

The worker-count sweep in `NEXT_STEPS.md` already says 8 workers is the highest-throughput
choice. So reducing workers reduces throughput. But the sweep was done at a fixed isohashrate
configuration — what if the nonce distribution per worker affects anything? No, nonces are
independent.

**Verdict:** Already measured and closed. 8 workers wins.

---

### B3. Fast-cluster-only mining with frequency boost

**The idea:** Use only 4 workers on cores 0-3. With cores 4-7 idle (or offlined via
`isolcpus=4-7`), the SoC's power budget might allow cores 0-3 to sustain a higher clock
frequency. Currently the 8-worker average per-core frequency drops to ~567 MHz as the slow
cluster drags the average down. With only 4 cores active, the peak frequency might be higher.

But frequency is per-cluster on Cortex-A53, not per-core — both clusters can run at different
frequencies. The fast cluster runs at ~998 MHz regardless of whether cores 4-7 are loaded.

**Verdict:** Already known — the fast cluster holds its frequency regardless of slow-cluster
load. No thermal benefit from reducing worker count on the slow cluster.

---

## Category C: Toolchain / compiler / ABI

### C1. `-moutline-atomics` + LSE-atomics-aware build

**The idea:** Cortex-A53 doesn't have ARMv8.1 LSE atomics. But... does it? Let me check. The
device's flags show `fp asimd evtstrm aes pmull sha1 sha2 crc32 cpuid`. No `lse`. Dead end.

---

### C2. Custom linker script for JIT code placement

**The idea:** The JIT code buffer is allocated via `mmap` at a kernel-chosen address. On
Cortex-A53, the branch range for `B`/`BL` instructions is ±128 MiB. If the JIT buffer is
more than 128 MiB away from the main executable's text section, cross-section branches
(superscalar → main code, or JIT stubs) need PLT indirection or register-relative jumps.

Use a custom linker script to place the JIT code buffer at a fixed offset from the main
text section, ensuring all relative branches are within range.

**Expected effect:** Probably zero. The existing JIT already uses register-indirect calls
for dataset-init and doesn't branch back to C++ code from JIT (the JIT calls into nothing;
it's a standalone function). The `BL` instructions inside the JIT buffer are within-buffer
and already in range.

**Verdict:** Already analyzed as part of the JIT ABI review. No gain.

---

### C3. Profile-guided layout of the JIT code buffer

**The idea:** The JIT emits code linearly: prologue → main program → superscalar code →
literal pool. The main program (executed once per hash) and superscalar code (executed ~16K
times per hash) are in the same buffer. On Cortex-A53's 16 KiB L1 I-cache, the superscalar
code might evict the main program's code between uses.

Reorder the buffer so the hot superscalar paths are contiguous and I-cache-line-aligned,
while the cold main program code is placed after. This is what `jit_correlate.py` already
has the data to guide (it knows which offsets are hot vs cold).

**Expected effect:** Small. I-cache misses on the Cortex-A53 are relatively cheap (1-2 cycle
penalty for L1 I-cache). Even a 1% I-cache miss rate on the superscalar path is ~0.7% of
total cycles.

**Effort:** ~1 day to prototype: reorder `generateSuperscalarHash`'s emission order, add
alignment directives before the hot entry points.

**Risk:** Low — no correctness change, just layout.

---

## Category D: Real "out there" ideas

### D1. Two-process cooperative mining: fast-mode across two machines

**The idea:** The 2 GiB dataset doesn't fit on this device. But what if the dataset is hosted
on a second machine (or a fast SD card / USB drive) and accessed over a memory-mapped file
or RDMA? RandomX's random access pattern makes this hopeless — every 64-byte read has a
random address, so network latency per read would be catastrophic.

---

### D2. Partial fast mode: dataset subset + light-mode fallback

**The idea:** Allocate whatever RAM is available (say 1 GiB after OS reservation) to hold
part of the dataset. Accesses that hit the pre-computed portion are fast; accesses that miss
fall back to light-mode on-the-fly derivation. The dataset is 2 GiB + 32 MiB in total, so
1 GiB holds ~49% of it. Each of the 2048 items per hash has a ~49% chance of being a fast
access.

This doesn't reduce the per-hash work — it just replaces some on-the-fly derivations with
pre-computed reads. The derivation (superscalar program) is instruction-heavy (72.71% of
instructions), so replacing even half of those with a cache read might save significant
instruction count.

**Implementation:**
- `Dataset::init()` allocates a 1 GiB buffer, initializes items 0..(1 GiB / 64 - 1).
- `VirtualMachine::run_jit()` in light mode: before calling the inline superscalar
  derivation, check if the item index is within the pre-computed range. If so, read from
  the dataset buffer instead of computing.
- The JIT's `generateProgramLight()` emits a conditional branch: compute address → check
  range → either load from dataset buffer or execute inline derivation.

**Expected effect:** Each item fetch replaced saves ~170 superscalar instructions (the typical
superscalar program length). With 2048 items per hash and ~49% hit rate, that's ~170K
instructions saved per hash, out of ~14.7M JIT instructions. About 1.2% reduction.

But that's only the instruction count. The REAL win might be in the stall profile: the
inline superscalar derivation is a long dependency chain (`IMUL_R`/`IMUL_RCP` heavy). A
simple dataset read is a load instruction with a predictable address (once the item index
is known). If the load can be scheduled earlier and the ALU chain eliminated entirely,
the pipeline has fewer stalls.

**Why it might not work:** The conditional check + branch is itself extra instructions.
If the hit rate is 49% and the check adds 5-10 instructions per item, the break-even point
is tight. Plus, the dataset buffer competes with the cache for DRAM bandwidth.

**Risk:** Low-medium. No correctness risk — the item data is identical to what the inline
derivation would produce. The JIT conditionals are straightforward.

**Effort:** ~3-5 days.

---

### D3. Dynamically switch between light and fast mode at seed rotation

**The idea:** When a new seed arrives and the cache is being re-initialized (seed rotation
latency), use the available 1.3 GiB RAM to pre-compute as much of the dataset as possible
in a background thread. During steady-state mining, use light mode (which is already the
default). The pre-computed dataset portion is available as a cache, not a requirement.

This is essentially D2 but focused on seed-rotation latency rather than steady-state
throughput. If the dataset pre-computation finishes within the seed rotation window, it's
free. If it extends past, the miner falls back to light mode transparently.

**Expected effect:** Seed rotation happens at pool-block-change frequency (minutes to hours
on this hashrate). The 256 MiB Argon2 cache initialization takes ~2-3 seconds. A background
thread pre-computing the dataset would take much longer (dataset init is ~30 minutes on this
hardware). Not feasible.

---

### D4. AArch64 SVE/SVE2 (future hardware)

**The idea:** SVE2 has scatter-gather memory access, predicated execution, and wide SIMD
that could vectorize the scratchpad operations. The current Cortex-A53 doesn't support SVE.
A future device would need a different JIT backend entirely. Not applicable.

---

### D5. Use the Adreno GPU via OpenCL for scratchpad operations

**The idea:** The Adreno 405 GPU in the MSM8929 is a low-end mobile GPU (128 ALU cores,
~450 MHz). OpenCL 2.0 is supported on some Android kernels. The scratchpad operations
(parallel 64-byte reads/writes with ALU dependency chains) might map to GPU compute
shaders.

**Reality check:** GPU RandomX implementations exist in research only. The data-dependent
control flow (CBRANCH) and tight ALU dependency chains are fundamentally serial — the GPU's
strength (massive parallelism) doesn't apply. The scratchpad is 2 MiB, too large for the
GPU's local memory, and the access pattern is random, killing GPU cache performance.

**Verdict:** Not viable for this device or any mobile GPU.

---

### D6. Custom kernel module for JIT code buffer management

**The idea:** The current JIT allocates RWX memory via `mmap(MAP_ANONYMOUS | MAP_PRIVATE,
PROT_READ | PROT_WRITE | PROT_EXEC)`. A kernel module could:
- Reserve a fixed physical memory region for JIT code (bypassing TLB for JIT instruction
  fetches).
- Lock the JIT buffer into L2 cache.
- Set up a dedicated ASID for JIT execution.

**Why it's crazy:** L2 is 256 KiB shared per cluster. The JIT code size is ~100-200 KiB.
You can't lock that much into L2 without starving other data. And a kernel module needs
root, a matching kernel build, and survives kernel upgrades poorly.

**Verdict:** Not practical for a deployable miner.

---

### D7. Non-uniform work distribution for multi-cluster

**The idea:** MiningEngine currently partitions nonces equally among workers. What if the
slow cluster's workers get a *wider* nonce search space (more work per share submission),
reducing the share-submission overhead relative to their throughput? Share submission
involves a blake2b hash + network send + response wait. If the slow cluster submits fewer
shares per second, the overhead per share is identical — no benefit.

Actually, the opposite: give the fast cluster MORE nonces per unit time to match their
higher throughput. But that's already what happens implicitly (all workers race on the
same atomic nonce counter).

---

### D8. Lock-free inter-worker scratchpad sharing for L1 reuse

**The idea:** Each worker has its own 2 MiB scratchpad. Workers on the same core (impossible
— 1 thread per core) or same cluster could share a scratchpad. The scratchpad is overwritten
every hash, so sharing would cause corruption.

Unless... the scratchpad is read-only between hash iterations? No — the VM writes to the
scratchpad during execution (ISTORE, and the mx/ma accumulator pattern overwrites scratchpad
entries). It's fundamentally per-thread state.

---

### D9. Out-of-order scratchpad loads in the JIT

**The idea:** The Cortex-A53 is in-order. But the memory system can handle multiple
outstanding loads (up to ~4). The JIT currently issues loads one at a time, waiting for
each to complete before using the result. What if we **hoist loads** as early as possible
— issue the scratchpad load at the start of the VM instruction, do other work (ALU) while
waiting for the load to complete, then consume the loaded value?

The JIT already does this for some opcodes (the scratchpad address is computed from the
ma/mx accumulator at the START of each instruction group in the interpreter — see
`vm.cpp:851-859`). In the JIT, the `emitMemLoad` functions compute the address and issue
the load in sequence with the ALU work. The emission is in-order, so the load and its
consumer are adjacent.

**The crack:** On Cortex-A53, a load instruction followed by an independent ALU instruction
can dual-issue (one load + one ALU per cycle). If there's an independent ALU instruction
BETWEEN the load and its consumer, the load's latency is partially hidden. The emitter
scheduler (which reorders emission within 3-instruction windows) already tries to do this
— but only for multiply instructions, not for loads. The memory-op extension was tried
and reverted.

What if we try the scheduler extension AGAIN, but with a narrower scope — only hoist
loads that are provably independent of EVERY instruction in a 5-instruction window, rather
than just the 2-instruction window the first attempt used? The wider window gives more
opportunity to find independent work, reducing the chance of accidentally causing a stall
that manifests as wrong hashes.

Or better: only hoist loads for opcodes where the loaded value is consumed MORE THAN N
instructions later, guaranteeing no data hazard. The JIT's `computeFootprint()` already
tracks register liveness — use it to find loads whose consumers are far enough away.

**Expected effect:** If the reverted memory-op scheduler extension's failure was a
narrowly missed hazard (not a fundamental correctness issue), a more conservative version
with wider analysis might work. The ceiling is the ~6% latency-recoverable gap.

**Risk:** Medium. The first attempt caused a JIT/interpreter divergence that was never
fully explained. A more conservative version with provable safety guarantees might avoid
it.

---

### D10. Co-locate cache and scratchpad on huge pages backed by 1 GiB pages

**The idea:** The kernel supports 1 GiB huge pages (`hugetlbfs` with `pagesize=1G`). If the
256 MiB Argon2 cache + per-worker scratchpads (~16 MiB for 8 workers) are backed by a
single 1 GiB huge page, the TLB coverage for the entire working set is 1 entry. On
Cortex-A53 with a 32-entry L1 dTLB and 512-entry L2 TLB, this effectively eliminates all
dTLB misses for the cache and scratchpad region.

**Implementation:**
- Reserve a 1 GiB hugetlbfs pool at boot (`hugepagesz=1G hugepages=1`)
- `mmap` the cache and scratchpads from this pool
- Verify TLB miss reduction with `perf stat -e dTLB-load-misses`

**Already tried?** No — previous huge-page work used transparent huge pages (2 MiB THP),
not 1 GiB hugetlbfs. `ROADMAP.md` line 236 says dTLB-load-misses are already negligible
(~1.6/million instructions) at the 2 MiB THP level. 1 GiB pages would be even better
but the gain from eliminating already-negligible misses is itself negligible.

**Verdict:** Already closed by the Phase 6 huge-page residency check. dTLB misses are
not a problem.

---

### D11. Cooperative nonce space: hash every 2048-input batch together

**The idea:** Each hash processes one 76-byte block header (nonce varies). The scratchpad
is filled from the block data + `tempHash`, then 2048 VM instructions run against it.
What if we hash N different nonces against the SAME scratchpad? I.e., fill the scratchpad
once, save it, then run 2048 VM instructions against it for N different nonce values
(which only change the first few instructions, if at all — the nonce changes the seed
hash, which changes the scratchpad fill, which is different for every nonce).

Actually, the scratchpad fill depends on the blake2b of the input, which includes the
nonce. So every nonce has a different scratchpad. Dead end.

---

## Quick-comparison table

| Idea | Target | Est. gain | Correctness risk | Effort |
|------|--------|-----------|-----------------|--------|
| A1 Hybrid JIT/interpreter | Main VM program (9.2% ins, 20% cyc) | Unknown — call it ~5% cycles | Low | 2-3 days |
| A3 Superscalar interleaving | Superscalar (72.7%) | Unknown — could be large or zero | High | Weeks |
| B1 Per-cluster cache replication | Slow-cluster interconnect (cores 4-7) | Up to ~6-12% aggregate | Low | 1-2 days |
| C3 JIT buffer layout | I-cache (all JIT) | ~1% | None | 1 day |
| D2 Partial fast mode | Superscalar derivation (72.7%) | ~1-2% instruction count | Low | 3-5 days |
| D9 Conservative load hoisting | Main VM program memory latency | ~3-5% | Medium | 3-5 days |

**My personal ranking of what to try first (if anything):**

1. **B1 (per-cluster cache)** — lowest effort for potentially meaningful gain. No correctness
   risk. Even if it's zero (memory-controller-bound, not interconnect-bound), it's 1-2 days
   to know for sure.

2. **A1 (hybrid JIT/interpreter)** — second-lowest effort. The only idea that directly
   addresses the architectural (non-latency) portion of the main VM program's IPC penalty.
   Could be zero, but the reasoning is sound and the implementation is low-risk.

3. **D2 (partial fast mode)** — moderate effort, no correctness risk, and addresses the
   superscalar instruction count (the only region where instruction-count-reduction hasn't
   been proven futile, because it REPLACES instructions rather than abbreviating them).

4. **D9 (conservative load hoisting)** — only if A1 and B1 both show promise but don't
   fully close the gap. Higher risk, needs a careful hazard analysis.

5. **C3 (JIT buffer layout)** — trivial effort, measure in an afternoon if curiosity strikes.

Everything else in this doc is either already ruled out by evidence (B2, B3, C1, C2, D10),
impractical on this hardware (D1, D4, D5, D6, D8, D11), or too risky to justify the effort
without an A/B result showing room (A2, A3).
