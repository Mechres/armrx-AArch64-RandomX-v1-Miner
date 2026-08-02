# armrx — Performance Log (per-iteration metrics)

**Append ONE row per measured iteration.** Protocol: device Cortex-A53 @ fixed 765 MHz,
single-core `taskset -c 3` unless noted, gated `bench_armrx --full-hash-only --perf-ready` +
`perf stat -e cycles,instructions` over a 500-hash window, executed binary md5 recorded.
Live H/s also taken from a plain `taskset -c 3 ./bench_armrx --full-hash-only` run
(median ms → H/s). Host x86_64 ctest 9/9 must stay green (hw branch compiled out there).

## Reference anchors (fixed comparison points — NOT iterations)
| Source | version / commit | instr/hash | cycles/hash | IPC | H/s (1c) | median hash |
|---|---|---:|---:|---:|---:|---:|
| XMRig | 6.26.0 (Kanedias static, `2ac4814a`) | 94.5M | — | 0.648 | 5.05 | ~198.0 ms |
| BSD upstream RandomX ref | `scratch_vm_study/upstream_rx` | 104.8M | — | 1.540 | ~5.5* | — |

\* BSD ref H/s carried `--verify` overhead in W1-4; treat as upper-bound, not like-for-like.

## Iterations
| # | Date | Commit | Change | instr/hash | cycles/hash | IPC | H/s (1c) | median hash | notes |
|---|---|---|---|---:|---:|---:|---:|---:|---|
| 0 | 2026-08-03 | `a1ea83c` | baseline — post-W4, pre-AES | 107.36M | 171.55M | 0.627 | 4.52 (gated) | 220.99 ms | B-M-B-M baseline runs; 500 hashes, core 3, md5 `1a5bb464…` |
| 1 | 2026-08-03 | `2687a2e` | + hardware AESE/AESD funnel (zero-key) | 89.47M | 162.26M | 0.551 | 4.78 (gated) / 4.75 (live bench) | 209.32 ms | KAT hw==T-table 60k blocks; all device gates PASS; md5 `87e63b33…` |
| 2 | 2026-08-03 | `2687a2e` | + E9 hugepages ON (256×2MiB reserved) | — | — | — | 4.79 (live bench) | 208.95 ms | 256MiB cache now on 2MiB pages (smaps KernelPageSize 2048kB); H/s +0.8% vs iter1; TLB was NOT the dominant cost (in-order mem-latency wall dominates) |

## Detail
### Iteration 0 — `a1ea83c` (baseline, post-W4, pre-AES)
- Gated B-M-B-M, core 3, 500 hashes, non-isolated. B runs reproduced to 4 sig figs
  (107.36M both) → measurement stable.
- instr/hash 107.36M, cycles 171.55M (run B2 171.13M), median 220.99 ms.
- This is the Item-0 re-baseline from the residual-gap roadmap. Closes the corrupted
  132.7M ungated figure.

### Iteration 1 — `2687a2e` (+ hardware AES funnel)
- Override `encrypt_transform`/`decrypt_transform` (include/armrx/aes.hpp) with zero-key
  `vaesmcq_u8(vaeseq_u8(s,zero))` / `vaesimcq_u8(vaesdq_u8(s,zero))` + trailing real-key EOR.
  Gated on `__ARM_FEATURE_AES`. ~25 lines in the header; zero edits to aes_hash.cpp /
  aes_generator.cpp / CMakeLists.txt.
- **Correctness:** `tools/aes_kat_check` (extended oracle) hw == T-table over 60,000 random
  blocks (qemu + device); `test_aes_hash` golden pins; `test_mining` real shares;
  `test_jit_equivalence` 16/16; determinism; dataset_2way; encodings — ALL PASS.
  Host x86_64 ctest 9/9 unchanged (hw branch compiled out).
- **Perf (gated M run, core 3, 500 hashes):** 89.47M instr (−16.7% vs iter 0),
  162.26M cycles (−5.4%), median 209.32 ms (−5.3%). Live `bench_armrx --full-hash-only`
  confirmed 4.75 H/s, median 210.72 ms.
- **Key finding:** armrx now emits FEWER instructions than XMRig (89.5M < 94.5M) and the BSD
  ref (104.8M). But IPC dropped 0.627 → 0.551 (removed a high-IPC T-table region). Real H/s
  gap to XMRig is now ~6% (4.75 vs 5.05) and is a **cycle-efficiency** gap, not instructions.
  → instruction-count hypothesis falsified; frontier is now IPC/cycle efficiency.

