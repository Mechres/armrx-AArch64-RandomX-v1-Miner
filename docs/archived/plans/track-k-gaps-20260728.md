# Track K — Gaps We Missed/Skipped (Tooling, Memory, Code-Density)

**Status:** proposed 2026-07-28. Sits alongside Tracks A–J. Scope: cross-cutting
diagnostic tooling those tracks need but lack, measurements predicted but never
taken, and code-level opportunities the code survey surfaced. NOT a duplication of
A–J.

**Verification note (important):** an earlier draft of this plan (GLM 5.2) cited
several code locations that do not exist at HEAD — it read the *reverted* Track B
hybrid emission (commits b58585c..196a6ad, reverted in 4809838) as if it were live.
Those items were dropped. **Every line-number citation below was re-checked against
HEAD = 4809838 on 2026-07-28.** Anyone extending this plan must re-verify citations
against current HEAD in the same session they edit it — this project has already
burned one verification pass on stale citations (see master-plan §verification).

---

## Priority order (canonical)

1. **K0.1** post-warmup perf harness — unblocks ~6 downstream items
2. **K2.5 + K2.6** AES T-table alignment/collocation — 5-min risk-free, validates K0.1
3. **K1.1** cluster-1 interconnect-vs-controller probe — 2h, zero code, answers a multi-day question
4. **K0.2** instruction-count correlator — unblocks K2 verification
5. **K2.7** Track C data-flow diff harness — unblocks the Track C retry
6. **K0.3 + K0.4** cost model + A53 dual-issue table — analytical input for Track E/F2
7. **K1.4** multi-hour MemAvailable sampler — unblocks Track B Gate C
8. **K3.1–K3.3** build-flag sweep, one chunked A/B
9. **K3.4** BOLT — close the lingering question
10. **K2.4** ISWAP eor-swap — only if the K0.3 cost model says it's not a wash

**Honest scope:** K's ceiling is what K0 unlocks + K1.1's answer (~up to 12.5% IF the
cluster-1 penalty is interconnect-bound — currently unknown) + K2's small
instruction-count savings (individually ≤2%, mostly near-noise). Frame K as "the long
tail plus one big if-conditional (K1.1)", not another systematic 5–10% win.

---

## K0 — Cross-cutting tooling (most other tracks depend on this)

### K0.1 — Post-warmup `perf stat` capture harness *(~half day; do first)*
`tools/perf_after_warmup.sh` (+ optional `tools/devbox/devbox_perf_after_warmup.py`).
Today `perf stat` captures run startup (Argon2 cache init, JIT compile, partial-dataset
fill), polluting every per-instruction-budget measure — the exact pitfall that made
Track A item 1's naive re-measure read 368.7M/hash (2.77× the real 132.93M). Five items
depend on a clean post-warmup capture: Track A item 1 reconciliation, Track A item 2
absolute numbers, Track B Gate B, Track E F2 pre/post, and K0.3 below.
- **Mechanism (lowest risk):** launch armrx, busy-wait on a stdout "READY" sentinel
  emitted after the first hash completes, then `perf stat --pid $PID --delay X`. No C++
  change. (Fallback: extend `--jit-dump` to emit start/stop markers — more work, skip
  unless the sentinel approach fails.)
- **Closure:** back-to-back runs report PMU samples within ±0.2% with no setup events in
  the window.

### K0.2 — Instruction-budget correlator *(~1 day)*
Extend `tools/jit_correlate.py` (already attributes cycles/samples to named functions and
has a region-split extension) to emit **per-opcode instruction counts** — the axis the
XMRig gap lives on. Requested in `analysis-methods-beyond-perf-20260727.md`, never built.
Feed `--jit-dump` + `perf record -e instructions` into the existing correlator. Unblocks
Track A item 1, Track F, and K2's individual verification.

### K0.3 — Python "predict before measuring" cost model *(~1 day)*
`instructions_per_hash(opcode_histogram)` calibrated against `--jit-dump`, per
`analysis-methods-beyond-perf-20260727.md` item 19. Closes small-payload debates
(Track C, D1, K2.x) analytically before spending an A/B. Also the tool that decides K2.4.

### K0.4 — A53 dual-issue pipe co-issuance table *(~half day)*
`docs/architecture/a53_dual_issue_table.md` — the per-instruction-class table Track E F2
Step 1 explicitly asks for and that doesn't exist yet. Build from the Cortex-A53 Software
Optimization Guide; verify each entry against a `perf stat` micro-bench (the Track A item
3 cross-domain-move harness is the template). Single authoritative source for F2 and any
scheduler work.

---

## K1 — Memory & cache hierarchy

