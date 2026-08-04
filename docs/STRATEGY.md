# Strategy — armrx Performance Program

## Era I — "The Stall Mirage" (closed)

*Why the name:* for ~2 weeks we chased `ld_dep_stall` / the `*_M` load-use bubble across the
scratchpad path on an in-order Cortex-A53. It looked like the gap (E19: armrx had +63%
`ld_dep_stall` growth 1w→8w; the E26 hoist was built to hide it). Then **M1 measured XMRig's
8w side and found XMRig pays the SAME stall** (30.1M vs 27.0M/hash, within noise). The oasis
was heat-shimmer. We had been optimizing a metric that was never the differentiator.

### What Era I established (the real wins, keep these)
- **E24** (+7.1% 1w): pad C* immediates to break the A53 4-cycle MAC interlock. *Real 1w win.*
- **E25**: skip redundant `add` on zero-offset `*_M` (equivalence-safe code-size win).
- **isolcpus +14%** and **GCC 16.1.0 cross-build +7.9%** — the two biggest wins were
  operational/toolchain, not code.
- Hardware AESE/AESD AES funnel; NEON-TTABLE AES (Track G) default ON.
- A **discipline**: every claim gated by KAT + 450/200 stress + PMU, every dead lead archived.

### What Era I exhausted (do NOT reopen — evidence in `docs/archived/`)
- `*_M` scheduler extension / E26 hoist — diverges JIT/interpreter (W3-2, SEGFAULTs). Dead.
- CBRANCH replay-map redesign — structurally unsafe without an emit-time byte-offset map;
  M1 weakened its ROI (the stall it would hide is not the competitive gap). Deferred.
- PRFM scratchpad hints, hugepages, PGO, dual-issue alignment, Track C, CSEL CBRANCH,
  FDIV/FSQRT, `-mtune=cortex-a53` — all measured null/regression. Closed with evidence.

### The contradiction — RESOLVED (Phase 0, `p0-instruction-count-resolution.md`)
Our own numbers disagreed on whether armrx emits *more* or *fewer* instructions than XMRig:

| source | armrx instr/hash | trust |
|---|---:|---|
| 1w census (w11) | 89.5M | **WRONG** (mis-divided/non-500 window) |
| 8w M1 (pool `Total`) | 103.5M | artifact (pool hash-count undercount) |
| **Phase 0 gated `--perf-ready` (1w AND 8w)** | **113.8M** | **authoritative** |

Phase 0 measured armrx with the self-counting 500-hash gated window at both 1w and 8w:
**113.8M instr/hash, flat across worker count (0% Δ)**. IPC 0.662, flat. This overturns BOTH
prior numbers. Compared to XMRig's M1 98.9M, **armrx is +15% HEAVIER per hash** — not leaner.

**Resolved direction (FINAL — updated through 2026-08-06, post re-baseline):** Two findings
supersede the original E3b "8w-NEUTRAL / contention-IPC" framing, and **both are now in the shipped
tree**:

1. **Hardware-AES (Item 1, `2687a2e`, 2026-08-02) shipped — a real but MODEST instruction-count cut.**
   The original A/B claimed 107.36M → 89.47M (−16.7%, "largest win in history, below XMRig") — that
   89.47M was a **contaminated-divisor artifact**. The reproducible 2026-08-06 HEAD re-baseline (two
   runs identical to 0.00006%) gives **101.10M instr/hash**, i.e. AES saves **−5.8%
   (107.36M → 101.10M)**. armrx at 101.10M is **~7% HEAVIER than XMRig (94.5M)** at 1w — the
   "below XMRig / gap closed on instruction count" claim was wrong. H/s parity still holds (1w 5.11 vs
   5.04, E24 real-pool) because armrx's better IPC (0.667 vs ~0.654) compensates the heavier count.
   The E24/C* padding E3b traced is genuinely IPC-preserving and 8w-neutral, so "instruction-count cuts
   are 8w-NEUTRAL" holds for *those* candidates — but AES (a density cut in `aes_hash.cpp`, not the
   JIT) is the one real lever, and it's modest, not a 16.7% swing.

