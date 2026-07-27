# armrx — Performance Plan, 2026-07-27: change the axis

*** Written by:*** Claude Opus 5 

**Status:** new plan, nothing here started. Opens after
`docs/plans/performance-plan-20260725.md` (gated steps all closed),
`docs/plans/experimental-performance-ideas-20260725.md` (worked to closure),
and `docs/plans/mid-high-risk-performance-ideas-20260726.md` (Tier 1 resolved).

---

## Why we are stuck: every closed lead shared one premise

Read the negative results as a set rather than individually:

| Attempt | Outcome |
|---|---|
| CSEL branchless CBRANCH | regression, reverted |
| Newton-Raphson FDIV/FSQRT | no win |
| NEON hardware AES / vector-permute AES / fused hash+fill | 3 measured regressions |
| Superscalar literal-pool relayout | −1.12% hashrate, reverted |
| `IMUL_RCP` literal-load elimination | +8.2% IPC, +8.5% instructions, net −0.3% |
| `IMUL_RCP` register pre-assignment | correctness failure; safe budget = 1 register |
| `IXOR_C*` logical-immediate encoding | 0 of 20,000 real immediates encodable |
| Peephole JIT coalescing | closed on evidence |
| Emitter scheduler (main + superscalar + widening) | +0.39% IPC total, adopted |
| Memory-op scheduler extension | correctness divergence, reverted |
| Prefetch insertion (Step 2) | closed by Step 1's 6% ceiling |
| PGO, twice | confirmed null |
| BOLT | expected null (I-cache miss 0.788%) |

Every one of these says: *do the same work, spend slightly fewer instructions or
slightly fewer stall cycles doing it.* The measured combined yield of that entire
program of work is under 1%. Two independent measurements explain why it had to be:

- **Step 1** (`scratchpad-locality-bound-20260726.md`): making the scratchpad
  effectively L1-resident bought only +6% IPC. The residual is dependency-chain
  latency and in-order pipeline depth.
- **The XMRig comparison** (`PLAN.md` Phase 6 item 3): armrx's IPC (0.731) is
  *better* than XMRig's (0.612) and its stall rate is *lower* (11.41% vs 16.70%).

We are not stalling more than the reference implementation. We are not
mis-scheduling. On an in-order dual-issue Cortex-A53 running a workload
deliberately designed by its authors to be latency-bound, micro-optimization is
exhausted, and the evidence has been saying so for three sessions.

**Two axes have never been tried, and both are structural rather than
micro-architectural:**

1. **Do less work.** 63%+ of all cycles go to re-deriving dataset items that are
   a pure, cacheable function of the seed. The device has 1.2 GiB of unused RAM.
2. **Do more independent work at once.** The one classical remedy for a
   latency-bound in-order pipeline — interleaving two independent instruction
   streams to fill the stall slots — has never been attempted anywhere in this
   codebase's hot path.

Items 1–2 below pursue axis 1. Items 3–4 pursue axis 2. They compose.

---

## Item 1 — Hybrid partial dataset (**highest expected value, lowest correctness risk**)

### The observation

RandomX has exactly two documented memory modes: fast (2080 MiB dataset,
impossible on a 2 GiB device) and light (256 MiB cache, every dataset item
re-derived on demand). armrx implements both. **Nothing in the specification
requires the choice to be all-or-nothing** — the dataset is a deterministic pure
function of the cache, so *any* precomputed subset of it is valid, and reading a
precomputed item is bit-identical to deriving it.

This device sits exactly in the gap the binary choice leaves open.

### Measured headroom

`devbox_status`, 2026-07-27, miner not running:

```
MemAvailable:    1248020 kB      (1219 MiB)
```

Light-mode steady-state footprint is 256 MiB cache + 8 × 2 MiB scratchpad =
272 MiB, so roughly **950 MiB stays free while mining**. Budget 512 MiB
conservatively, 768 MiB aggressively.

### Hit rate is directly computable

`src/vm.cpp:894` — `readPtr = dataset_offset_ + (ma_ & 0x7fffffc0)` — and
`src/vm.cpp:239` — `dataset_offset_ = (entropy_[13] % 524288) * 64`.

So the byte address is uniform over a 2 GiB window, shifted by a per-hash offset
of at most 32 MiB. Caching the first `B` bytes of the dataset gives a hit rate of
essentially `B / 2 GiB`:

| Partial dataset | Hit rate | Superscalar work removed |
|---|---|---|
| 256 MiB | 12.5% | 12.5% |
| 512 MiB | 25.0% | 25.0% |
| 768 MiB | 37.5% | 37.5% |
| 896 MiB | 43.8% | 43.8% |

`tools/jit_correlate.py` attributes **63.42% of all cycles** to superscalar
opcodes. Charging a DRAM read at ~15% of the cost of the derivation it replaces:

| Partial dataset | Estimated hashrate gain |
|---|---|
| 512 MiB | **+16%** |
| 768 MiB | **+26%** |
| 896 MiB | **+32%** |

These are estimates, not measurements — treat them as a reason to try, not a
promise. Even at half, this exceeds the entire measured yield of every
code-level optimization in this project's history combined, and rivals the
`isolcpus` deployment win.

### Why the correctness risk is genuinely low

Unlike every JIT-scheduler item, this has a **total, cheap correctness oracle**:
`partial[i] == generate_dataset_item(cache, i)` for all `i`, checkable
exhaustively over the populated range. Both sides of the hybrid branch already
exist and are already KAT-verified — fast mode's direct dataset load
(`rx_program_xor_with_dataset_line`) and light mode's derivation
(`rx_calc_dataset_item`). This item does not invent a new computation; it
chooses between two proven ones.

### Implementation sketch

1. **Allocation + fill.** New `PartialDataset` owning a `mmap`'d, `MADV_HUGEPAGE`
   buffer of `N` items. `initialize_dataset()` (`src/dataset.cpp:66`) already
   does exactly this fill, already 2-way NEON-vectorized, already parallelized
   across worker threads for fast mode. Reuse verbatim with
   `start_item = 0, item_count = N`.
2. **JIT emission.** In `randomx_program_aarch64_vm_instructions_end_light`
   (`jit_compiler_a64_static.S:528`), before the `bl rx_calc_dataset_item` path,
   insert a bound check:
   ```
   # x2 already holds the masked byte address + dataset offset
   cmp  x2, <partial_bytes>
   b.hs .Lderive
   add  x10, <partial_base>, x2
   b    rx_program_xor_with_dataset_line
   .Lderive:
       <existing light-mode block, unchanged>
   ```
   ~4 extra instructions on a path that currently costs ~3,563. The branch is
   poorly predicted (a ~50/50 split) but costs ~0.1% of cycles at 16,384
   executions per hash — verify, don't assume.
3. **Register for the base pointer.** Needs one live pointer beyond the existing
   cache pointer in x1. Options, in preference order: reuse a slot in the
   existing `MemoryRegisters` struct the prologue already loads; or a
   JIT-materialized immediate. Resolve by reading the prologue, not by guessing.
4. **Incremental fill (do this second, not first).** The bound is a
   compile-time immediate and the JIT recompiles the program 8× per hash
   anyway — so the bound can simply be re-read from the VM at each compile.
   That means mining can start immediately in pure light mode and the threshold
   can be raised as the background fill progresses, at zero runtime cost and
   zero added risk. Ship the blocking version first; add this once the win is
   confirmed.
5. **CLI.** `--dataset-mb=N` (0 = off), plus an `auto` policy that sizes from
   `MemAvailable` minus a headroom reserve, mirroring `src/memory.cpp`'s
   existing 256 MiB OS reserve logic.

### Gates and honest failure modes

- **Gate A — init cost.** Measure the wall-clock fill time for 512 MiB before
  wiring anything into the JIT. Rough estimate is 10–30 s across 8 cores,
  amortized over a ~2.8-day seed rotation, which would be negligible. If it is
  minutes rather than seconds, the incremental-fill design (step 4) becomes
  mandatory rather than optional.
- **Gate B — the memory-path caveat, and it is a real one.** This device's
  known 8-worker bottleneck is *shared memory-path arbitration between the two
  L2 clusters* (`NEXT_STEPS.md`, "Major finding"). This item deliberately trades
  ALU work for random DRAM traffic — roughly 2.5M extra random 64-byte reads per
  second aggregate at 50% hit rate. That is the exact resource already under
  contention. **A single-core `taskset` measurement will overstate this win.**
  Measure at 1 worker *and* at 8 workers; the 8-worker number is the one that
  decides adoption.
- **Gate C — memory pressure.** 2 GiB device with no swap. Size conservatively,
  verify no OOM-killer activity across a multi-hour run, and re-check
  `MemAvailable` while mining rather than idle.
