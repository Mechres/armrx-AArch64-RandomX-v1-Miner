# armrx — Next-Iteration Experiment Plan (living doc)

> **This is NOT a roadmap to "done."** It is an open experiment backlog. Each entry is a
> question we can answer with a measurement. We run one, write the result to
> `docs/measurements/performance-log.md`, update our belief, and pick the next. The point is
> to keep narrowing where the cycles go — not to close tickets.
>
> **Frontier shift (2026-08-03):** the *instruction-count* hypothesis is now **falsified by our
> own data**. With hardware AES in (`2687a2e`) armrx emits **89.5M instr/hash — fewer than
> XMRig's 94.5M and the BSD ref's 104.8M**. Yet we run **4.75 H/s vs XMRig 5.05** (~6% behind)
> because our **IPC is 0.551 vs XMRig 0.648**. The remaining gap is **cycle efficiency, not
> instruction count.** Every "density" experiment is now low-value; every "where do our cycles
> go / can we hide more latency" experiment is now the frontier.

## How we measure (lock this so iterations are comparable)
- Device: Cortex-A53 @ fixed 765 MHz (no cpufreq), non-isolated unless noted.
- Single-core: `taskset -c 3`. Multi-core: state the pin / isolcpus status.
- Gated: `bench_armrx --full-hash-only --perf-ready` + `perf stat -e cycles,instructions`,
  500-hash window, md5 of the executed binary recorded. Live H/s also read from a plain
  `taskset -c 3 ./bench_armrx --full-hash-only` run (median ms → H/s).
- Host x86_64 ctest 9/9 must stay green (hw branch compiled out there).
- Every iteration appends ONE row to `docs/measurements/performance-log.md`.

## E1 — Re-run region attribution on the current build  [DONE 2026-08-03 — time-based; chain = 86.6%]
- **Question:** What is the *real* post-AES per-region split? (Prior decomposition was pre-AES arithmetic.)
- **Result (device, post-AES + hugepages, core 3, `bench_armrx --attribution-only`):**
  Full hash = 209.1 ms (4.78 H/s). Phase split (% of full hash):
  - blake2b (input→seed): 2.97 μs — **0.00%**
  - init_scratchpad (AES 2 MiB fill): 701 μs — **0.34%**  ← AES region is now negligible (the win stuck)
  - **chain: 7×run() + 7×blake2b: 181,192 μs — 86.64%**  ← DOMINANT
  - final run(): 25,847 μs — **12.36%**
  - get_final_result (AES+blake2b): 1,091 μs — **0.52%**
  - JIT speedup vs interpreted: **10.58×** (sanity check, healthy).
- **Interpretation:** The "chain" (the 7 main-VM program iterations + their blake2bs) is the entire
  ballgame — 86.6% of all hash time. AES is dead as a bottleneck (<1% combined). So the remaining
  ~6% gap to XMRig lives almost entirely in **the chain's efficiency** (main-VM program execution
  + its scratchpad/dataset memory traffic). This is exactly E2/E3b/E7 territory.
- **Caveat:** `--attribution-only` is **time-based**, not instr/cycle. It tells us WHERE time goes
  but not whether the chain is instruction-bound or IPC-bound. To decompose the chain's 86.6% into
  "real memory latency vs compute/scheduling," run **E2** (`--scratchpad-real` vs `--scratchpad-l1`
  under `perf stat -e cycles,instructions`) — that's the next pivot.
- **Effort:** done (device measurement, no code change). Next: E2.
  E2/E3/E5 (whether the superscalar IPC is actually exposed, whether main-VM is the cycle sink).

## E2 — scratchpad-real vs scratchpad-l1 under perf  [DONE 2026-08-03 — CHAIN IS COMPUTE/IPC-BOUND, not memory]
- **Question:** How much of the chain's cycles are *genuine memory latency* vs *compute/scheduling*?
- **Result (device, core 3, 2000 executions each, bench-internal `perf stat`):**

  | metric | scratchpad-REAL (2MiB) | scratchpad-L1 (16KiB) | Δ |
  |---|---:|---:|---:|
  | wall μs/program | 24,801.54 | 23,288.81 | −6.1% |
  | **cycles** | 47,306,235,460 | 44,721,186,358 | **−5.5%** |
  | **instructions** | 26,221,057,422 | 26,217,928,865 | **−0.01%** (identical) |
  | cache-misses | 190,339,816 | 113,037,514 | **−40.6%** |
  | **IPC** | **0.554** | **0.586** | **+5.8%** |