2. **The 8w "contention-IPC / reduce-memory-traffic" axis is closed on evidence.** The 2026-08-05
   Reasonix audit (`reasonix_full-audit_2026-08-05.md`) root-caused the residual `*_M` `ld_dep_stall`
   as **L2/contention-class latency (25–40 cyc/op)**, localized to the main-VM region (9.9% instr /
   18.1% cycles, IPC 0.405) — the **fixed RandomX light-mode penalty that XMRig also pays**. The only
   software attack on it (E26 `*_M` hoist) **SEGFAULTED and is closed**; the scratchpad access pattern
   is spec-fixed, so "reduce memory traffic per hash" is not achievable. No remaining software lever.

⇒ **H/s parity IS the achieved outcome** (1w armrx 5.11 ahead of XMRig 5.04; 8w 26.65 vs 28 =
95.2%). The *instruction-count* gap is NOT closed (armrx ~7% heavier at 1w) — but that does not
prevent parity, because armrx wins on IPC. The remaining 8w shortfall is the SoC's own two-cluster
asymmetry (weak cluster = 0.53× fast; 8w ≈ 65% of linear scaling) which **XMRig bears identically** —
an SoC fact, not a code gap. `isolcpus` is an operational deployment knob (not an armrx feature, not
assumed on real devices) and is excluded from the baseline methodology. The clean gated 1w re-baseline
at HEAD is **DONE** (`measurements/2026-08-06-head-rebaseline.md`).

---

## The Era II principle (learned from the Mirage)

> **Lock the metric before you optimize it.**

We spent E24→E26→2 audits optimizing `ld_dep_stall` — a metric M1 proved isn't the gap. The
trap: optimizing a metric you haven't *confirmed* is the differentiator. In Era II every
experiment targets a metric that Phase 0 has locked as real, and is gated by the TESTING.md
sequence. No more phantom-chasing.

We also stop measuring *against XMRig* as the primary method. XMRig comparison gave us three
essential facts (stalls equal → not a stall gap; armrx leaner at 1w → not a density gap) and
now has diminishing returns (different build, hash-count accounting confounds). Era II optimizes
**our own IPC / instr-per-hash**, validated by our own 8w H/s (26.65 → 28) + PMU. XMRig becomes
a non-essential reference.

---

## Phases (Era II) — CLOSED; one open measurement remains

### Phase 0 — Resolve the 1w/8w contradiction — RESOLVED (113.8M, +15% heavier pre-AES)
Phase 0 locked the metric: armrx = 113.8M instr/hash vs XMRig 98.9M pre-AES. That measurement is
superseded by the shipped hardware-AES win (89.47M, below XMRig 94.5M). Phase 0's method (gated
`--perf-ready` 500-hash window) is the correct harness and is reused by the open re-baseline below.