### K1.1 — Cluster-1 interconnect-bound vs controller-bound probe *(~2h, ZERO code change)*
The master plan flags B1 (per-cluster cache replication) as "~12.5% aggregate estimated
IF the penalty is interconnect-bound, unknown if controller-bound" — never measured. This
answers it cheaply.
- **Mechanism:** two `perf stat` runs of `taskset -c 4-7 bench_armrx --workers=4
  --warmup=60 --seconds=180` — one with the shared Argon2 cache, one where cluster 1
  reads from a **second physical copy** of the same 256 MiB cache (a read-only
  `memfd_create` / `MAP_ANONYMOUS` buffer, filled once via `memcpy` from the real cache).
  Throughput rises → interconnect-bound → B1 is a real win. No change → controller-bound
  → B1 is null regardless of structure.
- **Design gap to close before building:** "zero code change" is aspirational — pointing
  the 4 workers at the copy requires *some* hook (e.g. an env-var / debug flag read in
  `MiningEngine` that swaps the cache base pointer per-worker for cluster 1). Scope this
  as a **throwaway measurement patch** (branch, never merged), not a shipped feature. The
  read side is genuinely zero-risk (read-only copy of read-only data, no JIT touch); the
  plumbing to select it is the only code, and it's discarded after the measurement.
- **Risk:** zero to correctness (measurement branch only).

### K1.2 — Dataset prefetch: `pldl2keep` A/B *(~1h, gated on single-worker perf reversal)*
**Correction to the earlier draft:** the main-loop dataset prefetch was NOT removed — it
is live at `src/jit_compiler_a64_static.S:358` (`prfm pldl1keep, [x20]`), and its own
comment already names the `pldl2keep` hypothesis. So this is a **prefetch-hint variant
test, not a revisit of a removal.** Hypothesis: A53 L1 is 16 KiB; `pldl1keep` on a 64 B
dataset line may evict live scratchpad state, whereas `pldl2keep` warms L2 for the next
iteration's read while leaving L1 for the scratchpad. Change the one `prfm` hint, A/B at
single worker, reversed order, long window. Revert if it doesn't beat baseline.

### K1.3 — Per-cluster memory-mode asymmetry *(gated behind K1.1)*
Already Track B's follow-on in the master plan; restated as a concrete K1 sub-item.
Give cluster 0 the memory-heavy path, cluster 1 the ALU-heavy derivation. Only pursue if
K1.1 says the penalty is interconnect-bound.

### K1.4 — Multi-hour MemAvailable + OOM/swap sampler *(~half day)*
`tools/devbox/devbox_mem_pressure.py` — the instrument Track B Gate C requires
(`track-b-gate-b-plan-20260728.md`), unbuilt. ~50 lines: poll `MemAvailable`, tail
`dmesg`/`journalctl -k`, **and sample `vmstat` swap-in/out** every 5s during a long run.
**Correction to plan-set docs:** this device is NOT swapless — it has 2.76 GiB swap
(used only during builds; mining stays resident). So Gate C's real risk is gradual
swap-thrashing of the partial dataset, not just cliff-edge OOM. When Track B resumes,
`mlock` the partial-dataset buffer so a "hit" can never become a page-in.

---

## K2 — Code-density / instruction count

**Dropped from the earlier draft (targeted code that does not exist at HEAD):**
- ~~K2.1 `mov x10, sp` at :921~~ — no such emission in current `.cpp` (grep: 0 hits). It
  was part of the **reverted** inline hit path (`0xAA1F03EA`, commit 196a6ad). The
  `mov x10, sp` that survives in the static `.S` common exit is **not** redundant: `x10`
  is the argument `rx_program_xor_with_dataset_line` reads (`ldp x20,x19,[x10]`), not an
  ldp base for sp.
- ~~K2.2 dataPtr MOVZ/MOVK at :902-905~~ — also reverted hybrid-only code; lines there now
  are the inert `_end_hybrid` offset patch + `dumpJitCode()`.
- ~~K2.3 `mov x10, address_register` at :1259~~ — that line is `emitAddImmediate`; citation
  does not resolve. Withdrawn pending a real, re-verified location.

### K2.4 — ISWAP_R swap form — *cost-model validation exercise only (~1h)*
The real `h_ISWAP_R` location must be re-confirmed (the earlier :1754-1756 citation points
at `h_FMUL_R`/`h_FDIV_M`, not ISWAP). Substance caveat: the proposed `eor`-swap is **also
3 instructions**, not 2, and creates a serial RAW dependency chain, whereas the classic
mov-through-temp form is 3 independent movs — on dual-issue in-order A53 the mov form is
likely **equal or faster**. **Do not adopt for perf.** Keep this item only as a way to
validate K0.3's cost model: predict "wash/regression", then confirm on hardware. If K0.3
says wash, skip — churn for no gain.

### K2.5 — `alignas(64)` on the AES T-tables *(~5 min, near-zero risk)*
`src/soft_aes.cpp:31,169` — `randomx_aes_lut_enc[4][256]` and `randomx_aes_lut_dec[4][256]`
(4 KiB each, `const`, `.rodata`) are not `alignas(64)`. One-line annotation each. Expected
effect on A53: ~nil (4 KiB already page-aligns), but it's free and grounds K2.6. Gate:
`test_aes_hash` KAT pins (no bit change possible).

