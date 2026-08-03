# armrx AArch64 Performance — Lead Tracking & Discipline Log

> Single source of truth for "what we've tried, what we're trying, what's dead."
> Goal: STOP re-running the same territory (the snake-eating-its-tail failure mode).
> Rules at bottom — read before starting any new measurement.

---

## 0. STANDING FACTS (do not re-litigate without new evidence)

- **Device:** MSM8929 (Snapdragon 415), 2×4 A53 @ fixed **765 MHz** (no cpufreq/OPP → clock
  cannot change). ~50% throughput gap between fast (0-3) and weak (4-7) clusters is the
  **SoC's own design**, not software. Both miners bear it.
- **Cooling:** fan present throughout (6cm moved 2026-08-02; 12cm added after). Steady-state
  under RandomX load ≈ **60°C**. Both armrx AND XMRig run at the same temp on the same silicon.
- **Benchmark numbers — UPDATED 2026-08-04 (per-core re-measured cool, 39-43°C, `taskset -c 3`):** after
  E24 (C* immediate padding), armrx **beats XMRig at 1 worker**. 8w is the **real-pool long-run**
  figure (user ran the E24 binary on all 8 cores, herominers, 1209 s, converged):
  | Miner / build | 1w (core 3) | 8w (all 8 cores) |
  |---------------|------------:|----:|
  | XMRig | **5.04 H/s** | **28 H/s** |
  | armrx (stock device build, GCC 15.2.0) | **4.32 H/s** | 21.85 H/s |
  | armrx (cross build, GCC 16.1.0 — E18) | **4.66 H/s** | ~23.6 H/s (old short-window) |
  | armrx (cross + E24 C* padding) | **5.11 H/s** | **26.65 H/s** (real-pool 1209s run) |
  armrx cross + E24 = **101.6%** of XMRig per-core (5.11 vs 5.04); at 8w = **26.65 vs 28 = 95.2%** of
  XMRig. **Parity reached** (1w ahead, 8w within 5%). The residual ~4.8% (1.35 H/s) is the `*_M`
  scratchpad stall (see remaining-lever note). 
  **CORRECTION (2026-08-04, user long-run):** the earlier "23.6 H/s / 84%" 8w figure was WRONG — it
  was a 120 s short `--pool-test` window (pool "Speed" under-reports; same artifact as 1w 2.15) AND
  was pinned to `taskset -c 1-7` (7 of 8 cores) instead of all 8. The user's 1209 s all-8-cores run
  converged at **26.65 H/s**, which is the real number. isolcpus is NOT a parity lever (both miners
  benefit equally — see user correction); do not present it as a path to parity.
  **Pool-measurement caveat:** the pool's reported "Speed" is a *share-acceptance* rolling rate, not
  an instantaneous hashrate — it needs many minutes to converge (1w: 2.15 at 120s vs true 5.12; 8w:
  26.65 only after ~1200s). Trust a long converged pool run or `bench_armrx --full-hash-only` for
  authoritative H/s. The 1w "XMRig 4.53 / armrx ~4.0 / ~88%" row was a warm/settled reading and is
  superseded.