### Iteration 2 follow-up — E1 (re-attribution) + E2 (scratchpad-real vs L1)
- **E1 (region attribution, `--attribution-only`, core 3, current build + hugepages):** full hash
  209.1 ms (4.78 H/s). Phase split: blake2b seed 0.00% / init_scratchpad(AES) 0.34% /
  **chain 7xrun+7xblake2b 86.64%** / final run 12.36% / get_final_result 0.52%.
  → AES is dead as a bottleneck (<1% combined). The chain dominates.
- **E2 (scratchpad-real vs L1 under perf, 2000 execs each, core 3):**

  | metric | REAL (2MiB) | L1 (16KiB) | Δ |
  |---|---:|---:|---:|
  | cycles | 47.31B | 44.72B | −5.5% |
  | instructions | 26.221B | 26.218B | −0.01% (identical) |
  | cache-misses | 190.3M | 113.0M | −40.6% |
  | IPC | 0.554 | 0.586 | +5.8% |

  → Eliminating 77M cache misses (whole scratchpad→L1) saves only 5.5% cycles / +5.8% IPC.
  **~94% of the chain is COMPUTE/IPC-bound, not memory-latency-bound.** The lever is instruction
  scheduling / dual-issue efficiency of the main-VM chain (IPC 0.554), NOT memory tricks.
  Memory ceiling (E9 +0.8%, E2 +5.8% IPC) is secondary. → Next: E11 (measured dual-issue analysis).

### E11 — Dual-issue analysis + XMRig AArch64 codegen comparison (DONE 2026-08-03 — loss is CODEGEN, not structural)
- Captured live JIT buffer (118,784 B, RWX from `/proc/<pid>/mem`), disassembled on host
  (aarch64-linux-musl-objdump), analyzed 7795 instructions. Two independent checks agreed.
- **Robust finding:** of 1234 LOADs, **83.1% are immediately followed by an instruction that
  consumes the loaded register** (classifier 82.6%, pure-text register check 83.1% — agreement).
  Only 16.9% of loads are followed by an independent op.
- **CORRECTION:** the 83% serial-load pattern is NOT structural. Cloned XMRig
  master, diffed `src/crypto/randomx/jit_compiler_a64_static.S` vs armrx's (620-line diff). XMRig's
  `randomx_calc_dataset_item_aarch64` (dataset derivation, 16,384×/hash = bulk of the chain) batches
  the superscalar constant loads via **`ldp` (load-pairs) with interleaved `eor` consumers**:
    XMRig: `adr x7,superscalarMul0` / `ldp x12,x13,[x7]` / `eor x1,x0,x13` / `ldp x12,x13,[x7,16]` / ...
  armrx's `rx_calc_dataset_item` instead does **serial `ldr`-then-consume**:
    armrx: `ldr x12,superscalarMul0` / `eor x1,x0,x12` / `ldr x12,superscalarAdd1` / `eor x1,x0,x13` / ...
  ⇒ the 83% is a **codegen choice armrx made**, not an algorithm necessity. XMRig keeps the A53 load
  port busy (batched/pipelined) where armrx stalls it serially. (armrx's *main VM loop template* IS
  pipelined — gap is specifically the dataset-derivation path.) Also: armrx has a Track-D1 "2-way
  interleaved derivation" block but it is **explicitly unwired from the live mining path**.
- **Revised conclusion:** the ~6% gap (0.551→0.648 IPC) looked attackable via load-batching in
  `rx_calc_dataset_item` per XMRig's pattern. E12 tested exactly that and came back **NULL** (below).

### E12 — Dataset-derivation load-batching, implemented + measured (DONE 2026-08-03 — NULL, precisely understood)
- Rewrote `rx_calc_dataset_item` in `jit_compiler_a64_static.S` to pipeline the 8 superscalar-constant
  loads ahead of their `eor` consumers. Two bugs found/fixed during implementation:
  1. `adr x13, superscalarMul0` + `ldp [x13]` **segfaulted** — the `superscalarMul0..Add7` `.quad`
     literals are **past `randomx_init_dataset_aarch64_end`** (outside the JIT `CodeSize` copy window),
     so `adr`→garbage post-memcpy. Original `ldr x12,<sym>` works only via in-bounds literal pool.
     Fix: pooled `ldr` (no `adr`).
  2. Using x14–x17 as load temps **segfaulted** (qemu gdbstub: `stp x4,x5,[x17]`, x17 = superscalarAdd4
     value). JIT main program keeps x14–x17 **live across the call**; prologue only saves x0–x13, so
     x12/x13 are the only caller-safe clobberable regs. Fix: x12/x13 only (depth-2 overlap).
- **RESULT:** gates GREEN (test_jit_equivalence 16/16 byte-identical, test_mining real shares,
  test_aes_hash). Live bench **4.75 H/s, median 210.3 ms** = baseline (4.75 H/s, 210.7 ms).
  perf A/B: **IPC 0.552 vs 0.554** — no instruction/cycle change. ⇒ **E12 NULL.**
