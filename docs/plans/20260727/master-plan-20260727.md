# armrx — Master Performance Plan, 2026-07-27

**Status:** synthesis document. Combines four independently-written 2026-07-27 plans into one
sequenced backlog. Nothing below has started; this file is the thing to work from going forward.
The four source documents are kept as-is for full reasoning/detail and are cited by short name
throughout:

| Short name | File | Author | Core thesis |
|---|---|---|---|
| **Opus** | `performance-plan-20260727.md` | Claude Opus 5 | Change the axis: do less work (partial dataset) or do more independent work at once (interleaving) |
| **Deepseek** | `hail-mary-ideas-20260727.md` | Deepseek V4 | Brainstorm across architecture/topology/toolchain/"out there"; ranks per-cluster cache + hybrid interpreter highest |
| **Sonnet-R2** | `hail-mary-round2-20260727.md` | Claude Sonnet 5 | Is the 94%-architectural ceiling actually proven, or just the current scheduler's ceiling? Proposes cross-hash (dual-nonce) interleaving |
| **Hermes** | `performance-plan-breakthrough-20260727.md` | Hermes (Hy3) | Pivot: the real gap vs XMRig is **instruction count** (132.93M vs 99.57M), not IPC — armrx already wins the stall war. Re-screens "IPC-closed" ideas on the count axis |
| **MidHigh** | `../mid-high-risk-performance-ideas-20260726.md` | (2026-07-26, pre-dates the four above) | Correctness-risk-ranked tiers. Tier 1 (items 1-2) already resolved — item 2 (scheduler window widening) adopted, item 1 (`IMUL_RCP` pre-assignment) closed-but-revisitable, both already reflected below. **Tier 2 (item 3, BOLT) and Tier 3 (items 4-5) were never started** and are folded into this synthesis for the first time in §Track E/J. There is no Tier 4 in the source file — it stops at Tier 3. |

All five open from the same closed state: `docs/plans/performance-plan-20260725.md` (gated plan,
closed), `docs/plans/experimental-performance-ideas-20260725.md` (backlog worked to closure),
and `docs/experiments/scratchpad-locality-bound-20260726.md` (the "+6% IPC ceiling, 94% architectural"
finding). `NEXT_STEPS.md` said no genuinely open item remained except worker-to-core placement —
the four 2026-07-27 documents were written to find out if that's actually true; MidHigh's leftover
Tier 2/3 items are the one piece of unstarted work from *before* that round that hadn't been folded
in yet.

*(Housekeeping note: Sonnet-R2 recorded that round 1's file had been moved to `~/Masaüstü/` outside
the repo. It is present at `docs/plans/20260727/hail-mary-ideas-20260727.md` now — that loose end
is resolved, all four source files live in this folder.)*

**Verification pass (Hermes, 2026-07-27, independently checked and confirmed against source):**
the mechanism claims and closed-leads list hold up, and the clean-room boundary (no XMRig
disassembly) is honored. Five concrete factual errors were found and corrected in place —
stale `.S` line numbers in Track C, a wrong handler-table symbol in Track A item 1
(`kCompileHandlers` is the interpreter's table; the JIT's is `engine[256]`), the unverified
"~96.6M/hash" superscalar figure now marked as such rather than presented as an established
discrepancy, an understated register-save count in Track C, and a note to confirm A53 PMU event
numbers before Track A item 2 is run. None of the corrections change any track's sequencing,
priority, or expected payoff — they only fix citations an executor would otherwise trip over.

---

## 0. The load-bearing disagreement between these docs — read this before picking an item

Opus, Deepseek, and Sonnet-R2 all reason primarily in **IPC / stall-cycle** terms (the framing that
closed everything in the 2026-07-25/26 plans). Hermes points out this framing has a blind spot:
armrx's IPC (0.731) and stall rate (11.41% `ld_dep_stall`) are already *better* than XMRig's (0.612
IPC, 16.70% stall) — the entire measured ~10-12% cluster-normalized deficit is explained by armrx
emitting **33.5% more instructions per hash** (132.93M vs 99.57M) doing equivalent work.

This matters concretely: several ideas below were killed in earlier plans *because they didn't move
IPC* (peephole coalescing, `IMUL_RCP` literal-load elimination net -0.3%, superscalar literal-pool
relayout). Those closures are correct on the IPC axis but were never evaluated on the
instruction-count axis, which is the axis the actual XMRig gap lives on. **Track F below
re-opens that class of idea, but only when scored against instruction count, not IPC** — this is
the single biggest methodological correction across all four documents and should govern how
everything else here gets judged.

---

## 1. Ground truth (shared numeric baseline all four docs build on)

- Superscalar / dataset-derivation region: **72.71% of instructions, 63.42% of cycles, 1.145× IPC**
  — already efficient. Every instruction-count-reduction attempt here has regressed
  (`IMUL_RCP` literal-load elimination, superscalar literal-pool relayout, CSEL/CBRANCH).
- Main VM program region: **9.23% of instructions, 20.04% of cycles, 0.461× IPC** — the "problem
  child." `docs/experiments/scratchpad-locality-bound-20260726.md` showed **~94% of this penalty is
  architectural** (dependency chains / in-order pipeline depth on the Cortex-A53), not
  memory-latency; forcing L1 residency recovered only +6% IPC.
- C++ overhead (AES fill/hash, Argon2, JSON, etc.): **15.56% of instructions, 1.107× IPC** —
  proportionally efficient, except `hash_aes_1r_x4`/`fill_aes_1r_x4` alone at **~12.3% of all
  cycles** (Track G below).
- vs. XMRig, same device/job/light-mode: armrx **132.93M instructions/hash** (IPC 0.731) vs XMRig
  **99.57M instructions/hash** (IPC 0.612). armrx wins on IPC and stall rate; loses on raw
  instruction count. Net: ~90% of XMRig per-cluster.
- The only large *measured* win in this project's history is operational, not code-level:
  `isolcpus=1-7 rcu_nocbs=1-7` on the kernel boot cmdline, **+14% aggregate hashrate**
  (`docs/experiments/isolcpus-rt-priority-win.md`).
- Device: 2 GiB RAM, light mode forced, ~1.2-1.3 GiB `MemAvailable` while idle. Two 4-core L2
  clusters (MSM8929/Snapdragon 415) with measured throughput asymmetry — cluster 0
  (cores 0-3) ~4.26 H/s/core, cluster 1 (cores 4-7) ~2.13-2.84 H/s/core depending on isolation and
  measurement window (long-window/sustained numbers are the trustworthy ones — see §6).

---

## 2. Deduplication map

Three ideas were independently proposed by more than one document. Treat these as **one item**
each, not two:

- **Partial/hybrid dataset in light mode** = Opus Item 1 = Deepseek D2. Nearly identical mechanism
  (precompute a prefix of the 2 GiB dataset, hit rate ≈ `bytes_cached / 2 GiB`, fall back to
  on-the-fly derivation on miss). Opus's writeup is more implementation-complete (exact register/
  JIT-emission sketch, gates A/B/C); Deepseek's is the same idea with a rougher instruction-count
  estimate. **Track B below uses Opus's version as primary, Deepseek D2 as corroborating estimate.**
- **Cross-hash / dual-nonce interleaving** — Opus Items 3 (superscalar-only 2-way interleave, cheap)
  and 4 (full dual-nonce, the "moonshot") sit at opposite ends of the same idea Sonnet-R2 develops
  independently as Category E (E1 full dual-nonce interleave, E2 lighter boundary-only version).
  These compose into one graduated track, not three separate ones. **Track D below merges them by
  cost, cheapest first: Opus Item 3 → Sonnet-R2 E2 → Sonnet-R2 E1 / Opus Item 4.**
- **Instruction-budget audit** = Opus Item 5 ("instruction-budget reconciliation") = Hermes Item 1
  ("per-opcode AArch64 instruction-budget audit"). Same diagnostic: account for where
  `132.93M instructions/hash` actually goes, per-opcode, against a theoretical minimum. Hermes's
  version is more actionable (ties directly into the JIT's per-opcode handler table, proposes the
  concrete tool extension). **Track A uses Hermes's framing** (with one correction applied — see
  Track A item 1's note on which handler table this actually means).