- **Interpretation (pivotal):**
  - Instructions are **identical** (0.01% diff) → L1-aliasing changes *memory behavior only*, not code.
    Clean isolation. ✓
  - Eliminating **77M cache misses** (whole scratchpad → L1) saves only **5.5% cycles / +5.8% IPC**.
  - ⇒ **~94% of the chain's cycles are COMPUTE/IPC-bound, NOT memory-latency-bound.** The A53's
    ~3-4 in-flight misses + working set fitting in L2 mostly hides the latency. Memory tricks
    (E9 hugepages +0.8%, E2 L1 +5.8% IPC ceiling) are **secondary**.
  - The chain runs at **IPC 0.554** — low for an in-order dual-issue core. The real cost is
    *instruction execution efficiency*: dual-issue utilization, dependency chains, the single
    memory-port serialization, integer/FP scheduling of the main-VM program.
- **Conclusion:** the remaining ~6% gap to XMRig (0.551→0.648 IPC = +17%) is **NOT memory**
  (that's capped at +5.8%). It is **instruction scheduling / dual-issue efficiency of the main-VM
  chain** → E3b / E5 / E7 territory. BUT the project has 4 prior confirmations that *blind*
  scheduling tweaks (PRFM, dual-issue padding, PGO ×2) regress/null on this core. So the next
  move is NOT another blind tweak — it's a **measured dual-issue analysis** of the chain's emitted
  AArch64 (JIT dump + the A53 dual-issue rules from the web sources: destevez.net, Tencent ncnn
  wiki) to find *concrete, evidence-based* pairing opportunities. See E11.
- **Effort:** done (device measurement, no code change). Next: E11 (dual-issue analysis) → then E3b/E5/E7.

## E11 — Measured A53 dual-issue analysis + XMRig AArch64 codegen comparison  [DONE 2026-08-03 — loss is CODEGEN, not structural]
- **Question:** where does the A53 dual-issue pipeline go idle in the emitted chain code — and does
  XMRig's *native AArch64* JIT do something we don't?
- **Method:** (a) captured live JIT buffer (118,784 B), disassembled, analyzed 7795 instrs; (b)
  cloned XMRig master, read `src/crypto/randomx/jit_compiler_a64.cpp` + `jit_compiler_a64_static.S`,
  diffed against armrx's `src/jit_compiler_a64_static.S` (620-line diff).
- **RESULT (robust):** of 1234 LOADs in our emitted chain, **83.1% are immediately followed by an
  instruction that consumes the loaded register** (two independent checks agree). Only 16.9% followed
  by an independent op.
- **CORRECTION :** the 83% serial-load pattern
  is NOT structural. XMRig's `randomx_calc_dataset_item_aarch64` (the dataset derivation, called
  16,384×/hash in light mode — the bulk of the chain) does **batched `ldp` (load-pairs) of the
  superscalar constants with interleaved `eor` consumers**:
    XMRig: `adr x7,superscalarMul0` / `ldp x12,x13,[x7]` / `eor x1,x0,x13` / `ldp x12,x13,[x7,16]` / ...
  armrx's `rx_calc_dataset_item` instead does **serial `ldr`-then-consume**:
    armrx: `ldr x12,superscalarMul0` / `eor x1,x0,x12` / `ldr x12,superscalarAdd1` / `eor x1,x0,x13` / ...
  ⇒ the 83% is a **codegen choice we made**, not an algorithm necessity. XMRig keeps the A53 load
  port busy (batched/pipelined) where armrx stalls it serially. (armrx's *main VM loop template*
  IS pipelined — the gap is specifically the dataset-derivation path.)
- **Also found:** armrx has a Track-D1 "2-way interleaved derivation" block but it is **explicitly
  unwired from the live mining path** ("NOT called from anywhere in the live mining path") — the
  interleaving idea exists but was never shipped to the hot path.
