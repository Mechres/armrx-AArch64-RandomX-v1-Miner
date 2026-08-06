# armrx Independent Audit — 2026-08-07 (unattended, read-only)

**Scope:** JIT emission path, TUI, pool-mining lifecycle, signals, DAG scheduler, perf-lever landscape, doc/code consistency. HEAD = `8aece22`, working tree clean. All findings below were verified by direct code reading (and where noted, by device logs); nothing was modified. Device: MSM8929 / 8× Cortex-A53 @ 765 MHz, postmarketOS/musl.

**Evidence base for the headline numbers:** the DAG A/B device logs survive at `/tmp/cross-dag/{b1,m1,m2}.log` (contents match the changelog tables exactly — baseline 171.63M cyc / 113.77M instr / 5.12 H/s; DAG 189.24M / 127.33M / 4.39 H/s, both runs reproducible to 3 decimal places). This is the only on-device data available; `/tmp` is a tmpfs and no DAG-era TUI/SIGINT logs exist.

---

## 1. Prioritized defects (file:line, FACTS verified by direct reading)

### D1 — HIGH: Dangling `std::string_view` use-after-free in the TUI, every frame, both modes
- `TuiSnapshot::pool_name` is `std::string_view` — `include/armrx/tui.hpp:21`.
- `PoolManager::current_pool_name()` returns `std::string` **by value** — `src/pool_manager.cpp:34-38`, header `include/armrx/pool_manager.hpp:48`.
- Stored at `src/miner_app.cpp:443` — `snap.pool_name = pool_mgr->current_pool_name();`. The temporary is destroyed at the end of the full expression; the view dangles.
- Dereferenced every 1 Hz frame at `src/tui.cpp:76` (`std::string pool_display(s.pool_name);`).
- **FACT:** this is the only provable memory-unsafety on the TUI path. It is a UAF in *both* DAG and non-DAG mode — which already explains bug (b)'s garbage text. (Other snapshot fields are safe: `snap.mode` is a `const char*` literal — `memory.hpp:41`; `snap.worker_rates` is a span into a loop-local vector alive through `render()` — `miner_app.cpp:459-463`.)

### D2 — HIGH: Unbounded blocking network I/O + mutex inversion ⇒ SIGINT teardown hang (bug c)
- **No `SO_SNDTIMEO`/`SO_RCVTIMEO`/connect timeout is set anywhere in `StratumClient::connect()`** (`src/stratum_client.cpp:86-226`); `write_all()` does an unbounded blocking `::send()` (`stratum_client.cpp:337-357`).
- A worker that finds a share calls `share_callback` (`mining_engine.cpp:554, 604`) → `pool_mgr->submit_share` which **takes `stratum_mutex_`** and blocks in that `send()` (`pool_manager.cpp:104-108`).
- On SIGINT, `run_pool_mining` exits the loop and calls `pool_mgr->disconnect()` (`miner_app.cpp:528`) which **waits on the same `stratum_mutex_`** (`pool_manager.cpp:96-102`) → if a worker holds it inside a stuck `send()` (half-open TCP connection), teardown blocks forever. `engine.stop()` (`mining_engine.cpp:202-217`) then also can't join that worker.
- The CryptoNote keepalive thread has the same shape (`stratum_client.cpp:684-703`: `send_mutex_` + blocking `send`).
- **Corroboration (FACT):** the team already knows this teardown hangs — `--pool-test` deliberately skips it via `std::_Exit(0)` with the comment "pool_mgr->disconnect()/engine.stop() can block waiting on the network/worker threads" (`miner_app.cpp:520-526`; `changelogs.md:37`). Bug (c) is that documented hang reached via Ctrl-C.
- **Secondary:** SIGINT arriving while the main thread is inside `tick()` → `connect_to_current()` → blocking `::connect()` (`stratum_client.cpp:127`, no connect timeout) delays exit by the OS TCP timeout (minutes). This variant would *not* print the final line, so it doesn't match the reported symptom, but it's the same class.