- **Worker/main-thread core-0 cost** = Opus Item 6 = Hermes Item 7. Both point at the same
  `NEXT_STEPS.md` open item: worker 0 shares core 0 with the stratum reader / JSON / per-second
  console print, and under `isolcpus` it can't be relocated (no sysfs `cpufreq` data →
  `detect_core_order()` falls back to sequential placement). **One item, Track I.**

---

## Track A — Diagnostics (cheap, parallel, zero/near-zero risk — run first)

Everything downstream should be scored against these, not against intuition.

1. **Per-opcode instruction-budget audit** *(Hermes #1 = Opus #5)*. For every emitted VM opcode
   and superscalar opcode, compute a theoretical-minimum AArch64 instruction count and diff
   against `--jit-dump`'s actual emission. This is the map to the whole remaining instruction-count
   gap and the gating input for Track F. **Correction (Hermes's own follow-up review, verified
   against source): the table to instrument is `JitCompilerA64::engine[256]`
   (`src/jit_compiler_a64.cpp:1941`, built from `INST_HANDLE` in `instruction_weights.hpp`), not
   `kCompileHandlers` — that symbol is the *interpreter's* dispatch table (`vm.cpp:542`,
   `vm.hpp:156`), a different, non-JIT code path. Hermes's own breakthrough plan made the same
   mix-up; instrumenting the wrong table would silently audit interpreted-mode instruction counts
   instead of the JIT's.**

   **DONE (2026-07-27), partial: real superscalar per-opcode data obtained, one number confirmed
   exact, one reconciliation attempt failed honestly and the reason is now documented.**

   `./armrx --jit-dump` on-device already builds exactly the per-opcode aggregate table this item
   wants for the superscalar region (`dumpJitCode()`, `jit_compiler_a64.cpp:965-1006` — count,
   bytes, avg instruction size, % of bytes per opcode; existing tooling, not something new to
   build). Real output for one `generateSuperscalarHash()` compile:

   | opcode | count | avg size (bytes) | instructions | notes |
   |---|---|---|---|---|
   | `ISUB_R`/`IXOR_R`/`IADD_RS`/`IMUL_R`/`IROR_C`/`IMULH_R`/`ISMULH_R` | — | 4.00 | 1 | already minimal — confirms the earlier source-reading finding with hard numbers |
   | `IMUL_RCP` | 239 | 8.00 | 2 | `ldr` (literal) + `mul`, unconditional — confirms the earlier source-reading finding exactly |
   | `IXOR_C7`/`C8`/`C9` | ~137/128/122 | 12.00 | 3 | `movz`+`movk`+`eor` — matches the already-closed 0/20,000-encodable finding (full 32-bit materialization is necessary, not waste) |
   | `IADD_C7`/`C8`/`C9` | 135/92/100 | ~12.00 (11.97 for C7) | 3 (occasionally 2) | **checked**: `emitAddImmediate` (`jit_compiler_a64.cpp:1225`) has a fast path for `imm < 2^24` (1-2 `ADD_IMM` instructions) and falls back to `emitMovImmediate`+`ADD` (up to 3 total) for `imm >= 2^24` — AArch64's `ADD`-immediate field simply cannot encode a value that large, so the 3-instruction sequence is architecturally necessary for this opcode's constant range, not waste. Same "already minimal" verdict as everything else checked. |

   **Total instructions: 3563, total bytes: 20916, for one compiled `rx_calc_dataset_item` body**
   (the superscalar-only portion, excluding fixed wrapper/frame code). This makes the earlier
   **58.4M/hash figure exact, not a lower bound**: `3563 × 16,384 calls/hash = 58,384,832 ≈ 58.4M`,
   confirmed by a hard current-code measurement rather than inferred.

   **Reconciliation attempt against the total 132.93M/hash figure — failed cleanly, and the
   failure is itself the useful finding.** Tried to get a clean, current, first-principles total by
   running `perf stat -e cycles,instructions -- bench_armrx --full-hash-only` (200-sample loop,
   `randomx_calculate_hash()` per sample) directly, rather than trusting old figures. Naive
   `total_instructions / 200` gave **368.7M instructions/hash — 2.77× the historical 132.93M**,
   clearly wrong. Root cause, verified by arithmetic: the benchmark's own reported median
   (223.47 ms/hash) implies only ~46.9s of wall-clock for the 210 hashes computed (10 warmup + 200
   measured), but the `perf stat`-wrapped process ran 131.14s — an **84.2s gap** consistent with
   the *one-time* `Argon2dCache::initialize()` setup call (separately measured at ~8-17.6s
   single-threaded in the Track B Gate A work above) and possibly also `generateSuperscalarHash()`'s
   one-time per-seed JIT compile, both landing inside the same `perf stat` capture window and
   getting divided by only 200 samples instead of being amortized away. **The historical 132.93M
   figure's own methodology ("derived from measured H/s" during sustained live multi-worker
   mining) inherently avoids this — a long sustained run amortizes one-time setup cost across many
   thousands of hashes, which a 200-sample bench loop does not.** This is not a contradiction of
   the historical figure; it's a measurement-methodology pitfall in the *naive* re-measurement
   attempt, now documented so nobody repeats it. **A clean re-measurement needs one of: (a) start
   the `perf stat` capture only after warmup/cache-init completes (nothing currently exposes a
   clean hook for this — a real, concrete tooling gap), or (b) run enough samples that one-time
   setup is negligible by construction (thousands of hashes, itself a multi-minute run on this
   hardware).** Neither was done here; the 132.93M figure should still be treated as the
   trustworthy total until one of these is done properly.

   **Main-program side also done (2026-07-27), same `--jit-dump` output, self-aggregated** (the
   codebase only prints an automatic per-opcode summary for the superscalar table, not the main
   one, so this required a small offline script over the existing raw per-instruction rows — no
   new on-device tooling). One compiled 8-chained-program main-VM buffer: **2048 RandomX-level
   instructions, 19,340 bytes = 4835 AArch64 instructions** (a *static compiled-code-size* figure,
   not a dynamic per-hash execution count — CBRANCH's backward-loop semantics mean dynamic
   execution count differs from this; **do not treat 4835 as commensurable with the 58.4M
   superscalar figure**, they measure different things and conflating them would be the same class
   of error the 132.93M reconciliation attempt above just got caught making). Every opcode is
   accounted for; the standouts are the float memory ops (`FADD_M`/`FSUB_M` ≈27 bytes/6.8
   instructions avg, `FDIV_M` ≈31 bytes/7.9 instructions avg — noticeably above the ~18-byte/4.5
   instruction average for integer memory ops). **Checked and closed, not waste**: `h_FADD_M`/
   `h_FSUB_M` are `emitMemLoadFP` (5 instructions: address+mask+load+sign-extend+convert-to-double
   — the sign-extend/convert pair is unavoidable, the scratchpad stores 32-bit integers and FP ops
   need a double) plus one `FADD`/`FSUB` = 6 instructions, matching the measured average once the
   occasional 2-instruction address-immediate case is folded in. `h_FDIV_M` additionally emits a
   spec-mandated `bif` exponent/sign-mask operation before the divide (RandomX's FDIV_M semantics
   require masking the loaded value against `eMask` to avoid denormal/NaN/Inf results) — not an
   emitter inefficiency, a correctness requirement. Every other main-program opcode checked
   (`ISTORE`, `IADD_M`/`ISUB_M`/`IMUL_M`/`IXOR_M`, `CBRANCH`) shows average sizes consistent with
   `emitAddImmediate`'s already-tight per-value-encoding behavior (1 or 2 instructions depending on
   whether a given random immediate needs both 12-bit halves) — same "already minimal" verdict.

   **Overall verdict for this item, after checking essentially every high-frequency opcode in both
   the main program and superscalar path, by source reading *and* hard `--jit-dump` measurement:
   per-opcode emission waste is not where the remaining instruction-count gap lives.** Every
   opcode checked is at, or architecturally forced to, its AArch64 instruction-count minimum.
   **This is a real result, not just an absence of findings — it redirects priority within this
   master plan: Track F (instruction-count-driven fusion/peephole) should be treated as low-yield
   unless a *specific*, still-unchecked opcode surfaces a real gap (none has, in everything sampled
   so far), while Track C (the light-mode dataset-item helper's store/reload relay — overhead that
   lives *outside* individual opcode emission, in the fixed call/frame wrapper around
   `bl rx_calc_dataset_item`, paid 16,384×/hash) remains the strongest concrete, well-scoped,
   still-unimplemented lead for the instruction-count axis.** The "reconcile against 132.93M total"
   half of this item is still open — correctly scoped now as needing better isolation tooling
   (a way to start `perf stat` counting only after warmup/cache-init, which doesn't exist yet)
   rather than a bigger sample count. No correctness risk incurred anywhere in this item — entirely
   read-only tooling and existing `--jit-dump`/`perf stat` usage.
2. **PMU frontend vs. backend stall breakdown on the main VM program region** *(Sonnet-R2 F1)*.
   **Event names confirmed on-device 2026-07-27 (`perf list` + a live `perf stat` probe) — Hermes's
   caution was correct, and the specific generic names are unusable here.** The portable generic
   events this item originally assumed (`stalled-cycles-frontend`/`stalled-cycles-backend`, exposed
   as perf's `frontend_cycles_idle`/`backend_cycles_idle` metric aliases) come back **`<not
   supported>`/NaN on this Cortex-A53's kernel PMU driver** — confirmed by directly running
   `perf stat -M backend_cycles_idle,frontend_cycles_idle -- sleep 0.2` and getting exactly that.
   There is no `STALL_FRONTEND`/`STALL_BACKEND` on this hardware at all. What *is* available is a
   set of A53-specific implementation-defined dep-stall events (`perf list`), several finer-grained
   than a simple two-way split and one of which (`ld_dep_stall`) is already a proven, previously-used
   event in this project's own history (the Phase 6 XMRig comparison's 11.41%-vs-16.70% figure).
   Use these directly with `-e`, bucketed:
   - **Front-end-like** (fetch/decode starvation): `ic_dep_stall` (I-cache miss), `iutlb_dep_stall`
     (I-µTLB miss), `decode_dep_stall` (pre-decode error), `other_iq_dep_stall` (instruction-queue
     empty, other cause).
   - **Back-end-like** (execution-stage interlock): `ld_dep_stall`, `st_dep_stall`, `agu_dep_stall`
     (address-generation interlock), `other_interlock_stall`, `simd_dep_stall`, `stall_sb_full`
     (store-buffer full).
   The "94% architectural" finding was derived from memory-latency experiments, not a direct
   front-end/back-end stall split. If a meaningful share of the main VM program's stall cycles turns
   out to be front-end-like rather than back-end-like, that redirects effort toward code-layout
   fixes (cheap, Track J) instead of concurrency (expensive, Track D/E) — determines whether those
   tracks are even pointed at the right problem.

   **DONE (2026-07-27), decisive result: back-end-dominated, ~26:1.** Measured on
   `bench_armrx --scratchpad-real` (the same main-VM-program-execute-only isolation Phase 9's
   scratchpad-locality experiment used), `taskset -c 3`, three grouped `perf stat` passes (A53 has
   too few physical counters for all 11 events + cycles at once):

   | Group | Event | % of cycles |
   |---|---|---|
   | Front-end-like | `ic_dep_stall` | 0.855% |
   | Front-end-like | `iutlb_dep_stall` | 0.0009% |
   | Front-end-like | `decode_dep_stall` | 0.000% |
   | Front-end-like | `other_iq_dep_stall` | 0.045% |
   | **Front-end-like total** | | **0.901%** |
   | Back-end-like | `ld_dep_stall` | 11.132% |
   | Back-end-like | `st_dep_stall` | 0.309% |
   | Back-end-like | `agu_dep_stall` | 1.107% |
   | Back-end-like | `other_interlock_stall` | 6.735% |
   | Back-end-like | `simd_dep_stall` | 3.645% |
   | Back-end-like | `stall_sb_full` | 0.149% |
   | **Back-end-like total** | | **23.08%** |

   **Back-end-like stalls outweigh front-end-like ones ~26:1.** Cross-validated: the isolated
   `ld_dep_stall` figure (11.13%) lands almost exactly on the independently-measured Phase 6
   XMRig-comparison figure (11.41%, measured a different way, on the full mining workload rather
   than this main-program-only isolation) — strong evidence the methodology here is sound, not an
   artifact. `simd_dep_stall` (3.65%) and `other_interlock_stall` (6.74%, execution-stage
   interlocks other than SIMD/FP) are both real and non-trivial too — worth remembering as
   secondary contributors if `ld_dep_stall` alone is ever targeted in isolation.
   **Consequence: this confirms Track D and Track E are pointed at the right problem
   (execution-stage/dependency-chain stalls dominate overwhelmingly) and Track J (code-layout,
   I-cache) is correctly deprioritized — front-end effects are real but under 1% of cycles, not
   worth chasing on their own.** Does not by itself validate any specific Track D/E mechanism will
   pay off — it only rules out redirecting effort toward front-end fixes instead.
3. **NEON cross-domain move latency measurement** *(Sonnet-R2 F3, cheap-to-falsify)* —
   **DONE (2026-07-27), and it did NOT falsify the idea — the opposite of what was expected.**
   Built a three-way AArch64 micro-benchmark (`bench_gpr_chain`/`bench_neon_chain`/
   `bench_cross_chain`, 16×2 = 32 chained same-length instructions per iteration, ×20M iterations,
   pinned via `taskset -c 1`, measured with `perf stat -e cycles,instructions`, each chain a true
   serialized RAW dependency chain an in-order core can't reorder around): a pure-GPR `mov` chain,
   a pure-NEON `fmov d,d` chain, and a GPR↔NEON `fmov d0,x0`/`fmov x0,d0` cross-domain chain
   (confirmed via `objdump` to be the real general-purpose `FMOV` cross-domain encodings, `9e67`/
   `9e66`, not the immediate-move form). **Result: cycle counts were identical across all three
   chains to within 0.004%** (640,116,634 / 640,113,517 / 640,136,097 cycles respectively, at
   680,031,6xx instructions each). On *this specific* Cortex-A53 implementation, a GPR↔NEON
   `FMOV` round-trip costs the same as a same-domain register move — there is no measurable
   cross-domain transfer penalty here, contradicting the generic A53 Software Optimization Guide
   assumption this item was built on (SOG figures are worst-case/typical across A53
   implementations generically, not necessarily this exact core revision `r0p1`). **This does not
   mean Track F3 (NEON-pipe multiply offload for `IMUL_R`/`IMUL_RCP`) is proven to work — it means
   the specific mechanism this item worried about (move overhead eating the gain) is not the
   blocker on this hardware, so F3 is no longer "almost certainly negative" and is worth an actual
   NEON-multiply-vs-scalar-multiply latency comparison next, not dismissal on paper.** That
   follow-up measurement (NEON integer multiply latency vs. `mul`/`umulh`/`smulh`) has not been
   done yet — it's the next cheap step if anyone picks F3 back up. Tool:
   `/tmp/.../reg_move_latency.S` + `reg_move_latency_main.c` (ad hoc, not committed).
4. **Register-liveness check for dual-nonce interleaving** *(Sonnet-R2, gates E1/Item 4 specifically)*
   — **DONE (2026-07-27), definitive negative result, no slack exists.** Built a standalone
   backward-liveness analyzer (links against `armrx_core`, generates real main programs via the
   production `AesGenerator4R` path, classifies each instruction's register reads/writes by
   mirroring `src/vm.cpp`'s `h_*` handlers, then does classic backward dataflow liveness with the
   sink at end-of-program = "all registers live," matching `getFinalResult()`'s real behavior of
   reading every register). 20,000 programs × 256 instructions = 5.12M sample points. **Result: the
   8 integer registers, the 4 `f`-group floats, and the 4 `e`-group floats are simultaneously live
   100.00% of the time, at every single instruction point, with zero variance across all 20,000
   programs.** This is not a measurement artifact — verified directly against `src/vm.cpp`'s opcode
   handlers: every register-writing instruction in the RandomX v1 ISA is compound-assignment style
   (`*ibc.idst += ...`, `-=`, `*=`, `^=`, in-place `sqrt`/byte-swap) — **every write reads its own
   destination first, by construction of the ISA.** There is no instruction anywhere in the set that
   kills a register's value without using it, so backward liveness can structurally never drop below
   8/8 (or 4/4 per float group). **Consequence for Track D**: the optimistic escape hatch Sonnet-R2's
   original writeup left open — "find that RandomX programs rarely use all 8 registers live-
   simultaneously in practice" — is now closed with certainty, not just suspected. This is a
   VM-logical-register finding, not a physical-AArch64-register finding: it says nothing new about
   the JIT's ~12-14-of-31 physical GPR budget per stream (still the open question), but it does mean
   **D3/E1's register-pressure mitigation option (b) is eliminated — spilling (option (a)) is the
   only remaining path if D3 is ever attempted**, raising its effective cost/risk versus how the
   original writeup framed the choice. Does not change D1's viability (D1 doesn't touch the main
   VM program's logical registers at all — it's superscalar-only). Analysis tool:
   `/tmp/.../reg_liveness.cpp` (ad hoc, not committed — self-contained, ~230 lines, rerunnable if
   the opcode frequency table or handler semantics ever change).

---

## Track B — Do less work: hybrid partial dataset *(Gate B passed — JIT crash fixed, hybrid enabled and verified)*

*Opus Item 1 = Deepseek D2; Opus Item 2 / Deepseek B1 as follow-ons.*

**Handoff plan (Gate B/C, ready for an independent agent):**
`docs/plans/track-b-gate-b-plan-20260728.md`.

RandomX's fast/light split is a spec convenience, not a hard requirement — the dataset is a pure
function of the cache, so any precomputed prefix is valid and bit-identical to on-the-fly
derivation. This device has ~950 MiB free while mining. Caching a `B`-byte prefix gives a hit rate
of `B / 2 GiB`; Opus estimates +16% to +32% hashrate at 512-896 MiB cached (treat as directional,
not promised — see gates).

**Why this is unusually safe for a big-ticket item:** it has a **total, cheap correctness oracle**
— `partial[i] == generate_dataset_item(cache, i)` for all `i` in the cached range, and both branches
of the resulting hybrid (fast-mode's direct load, light-mode's derivation) are already independently
KAT-verified. This item chooses between two proven computations; it invents nothing new.

**Implementation** (Opus's sketch, already concrete):
1. `PartialDataset`: `mmap`/`MADV_HUGEPAGE` buffer of `N` items, filled via the existing
   `initialize_dataset()` (`src/dataset.cpp:66`) — already NEON-vectorized and parallelized, reuse
   verbatim with `item_count = N`.
2. JIT emission: in `randomx_program_aarch64_vm_instructions_end_light`
   (`jit_compiler_a64_static.S:528`), a bound check before `bl rx_calc_dataset_item` — hit → direct
   load via `rx_program_xor_with_dataset_line`; miss → existing derivation, unchanged. ~4 extra
   instructions on a ~3,563-instruction path.
3. Incremental fill. **Correction (Gate A result below, measured 2026-07-27): build this from the
   start, not after validating a blocking version** — a 512 MiB fill takes ~3 minutes even with
   correct 8-core parallelism, so a blocking implementation would stall mining for minutes at every
   seed rotation. The bound is re-readable at each of the JIT's 8 per-hash recompiles, so mining can
   start in pure light mode and the threshold can rise as a background fill progresses, at zero
   extra runtime risk. Also **use explicit per-thread `pthread_setaffinity_np` pinning for the fill
   worker threads** (see Gate A's isolcpus finding below) — without it, an `isolcpus`-configured
   deployment silently serializes the fill onto one core, ~4-5× slower than genuine parallelism.
4. CLI: `--dataset-mb=N` (0 = off) plus an `auto` policy sized from `MemAvailable`.

**Gates, in order:**
- **Gate A — init cost. DONE (2026-07-27), decisive: it's minutes, not seconds — incremental fill
  is now mandatory, not optional.** Measured on-device with a standalone harness that replicates
  production's exact call shape (`Argon2dCache::initialize()` then `initialize_dataset()` split
  across N `std::thread`s, one sub-range each, mirroring `mining_engine.cpp`'s fast-mode fallback
  at lines 234-259). **Real numbers, genuinely 8-core-parallel** (see the pinning finding below —
  this required an explicit fix to get true parallelism): 512 MiB (8,388,608 items) —
  `cache_init_ms=7979.9` (~8.0s) + `fill_ms=179393.1` (**179.4s, ~3.0 minutes**) = **~187s
  (~3.1 minutes) total**. Extrapolating linearly (the per-item cost is constant, independent of
  which items are computed): 768 MiB ≈ 4.5 min, 896 MiB ≈ 5.2 min, all fill-only (cache init is a
  fixed ~8s regardless of partial-dataset size). **This triggers the gate as written: the phasing
  in the implementation sketch above ("ship the blocking version first; add incremental fill once
  the win is confirmed") is now wrong — incremental fill (step 3 in that sketch) must be built from
  the start, not deferred, or every seed rotation blocks mining for 3-5+ minutes.** Amortized over a ~2.8-day seed
  rotation this is a negligible ~0.07-0.13% of uptime *if* incremental fill means mining starts
  immediately in pure light mode while the partial dataset fills in the background — but a naive
  blocking implementation would impose a real, user-visible multi-minute stall at every rotation.

  **Related finding, found while chasing down why the first (unpinned) measurement showed 835s
  instead of ~180s: a real latent gap in production's own fast-mode dataset-init fallback code,
  not just an artifact of the ad hoc test harness.** Under this device's `isolcpus=1-7` config, 8
  plain `std::thread`s spawned with only a process-level `taskset -c 0-7` (no per-thread affinity
  call) all landed on core 0 — confirmed via `/proc/<tid>/stat` field 39 (`psr`) showing all 9
  threads (main + 8 workers) at `psr=0`. This is the same isolcpus scheduler-placement gotcha
  `docs/experiments/isolcpus-rt-priority-win.md`'s "Second footgun" and `PLAN.md` Phase 12's
  auto-repin watcher already documented for *build* jobs — the kernel doesn't proactively spread
  inherited-affinity threads across isolated cores even when the mask allows it. **The exact same
  pattern exists in `mining_engine.cpp`'s fast-mode dataset-init fallback (lines 234-259,
  `init_threads.emplace_back([...]{ initialize_dataset(...); })` — plain `std::thread`, no
  `pthread_setaffinity_np`), unlike `worker_loop()` elsewhere in the same file, which explicitly
  pins every persistent worker thread (lines 318-335).** This path is currently dormant (only
  reachable in fast mode, and this device is forced into light mode), so it isn't live today — but
  Track B's own fill code, if it reuses `initialize_dataset()` the way the implementation sketch
  above says to ("reuse verbatim"), needs to add explicit per-thread pinning from day one, or it
  will silently serialize onto one core on any `isolcpus`-configured deployment and produce
  wall-clock numbers ~4-5× worse than genuine 8-core parallelism (835s vs. 179s observed here —
  close to, though not exactly, the naive 8-9x oversubscription ratio, plausibly reduced somewhat
  by this device's fast/slow cluster asymmetry). Fix demonstrated and verified: adding
  `pthread_setaffinity_np(pthread_self(), ..., CPU_SET(i, ...))` at the top of each worker thread
  (mirroring `worker_loop()`'s own `AffinityMode::All` pattern) spread the 8 threads cleanly across
  `psr=0..7`, one each, confirmed live before trusting the timing result.
- **Gate B — the memory-path caveat (real, not theoretical).** This device's known 8-worker
  bottleneck is shared memory-path arbitration between the two L2 clusters. This item trades ALU
  work for random DRAM traffic (~2.5M extra 64-byte reads/sec aggregate at 50% hit rate) — exactly
  the contended resource. **A single-core `taskset` measurement will overstate the win; measure at
  1 worker AND 8 workers, and let the 8-worker number decide.**
- **Gate C — memory pressure.** No swap on this device. Size conservatively; watch for OOM-killer
  activity over a multi-hour run; re-check `MemAvailable` while mining, not idle.
- **TLB.** 768 MiB of random access needs THP or this could be page-walk-dominated. Verify via
  `smaps` (the Argon2 cache precedent is good, but its working set is smaller).

**Follow-on, contingent on Track B landing — asymmetric per-cluster memory mode**
*(Opus Item 2)*: cluster 0 already wins interconnect arbitration (~4.26 H/s) — give it the
memory-heavy partial-dataset path; keep cluster 1 on pure ALU-heavy derivation, since its ALUs sit
idle waiting on the bus anyway. Hours of effort once Track B is measured; per-worker boolean
threaded into VM construction.

**Related but distinct — per-cluster cache *replication*** *(Deepseek B1)*: instead of one shared
256 MiB Argon2 cache, give cluster 1 (cores 4-7) its own physical 256 MiB copy, eliminating
interconnect arbitration on cache reads specifically (as opposed to Track B's dataset reads).
~1-2 days, no correctness risk (identical cache data, just duplicated), ~12.5% aggregate estimated
*if* the slow-cluster penalty is interconnect-bound rather than memory-controller-bound — genuinely
unknown until measured. Independent of Track B; can run in parallel or be tried first since it's
cheaper. If Track B's Gate B measurement shows severe cross-cluster memory contention, B1 is worth
promoting ahead of Track B's main item as a smaller, faster test of the same underlying hypothesis
(is the slow cluster's penalty interconnect- or controller-bound).

---

## Track C — Structural ABI/call overhead: inline the light-mode dataset-item helper

*Hermes Item 2. Traces to an abandoned 2026-07-19 handoff priority, never executed. Independent of
Track B — orthogonal axis (reduces the cost of every derivation call rather than reducing the
number of calls) and composes with it.*

**Handoff plan (Phase A retry bisection, ready for an independent agent):**
`docs/plans/track-c-phase-a-bisection-plan-20260728.md`. Read alongside
`docs/experiments/light-mode-dataset-item-prologue-attempt.md` (the prior attempt's full account).

Per light-mode iteration (16,384×/hash: 2048 iterations × 8 programs), the current path pays a
`bl`/`ret`, a 96-byte outer frame, a 112-byte inner frame, 14 saved+restored GPRs (x0-x13, seven
`stp` pairs), a 64-byte store to a temp buffer, then an immediate 64-byte reload of that same
buffer to XOR into VM registers — a full round-trip the data never needed to make. **Line numbers
corrected against source (verified 2026-07-27; the plan originally cited stale ones):** caller
side is `randomx_program_aarch64_vm_instructions_end_light` at
`jit_compiler_a64_static.S:528` (not 504), which saves x0/x1/x2/x30 to a 96-byte frame (529-531),
calls `bl rx_calc_dataset_item` (560), restores (562-564), and branches to
`rx_program_xor_with_dataset_line` (567) — whose body is at **358-369** (not 338-349; 338-349 is
the dataset-prefetch region, a different block). The helper's own 112-byte frame/14-register save
is at 848-855; it stores the derived 64-byte item to the caller's frame at **931-934** and reloads
its own saved registers at **936-942**. The item is computed in x0-x7 and could be XORed
**directly** into the live VM registers.

**Phased, each independently revertible:**
- Phase A — remove duplicate register preservation. **Attempted 2026-07-27, hung, reverted —
  see `docs/experiments/light-mode-dataset-item-prologue-attempt.md` for the full account,
  including the complete code changes for reference.** Designed and implemented a conservative
  version (drop x0-x3 preservation only, keep x4-x13) as a new, separate light-mode-only static
  entry point (not touching the shared/general-purpose function at all, which turned out to also
  be used by dead fast-mode code — `getDatasetInitFunc()` has zero call sites). Verified
  byte-correct via both static (`objdump`/`nm` on the compiled binary) and dynamic (disassembling
  the actual live JIT buffer's runtime-constructed bytes via `/proc/<pid>/mem`) inspection —
  every instruction matched the design exactly. Despite that, the full system hung on the very
  first JIT-mode hash (confirmed via `/proc/<pid>/stat` as genuinely spinning, not deadlocked;
  `perf record` sampling showed execution spread across both the main VM loop and the derivation
  region, not concentrated at one instruction — more consistent with a data-corruption-driven
  blowup, e.g. wrong values reaching a `CBRANCH` and causing far more re-execution than the
  spec's ~0.4% branch-taken rate, than a tight infinite loop). Root cause not identified despite
  the verification above; reverted per this project's established discipline for exactly this
  failure class (same call made for the memory-op scheduler attempt). **Before attempting this
  again: the experiment doc's "where this leaves future work" section has a concrete next
  diagnostic step (diff the derivation's actual computed `rl[0..7]` values against
  `generate_dataset_item()`'s reference output for fixed inputs, to test the "wrong data, not
  wrong instructions" hypothesis directly) — don't just re-verify the instruction sequence again,
  that was already done exhaustively and found nothing.**
- Phase B — direct result mixing (skip the store/reload relay entirely). **Not attempted; also
  turns out to be substantially harder than originally scoped** — found during Phase A's design
  work that the superscalar computation's working registers (`rl[0..7]`) are hard-wired to the
  same physical registers (x0-x7) as some of the caller's live VM state, so "leave the result in
  registers for direct XOR" as originally described isn't mechanically possible without first
  re-mapping the derivation's working-register set to genuinely free registers — a bigger,
  separate design problem than Phase A turned out to be, not a natural follow-on once Phase A
  works. Do not attempt this without a dedicated register-liveness analysis across the *exact*
  call site first.
- Phase C — only then consider inlining the `bl` away (this is what Track E's "monolithic JIT"
  extends to). **Contingent on Phase A actually landing — currently blocked by Phase A's revert.**

**Why it's different from everything IPC-framing closed:** this attacks call/ABI overhead and
redundant memory traffic, not instruction-level stalls — it reduces raw instruction count (the axis
§0 says is the real XMRig gap) *and* removes latency the scheduler structurally can't hide because
it's outside the JIT body (across a `bl`).

**Correctness risk: high** — this is the JIT's most safety-critical path; a register-contract bug
here is a silent wrong hash. Mitigate with full JIT+interpreted KATs per phase and a new
differential test comparing light-mode dataset-item results before/after across many deterministic
indices. Treat each phase as a separate, individually-revertible landing.

**Effort:** ~3-5 days phased. **Expected payoff:** hypothesized 3-8% (never measured, because never
built) — potentially the largest single win in this entire combined backlog, on the axis that
matters most per §0. **Gate:** if instruction count drops but hashrate regresses, that's a hidden
hazard signal — stop and investigate, don't push through.

---

## Track D — Concurrency: filling in-order pipeline bubbles with a second, independent hash

*Merged from Opus Items 3-4 and Sonnet-R2 Category E, cheapest-to-most-expensive.*

The shared insight: the main VM program's dependency chain is ~94% architecturally serial (proven,
§1) — nothing *within* one hash's chain can fill its own stalls. But two different nonces are
**already fully independent** with zero new hazard analysis required *between* them (only within
each, which is already verified). Interleaving two streams at emission time is "poor-man's SMT,"
done by the compiler instead of hardware.

**D1 — 2-way interleaved superscalar dataset-item derivation** *(Opus Item 3, do this first — cheapest, best-understood)*.
**🍅 CONCLUDED (2026-07-29): NEGATIVE.** See `docs/experiments/track-d1-bench-1way-hang-status.md`.
The 2-way path causes ~66× more L1I refills (0.023% → 1.51% miss rate)
with a −1.2% IPC regression on this Cortex-A53. The I-cache locality
tradeoff is worse, not better, for interleaved emission here.
**D3 is contraindicated** by this result — if D1 can't clear this bar,
the much larger full-VM interleave won't either.

**D2 — Cross-hash boundary-only pipelining** *(Sonnet-R2 E2, the fallback if D3's liveness check is
bad)*. Overlap only the *tail* of hash N (AES finalization/result compression — short, fixed,
low-register-pressure) with the *head* of hash N+1 (scratchpad fill from blake2b — also short,
load/store-heavy, no VM register-file dependency). Much smaller ceiling than D3 (a few percent of
the main-program region's cycle share) but much smaller register-pressure risk, since neither
touched sequence uses the RandomX integer/float register file. ~3-5 days, low-medium risk.

**D3 — Full dual-nonce interleaved JIT emission (the moonshot)** *(Opus Item 4 = Sonnet-R2 E1)*.
Interleave the *entire* native instruction stream of two hashes, instruction-for-instruction or in
small groups, each stream's internal order left untouched (still whatever the verified scheduler
produced) — correctness reduces to "no register/memory collision between streams" (checkable
exhaustively) rather than a semantic reordering proof (the class of proof that broke the memory-op
scheduler). Register pressure is the central open question: one stream already occupies ~12-14 of
31 GPRs; two need ~24-28, leaving little room for loop/base-pointer bookkeeping. **Track A item 4
(the liveness check) is done (2026-07-27): definitive negative result.** The VM-logical 8 integer
registers (and all 8 float destination registers) are live 100.00% of the time at every instruction
point, with zero exceptions across 20,000 sampled programs — a direct consequence of RandomX v1's
accumulator-style ISA (every register-writing opcode reads its own destination first, so no
instruction ever kills a value without using it). **This closes off the optimistic mitigation this
item originally listed as option (b) ("find that RandomX programs rarely use all 8 registers live-
simultaneously") — there is no such slack, anywhere, by construction.** Only option (a) remains:
explicit register spilling to a small stack frame for stream B between uses, paying real load/store
cost on top of the interleave. This doesn't kill D3 outright (spilling was always the fallback), but
it removes the free-lunch case and should raise this item's effort/risk estimate accordingly — go in
expecting spill cost, not hoping to avoid it. If it's still pursued, ceiling
is "up to most of the main VM program's 20% cycle share," likely much less once register-spill and
doubled memory-bandwidth-per-worker costs are counted (this could just move the bottleneck from ALU
stalls to memory bandwidth, mirroring Track B's Gate B risk, self-inflicted instead of
interconnect-inflicted). Effort: 1-2 weeks, the largest single item across the combined backlog.
**D1 is now measured (2026-07-29): clean negative result.** The 2-way superscalar
interleave causes ~66× more L1I refills (−1.2% IPC) — see
`docs/experiments/track-d1-bench-1way-hang-status.md`. **D3 is contraindicated
by this result.** If the cheap, simple superscalar interleave can't clear the
L1I bar on this core, the much larger full-VM interleave (with additional
register-spill cost) will not either.

---

## Track E — Is the scheduler actually at its ceiling? *(gated on Track A item 2)*

*Sonnet-R2 Category F, minus F1/F3 which live in Track A as diagnostics.*

**F2 — exact A53 dual-issue-slot scheduler** *(Sonnet-R2)*.
**Handoff plan, ready for an independent agent (gate now cleared — Track A item 2 confirmed
back-end/dependency attribution):** `docs/plans/track-e-f2-dual-issue-scheduler-plan-20260728.md`.
The current emitter scheduler reorders
on a register-hazard model only — it doesn't know which specific instruction-type pairs can actually
co-issue on the A53's two pipes (branch+simple-ALU vs ALU+MUL+DIV+NEON, per the A53 Software
Optimization Guide). A hazard-clean reorder can still be issue-slot-suboptimal. Building a real
list scheduler with a pipe-occupancy model is strictly a scheduling change (same hazard-safety net
as today) — low correctness risk, real effort risk (1-2 weeks; getting dual-issue pairing rules
wrong risks a *slower* schedule, not a wrong one). **Only pursue if Track A item 2 (PMU breakdown)
confirms back-end/dependency attribution** — if it's front-end-bound instead, this effort is pointed
at the wrong problem.

**D9 / conservative load hoisting retry** *(Deepseek D9)*. The memory-op scheduler extension was
tried and reverted for an unexplained JIT/interpreter divergence. A narrower retry — widen the
hazard-check window from 2 to 5 instructions, or use the existing `computeFootprint()` liveness
tracking to hoist only loads whose consumer is provably far enough away — might avoid whatever the
first attempt missed. Medium risk (same failure class as the original attempt); only worth it if F2
above shows real remaining slack, since both compete for the same "is there schedulable room left"
budget. Ceiling bounded by the same ~6% latency-recoverable gap Step 1 already found.

**Escalation — full dependency-graph list scheduler** *(MidHigh Tier 3, item 4, never started)*.
F2 above still keeps the existing fixed-window heuristic and just teaches it about A53 issue-slot
pairing. This item goes further: throw the heuristic away and replace it with genuine list
scheduling over the main VM program's *full* per-program dependency graph — the "textbook correct"
answer to the scratchpad-locality experiment's architectural finding, rather than a bounded-window
approximation of it. High effort (amounts to building a compiler-backend scheduler from scratch),
high correctness risk (silent wrong hashes), and per the source doc would need review rigor
matching or exceeding the *original* emitter scheduler's three independent audits before being
trusted. **Not worth starting before F2 is measured** — F2 answers essentially the same question
(is there schedulable room the current heuristic is leaving on the table) far more cheaply; only
escalate to a full graph scheduler if F2 finds real remaining slack that a wider-but-still-windowed
model can't capture.

**Escalation — register-allocation restructuring in the main VM program's JIT** *(MidHigh Tier 3,
item 5, never started)*. Distinct axis from both F2 and the list-scheduler escalation above:
instead of reordering *emission*, reduce false WAW/WAR dependencies between virtual registers by
allocating more physical registers per virtual register in the first place, so the scheduler (of
whichever kind) has fewer artificial serialization points to work around. Plausible given the main
VM program's register pressure (§ Track D's liveness-check discussion notes ~12-14 of 31 GPRs
already committed in a single stream), but genuinely unexplored — no concrete measurement yet
isolates false-dependency-driven serialization as *the* bottleneck specifically, as opposed to true
dependency-chain latency (which the 94%-architectural finding already attributes most of the
penalty to). Speculative; park behind Track A's diagnostics and the two items above rather than
prioritizing it on its own.

---

## Track F — Instruction-count-driven micro-optimization *(re-opened under §0's reframing; gated on Track A item 1)*

*Hermes Items 3 and 6. This explicitly re-litigates ideas closed under the IPC framing — legitimate
per §0, because instruction count is a different axis from IPC and was never separately screened.*

- **Fusion/peephole, re-scored.** `docs/plans/peephole-jit-plan.md` and the 2025-07-25 closure were
  right that the main-program region isn't stall-dominated — but that's an IPC-axis conclusion, not
  an instruction-count one. Re-run candidate search using Track A item 1's budget table as the
  source, and **gate purely on `instructions/hash` delta**, not IPC. Adopt only if instruction count
  drops *and* hashrate holds or rises. **Partial result (2026-07-27, direct source reading of
  `src/jit_compiler_a64.cpp`, no device needed): the specific example this item originally named —
  "an `IADD_RS` shift+add the emitter currently splits" — is factually wrong, both in the main
  program's `h_IADD_RS` (line 1322) and the superscalar path (line 1101). Both already emit a single
  fused `add dst, dst, src, lsl #shift` — AArch64's shifted-register add form, not a split
  shift-then-add. There is no fusion opportunity here; withdraw this example.** The other named
  example, `ISTORE`+`IADD_M` address-computation sharing, is also checked now: `h_ISTORE`
  (line 1904) and `h_IADD_M` (via `emitMemLoad`, line 1261) both already use the minimal 3-instruction
  shape for their own address (`add`-immediate + mask + load-or-store), and structurally there is
  nothing to *share* between two adjacent instances — `ISTORE`'s address comes from its own `dst`
  register and immediate, `IADD_M`'s from its own independently-random `dst`/`src`/immediate, with
  no guaranteed relationship between them. Two independently-random-field instructions coincidentally
  addressing the same base+offset would be rare in real generated programs — the same shape of
  reasoning that closed the `IXOR_C*` logical-immediate idea (0 of 20,000 real cases applicable).
  Not exhaustively quantified the way that case was (no 20,000-sample check run here), so treat as
  "likely a dead end, not fully closed" rather than fully closed — but not a promising lead either.
- **Multiply-width reduction in the superscalar path — largely re-treads already-closed ground,
  verified 2026-07-27.** Read every candidate opcode's actual emission directly (no `--jit-dump`
  needed; the emitter's code *is* the ground truth for what it emits):
  - `IMULH_R`/`ISMULH_R`, **both** the main-program handlers (`h_IMULH_R`/`h_ISMULH_R`,
    `jit_compiler_a64.cpp:1441,1472`) and the superscalar path's inline switch
    (`generateSuperscalarHash()`, lines 1121-1126): **already minimal — a single `umulh`/`smulh`,
    nothing to trim.** AArch64 has no cheaper way to get a 128-bit product's high half. This item's
    premise (non-minimal emission) is false for these two opcodes.
  - `IMUL_RCP`, **main program** (`h_IMUL_RCP`, line 1503): already has a smart register-
    pre-assignment fast path — the first 12 distinct divisors per compile get a dedicated
    pre-loaded literal register (1 `mul` instruction, no load), only the 13th+ falls back to
    `ldr`+`mul` (2 instructions). Given `IMUL_RCP`'s frequency (8/256 ≈ 3.1%) rarely exceeds 12
    distinct divisors in one 256-instruction program, the 1-instruction fast path dominates in
    practice. Already good; not a fresh candidate.
  - `IMUL_RCP`, **superscalar path** (line 1127-1139): unconditionally `ldr` (literal load) +
    `mul` — 2 instructions, no pre-assignment fast path. **This is not a new finding — it is
    exactly the ground two already-closed experiments covered**: "`IMUL_RCP` literal-load
    elimination" (replacing the `ldr`+`mul` with direct immediate materialization, implemented,
    measured, reverted at −0.3% net — see the "do not repeat" list) and "`IMUL_RCP` register
    pre-assignment" (extending the main program's own working <12-slot trick to this exact
    superscalar site, root-caused and closed-for-now at a safe budget of ≤1 register — see the
    same list, revisit path in `mid-high-risk-performance-ideas-20260726.md` §1). **Re-labeling
    this as a fresh Track F candidate would send an executor down a path already walked twice.**
  - `IXOR_C7`/`C8`/`C9` (superscalar, line 1115-1119): 2 instructions (`movImmediate`+`eor`) — also
    not fresh; already closed by the standalone logical-immediate-encoder result (0 of 20,000 real
    immediates encodable as a single AArch64 logical immediate, see "do not repeat" list). The
    2-instruction cost here is verified-necessary, not waste.
  - **Net: every specific opcode this item named has already been checked, and none of them yields
    a new instruction-count-reduction opportunity — either because the emitter is already minimal,
    or because the gap is real but has already been tried and closed twice.** This doesn't mean
    Track F is dead — only ~7 of the ISA's ~30+ opcodes (main + superscalar combined) have been
    checked this way. It does mean this item's *original* framing (treating these as unexamined
    candidates) was wrong, and the remaining search space is genuinely unexamined opcodes, not
    these ones. Whoever picks this up next should start from Track A item 1's full audit table
    once built, not from this item's original examples.

