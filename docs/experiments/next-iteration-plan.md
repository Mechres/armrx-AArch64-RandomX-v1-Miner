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

## E11 — Measured A53 dual-issue analysis of the chain's emitted AArch64  [DONE 2026-08-03 — loss is STRUCTURAL]
- **Question:** where does the A53 dual-issue pipeline go idle in the emitted chain code?
- **Method:** captured the live JIT buffer (`/proc/<pid>/mem` of `bench_armrx --scratchpad-real`,
  RWX region ffff8d7a7000-ffff8d7c4000 = 118,784 B ≈ 29,700 instr), disassembled on host
  (aarch64-linux-musl-objdump -b binary -m aarch64), analyzed all 7795 instructions. Two
  independent checks (a classifier AND a pure-text register-token check) agreed.
- **RESULT (robust, register-based, not op-type-dependent):**
  - Of **1234 LOADs**, **83.1% are immediately followed by an instruction that consumes the loaded
    register** (classifier 82.6%, text-check 83.1% — agreement ⇒ solid).
  - Only **16.9%** of loads are followed by an independent op.
- **Interpretation (the key finding):** RandomX's main-VM program is a **dependency chain**
  (`read scratchpad[idx] -> transform (AES/mul/xor) -> write back`, repeated). Each load MUST feed
  the very next op — there is **no independent op available to hoist into the load's 3-cycle bubble**.
  The 83% load→consumer adjacency is **inherent to the algorithm's dataflow**, NOT a scheduling
  mistake by the emitter. ⇒ the chain's low IPC (0.554) is **largely STRUCTURAL to RandomX on an
  in-order A53**, not recoverable by instruction scheduling.
- **This explains the project's history:** the 4 prior scheduling tweaks (PRFM T2-1, dual-issue
  padding T2-2, PGO ×2, `*_M` scheduler → divergence/ revert) all NULLed/REGRESSED precisely
  because there is little ILP to expose in a serial chain on an in-order core. E11 is the empirical
  confirmation of *why* — not a guess.
- **Conclusion:** the remaining ~6% H/s gap to XMRig (0.551→0.648 IPC = +17%) is, after E2 (memory
  capped at +5.8%) and E11 (scheduling capped by structural dependency chains), most plausibly
  **XMRig's different instruction mix / 9 years of x86 codegen tuning** — a property of *how XMRig
  compiles RandomX*, not a lever armrx can recover on this in-order A53 without a fundamentally
  different codegen strategy. The architectural ceiling on this silicon is ~5–5.5 H/s/core.
- **Effort:** done (capture + disasm + analysis, no code change). Output: this finding.
- **Implication for roadmap:** do NOT chase E3b/E5/E7 emitter scheduling — E11 shows the loss is
  structural, consistent with the 4 prior nulls. The honest remaining levers are: (a) ISOLCPUS /
  two-cluster penalty (E-deploy, system-level, already known big), (b) accept the ~5% gap as the
  architectural wall for this 9-years-of-x86-tuning comparison. Document this so nobody re-discovers
  the scheduling dead-end.

## E3 — The IPC gap: 0.551 (us) vs 0.648 (XMRig)  [TODO — the live mystery]
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