### D3 — MEDIUM: Unsynchronized multi-thread `std::cout` writes (bug b's interleaving)
- TUI renders from the **main thread** (`miner_app.cpp:463` → `tui.cpp:160`); share callback prints to `std::cout` from **worker threads** (`miner_app.cpp:387`); stratum/job logs print to `std::cout` from the **reader thread** via `log.hpp:116` (e.g. "New job…" → `engine.set_job` → `ARMRX_LOG_INFO`). No mutex serializes any of it.
- Additionally the TUI's cursor-relative redraw assumes exclusive screen ownership: the startup banner (`miner_app.cpp:549-561`, begins with `"armrx "`) is printed before the TUI exists and is **never cleared** — `prev_lines_` starts at 0 (`tui.cpp:162`), so the first frame renders below the banner and every frame's `\033[A\033[2K`+`\033[J` math is wrong whenever anything else touched the screen. This directly produces "binary name `armrx` followed by raw control bytes inline" + overlapping lines.

### D4 — MEDIUM/LOW: JIT emission buffer has no runtime bounds checks
- `emit32`/`emit64` do unchecked `memcpy(code + codePos, …)` — `include/armrx/jit_compiler_a64.hpp:171-181`. Safety rests entirely on static worst-case math (`jit_compiler_a64.cpp:39-46`; `.fill` in `jit_compiler_a64_static.S:308`).
- Main-VM region: real margin (≤ ~5,120 words vs 12,288-slot `.fill`). **Superscalar region: theoretical overflow if a program ever had >256 IMUL_RCP** (budget `(RANDOMX_SUPERSCALAR_LATENCY*3+2)*16` = 8,192 B/program, `jit_compiler_a64.cpp:120-133`; worst case ≈ 9,220 B). Unreachable with the current generator (constraints in `src/superscalar.cpp:204-214, 568-656`), so **HYPOTHESIS**: latent, not exploitable today.

### D5 — LOW: NEON scratch registers clobber the 32-bit literal registers (gated flag only)
- Under `ARMRX_ENABLE_JIT_FAST_DIV_SQRT` (default **OFF**, `CMakeLists.txt:21`), `h_FDIV_M`/`h_FSQRT_R` use v0–v3 as scratch (`jit_compiler_a64.cpp:2048-2139`) with a comment claiming they're "safe — not used by other instruction handlers". But `emitMovImmediate`'s literal path emits `smov/umov dst, vN.s[M]` for N = v0–v3 (`jit_compiler_a64.cpp:1431-1445`), with v0–v3 loaded once in the prologue (`jit_compiler_a64_static.S:206-221`) and read **at runtime mid-loop**. A FDIV_M/FSQRT_R earlier in the emitted program corrupts later literal loads. **HYPOTHESIS (high-confidence mechanism, zero observed exposure, non-default flag only)** — flagged for completeness, not urgent.

### D6 — LOW: Dead C\*-literal-pool machinery contradicts docs/header
- `cpoolBase_/cpoolSlot_/cpoolLiteralPos_` (`hpp:162-167`) are **written** (`jit_compiler_a64.cpp:985-986, 1310-1312`) but **never read**; `emitCpoolImmediate` always emits the 3-instr MOVZ/MOVN+MOVK form and never emits `LDR_LITERAL` (`jit_compiler_a64.cpp:1476-1509`); the `SuperscalarCpoolSlots = 128` reservation (`:1307-1312`) is 8 KB of jumped-over dead padding per dataset build. The header comment (`hpp:155-167`) still describes the nonexistent LDR-pool mechanism. `changelogs.md:9`/`README.md:133` claim the machinery "was removed as dead code" — false (only the LDR emission is gone). Pure dead weight; zero runtime cost in the hot path.