Both items are contingent on Track A item 1's output — there is no full candidate list without it,
though the partial manual check above already removed the specific examples originally cited.

---

## Track G — NEON T-table AES vectorization *(independent, can run in parallel once flagged)*

*Hermes Item 4.*
**Handoff plan, ready for an independent agent:** `docs/plans/track-g-neon-ttable-aes-plan-20260728.md`.
`hash_aes_1r_x4`/`fill_aes_1r_x4` cost ~12.3% of all cycles — the single biggest
named C++ cost. The hardware AESE/AESD path is spec-incompatible (wrong AddRoundKey order,
previously removed) and the tried `vtbl`/vector-permute NEON AES measured **-19.4%** and is
flag-gated off (`docs/experiments/neon-vector-permute-aes.md` — read before touching this again).

**The untried variant:** keep the scalar T-table *algorithm* exactly as-is (bit-identical by
construction), but vectorize the table *lookups* — `hash_aes_1r_x4` already processes 4 independent
16-byte blocks concurrently, so NEON `tbl`/`tbx` can gather across 4 lanes at once instead of 1
scalar lookup at a time, with XOR-accumulate in NEON registers. This is a throughput change to the
gather width, not a round-structure change — different mechanism from the failed attempt, different
risk profile.

**Correctness risk: medium** (bit-exactness mandatory; existing golden-pin tests
(`tests/test_aes_hash.cpp`) and the hash/fill decomposition-equivalence check must stay green).
Ship behind a new `ARMRX_ENABLE_NEON_TTABLE_AES` flag, default OFF, never on the hot path until
verified. Effort: ~2-4 days. Potentially the largest single-target upside in the whole backlog
(12.3% of cycles) or null — must be benchmarked, not assumed given the sibling attempt's outcome.

