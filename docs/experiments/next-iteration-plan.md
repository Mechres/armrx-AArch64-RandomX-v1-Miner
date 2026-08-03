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

## E15 — Non-isolated gap: scaling is LINEAR (no software inefficiency); remaining gap is CODE (instr count)  [scaling CLOSED]
- **The finding (2026-08-03, real pool, settled, same silicon):**
  | Test | armrx | XMRig | armrx / XMRig |
  |------|------:|------:|---------------:|
  | 1 worker, core 3 | **4.08 H/s** (settled 3.5 min) | 4.53 H/s (core-3) | **~90%** |
  | 8 workers, no isolcpus | **21-23 H/s** | 27.77 H/s | **~79%** |
  | 8 workers, isolcpus | ~28.4 H/s | (n/a) | == XMRig |
- **Per-core is FINE (~90-95%):** 1 worker on core 3 = 4.08 vs XMRig 4.53 (early-cool reading; the
  fan-cool pool-test later gave 3.96 — same ballpark). **RETIRED 2026-08-04 (E19):** the "~10% per-core
  gap is an instruction-count delta" framing is wrong — a clean 500-hash census shows armrx emits
  **12% FEWER** instructions and loses on **IPC 0.547 vs 0.654** (`other_interlock_stall`, 2.12×).
  The deficit is stalls, not instruction count; the *size* of the per-core gap (~5-10%) still stands.
- **Multi-worker SCALING is actually LINEAR (corrected):** the fan-cool `--pool-test` per-worker
  breakdown shows the fast cluster scales 3.8× from 1w→4w with NO interconnect penalty (every fast
  worker ~3.75 H/s) and the weak cluster runs at exactly 0.53× fast (1.88 vs 3.58) — the SoC's own
  design ratio, which XMRig also bears. So armrx's 8w "5.5× from 1w" is just 4 fast + 4 half-speed
  cores; there is NO software multi-worker inefficiency. The old "armrx scales 5.4× vs XMRig 6.1×"
  framing (built on throttled 4.08/21-23 vs XMRig 27.77) was a measurement artifact — both miners
  carry the same weak-cluster penalty.
- **Therefore the real, remaining gap is CODE (per-instruction efficiency), not scaling:** XMRig 28
  vs armrx 21.85 @8w on identical HW+cooling, and the ~26% excess instruction count in armrx. See
  the E16 code-level leads (E3a/E3b/E3c) for where to attack it.
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
  (recover ~3-4 H/s at 8w non-isolated).
- **Constraints:** full gates; measure on real pool; do NOT regress isolcpus path. `jit_compiler_a64_
  static.S` E12 edit stays as dead-end reference. No code change made for E15 (localization only).

## E16 — Multi-worker scaling: per-worker structure captured; GAP IS CODE (instr efficiency)  [scaling CLOSED, code-leads OPEN]
- **Method:** `armrx --mine --seconds=60` (self-terminating local bench, same MiningEngine threading
  as pool). Light mode, 60s, settled. taskset pins as noted. Per-worker H/s printed at end.
- **Raw data (Steady-state, H/s):**
  | Config | Pinned | Total | w0 | w1 | w2 | w3 | w4 | w5 | w6 | w7 |
  |--------|--------|------:|---:|---:|---:|---:|---:|---:|---:|---:|
  | 1w | 0-7 | 1.32 | 1.32 | | | | | | | |
  | 4w | 0-7 (→0-3 fast) | 9.18 | 1.89 | 2.55 | 2.42 | 2.32 | | | | |
  | 8w | 0-7 (0-3 fast / 4-7 weak) | 11.36 | 1.32 | 1.92 | 1.92 | 1.92 | 1.06 | 1.03 | 1.06 | 1.13 |
  | 4w-weak | 4-7 | 6.14 | 1.58 | 1.55 | 1.48 | 1.52 | | | | |
- **Structural findings:**
  1. **Placement is CORRECT** (not a bug): in 8w, fast cores 0-3 get 1.92/w, weak cores 4-7 get
     1.06/w → ratio 0.55 ≈ topology's 0.53 (XMRig fast 4.5 / weak 2.4). So weak-cluster workers ARE
     on weak cores; the gap is NOT mis-placement.
  2. **worker[0] anomaly (real, reproducible):** w0 is always the slowest — 1.32 in 1w AND 8w, vs
     1.89 in 4w and 1.92 for w1-3 in 8w. ~half of sibling workers in 8w (1.32 vs 1.92). Strongly
     suggests **worker[0] is the main/coordinator thread** doing non-hash work (job dispatch, dataset
     rebuild on new seed, pool network) and thus hashes less. Minor absolute effect (~0.6 H/s of 11.36).
  3. **Interconnect contention signal:** 4 fast-only = 9.18 (2.30/w). Adding 4 weak workers drops fast
     workers to 1.77/w avg (**−23%**) while weak add only 1.06/w. Net 8w = 11.36. So weak-cluster
     workers' memory traffic contends on the shared two-cluster interconnect and starves the fast
     cluster — a real multi-worker scaling loss, observed even in --mine.