### D7 — LOW: Latent uninitialized state / unguarded dumps
- `flags` (`hpp:169`) is only set by `setFlags` (called once, `vm.cpp:155`); any `generateProgram*` without it reads an uninitialized `uint32_t` (UB, unreachable in current callers).
- `dumpJitCode` dereferences `jit_dump_.back()` unguarded (`jit_compiler_a64.cpp:1137, 1150`) — crash if empty; only reachable via `--jit-dump` which always hashes first (`miner_app.cpp:97-128`).

### D8 — INFO: `sockfd_` shared across threads without synchronization
- `close()` from the disconnect thread while the reader/keepalive threads may be inside `recv`/`send` (`stratum_client.cpp:59-84`) — the fd is a plain `int`. Linux `shutdown(SHUT_RDWR)`-first mitigates the common case, but the pattern is a latent close-vs-use fd race (possible fd reuse). Low exposure in this single-purpose process.

**Not a defect (verified):** the mining path is race-free w.r.t. the JIT — `JitCompilerA64` is per-worker (`vm.cpp:152-157`, `mining_engine.cpp:401`), no shared mutable JIT state across threads; worker/engine counters are atomics or padded atomics (`mining_engine.cpp:619-620`); `engine.stop()` normally joins within one hash (~200 ms) since workers poll `running_` every iteration (`mining_engine.cpp:428`).

---

## 2. The three OPEN bugs — assessment

### (a) `--tui` segfault under `ARMRX_DAG_SCHED=1` (after ~10-20 s)
**No backtrace has ever been taken** (the handoff itself says so — `docs/briefs/2026-08-07-handoff.md:47-48`), so "crash in the TUI render/shutdown path" (STRATEGY.md:130-137) is an **attribution, not a finding**. There is also no TUI thread — rendering is inline on the main thread (`miner_app.cpp:441-463`), so "render/shutdown path" means a main-thread fault.
What the code proves: the **only** memory-unsafety on this path is D1 (the dangling `pool_name` view), which is active in both modes. Why DAG specifically crashes while non-DAG only shows garbage is a **hypothesis**: under DAG, worker threads churn the heap ~9 allocations × 8 workers per hash (`scheduleProgramDag`, `jit_compiler_a64.cpp:749-752, 786-787`), so the freed pool-name chunk's contents/state differ, and the read lands on different (possibly heap-poisoned or, for long pool names >15 chars, previously-unmapped-then-reused) memory. An alternative unproven hypothesis: a DAG-path heap out-of-bounds that only surfaces when the TUI's per-frame `stringstream` allocations (`tui.cpp:110`) traverse corrupted malloc metadata. **Neither is provable offline.** Fixing D1 is mandatory regardless; the planned `gdb` backtrace (one on-device run, per TESTING.md §8) is the correct next step and should settle it.

### (b) `--tui` garbage / control bytes (scheduler-independent)
Three code-visible contributing defects, all FACTS: D1 (garbage pool-name text), D3 (concurrent `std::cout` writers: worker share prints `miner_app.cpp:387`, reader-thread stratum logs `log.hpp:116`, main-thread TUI render `tui.cpp:160`, zero serialization; plus the never-cleared startup banner making the cursor-relative redraw misalign), and `render()` emitting escape sequences unconditionally (only *color* is gated on `isatty` — `tui.cpp:22-31, 158-160`). The reported "`armrx` + raw control bytes" is the banner's first word followed by the TUI's `\033[...` redraw sequences. Fixing D1+D3 and clearing the banner before the first frame addresses the reported symptoms.

### (c) SIGINT on `--pool` may not exit cleanly
**The loop-side fix is already in place** — `run_pool_mining` uses the ten-100ms-slice pattern (`miner_app.cpp:418-419`), so STRATEGY.md:157's candidate "the pool-mining teardown path... likely the same 100ms-poll pattern applied to `run_pool_mining`'s loop" is stale (the loop already re-checks). The hang is **downstream, in `pool_mgr->disconnect()` / `engine.stop()`** (`miner_app.cpp:528-529`), via the D2 chain: worker blocked in an unbounded `send()` while holding `stratum_mutex_` → `disconnect()` blocked on that mutex → `engine.stop()` can't join the worker. The team's own `--pool-test` workaround (`std::_Exit(0)`, `miner_app.cpp:520-526`) documents that this teardown hangs. The terminal-delivery alternative remains plausible and **cannot be resolved offline** — the documented `kill -INT` from a second SSH session is the right discriminator. Note: even without a stuck share-send, SIGINT during a blocking reconnect `::connect()` (no timeout) delays exit by minutes.