---

## Track H — Alternative execution-model experiments *(exploratory, lower priority than B/C/D)*

- **Hybrid JIT/interpreter for the main VM program only** *(Deepseek A1)*. Keep JIT for the
  superscalar path (72.71% of instructions, already efficient at 1.145× IPC); run the main VM
  program (9.23% of instructions, 0.461× IPC) through the interpreter instead, on the theory that
  the interpreter's per-opcode dispatch naturally inserts pipeline bubbles the tight JIT sequence
  doesn't. Real risk: the interpreter's `compile_instruction()` re-expands per hash (JIT compiles
  once, runs 2048×) — recompilation cost could eat any IPC gain, the same trap that would sink
  several other items here. ~2-3 days to prototype (`force_interpreted_main` flag,
  `bench_armrx` full-hash comparison). Low-medium risk — both halves are individually
  KAT-verified already.
- **Superscalar/main-program dual-issue interleaving via separate code buffers** *(Deepseek A3)*.
  Largely superseded by Track D's cleaner mechanism (same-hash-pair interleaving needs no
  cross-register-file/cross-memory-region isolation the way two *different* logical programs on one
  core would). Listed for completeness; do not pursue unless Track D fails and this offers a
  meaningfully different risk/reward — as specified it's weeks-to-months effort with very high
  correctness risk (register file isolation, memory isolation, interrupt handling across two logical
  programs on one core) for an unknown payoff.

