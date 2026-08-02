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

## How to read deltas
- instr/hash: lower = leaner. We won this vs both references (iter 1).
- cycles/hash + IPC: the real throughput lever now. XMRig 0.648 vs our 0.551 is the open gap.
- H/s: the only number that pays. ~5–5.5/core is the architectural wall on this silicon.