---

## 3. DAG scheduler — gate confirmed; documented regression mechanism is **wrong**

**Gate — CONFIRMED correct and harmless when off (FACT).** `ARMRX_DAG_SCHED` is a runtime env-var check (`std::getenv(...) == "1"` at `jit_compiler_a64.cpp:946-947`); with it absent, `scheduleProgram` (the legacy scheduler) runs exactly as before — the OFF cost is one `getenv` + branch per program compile. It appears **nowhere in CMakeLists.txt** — by design per `docs/briefs/2026-08-06-dag-scheduler-attempt.md:66,113`. Caveat: any deployed binary can activate the DAG path (and the documented TUI crash) by setting the env var — the gate is advisory only. Do not re-enable; nothing here changes that verdict.

**Why it regressed — the documented root cause is contradicted by code + device data (FACT of contradiction).** `changelogs.md:30` claims the reorder "forces a *longer emitted sequence* (order-dependent codegen)". The code shows:
1. **Per-opcode emitted length is order-invariant** — every handler's emission depends only on the instruction's fields; the single order-dependent case (`h_IMUL_RCP`'s `literal_id < 12` single-instruction form, `jit_compiler_a64.cpp:1823-1847`) is provably preserved because IMUL_RCPs are `is_fixed` anchors and the DAG returns identity whenever a fixed node isn't ready at its slot (`:759-763, 863-882`). CBRANCH replay anchors are likewise pinned (`:770-784`).
2. **The extra instructions are the planner's own per-hash runtime**, counted by perf: an O(n²) hazard matrix (32,576 `hasHazard` evaluations per compile, `:788-797`) plus ~9 heap structures (`:749-752, 786-787`), executed once per hash (main program compile).
3. **Device logs prove it:** across the 500-hash gated window, **sys time tripled** under DAG (2.13 s → 7.5 s — malloc/mmap churn from per-hash allocations) and user time rose +10% (b1.log vs m1/m2.log), while instr/hash rose +13.56M (113.77M → 127.33M, reproducible M1≈M2). H/s −14.3% is consistent with compile cost growing from ~1.76% of hash time to ~16% (benchmark-v2 baseline: JIT compile is 1.76%).
So: the regression is **scheduler runtime cost (per-hash planning), not emitted-code length** — the docs' mechanism should be corrected. The "emitted +13.6M extra instructions" phrasing is wrong on both counts (nothing extra is emitted; the count is the planner executing).

---

## 4. Remaining performance levers — ranked (facts vs hypotheses)

1. **Multi-worker `bench_armrx` (handoff Lever 3) — SOUND, the correct next methodology step. Low risk.** Confirmed: `bench_armrx.cpp` has no `--workers` parse at all; `--full-hash-only` runs one VM sequentially (`:798-799, 872-927`, `kSamples=500` at `:915`). It is the prerequisite for measuring the residual 8w instruction-mix gap, which is currently only visible as H/s. Design caveat: the window must be CPU-saturated with no printer-thread interference (the documented `--mine` census corruption mode).
2. **`-mtune=cortex-a53` (handoff Lever 1) — ALREADY DONE and ALREADY MEASURED NULL. Do not spend a session.** The handoff's premise is false: the cross toolchain file *already* sets it (`cmake/toolchain-aarch64-musl.cmake:66-67`), and E18 measured it exactly null on-device (`docs/experiments/perf-tracking.md:170, 546`: "4.32 with, 4.32 without"). `STRATEGY.md:24` correctly lists it as closed. Zero ROI.
3. **Worker-cluster placement (handoff Lever 2) — as stated, UNSOUND: a quantified regression.** Default affinity is already `AffinityMode::All` (`mining_engine.hpp:168`), not Unpinned as the handoff guessed; workers already land 1:1 on 0-7 (`mining_engine.cpp:367-374`). Pinning 8 workers to 0-3 discards the weak cluster's real contribution (~16.45 H/s at 4 workers vs ~25.28 at 8, per the team's own sweep — `alpha-changelogs.md`), and the "isolcpus → 28.4" number it's built on is burst-only (~30-45 s; sustained ≈ 24.4-24.8 — `isolcpus-rt-priority-win.md`). **The defensible restatement is real and open:** main-thread/worker-0 contention on core 0 in pool mode (main + stratum thread stealing ~1 H/s at 8w ≈ **+2-4% potential**), fixable by scheduling only (deprioritize main thread / lighter per-second printing) — explicitly left open by `isolcpus-rt-priority-win.md`. Low risk, measurable via the 8w real-pool A/B.
4. **NEW — LTO on the GCC 16.1.0 cross toolchain (missed by the team).** The shipping cross build forces LTO off (`toolchain-aarch64-musl.cmake:57`, `ARMRX_DISABLE_LTO=ON`); the "GCC 15 + musl LTO crash" that closed the lever was root-caused and fixed in `CMakeLists.txt:379-390` (fortify-headers vs LTO, scoped `-U_FORTIFY_SOURCE`), and `perf-tracking.md:569-571` explicitly notes LTO on/off could never be A/B'd on the cross toolchain — i.e. **cross-GCC-16-LTO was never actually tested**. Prior: when LTO linked on-device it was **+1.9%** (`perf-tracking.md:169`). Expected ~+1-2%; risk = link crash (revert, one session), KATs gate correctness. HYPOTHESIS with a positive prior.
5. **Cheap flag batch (`-fno-plt`, `-ffunction-sections`/`-Wl,--gc-sections`, `-falign-functions=64`) — expected null.** JIT code is ~98% of cycles; I-cache miss rate 0.788% — ceiling ~2%, realistic ~0. Worth one batched build only with spare budget (not in the closed lists, but low expected value).
6. **BOLT / interpreter-path prefetch — skip** (toolchain absent; JIT-only production path).