---

## Track I — Operational: worker/main-thread cost on core 0 *(the one confirmed-open non-JIT item)*

*Opus Item 6 = Hermes Item 7, deduplicated.*
**Handoff plan, ready for an independent agent:** `docs/plans/track-i-core0-cost-plan-20260728.md`.
Under `isolcpus=1-7`, `detect_core_order()` has no
`cpufreq` sysfs data to work from and falls back to sequential `[0..7]` placement, landing worker 0
on core 0 — the only unisolated core, which also hosts the stratum reader, JSON/job handling, and
the per-second console print. This costs worker 0 real throughput under actual pool mining
(sustained ~24.76 H/s vs. the 28.4 H/s burst figure). Removing worker 0 entirely is confirmed
*wrong* (nets -0.6 H/s). The unmeasured lever: reduce the main thread's *own* cost on that shared
core — batch/throttle the per-second render, move JSON parsing off the critical path, check whether
metrics/TUI threads also land on core 0. Worth an hour of `perf` on the main thread before designing
anything. Not a JIT change; can land independently, any time, in parallel with everything else.

---

## Track J — Cheap layout tweak *(low priority, measure-if-curious)*

**JIT buffer hot/cold reordering** *(Deepseek V4)*: reorder emission so hot superscalar code is
I-cache-line-contiguous and the cold, once-per-hash main program trails after. Current I-cache miss
rate is already 0.788% (cheap on A53, ~1-2 cycle penalty), so expected effect is small (~1% at
most). ~1 day, no correctness risk (layout only). Worth doing if Track C's Phase C /
"monolithic JIT" idea (below) is ever pursued, since that's the same lever at larger scale.