- **RESOLVED — the multi-worker "gap" was THERMAL THROTTLING (confirmed with bigger fan, 2026-08-04):**
  the device's MEMORY subsystem throttles at 60°C (CPU clock fixed 765 MHz, but RandomX is
  memory-bound → craters). User installed a bigger fan; re-ran the `--pool-test` sweep with the
  SoC held COOL. Same 1-worker light-mode hash, before/after fan:
  | 1w pool-test | Temp | Rate |
  |--------------|------|-----:|
  | old (saturated) | 60°C | 0.68 H/s |
  | old (cool/early) | 46°C | 3.19 H/s |
  | **new fan (stable whole run)** | **43°C** | **3.96 H/s** |
  The fan holds 43°C → **3.96 H/s sustained** (vs 0.68 @60°C = **~5.8× recovery**). 3.96 ≈ the old
  cool reading (4.08) → **~4 H/s is the TRUE un-throttled 1w rate**; every prior "settled" 150s+
  number at 60°C was ~5-6× too low. Temperature was the uncontrolled confound in all E15/E16 numbers.
- **THERMALLY-CLEAN pool-test sweep (fan, 120s each, settled):**
  | Config | Pinned | Temp | Total | w0 | w1 | w2 | w3 | w4 | w5 | w6 | w7 |
  |--------|--------|------|------:|---:|---:|---:|---:|---:|---:|---:|---:|
  | 1w | core 3 | 43°C | **3.96** | 3.96 | | | | | | | |
  | 4w | 0-3 (fast) | 53°C | **15.01** | 3.72 | 3.75 | 3.82 | 3.71 | | | | |
  | 8w | 0-7 (0-3 fast/4-7 weak) | 57°C | **21.85** | 3.56 | 3.49 | 3.59 | 3.68 | 1.88 | 1.87 | 1.89 | 1.88 |
  - **Fast cluster scales CLEANLY:** 4w = 3.8× from 1w (15.01/3.96), every fast worker ~3.75 H/s.
    The earlier "--mine" finding of weak workers dragging fast workers, and worker[0] being the
    slowest (main-thread tax), were BOTH thermal artifacts (at 60°C the shared memory bus was
    bandwidth-starved; cool, w0 ≈ siblings and fast scaling is linear). NO structural interconnect
    or main-thread deficit at 53-57°C.
  - **Weak cluster = exactly 0.53× fast** (1.88 vs 3.58) — matches the topology's intrinsic 0.53
    ratio (XMRig fast 4.5 / weak 2.4). It is NOT a scaling defect; the weak A53s are just ~half
    speed, by design. Placement is correct (weak workers ARE on weak cores).
  - **8w = 21.85 H/s = 5.5× from 1w** (ideal 8× = 31.7; actual 69%). The 31% loss vs ideal 8×
    is ENTIRELY the weak cluster being half-speed (4 fast×3.7 + 4 weak×1.9 = 22.4 ≈ measured 21.85).
    So armrx's multi-worker scaling is actually **linear per-core**; the "gap" vs ideal 8× is just
    the hardware's own fast/weak asymmetry, not a software inefficiency.
- **`--pool-test` feature (shipped, measurement-only):** `armrx --pool-test ... --seconds=S`
  honors `--seconds` (self-terminates via `std::_Exit(0)` right after printing the per-worker
  summary — skips the pool/engine teardown that otherwise hangs), prints `worker[0..N]` H/s + CPU
  max temp. Pool correctness unchanged (still connects/submits). No credentials in source — the
  user supplies `--pool/--wallet/--password` as always. CLI test passes; cross-build clean.