Do not re-litigate: hugepages (E9, +0.8%), PGO (null ×2), PRFM (triple-closed), `-Ofast` (consensus-critical FP; `-frounding-math` deliberately set), `*_M` scheduler (W3-2, reverted), E26 hoist (segfault, closed), Clang swap (null).

---

## 5. Doc/code inconsistencies (verified)

| # | Doc | Claim | Reality (file:line) |
|---|---|---|---|
| 1 | `README.md:132` | HW-AES "−16.7% (107.36M → 89.47M), largest win… armrx now below XMRig" | **Retracted** — reproducible gated re-baseline: −5.8% → 101.10M, ~7% *heavier* than XMRig (`changelogs.md:12,39`; `measurements/2026-08-06-head-rebaseline.md`). README is the only top-level doc not corrected. |
| 2 | `README.md:64,66` | isolcpus worker-count/pool-pinning "**Not yet fixed in code**" | T2-3 shipped 2026-08-01: workers capped + pinned to isolated cores (`mining_engine.cpp:27-43`, `cli_parser.cpp:36-40`); README's own `:139` says so. Self-contradiction. |
| 3 | `README.md:40` | "8× Cortex-A53 **(4×1.1 GHz + 4×1.4 GHz)**" | Fixed 765 MHz, no cpufreq (`TESTING.md:7`; commit `290e323`). |
| 4 | `README.md:133`, `changelogs.md:9` | W4 cpool machinery "**removed as dead code**" | Machinery retained: `cpoolBase_/cpoolSlot_/cpoolLiteralPos_` (`hpp:162-167`), 8 KB reservation (`jit_compiler_a64.cpp:1307-1312`), `emitCpoolImmediate` still called (`:1350,1356`). Header comment describes a nonexistent LDR mechanism. |
| 5 | `TESTING.md:245` | "instr/hash **113.8M (1w AND 8w)**", IPC 0.662 | Pre-AES; authoritative = **101.10M / IPC 0.667**; the "8w" column is a single-threaded artifact (bench ignores `--workers`; `rebaseline.md:60-71`). `TESTING.md:29` also still presents the superseded 89.5M census as the "reference baseline" — contradicts its own §7. |
| 6 | `changelogs.md` | — | Missing: the OPEN SIGINT-on-pool bug (HEAD `8aece22`, STRATEGY.md:148-159), the 2026-08-07 handoff, and `ec30c13` (real 1-line code change, DAG swap-budget reload). |
| 7 | `handoff:31-39` | "cross-build has **no -mtune**"; "Default affinity **may be Unpinned**" | Toolchain sets `-mtune=cortex-a53` (`toolchain:66-67`); default is `AffinityMode::All` (`mining_engine.hpp:168`). |
| 8 | `changelogs.md:30` / `handoff:86` | DAG regression = "longer emitted sequence / order-dependent codegen" | Contradicted: planner runtime (see §3), proven by code + device logs (sys 2.1→7.5 s). |
| 9 | `STRATEGY.md:157` | "likely the same 100ms-poll pattern applied to `run_pool_mining`'s loop" | Pattern already present (`miner_app.cpp:418-419`); the gap is teardown, not the loop. |
| 10 | `STRATEGY.md:24, 148-159`; `TESTING.md` gate names/counts; `changelogs.md` SIGINT/Metrics "FIXED" entries | — | **Consistent** — verified against code (sigaction `sa_flags=0` + 100ms slices, `miner_app.cpp:64-80, 418-419`; `server_fd_` gone from `metrics.hpp`; tests exist with 16/450/200 pairs). |

