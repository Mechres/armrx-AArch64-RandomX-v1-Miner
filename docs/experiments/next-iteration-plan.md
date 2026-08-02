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

---

## E1 — Re-run region attribution on the current build  [TODO — recommended next]
- **Question:** What is the *real* post-AES per-region instruction + cycle split?
- **Why it matters:** Our current decomposition (superscalar ~84M / AES ~10.7M / mainVM ~11.8M /
  blake2b ~0.7M) is **arithmetic from pre-AES numbers** (W1-1 census, 2026-08-01). We never
  re-measured after W4 + AES. The AES region just shrank ~9M instr / ~8M cycles — that energy
  didn't vanish, it redistributes. We are optimizing blind until we re-baseline the search space.
- **Measurement:** `bench_armrx --attribution-only` on device (post-AES build), plus a fresh
  `perf stat` region split if the tool supports it; else the `--scratchpad-real`/`--scratchpad-l1`
  pair (E2) gives the memory-latency slice.
- **Hypothesis:** AES region now ~1–2M instr; superscalar still dominant; the *cycle* share of
  main-VM likely grew as a fraction.
- **Effort:** ~15 min device, **no code change**.
- **What a result tells us:** re-opens the search with real data instead of my arithmetic. Gates
  E2/E3/E5 (whether the superscalar IPC is actually exposed, whether main-VM is the cycle sink).

## E2 — scratchpad-real vs scratchpad-l1 under perf  [TODO]
- **Question:** How much of our cycles are *genuine DRAM latency* vs *our access pattern*?
- **Why:** `-scratchpad-l1` aliases the 2 MiB scratchpad to 16 KiB L1 — removes DRAM entirely.
  If IPC jumps, our access pattern / working set is the lever (fixable). If it doesn't, the
  cycles are inherent RandomX light-mode memory latency (the 2.2× main-VM penalty both impls pay).
- **Measurement:** both flags under `perf stat -e cycles,instructions,L1-dcache-misses,
  cache-misses` on device, core 3, md5-recorded binary.
- **Hypothesis:** partial jump — some penalty is pattern, some is inherent.
- **Effort:** ~5 min device, no code change.
- **What a result tells us:** whether the 6% H/s gap is attackable in code (pattern) or a wall
  (inherent DRAM). Directly scopes E3/E7.

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

## E9 — Hugepages actually allocated? (deployment, FREE, unmeasured)  [TODO — high ROI]
- **Source:** XMRig RandomX Optimization Guide (xmrig.com/docs/miner/randomx-optimization-guide):
  "Huge Pages can increase RandomX performance up to 50%; 1GB huge pages +1-3%."
- **Question:** armrx has `MAP_HUGETLB` + `MADV_HUGEPAGE` ("Memory Tiering" in README), but
  does the device *actually* get 2 MiB hugepages for the scratchpad/cache? `getconf HUGETLB`
  / `cat /proc/meminfo | grep Huge` / a runtime assert in `allocate()` would confirm. If the
  allocation silently falls back to 4 KiB pages, RandomX (which is TLB-pressure-heavy: 256 MiB
  cache + 2 MiB scratchpad per thread) pays huge TLB-miss penalties on an A53 with a small TLB.
- **Measurement:** check `/proc/meminfo` HugePages_*, and/or add a one-line log of the actual
  mapping (hugetlb vs anon) at startup. If 4 KiB: enable hugepages on device (`sysctl vm.nr_hugepages`
  or mount `hugetlbfs`) and re-bench.
- **Why high ROI:** XMRig credits up to 50% to hugepages; even recovering a fraction is free and
  we have NEVER verified armrx gets them on this device. Likely the single biggest untested lever.

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