### Phase 1 — Density experiment — SUPERSEDED
The E3b per-opcode diff and Clang A/B are moot for the residal gap: hardware AES (Item 1) closed
the instruction-count gap, and the E3b-traced `*_M`/C*/CBRANCH "excess" is IPC-preserving padding
(8w-neutral, proven by E24's A/B). The Clang cross-build A/B remains a *cheap, zero-risk* probe if
a future session wants to re-verify codegen density, but it is no longer the critical path.

### Phase 1.5 (DONE) — Clean gated re-baseline at HEAD (non-isolated)
The re-baseline is complete (2026-08-06): two identical runs give **101.10M instr/hash, IPC 0.667,
median 195.5 ms** at 1w (clock-valid 763 MHz). `bench_armrx --full-hash-only --workers=N` (Lever 3,
implemented 2026-08-07) now reports TRUE N-worker aggregate hash/s (shared cache, per-thread VM) — e.g.
8w ≈ 12.3 H/s pinned to 0-3 on MSM8929 vs 5.12 H/s @1w. The authoritative **real-pool** 8w figure
remains armrx 26.65 vs XMRig 28 = 95.2% (`perf-tracking.md` §0). `isolcpus` is an operational
deployment knob (not an armrx feature and not assumed on real devices), so it is NOT part of the
baseline methodology. See `docs/measurements/2026-08-06-head-rebaseline.md`.

### Phase 2 — Gate, keep, or revert (discipline unchanged)
Any future candidate: KAT 16/16 → 450 stress → 200 stress → 1w+8w H/s + PMU. Keep only if it
reduces instr/hash without regression. Archive the dead attempt with its evidence (per Era I).
The 95.2%→parity goal was **earned by measurement** (E24 + real-pool verification), not assumed.

---

## Known bugs to fix alongside (not perf, but real)
- **SIGINT/Ctrl-C ignored during pool mining** — FIXED (2026-08-06). Root cause was twofold:
  `std::signal()` was used (no `SA_RESTART` control) and, more importantly, the run loops used a
  single `std::this_thread::sleep_for(1s)` which **swallows EINTR and re-sleeps**, so the
  `keep_running` flag set by the handler was never re-checked until the full second elapsed (and
  libstdc++'s `sleep_for` re-loops on EINTR regardless of `SA_RESTART`). Fixed by: `sigaction` with
  `sa_flags=0` (SA_RESTART explicitly cleared) + replacing the 1s sleep with ten 100ms slices that
  re-check `keep_running`. Verified: `armrx --mine --mode=light` now exits cleanly within ~2s of
  SIGINT (graceful teardown, no `kill -9`). The worker threads already poll `running_` once per
  hash (~200ms), so `engine.stop()` joins promptly. (GLM §5.7-C.)
- **MetricsExporter data race** (`server_fd_` read in dtor without fence, written by bg thread) —
  FIXED (2026-08-06). `server_fd_` removed entirely; the listening socket is now created, used, and
  `close()`d solely inside the worker thread, so the destructor only flips `running_` + joins —
  no shared fd access. (GLM §5.7-E.)
- **TUI segfaults with `ARMRX_DAG_SCHED=1` — ROOT CAUSE FIXED (2026-08-07).** `armrx --tui`
  under the DAG scheduler ran correctly for ~10-20s then died with `Segmentation fault`.
  Corrected attribution (2026-08-07 audit): the crash was the **dangling
  `std::string_view pool_name`** — `TuiSnapshot::pool_name` bound to a temporary
  `std::string` from `PoolManager::current_pool_name()`, dangling by the time
  `render()` read it (UAF active in BOTH modes). The fix (see Bug 2 commit
  `c664195` + follow-up) makes `TuiSnapshot::pool_name` an **owned `std::string`**,
  eliminating the dangle. Verified on-device: `--tui --pool-test --seconds=15`
  now renders `armrx  <real pool name>  [light]` with **0 NUL bytes** in the
  header (was NUL-padded garbage before) and `DONE_EXIT=0`. The segfault only
  manifested with `ARMRX_DAG_SCHED=1` (gated OFF), whose different heap-reuse
  timing exposed the bad read; that read is now gone, so the segfault root cause
  is closed. A `gdb` backtrace was the originally-proposed confirmation but is no
  longer required — the UAF is structurally removed. (Reported 2026-08-06;
  reattributed + root-caused 2026-08-07 audit; fixed 2026-08-07.)
- **TUI emits garbage control bytes / overlapping lines — FIXED (2026-08-07).** Root cause was **twofold**:
  (1) `armrx::log::set_tui_mode(true)` was **never called** (audit: dead ring-buffer), so
  worker-thread `ARMRX_LOG_*` writes went straight to `std::cout` under `sink_mutex`; (2)
  `Tui::render()` / `Tui::shutdown()` wrote their ANSI escape sequences to `std::cout` **without**
  taking `sink_mutex`. The two writers interleaved on the fd, splitting escape sequences with log
  text → broken control bytes + overlapping lines. Fix: call `set_tui_mode(true)` when the TUI
  comes up (worker logs → ring buffer, off stdout) and `set_tui_mode(false)` after the loop;
  `render()`/`shutdown()` now lock `armrx::log::sink_mutex()` around their whole stdout write so
  the escape sequence is atomic. Also restored the cursor on the `--pool-test --tui` `_Exit` path
  (was skipping `tui->shutdown()` → hidden cursor + dashboard left on screen). Verified on-device:
  `--tui --pool-test --seconds=15` renders clean complete frames with no interleaved log text and
  exits `DONE_EXIT=0` (no crash). (`src/tui.cpp`, `src/miner_app.cpp`)
- **SIGINT on `--pool` may not exit cleanly — FIXED (2026-08-07).** Root cause was a
  **self-deadlock in `PoolManager::disconnect()`**: it held `stratum_mutex_` while
  calling `StratumClient::disconnect()`, which `join()`s the reader thread, whose
  teardown invokes `error_callback_` → `PoolManager::current_pool_name()` →
  `lock(stratum_mutex_)` → deadlock on every clean teardown. A secondary stall was
  an **unbounded blocking `send()`** (no `SO_SNDTIMEO`) that could wedge a share-
  submitting worker holding the same mutex. Fixed by: (a) `disconnect()` drops the
  mutex before tearing down the `StratumClient`; (b) `run_pool_mining()` calls
  `engine.stop()` *before* `pool_mgr->disconnect()` so no worker enters
  `submit_share()` during teardown; (c) `SO_SNDTIMEO` (2s) on the socket so a
  stalled `send()` returns and releases the mutex. **Verified on-device 2026-08-07:**
  `kill -INT <pid>` from a 2nd SSH session prints `Pool mining stopped.` + totals
  and returns to the prompt within ~2s. See changelogs.md (2026-08-07).
- **`MetricsExporter` shutdown blocks behind idle HTTP client — FIXED (2026-08-07).** The
  worker did a blocking `accept()` then a blocking `read()` per connection. An idle client
  (connected, sent nothing) pinned `read()` forever, and even *no* incoming connection left the
  worker stuck in `accept()` — so the destructor's `join()` hung teardown whenever `--metrics-port`
  was enabled. Fix: `poll()` the listen socket with a 250 ms timeout (shutdown unblocks even with
  no connection) + `SO_RCVTIMEO` (2 s) on the accepted client fd (idle `read()` returns and the
  worker loops). The listening fd stays worker-owned, preserving the prior data-race fix. Verified
  on-host with a header-only regression test: start exporter, open an idle connection, destroy it →
  teardown completes in ~2.0 s (was infinite hang); `/metrics` still serves the Prometheus body.
  (`include/armrx/metrics.hpp`)
- **IPv6 bare-address pool parse wrong (OPEN, LOW).** `cli_parser.cpp:181-202` splits on last `:`;
  bare `2001:db8::1` mis-parsed (host `2001:db8:`, port `1`). `[v6]:port` handled. Fix: detect `:` count /
  bracket form. (2026-08-07 audit.)
- **`PartialDataset` latent out-of-order publish (OPEN, LOW, NOT active mis-hash).** Fill workers advance
  `item_count_` via CAS to `start_item + total_items` (`partial_dataset.cpp:150-155`) — later chunk
  finishing first publishes earlier unfinished chunk as done. **Latent only:** dataset is fully built via
  `wait_for_fill()` before mining starts, so no hash reads mid-fill in normal flow. Real race smell;
  verify no path reads during fill. (2026-08-07 audit; downgraded from "correctness bug".)
- **`--pool-test --tui` skips cursor restore (OPEN, LOW).** `std::_Exit(0)` (`miner_app.cpp:520-525`)
  skips destructors → `Tui::~Tui()` never restores hidden cursor. (2026-08-07 audit.)

## Entry points for the next agent / session
1. `docs/TESTING.md` — how to measure (the only valid commands).
2. `docs/plans/era2-plan.md` — the concrete Phase 0/1/2 steps.
3. `docs/experiments/m1-miner-to-miner-pmu-diff.md`, `ab-e24-8w.md`, `perf-tracking.md` — the
   evidence that closed Era I.
4. `docs/audits/` — GLM, opencode, reasonix, gemini audits (context, some stale — check dates).