---

## 6. What I could NOT verify (and why)

1. **The `--tui`+DAG segfault's actual fault site** — no backtrace exists anywhere (device logs/dirs contain none; the `/tmp/cross-dag` logs predate the DAG-era runs and only the A/B bench + gate logs survive). Reproduction would require a live 8-worker pool run on-device, which is both outside read-only constraints and the TESTING.md one-test discipline. The UAF (D1) is proven; its role in the segfault is a hypothesis.
2. **Bug (c): code-vs-terminal-delivery** — needs the live `kill -INT` test from a second SSH session. Not run (read-only + no pool credentials in scope). The D2 deadlock chain is code-proven but its *occurrence* in the reported session is not.
3. **Any fresh on-device measurement** — qemu forbidden, full suite forbidden, one-test-per-session discipline, and no binaries were shipped (device state untouched). The AES −5.8% figure and all H/s baselines are taken from the team's own logs/docs, cross-consistent but not re-measured.
4. **D5 (fast-div/sqrt NEON clobber)** — mechanism verified in code, but the flag is OFF by default and no build/run was made to confirm an actual mis-hash (would require a special build + KAT run).
5. **D4 superscalar overflow** — practical unreachability depends on generator statistics I did not simulate.
6. **Terminal-specific behavior of bug (b)** (the user's `lenovo` emulator) — cannot be reproduced here; the code-side defects (D1/D3/banner) stand independently.

**Bottom line:** two of the three OPEN bugs have concrete, code-proven root causes (b: D1+D3+banner; c: D2, with loop-side already fixed); bug (a) has one proven memory-unsafety (D1) that is likely involved but the segfault mechanism is unproven until a backtrace exists. The DAG gate is correctly closed; its documented regression mechanism should be corrected to "per-hash planner runtime" (evidence: code + `/tmp/cross-dag/{m1,m2}.log`). Of the remaining perf levers, only multi-worker `bench_armrx` and cross-toolchain LTO are worth sessions; `-mtune` is done+null, and cluster pinning as proposed is a regression.