- **TLB.** 768 MiB of random access needs THP; without it this could be
  dominated by page walks. The Argon2 cache is already ~100% THP-coalesced on
  this kernel, so the precedent is good — but verify via `smaps`, since
  `dTLB-load-misses` is currently negligible only because the working set is
  currently small.

---

## Item 2 — Asymmetric per-cluster memory mode (cheap, falls out of Item 1)

Contingent on Item 1 landing, and directly motivated by Gate B.

Cores 0–3 win the interconnect arbitration and sustain ~4.26 H/s; cores 4–7 lose
it and settle to ~2.13 H/s. Item 1 shifts load from ALU to the contended memory
path. So run the two clusters **differently**: cluster 0 workers use the partial
dataset (memory-heavy, they win arbitration anyway); cluster 1 workers stay on
pure derivation (ALU-heavy, near-zero extra memory traffic, and their ALUs are
idle waiting on the bus).

Implementation is a per-worker boolean threaded into VM construction — the JIT
already compiles per worker. Effort is hours, and the two clusters are already
individually measurable with the existing per-worker hashrate reporting.

This is speculative but it is the *only* idea in this project's history that
turns the cluster asymmetry from a liability into a scheduling opportunity. It
is worth one afternoon after Item 1 is measured.

---

## Item 3 — 2-way interleaved dataset-item derivation

### The observation

`src/jit_compiler_a64_static.S:528-560`: at iteration *i*, the light-mode block
extracts this iteration's item address (`lsr x2, x9, 32`) and *then* computes the
next iteration's address (`eor x9, x9, x20` / `ror x9, x9, 32`). **Both addresses
are live simultaneously.** RandomX's one-iteration dataset lookahead — which
exists in the spec so implementations can prefetch — means the derivation for
item *i+1* can legally begin at iteration *i*.

So derive two items per call, every other iteration, with the second cached in a
64-byte stack slot.

### Why this should work where scheduling did not

SuperscalarHash is *designed* to be latency-bound (`kSuperscalarLatency = 170`).
The emitter scheduler can only reorder within a 3–4 instruction window inside a
single dependency chain, which is why it yielded +0.39% total. Two derivations
are **completely independent** — there is no dependency to hide, only two chains
to interleave. On an in-order dual-issue core with `IMUL_R` (21% of cycles) and
`IMUL_RCP` (14%) both multi-cycle-latency, filling one chain's stall slots with
the other's work is the textbook remedy, and it is the remedy Step 1's
"architectural — dependency chains and pipeline depth" conclusion points at.

### The emission is embarrassingly simple

Both streams execute the *same* eight superscalar programs (same seed). So:

```
for each instruction in program:
    emit(instruction, register_set_A)
    emit(instruction, register_set_B)
```

Two disjoint register sets, zero cross-stream dependencies, **no hazard analysis
required at all**. This is categorically unlike the memory-op scheduler that
failed — there is no reordering within a chain, so there is no new hazard class
to model. The existing scheduler needn't be touched.

### Register budget

Each stream needs 8 registers (`rl[0..7]`) plus a mix pointer and a temp = ~20
total for two streams. `rx_calc_dataset_item` currently saves x0–x13. A new
2-way entry point can additionally save x19–x28 in its prologue (~6 extra
`stp`/`ldp` pairs, amortized over two items) — which also *removes* the exact
hazard that killed the `IMUL_RCP` pre-assignment attempt, since those registers
become explicitly owned rather than accidentally untouched.

### Costs and gates

- **Code size doubles** (20,916 → ~42 KB) against a 16 KiB L1 I-cache. Current
  miss rate is 0.788% because the region streams sequentially; doubling it may
  or may not matter. **Gate: measure `l1i_cache_refill` before and after.** This
  is the most likely way this item fails.
- **Correctness oracle is total and cheap**: derivation is a pure function of
  `item_number`. A differential test over random item numbers against the
  existing 1-way path is exhaustive in practice and trivial to write. Build it
  *before* the implementation, in the style of `test_jit_scheduler_stress.cpp`.
- Interacts with Item 1: fewer derivations means less to interleave. The two
  compose but sub-additively — sequence Item 1 first and re-estimate.

### Precedent inside this repo

`src/dataset.cpp:88-135` already does exactly this 2-way interleave in C++/NEON
for fast-mode dataset init, and it is already correct and tested. This item is
that same idea moved into the JIT and onto the mining hot path.

---

## Item 4 — 2-way whole-hash batching (the moonshot)

Generalize Item 3 to the entire VM: one worker thread runs **two nonces
simultaneously**, interleaved at the instruction level, sharing one dataset-item
derivation call (which subsumes Item 3 for free).