### K2.6 — Collocate enc+dec T-tables *(~1h)*
Same file: the two tables are separate `extern "C"` objects, linker-placed independently.
Combining as `const uint32_t randomx_aes_lut[2][4][256]` + alias accessors guarantees the
8 KiB pair is contiguous (fits one 16 KiB L1 D-cache way). Bit-identical. Gate: KAT + a
single-worker `perf stat -e l1d_cache_refill` A/B. Expected null on A53; harmless.

### K2.7 — Track C data-flow diff harness — build FIRST *(~half day)*
`track-c-phase-a-bisection-plan-20260728.md` Step 1 calls for a host-side harness that
computes `rl[0..7]` for fixed `(cache, itemNumber)` and diffs against `dataset.cpp`'s
reference, to test the "wrong data, not wrong instructions" hypothesis behind the Track C
hang. It doesn't exist. Zero risk, host-side (no devbox), and it's the single most
leveraged tooling gap in the plan set — it unblocks the Track C retry without re-doing the
exhaustive instruction-sequence review that already found nothing. (Filed under K2 for
convenience; it's really Track C tooling.)

### K2.8 — Monolithic-JIT feasibility probe *(gated on Track C Phase A/B)*
Cheap precursor to Track J's monolithic idea: measure `l1i_cache_refill` on the current
split JIT to confirm/deny Track A item 2's finding that I-cache misses aren't the
bottleneck (0.788%). If confirmed, monolithic JIT stays deprioritized; if it surprises,
Track J revives. No code change beyond a measurement run.

---

## K3 — Build / codegen flags (lower priority, cheap)

**Verify first:** the earlier draft claimed a "-O2 baseline". Not confirmed — CMakeLists
sets no explicit `-O2`/`-O3` in its flag block, so the actual level comes from
`CMAKE_BUILD_TYPE` (Release ⇒ `-O3` by CMake default). **Before building K3.2, confirm the
real optimization level** (`cmake -LA | grep FLAGS`, or inspect a compile line) — if the
build is already `-O3`, K3.2 is moot.

### K3.1 — `-fno-plt` + `-ffunction-sections -fdata-sections -Wl,--gc-sections` *(~20 min)*
None set today. PLT-less calls help on A53's fragile branch prediction; section GC trims
the binary. Low risk. Gate: `bench_armrx --full-hash-only` A/B via the K0.1 harness.

### K3.2 — Per-file `-O3` override on hot JIT files *(~10 min; conditional on the verify above)*
If (and only if) the base build is not already `-O3`: `set_source_files_properties` on
`vm.cpp`, `superscalar.cpp`, `jit_compiler_a64.cpp`, `aes_hash.cpp`, `soft_aes.cpp`,
`argon2.cpp` without polluting the rest. Risk low-medium (different opt can trip edge-case
compiler bugs; `test_mining` KAT is the net). Gate: KAT + hashrate A/B + K0.1 harness.

### K3.3 — `-falign-functions=64 -falign-loops=32 -falign-jumps` *(~10 min)*
Unset. A53 fetches 4 instr/cycle; misaligned loop heads can lose a fetch cycle. Gate: K0.1
post-warmup A/B. Expected small.

### K3.4 — BOLT — close the question *(~1 day)*
Master plan lists it as "genuinely never attempted." Wire an `ARMRX_ENABLE_BOLT` CMake path
(link `-Wl,--emit-relocs`, post-process with `llvm-bolt -data=perf.data`), measure once.
Expectation is null (I-cache miss already 0.788%), but it's unfalsified. Accept the null and
move on if that's the result.

---

## Deliberately NOT in K (avoid duplication)
- Track B partial-dataset Gate B — lives in Track B; K only adds K1.4's Gate C sampler.
- Track D1 main item — lives in Track D1; K only adds the K0.1 perf hook it needs.
- Track E F2 main item — lives in Track E; K only adds the K0.4 SOG table it needs.
- Track G NEON-T-table AES — lives in Track G; K2.5/K2.6 are orthogonal layout-only and can
  land first as a 0-risk precondition.
- Track I core-0 cost — has its own plan (`track-i-core0-cost-plan-20260728.md`); not
  duplicated here.
- IRQ-affinity audit (`isolcpus-rt-priority-win.md`) — operational; a future Track L (ops),
  not a code track.

## Discipline for anyone executing K
- Re-verify every line-number citation against current HEAD in the same session you touch a
  K item — this doc was written precisely because an earlier draft cited reverted code.
- Full measurement discipline (master-plan §4): `taskset`-pin both sides, `pgrep -f armrx`
  empty between trials, long window (`--warmup=60 --seconds=180`+), reversed trial order,
  cycles/hash + instructions/hash as primary metrics (not wall-clock), log temps.
- isolcpus OFF for builds, ON + `/proc/cmdline`-verified for decision measurements.
- If devbox MCP drops: reconnect once, else fall back to direct ssh.