- **Why null:** load stream is **dependency-bound, not load-port-bound** — each `eor` waits 3 cycles
  for its constant regardless of overlap; deeper overlap (x14–x17) is forbidden by the call convention.
  The "simple load-batching" hypothesis is exhausted. The 0.551→0.648 IPC gap lives elsewhere
  (main-VM program emission — E13). Unlike prior blind nulls, this one is *precisely understood*.

### E13 — Locate the IPC gap: scratchpad memory-op emission (DONE 2026-08-03 — localized, forbidden)
- Captured the live JIT buffer from a **real-mining** run (RWX `ffff9062c000`, 118,784 B = the static
  template; same code as the E11 scratchpad-bench buffer) and disassembled (7907 instr). Classified
  every bracketed `ldr`/`ldp` by base register + immediate-consumer rate:
  - scratchpad (x2): **14 loads, 64.3% serial** (load→immediate consume) — the main-VM scratchpad path.
  - dataset (x1/x20): 1 load, 0% serial — already pipelined.
  - NEON vector + PC-relative literal loads (15): 0% serial — not relevant.
  - Cross-check: E11 `--scratchpad-real` buffer gave 71.4% scratchpad-serial; the real-mining 64.3%
    is authoritative (scratchpad-bench does not materially skew it).
- **Conclusion:** the 0.551→0.648 IPC gap lives in **scratchpad memory-op emission** (`ldr x2 →
  immediate consume` 64% of the time). This is exactly the **`*_M` memory-op scheduler region** that
  AGENTS.md records as having **diverged and been reverted** (real JIT/interpreter mismatch). RandomX
  scratchpad ops are `read → transform → write back` with the transform depending on the loaded value,
  so there is little independent work to hoist into the 3-cycle bubble, and reordering risks divergence.
  Dataset path, dataset-block read, and NEON AES are already pipelined — so the prior 4 nulls + E12
  all attacked pipelined or forbidden regions.
- **CORRECTED by real-pool like-for-like test (2026-08-03):** the IPC delta (0.551 vs 0.628) is a
  **phantom real-world deficit**. On the herominers pool, 8 workers, NO isolcpus, same silicon:
  - **XMRig = 27.77 H/s** (settled by 60s; per-core 4.5 fast-cluster / 2.4 weak-cluster)
  - **armrx = 21.4 H/s** (settled ~150s; ~23% behind XMRig)
  - **armrx WITH isolcpus = ~28.4 H/s** (== XMRig)
  So the gap is NOT codegen or the `*_M` scratchpad path — it is armrx's **non-isolated default
  behavior**: (a) hugepages — XMRig auto-acquires them ("huge pages 100% 8/8"); armrx's E9 showed
  `MAP_HUGETLB` fails without root-reserved pages (falls back to 4 KiB); (b) affinity — non-isolated
  armrx lets the OS scatter workers onto weak cores 4-7 (documented ~half-throughput penalty) while
  XMRig rides the fast cluster. E15 investigated both obvious non-isolated causes (hugepages,
  affinity) and **ruled both out**: armrx already pins correctly (AffinityMode::All → 1:1 to
  core_order_, which degenerates to 0..7 on this no-cpufreq device, coincidentally correct) and as
  root already acquires 128×2 MiB hugepages (HugePages_Free 256→128) yet only reaches 22.9 H/s (+1.5).
  So the ~18% residual gap's cause is **open** (real-program hash-loop efficiency vs XMRig, or
  8-worker threading/contention overhead — not yet measured). **E14** (root-cause `*_M`) de-prioritized
  — even a perfect IPC fix wouldn't move real-world H/s. Next step: a clean settled per-core A/B
  (armrx 1w core 3 vs XMRig 4.53 H/s) before any code.

## How to read deltas
- instr/hash: lower = leaner. We won this vs both references (iter 1).
- cycles/hash + IPC: the per-cycle gap to XMRig (0.551 vs 0.628) is REAL in microbenchmarks but is a
  **phantom real-world deficit** — it does not show up as a real-world H/s gap (both are interconnect/
  cluster-bound at 765 MHz). The non-isolated gap (E15) had its two leading hypotheses (hugepages,
  affinity) **ruled out**; its cause is open pending a clean settled per-core A/B.
- E2 reframes: the gap is instruction-scheduling (dual-issue), not memory latency — but that gap is
  microarchitectural only; system-level throughput is gated by the two-cluster topology + page size.


- H/s: the only number that pays. ~5–5.5/core is the architectural wall on this silicon.