- **Revised conclusion:** the ~6% gap (0.551→0.648 IPC) is **attackable** via load-batching in
  `rx_calc_dataset_item` (and confirming the main-loop template matches XMRig). The 4 prior nulls
  were blind tweaks (PRFM/padding/PGO/*_M) on the WRONG region; the real lever is the
  dataset-derivation load stream, which XMRig demonstrably pipelines. ⇒ E12.
- **Effort:** analysis done (no code change yet). Output: this finding + the XMRig diff.

## E12 — Batch the dataset-derivation load stream (A53 load-port pipelining)  [DONE 2026-08-03 — NULL, precisely understood]
- **Question:** if we re-implement the dataset-derivation load stream to keep the A53 load port fed
  (batch the superscalar-constant loads via `ldp` pairs with interleaved independent `eor` consumers),
  does IPC rise toward XMRig's 0.648?
- **Technique source (READ-ONLY, no code copied):** XMRig's `randomx_calc_dataset_item_aarch64`
  (public GPL source, read for technique only) shows the pattern — `adr x7,superscalarMul0` then
  `ldp` pairs with interleaved consumers. **We write this fresh in armrx's own AArch64** using our
  register conventions + literal pool. No XMRig code is copied or imported (clean-room: idea only).
- **Implemented + measured (NULL):** rewrote `rx_calc_dataset_item` in `jit_compiler_a64_static.S`
  to pipeline the 8 superscalar-constant loads ahead of their `eor` consumers. Two bugs found and
  fixed during implementation (both instructive):
  1. `adr x13, superscalarMul0` + `ldp [x13]` SEGFAULTED — the `superscalarMul0..Add7` `.quad`
     literals sit **past `randomx_init_dataset_aarch64_end`** (outside the JIT's `CodeSize` copy
     window), so `adr` resolved to a garbage address post-memcpy. The original `ldr x12, <sym>`
     works only because the assembler *pools the value* in-bounds. Fix: use pooled `ldr` (no `adr`).
  2. Using x14–x17 as load temps SEGFAULTED (qemu gdbstub caught `stp x4,x5,[x17]` with x17 =
     `superscalarAdd4`'s value). The JIT-emitted main program keeps x14–x17 **live across the
     `rx_calc_dataset_item` call**; the prologue only saves/restores x0–x13, so x12/x13 are the
     ONLY caller-safe clobberable registers. Fix: restrict to x12/x13 (depth-2 overlap only).
- **RESULT (measurement, not guess):** gates GREEN (test_jit_equivalence 16/16 byte-identical,
  test_mining real shares, test_aes_hash). Live bench **4.75 H/s median 210.3 ms** = identical to
  pre-E12 baseline (4.75 H/s, 210.7 ms). perf A/B: **IPC 0.552 vs 0.554** — no change in
  instructions or cycles. ⇒ **E12 is NULL.**
- **Why null (the real finding):** the dataset-derivation load stream is **dependency-bound, not
  load-port-bound**. Each `eor` must wait 3 cycles for its loaded constant regardless of how many
  loads are in flight; depth-2 overlap doesn't change the critical path. The deeper overlap XMRig
  achieves (4+ loads via x14–x17) is **forbidden here** by the calling convention (those regs are
  live across the call). So E12 is a genuine dead-end, now *precisely understood* — unlike the prior
  blind nulls (PRFM, padding, PGO, `*_M`).
- **Revised read on the gap:** E11 correctly showed the loss is *codegen, not structural*, and XMRig
  does pipeline its loads — but E12 proves armrx's load stream is **not where the IPC gap lives**.
  The remaining 0.551→0.648 IPC gap is therefore NOT the dataset-derivation loads. It must be
  elsewhere (the main-VM program emission, or XMRig's different dependency-breaking in the program
  loop). The "simple load-batching fix" hypothesis is now exhausted. See E13.
- **Effort:** implemented + measured (code change + 2 bug fixes + gates + bench + perf A/B).

## E13 — Locate the real IPC gap: main-VM program emission vs XMRig  [TODO — next research step]
- **Question (the gap after E12):** E11 showed the loss is codegen; E12 proved it's NOT the
  dataset-derivation loads (dependency-bound, deeper overlap forbidden by call convention). Where
  does XMRig actually win its 0.551→0.648 IPC? The remaining candidates are the **main-VM program
  loop emission** (`randomx_program_aarch64_vm_instructions` in the static.S) and XMRig's different
  dependency-breaking in its program loop.
- **DONE (measured, 2026-08-03) — gap localized to scratchpad memory-op emission, which is the
  historically-forbidden `*_M` scheduling region.** Method: captured the live JIT buffer from a
  **real-mining** run (`bench_armrx --full-hash-only`; RWX region `ffff9062c000`, 118,784 B, same
  size as the E11 scratchpad-bench buffer — both are the static template, so identical code),
  disassembled (7907 instr), and classified every bracketed `ldr`/`ldp` by its base register and
  whether the next instruction immediately consumes the loaded register:
  - **scratchpad (x2): 14 loads, 64.3% serial** (load → immediate consume). This is the main-VM
    scratchpad path (IADD_RS / IXOR_R / IMUL_R → read scratchpad[idx] → transform → write back).
  - **dataset (x1/x20): 1 load, 0% serial** — already pipelined (confirms the lines ~837-857
    `umov`/`add` interleaved with `ldr` dataset reads are doing their job).
  - **NEON vector + PC-relative literal loads (15): 0% serial** — not relevant memory traffic.
  - Re-ran the same classification on the original E11 `--scratchpad-real` buffer: scratchpad 71.4%
    serial — the scratchpad-bench does NOT materially skew it; the real-mining buffer (64.3%) is the
    authoritative number.
- **Conclusion:** the 0.551→0.648 IPC gap lives in the **scratchpad memory-op emission** — `ldr
  x2 → immediate consume` 64% of the time, with no independent op hoisted into the 3-cycle load-use
  bubble. This is exactly the **`*_M` memory-op scheduler region** that AGENTS.md records as having
  **diverged and been reverted** (caused a real JIT/interpreter mismatch). The historical blocker is
  that the **divergence mechanism was never identified** — that makes `*_M` a *solved-unknown*, not a
  proven-impossible wall. RandomX scratchpad ops are `read → transform → write back` where the
  transform depends on the loaded value, so there is little independent work to schedule into the
  bubble, which is likely *why* a naive scheduler pass broke — but the exact constraint violated was
  never root-caused. The dataset path, dataset-block read, and NEON AES are already pipelined (so the
  prior 4 nulls + E12 all attacked either pipelined or forbidden regions).
- **XMRig ground truth (measured on-device, 2026-08-03):** a compiled `xmrig-dev` (XMRig/6.26.1-dev,
  ARMv8, musl) runs on this device. `taskset -c 3 perf stat` on a 1-thread run gives **IPC 0.628**
  (181.1B instr / 288.6B cycles over 70s) — confirming the per-cycle gap vs armrx's 0.551 is **real
  (~14%)**, not a measurement artifact. At 8 threads XMRig = 28 H/s ≈ armrx isolcpus 28.4 H/s →
  **multi-core parity**; the gap is purely per-cycle efficiency. So the E3/E11/E13 premise (XMRig
  ~5 H/s/core, higher IPC) is vindicated by running XMRig on the same silicon.
- **Why the code-level search is NOT over (correction):** the scratchpad path is a small slice (~9%
  of instructions per E1's chain census), so even a *perfect* `*_M` fix yields <9% IPC — short of the
  0.551→0.628 gap. BUT the gap is confirmed real and XMRig demonstrably closes it, so the remaining
  question is concrete: **what does XMRig's emitted scratchpad code do that armrx's doesn't?** The
  answer is NOT "9 years of untouchable tuning" — it is a specific, learnable emission difference
  (XMRig reads technique-only; no code copy). The blocker is the unsolved `*_M` divergence mechanism.
  => **E14: root-cause the historical `*_M` JIT/interpreter divergence** (build a minimal
  scratchpad-reorder that trips it, bisect the violating constraint) so the lever can be attempted
  safely. Until that mechanism is understood, `*_M` stays off-limits; once understood, a *correct*
  scratchpad scheduler is the one remaining real per-cycle lever.
- **Constraints honored:** clean-room (XMRig run + read for technique only; no code copied); full
  gates before any change; did NOT touch `*_M` in E13. The `jit_compiler_a64_static.S` E12 edit is
  retained as a documented dead-end reference (correct, non-regressing, null).
- **Effort:** analysis + measurement (live buffer capture, disassembly, per-base load classification
  on two independent buffers, on-device XMRig perf A/B). No code change for E13 itself.

## E14 — Root-cause the historical `*_M` JIT/interpreter divergence  [TODO — the real next lever]
- **Why this is THE next step:** E13 localized the IPC gap to the scratchpad memory-op (`*_M`) emission
  and confirmed (via on-device XMRig, IPC 0.628 vs 0.551) the gap is real. The only blocker to
  attacking it is AGENTS.md's note that a prior `*_M` scheduler **diverged from the interpreter and was
  reverted — mechanism not identified**. So the gap is closed behind an *unsolved bug*, not a wall.
  E14 is to solve that bug so the lever becomes safe to pull.
- **Hypothesis for the divergence:** RandomX scratchpad ops are `read scratchpad[idx] -> transform ->
  write scratchpad[idx2]`. A naive scheduler that hoists independent ops across a scratchpad read/write
  likely violated one of: (a) **address aliasing** — idx and idx2 can coincide (read-modify-write of
  the same cell), so reordering a later op's load before an earlier store changes the value observed;
  (b) **program-counter / iteration-order coupling** — some opcodes' effect depends on the *sequence*
  of scratchpad accesses, not just the final state; (c) the **AES/FP state** (mx, ma, spMix) must
  advance in lockstep with the integer ops, and a reorder desynced it. The exact one was never pinned.
- **Method (safe, no production risk):** start from the gated setup that originally tripped it. Build a
  *minimal* scratchpad-reorder variant (e.g., hoist one independent integer op across one `IADD_RS`
  scratchpad load) and run the existing `test_jit_equivalence` (16 seed/input pairs, byte-identical
  gate) — when it diverges, bisect: is it aliasing (same-cell read/write)? PC coupling? AES-state
  desync? Capture the exact violating constraint in a doc. Keep the emitter scheduler's `*_M` path
  DISABLED in production until the mechanism is understood and a *correct* (constraint-respecting)
  scheduler is proven green on all gates.
- **Clean-room:** XMRig's scratchpad emission is readable for *technique* only; no code copy. The fix,
  if found, is implemented in armrx's own emitter.
- **Success criterion:** a written root-cause (which constraint the original reorder violated) + either
  (a) a *correct* scratchpad scheduler that passes all gates and improves IPC, or (b) proof that NO
  safe reorder exists (gap is then truly structural for this core). Either outcome closes E14.
- **Effort:** analysis + a contained emitter experiment + the full gate suite. No production change
  until gates are green and the mechanism is documented.
- **DE-PRIORITIZED (2026-08-03, real-world evidence):** the like-for-like real-pool test (both 8-worker,
  no isolcpus) shows armrx = **21.4 H/s** vs XMRig = **27.77 H/s** — a real **~23% gap**. But armrx WITH
  isolcpus = **~28.4 H/s** (matches XMRig). So the gap is NOT the IPC microbenchmark delta (0.551 vs
  0.628) — it is armrx's **non-isolated hugepage + worker-affinity handling** vs XMRig's. XMRig gets
  hugepages ("huge pages 100% 8/8") and tolerates the two-cluster topology without isolcpus; armrx's
  non-isolated default falls back to **4 KiB pages** (E9: MAP_HUGETLB failed) and lets the OS scatter
  workers onto weak cores 4-7 (the documented ~half-throughput penalty). Therefore the IPC-gap chase
  (E11-E13) was a **phantom real-world deficit**; the *next* concrete lever is E15, not E14. E14 is
  only worth doing as a pure correctness-exercise, not for performance.

## E15 — Non-isolated gap: localized to MULTI-WORKER SCALING (per-core is fine)  [DECISIVE — lever found]
- **The finding (2026-08-03, real pool, settled, same silicon):**
  | Test | armrx | XMRig | armrx / XMRig |
  |------|------:|------:|---------------:|
  | 1 worker, core 3 | **4.08 H/s** (settled 3.5 min) | 4.53 H/s (core-3) | **~90%** |
  | 8 workers, no isolcpus | **21-23 H/s** | 27.77 H/s | **~79%** |
  | 8 workers, isolcpus | ~28.4 H/s | (n/a) | == XMRig |
- **Per-core is FINE (~90%):** 1 worker on core 3 = 4.08 vs XMRig 4.53. The ~10% per-core gap is the
  known IPC microbenchmark delta (0.551 vs 0.628), already deemed a *phantom real-world deficit*. So
  the hash loop / JIT is NOT the problem.
- **The real gap is MULTI-WORKER SCALING:** armrx 1w→8w scales only **5.4×** (8 × 4.08 = 32.6 ideal;
  actual 22 → **~67% of linear potential**). XMRig scales **6.1×** (27.77 / 4.53). So armrx's 8
  concurrent workers lose ~33% of their own per-core potential to contention/placement on this
  two-cluster MSM8929 interconnect, while XMRig loses less. The 8w armrx/XMRig ratio (79%) = per-core
  (90%) × multi-worker scaling (armrx 5.4 vs XMRig 6.1 → ~88%) → the extra ~10-13% 8w gap beyond
  per-core is pure multi-worker inefficiency.
- **Two earlier hypotheses RULED OUT** (measurement + code inspection):
  - **Hugepages:** as root armrx acquires 128×2 MiB via `MAP_HUGETLB` (HugePages_Free 256→128) yet
    only 22.9 H/s (+1.5). armrx's `virtual_memory.c:235` already uses XMRig's exact technique.
  - **Affinity:** default `AffinityMode::All` (mining_engine.cpp:168) pins worker i 1:1 to
    `core_order_`; on no-cpufreq device that = 0..7 (coincidentally correct fast/weak split).
- **The actual lever = multi-worker scaling on the two-cluster interconnect.** Concrete sub-areas to
  investigate (NOT yet done): (a) armrx's 8 workers may under-utilize fast cores / over-subscribe weak
  cluster under load (the `detect_core_order` fallback 0..7 means no awareness that 4-7 are weak);
  (b) cross-thread cost unique to armrx — partial-dataset fill threads, dataset-rebuild handshake,
  shared-cache locking — contending with workers on the shared interconnect; (c) XMRig may pin/balance
  differently under load to avoid weak-cluster loss.
- **Notes:** (1) the earlier "2× per-core" claim was WRONG (4w run read un-settled at 9 H/s; its own
  log showed 10 H/s at 380s). Retracted. (2) armrx ignores SIGTERM in pool mode (`timeout`/`Ctrl+C`
  don't kill it; needs `sudo kill -9`) — a test-methodology quirk, not a perf issue. (3) per-core 4.08
  being ~90% of XMRig confirms the E11-E13 IPC-gap chase was a phantom real-world deficit.
- **Next step (E16):** attack multi-worker scaling — measure per-worker H/s at 8w to find which workers
  (likely weak-cluster 4-7) under-perform, and whether armrx's fast/weak awareness (currently absent on
  no-cpufreq devices) or its fill-thread/handshake contention is the cause. Target: close the 79%→90%
  8w ratio (recover ~3-4 H/s at 8w non-isolated).
- **Constraints:** full gates; measure on real pool; do NOT regress isolcpus path. `jit_compiler_a64_
  static.S` E12 edit stays as dead-end reference. No code change made for E15 (localization only).


- **Question:** XMRig gets more work/cycle doing the *same algorithm* on the *same silicon*. Where?
- **Sub-experiments:**
  - **E3a — Blake2b IPC vs reference.** Roadmap *assumed* Blake2b parity, never *measured* it.
    Run armrx Blake2b microbench under perf; compare IPC to XMRig's (or a known reference impl).
    Blake2b is ~0.7M instr but could be high-cycle on A53.
  - **E3b — main-VM emission / memory-op ordering diff vs XMRig.** XMRig may order/ interleave
    its scratchpad loads to hide latency better. Diff the two under `perf mem` / cache-miss stats.
- **Effort:** E3a ~10 min (microbench exists); E3b ~30 min + XMRig binary present on device.
- **What a result tells us:** localizes the IPC deficit to a named region we can actually attack.

## E4 — 8-worker cluster-penalty measurement  [TODO]
- **Question:** The 1-core number ignores the real deployment (8 workers). Cores 4–7 lose ~50%
  under contention (interconnect arbitration). What's armrx's *actual* 8-worker H/s vs XMRig's?
- **Measurement:** `taskset -c 0-7 ./bench_armrx` (or miner) both impls; compare total H/s and
  per-cluster contribution. Note isolcpus status.
- **Why it's the biggest *real-world* gap:** 1-core parity is ~5% off; 8-core could be much worse
  if the weak cluster drags. This is system-level, not armrx code, but it's the number that matters
  for mining.
- **Effort:** ~10 min device, no code change.
- **What a result tells us:** whether "parity" is a 1-core illusion; scopes a deployment/threading
  investigation.

## E5 — C* LDR latency rotation (old "Item 2")  [TODO — conditional, low priority]
- **Question:** Does software-pipelining the C* constant load (emit op N+1's LDR one op ahead)
  recover superscalar-region cycles?
- **Gate:** ONLY if E1 shows superscalar-region IPC materially below the 0.800 census figure.
- **Effort:** code change + device gates + scheduler-stress (W3-2 stands — never touch `*_M`).
- **Risk:** project has 4 prior confirmations that micro-latency tweaks on this core regress/null.
  Treat as likely-null until E1 justifies it.

## E6 — Custom Blake2b / batch fusion (longer-term, XMRig's residual edge)  [BACKLOG]
- **Question:** XMRig's ~1–3% residual is "custom Blake2b, batch-mode fusion, pipeline tricks."
  Is armrx's Blake2b sub-optimal on A53 specifically?
- **Effort:** real code work, needs its own brief + gates.
- **Why backlog:** only worth it after E3a shows Blake2b is actually a cycle sink. Don't speculate.

## E7 — main-VM memory-op scheduling (RE-OPEN WITH CARE)  [BACKLOG, risky]
- **Question:** W3-2 closed the `*_M` scheduler extension as a *divergent* dead end. But that was
  about *correctness divergence*, not the IPC question. A *different* approach (reorder only
  loads vs ALU, never touch store ordering) might hide latency without the divergence.
- **Risk:** HIGH — previously produced deterministic JIT/interpreter hash mismatch. Only revisit
  with a fresh, isolated brief + the `ARMRX_MAX_SWAPS` bisect harness from W3-2.
- **Gate:** only after E2/E3b show main-VM latency is the dominant cycle sink AND a safe reorder
  shape is designed.

---

## E8 — CRC32 method swap (Chorba / table-less braiding)  [RETIRED 2026-08-03 — not applicable]
- **Source:** `armv8 papers/2412.16398v1.pdf` ("Chorba: A novel CRC32 implementation", Russell 2024).
  +100% CRC32 throughput on ARMv8 vs table-based Sarwate; on par with / exceeds hardware
  CRC32C on Graviton & Raspberry Pi 4. Method: table-less "zero polynomial" braiding/folding.
- **RETIRED — evidence:** armrx's main build contains **no CRC32 in the hash path**.
  (1) `objdump -d build-cross/bench_armrx` → 0 `crc32` opcodes. (2) No CRC32 polynomial
  constant (`0xEDB88320`/`0x82F63B78`) in `.rodata`. (3) `VirtualMachine::get_final_result`
  (src/vm.cpp:962) is `hash_aes_1r_x4` only — pure AES. (4) `rx_crc`/`soft_crc` appear only in
  `scratch_vm_study/` (embedded reference, excluded from main build per AGENTS.md). The
  `cpu.crc32` flag in `cpu_features.cpp` is inherited upstream cruft, **unreferenced**.
  → there is no software CRC32 path to replace, so Chorba yields no win. Kept as a note so we
  don't re-discover it. (Paper itself remains a good ARMv8 reference; just not for armrx.)

## E9 — Hugepages actually allocated?  [FIX APPLIED 2026-08-03 — +0.8% H/s, TLB not dominant]
- **Source:** XMRig RandomX Optimization Guide: "Huge Pages can increase RandomX performance up to 50%."
- **DIAGNOSIS:** confirmed armrx was on 4 KiB pages (smaps KernelPageSize 4kB, AnonHugePages 0kB);
  `MAP_HUGETLB` failed because `HugePages_Total: 0`.
- **FIX (user, as root, 2026-08-03):** `echo 256 | sudo tee /sys/kernel/mm/hugepages/
  hugepages-2048kB/nr_hugepages` → `HugePages_Total: 256`. Re-bench: smaps now shows the 256 MiB
  cache on `KernelPageSize: 2048 kB` (genuine 2 MiB hugepages). (Note: plain `echo 256 > /sys/...`
  fails for non-root — the shell opens the file as the user before sudo; `sudo tee` is required.)
- **RESULT:** live bench 4.75 → **4.79 H/s** (median 210.72 → 208.95 ms) = **+0.8%**. Much smaller
  than XMRig's headline because on this in-order A53 the bottleneck is the **memory-latency wall**
  (L1D 3cyc / L2 17cyc / DRAM 129ns, single port), NOT TLB misses — the random-access dataset
  derivation fits per-iteration working set in L2, so TLB pressure was secondary. Still free and
  correct; stays on. **Lesson:** XMRig's "up to 50%" is for x86 ref targets with different TLB
  pressure; on this A53 the win is marginal. Do not over-invest in TLB tricks (E10 prefetcher likely
  similarly marginal — but still worth one measurement).

## E10 — Disable hardware prefetchers (deployment, FREE, unmeasured)  [TODO]
- **Source:** XMRig RandomX Optimization Guide: "You must disable hardware prefetchers to get the
  optimal RandomX performance." Also chipsandcheese: RandomX is *random-access*, not streaming, so
  the HW prefetcher can't learn the pattern and may *pollute* the tiny L1D/L2 with useless lines.
- **Question:** are the A53's L1/L2 hardware prefetchers enabled on this device? (Typically via
  `MSR`/`CPUECTLR` — often locked on ARM64 Linux without firmware support; postmarketOS may leave
  them on.) RandomX's random scratchpad stride defeats stride/stream prefetchers.
- **Measurement:** try the known knobs (if accessible): `echo 0 > /sys/devices/system/cpu/cpuN/
  cache/index2/prefetcher` (where exposed) or a kernel/boot flag; re-bench. If the sysfs node is
  absent (common on ARM), this is a no-op on this device and we record it as "not applicable here."
- **Caveat:** our T2-1 PRFM test (inline `PRFM PLDL1KEEP` in the main-VM path) measured a
  *regression* — but that was the MAIN-VM load path under contention, NOT the scratchpad. The
  XMRig guidance is about the *prefetcher hardware*, orthogonal to our PRFM test. Don't conflate.

## Microarch knowledge (from web pass 2026-08-03 — informs E2/E3b/E7)
- **A53 single memory port:** only ONE load OR store per cycle; cannot dual-issue load+store
  (destevez.net "Coding NEON kernels for the Cortex-A53"; Tencent ncnn A53/A55 dual-issue wiki).
  RandomX scratchpad traffic is register-indexed (`[x2, Xm]`), NOT streaming → every access is a
  distinct AGU op serialized at the single port. This is the prime suspect for our 0.551 vs XMRig
  0.648 IPC gap (XMRig may order/software-pipeline its memory ops to keep the port busy + hide
  the 3-cycle L1D latency better).
- **In-flight misses:** A53 L1D tracks only ~3-4 pending misses; L1D hit=3cyc, L2≈17cyc, DRAM≈129ns.
  In-order → a miss stalls the whole pipe. RandomX light-mode = inherently memory-latency-bound.
- **Prefetch contradiction resolved:** HW stream prefetcher helps streaming (Sonos found PRFM
  useless there), but RandomX is random-access → HW prefetcher likely useless or polluting →
  disabling it (E10) is the XMRig-recommended move; our PRFM test was a different path.
- **Sources:** chipsandcheese "ARM's Cortex A53: Tiny But Important"; destevez.net 2025-02
  "Coding NEON kernels for the Cortex-A53"; Sonos tech-blog "Assembly still matters: A53 vs M1";
  Tencent ncnn arm-a53-a55-dual-issue wiki; ARM Cortex-A53 TRM; xmrig.com RandomX Optimization Guide.


- **Source:** `armv8 papers/2412.16398v1.pdf` ("Chorba: A novel CRC32 implementation", Russell 2024).
  +100% CRC32 throughput on ARMv8 vs table-based Sarwate; on par with / exceeds hardware
  CRC32C on Graviton & Raspberry Pi 4. Method: table-less "zero polynomial" braiding/folding.
- **RETIRED — evidence:** armrx's main build contains **no CRC32 in the hash path**.
  (1) `objdump -d build-cross/bench_armrx` → 0 `crc32` opcodes. (2) No CRC32 polynomial
  constant (`0xEDB88320`/`0x82F63B78`) in `.rodata`. (3) `VirtualMachine::get_final_result`
  (src/vm.cpp:962) is `hash_aes_1r_x4` only — pure AES. (4) `rx_crc`/`soft_crc` appear only in
  `scratch_vm_study/` (embedded reference, excluded from main build per AGENTS.md). The
  `cpu.crc32` flag in `cpu_features.cpp` is inherited upstream cruft, **unreferenced**.
  → there is no software CRC32 path to replace, so Chorba yields no win. Kept as a note so we
  don't re-discover it. (Paper itself remains a good ARMv8 reference; just not for armrx.)


- Superscalar NEON register pool (all 32 v-regs live across the loop; register-infeasible).
- Superscalar density (post-W4 body at A64 ISA floor; w23's list exhausted).
- PRFM hints (T2-1, regression), dual-issue padding (T2-2, regression), PGO (null ×2),
  Track C wrapper (≤0.5%), CBRANCH CSEL (regression), D1/D3 interleaves.
- Memory-op (`*_M`) scheduler extension in its original form (W3-2 divergence).

## Meta
- 3 weeks of agent work got armrx to ~parity-with-XMRig on a part XMRig devs never tuned for;
  XMRig is 9 years old. The instruction-count fight is won. The cycle-efficiency fight is open.
- Next physical step recommended: **E1** (re-attribution) — it re-baselines everything else.