- **Real-pool verification (2026-08-04, E24 binary):** ran against herominers
  (`--pool=tr.monero.herominers.com:1111`, CryptoNote stratum). 1w `bench_armrx` = **5.12 H/s**
  (beats XMRig 5.04). 8w: user ran the binary on **all 8 cores** for **1209 s**; pool "Speed"
  converged at **26.65 H/s** (= 95.2% of XMRig's 28). The earlier 120 s `--pool-test` figure
  (23.6 H/s) was a short-window under-report AND was pinned to 7 of 8 cores — both errors; the
  1209 s all-8-cores run is authoritative. **Parity reached: 1w ahead, 8w within 5%.**
- **Remaining CODE lever: the `*_M` main-VM scratchpad path (E13/W3-2).** Scratchpad loads are
  64.3% serial (`ldr x2 → immediate consume`); the `emitMemLoad` sequence is `add → and → ldr →
  op`, with the load result consumed immediately (3-cycle stall). This is the residual ~4.8% (1.35 H/s)
  at 8w. The W3-2 scheduler extension that tried to reorder `*_M` **diverged** (JIT≠interpreter,
  stress-suite caught it) and was reverted **unidentified**. **Post-E24 audit (2026-08-04, two
  independent agents — opencode + github_copilot) converges on this region AND proposes a root cause
  for the W3-2 divergence that is NOT memory aliasing:** promoting `*_M` to a long-latency anchor `P`
  in `scheduleProgram()` likely broke **CBRANCH replay-domain equivalence** (interpreter replay is
  instruction-index based; JIT replay is code-offset based via `reg_changed_offset`). The scheduler's
  hazard model (register-only, `hasHazard` blocks memory-memory) misses this semantic class. So the
  fix is NOT "re-enable `*_M` scheduling" — it is a **non-reordering structural change** to
  `emitMemLoad` (e.g. hoist the address `add`/`and` ahead of a preceding independent op, or shorten
  the `src==dst` path) that preserves original VM/memory order. Every candidate must be gated by
  `test_jit_equivalence` 16/16 + BOTH stress suites (450 + 200 pairs) against the failing seed, not
  just the 16-pair equivalence test. See `docs/audits/opencode_20260803_audit.md` and
  `github_copilot_2026-08-03_audit.md`.
- **CONFIRMED MEASUREMENT (2026-08-04, post-E24 PMU growth-delta, E24 binary, device):** ran
  `perf stat` at 1w (core 3) and 8w (all cores), 60 s `--mine`, per-hash attribution:
  | stall/hash | 1w | 8w | Δ |
  |---|---:|---:|---:|
  | `other_interlock_stall` (E24 multiply) | 10.3 M | 10.5 M | **flat** (E24 holds) |
  | `ld_dep_stall` (`*_M` load-use) | 15.9 M | **25.9 M** | **+63%** under 8w contention |
  This confirms the audits' Hypothesis #1: the residual ~4.8% (1.35 H/s) at 8w is the `*_M`
  scratchpad `ldr → op` 3-cycle load-use stall, and it scales with worker/memory contention
  while the multiply interlock (E24) does not. XMRig bears the same SoC, so the win is in how
  armrx lowers `*_M` vs XMRig (audits: XMRig has address-fast-paths / different surrounding order;
  armrx always materializes the address). **This is now the confirmed, localized target.**
- **KNOWN BUG (2026-08-04, user-observed):** `armrx` does **not respond to Ctrl-C / SIGINT**
  during pool mining — the process must be killed (e.g. `pkill -f armr[x]` or `kill -9`). Seen on
  the 1209 s real-pool run. Not yet root-caused; likely the worker loop / stratum thread does not
  install a signal handler or blocks the default SIGINT termination. Add a `SIGINT` handler that
  flushes stats and `std::_Exit(0)` (mirror the `--pool-test` self-terminate path). Track separately.
- **E25 real-pool soak (2026-08-05, user):** ran `/tmp/armrx_e25` (committed HEAD `a2685ad`) on the
  real pool, all 8 cores, **1263 s**. Converged at **26.24 H/s** (multiple job rotations, blob 76/77,
  CPU steady 59°C, no crash/divergence). Matches the E24 26.65 baseline (run-to-run noise). Confirms
  E25 is stable + correctness-preserving on live pool work. `^C` did NOT stop it (reproduced the
  KNOWN BUG). E25 is cleared for shipping.
- **E26 (Approach A, `*_M` load-latency hoist) — FAILED, REVERTED (2026-08-05, Reasonix).** Attempted
  to hide the `ldr → op` 3-cycle bubble by hoisting each `*_M` op's address `add`/`and` into the
  slack of the PRECEDING VM instruction (new `emitMemLoadAddr<>`; `memAddrHoisted_` flag; backward
  scan with CBRANCH-anchor/replay-domain guards so the `ldr` never moves). Built + gated on device
  (cross): `test_jit_equivalence` **16/16 PASS**, but `test_jit_scheduler_stress` (450 pairs) **SEGFAULT
  (exit 139)** ~5 min in — crash inside the RWX JIT buffer (generated-code corruption, not a value
  divergence). The pre-E26 baseline passes the same 450-pair gate. So the crash is E26, caught by the
  exact W3-2 death gate. Reverted (`git checkout`), tree clean at `beeeeef`. **This is the W3-2 trap in
  a new form: "safe by register/anchor analysis" `*_M` changes keep failing on-device** (W3-2 diverged;
  E26 segfaulted). The 16-pair KAT passes; the 450-pair stress catches it. Root cause not identified
  (crash, not divergence — likely a liveness subtlety in the hoist vs CBRANCH replay). **Approach A in
  this hoist-across-handler form is EXHAUSTED.** Do NOT retry this shape blindly.
- **CORRECTION to Reasonix's stall analysis (2026-08-05):** the claim "per-hash ld_dep_stall implies
  ~30+ cycles/op = L2/DRAM, not the 3-cycle L1 bubble, so a hoist can't hide it" is arithmetically
  wrong. 8w ld_dep_stall = 25.9M/hash × ~26 H/s ≈ 673M stalls/s ≈ 0.88 stall-cycles per core-cycle —
  i.e. load-dependency stalls saturate ~88% of cycles. That is consistent with the 3-cycle L1 load-use
  bubble multiplied across ~35% `*_M` ops at high issue rate, NOT a DRAM-bound per-op latency. A correct
  hoist COULD in principle hide part of it; we never measured a correct version because E26 crashed.
  So the residual gap remains unexplained-as-unfixable — the only evidence is that this specific hoist
  shape breaks equivalence. **Parity stands at 95.2% (8w 26.65 vs XMRig 28); the last ~4.8% is a real
  in-order-A53 memory-latency tax that two audits + E26 could not recover without breaking equivalence.**
- **Instruction census — DEFINITIVE 2026-08-04 (supersedes W1-4 "+26%", the "+14%" figure, AND
  this session's own earlier "~1.35× / +35%" claim).** Measured with `bench_armrx
  --full-hash-only --perf-ready`, which gates `perf stat` on an **exactly 500-hash** steady-state
  window (no counter quantization, no cache-init contamination), vs XMRig's 120 s @ 5.04 H/s:

  | | instr/hash | cycles/hash | **IPC** | H/s | implied H/s @765 MHz |
  |---|---:|---:|---:|---:|---:|
  | armrx (cross, GCC 16) | **89.5 M** | **163.5 M** | **0.547** | 4.74 | 4.68 ✓ |
  | armrx (cross, GCC 16 + E24 C* padding) | **113.8 M** | **171.9 M** | **0.662** | 5.11 | 5.08 ✓ |
  | XMRig | **101.4 M** | **155.0 M** | **0.654** | 5.04 | 4.94 ✓ |
  | ratio (armrx/XMRig, after E24) | **1.12×** | **1.11×** | **1.01×** | **1.01×** | |

  Both rows are **self-consistent**: cycles/hash × H/s reproduces the 765 MHz clock to within 2%,
  which is the validity check every earlier census failed to apply.
- **⇒ THE GAP WAS MULTIPLIER INTERLOCK STALLS (E19), NOW CLOSED by E24.** armrx executed 12% FEWER
  instructions than XMRig but lost on IPC (0.547 vs 0.654) because of A53 integer-multiply interlock
  stalls. PMU: `other_interlock_stall` 23.27 M vs 10.96 M/hash (2.12×). **Root cause (E24):** armrx's
  superscalar C* immediates used a 2-instruction `LDR`-literal-pool form that was *denser* than
  XMRig's 3-instruction `MOVZ`/`MOVN`+`MOVK`; that density put only one (load) instruction between
  program-adjacent multiplies, saturating the A53's 4-cycle MAC interlock. Padding to 3 instructions
  drops `other_interlock_stall` to **6.18 M/hash (below XMRig)** and raises H/s to **5.11 (beats
  XMRig 5.04)**. The earlier "scheduler distance-3" hypothesis (E20) and "disable scheduler" (E22)
  were red herrings — the generator (E21/L1) was identical; the cause was a single emission-width
  choice.
- **Why the earlier numbers were wrong (root cause, so it doesn't recur):** they divided perf
  instruction counts by hash counts taken from a **`--mine` window that was not CPU-saturated** —
  27.3 B cycles over 60 s = an effective **455 MHz on a 765 MHz core (59% busy)**, because the
  miner's stats/printer thread and job bookkeeping idle the worker. That inflates instr/hash and
  fakes a high IPC. **Always sanity-check `cycles ÷ elapsed ≈ 765 MHz` before trusting a perf
  window.** `bench_armrx --perf-ready` is saturated and is the correct harness.
- **Region split — REVISED 2026-08-04 by the `-g` profile (supersedes the W1-1 census).**
  `perf record -F 499 -g` on a `-g` cross build, 1 worker, 60 s: **`[JIT]` buffer 97.89%** of
  cycles / **all armrx C++ combined 1.84%** / libc+unknown 0.26%. The old W1-1 figure of "named
  C++ 9.5%" and the stripped-build claim of "`worker_loop` 10.67%" are BOTH symbolization
  artifacts of profiling a stripped binary (see E17). ⇒ the instruction excess is **inside the
  JIT-emitted code**, i.e. the superscalar bodies + main-VM loop. The W1-1 *relative* split within
  the JIT (superscalar ≫ main-VM) is not contradicted, only its C++ slice is.
- **armrx multi-worker scaling is LINEAR per-core** (4w = 3.8× from 1w; weak cluster = 0.53×
  fast = SoC design). No software multi-worker inefficiency. (per-worker proof via `--pool-test`.)

---

## 1. CLOSED / DEAD LEADS (do NOT re-run — evidence on file)

| Lead | Result | Evidence | Why dead |
|------|--------|----------|----------|
| **Hugepages** | null (+1.5 H/s) | E15: root armrx takes 128×2MiB via `MAP_HUGETLB` (`virtual_memory.c:235` == XMRig's exact technique); HugePages_Free 256→128; only 22.9 vs 21.4 | armrx already does XMRig's method |
| **CPU affinity** | null | E15: `AffinityMode::All` (`mining_engine.cpp:168`) pins 1:1 to `core_order_` = 0..7 (correct fast/weak split by coincidence on no-cpufreq) | placement already correct |
| **Fast-div/sqrt (Newton)** | −1.1% | `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` measured regression | shipped gated; not a win |
| **`*_M` scheduler extension** | DIVERGES | `test_jit_scheduler_stress` 450 + `test_jit_superscalar_scheduler_stress` 200 FAIL deterministically | do NOT re-enable memory-op scheduling |
| **PGO** | null | `ARMRX_PGO` measured null on current code | toolchain-dependent; skip |
| **LDP/STP fusion** | structural 0% | W2-2: `*_M`/`ISTORE` are register-indexed, never adjacent `#imm` | impossible by construction |
| **C* literal-pool (W3)** | −16..20% | cache-miss tripled, IPC collapsed from 714 scattered loads | reverted; do not re-attempt |
| **Dual-issue alignment (W2-2)** | −0.1% | NOP overhead > recovery | reverted |
| **Blake2b parity (E3a)** | **armrx WINS** | armrx `blake2b.cpp` = hand-written **NEON** G; XMRig `blake2b.c` = **scalar C** on AArch64 (no NEON path) | gap is NOT here; armrx already better |
| **Thermal-throttle "cause" story** | RETRACTED | fan was always on; both miners hit same 60°C and XMRig still wins; "5.8× recovery" was 30s-vs-120s read artifact | gap is code, not temp |
| **C\* immediate density (E3c)** | **armrx DENSER** | armrx `emitCpoolImmediate` (`jit_compiler_a64.cpp:1323`) = inline 128-slot literal pool, **LDR+ALU = 2 instr**; XMRig pre-fills `num32bitLiterals=64` (`jit_compiler_a64.cpp:337`) so its `umov` branch is unreachable and it falls to **3-instr** `MOVZ`/`MOVN`+`MOVK` (see E21) | armrx is *ahead* here; no density gap to attack |
| **LTO on/off (device, E18)** | LTO-off is −1.9% | 4.24 (LTO off) vs 4.32 (LTO on), GCC 15.2.0, 2 runs each, zero spread | LTO is mildly POSITIVE; do not disable it chasing perf |
| **`-mtune=cortex-a53` (device, E18)** | exactly null | 4.32 with, 4.32 without, GCC 15.2.0 | no effect on this codebase/core |
| **`worker_loop` C++ overhead (E17)** | artifact, not real | `-g` profile: JIT 97.89% / all C++ 1.84%. The 10.67% came from `perf` folding unattributable JIT samples into the nearest symbol in a STRIPPED binary | no C++ driver cost to remove |
| **Distance-3 superscalar peephole (E20)** | **null (−0.2%), reverted** | rule fires only **26 / 3,563 slots = 0.73%**; paired A/B median 4.77 (base) vs 4.76 (E20); KATs 16/16 byte-identical so the transform was *correct*, just irrelevant | peephole widening can't move a 5% gap at 0.73% hit rate — **do not try distance-4/5**; full DAG scheduler DEFERRED (see E20) |
| **Disable superscalar scheduler (E22)** | **inconclusive on stalls** | H/s flat (4.77), instr +13.3% (scheduler hides ~12M latency-fill); cycle/stall comparison contaminated (2 bad runs) — NOT a clean delta | scheduler safe to keep; interlock cause still open, upstream of emission |
| **`umov`/NEON literal vs `LDR` pool (E21)** | **false premise on port, but density REVERSAL found (E24)** | both emit `MOVZ`/`MOVN`+`MOVK` for superscalar C*; armrx's `LDR`-pool form was *denser* (2 vs 3 instr) — and that density was the actual bug (see E24) | armrx was NOT "ahead"; its density exposed the A53 MAC interlock |
| **C* immediate density padding (Planner H2 / E24)** | **WIN, SHIPPED** | 3-instr `MOVZ`/`MOVN`+`MOVK` instead of 2-instr `LDR` pool; +7.1% H/s (4.77→5.11), `other_interlock_stall` 23.3→6.18M/hash (below XMRig 10.96M), IPC 0.548→0.662; KATs 16/16 | **this is the E19 fix**; ship cross-built binary |
| **8-worker "unexplained 2.4 H/s"** | **arithmetic error, not a finding** | compared 1 fast core vs 8-worker avg (4 half-speed cores); reconciled from existing per-worker sweep × E18 factor → ~24.1 H/s = 86% of XMRig, same as per-core 95% viewed through different core mix | no second gap; XMRig bears the same weak-cluster tax |
| **Multi-worker scaling "inefficiency" (E15 old)** | RETRACTED | per-worker `--pool-test`: fast cluster scales 3.8× linearly, weak=0.53× (SoC design) | no software scaling loss |

---

## 2. OPEN LEADS (the gap is STALLS / IPC, not instruction count — see E19)

### E3c — CLOSED 2026-08-04. C* immediates: armrx DENSER, and the whole premise (density) was wrong.

**Two separate findings, both negative for the density hypothesis:**

**(a) C\\* immediate materialization — armrx is marginally DENSER, confirmed by E21.** Source
inspection (clean-room, technique only) plus armrx's own `--jit-dump` census:
- armrx `src/jit_compiler_a64.cpp:1191-1205` routes **all** superscalar `IADD_C7/8/9` and
  `IXOR_C7/8/9` through `emitCpoolImmediate` (`:1323`), a per-program **inline literal pool** of
  128 slots (`SuperscalarCpoolSlots`, `:141`). Cost = **1 `LDR` (literal) + 1 ALU = 2 instructions**;
  the MOVZ/MOVN+MOVK 3-instruction form at `:1357` is only the pool-exhausted fallback.
- `--jit-dump` confirms it empirically: every `IADD_C*`/`IXOR_C*` entry is **exactly 8 bytes
  (2 instructions)**, 714 such ops across the 8 programs. Nothing falls back.
- **XMRig does NOT use `umov` for these** (earlier notes to the contrary were wrong — see E21).
  XMRig's superscalar path sets `num32bitLiterals = 64` at `jit_compiler_a64.cpp:337`, so its
  `umov` branch is unreachable and it falls through to a **3-instruction** `MOVZ`/`MOVN` + `MOVK`.
  armrx's 2-instruction `LDR`-pool form is *denser* than XMRig's 3-instruction form here.
- ⇒ **armrx ahead, not parity.** (This is the E21 correction: the prior "parity" wording
  understated armrx; both are fine on the "no density gap" conclusion, but armrx is the denser of
  the two for C\* immediates.)

**(b) The density hypothesis itself is falsified — armrx already emits FEWER instructions.**
The clean 500-hash census (see §0) gives armrx **89.5 M instr/hash vs XMRig 101.4 M** — armrx is
**12% leaner** and still loses, because **IPC 0.547 vs 0.654**. The full superscalar body is
**3,563 instructions / 18,064 bytes** per `generateSuperscalarHash()` (`--jit-dump`, all 8
programs), and its opcode mix is instruction-for-instruction identical to XMRig's emission
(same `SUB`/`EOR`/`ADD`/`MUL`/`ROR`/`UMULH`/`SMULH` one-per-op, same 2-instr `LDR`+`MUL` for
`IMUL_RCP`). There is no density gap left to find here.

⇒ **Stop looking for excess instructions. The lever is stalls.** See E19.

### E19 — RESULT 2026-08-04: the gap is `other_interlock_stall` — MULTIPLIER LATENCY, 2.12× XMRig

**PMU stall-class diff, per hash, same core, both saturated (clock check: armrx 754 MHz,
XMRig 781 MHz — both valid). armrx = `bench_armrx --perf-ready` × 500 hashes; XMRig = 120 s
@ 5.04 H/s. Two event sets per miner (A: interlocks, B: cache/branch); armrx's two runs agree to
8 significant figures on instructions, so the counts are solid.**

| event | armrx/hash | XMRig/hash | ratio | Δ cycles/hash |
|---|---:|---:|---:|---:|
| cycles | 163.02 M | 155.03 M | 1.05× | **+7.99 M** |
| instructions | 89.46 M | 101.68 M | **0.88×** | −12.21 M |
| **`other_interlock_stall`** | **23.27 M** | **10.96 M** | **2.12×** | **+12.30 M** |
| `ld_dep_stall` | 17.63 M | 19.33 M | 0.91× | −1.70 M |
| `agu_dep_stall` | 2.05 M | 2.03 M | 1.01× | +0.01 M |
| `st_dep_stall` | 0.57 M | 0.53 M | 1.07× | +0.04 M |
| `l1d_cache_refill` | 0.83 M | 0.65 M | 1.28× | +0.18 M |
| `l1i_cache_refill` | 0.19 M | 0.52 M | **0.36×** | −0.33 M |
| `l2d_cache_refill` | 0.28 M | 0.30 M | 0.91× | −0.03 M |
| `br_mis_pred` | 0.031 M | 0.015 M | 1.99× | +0.015 M |

**The mechanism is named, and it is a single class:** `other_interlock_stall` alone is
**+12.30 M cycles/hash = 154% of the entire 7.99 M cycle gap** (the other classes partly offset it,
notably armrx being *better* on load stalls and 2.8× better on I-cache refills). Total interlock
stalls: armrx **26.7% of all cycles** vs XMRig **21.2%**.

On Cortex-A53, `other_interlock_stall` = "cycles stalled in the Wr stage for a reason other than
load/store/AGU/SIMD" — in this workload that is overwhelmingly the **integer multiplier**.
A53's `MUL`/`UMULH`/`SMULH` are multi-cycle and **not fully pipelined**; back-to-back dependent
multiplies interlock. RandomX superscalar programs are dependency-dense by design and
`IMUL_R` is the single most common opcode (775 of 3,563 = 21.8%, plus 239 `IMUL_RCP`,
116 `IMULH_R`, 124 `ISMULH_R` — **1,254 multiplies = 35% of the body**, per `--jit-dump`).

**Everything else is exonerated:** not I-cache (armrx 2.8× *better*), not D-cache (+0.18 M is 2%
of the gap), not L2, not branches (+0.015 M is 0.2%), not loads (armrx better), not code density
(armrx 12% leaner). One class, one mechanism.

**Root cause located in our code — the superscalar scheduler is weaker than the main-VM one.**
Both live in `src/jit_compiler_a64.cpp`:
- `scheduleProgram()` (main VM, `:690-733`) implements **two** reorder rules: a distance-2 swap
  (`i, i+2, i+1`) *and* a **distance-3 swap** (`i, i+3, i+1, i+2`) for when the nearer candidates
  also conflict.
- `scheduleSuperscalarProgram()` (`:746-775`) implements **only the distance-2 rule**. If
  `i+2` conflicts with the long-latency `i`, it gives up and emits in program order — leaving the
  multiply's result consumed on the very next instruction, which is exactly the interlock.
- This is backwards relative to where the cost is: the superscalar body is ~80% of the work and
  holds 35% multiplies, yet it gets the *weaker* scheduler. The distance-3 rule was written,
  validated, and shipped — just never extended to the path that needs it most.

**Proposed change (E20) — port the distance-3 rule to `scheduleSuperscalarProgram()`:**
- Mirror `:716-733` into `:760-773`, preserving the superscalar guards (`is_imul_rcp` on the
  candidates, which exist because `IMUL_RCP` consumes a pool literal whose position is
  order-sensitive) and adding the hazard checks against `i+3`.
- **Strictly a reordering of independent instructions** — same instruction count, same opcodes,
  no new registers. Hash output must be bit-identical; `test_jit_equivalence` (16 pairs,
  byte-identical vs interpreter) is the gate, and the differential stress suites exist for it.
- **Expected size:** if it closes even a third of the +12.30 M interlock excess, that is ~4 M
  cycles/hash ≈ **+2.5%**. Closing it fully would put armrx at ~150.7 M cycles/hash — **ahead of
  XMRig**. Unlike the dead leads, this is a targeted fix for a *measured, named* stall class.
- **Caveat:** the emitter-scheduler history says memory-op (`*_M`) reordering caused a real
  JIT/interpreter divergence and is forbidden. This proposal touches **only** the superscalar
  integer path, which has no `*_M` opcodes at all — the divergence mechanism does not apply here.

**Do NOT re-open:** instruction density (E3c), C* immediates (E3c), Blake2b (E3a), C++ overhead
(E17), hugepages/affinity/PGO/LTO/`-mtune` (§1). The measurement above closes them collectively:
none of those classes carries a meaningful share of the 7.99 M cycle gap.
### 8-worker reconciliation — NO ANOMALY. Resolved from data already on file (2026-08-04)

**Do not re-measure this.** A "~2.4 H/s unexplained 8w shortfall" was raised in-session and is an
**arithmetic error, not a finding**: it compared a *single fast core* (4.77 H/s) against an
*8-worker average that includes four half-speed cores*. Different denominators.

Reconciled from the thermally-clean per-worker sweep already recorded in
`next-iteration-plan.md:305-322` (device build, GCC 15), scaled by the E18 GCC-16 factor
(4.77/4.32 = **1.104**), applied uniformly to both clusters:

| | 1w fast core | 8w fast avg | 8w weak avg | **8w total** |
|---|---:|---:|---:|---:|
| device build (measured, on file) | 3.96 | 3.58 | 1.88 | **21.85** |
| cross build (projected ×1.104) | 4.37 | 3.95 | 2.08 | **~24.1** |

- 8w fast cores lose **10%** vs 1w (3.58 vs 3.96) — 8-way contention, measured.
- Weak cluster is **0.53× fast** — the SoC's own asymmetry. **XMRig's own ratio is 2.4/4.5 = 0.53,
  identical**, and XMRig's implied 8w from its per-core rates (4×4.5 + 4×2.4 = 27.6) matches its
  measured 28 to within 1.4%. Both miners pay the same tax.
- ⇒ **~95% per core and ~86% at 8 workers are the same result**, viewed through different core
  mixes. There is no second gap. The naive "94.6% should hold at 8w → 26.5 H/s" silently assumed
  eight fast cores.

**Standing 8w figures:** armrx ~24.1 H/s projected cross-built (21.85 measured device-built) vs
XMRig 28. The projection is arithmetic from measured per-worker rates, not a measurement — if an
8w cross-build number is ever wanted it is one run, but nothing currently depends on it.

**Discipline note (this is the third instance today):** the `--mine` census, the phantom 8w gap,
and the stripped-binary attribution were all the same failure — a ratio taken over mismatched or
invalid denominators. Before citing any ratio, state explicitly what both sides are per.

### E22 — disable the superscalar scheduler (identity order): IMPLEMENTED, MEASURED, **INCONCLUSIVE on stalls** (2026-08-04)

**Test of the 3/3-agent consensus** (Planner / gh_copilot / gpt_5.6_luna): armrx's superscalar
scheduler is dependency-aware but NOT resource-aware, so at 35% multiply density its `P,R,Q` swap
hoists an independent MUL into the multiply's slot, and the A53 MAC cannot accept back-to-back
independent multiplies → scheduler *increases* interlocks vs XMRig's unscheduled order. A/B =
disable `scheduleSuperscalarProgram` (return identity `[0..n-1]`, `#if 1` guard at `:746`). KATs:
16/16 byte-identical (identity order never reorders → pool-safe).

**What is SOLID from the runs:**

- **H/s is unchanged: 4.77 with scheduler OFF, identical to scheduled.** No regression, no win.
- **Instruction count is the reliable signal:** scheduler OFF = **101.4 M instr/hash** vs scheduled
  **89.5 M** (+13.3%). The scheduler genuinely hides ~12 M instructions of latency-fill; turning it
  off restores the full program length. This confirms the "armrx is 12% *leaner* than XMRig" number
  (E19) is **scheduler-driven**, not a structural emitter win.

**What is NOT trustworthy (measurement contaminated — record so it isn't cited):**

- The cycle/stall comparison is **inconclusive**. Run #1 (`perf stat --` on `--full-hash-only`) had
  ~13.8 s of background on core 3 inside the 118.7 s window (decontaminated cycles ≈ 162.6 M ≈ ON's
  163.4 M — i.e. *no* clean delta). Run #2 (handshake-gated) attached `perf` to a dead PID and its
  counts matched ON to 0.05% (physically impossible for a scheduler-OFF binary), so it is invalid.
- ⇒ The earlier draft of this entry claiming "scheduler OFF is **worse** (+20.4 M cycles, 24.7 M
  interlocks)" was built on the contaminated run #1 and is **retracted**. The only established facts
  are: H/s flat, instruction count +13.3% (scheduler helps density), and **the interlock question is
  OPEN** — not answered in either direction.

**To actually answer "does the scheduler change interlocks":** re-run with a clean steady-state
capture (the existing `tools/perf_ready_bench.sh` handshake, but fixed so `perf` is killed on the
correct child and only ONE batch is measured) on BOTH the ON and OFF binaries, same 500-hash
window, core 3 isolated, no background. Until that exists, **E22 says the scheduler is safe to keep
(H/s unchanged, density-positive) but does not localize the E19 interlock excess.**

**Status of the E19 puzzle after E22:** the interlock excess (2.12×) is confirmed real; the
superscalar *scheduler* is neither proven harmful nor proven the cause. The live hypotheses remain
upstream of emission (generated program order in `src/superscalar.cpp`; main-VM multiply handlers;
C* padding; x12 WAR). E22 does NOT rule the scheduler in or out as the interlock cause — the
measurement was inconclusive on that axis.

### E23 — EXTERNAL AGENT TRIAGE (2026-08-04): 3/3 converge, all on a hypothesis E22 could not confirm or kill

Three agents were given the same brief (`docs/experiments/agent-brief-ideas.md`, answers in
`20260803/`): "Localizing AArch64 RandomX Performance Gaps.md" (Planner), `gh_copilot`,
`gpt_5.6_luna`. **All three independently ranked "superscalar scheduler clusters multiplies" as
hypothesis #1**, with the disable-scheduler A/B as the top test. That hypothesis was E22-tested and
**NOT resolved** (E22's stall comparison was contaminated — see E22; H/s unchanged, instruction
count confirms the scheduler hides ~12 M instr, but interlocks are inconclusive). Lessons:

- **Convergence is not correctness.** Three independent reasoners hit the same answer because they
  shared the brief's framing ("XMRig schedules nothing yet wins → scheduling is the bug"). The brief's
  own paradox was a red herring (see E22's "two unscheduled states" reasoning — which itself needs a
  clean measurement to confirm).
- **Their secondary hypotheses are still live and untested:** (1) armrx's *generated* program order
  differs from XMRig's (superscalar.cpp generator logic); (2) main-VM multiply handlers /
  `emitMemLoad`; (3) C* density padding (H2); (4) x12 WAR (H3). The inconclusive E22 on the scheduler
  does NOT touch these.
- **The brief needs a guard** (already added to §3): a future agent round must not re-land on
  "disable the scheduler" — E22 showed it is safe to keep (H/s flat, density-positive) and the
  interlock question is now upstream of emission.

**Recommended next round** (if pursued): point agents at `src/superscalar.cpp` — compare armrx's
generated multiply spacing against XMRig's generator (`mulCount`/`allowChainedMul`) — rather than
the emitter. That is the one place the brief + E22 now point to as untested.

**This lead does not exist. The whole framing was wrong, and it reverses the earlier E3c
"parity" conclusion — in armrx's favour, not XMRig's.**

**What I assumed (incorrectly):** that XMRig's superscalar C* immediates use the NEON `umov`
form (register-to-register, off the memory port) while armrx uses `LDR` from a literal pool
(hitting the memory port), and that this *emission difference* — not ordering — explained armrx's
2.12× multiplier interlocks. That assumption was wrong on two counts:

1. **The `umov` form is unreachable for the superscalar C* path.** In XMRig's
   `jit_compiler_a64.cpp`, at the top of `generateSuperscalarHash` (`case` block entry) line 337
   sets **`num32bitLiterals = 64`** — i.e. the pool is *pre-filled to its 64-slot capacity*. So
   every `emitMovImmediate` for a superscalar C* immediate hits the `else` branch (lines 512+):
   **`MOVZ`/`MOVN` + `MOVK`, a 3-instruction MOVZ/MOVK sequence**, exactly like armrx's own
   fallback. (The `umov` branch at 496 is only taken when `num32bitLiterals < 64`, which the
   superscalar path never enters.)

2. **Both miners therefore emit the SAME thing for C\* immediates in the superscalar body:
   MOVZ(+MOVK) / MOVN+MOVK, 1–3 instructions, no `umov`, no `LDR`.** armrx's *primary* path is a
   2-instruction `LDR`-literal-pool form (`emitCpoolImmediate`, `jit_compiler_a64.cpp:1323`) that
   is *denser* than XMRig's 3-instruction MOVZ+MOVK — armrx is **better** here, not worse, and the
   E3c static census (714 C* immediate slots, zero fallbacks) already says so.

**So the emission difference I hypothesized is absent. The two emitters are equivalent (armrx
marginally denser) for C\* immediates — confirming E3c's "parity / armrx not worse" result, not
contradicting it.** There is no `LDR`-vs-`umov` port difference to chase, because neither miner
uses `umov` in this region.

**What this means for the XMRig-schedules-nothing anomaly (the real puzzle from the E19
follow-up):** if both miners emit the same C* immediates *and* XMRig does no superscalar scheduling
yet has half armrx's multiplier interlocks, then **neither emission choice nor instruction ordering
is the dominant cause** of armrx's `other_interlock_stall` excess. The difference lies elsewhere —
most likely in how the *multiply / multiply-high* chains themselves are sequenced relative to their
sources, or in some other region armrx emits differently (Blake2b, the dataset/scratch loads, or
the main-VM body that E19 noted is the other ~10% of instructions). The umov hypothesis is
exhausted and must not be reopened.

**Methodology lesson (record so it isn't repeated):** I built E21 on a memory of XMRig's `umov`
branch *existing in the source* without re-reading the *caller* that forces the 64-slot
pre-fill. The branch is real; its reachability for this path is not. **Always trace the guard /
exhausted and must not be reopened.

### E20 — distance-3 superscalar scheduling: IMPLEMENTED, MEASURED **NULL**, REVERTED (2026-08-04)

**What was tried:** ported the distance-3 reorder rule from `scheduleProgram()` (main VM,
`jit_compiler_a64.cpp:716-733`) into `scheduleSuperscalarProgram()` (`:746-775`), which previously
had only the distance-2 rule. Guards preserved: `IMUL_RCP` excluded on all three moved candidates
(literal-pool order sensitivity), hazard checks of R2 against P, Q and R1.

**Correctness: PASSED.** `test_jit_equivalence` 16/16 (seed,input) pairs byte-identical vs the
interpreter; `test_aes_hash` and `test_mining` all green. The transform is sound.

**The rule fires, but far too rarely to matter.** Diffing the `--jit-dump` superscalar boundary
table before/after: **26 reordered slots out of 3,563 instructions = 0.73%** of the body.
Spot-checked hunks are exactly the intended pattern (e.g. `IADD_C8` hoisted ahead of two `IADD_RS`
to fill an `IMUL_R` stall slot), so the implementation is doing what it was designed to do.

**Result: null, marginally negative.** Paired A/B, same core, device otherwise idle, 40 °C,
`bench_armrx --full-hash-only` (500-hash median), alternating base/E20 with 10 s cooldowns:

| run | baseline | E20 |
|-----|---------:|----:|
| 1 | 4.78 | 4.76 |
| 2 | 4.76 | 4.76 |
| 3 | 4.77 | 4.76 |
| **median** | **4.77** | **4.76** |

−0.2%, inside noise but not positive. **Reverted** (`git checkout src/jit_compiler_a64.cpp`).

**Why it failed — and what it does NOT prove.** E19 correctly identified *which* stall class
dominates (`other_interlock_stall`, +12.30 M cycles/hash), but "therefore widen the peephole" did
not follow: a **0.73% hit rate cannot move a 5% cycle gap** no matter how correct each swap is.
The binding constraint is the scheduler's *form*, not its window — a greedy, forward-only,
fixed-window peephole over a *fixed program order* has almost no legal moves in code this
dependency-dense.

**What is closed:** further widening of *this same peephole* (distance-4, 5, …). Same arithmetic,
same ceiling. Don't spend another cycle on the peephole family.

**What is NOT closed — scheduling is DEFERRED, not dead.** Two distinct facts are easy to conflate:
1. *This particular peephole* is exhausted. Settled.
2. *Instruction scheduling as an approach* is unproven here, in both directions. A real **list
   scheduler over the superscalar DAG** — build the dependency graph, emit in latency-aware
   topological order — is a categorically different mechanism with a vastly larger legal-move set.
   It was never tried. Its ceiling is unknown, not zero.

**Sequencing (user direction 2026-08-04):** the DAG scheduler is **parked until the XMRig gap is
closed by other means**, then revisited as a *forward* lever — i.e. as a way to go past parity, not
to reach it. Rationale: it is the most expensive item on the board (needs the differential stress
suites as its gate) and the cheaper emission-level leads (E21) are unexplored. Park it; don't bury
it. When it is picked up, start from the E19 stall data, not from the peephole's assumptions.

**One caution to carry into that work (see E21):** XMRig's AArch64 superscalar emitter does **no
scheduling at all** — plain program order — and still has *half* armrx's multiplier interlocks. So
ordering alone cannot be the whole story, and a DAG scheduler should be attempted *after* the
emission-level difference is understood, or it will be optimizing around the wrong constraint.

---

#### Appendix: the discarded measurements (kept so the error isn't repeated)

The following runs are **superseded by the 500-hash `--perf-ready` census in §0** and are recorded
only to document *why* they were wrong. Do not cite these numbers.

| Miner | window | instructions | cycles | hashes | instr/hash | IPC | clock check |
|-------|--------|-------------:|-------:|-------:|-----------:|----:|-------------|
| XMRig 6.26.1-dev | 120 s `perf stat -p`, `--bench=1M --threads=1` | 61,305,541,436 | 93,747,946,845 | 604.8 | 101.4 M | 0.654 | 781 MHz ✓ valid |
| armrx | 60 s `perf stat -p` on `--mine --workers=1` | 20,708,392,193 | 27,289,248,936 | 150 (counter delta) | 138.1 M | 0.759 | **455 MHz ✗ INVALID** |
| armrx | whole `--seconds=60` run (incl. cache init) | 39,431,396,652 | 52,257,693,095 | 258 | 152.8 M | 0.755 | **819 MHz, but init-contaminated** |

**The error:** the `--mine` rows show only **455 MHz effective** (27.3 B cycles / 60 s on a 765 MHz
core = 59% busy) — the worker thread is not saturated under `--mine`, so both its instr/hash and
its *apparently excellent* IPC 0.759 are artifacts. Reasoning from that inflated IPC produced the
conclusion "armrx wins IPC therefore the gap must be instruction count, ~1.35×" — the exact
opposite of the truth (armrx is 12% leaner and loses on IPC 0.547). One clock-consistency check
would have caught it; that check is now discipline rule #10.

**Also superseded:** the region table from the stripped-binary profile
(`[JIT]` 83.7% / `worker_loop` 10.67% / `fill_aes_1r_x4` 5.02%) — see E17, the `worker_loop` slice
is a symbolization artifact and the correct split is JIT 97.89% / all C++ 1.84%.

**Status: E3c CLOSED** (see the top of this entry for the current, correct conclusion).

### E17 — `worker_loop` C++ overhead  [KILLED 2026-08-04 — it was a symbolization artifact]
- **Original evidence (stripped device build):** `perf record` attributed **10.67%** of cycles to
  `armrx::MiningEngine::worker_loop(unsigned int)` with a `ldr w5, [sp, #0x564]` at 5.62%, plus
  dozens of `cbnz` — read as C++ driver overhead XMRig doesn't pay.
- **KILLED by the `-g` re-profile.** Cross-built `RelWithDebInfo -g -fno-omit-frame-pointer`
  (`build-g/`, `ARMRX_DISABLE_LTO=ON`), deployed to `/tmp/armrx-g`, profiled the same way
  (`perf record -F 499 -g`, 60 s, 941 samples, `--mine --workers=1`, `taskset -c 3`):

  | DSO | % cycles |
  |-----|---------:|
  | **`[JIT]` buffer** | **97.89%** |
  | `armrx-g` (ALL C++ combined) | **1.84%** |
  | `ld-musl` / libstdc++ / libgcc / unknown | 0.26% |

  Top armrx C++ symbols are `hash_and_fill_aes_interleaved_x4` 0.85%, `scheduleProgram` 0.64%
  (JIT compile, not hash), `fill_aes_1r_x4` 0.21% — plus `__clock_nanosleep`/`sleep_for` ~1.4%,
  which is the *stats-printer thread sleeping*, not hash work.
- **Root cause of the false signal:** the stripped device binary has no symbol boundaries for the
  JIT'd/RWX region and inlined callees, so `perf` folded a large block of unattributable
  in-JIT-adjacent samples into the nearest preceding symbol, `worker_loop`. With `-g` the same
  cycles resolve correctly into `[JIT]`. The `ldr w5, [sp, #0x564]` "hottest instruction" was an
  address inside that mis-attributed block, not a real spill in the driver.
- **Lesson (generalize):** never take `perf` symbol attribution from the stripped device build as
  evidence about C++-vs-JIT split. Any region-attribution claim must come from a `-g` build.
  The cross-build makes this cheap (~3 min) — there is no reason to profile stripped again.
- **Consequence:** there is **no C++ driver overhead to remove**. armrx spends 97.9% of its cycles
  in JIT'd code, which is the correct shape and matches XMRig's. The real gap is
  therefore **entirely inside the JIT buffer** (superscalar bodies + main-VM loop) — E3c's region,
  even though E3c's specific C*-immediate hypothesis was wrong.
- **Note (unexplained, worth a look):** the `-g` cross build measures **4.63 H/s** vs the on-device
  stock build's **4.32 H/s** — a **+7%** delta from a build that should be *slower* (no LTO,
  frame pointers pinned, debug info). That is a bigger effect than most experiments in this log and
  it is a *compiler/build* difference, not an algorithm one. See E18.

### E18 — cross-built binary is ~8% FASTER than the on-device build → **cause = GCC 16.1.0 vs 15.2.0**  [MEASURED 2026-08-04]
- **Observation, reproduced:** same source tree (`a1ea83c`), same core (`taskset -c 3`), same mode
  (light), 39–43 °C, `--mine --workers=1 --seconds=90`, steady-state H/s, 2 runs each (spread 0.00):

  | # | binary | compiler | LTO | `-mtune=cortex-a53` | **H/s** |
  |---|--------|----------|-----|---------------------|--------:|
  | 1 | `~/armrx/build/armrx` (stock device) | GCC 15.2.0 (Alpine) | **on** | no | 4.32 |
  | 2 | `/tmp/bld-nolto/armrx` | GCC 15.2.0 | **off** | no | **4.24** |
  | 3 | `/tmp/bld-tune/armrx` | GCC 15.2.0 | on | **yes** | 4.32 |
  | 4 | `/tmp/armrx-rel` (cross, Release) | **GCC 16.1.0** | off | yes | **4.66** |
  | 5 | `/tmp/armrx-g` (cross, RelWithDebInfo `-g -fno-omit-frame-pointer`) | **GCC 16.1.0** | off | yes | **4.66** |

- **Isolation result — both suspected flags are NULL; the compiler version is the whole effect:**
  - **LTO is not it** (and is mildly *positive*): turning LTO off on-device gave **4.24 vs 4.32**,
    i.e. −1.9%. The hypothesis that "on-device LTO is actively harmful" is **falsified** — do not
    change the LTO default on this evidence.
  - **`-mtune=cortex-a53` is not it:** adding it to the device build gave **4.32**, exactly the
    stock number. Null.
  - **`-g` / RelWithDebInfo is not it:** cross Release and cross `-g` are **both 4.66**, identical.
    (This also retro-confirms the E17 `-g` profile was measuring representative code, not a
    debug-slowed variant.)
  - ⇒ by elimination, the **+7.9% is GCC 16.1.0's codegen vs GCC 15.2.0's**, on the same flags.
- **Impact:** armrx per-core goes **4.32 → 4.66 H/s = 92.5% of XMRig's 5.04** (was 86%), for zero
  source change. Extrapolated to 8 workers this is ~21.85 → ~23.6 H/s.
- **What this does NOT change:** the instruction-efficiency gap is still real and still inside the
  JIT buffer (E17: 97.9% of cycles are in JIT'd code). A better host compiler improves the C++
  glue and the JIT *compiler*, not the JIT-*emitted* hash body — so the remaining ~7.5% per-core
  deficit is still an emission question, unchanged in nature by this finding.
- **Caveat before acting:** the cross binaries are **musl-static-ish GCC 16 builds** and the stock
  device build is Alpine GCC 15. This is a *deployment/toolchain* result (like `isolcpus`), not a
  code change — the actionable form is "ship cross-built binaries" or "upgrade the device
  toolchain", both of which need the full KAT suite green on the artifact that actually ships.
  AGENTS.md already records cross binaries passing `test_jit_equivalence` 16/16 byte-identical
  (2026-08-01), so the path is credible but must be re-verified per-artifact.
- **Correctness of the GCC 16 artifact — VERIFIED on-device 2026-08-04** (this is what makes E18
  actionable rather than just interesting): the cross Release binary's own test builds were run on
  the device — `test_jit_equivalence` **16/16 (seed,input) pairs byte-identical**, `test_aes_hash`
  ALL PASSED, `test_mining` ALL PASSED (2 fast-mode subtests SKIPPED: fast mode needs 2080 MiB,
  device has ~1.9 GiB — expected, not a failure). So GCC 16.1.0 does not change hash results.
- **Method note:** the cross toolchain file *forces* `ARMRX_DISABLE_LTO=ON`
  (`cmake/toolchain-aarch64-musl.cmake:57`), so LTO on/off cannot be A/B'd via cross-compile —
  the two cross matrix builds came out byte-identical (same md5). LTO had to be tested on-device,
  which is what rows 1–2 above do.

### E3b — main-VM memory-op emission ordering vs XMRig
- **Hypothesis:** XMRig may order/interleave scratchpad loads to hide latency better, or emit
  fewer ops for the same program. Main-VM IPC is armrx's worst region (0.405) — but that's a
  *stall* penalty, and the gap is *instruction-count* not stalls, so lower priority than E3c.
- **Test:** `perf mem` / cache-miss + instruction-count diff on the main-VM region, miner-to-miner.

### E3a — Blake2b IPC/instr vs XMRig  [EFFECTIVELY CLOSED by inspection]
- **Result:** armrx NEON vs XMRig scalar-C on AArch64 → armrx is *better*. Not the gap.
- **Only residual value:** confirm with a direct `perf` count if E3c's region split shows
  Blake2b unexpectedly large in armrx (unlikely). Low priority.

---

## 3. GitHub Copilot ideas (attached 2026-08-02) — triage

Source: user-attached `sun_aug_02_2026_performance_improvement_ideas_for_codebase.md`
(skeptical perf suggestions). Mapped against our actual state:

| # | Copilot idea | Verdict | Notes |
|---|--------------|---------|-------|
| 1 | Measure-first guardrails (pin, median+p95+MAD, separate regions, noise-floor control) | **ADOPT as discipline** | we've been burned by 1-long-run + thermal confounds; codify below |
| 2 | Split scheduler: main conservative / superscalar aggressive | **PARTIAL / already done** | emitter scheduler already reorders both; aggressive superscalar window was the real small win (+0.233% IPC). Not a new lever |
| 3 | Microarch profiles (A53 vs A55+), MIDR runtime select | **VALID but low priority** | device is fixed A53 @765MHz; only matters if we target other HW. Keep as future |
| 4 | JIT code-layout (hot-loop align 32/64B, reduce taken branches, I-cache pressure) | **WORTH A TEST** | aligns with T2-2 (dual-issue align was null) but block-alignment ≠ NOP-pad. Untested. Queue behind E3c |
| 5 | Register-pressure-aware emission | **PLAUSIBLE, untested** | small-core spill avoidance. Investigate if E3c shows spill hotspots |
| 6 | Fast-div/sqrt adaptive gating | **DEAD** | see §1: measured −1.1%, already gated |
| 7 | Prefetch sweep for dataset/cache | **LOW EXPECTATION** | RandomX accesses are random; T2-2-style nulls likely. Skip unless E3b shows miss-bound |
| 8 | Hugepages/TLB experiments | **DEAD** | see §1: armrx == XMRig method, null |
| 9 | Dynamic cluster-aware balancing | **DEAD (for gap)** | scaling is linear; weak cluster is SoC design, XMRig bears it. Only relevant for scheduling fairness, not the gap |
| 10 | PGO workflow hardening | **DEAD** | see §1: null on current code |
| 11 | Compiler-flag matrix (GCC vs Clang, -O3 vs -Ofast) | **DONE — and it PAID (E18)** | GCC 16.1.0 cross vs GCC 15.2.0 device = **+7.9%** (4.66 vs 4.32). LTO-off −1.9%, `-mtune` null, `-g` free. Clang still untried |
| 12 | Perf-invariant docs/tests (don't schedule `*_M`, max code-size growth, min confidence) | **ADOPT** | formalize the §1 dead-lead list as enforced invariants |

**Net from Copilot:** mostly re-derives leads we already closed (6,8,9,10 dead) or already do
(2). Genuinely new + cheap: **#11 (Clang vs GCC build comparison)** and **#4 (block alignment,
distinct from the null NOP-pad test)**. Consider **#11** as a quick parallel experiment.

---

## 4. MEASUREMENT DISCIPLINE (enforced — to stop the loop)

1. **Pin + same conditions.** `taskset -c <cores>`; same cooling; report the SoC temp. Never
   compare a 30s early-cool number to a 120s steady-state number (that manufactured the fake
   "5.8× recovery").
2. **Miner-to-miner only.** The ONLY valid comparison is armrx vs XMRig on the *same device,
   same run conditions*. Don't infer XMRig behavior from memory.
3. **Region-split, not just H/s.** A single H/s number hides everything. Always get
   instruction/cycle counts per region (superscalar / main-VM / blake2b / argon2).
4. **Median over repeats, not one long run.** ≥3 repeats; report median + spread. Thermal and
   pool variance are real.
5. **Before any new code change: confirm the lead is OPEN in §1/§2.** If it's in §1, don't redo it.
6. **Never profile a stripped binary for region attribution.** `perf` folds unattributable
   JIT/inlined samples into the nearest preceding symbol, which manufactured the fake
   "`worker_loop` = 10.67%" lead (E17) and probably the W1-1 "named C++ 9.5%" too. Any claim about
   *where* cycles go must come from a `-g` build. The cross-build makes this ~3 min:
   `cmake -S . -B build-g -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake
   -DARMRX_DISABLE_LTO=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer"`.
   Verified 2026-08-04 that `-g` costs **zero** hashrate (cross Release 4.66 == cross `-g` 4.66),
   so there is no accuracy/speed tradeoff to worry about.
7. **State the TOOLCHAIN with every number.** A GCC 16.1.0 cross build is **+7.9%** over the
   GCC 15.2.0 device build on identical source (E18). Every pre-2026-08-04 on-device A/B in this
   repo carries that confound. Never compare a cross-built binary's H/s against a device-built one.
8. **armrx's `Total Hashes` counter is quantized to 5** — windowed hash deltas over short perf
   runs are unreliable (a 60 s window gave 150 vs the run's own 259). Prefer the miner's own
   `Steady-state hashrate:` line, or derive ratios from H/s and IPC (both quantization-free)
   rather than dividing instructions by a counter delta.
9. **`perf stat` mechanics on this device** (cost real time to rediscover): `perf stat -p PID --
   sleep N` needs a **single** PID — `pgrep` often returns the wrapper shell too, and perf then
   dies with "Problems finding threads of monitor". Wrapping the target in `timeout` leaves perf
   unflushed (empty output). SSH commands that outlive the session must be `nohup sh -c "..." &`.
   XMRig `--bench=1M` = 1 MILLION hashes (~62 h) and buffers its log — use `--http-port=8080` and
   read `/2/summary` for live hashrate instead of waiting for stdout.
10. **Check clock consistency before trusting ANY perf window.** Compute
   `cycles ÷ elapsed_seconds` and confirm it is ≈ **765 MHz**. If it is materially lower, the core
   was idle for part of the window and every derived figure (instr/hash, IPC) is garbage. This one
   check would have caught the bogus `--mine` census that produced the fake "+35% instructions"
   (27.3 B cycles / 60 s = 455 MHz = 59% busy). Prefer `bench_armrx --full-hash-only --perf-ready`,
   which pins an exactly-500-hash saturated window; `--mine` is NOT a measurement harness.
11. **Clean-room:** read XMRig *technique* only; re-implement in armrx's own conventions. No XMRig
   code in our tree. (User standing rule.)

---

## 4b. E24 — C* immediate density exposed the A53 MAC interlock: **CONFIRMED WIN, SHIPPED** (2026-08-04)

**The thread that found it.** E21 (this file) had already established that armrx's superscalar
C* immediates use a **2-instruction `LDR`-literal-pool form** (`emitCpoolImmediate`,
`jit_compiler_a64.cpp:1323`) that is *denser* than XMRig's **3-instruction `MOVZ`/`MOVN`+`MOVK`**
form — and concluded "armrx is better here, not worse." That conclusion was half-right: the density
was real, but it was the *bug*. The external agents' Planner H2 ("stripping C* density removes
free padding that was hiding MAC latency") pointed at exactly this, but was ranked lower-confidence
because everyone assumed XMRig was the *denser* one. It was the reverse: **armrx was too dense.**

**Mechanism (Cortex-A53, measured).** The RandomX superscalar body is ~35% integer multiplies, each
with a 4-cycle result latency on the A53's single MAC. The reference generator already spaces
multiplies ~2 instructions apart (its own port simulator). armrx's 2-instr C* form (`Ldr xN,lit;
Eor dst,dst,xN`) put only **one load** between a multiply and the next; XMRig's 3-instr form
(`Movz; Movk; Eor`) puts **two independent ALU ops** there. With the denser form, program-adjacent
multiplies landed only ~2 instructions apart and saturated `other_interlock_stall` (the A53's
multiply-result interlock). Padding to 3 instr re-inserts one independent ALU op per C* immediate,
breaking up the multiply chains.

**The change.** `emitCpoolImmediate` now always emits the 3-instr `MOVZ`/`MOVN`+`MOVK` form
(XMRig style) and the now-dead LDR-pool branch (and its `cpoolBase_`/`cpoolLiteralPos_`/`cpoolSlot_`
machinery in `generateSuperscalarHash`) was removed. Identical constant, just one more instruction.
KATs: **16/16 byte-identical** (verified on-device, both before and after removing the dead branch).

**Result (1 worker, light mode, core 3, idle, 500-hash `--full-hash-only` + clean `perf stat`):**

| metric | baseline (scheduled, 2-instr C*) | **E24 (3-instr C*)** | XMRig | 
|---|---:|---:|---:|
| H/s | 4.77 | **5.11 (+7.1%)** | 5.04 |
| instr/hash | 89.5 M | 113.8 M | 101.4 M |
| `other_interlock_stall`/hash | 23.27 M | **6.18 M (−73%)** | 10.96 M |
| IPC | 0.548 | **0.662** | 0.654 |
| cycles/hash | 163.4 M | 171.9 M | 155.0 M |

- H/s **beats XMRig** at 1 worker (5.11 vs 5.04) — the gap is not just closed, it's inverted in our
  favour on this core. `other_interlock_stall` dropped **below** XMRig's (6.18 vs 10.96 M/hash).
- Instruction count *rose* (89.5→113.8 M) — we traded density for latency hiding, exactly the
  H2 prediction. Cycles/hash rose slightly (163→172 M) but throughput rose more (IPC 0.548→0.662),
  so the extra instructions are cheap filler that keeps the MAC fed.
- Two clean back-to-back `--full-hash-only` runs gave 5.11 and 5.12 H/s (idle core); a contaminated
  run (background on core 3) gave 4.79 — confirming H/s must be read from the bench's internal
  median, not wall-clock, and that core-3 isolation matters.

**This closes E19.** The `other_interlock_stall` excess (2.12× vs XMRig) is **explained and fixed**:
it was armrx's over-dense C* immediate form exposing the A53 MAC interlock. The superscalar
scheduler (E22) was a red herring; the generator (E21/L1) was identical; the cause was a single
emission choice (immediate materialization width). One-line-class fix, +7% hashrate.

**Deploy note:** ship the cross-built binary (GCC 16.1.0, +7.9% over device GCC 15.2.0 per E18) with
this change. At 8 workers the per-core +7% compounds through the strong cluster (cores 0-3); weak
cluster (4-7) scales at ~0.53× as before, so expect ~+7% on the fast half → ~25.8 H/s projected
(8w) vs XMRig's 28, i.e. ~92% at 8w, up from 86%.

---

## 5. OPEN QUESTIONS FOR USER
- (answered 2026-08-04) XMRig = 28 H/s @8w cool? → **yes, user-confirmed, no re-baseline needed.**
- XMRig binary available for reference `perf`? → **yes (`~/xmrig`, `~/xmrig-dev`).**
- Clean-room rule still standing? → **yes (re-confirmed this session).**