- **REMAINING (measurement, not code):** the old "armrx 21-23 vs XMRig 27.77 H/s @8w" E15 comparison
  is apples-to-oranges — that XMRig number was ALSO taken at 60°C (throttled). Re-baseline XMRig
  COOL (same fan) to get the true 8w ratio. Per-core armrx is already ~88% of XMRig's 4.53 (3.96).
  If XMRig@cool ≈ its known ~27-28 H/s, the residual (~22 vs ~28) is the weak-cluster 0.53×
  hardware asymmetry that XMRig also bears — i.e. armrx may be within a few % of XMRig once both
  are measured cool. No code change pending; this is a measurement to close the book on E15/E16.
- **Constraints:** full gates; do NOT regress isolcpus path or pool correctness; E12 edit stays as
  dead-end reference. `--pool-test` is a supported measurement mode (like other miners' test flags).
- **CORRECTION (2026-08-04):** the earlier "thermal throttle is THE cause / weak-cluster 0.53× hardware
  asymmetry explains the gap" write-up is **RETRACTED**. The fan was present throughout (6cm moved 2 days
  prior; 12cm added after). Both miners ran on the SAME silicon + SAME cooling and both hit the same
  ~60°C steady-state — so the gap is NOT thermal and NOT hardware asymmetry (XMRig bears the identical
  asymmetry + identical temp and still wins). The "5.8× recovery" was a measurement error: comparing a
  30s early-cool reading against a 120s steady-state reading, not a real before/after. **The real,
  still-open gap is CODE.** Measured on identical HW: XMRig 28 H/s vs armrx 21.85 H/s @8w (both ~60°C,
  both fan-cooled) → armrx ~78%. The original W1-4 census claimed armrx emits **~119M vs XMRig
  ~94.5M instr/hash (~+26%)** for the same algorithm. That instruction-gap claim was
  the original lead — **RETIRED 2026-08-04 (E19)**: a clean 500-hash `--perf-ready` census shows
  armrx actually emits **12% FEWER** instructions (89.5M vs 101.4M) and loses on **IPC 0.547 vs
  0.654** from `other_interlock_stall` (integer-multiplier interlocks, 2.12× XMRig). What DID hold up
  from the fan re-test: armrx's per-core/multi-worker scaling is LINEAR (4w = 3.8× from 1w, weak
  cluster = 0.53× fast = the SoC's design, not a software defect). So the scaling structure is fine;
  the deficit is per-instruction efficiency (stall/IPC), i.e. CODE — but the lever is stalls, not
  instruction count.
