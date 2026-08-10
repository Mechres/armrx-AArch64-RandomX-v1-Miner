# armrx — Changelog

> **Post-alpha archive:** For the full retrospective covering all 210 commits (2026-07-13 to 2026-07-30) and 10 performance tracks A–J, see [`RETROSPECTIVE.md`](RETROSPECTIVE.md).
>
> The complete alpha-phase changelog is preserved at
> [`docs/archived/alpha-changelogs.md`](docs/archived/alpha-changelogs.md).

## 2026-08-10 — docs: correct DAG scheduler stale root cause + closure label
- **Source:** post-audit re-review (Deepseek 🔴 #2). `docs/briefs/2026-08-06-dag-scheduler-attempt.md:13`
  and `ROADMAP.md:37` claimed the DAG regression was "reorder forces a longer emitted sequence"
  (order-dependent codegen). Branch `try/t8-dag-docfix`, commit `525488f`, merged `1d15d88`.
- **Correction (grounded in 2026-08-07 changelog `:418,429`):** emitted length is **order-invariant**
  under the DAG scheduler; the −14.3% H/s was **planner runtime overhead** (O(n²) hazard-matrix build +
  ~9 heap allocs/hash, `sys` time tripled), not more instructions. Also downgraded the overstated
  "exhaustively closed" label to "current implementation closed-negative; family not exhaustively
  disproven" — a cheap planner (bitmap hazard matrix, schedule reuse) is a legitimate reopening variant
  (small expected ROI, E20's 0.73% legal-move rate). Doc-only; no code change. Recorded in
  `docs/briefs/2026-08-10-postaudit-consolidated.md` §2C(b).

## 2026-08-10 — fix(test): hybrid --dataset-mb>0 KAT resolved (README correct, stale comment fixed)
- **Source:** post-audit re-review (Deepseek 🔴 #1) flagged a contradiction: `README.md:167-171`
  says hybrid `--dataset-mb>0` is "✅ correct and recommended (verified on-device same-nonce)" while
  `tests/test_mining.cpp:438-448` claimed the hybrid consumption path "is currently NOT producing
  correct end-to-end hashes on-device." Branch `try/t8-hybrid-kat`, commit `139bfd3`, merged `a6b0667`.
- **On-device gate:** built aarch64-musl `test_mining`, ran on Lenovo. The sibling test
  `test_light_mode_partial_dataset_matches_reference` is the same-nonce end-to-end KAT (hashes with
  the partial-dataset hybrid path, asserts `engine_hash == light-reference_hash` for same nonce).
  Result: `TEST_MINING_RC=0`, `ALL MINING TESTS PASSED`, including that test. **README is correct;
  the `:438-448` comment was STALE** (false claim of on-device wrong hashes). The recommended
  `--dataset-mb=512` config is verified safe — no live wrong-hash bug.
- **Change:** corrected the stale test comment to reflect the verified-correct state (kept the
  buffer-byte check as an isolation of the rotation-rebuild fix). README unchanged. Also recorded the
  resolution in `docs/briefs/2026-08-10-postaudit-consolidated.md` §4.

## 2026-08-10 — ci: disable optional OpenSSL in cross-build (TLS header fix)
- **Source:** GitHub Actions `cross-build` job failed compiling `tls_client.cpp`
  (`fatal error: openssl/opensslconf.h: No such file or directory`). Copilot suggested
  installing `libssl-dev:arm64`, but that needs `dpkg --add-architecture arm64` first or it
  fails again, and it compiles the TLS path the **real musl deploy doesn't use** (no OpenSSL
  there). Branch `try/ci-openssl-fix`, commit `045c4e2`, merged `784b018`.
- **Fix (chosen over Copilot's):** pass `-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE` to the CI
  configure. This force-skips the optional OpenSSL detection (TLS compiled out), exactly
  mirroring the shipping musl build. No OpenSSL install, no extra apt arch, self-sufficient;
  the JIT path still compiles. Verified locally with the identical flag: CMake reports
  "OpenSSL not found — TLS pool connections disabled", configure+build rc=0, `ARMRX_HAVE_TLS`
  unset. The workflow documents the alternative `libssl-dev:arm64` route in a comment for
  anyone who later wants CI to also compile-check the TLS path.

## 2026-08-09 — docs(T7): reconcile documentation drift
- **Source:** Hermes-led T7 from the consolidated Luna/Deepseek audit. Doc-only; no code change.
  Branch `try/t7-docsync`, commit `4d5c6f3`, merged `6acba99`.
- **Adopted:** (1) `AGENTS.md` Architecture: fixed stale "All AES uses software T-table path"
  → hardware AESE/AESD funnel is default on aarch64+crypto (Track-G NEON T-table also default
  ON), matching Gotchas. (2) `closed-levers-ledger.md` — added "Authoritative source" header
  (this file wins on status disagreements) + flagged historical `3,563` body figure superseded
  by W1-1 census `5,224`. (3) `perf-tracking.md` — "Measurement provenance" note for the `3,563`
  → `5,224` supersession (left historical mentions as-is). (4) `README.md` — "Hashrate figure
  provenance" note clarifying 24.95 / ~26 / 26.65 / 30-32 / 13.37 H/s are different measurement
  contexts, not contradictions.
- **Audit overstatements (verified, NOT changed):** (a) the ledger's "Worker-local buffer reuse
  (D2) — OPEN" is a *distinct* untried lever from README's adopted "Cross-hash boundary
  pipelining (Track D2)" — ledger is correct; (b) `STRATEGY.md` Known-bugs items 117-180 are all
  marked FIXED with dates — the audit misread them as stale.

## 2026-08-09 — build(ci): make .git_sha optional (clean-clone configure fix)
- **Source:** user-reported GitHub CI configure error. `CMakeLists.txt` read `.git_sha`
  unconditionally with `file(READ .git_sha ...)`; `.git_sha` is gitignored (written by
  devbox_sync/build host) so it is **absent in clean clones and CI**, causing a hard
  `cmake` configure failure. Branch `try/ci-gitsha-optional`, commit `f6be0e3`, merged `2d9a848`.
- **Fix:** `.git_sha` is now read only `if(EXISTS ...)`; if absent, fall back to
  `git rev-parse --short HEAD` (populates a real SHA when inside a git work tree), and
  finally "unknown" if git is also unavailable. Verified: configure + `armrx --version`
  succeed with `.git_sha` absent (printed `0.2.0 (9b0a00e, built ...)` from the git
  fallback); the `.git_sha`-present path still works.

## 2026-08-09 — T6: surface swallowed errors (diagnostics gaps)
- **Source:** Hermes-led T6 from the consolidated Luna/Deepseek audit (Adoption ledger
  `docs/briefs/2026-08-09-audit-consolidated.md`). Logging/counter-only fixes (no behavior
  change). Branch `try/t6-diag`, commit `eea37fd`, merged `a09cfe4`.
- **Adopted:** (1) `metrics.hpp` `socket()` failure now logs `strerror(errno)`; (2)
  `handle_set_difficulty` `catch(...)` now logs the malformed difficulty instead of swallowing;
  (3) `partial_dataset.cpp` `pthread_setaffinity_np` return checked + warns if pinning failed
  (no longer silently claims topology-aware fill); (4) `send_line` share-loss now increments a
  `shares_dropped_` counter (mirrors `shares_accepted_/rejected_`) + logs "(share dropped)".
- **Audit line-number corrections:** the `catch(...)` is at `:633` (not `:619`); the pinning
  call is `:186` (not `:179-183`); `write_all` send failure was already logged (only the
  counter was missing). Deferred: `--pool-test` hardcoded wallet (user-deferred, open-source
  prep).
- Gate (on-device, Lenovo): `armrx_tests` Input1/2 byte-identical; `test_partial_dataset` ALL
  PASSED.

## 2026-08-09 — T5-A/T5-B: CI cross-build + benchmarks out of CTest
- **Source:** Hermes-led T5-A/T5-B from the consolidated Luna/Deepseek audit (Adoption ledger
  `docs/briefs/2026-08-09-audit-consolidated.md`). No armrx CI existed; benchmarks were
  registered as CTest tests (slow/thermal-variant). Branch `try/t5-infra`, commit `9c189ed`.
- **T5-A (honest scope):** added `.github/workflows/ci.yml` — `cross-build` job (ubuntu +
  `crossbuild-essential-arm64`) cross-compiles the AArch64 JIT + test executables on every PR,
  proving the JIT compiles in CI (audit's core complaint). `device-kat` job documents the
  required on-device KAT gate but SKIPs without a self-hosted `aarch64-device` runner (not
  faked). Side fix: migrated `tools/verify_seed_rotation.cpp` off the pre-T2-B raw-pointer
  PartialDataset API (it had silently stopped building under `all`).
- **T5-B:** removed `bench_armrx` / `bench_opcodes` / `bench_imul_magnitudes` from `add_test`
  (kept as buildable opt-in targets). Verified: no `bench*` in `ctest -N`; cross build clean.

## 2026-08-09 — T4-A: make W^X JIT the default (RWX opt-out)
- **Source:** Hermes-led T4-A from the consolidated Luna/Deepseek audit (Adoption ledger
  `docs/briefs/2026-08-09-audit-consolidated.md`). On Linux AArch64, `RANDOMX_FORCE_SECURE`
  was NOT auto-defined, so the JIT code buffer defaulted to RWX (a known exploit primitive).
  Committed `d90b487`, merged `93aae9d` (origin/main).
- **Change:** added `ARMRX_SECURE_JIT` CMake option **defaulting ON** (W^X via
  `RANDOMX_FORCE_SECURE`); RWX is now the explicit opt-out (`ARMRX_SECURE_JIT=OFF`) for
  benchmarking. Only `CMakeLists.txt` changed.
- **A/B test (user directive: adopt only if real-world H/s not hurt):** on-device
  `bench_armrx` steady-state — RWX **5.13 H/s** vs W^X **5.12 H/s** (~0.2%, within noise;
  per-phase identical). W^X is a free security win with no hashrate regression. Correctness
  gate (WX build): `armrx_tests` byte-identical, `test_jit_equivalence` 16/16,
  `test_partial_dataset` ALL PASSED.

## 2026-08-09 — T3-C: derive resolveInstructionType from engine[256] ordering
- **Source:** Hermes-led T3-C from the consolidated Luna/Deepseek audit (Adoption ledger
  `docs/briefs/2026-08-09-audit-consolidated.md`). The audit OVERSTATED this as a large
  3-file unification; verified the interpreter `kCompileHandlers[256]` and JIT `engine[256]`
  were ALREADY macro-derived from `instruction_weights.hpp`. The only drift-prone piece was
  the hand-maintained `resolveInstructionType` if-ladder. Committed `33c8ae3`, merged
  `694ef21` (origin/main).
- **Scope (bounded):** replaced `resolveInstructionType`'s 256-branch handler-ptr→enum
  if-ladder with `static constexpr InstructionType kTypeOfEngine[256]`, built from the SAME
  `INST_HANDLE`/`REPN`/`WT` ordering as `engine[256]` (verified identical slot alignment),
  so the two can no longer drift. Implemented by an external coding agent on
  `try/t3c-opcode-table`, reviewed + gated by Hermes. Only `src/jit_compiler_a64.cpp` changed.
- **Gate (on-device, Lenovo):** `armrx_tests` Input1/2 actual == JIT hash byte-identical;
  `test_jit_equivalence` 16/16 byte-identical (decisive per-opcode scheduler-typing catcher);
  `test_partial_dataset` ALL PASSED. No hashing-behavior change.

## 2026-08-09 — T3-D: drop dead cfg.tui JSON field (conservative slice)
- **Source:** Hermes-led T3-D from the consolidated Luna/Deepseek audit (Adoption ledger
  `docs/briefs/2026-08-09-audit-consolidated.md`), verified against source before acting.
  Committed `b929d50` (origin/main).
- **Scope (safe slice only):** `AppConfig::tui` JSON parse (`config.cpp`) + field
  (`config.hpp`) were dead — only the CLI `--tui`/`opts_.use_tui` path drives the TUI
  (`miner_app.cpp`). Removed. The audit's other "dead code" claims (`subscribe_try_` inert
  fallback, `set_color`/`set_nonce_config` public API, 250 lines in `virtual_memory.c`, TLS
  EAGAIN) were verified to be live reconnect logic, intentional API, critical JIT-platform
  code, or latent/dead-on-shipping-build — all **left as-is** (overstated by the audit).
- **Behavior-preserving:** TUI still works via CLI `--tui`. Gate: `armrx_tests` Input1 actual
  == JIT hash byte-identical; `test_partial_dataset` ALL PASSED.

## 2026-08-09 — T3-B: name raw scratchpad/AND/bitfield A64 encoders
- **Source:** Hermes-led T3-B from the consolidated Luna/Deepseek audit (Adoption ledger
  `docs/briefs/2026-08-09-audit-consolidated.md`), gated on-device (Lenovo AArch64).
  Committed `c770b57` (origin/main).
- **Scope:** the `ARMV8A::` namespace already named core ALU/FP opcodes, but the scratchpad
  memory-op and masking emitters still used bare hex bases. Added named constexprs
  (`LDR_64_REG`, `LDR_64_REG_LSL3`, `LDR_64_SCALAR`, `LDR_32_REG`, `AND_IMM_32`,
  `AND_IMM_64`, `AND_IMM_SHIFT`, `UBFX`) and replaced the raw literals at their emit sites.
- **Pure naming change:** no emitted instruction differs (audit overstated the remaining raw
  scope — vector FP/AES emitters are also raw but field-composed correctly; left as future
  cleanup, not a defect). Gate: `armrx_tests` Input1 actual == JIT hash byte-identical;
  `test_partial_dataset` ALL PASSED.

## 2026-08-09 — T3-A: make JIT register/label contract mechanical
- **Source:** Hermes-led T3-A from the consolidated Luna/Deepseek audit (Adoption ledger
  `docs/briefs/2026-08-09-audit-consolidated.md`), gated on-device (Lenovo AArch64).
  Committed `d19bcb6` (origin/main).
- **Hazard:** the JIT emitter derived template sizes from linker symbols
  (`CodeSize`, `PrologueSize`, `MainLoopBegin`, `ImulRcpLiteralsEnd`) and an integer-register
  map `IntRegMap[8]` that lived only as an informal comment block in `jit_compiler_a64_static.S`
  plus an independent constexpr in `jit_compiler_a64.cpp`. When the hand-written assembly
  changed layout without a matching C++ constant update, the emitter silently produced
  wrong-size / overlapping JIT code (the historical register-clobber hazard class).
- **Fix:** new `include/armrx/jit_contract.h` is the single source of truth — `IntRegMap`
  with `static_assert` invariants (8 distinct entries, in x0..x30) checked at compile time,
  plus `kExpected*` known-good label deltas. `jit_compiler_a64.cpp` reuses the shared
  `IntRegMap` and validates the computed deltas against the contract via a
  `[[gnu::constructor]]` check that aborts at load if the asm/C++ layout ever drifts.
- **Correctness unchanged:** no emitted instruction differs. Gate: `armrx_tests` Input1
  actual == JIT hash byte-identical; `test_partial_dataset` ALL PASSED; the load-time
  validator passes (no abort).
- **Fixup (`ec7be96`):** the first cut compared the emitter's `static const size_t` template
  sizes against the contract, but those are dynamic-init and the `[[gnu::constructor]]`
  order vs. them is unspecified — it could observe them zero-initialized and fire a false
  "drifted from contract" abort at load. The validator now computes the symbol deltas
  inline (init-order independent). Re-verified on-device.

## 2026-08-09 — T2-C: close lost-wakeup deadlock in PartialDataset fill waiters
- **Source:** Hermes-led T2-C from the consolidated Luna/Deepseek audit (Adoption ledger
  `docs/briefs/2026-08-09-audit-consolidated.md`), gated on-device (Lenovo AArch64).
  Branch `try/fix-partial-publish-race` → merged `79a7db2` (origin/main).
- **Bug:** `PartialDataset::fill_worker` raises the atomic `item_count_`/`fill_complete_`
  then calls `fill_cv_.notify_all()` **without holding `fill_cv_mutex_`** (the publish path
  is intentionally lock-free). A `notify_all()` landing in the window between a waiter's
  predicate check and its `futex_wait` is lost, and with no guaranteed spurious wakeup the
  waiter blocks forever — a genuine lost-wakeup deadlock, observed as `test_partial_dataset`
  intermittently hanging (and the `test_contiguous_publish_no_uninitialized_read:260`
  `item_count()==kChunkItems` assert flaking).
- **Fix:** `wait_for_fill()` and `wait_until_published()` now use `fill_cv_.wait_for(lock, 20ms)`
  re-checking the atomic predicate in a `while` loop instead of the notify-dependent
  `wait(lock, pred)`. Liveness no longer depends on a single notify. **Correctness unchanged:**
  published counters stay `std::atomic` (release/acquire with the fill writes); the CV is only
  a wakeup hint; hot publish path remains lock-free. No hashing-behavior change
  (`armrx_tests` byte-identical).
- **Gate:** single 400 s run `ALL PARTIAL DATASET TESTS PASSED`; undisturbed 3×`timeout 450`
  loop `RESULT pass=3 fail=0 hang=0`. (An earlier `HUNG(350)` coincided with aggressive
  15 s-interval ssh polling loading the housekeeping core running chunk 0; the undisturbed run
  PASSED, confirming no genuine deadlock remains.)

## 2026-08-08 — adopt 3 correctness/cleanup branches from Luna follow-up audit
- **Source:** Luna read-only audit (round 2) + Hermes on-device gating. 3 branches adopted
  into `main` via `git merge --no-ff`. All gated green on-device (Lenovo AArch64); `main`
  ctest 9/9 after merge. `try/` branches preserved as evidence.
- **L-2 — PartialDataset destructor joins fill workers on teardown:** the old destructor
  `detach()`ed still-running fill threads that kept touching `data_`/atomics after the object
  was destroyed → use-after-lifetime UB on any non-process-exit teardown (engine recreation,
  exception, `--dataset-mb` re-init). Now a `shared_ptr<std::atomic<bool>> stop_` (independent
  of `this` lifetime) is set in the destructor, workers always finish their in-flight chunk,
  and the destructor `join()`s all workers before `munmap`. First Luna attempt **crashed
  on-device** (SIGSEGV at the 256 MiB lagging-chunk teardown — host ASan/TSan missed it);
  revised to remove mid-critical-section early-returns → re-gated green (`test_partial_dataset`
  256 MiB PASS, exit 0). This is the workflow working: device caught what sanitizers didn't.
- **C2 — Hoist per-hash `next_block` vector allocation:** `mining_engine.cpp` allocated +
  freed a `std::vector<std::byte>` every pipelined hash inside the worker loop; now a persistent
  buffer reused via `swap()`. Removes allocator churn from the per-hash path (0 instructions
  removed; impact below noise but harmless). Gated: `test_mining` + `test_jit_equivalence` 16/16.
- **B — Skip redundant zero FP memory offset add:** `emitMemLoadFP` unconditionally emitted
  `add x19, xSrc, #0` when the masked FP memory offset was zero; now guarded by `imm != 0`
  (mirroring the integer `emitMemLoad` path). Negligible instruction saving (~0.007/hash) but
  correct and consistent. Gated: `test_jit_equivalence` 16/16 + `test_jit_encodings` 136/5 seeds.

## 2026-08-07 (night) — adopt 5 branches from independent audit: dead-pool removal, JIT bounds assert, stratum robustness, extranonce, redundant pipelined fill
- **Source:** 6-branch delivery from an independent read-only audit (verified against source +
  gated on-device per project discipline). 5 adopted into `main` via `git merge --no-ff`
  (P2 `diag-fill-stall` kept on `try/`, default OFF — debugging tool, job complete; the
  older parallel `try/remove-dead-superscalar-cpool` left unmerged as evidence).
- **P3a — dead superscalar C* literal-pool removal:** dropped `cpoolBase_`/`cpoolSlot_`/
  `cpoolLiteralPos_` (written, never read) + fixed two stale W4 comments. Pre-E24 leftover;
  E24 already switched emission to `MOVZ`/`MOVN`+`MOVK`. Byte-identical: `test_jit_equivalence`
  16/16 on-device.
- **P3b — JIT code-bounds assert:** `generateSuperscalarHash` `fprintf`+`abort`s if
  `codePos > CodeSize + CalcDatasetItemSize` (no silent overflow). No behavior change on
  valid programs (assert never trips: 16/16 on-device).
- **P3c — Stratum client robustness:** reader thread try/catch (closes remote-abort DoS);
  `mining.notify` seed `params[3]` (was `[4]`); difficulty `isfinite` + `2^64` clamp; job blob
  `>= 76 B` / seed `== 32 B` hex validation. `test_pool_protocol` green on-device (incl failover).
- **P4 — Submit extranonce handling:** Monero nonce is fixed 4-byte → submitted via `job.nonce_size`
  (not Bitcoin-style concatenated); one-time warning on V1 pools advertising extranonce.
  `test_pool_protocol` green on-device.
- **P1 — Redundant pipelined scratchpad-fill skip:** from the 2nd pipelined
  `randomx_calculate_hash` call, `init_scratchpad` 2 MiB fill skipped — Part D's AES writeback
  is threaded back as the next call's run key (byte-identical to what the fill would produce for
  the same seed). Principled-correct, not a hash shortcut. Verified correct (`test_mining` all
  pass, incl. seed-rotation partial-dataset) + live shares on-device. Steady-state A/B (Lenovo
  A53, light, 8w): **+0.91% (26.38 → 26.62 H/s, wins in both orderings)** — real but small.
- **Gate discipline honored:** every merge gated on-device before the next (test_jit_equivalence
  16/16 / test_pool_protocol all / test_mining + A/B); no rollbacks needed. Audit artifact:
  `docs/briefs/2026-08-07-source-verified-audit.md` (untracked working doc).

## 2026-08-07 (late) — device perf A/B: `--main-thread-policy` lever measured as NO-OP
- **Decisive gate for the core-0/main-thread deprioritization lever** (branch
  `try/main-thread-deprioritize`). Per the device discipline (rule §8: a
  main-thread/worker-0 contention win is only visible via the REAL pool, never the
  bench), ran a real-pool `--pool-test` A/B on lenovo / MSM8929 A53 (no isolcpus):
  - Both arms: `--pool-test --dataset-mb=512 --workers=8 --seconds=360 --warmup=0`,
    identical `taskset -c 0-7`, built-in test pool+wallet (no real creds).
  - **Baseline (`main`):** steady-state aggregate **~32.2 H/s** (live Speed 31.8–32.8;
    INST agg 31–33; fast-cluster 4.2–6.0 / weak-cluster 2.4–3.0 H/s per worker).
  - **Treatment (`--main-thread-policy=nice=10`):** steady **~31.5 H/s** (live 30.8–32.2;
    INST agg 29.6–33.1).
  - **Δ ≈ −0.7 H/s (≈ −2%), within run-to-run thermal/working-set noise (CPU held 57°C
    both arms).** Brief treatment-arm dips tracked CryptoNote job rotations, not policy.
- **Conclusion:** deprioritizing the main thread produces **no measurable H/s change**.
  Under `SCHED_OTHER` the OS already yields the non-realtime main thread to the
  `SCHED_OTHER` workers; the main thread's pool-tick/console work is too light/infrequent
  to steal meaningful worker-0 time. Measured WITHOUT `--rt-priority` (where workers on
  `SCHED_FIFO` would already out-prioritize the main thread, making the lever even less
  likely to help). Lever **CLOSED as measured-no-effect**; branch kept as evidence. The
  flag is opt-in (default = unchanged) so it is regression-safe if retained. Ledger updated
  (`docs/closed-levers-ledger.md`).

## 2026-08-07 (late) — device correctness gate: `--main-thread-policy` lever (branch `try/main-thread-deprioritize`)
- **Lever:** opt-in `--main-thread-policy=default|idle|batch|nice=N` deprioritizes the
  main (calling) thread in `MiningEngine::start()` via `sched_setscheduler`/`setpriority`
  (portable; no CAP_SYS_NICE needed to *lower* priority; default = unchanged behavior).
  Re-opened 2026-08-07 by independent review as the correct portable form of the
  core-0/main-thread contention lever (the earlier "structurally dead" closure used a
  false dichotomy and waived the per-family rule). See `docs/closed-levers-ledger.md`.
- **Device correctness gate (cross-built with AUR GCC-16 musl toolchain, run on
  lenovo / MSM8929 A53, fast-cluster-pinned):** ALL PASS —
  `test_jit_equivalence` 16/16 byte-identical; `test_mining` (real shares, bad-nonce
  recovery, partial-dataset seed rotation, light-mode reference match — 2 sub-tests
  SKIPPED as fast-mode-OOM-on-device, expected); `test_aes_hash`; `test_jit_determinism`;
  `test_jit_encodings`. The scheduling plumbing does not perturb generated programs,
  hashes, AES, or JIT emission.
- **NOT yet measured:** the decisive H/s gate is a real-pool `--pool-test` A/B
  (baseline vs `--main-thread-policy=idle`/`nice=N`), per the device discipline
  (a main-thread/worker-0 contention win is only visible via the real pool, never the
  bench). That A/B is a separate session; lever remains **OPEN** pending it. Branch
  kept unmerged as evidence.

## 2026-08-07 (night) — PartialDataset: contiguous-publish (closes audit C1, no behavior change yet)
- **Why:** the fill published `item_count_ = max(completed)` = the END bound of a
  worker's chunk. Cross-chunk, a faster chunk finishing later items could advertise
  `[0, end)` ready while an earlier lagging chunk's bytes were still uninitialized →
  any hashing worker reading those items (the hybrid hit/miss path) could get wrong
  hashes. This was masked ONLY because `wait_for_fill()` blocked all workers until
  `fill_complete_` (audit C1). To ever let workers hash during fill (killing the
  172s dead-start on `--dataset-mb=N`), the publish must be contiguous-safe first.
- **Fix:** `item_count_` now advances only over the *contiguous filled prefix*.
  New `contiguous_done_` cursor + per-chunk `chunk_done_` flags (set release after
  `initialize_dataset`); whichever worker closes the gap advances `item_count_`
  (release) over consecutive finished chunks. `start_fill` gained a test-only
  `chunk_delays_ms` hook (default empty = production no-op) so a test can force a
  lagging chunk. `wait_for_fill()` UNCHANGED — current miner behavior identical
  (workers still block); this is a pure safety upgrade enabling Part 2.
- **Verification (host x86_64, 2026-08-07):** new KAT
  `test_contiguous_publish_no_uninitialized_read` — 4 chunks, middle chunk lagged
  500ms; reader samples `item_count_` and checks every published item ==
  `generate_dataset_item`. Result: `item_count_` held at 1 chunk (64M) during the
  lag (never exposed the unfinished middle chunk), no uninitialized reads. Full
  `ctest` 9/9 PASS. On-device gating deferred to Part 2 (the behavior change).

## 2026-08-05 — FIX hybrid partial-dataset JIT consumed the wrong item
- **Bug:** the AArch64 hybrid hit path compared and indexed the raw dataset
  address item before applying the light-mode dataset offset, while the miss
  path derived the offset-adjusted item. Any cached hit could therefore use a
  different dataset item than the reference light-mode hash.
- **Fix:** apply the same patched dataset offset before the hit/miss bound check;
  both the direct load and derivation now use the identical item number. Added
  `test_light_mode_partial_dataset_matches_reference` in `tests/test_mining.cpp`
  to compare an engine hash against a fresh light-mode reference.
- **Verification:** host mining/KAT tests pass. **On-device (lenovo, aarch64,
  cross-built, 2026-08-07) independently re-gated by Hermes:** with
  `--dataset-mb` (items=65536) the engine hash now equals the light reference
  for both single job (`8c7c5d6169128438618971d99e43bdb28456cdba85ffee2509cc0f42a9ef2ef2`)
  and rotate mode (previously wrong: `241b3376…` / `cd5cc806…`);
  `test_mining` passes incl. `test_light_mode_partial_dataset_matches_reference`;
  `armrx_tests` JIT 16/16 EXIT=0. Residual: the new regression test can't be
  negated on host (x86_64 skips the AArch64 hybrid JIT), so its
  fail-without-fix was reasoned from pre-fix device hashes, not re-run.

## 2026-08-07 (latest) — FIX light-mode seed rotation now rebuilds the partial dataset
- **Bug (was OPEN in the 2026-08-07 handoff brief):** in hybrid light mode
  (`--dataset-mb=N`) the partial dataset was filled exactly **once**, gated by a
  one-shot `atomic_flag`. On a live pool seed-rotation (`set_job` with a new
  `seed_key`), `shared_cache_` was rebuilt but `start_fill` was never
  re-triggered → workers kept mining on the OLD seed's partial dataset → silent
  wrong hashes / invalid shares. Single-job KATs never exercised rotation, so it
  stayed latent. Full write-up in `docs/changelogs.md`.
- **Fix:** replaced the one-shot flag with a monotonic
  `partial_dataset_fill_generation_` counter. `set_job()` re-runs `start_fill()`
  with the new cache and bumps the generation on any seed-key change; each
  mining worker re-waits on `wait_for_fill()` before hashing again.
  `PartialDataset::start_fill()` made re-fillable (resets progress + joins any
  prior fill threads). New regression test `test_refill_with_new_seed`.
- **Verification (host x86_64 + on-device aarch64, 2026-08-07):** host
  `test_partial_dataset` (incl. refill), `test_mining` (incl. new
  `test_light_mode_seed_rotation_rebuilds_partial_dataset`), `armrx_tests` (JIT
  16/16), `test_aes_hash`, `test_config` all PASS. **On-device (lenovo, aarch64,
  cross-built):** `test_mining` passes the rotation test — the partial-dataset
  buffer is byte-verified rebuilt with the NEW seed after rotation (log: "seed
  rotation — restarted background fill, workers will re-wait"). Only
  `test_cli_parser` fails — pre-existing, parser/test inconsistency unrelated to
  this change (confirmed on a clean stash tree).
- **Separate pre-existing finding (NOT this fix):** the RandomX *hybrid
  partial-dataset consumption path* (JIT reading cached prefix items) produces
  WRONG end-to-end hashes on-device even for a single non-rotated job with
  `--dataset-mb>0` (items=65536 wrong vs same-nonce light reference; items=0
  correct). Independent of this fix and the reason README already flags the
  partial dataset "⚠️ not adopted for production". The rotation test asserts the
  *buffer rebuild* directly, not the engine's end-to-end hash, to keep the two
  issues separate.

## 2026-08-07 (pm) — Live `Speed:` now uses a rolling window (XMRig-parity display)
- **The cumulative-average `Speed:` was the wrong comparison metric vs XMRig.**
  `MiningEngine::hash_rate()` returns whole-run `total_hashes/elapsed`, so after
  the ~164 s dataset dead-start the live `Speed:` "ramped" for minutes even
  though instantaneous throughput was flat ~29 H/s — making the numbers
  apples-to-oranges against XMRig's rolling-window `Speed:`. Fixed: the live
  pool `Speed:` line now uses a **10 s rolling-window rate** computed from
  `engine.snapshot()` deltas (same mechanism as the `--pool-test` `INST agg=`
  dump). It holds 0 during the dead-start (honest: no stable rate yet), then
  jumps to the true rate (~29 H/s) within ~10 s of fill completion and stays
  flat — matching XMRig's near-instant settle. The computed window rate is
  HELD between window closes (no fallback to cumulative, or the ramp returns).
  Cumulative `hash_rate()` is still used for `/metrics` and the `--mine`
  benchmark (legitimately whole-run there). Verified on-device: `Speed:` holds
  0.00 at t=20–120 s, then 28.85→28.91 H/s flat at t=200–240 s. (`src/miner_app.cpp`)
- **Tier 2(a) (drop-NEON fill interpreter) REVERTED — negative A/B.** Delegated
  to an external agent; the scalar-GPR fill regressed the dataset fill from
  **164 s → 207 s** (~27% slower), not faster. The NEON lane-extract was NOT
  the bottleneck; the scalar path lost the genuinely-parallel vector
  ISUB/IXOR/IADD. `execute_superscalar_neon` in `dataset.cpp` restored. The
  remaining fill gap to XMRig's ~10 s is a separate, harder problem (across-item
  SIMD or a different fill algorithm) — not pursued this session.

## 2026-08-07 — Warmup "ramp" root-cause + Tier 1 `wait_for_fill` (SHIPPED)
- **The ~20-min H/s "ramp" is a measurement artifact, not JIT/VM warmup.** The
  printed rate is a cumulative average (`total_hashes / elapsed`); under the old
  behavior workers hashed on an *empty* partial dataset while the background fill
  slowly populated it, so the average climbed even though each post-fill hash was
  at full speed. Root cause of the (separate) slow fill: `execute_superscalar_neon`
  is scalar-lane-extract heavy (per-op `vgetq_lane → scalar mul → vcombine`);
  8.4M items × 8 rounds ≈ 67M program executions × ~2.5 µs ≈ **164 s** (XMRig ≈ 10 s).
  **Retraction:** an earlier draft claimed "AES is missing from the item
  computation — a spec divergence"; this is WRONG — `initDatasetItem` has no AES
  in the reference spec (AES is in cache/scratchpad fill), so the fork is
  spec-correct; the claim was a conflation with the earlier T-table fix.
- **Tier 1 (DONE):** workers now block in `worker_loop` on
  `PartialDataset::wait_for_fill()` until the background fill completes, then
  mine at full speed instantly — no empty-dataset hashing, no misleading ramp.
  Fixes vs an earlier abandoned attempt: `fill_complete_` is now set by the **fill
  worker itself** (reliable handshake); `start_fill` is given **all cores**
  (miners are idle during fill, so excluding them left only 1 core → ~22-min
  dead-start bug, caught on-device); `wait_for_fill()`'s `join()` is mutex-guarded
  against concurrent worker callers (UB). `--pool-test` now prints an
  **instantaneous** (per-5s-window `snapshot()` delta) rate instead of the
  cumulative average — post-fill jumps to **~30 H/s** at t≈185 s (fill done
  t≈164 s). `test_partial_dataset` + `test_mining` KATs PASS.
- Added `--pool-test` default test pool/wallet in `cli_parser.cpp` (TEST creds,
  not production) and `tools/time_partial_fill.cpp` diagnostic (CMake). See
  `docs/briefs/2026-08-07-warmup-ramp-tier1-and-tier2-scope.md`. (Hermes: Tier-1
  implementation + on-device verification)

## 2026-08-04
- **E24 — Superscalar C* immediate density was the A53 MAC-interlock gap; closed with +7.1% H/s (SHIPPED).** The RandomX superscalar body is ~35% integer multiplies (4-cycle result latency on the Cortex-A53's single MAC). armrx materialized C* immediates via a 2-instruction `LDR`-literal-pool form (`emitCpoolImmediate`, the W4 phase-2 path) that is *denser* than XMRig's 3-instruction `MOVZ`/`MOVN`+`MOVK`. That density put only one (load) instruction between program-adjacent multiplies, saturating `other_interlock_stall` (23.3M/hash, 2.12× XMRig's 10.96M). Reverting to the 3-instr form pads each C* immediate with one extra independent ALU op, breaking up the multiply chains. **Result (1w, light, core 3, clean `perf`): H/s 4.77 → 5.11 (+7.1%, beating XMRig 5.04); `other_interlock_stall` 23.3 → 6.18M/hash (below XMRig); IPC 0.548 → 0.662 (vs XMRig 0.654); instr/hash 89.5 → 113.8M.** This closes E19 (the interlock excess) — the earlier scheduler-distance (E20) and disable-scheduler (E22) hypotheses were red herrings, and the generator (E21/L1) was identical to XMRig's. The W4 literal-pool branch and its `cpoolBase_`/`cpoolLiteralPos_`/`cpoolSlot_` machinery in `generateSuperscalarHash` were removed as dead code. `test_jit_equivalence` 16/16 byte-identical (verified before and after the dead-code removal). Ship the cross-built binary (GCC 16.1.0, +7.9% over device GCC 15.2.0 per E18). See `docs/experiments/perf-tracking.md` §4b. (`src/jit_compiler_a64.cpp` `emitCpoolImmediate` + `generateSuperscalarHash` pool removal)

## 2026-08-06
- **Doc correction + clean 1w re-baseline at HEAD (`ed466a2`).** The Item 1 (hardware-AES) perf claim was **wrong**: the original A/B reported instr/hash 107.36M → 89.47M (−16.7%, "largest win in history, below XMRig") — the 89.47M was a **contaminated-divisor artifact** (non-500-window / ungated division, the project's own flagged pitfall). A reproducible gated `--perf-ready` 500-hash re-baseline (two runs identical to 0.00006%) gives **101.10M instr/hash, IPC 0.667, median 195.5 ms** at 1w (clock 763 MHz, valid). Real AES delta = **−5.8% (107.36M → 101.10M)**; armrx is **~7% HEAVIER than XMRig (94.5M)** at 1w — the "gap closed / below XMRig" wording was overstated. H/s parity still holds (1w 5.11 vs XMRig 5.04, E24 real-pool) because armrx's better IPC compensates the heavier instruction count. `bench_armrx --full-hash-only` is single-threaded (ignores `--workers`), so 8w comes from the real-pool long-run (26.65 vs 28 = 95.2%). `isolcpus` is an operational deployment knob, NOT an armrx feature, and excluded from the baseline methodology. Corrected: `changelogs.md` Item 1 entry, `ROADMAP.md`, `docs/STRATEGY.md`, `docs/plans/era2-plan.md`; added `docs/measurements/2026-08-06-head-rebaseline.md`. (Hermes: build + 2× device gated re-baseline + doc sync; no source changes)

## 2026-08-06 (pm) — fix two known non-perf bugs
- **SIGINT/Ctrl-C now stops the miner cleanly (FIXED).** Two root causes: (1) `install_signal_handlers()` used `std::signal()` (no `SA_RESTART` control); (2) both run loops used a single `std::this_thread::sleep_for(1s)`, which **swallows EINTR and re-sleeps** — so the `keep_running` flag set by the handler was not re-checked until the full second elapsed (libstdc++'s `sleep_for` re-loops on EINTR regardless of `SA_RESTART`). Fixed: `sigaction` with `sa_flags=0` (SA_RESTART explicitly cleared) + the 1s sleep split into ten 100ms slices that re-check `keep_running`. Worker threads already poll `running_` once per ~200ms hash, so `engine.stop()` joins promptly. Verified on host: `armrx --mine --mode=light` exits gracefully (~2s, normal teardown, no `kill -9`) on SIGINT. (`src/miner_app.cpp`)
- **MetricsExporter socket data race (FIXED).** Removed the shared `std::atomic<int> server_fd_` that the dtor read (with no fence) while the worker thread wrote it. The listening socket is now created, used, and `close()`d entirely inside the worker thread; the destructor only flips `running_` and joins — no cross-thread fd access. Eliminates the TSAN-flagged read/write race. (`include/armrx/metrics.hpp`)
- Updated `docs/STRATEGY.md` "Known bugs" list to mark both FIXED with root cause + verification. (Hermes: source fix + host SIGINT smoke test + native + cross build green; `test_mining` KAT 16/16 passes)

## 2026-08-06 (late) — plan DAG list-scheduler attempt (NO code yet; external agent)
- Wrote `docs/briefs/2026-08-06-dag-scheduler-attempt.md`: a **different-shape** attempt at beating XMRig's residual instruction-mix edge. Post-double-trap rule (W3-2 memory-op scheduler + E26 load-hoist segfault) requires a NEW approach shape + a DIFFERENT external planner — Hermes stays the skeptical gate; the planner is executed externally.

## 2026-08-06 (latest) — DAG list-scheduler attempt: CLOSED (negative, not adopted)
- External planner (luna) implemented `scheduleProgramDag()` behind `ARMRX_DAG_SCHED=1` (default OFF, legacy scheduler ships). **Correctness: all 4 gates PASS on-device silicon** — `test_jit_equivalence` 16/16, `test_jit_determinism`, `test_jit_dataset_2way` (40020/40020), `test_jit_scheduler_stress` 450/450 + `test_jit_superscalar_scheduler_stress` 200/200 pairs byte-identical, `test_mining` real shares. The different shape (multiply-anchored DAG, `*_M` never an anchor, `hasHazard()` reused verbatim) **successfully avoided the W3-2 trap**.
- **Performance: REGRESSION — not adopted.** B-M-B-M A/B, core 3, 500-hash gated window:
  | run | variant | cyc/hash | instr/hash | IPC | H/s |
  |---|---|---:|---:|---:|---:|
  | B1 | default | 171.63M | 113.77M | 0.663 | 5.12 |
  | M1 | DAG | 189.24M | 127.33M | 0.673 | 4.39 |
  | M2 | DAG | 189.34M | 127.33M | 0.673 | 4.38 |
  DAG = **+10.3% cycles, +11.9% instr, −14.3% H/s** (reproducible M1≈M2). **Root cause (corrected 2026-08-07 audit):** NOT "longer emitted sequence" — per-opcode emitted length is order-invariant (IMUL_RCP/CBRANCH are `is_fixed` anchors, DAG returns identity when not ready). The regression is **per-hash planner runtime**: an O(n²) hazard matrix (32,576 `hasHazard` evals/compile) + ~9 heap allocs/hash, executed once per hash. Device logs prove it: **sys time tripled** (2.13s→7.5s, malloc/mmap churn) while instr/hash rose +13.56M (113.77M→127.33M, reproducible). The "+13.6M extra instructions" count is the *planner executing*, not extra emitted code. H/s −14.3% = compile cost growing from ~1.76% to ~16% of hash time.
- **Verdict:** JIT scheduler reordering is now **exhaustively closed** as a lever on this silicon (prior closed: `*_M` memory-op scheduler / W3-2, peephole / E20, PRFM / T2-1, dual-issue / T2-2). The residual ~4.6% gap is instruction *mix* / *count*, not ordering — and reordering only made count worse. **Code kept gated** (`ARMRX_DAG_SCHED=1`) as a documented negative result; not enabled, not deleted. Remaining real lever = worker-cluster placement (fast cluster 0-3), NOT JIT emission. (`src/jit_compiler_a64.cpp` `scheduleProgramDag`, `include/armrx/jit_compiler_a64.hpp`; brief `docs/briefs/2026-08-06-dag-scheduler-attempt.md`)

## 2026-08-06 (final) — two TUI bugs reported (OPEN, not fixed)
- **TUI segfaults with `ARMRX_DAG_SCHED=1` (OPEN).** `armrx --tui` under the DAG scheduler runs ~10-20s with valid per-worker H/s, then `Segmentation fault`. Hashing is correct (matches the 16/16 + 450/200 gates); the crash is in the TUI render/shutdown path under DAG emission order. Non-TUI pool mining is fine under DAG. Only bites if DAG is enabled + `--tui`. Recorded in `docs/STRATEGY.md` Known bugs (OPEN).
- **TUI garbage artifacts (OPEN, scheduler-independent).** `--tui` WITHOUT DAG dumps the binary name `armrx` + raw control bytes inline, with overlapping/duplicate lines — display unusable. A TUI rendering bug (escape-sequence / multi-thread fd-write without serialization), not scheduler-related. Non-TUI runs unaffected. Recorded in `docs/STRATEGY.md` Known bugs (OPEN). Reported by user on `lenovo` terminal after the DAG A/B.

## 2026-08-07 — independent audits (2 agents, read-only) + doc corrections
- Two unattended audits ran (`docs/audits/independent-audit_2026-08-07.md`, `docs/audits/opencode-unattended-audit-2026-08-04.md`). Both converged; Hermes verified the load-bearing claims against code. **Three of my prior doc entries were WRONG and are corrected:**
  1. **DAG segfault attribution** (STRATEGY bug a) — not "TUI render/shutdown under DAG order"; it's the **dangling `std::string_view pool_name`** (`tui.hpp:21` ← `pool_manager.cpp:34-38` ← `miner_app.cpp:443`), a UAF active in BOTH modes; DAG only changes heap-reuse timing to expose it.
  2. **SIGINT-on-pool attribution** (STRATEGY bug c) — the run-loop already polls (10×100ms); the gap is **teardown deadlock**: worker blocked in unbounded `send()` (no `SO_SNDTIMEO`, `stratum_client.cpp:337-357`) under `stratum_mutex_`, `disconnect()` waits on it → `engine.stop()` can't join.
  3. **DAG regression mechanism** (changelogs) — NOT "longer emitted sequence"; it's **per-hash planner runtime** (O(n²) hazard matrix + ~9 heap allocs/hash); device logs show sys time tripled (2.13→7.5s). The "+13.6M instr" is the planner executing, not extra emitted code.
- Audits also **found new real bugs** (now in STRATEGY.md Known bugs OPEN): unsynchronized `std::cout` (3 threads; `set_tui_mode` never called → dead ring-buffer) causing TUI interleave; `MetricsExporter` blocks behind idle HTTP client; IPv6 bare-address parse wrong; `PartialDataset` latent out-of-order publish (LATENT — dataset complete before mining, not an active mis-hash); `--pool-test --tui` skips cursor restore.
- **Lever corrections:** `-mtune=cortex-a53` is ALREADY set (`toolchain:66-67`) + measured null (E18) — removed from open levers. Cluster-placement restated: default affinity is `AffinityMode::All` (not Unpinned); pinning 8w to 0-3 is a REGRESSION (discards weak cluster's ~16.45→25.28 H/s); the real play is main-thread/worker-0 core-0 contention (~+2-4%). NEW lever: **cross-toolchain LTO** (GCC-16 LTO never A/B'd; prior on-device +1.9%; worth a session).
- Audits did NOT run anything (no backtrace) — the DAG-segfault↔UAF linkage remains a hypothesis; get a `gdb` backtrace to confirm.

- **`--pool-test` self-terminating pool measurement mode (E16 enablement).** Added a `--pool-test` flag (cli_parser.hpp/.cpp + help) that makes `run_pool_mining` honor `--seconds` (self-terminates via `std::_Exit(0)` right after printing a per-worker summary — skips the pool/engine teardown that otherwise hangs waiting on the network/worker threads) and prints `worker[0..N]` H/s + CPU max temp at the end, matching the `--mine` benchmark shape. Pool correctness is unchanged (still connects/submits shares); only run length + reporting differ. No credentials in source — these are supplied via `--pool/--wallet/--password` CLI arguments. Purpose: reproducible A/B sweeps on the REAL pool workload without needing `sudo kill -9` (no sudo access is available on the device). Cross-build clean; `test_cli_parser` passes. Discovered via this feature: the device **thermally throttles memory throughput at 60°C** (~4.7× crash 1w: 3.19 H/s @46°C → 0.68 H/s @60°C for identical work), making temperature an uncontrolled confound in all prior E15/E16 absolute numbers — re-measurement must control temp. (`src/miner_app.cpp`, `include/armrx/miner_app.hpp`, `src/cli_parser.cpp`, `include/armrx/cli_parser.hpp`)

- **Hardware AESE/AESD AES adopted as the default aarch64+crypto funnel (Item 1 of the residual-gap roadmap — THE lever).** Overrode `encrypt_transform`/`decrypt_transform` in `include/armrx/aes.hpp` with the **zero-key hardware form** (`vaesmcq_u8(vaeseq_u8(s, zero))` / `vaesimcq_u8(vaesdq_u8(s, zero))` + trailing real-key XOR already present in the callers), gated on `__ARM_FEATURE_AES` (defined under the existing `-march=armv8-a+crypto`). The scalar T-table bodies are renamed `*_ttable` and kept as the `#else` fallback + KAT oracle; `aes_encrypt_round_ttable`/`aes_decrypt_round_ttable` added for the oracle. **Why it was previously "incompatible":** the 2026-07-20 attempt fed `AESE` the *real* round key, and AESE applies AddRoundKey **first** (RandomX applies it **last**) → wrong round. The zero-key compensation (AddRoundKey becomes a no-op, AESMC does MixColumns, trailing EOR applies the real key) is byte-identical to the T-table path and matches armrx's own JIT v2 FE_mix (`jit_compiler_a64_static.S:425-489`) + upstream `intrin_portable.h:476-484` (which ran `--verify` on this silicon in W1-4). **Single funnel:** every AES path (the 5 x4 hash/fill fns in `src/aes_hash.cpp` call `encrypt_transform`/`decrypt_transform` directly under `ARMRX_ENABLE_NEON_TTABLE_AES`; `aes_generator.cpp` routes through `aes_encrypt_round`) rides the override — ~25 lines in the header, ZERO edits to `aes_hash.cpp`/`aes_generator.cpp`/`CMakeLists.txt`. **Verification (device, gated W11/T1-2, B-M-B-M, core 3, 500 hashes, non-isolated):** hw == T-table over 60,000 random blocks (`tools/aes_kat_check.cpp` extended as the oracle); `test_aes_hash` golden pins; `test_mining` real shares; `test_jit_equivalence` 16/16; determinism; dataset_2way; encodings — all PASS. **Perf (CORRECTED 2026-08-06 re-baseline):** the original A/B reported instr/hash **107.36M → 89.47M (−16.7%)** — that 89.47M figure was a **contaminated-divisor artifact** (non-500-window / ungated division, the project's own flagged pitfall). The reproducible HEAD re-baseline (two runs, identical to 0.00006%) gives **101.10M instr/hash** at HEAD, i.e. AES saves **107.36M → 101.10M = −5.8%** (modest, real, not the 16.7% first claimed). armrx at 101.10M is **~7% HEAVIER than XMRig (94.5M)** at 1w — the earlier "armrx now below XMRig / largest win in project history" wording was wrong; the **correctness** evidence (hw==T-table, golden pins, real shares) is solid and unchanged. H/s parity still holds (1w 5.11 vs XMRig 5.04, E24 real-pool) because armrx's better IPC (0.667 vs ~0.654) compensates the heavier instruction count. See `docs/measurements/2026-08-06-head-rebaseline.md`. Host x86_64 ctest 9/9 unchanged (hw branch compiled out). Doc fixes: `aes.hpp` Track-G comment, `AGENTS.md` ("NEON AES disabled" → adopted), `docs/postmortems/aes-ttable-bug-postmortem.md` (records the corrected re-adoption). See `docs/briefs/2026-08-03-hardware-aes-item1.md`, `docs/audits/residual-gap-optimization-roadmap.md`, `docs/measurements/2026-08-03-post-w4-baseline.md`. (Hermes: brief + source edit + KAT oracle + cross-build + device gates + B-M-B-M perf A/B + doc fixes — by directive, easy verified change done directly rather than via Reasonix)

## 2026-08-02
- **W4 phase-2 — dedicated superscalar C* literal pool: SOLVED (correctness) + no regression.** The dense per-program PC-relative literal pool for `IADD_C*`/`IXOR_C*` (phase-1's shared-region collision is avoided by giving the superscalar path its OWN 512 B–1 KiB inline pool) is now correct and passes the full device gate set: `test_jit_equivalence` 16/16 byte-identical, `test_jit_dataset_2way`, `test_jit_determinism`, `test_jit_scheduler_stress` (450 pairs), `test_jit_superscalar_scheduler_stress` (200 pairs) — all `EXIT=0`, no FAIL. **Root cause of the prior 15+ failure iterations:** the offset formula carried a `- 8` (`off = (litpos - k - 8)/4`), which on A64 `LDR (literal)` (target `= k + off*4`, NO `+8` — that's an A32/T32 quirk) made every pooled C* op load from `litpos - 8` (the previous slot / pre-pool garbage) → silent hash mismatch. The `k + 8` convention had been "proven" by a **circular runtime self-check** that recomputed the target with the same wrong formula, so `match=1` was guaranteed regardless. A QEMU + on-device micro-test settled the PC semantics empirically (`target = k + off*4`), and the fix (drop `-8`, mirror the proven IMUL_RCP loader `off = (literal_pos - codePos)/4`) made all 703 pooled C* ops land on valid sign-extended constants. Luna's earlier sign-extension fix (RandomX C* constants are SIGNED → pool entry must be sign-extended 32→64) was also required; both bugs had to be fixed together. **Perf:** instruction count drops (C* sites 2–3 instr → 1 `LDR`), but hashrate is latency-neutral on the in-order A53 — pooled `--mine` = 4.32 H/s vs documented baseline 4.27 H/s (non-isolated, 1 worker), i.e. **no regression** and consistent with W3's finding that fewer instructions ≠ faster here. This is NOT the earlier W3 NEON-vector-pool attempt (which scattered ~714 loads and thrashed cache, −16% to −20% H/s) — the PC-relative inline pool sits adjacent to code and stays cache-clean. The `aarch64-ldr-literal-pool` skill was corrected (the `k+8` formula + circular self-check are now flagged as the trap that cost the iterations). See `docs/briefs/w4-phase2-investigation-notes.md` and `session-ses_0416.md`. (Hermes: gate takeover + docs; Kimi K3: root-cause solve via hardware micro-test + byte-stream diff; Luna: sign-extend fix; Reasonix: unavailable)

## 2026-08-01
- **W4 — dense NEON literal-pool for C* immediates: phase-1 FAILED correctness, REVERTED.** Learned the technique from the BSD upstream reference (`scratch_vm_study/upstream_rx`): its `generateSuperscalarHash` resets `num32bitLiterals = 0` so `IADD_C*`/`IXOR_C*` pool into a dense 64-slot NEON literal region via fixed-index `UMOV`/`SMOV` (order-independent), achieving **104.8M instr/hash @ IPC 1.54** vs armrx's 132.7M @ 1.369. armrx's `emitMovImmediate` (jit_compiler_a64.cpp:1240) already has this exact path but is pinned to `num32bitLiterals = 64` in `generateSuperscalarHash` (line 1105) to forbid C* pooling. Un-pinned to 0 (Reasonix) → `test_jit_equivalence` **FAIL** seed_0 (JIT≠interpreter). **Root cause:** `ImulRcpLiteralsEnd` is a SINGLE region shared by main-VM `generateProgram` AND superscalar — un-pinning let superscalar C* ops overwrite the main VM's literals → deterministic mismatch. The reference separates the regions; armrx does not. Reverted; equivalence re-confirmed 16/16. **Technique is valid (reference proves it on this silicon) but needs a DEDICATED superscalar literal region + NEON reg allocation + static.S reservation — phase-2-class, deferred.** Evidence: dense-pool works, but armrx's shared-buffer layout makes a naive port unsafe. See `docs/briefs/brief-w4-cpool.md`. (Hermes: brief + device A/B gates + revert; Reasonix: one-line edit)
- **W3 register-hoist: considered and REJECTED (infeasible) — perf-optimization pursuit CLOSED**
- **W3-2 — memory-op scheduler extension: CONFIRMED UNSAFE, closed (bisection executed):** Re-applied the `*_M` `is_long_latency` change to `computeFootprint()` (the exact change that previously hung/diverged) and ran the scheduler stress suite on device. `test_scheduler_bisect` `--budget=N` sweep (unset + 0..8 + 16/32/64, all PASS) showed the **main-VM** path is correct under the change — but the proper stress gates caught the divergence the 16-pair equivalence check missed: `test_jit_scheduler_stress` (450 pairs) **FAIL** `seed_0` and `test_jit_superscalar_scheduler_stress` (200 pairs) **FAIL** `seed_4`. The memory-op scheduler extension reproduces a **deterministic JIT/interpreter divergence** under stress coverage → W3-2 closed as a dead end with evidence. The `ARMRX_MAX_SWAPS` hook only gates `scheduleProgram` (main-VM), not `scheduleSuperscalarProgram`; the superscalar path was the one that diverged first. The speculative `*_M` change was reverted; the bisection instrument (hook + `--budget` test mode) stays as proven-useful diagnostic infrastructure. Do not re-enable memory-op scheduling. See `docs/experiments/w3-2-swap-budget-bisect.md`. (Hermes: design + code + cross-build + device A/B + stress sweep + revert + docs; Reasonix briefly usable then hit limits)
- **W3-2 — scheduler swap-budget bisection harness (diagnostic instrumentation, done):** Added an `ARMRX_MAX_SWAPS` env-gated swap budget to the emitter scheduler (`scheduleProgram` in `src/jit_compiler_a64.cpp`) to enable binary-search isolation of the memory-op scheduler divergence that previously caused a JIT/interpreter hash mismatch and was reverted unidentified (`docs/experiments/memory-op-scheduler-attempt.md`). The hook wraps the two existing swap-commit sites: budget `N>0` permits exactly N swaps then falls back to original order; `0` = no swaps; unset/`-1` = unchanged (full scheduler). Re-reads the env every call so a driver can sweep budgets in one process. Public `computeMainEmitOrder` accessor added (mirrors `computeSuperscalarEmitOrder`). New `tests/test_scheduler_bisect.cpp` asserts JIT==interpreter across budgets {unset,0,1,100000}; registered in the `if(ARMRX_HAVE_JIT)` block with a 600s timeout. **Verification (device, AArch64 cross-build): all 4 settings pass — the hook never changes which program executes, only emission order.** This is the instrument that unblocks W3-2 by enabling binary-search isolation of the memory-op-divergence bisection. Written directly by Hermes because all three coders were down (Cursor usage limit; Reasonix DeepSeek 402; AGY broken model pin) and the no-source-edit rule was lifted for this diagnostic. See `docs/experiments/w3-2-swap-budget-bisect.md`. (Hermes: design + code + cross-build + device A/B + docs)
- **W3 — superscalar immediate-materialization density (CLOSED: measured regression, reverted):** The only evidence-backed W2-3 lever — replace `IADD_C7/8/9`/`IXOR_C7/8/9` MOVZ/MOVN+MOVK+ALU (~3 A64) with a single `LDR` from a per-program per-original-index literal pool (~714 ops/hash). Implemented scheduler-safely (pool addressed by original index `j`, order-independent — strictly safer than IMUL_RCP's sequential-pointer pool; the `num32bitLiterals=64` pin was removable). Correctness gate **passed** on device: `test_jit_equivalence` 16/16 byte-identical, `test_jit_dataset_2way` 40,020 derivations/20 seeds bit-identical, `test_jit_determinism` deterministic. **But perf A/B (1 worker, core 3, `perf stat`, two trials each) showed −16% to −20% H/s** despite −20% instructions, because cache-misses **tripled** (109M→339M) and IPC collapsed (0.71→0.56): ~714 scattered literal loads/program (3× IMUL_RCP's density) thrash the cache and compete with the I-cache on this in-order-ish A53. **Fourth independent confirmation that fewer instructions ≠ faster here** (after T2-2, T2-1, fill-loop-hints removal). Reverted bit-clean (`git checkout`); tree restored to `c5ac985`. The W2-3 instruction census is correct — the lever is real but not realizable as a throughput win via literal-pool loads on this hardware. Do not re-attempt the literal-pool approach. See `docs/experiments/w3-immediate-density.md`. (Cursor: source edits; Hermes: brief + cross-build + md5-verify + device A/B + revert + docs)
- **W2-2 / N1 — `ldp`/`stp` scratchpad fusion (closed, not actionable):** Static source analysis (Cursor) of `emitMemLoad`/`emitMemLoadFP`/`h_ISTORE` in `src/jit_compiler_a64.cpp` and the thunks in `jit_compiler_a64_static.S`. Verified against source by Hermes: the superscalar opcode body is **pure register-file ALU** (ADD/MUL/UMULH/SMULH/EOR/ROR on x0–x7, 1 instr each — `generateSuperscalarHash` switch ~1120-1198) with **0% scratchpad LDR/STR**; the only memory traffic is inside the dataset thunks, which already use `ldp`/`stp`. In the main-VM path, `*_M`/`ISTORE` are always separated by address-setup ALU and emit **register-indexed** `[x2, Xm]` (not `#imm`), so classical `ldp`/`stp` fusion is structurally impossible. **N1 closed** — no LDP/STP pass. See `docs/experiments/w22-n1-adjacency-analysis.md`. (Cursor analysis, Hermes verified)
- **W2-3 — superscalar gap root-cause (static):** Two static analyses (Cursor) locate the ~24.5M instr/hash excess vs XMRig. (1) Dataset thunks = 161 instr/call × 16,384 = **2.64M/hash = 2.76%** of superscalar — ruled out as the gap home (`docs/experiments/w23-thunk-count.md`). (2) Opcode body static cost = **5,229 A64/call ≈ measured 5,224** (validated) — dominant non-1-instr cost is `IADD_C7/8/9`/`IXOR_C7/8/9` immediate materialization (~3 A64 each via MOVZ/MOVN+MOVK+ALU), ~86% of the >1-instr extras (`docs/experiments/w23-opcode-cost.md`). Top proposal: pool-load immediate materialization → ~10% of total instr/hash. **Caveat (high risk):** the `num32bitLiterals=64` pin exists specifically to keep C7-C9 safe under the emitter scheduler (order-sensitive literal pool); any immediate-materialization change must preserve scheduler safety. (Cursor analysis, Hermes verified)
- **W2-1 — BOLT (deferred):** `llvm-bolt` not installed on host; expected null because BOLT cannot see runtime-generated JIT code (only the ~9.5% C++ region). Recorded as toolchain-dependent deferral, not a blocker.
- **Census residual resolved (source analysis) — call count is exactly 16,384/hash:** The W1-1 census left an "8% call-count residual" (measured 95.77M superscalar instr/hash vs static 5,401 × 16,384 = 88.49M → implied 17,727 calls). Source tracing closes it: the light-mode dataset read is a hard loop — 8 `run()` calls/hash (vm.cpp:982-990) × 2,048 loop iterations/call (x3 = hardcoded `2048ULL` at vm.cpp:843; `subs x3, x3, 1; bne .Lmain_loop` at jit_compiler_a64_static.S:510-511) × one `bl rx_calc_dataset_item` per iteration (.S:577) = **16,384 calls/hash, exact by construction** (2,048 = 256 × 8 = RANDOMX_PROGRAM_MAX_SIZE × kRandomXCacheAccesses). The "17,727" was an arithmetic artifact of an under-measured static divisor: the true per-call dynamic cost is ~5,845 A64 (+8.2% over the 5,401 counted from the 23,588-B live read — literal-pool identification slack). Budget closes exactly: 16,384 × (719.6 main-VM + 5,845.3 item) = 107.56M = measured 95.77M + 11.79M; + AES ~10.7M + glue ~0.6M ≈ 118.96M. The documented "16,384×/hash" multiplier was right all along. See `docs/experiments/w11-instruction-census.md` (Hypothesis C resolution). (Hermes: source analysis, no code changes — measurement-only verification, docs)
- **W1-1 — region instruction census (done, reconciles the 119.0M vs 58.4M contradiction):** Device census of the clean 118.96M instr/hash total (`perf record` passes + live JIT-buffer read, `tools/w11_census_device.sh` + `tools/w11_census.py`): **superscalar region 80.5% (95.77M, IPC 0.800) / main-VM JIT 9.9% (11.79M, IPC 0.405 — the known memory-stall penalty) / named C++ 9.5% (11.33M, AES ≈10.7M) / unattributed 0.05%**. Reconciliation: Hypothesis A (sample bias) **dead** — the census reproduces the old 96.7M superscalar attribution on the new total; Hypothesis B **primary** — the live superscalar body is **5,224 A64 instr/call (1.47× the documented 3,563)**, 8 programs × ~490 RL-instr at the 512 cap (`kSuperscalarMaxSize`), so the 58.4M static figure (stale `--jit-dump`) is superseded; Hypothesis C **mostly dead, 8% residual** (implied 17,727 vs 16,384 calls/hash — undistinguished). A kernel **I-cache coherence anomaly** (6.12.1-msm8916: bytes written to executable memory execute as different instructions; reproduced standalone) blocked direct call-count instrumentation — documented as a device measurement hazard. Gates stay closed: W3-1 wrapper slice 2.4% (save/restore target ~0.4%), W3-3 gap is 96.7% opcode body. D11 doc-path fix included (CMakeLists NEON_AES help text → `docs/experiments/`). See `docs/experiments/w11-instruction-census.md`. (implemented/run by Reasonix from a Hermes brief; verified by Hermes: arithmetic, source-constant cross-check, W1-5 consistency)
- **W1-5 — Track G full-workload E2E A/B (done, confirmed win):** First full-workload Track G measurement — ON-OFF-ON-OFF `--perf-ready` A/B on device (md5-verified per run; ON `257e6d14…` default build vs OFF `5e76629c…` from a `-DARMRX_ENABLE_NEON_TTABLE_AES=OFF` cross build). Result: **ON +1.68% H/s, −2.07% cycles, −4.93% instructions** vs OFF (ON 118.96M instr/hash vs OFF 124.8M). Below the ~3.6% projection — back-solved AES cycle share ~7.2% (D2 interleave halved it since the 12.3% microbench measurement) — but the default-ON decision is validated. The −4.9% instruction delta confirms Track G as a real contributor to the 132.93M → 119.0M drop (feed into W1-1 census, in flight). See `docs/experiments/track-g-e2e-ab.md`. (Hermes: brief + A/B + docs; OFF build by Hermes)
- **W1-4 — XMRig same-device re-baseline (done, reframes the +19.5% claim):** Measured XMRig 6.26.0 (Kanedias `xmrig-static` aarch64 static-pie, glibc, md5 `2ac4814a`, perf-stat totals only — NO JIT-buffer disassembly, clean-room compliant) on the target device: light mode, 1 thread, `taskset -c 3`, 765 MHz. **94.5 M instr/hash, IPC 0.648, 5.05 H/s** (250K benchmark completion + two 120 s/100 s perf windows, IPC consistent 0.6475). Reconciliation vs armnx (119.0M instr/hash, IPC 0.731, 4.84 H/s): the audit's **+19.5% gap hypothesis is REJECTED** — the true instruction gap is **+25.9%** (armnx emits 25.9% more instructions/hash), but armnx's superior IPC (0.731 vs 0.648) shrinks the cycles/hash gap to **+11.5%** and the raw H/s gap to only **−4.2%** (armnx 4.84 vs XMRig 5.05). Implication: remaining headroom vs XMRig is ~26% excess instructions/hash (dominant lever, ~80% in the superscalar body); there is NO separate stall/scheduling deficit (armnx IPC already better). The historical 99.57M XMRig figure was 5.1% above the device re-baseline (likely fast-mode/different-clock). See `docs/experiments/w14-xmrig-rebaseline.md`. (Hermes: binary sourcing, schema debugging, device measurement, docs)
- **W0-1/W0-2 — doc sync (external-audit D1–D14 corrections applied):** `RETROSPECTIVE.md` (D2 on-device A/B done, Track G default ON, wins table, Discovery #1 correction note: clean total 119.0M not 132.93M), `master-plan-20260727.md` (Track C Phase A reverted/hang, Track G default ON + W1-5 pointer, Track A tooling-gap closed by T1-2), `AGENTS.md` (experimental-flags blurb: NEON_TTABLE_AES is default ON), `README.md` (D2 status 🧪→✅ with device A/B numbers), `docs/archived/NEXT_STEPS.md` (Track C "code stays" correction), `combined-audit-20260731.md` (⚠️ SUPERSEDED banner pointing to the new live backlog; T3-1 ROI reframed; N3 Blake2b NEON already implemented; N7 cpufreq refined to no-OPP-table). All historical narrative preserved; corrections added as dated notes per project discipline. (`RETROSPECTIVE.md`, `docs/plans/20260727/master-plan-20260727.md`, `AGENTS.md`, `README.md`, `docs/archived/NEXT_STEPS.md`, `docs/audits/combined-audit-20260731.md`)
- **T3-3 gate check — implemented, run, FAIL (closed):** The real IMUL_R operand-magnitude gate. Added an interpreter sampling hook (`setImulSampleCallback` in `include/armrx/vm.hpp`, fired from `src/vm.cpp` IMUL_R case with `ibc.isrc == &ibc.imm` pointer-identity segregating RCP-lowered instructions) and `tests/bench_imul_magnitudes.cpp` (self-checking sampler: hook on/off outputs byte-identical; tiered one/both fractions; per-seed variance; GATE verdict). Registered in CMakeLists (ctest `8 20`, host-runnable, no JIT dependency). Result: **0.003% of 74,079,016 genuine IMUL_R executions have one operand ≤ 2^32** (gate ≥30%; both-operands 0.0003%; per-seed max 0.03%; cross-confirmed at 367.8M samples by Reasonix). **Gate FAIL — the NEON lane-parallel IMUL_R premise is falsified; T3-3 and the F3 follow-on close on evidence.** The tool remains as a reusable, self-checking operand-distribution sampler. See `docs/experiments/t33-imul-magnitude-gate.md`. (`include/armrx/vm.hpp`, `src/vm.cpp`, `tests/bench_imul_magnitudes.cpp`, `CMakeLists.txt` — implemented by Reasonix from a Hermes brief; verified by Hermes)
- **Audit doc correction — T3-3 gate status:** The "✅ gate check done" mark on T3-3 (NEON mul analysis) was erroneous — it entered `combined-audit-20260731.md` in f5c60dd (a T2-3 commit) with no supporting evidence: `bench_opcodes.cpp` was never extended with operand-magnitude reporting, and no gate writeup exists. Status corrected to "gate NOT done"; spec caveat recorded (IMUL_R operands are runtime values — requires instrumented execution sampling, not static program generation). (`docs/audits/combined-audit-20260731.md`)
- **T2-2 — dual-issue alignment (closed, measured regression):** NOP-padded long-latency op emissions (IMUL-family + scratchpad LDRs) to aligned 8-byte group starts in the main-VM JIT (`emitPadTo8`, 8 sites in `src/jit_compiler_a64.cpp`). Device A/B (B-P-B-P, per-run md5-verified): cycles +0.125%/+0.197% (worse), instructions +0.472%/+0.467% (NOP overhead), but **IPC +0.36%/+0.27% — the alignment mechanism genuinely improved dual-issue**; the NOP cost simply exceeds the recovery. Third independent confirmation of "added instructions cost real cycles on in-order A53" (after T2-1 and the 2026-07-24 fill-loop hints) — consistent with the audit's 94%-architectural-stall ceiling. Reverted bit-clean (`git checkout`; cross rebuild reproduced baseline md5 `257e6d14` exactly), device binary restored, tree clean. See `docs/experiments/t22-dual-issue-alignment.md`. (`src/jit_compiler_a64.cpp` — implemented + reverted by Reasonix from a Hermes brief; A/B + docs by Hermes)
- **T2-1 — PRFM hints (closed, measured regression):** Fresh on-device A/B of inline main-VM `PRFM PLDL1KEEP` hints (one per scratchpad LDR, in `emitMemLoad`/`emitMemLoadFP`) — the measurement the 2026-07-24 removal comment mandated. Result: **regression** — +0.26–0.50% cycles, +0.470% instructions (exact, both trials), l1d_cache_refill unchanged (no prefetch effect). In-order A53 issues the hint in the same window as the dependent LDR, so no stall is hidden; pure issue-slot overhead. Corroborates the 2026-07-24 fill-loop hints removal (+0.885% IPC from removing them). All JIT tests passed byte-identical with hints present (PRFM is architecturally inert). Change reverted; tree clean. See `docs/experiments/t21-prfm-hints.md`. (`src/jit_compiler_a64.cpp` — implemented + reverted by Reasonix from a Hermes brief)
- **Track G — NEON T-table AES enabled by default:** Flipped `ARMRX_ENABLE_NEON_TTABLE_AES` from OFF to ON in CMakeLists.txt. Code already implemented and KAT-verified (10,000-trial parity test, +28.8% AES throughput microbenchmark). Projected ~3.6% hashrate gain. See `docs/audits/combined-audit-20260731.md` (T0-1). (`CMakeLists.txt`)
- **isolcpus-aware worker pinning (T2-3):** Worker threads now pin exclusively to isolated cores when `isolcpus=` is active. `detect_core_order()` applies an isolation filter (`filter_to_isolated`) on ALL return paths — including the cpufreq-less fallback that previously returned the raw sequential order (verified bug on MSM8929: worker 0 stole housekeeping core 0, core 7 left idle). Default worker count also capped to isolated-core count (`cli_parser.cpp`). On-device verification: `isolcpus detected — workers pinned to isolated cores: 1, 2, 3, 4, 5, 6, 7`, main thread on core 0, all 7 workers on cores 1-7. See `docs/audits/combined-audit-20260731.md` (T2-3). (`src/mining_engine.cpp`, `src/cli_parser.cpp`, `src/cpu_features.cpp`, `include/armrx/cpu_features.hpp`)
- **T1-2 — `--perf-ready` hook (done, device-verified 2026-08-01):** `bench_armrx --full-hash-only --perf-ready` prints `PERF_READY` after warmup/cache-init and gates the measured loop on a stdin line, so `perf stat -p <pid>` captures ONLY steady state (closes the master-plan 2.77× instructions/hash measurement pitfall). Plus `tools/perf_ready_bench.sh` wrapper (fifo + perf attach; fifo deadlock fixed via `<>` open). Host-verified (EOF path, 2s-timeout path, error path, 8/8 ctest). Device-verified after the GCC-16 toolchain upgrade: first clean steady-state run — 500 hashes, 105.13s window, **IPC 0.740, 119.0M instr/hash, 3.11% branch miss, 4.83 H/s median (207 ms/hash light-mode single-core)**; core at fixed 765 MHz (device has NO cpufreq — no OPP table in DT, see `docs/experiments/t12-perf-ready-first-run.md`). (`tests/bench_armrx.cpp`, `tools/perf_ready_bench.sh`)
- **T1-1 — D2 microbenchmark + on-device A/B (done):** `bench_armrx --bench-d2-pipeline` tests `hash_and_fill_aes_interleaved_x4` in isolation vs the sequential pair (`--d2-only=sequential|interleaved` single-variant mode for perf stat; byte-identical equivalence gate; flag-gated, not in run_all). Host: equivalence PASS, 8/8 ctest. Device (A53, NEON T-table, Track G): **interleaved −2.77%** (13081→12718 μs median), perf stat A/B shows the mechanism — same instructions (+0.17%), IPC 1.735→1.787 (+3.0%), −29% branch instructions (fused loop), −2.74% cycles. Interesting negative: on x86_64 (software T-table, no NEON) interleaved is +32% SLOWER — the overlap only pays off on the NEON path. E2E projection ~0.34% (12.3% cycle share); the mining engine already runs the interleaved path, so the win is already live. Implemented by Reasonix from a Hermes brief; verified by Hermes. See `docs/experiments/t11-d2-microbenchmark.md`. (`tests/bench_armrx.cpp`)
- **KNOWN ISSUE — device JIT execution hangs (root cause FOUND and FIXED):** On-device bisect (4794890 → c7c1b5c → a683701) isolated the regression: **Track C Phase A** (`c7c1b5c`, light-mode dataset-item prologue). 4794890 passes test_jit_equivalence 16/16; c7c1b5c hangs (100% CPU spin in JIT code, zero hashes). The identical structural change hung on 2026-07-27 and was reverted with "root cause never conclusively identified" — re-applied anyway. The 2500-pair diagnostic (test_jit_dataset_light) passes, so the prologue's data flow is correct; the hang is in the full-VM call-site interaction. Track C measured ZERO impact → reverted in 27e7c41. Host 8/8 green; device verification pending. Note: the earlier test_mining hang on the D2-era device build predates this diagnosis — the pipelined-path bugs (fixed later, host-verified) may have been the cause there, to be re-confirmed on device now that the JIT hang is gone. (`src/jit_compiler_a64_static.S`, `src/jit_compiler_a64.cpp`, `src/jit_compiler_a64_static.hpp`, `tests/test_jit_dataset_light.cpp`, `CMakeLists.txt`)
- **Rate display fix:** Fixed bug where hashrate and total hashes displayed 0 during hashing on low H/s devices by adding a time-based (1s) flush condition alongside the count-based batching. (`src/mining_engine.cpp`)
- **Cross-compile toolchain upgraded to GCC 16.1.0 — JIT caveat LIFTED:** Replaced musl.cc GCC 11.2.1 with AUR `aarch64-linux-musl-cross` (GCC 16.1.0 + binutils 2.44 + musl 1.2.5, `paru -S`; drivers `/usr/bin/`, sysroot `/usr/aarch64-linux-musl/`). The GCC-11 seed-dependent JIT codegen hang is GONE: device-verified 2026-08-01 with cross binaries — test_jit_equivalence 16/16 byte-identical (the former hang), test_mining + test_aes_hash + test_jit_determinism + test_jit_encodings all pass, and the cross-built miner mines ~18 H/s with valid shares. **Two new quirks handled:** (1) AUR cross-ar/ranlib hang at 100% CPU archiving (State R/wchan 0, archive stuck at 8 bytes — verified); GNU archive format is arch-independent, so the toolchain file hard-pins host `/usr/bin/ar`/`ranlib`/`nm` via `CMAKE_<LANG>_ARCHIVE_*` rules — `CMAKE_AR` cache FORCE alone is insufficient because compiler detection shadows it with a directory-scoped value (cache said /usr/bin/ar, link.txt still had musl-ar). (2) The two scheduler stress tests are SLOW on device (light-mode dataset-on-demand, NOT a hang — identical to native builds): test_jit_scheduler_stress 450 pairs ~20 min, test_jit_superscalar_scheduler_stress 200 pairs ~35 min, both all byte-identical. (`cmake/toolchain-aarch64-musl.cmake`, `AGENTS.md`)