**Note — monolithic JIT (Track C's natural extension, not separately tracked):** Hermes's Item 5
("compile main program + 8 superscalar programs + the loop into ONE routine, no `bl` at all")
is explicitly framed as **Track C Phase C's natural evolution**, not a separate item — attempt it
only after Track C's Phase A/B show the ABI plumbing was the dominant cost and the `bl` itself is
next. Code size grows to ~160+ KiB against 16 KiB L1I; whether that regresses depends on whether
the *current* split is already I-cache-bound (it isn't, per the 0.788% figure) or whether removing
the call/return trampoline improves locality enough to offset it. High correctness risk (whole-hash
codegen rework), 1-2 weeks, gate on `l1i_cache_refill` + `instructions/hash` both improving.

**BOLT (post-link profile-guided binary layout)** *(MidHigh Tier 2, item 3 — genuinely never
attempted, not merely predicted-and-closed).* Uses real `perf`-recorded profiles, unlike PGO's
compile-time instrumentation, to reorder hot functions/basic blocks for I-cache locality. The
MidHigh doc's own assessment is that this is **expected** to replicate PGO's null result — I-cache
miss rate is already 0.788%, so there isn't much layout locality left to win back — but that's a
prediction, not a measurement, and it has genuinely never been run. Worth a quick try only if
someone wants to close the question definitively rather than leave it as an untested assumption;
not because any evidence currently points at a real gap here. Same low-priority bucket as the JIT
buffer hot/cold reordering above — both are cheap, correctness-risk-free, and last in line.

---

## Do NOT repeat — closed on this project's own evidence (consolidated across all four docs)

- CSEL branchless CBRANCH — +46% branch misses, reverted.
- Newton-Raphson FDIV/FSQRT — -1.1% / net regression, spec risk. Frozen.
- NEON hardware AES (AESE/AESD) — spec-incompatible AddRoundKey ordering, removed.
- NEON `vtbl` vector-permute AES / fused hash+fill — measured -19.4%, flag-gated off. (Track G's
  T-table-gather idea is a *different* mechanism and is not this.)
- Superscalar literal-pool relayout — -1.12% hashrate, reverted.
- `IMUL_RCP` literal-load elimination — +8.2% IPC, +8.5% instructions, net -0.3%.
- `IMUL_RCP` register pre-assignment — correctness failure (`test_jit_equivalence` divergence);
  safe budget ≤1 register, payoff too small. Revisit path recorded in
  `mid-high-risk-performance-ideas-20260726.md` §1 if anyone wants to reopen it.
- `IXOR_C*` logical-immediate encoding — 0 of 20,000 real immediates encodable.
- Peephole JIT coalescing **under the IPC framing** — closed on evidence there. **Reopened under the
  instruction-count framing as Track F**; that reopening is deliberate, not an oversight.
- Memory-op scheduler extension (to `*_M` opcodes) — reverted, unexplained JIT/interpreter
  divergence. (Track E's D9 is a narrower, gated retry, not a blind repeat.)
- Prefetch insertion — closed by the scratchpad-locality experiment's 6% ceiling.
- PGO — null twice. Re-measure only after a substantial binary reshape (e.g. the monolithic-JIT
  idea in Track J, if ever attempted).
- Worker-count sweep — 8 workers already confirmed as the highest-throughput choice; do not
  re-sweep without new evidence.
- Fast-cluster-only mining with frequency boost — dead end; cluster frequency is independent of the
  other cluster's load on this SoC.
- `-moutline-atomics` / LSE atomics — Cortex-A53 has no `lse` in its HWCAP; dead end.
- Custom linker script for JIT code placement — already analyzed as part of the JIT ABI review; the
  JIT's `BL`s are already within-buffer and in range. No gain.
- 1 GiB hugetlbfs backing — dTLB misses are already negligible (~1.6/million instructions) at the
  existing 2 MiB THP level; eliminating an already-negligible miss rate is itself negligible.
- Early-abort on partial hash vs. difficulty target — **not just impractical, cryptographically
  impossible.** RandomX's AES+Blake2b finalization is specifically designed so no bit of the output
  is predictable from any strict subset of final VM state without completing finalization; a
  "partial statistic" would be a break of the hash construction's avalanche property, not a mining
  optimization. Recorded so nobody re-derives this.
- Two-machine/RDMA dataset hosting, GPU (Adreno/OpenCL) offload, custom kernel module for JIT buffer
  placement, non-uniform per-cluster nonce-space width, lock-free inter-worker scratchpad sharing,
  dynamic light→fast switch at seed rotation (dataset init ~30 min vs. seed rotation ~seconds-to-
  minutes) — all dead ends for concrete, hardware- or spec-level reasons documented in
  `hail-mary-ideas-20260727.md` Category D. Not worth re-reading unless the underlying hardware
  changes.

---

## 3. Recommended sequence

```
Track A (diagnostics — items 1-4, all cheap, run first, mostly parallel)
  │
  ├─ item 2 (PMU frontend/backend) ─┬─→ gates Track E (F2, D9)
  │                                  └─→ if front-end-bound, redirect toward Track J instead
  │
  ├─ item 4 (register liveness) ────→ gates Track D's expensive end (D3)
  │
  └─ item 1 (instruction budget) ───→ gates Track F entirely (both sub-items)

Track B (partial dataset)              ── Gate A → Gate B (8-worker!) → Gate C
  │                                          │
  │                                          └─→ Track B follow-ons (asymmetric clusters / B1 cache replication)
  │
Track C (inline dataset-item helper)   ── Phase A → Phase B → (Phase C only if A/B show `bl` is next)
  │                                                                  │
  │                                                                  └─→ Track J's monolithic-JIT note
Track D1 (cheap 2-way superscalar interleave) ── 🍅 CONCLUDED: NEGATIVE (66× more L1I refills, −1.2% IPC)
  │
  └─→ D3 contraindicated by this result — see `docs/experiments/track-d1-bench-1way-hang-status.md`

Track D2 (boundary pipelining)           ── depends on nothing above beyond accepting D1's result

Track G (NEON T-table AES gather)      ── independent, start once flag scaffold exists
Track I (worker/core-0 cost)           ── independent, operational, land anytime
Track E, F, H, J                       ── each gated as noted above; lowest scheduling priority
  └─ Track E's list-scheduler / register-allocation escalations (MidHigh Tier 3) ── gated
     behind F2 specifically, not just Track A item 2 — do not promote these ahead of F2's result
```

**Suggested order of attack**, folding priority and dependency together:

1. **Track A, all four items — DONE (2026-07-27).** Results: Track D3's register-slack hope is
   closed (item 4, 100% liveness, no slack exists); front-end stalls confirmed negligible vs.
   back-end (item 2, ~26:1), so Track D/E are pointed at the right problem and Track J correctly
   stays low-priority; NEON cross-domain move cost is confirmed cheap on this hardware, reopening
   rather than closing Track F3 (item 3); and the opcode-emission-waste hypothesis behind Track F
   is now fairly thoroughly falsified (item 1, essentially every high-frequency opcode checked is
   already minimal or architecturally forced) — **Track F should not be prioritized without a
   specific new opcode surfacing a real gap; none has.**
2. **Track B (partial dataset)** — highest expected value of any single item (+16-32% *estimated*,
   caveated hard by Gate B), lowest correctness risk of any big-ticket item here (total/cheap
   oracle). **Gate A is DONE**: ~187s (~3 min) for a 512 MiB fill, confirmed genuinely
   8-core-parallel — flips the implementation phasing to build incremental fill from day one, not
   defer it. Gate B (the 8-worker memory-contention decision) is next and requires actual JIT
   implementation work, not just measurement.
3. **Track C, Phase A — attempted 2026-07-27, hung, reverted; currently blocked.** Still
   conceptually the strongest concrete lead on the instruction-count axis (reinforced by Track A
   item 1's finding that per-opcode emission is already near-minimal everywhere checked, so
   overhead *outside* individual opcode emission is the more promising remaining place to look) —
   but the first implementation attempt failed for a reason not yet identified despite thorough
   static and dynamic verification. See `docs/experiments/light-mode-dataset-item-prologue-
   attempt.md` before trying again; it has a concrete next diagnostic step (compare actual
   computed values against the reference implementation, not another instruction-sequence review).
   Do not resume this track without following that lead first. Phase B is *also* harder than
   originally scoped (needs a register remapping design, not just "skip the store") — see the
   Track C section above.
4. **Track D1 — 🍅 CONCLUDED: NEGATIVE** (2026-07-29, see above). The cheap 2-way superscalar interleave causes ~66× more L1I refills (−1.2% IPC) on this core — clean negative result.
5. **Track G (NEON T-table AES)** — independent axis, can be developed in parallel with any of the
   above once a flag scaffold exists; targets the single largest named C++ cost (12.3% of cycles).
6. **Track F, Track E, Track D2/D3, Track H, Track J** — in roughly that order, each strictly gated
   on its diagnostic prerequisite from Track A or on an earlier track's measured result. Track F
   specifically should stay deprioritized per item 1's result above unless new evidence surfaces.
7. **Track I (core-0 cost)** — independent, operational, no dependency on anything above; land
   whenever convenient.

---

## 4. Measurement discipline (consolidated, unchanged from prior sessions, and it has already caught real errors)

- `taskset`-pin both sides of every A/B — an unpinned comparison already produced a false "PGO wins
  2×" read on this device (`bench_armrx needs core pinning`, prior session).
- `perf stat -e cycles,instructions` over wall-clock hashrate; wall-clock has already been shown
  insufficiently sensitive at these effect sizes.
- Reverse trial order at least once per A/B to rule out thermal drift — this device has real,
  measured thermal variance between runs.
- Distinguish burst from sustained: cores 4-7 read 2.84 H/s at `--warmup=15 --seconds=60` and
  2.13 H/s at `--warmup=60 --seconds=180`. **Always use the long window.**
- **For Track B specifically: the 8-worker number decides, not the 1-worker number.** The entire
  risk of that item lives in cross-cluster memory contention a single-core measurement cannot see —
  this is the same class of mistake as the historical false "PGO wins" read, just on a different
  axis (memory contention instead of core-cluster placement).
- For any item gated on "instruction count," measure `instructions/hash` directly (`--jit-dump` /
  `bench_armrx`), not IPC — per §0, these are different axes and conflating them is exactly the
  mistake this synthesis exists to correct.