- **OPEN — code-level leads (see perf-tracking.md §1/§2 for live status):** E3c (C* immediates) and
  E3a (Blake2b) are CLOSED; E20 (peephole) NULL-reverted; E21 (umov) false-premise-closed. The
  dominant stall class is named (E19) but its proximate cause in *our* emitters is not yet localized
  to a specific region or instruction sequence. Remaining live investigations:
  - **Localize the `other_interlock_stall` excess to a specific emitter region** — the superscalar
    body (~80% of work, 35% multiplies) and the main-VM body (~10%) are the candidates. A
    region-tagged `perf annotate` / `--jit-dump` attribution per opcode class, miner-to-miner, would
    say which region over-emits multiply-dependent chains. (E19 named the class; this names the place.)
  - **E22 (2026-08-04): superscalar scheduler — INCONCLUSIVE on the interlock cause.** Disabling
    `scheduleSuperscalarProgram` (identity order) gave H/s **unchanged (4.77)** and instruction
    count **+13.3%** (101.4 vs 89.5 M — the scheduler hides ~12 M instr of latency-fill, so the
    "armrx 12% leaner than XMRig" figure is scheduler-driven, not structural). The cycle/stall
    comparison was **contaminated** (two bad perf runs: background on core 3 in run #1; perf attached
    to a dead PID in run #2) and is **retracted** — the interlock question is OPEN, not answered.
    Net: the scheduler is safe to keep (density-positive, no H/s regression) but E22 does NOT
    localize the E19 excess; the live hypotheses stay upstream of emission (generated program order,
    main-VM multiply handlers, C* padding, x12 WAR).
  - **TOP UNTESTED LEAD — generated program order (`src/superscalar.cpp`).** E22 rules out the
    *emitter*; the difference must be upstream. armrx's **generated** superscalar stream may differ
    from XMRig's before emission. XMRig's generator deliberately tracks multiply availability
    (`mulCount`, `fetchNext`, `allowChainedMul` in its `superscalar.cpp`). A static diff of the two
    generators' multiply-to-multiply spacing, or a region-tagged PMU on armrx's generated vs XMRig's
    order, localizes this. **This is the next thing to measure.**
  - **Main-VM multiply handlers / `emitMemLoad`** (h_IMUL_R/M, h_IMULH_M, h_ISMULH_M + the
    address/load/use chain) — structurally different from the superscalar path; region-tagged PMU
    could show disproportionate contribution. Not yet measured.
  - **C* density stripped "free" padding (Planner H2, lower confidence):** armrx's 2-instr form is
    denser than XMRig's 3-instr; on a latency-bound in-order core, removing instructions can *expose*
    MAC latency. Only matters if the generated-order test comes back clean.
  - **`x12` WAR (Planner H3, lower confidence):** `computeSuperscalarFootprint` tracks only VM regs
    r0–r7, blind to the `x12` temp shared by `IMUL_RCP`/`IXOR_C*`. Cheap check: repoint `IXOR_C*`'s
    temp to `x14`.

> **Lead tracking & discipline:** see [`docs/experiments/perf-tracking.md`](perf-tracking.md) —
> standing facts, the CLOSED/DEAD lead table (so we never re-run them), open lead status, and the
> GitHub-Copilot-ideas triage. Read it before starting any new measurement.
>
> **UPDATE 2026-08-04 — E16's code-lead list above is partly SUPERSEDED. Read perf-tracking.md
> §1/§2 before acting on E3a/E3b/E3c:**
> - **E3a (Blake2b)** — closed by inspection: armrx is NEON, XMRig is scalar C. armrx wins.
> - **E3c (C\* immediates)** — **CLOSED.** armrx routes all `IADD_C*`/`IXOR_C*` through a 128-slot
>   inline literal pool (`emitCpoolImmediate`, `jit_compiler_a64.cpp:1323`) = `LDR`+ALU = **2 instr**.
>   **XMRig does NOT use `umov` here** (see E21): its superscalar path pre-fills `num32bitLiterals=64`
>   (`jit_compiler_a64.cpp:337`), making the `umov` branch unreachable — it falls to a **3-instr**
>   `MOVZ`/`MOVN`+`MOVK`. So armrx is *denser* (2 vs 3), not parity. No density gap to attack.
> - **DIFF RESULT 2026-08-04 — the density premise is DEAD, and it inverted.** A clean 500-hash
>   `bench_armrx --perf-ready` census (saturated window, clock-consistency-checked) gives
>   **armrx 89.5 M instr/hash @ IPC 0.547** vs **XMRig 101.4 M @ IPC 0.654**. armrx executes **12%
>   FEWER instructions** and loses anyway: **163.5 vs 155.0 M cycles/hash**. The gap is **stalls
>   (IPC), not code size** — every "+14% / +26% / +35% too many instructions" figure in this repo
>   came from an unsaturated `--mine` perf window (455 MHz effective on a 765 MHz core) and is
>   retired. See **E19**.
> - **E19 (PMU stall breakdown) — RESULT:** the gap is **`other_interlock_stall` 2.12× XMRig**
>   (+12.30 M cycles/hash = 154% of the gap) = A53 integer-multiplier interlocks on
>   dependency-dense superscalar code. Cache/branches/loads/external all exonerated.
> - **E20 (distance-3 superscalar peephole) — IMPLEMENTED, NULL (−0.2%), REVERTED.** Fires only
>   0.73% of slots; a peephole can't move a 5% gap. **Scheduling is DEFERRED, not dead**: a full DAG
>   list scheduler is parked until the XMRig gap closes by other means, then revisited as a *forward*
>   lever (go-past-parity). Do not widen the peephole (distance-4/5); that family is exhausted.
> - **E21 (`umov`/NEON vs `LDR` pool) — CLOSED as false premise.** The hypothesized emission
>   difference does not exist: neither miner uses `umov` in the superscalar C* path. See perf-tracking
>   §1. Combined with E19 (XMRig schedules nothing, yet 2× better interlocks), **neither emission
>   choice nor ordering is the dominant cause** of the stall excess — the difference is elsewhere.
> - **Region split:** a `-g` profile puts **97.89% of cycles in the JIT buffer** and only **1.84% in
>   all armrx C++ combined**. The W1-1 "named C++ 9.5%" and a suspected "`worker_loop` 10.67%" were
>   both artifacts of profiling a *stripped* binary (E17).
> - **E18 (cross build) — KEEP.** The GCC 16.1.0 cross build is **+7.9%** over the GCC 15.2.0
>   device build on identical source (4.66 vs 4.32 H/s), KATs byte-identical. LTO-off measured
>   −1.9% and `-mtune=cortex-a53` measured null, so the compiler version is the whole effect.
>   Deployment/toolchain lever, not a code change.

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