This is the direct answer to Step 1's finding. Both regions — the main VM
program's ~2.2× IPC penalty and the superscalar chain's latency-bound design —
are dependency-chain-bound, and neither has any independent work available to
fill its stalls today. Two nonces provide exactly that.

**Feasibility, honestly assessed:**

- *GPR budget*: 16 VM registers + 2×(spAddr pair, mx/ma, scratchpad base) +
  temps ≈ 28 of 31. Tight, plausibly workable.
- *NEON budget*: 2 × (4 `f` + 4 `e` + 4 `a`) = 24 of 32. Fits.
- *Code size*: main program doubles, on top of Item 3's doubling. I-cache
  pressure is the central risk, same as Item 3 but worse.
- *Effort*: weeks. Requires reworking the static asm template and the JIT's
  register allocation. This is the largest change ever proposed for this
  codebase.
- *Correctness oracle*: still clean — `hash(nonce)` must equal the 1-way result,
  for both lanes. Every existing KAT applies unchanged.
- *Upside if the latency-bound diagnosis is right*: 1.4–1.8× on >80% of cycles.

**Do not start this before Item 3 is measured.** Item 3 is the same hypothesis at
a tenth of the cost, on a self-contained pure function with a trivial oracle. If
2-way interleaving does not pay off there, it will not pay off here, and Item 3
will have answered the question for a few days of work instead of a few weeks.

---

## Item 5 — Instruction-budget reconciliation (cheap diagnostic, run anytime)

armrx uses **33.5% more instructions per hash than XMRig** (132.93M vs 99.57M) on
the same device, same job, same light mode — and that gap is the entire
cluster-normalized ~10-12% performance deficit. It has never been explained.

A static accounting is cheap and needs no XMRig internals (respecting the
clean-room boundary): superscalar instructions/call × 16,384, plus the main
program's dumped size × iterations, plus AES fill/hash, plus Argon2, versus the
measured 132.93M. All the tooling exists (`--jit-dump`, `tools/jit_correlate.py`).

Two prior counts do not reconcile: item 13's 58.4M/hash for the superscalar path
(explicitly a lower bound) versus the region-split's "72.71% of instructions"
(≈96.6M). That ~38M discrepancy is ~2,300 unaccounted instructions *per dataset
item call*. Either one of the numbers is wrong, or there is a large unexamined
overhead in the hottest path in the program. Both outcomes are worth knowing,
and finding out is an afternoon.

Run this in parallel with Item 1 — it is measurement-only, zero risk, and could
redirect everything below it.

---

## Item 6 — Main/stratum thread cost on core 0 (carried over, still open)

The only genuinely open item from `NEXT_STEPS.md`. Worker 0 shares core 0 with
the stratum reader, JSON/job handling, and the per-second console print, costing
it ~3.6 H/s of its ~4.26 H/s potential under real pool mining. Removing worker 0
is confirmed *wrong* (it would net −0.6 H/s).

The unmeasured lever is reducing the main thread's own cost: batch or throttle
the per-second console render, move JSON parsing off the critical path, and
check whether the metrics/TUI threads are also landing on core 0. Worth an hour
of `perf` on the main thread before designing anything.

---

## Suggested sequence

```
Item 5 (diagnostic, parallel, zero risk)
   │
Item 1 ── Gate A (init cost) ── Gate B (8-worker measurement) ── Gate C (memory)
   │                                      │
   │                                      └── Item 2 (asymmetric clusters)
   │
Item 3 ── Gate (l1i_cache_refill) ── differential test first
   │
   └── Item 4 (only if Item 3 pays)

Item 6 anytime (independent, operational)
```

## Measurement discipline (unchanged, and it has caught real errors here)

- `taskset`-pin both sides of every A/B — an unpinned comparison already produced
  a false "PGO wins 2×" read on this device.
- `perf stat -e cycles,instructions` over wall-clock hashrate; wall-clock has
  already been shown insufficiently sensitive at these effect sizes.
- Reverse trial order at least once to rule out thermal drift.
- Distinguish burst from sustained: cores 4–7 read 2.84 H/s at
  `--warmup=15 --seconds=60` and 2.13 H/s at `--warmup=60 --seconds=180`. Use
  the long window.
- **For Item 1 specifically: the 8-worker number decides, not the 1-worker
  number.** The whole risk of that item lives in cross-cluster memory contention
  that a single-core measurement cannot see.
