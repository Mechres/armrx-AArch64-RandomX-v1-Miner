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

## Explicitly dead (do NOT reopen — recorded so nobody re-discovers)
- Superscalar NEON register pool (all 32 v-regs live across the loop; register-infeasible).
- Superscalar density (post-W4 body at A64 ISA floor; w23's list exhausted).
- PRFM hints (T2-1, regression), dual-issue padding (T2-2, regression), PGO (null ×2),
  Track C wrapper (≤0.5%), CBRANCH CSEL (regression), D1/D3 interleaves.
- Memory-op (`*_M`) scheduler extension in its original form (W3-2 divergence).

## Meta
- 3 weeks of agent work got armrx to ~parity-with-XMRig on a part XMRig devs never tuned for;
  XMRig is 9 years old. The instruction-count fight is won. The cycle-efficiency fight is open.
- Next physical step recommended: **E1** (re-attribution) — it re-baselines everything else.
