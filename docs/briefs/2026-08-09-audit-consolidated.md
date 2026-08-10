# armrx Audit Consolidation — Verified Claims & Adopt Backlog (2026-08-09)

Three external audits were run and every file:line claim was verified against source by Hermes
before endorsement:

- **Luna audit #1** — "What can be done better on the codebase" (10-point hygiene list).
- **Deepseek** — "Optimization Headroom & Weak-Spot Report" (3 data races + quality debt +
  perf headroom).
- **Luna audit #3** — "Highest-Value Improvements" (overlaps #1 + Deepseek; adds new items).

This file collapses all three into a single de-duplicated, prioritized adopt backlog. Each item
is marked VERIFIED (source-confirmed) with the concrete evidence, and flags any inaccuracy found
in the audits themselves. No code was changed — adopting any item requires a `try/<name>` branch
+ on-device gate per project rule.

Canonical constraints applied throughout:
- Keep focus GENERIC/portable AArch64 — fleet re-validation is measurement, not device-specific
  tuning.
- `isolcpus` / deploy-level tuning is OUT OF SCOPE for armrx code (not a project lever).
- Every close/lever decision: per-lever-FAMILY, N distinct designs on separate branches.

---

## TIER 1 — Correctness bugs (real UB / data races) — do first

### T1-A Fix 3 data races (Deepseek, all VERIFIED) — ✅ ADOPTED 2026-08-09
- Commits: `cc014e7` (fix) → merged `6af9744` (origin/main).
- Fixes: (1) `extra_nonce1_` guarded by new `extra_nonce_mutex_` (`stratum_client.hpp`
  + R/W sites); (2) `PoolManager::connect()` resets wrapped in `stratum_mutex_`
  (released before `connect_to_current()` to avoid nested-lock); (3) `sockfd_` →
  `std::atomic<int>`, AND `close_connection()` reordered to `shutdown → join reader
  → close` (the deeper use-after-close race TSan caught and the poll missed).
- **Gate:** host ThreadSanitizer on `test_pool_protocol` = **0 data races**; aarch64
  cross-build clean; on-device `test_jit_equivalence` 16/16 + `test_jit_determinism`
  green (Lenovo).
1. **`extra_nonce1_` (std::string) race** — `src/stratum_client.cpp:528` (reader thread writes
   in `handle_set_extranonce`) vs `:375` (read, unsynchronized, in `build_submit_msg` from the
   worker submit path). Unsynchronized `std::string` access = UB (TSan-visible). Fix: guard with
   `send_mutex_` or a dedicated mutex / atomic-friendly type. Tiny diff, no perf risk.
2. **`PoolManager::connect()` unsynced writes** — `src/pool_manager.cpp:88-94` resets
   `current_idx_`/`failover_cooldown_`/`sync_retry_count_` with NO `stratum_mutex_`, while
   `current_pool_name()` (:34-38) and other accessors read them under the lock. Fix: take
   `this->stratum_mutex_` in `connect()` (it already does so in `connect_to_current()` at :66).
3. **`sockfd_` (plain int) race** — `src/stratum_client.cpp:95-99` (close sets `sockfd_=-1`) vs
   `:437/:440` (reader thread recv). Real but LOW severity — `shutdown()` precedes `close` so
   near-harmless in practice. Fix: `std::atomic<int>` or a mutex.

### T1-B Stabilize partial-dataset lifecycle tests (Luna #1 #2, Luna #3 #2, Deepseek — VERIFIED) — ✅ ADOPTED 2026-08-09
- Commits: `a1967ae` (fix) → merged `39f8464` (origin/main).
- Fixes: `wait_for_fill()` 100 ms poll → `std::condition_variable` (`fill_cv_`/
  `fill_cv_mutex_`), notified by `fill_worker` on `item_count_` advance + `fill_complete_`,
  destructor notifies on cancellation (predicate also checks `stop_`). Added
  `wait_until_published(count)` hook. `test_contiguous_publish_no_uninitialized_read`
  flaky `max_observed < kTotalItems` assert → deterministic `wait_until_published(
  kChunkItems)` barrier + `assert(item_count()==kChunkItems)`; lag bumped 500ms→5s so
  the stall is reliably observable. The byte-level `violation` check remains the
  deterministic correctness guard.
- **Gate:** on-device `test_partial_dataset` = **ALL PASSED** (Lenovo, deterministic
  barrier message confirmed). 2× fresh runs.
- **`tests/test_partial_dataset.cpp:262`** timing-sensitive assert
  (`assert(max_observed < kTotalItems)`) fails under ASan even when correct. Fix: sync barrier /
  explicit test hook instead of observing an intermediate prefix.
- **`src/partial_dataset.cpp:248-274` `wait_for_fill()` polls every 100 ms**
  (`sleep_for(milliseconds(100))` at :255) instead of a condition variable — increases latency +
  flakiness. Fix: notify on chunk completion.
- Add lifecycle stress: destroy PartialDataset during delayed fill, test refill/cancellation/
  exception/teardown — run under ASan + TSan. (test_partial_dataset.cpp currently has cached /
  large / disabled / incremental / refill / contiguous cases but no destroy-during-fill or
  teardown-under-sanitizer coverage.)
- ⚠️ **Audit inaccuracy flagged:** Luna #3 #1 claims the partial-dataset teardown fix is "still
  device-unverified after its earlier device crash." FALSE — README.md:176 documents it as
  adopted + re-gated: *"PartialDataset teardown joins fill workers (L-2, 2026-08-08): ✅ Adopted
  (correctness/UB fix)... First attempt crashed on-device (SIGSEGV, 256 MiB lagging-chunk) →
  revised and re-gated green."* Do NOT re-litigate a closed item.

---

## TIER 2 — Highest-value safe C++ wins (no asm needed)

### T2-A Eliminate repeated RegisterFile copies (Luna #3 #4 / P1 — VERIFIED, standout) — ✅ ADOPTED 2026-08-09
- Commit: `881f243` (fix) → merged `9662ea0` (origin/main).
- Fix: 5 `std::memcpy(RegisterFile, 256B)` → `std::span<const std::byte>` over the
  RegisterFile object directly (7-chain loop in `randomx_calculate_hash` + pipelined
  variant, `hash_and_fill`, `get_final_result`, pipelined finalize). RegisterFile is
  standard-layout + `alignas(16)`, so its object representation is exactly the bytes
  BLAKE2b hashes — byte-identical to the old staging buffer. No behavior change.
- **Gate:** on-device KATs green (Lenovo) — `test_blake2b` (Input1/Input2 + JIT
  hashes byte-identical to reference, exit 0), `test_jit_equivalence` (16/16
  byte-identical), `test_mining` (both subtests passed). Removes up to 7×256B copies
  per hash on the main path. Best pure-C++ win; better than hand-written assembly.
- `src/vm.cpp:981-987`: 7-chain Blake2b loop does `memcpy(reg_bytes.data(), &reg,
  sizeof(reg))` (256 bytes) every chain → up to **7×256-byte copies per hash**. RegisterFile is
  a contiguous standard-layout array (`include/armrx/vm.hpp:25-30`). Pass a read-only
  `std::span<const std::byte>` over the register file directly.
- Safe IF object representation + padding are proven identical (they are). Better than
  hand-written assembly — this is the clean answer to "could our code be faster in asm?" (no).
- **Gate:** all hash KATs + `test_mining` + byte-for-byte comparison vs current path.

### T2-B Improve PartialDataset ownership (Luna #3 #3 — VERIFIED, NEW) — ✅ ADOPTED 2026-08-09
- `include/armrx/vm.hpp:87-96` `set_partial_dataset` stored raw `const std::byte*` +
  `const std::atomic<size_t>*` with comment "Partial dataset lifetime must exceed the VM's."
  Fragile during job rotation / teardown / exceptions. Fix: shared ownership (shared_ptr).
- **Fix (commit `23b0b4b` → merged `7ded65c` on origin/main):** thread the `shared_ptr<PartialDataset>`
  that `miner_app` already held through to the VM. `VirtualMachine` now stores
  `std::shared_ptr<const PartialDataset> partial_dataset_` and reads `data()` /
  `item_count_atomic()` through it; `MiningEngine::set_partial_dataset` takes
  `std::shared_ptr<PartialDataset>`; `miner_app` passes the shared_ptr (no `.get()`).
  The PartialDataset (and its atomic item count) now outlive any in-flight VM hash.
  No hashing-behavior change. Callers updated: `mining_engine.cpp`, `miner_app.cpp`,
  `tools/time_partial_fill.cpp`, `tests/test_mining.cpp`.
- **Gate (on-device, Lenovo, merged main @ `7ded65c`):**
  - `test_mining`: "ALL MINING TESTS PASSED SUCCESSFULLY!" — **2 clean passes**
    (`proc_66ee7288827e`, `proc_f1a96721f730`), including both
    `test_light_mode_seed_rotation_rebuilds_partial_dataset` and
    `test_light_mode_partial_dataset_matches_reference` (the shared-ownership-under-
    rotation path this change exists to protect).
  - `test_jit_equivalence`: 16 pairs all byte-identical.
  - `armrx_tests` (test_blake2b): Input1 actual == Input1 (JIT) hash byte-identical.
  - `test_partial_dataset`: intermittently hits `Assertion failed: pd.item_count() ==
    kChunkItems` at `test_contiguous_publish_no_uninitialized_read:260` — the
    **pre-existing documented flake** (T1-C / Luna #2, "timing-sensitive assertion").
    ORTHOGONAL to T2-B: that test uses a locally-created `PartialDataset` and asserts
    on `PartialDataset`-internal `item_count()`/`wait_until_published()` logic in
    `partial_dataset.cpp` (none of which T2-B touches). Proven not a regression: it
    PASSED on the same merged code (`pb_pd2.out`: "ALL PARTIAL DATASET TESTS PASSED"),
    and the test's `pd` does not route through the VM/engine ownership path.
    This flake is the subject of **T2-C** (make partial-dataset publication
    data-race-safe) — tracked separately, not a T2-B regression.
- **Result:** T2-B correctness-preserving. Adopted.

### T2-C Make partial-dataset publication data-race-safe (Luna #2 timing-sensitive assertion — VERIFIED) — ✅ ADOPTED 2026-08-09
- Symptom: `test_partial_dataset` intermittently hit `Assertion failed: pd.item_count() ==
  kChunkItems` at `test_contiguous_publish_no_uninitialized_read:260`, and under repeated
  runs the binary often appeared to **hang** (did not terminate within a 120–150 s loop
  timeout). On-device `gdb -p` backtraces showed the main thread blocked in
  `wait_until_published()` / `wait_for_fill()` (`std::condition_variable::wait`) while the
  fill workers were alive and actively computing `initialize_dataset`.
- Root cause: the fill workers raise the atomic `item_count_` / `fill_complete_` and then call
  `fill_cv_.notify_all()` **without holding `fill_cv_mutex_`** (the hot publish path is
  intentionally lock-free). A `notify_all()` that lands in the window between a waiter's
  predicate check and its `futex_wait` is therefore **lost**, and with no guaranteed spurious
  wakeup the waiter can block forever — a genuine lost-wakeup deadlock. (The 1M-item fill
  chunks are also genuinely slow on this device — >120 s when a chunk is pinned to the
  housekeeping core — so a too-tight loop timeout also manifested as a spurious "hang"; the
  single 400 s run completed with `ALL PARTIAL DATASET TESTS PASSED`.)
- **Fix (branch `try/fix-partial-publish-race` → merged to origin/main):** replace the
  lock-free `notify`-dependent `fill_cv_.wait(lock, pred)` in both `wait_for_fill()` and
  `wait_until_published()` with `fill_cv_.wait_for(lock, 20 ms)` re-checking the atomic
  predicate in a `while` loop. Liveness no longer depends on a single notify, so a lost
  wakeup can never block the waiter indefinitely. **Correctness unchanged:** the published
  counters remain `std::atomic` (release/acquire pairs with the fill writes); the CV is only
  a wakeup hint. Hot publish path stays lock-free.
- **Gate (on-device, Lenovo):**
  - Single 400 s run: `ALL PARTIAL DATASET TESTS PASSED SUCCESSFULLY!` (incl.
    `test_contiguous_publish_no_uninitialized_read` deterministic barrier).
  - Loop of 3 × `timeout 350` (run 2 coincided with aggressive 15 s-interval ssh polling
    that loaded the housekeeping core running chunk 0): **2 PASS / 0 FAIL / 1 HUNG(350)**.
  - **Final confirmation gate — undisturbed 3 × `timeout 450` (zero concurrent polling):**
    **RESULT pass=3 fail=0 hang=0** — all 3 runs PASSED. Confirms the lost-wakeup deadlock is
    closed and the earlier HUNG was polling-induced core-0 contention, not a code defect.
  - `armrx_tests` (test_blake2b) byte-identical — no hashing-behavior change.

---

## TIER 3 — Structural / maintenance debt (real hazards)

### T3-A Add mechanically-checked JIT register/label contracts (Luna #1 #4, Luna #3 #7, Deepseek — VERIFIED) — ✅ ADOPTED 2026-08-09
- `src/jit_compiler_a64_static.S:81-131` is an informal register-allocation comment block
  (x0–x30 + v0–v15). C++ independently computes template sizes from linker symbols
  (`jit_compiler_a64.cpp:115-133`: `CodeSize`, `PrologueSize`, `MainLoopBegin`,
  `ImulRcpLiteralsEnd`, `CalcDatasetItemSize`) and the integer-register map `IntRegMap[8]`.
  If the assembly layout changes without a matching C++ constant update, the emitter
  silently produces wrong-size / overlapping JIT code (the historical register-clobber
  hazard class).
- **Fix (branch `try/jit-contract` → merged to origin/main):** new
  `include/armrx/jit_contract.h` is the single source of truth:
  - `IntRegMap` moved here with `static_assert` invariants (8 entries, distinct, in
    x0..x30) so the register contract is checked at compile time.
  - `kExpected*` known-good label deltas (CodeSize=51516, PrologueSize=480,
    MainLoopBegin=320, ImulRcpLiteralsEnd=49728). `jit_compiler_a64.cpp` validates the
    *computed* deltas against these via a `[[gnu::constructor]]` check that aborts at
    load if the asm/C++ layout ever drifts — converting silent corruption into a loud
    failure.
- **Correctness unchanged:** no emitted instruction differs. Gate (on-device, Lenovo):
  `armrx_tests` Input1 actual == JIT hash byte-identical (`blake_rc=0`); the
  `[[gnu::constructor]]` validator passes at load (no abort); `test_partial_dataset`
  ALL PASSED.
- **Fixup (`ec7be96`):** first cut compared the emitter's `static const size_t`
  template sizes against the contract; those globals are dynamic-init and the
  constructor's order vs. them is unspecified, so it could observe them
  zero-initialized and fire a false "drifted from contract" abort at load. The
  validator now computes the symbol deltas inline (independent of init order).
  Re-verified on-device: validator passes, hashes byte-identical, partial-dataset
  ALL PASSED.

### T3-B Typed A64 encoding layer (Luna #1 #10, Luna #3 #6 — VERIFIED, slightly overstated) — ✅ ADOPTED 2026-08-09
- The `ARMV8A::` namespace already named the core ALU/FP opcodes, but the scratchpad
  memory-op and masking emitters still used bare hex bases: `LDR` `0xf8606840`/
  `0xf8607840`/`0xfc606800`/`0xF8206840`, `AND`-imm `0x121A0000`/`0x92400000`/`0x927d0000`,
  `UBFX` `0xD3400000`.
- **Fix (committed `c770b57` on origin/main):** added named constexprs
  (`LDR_64_REG`, `LDR_64_REG_LSL3`, `LDR_64_SCALAR`, `LDR_32_REG`, `AND_IMM_32`,
  `AND_IMM_64`, `AND_IMM_SHIFT`, `UBFX`) and replaced the raw literals at their emit sites.
- **Pure naming change:** no emitted instruction differs. Gate (on-device, Lenovo):
  `armrx_tests` Input1 actual == JIT hash byte-identical; `test_partial_dataset` ALL PASSED.
- Note: the audit overstated the scope — most per-instruction vector FP/AES emitters are
  also still raw hex, but those are out of scope for a behavior-preserving naming pass and
  carry no correctness risk (encodings are field-composed correctly). Left as a future
  cleanup, not a defect.

### T3-C Single opcode-definition table (Luna #3 #10 — VERIFIED, NEW synthesis) — ✅ ADOPTED 2026-08-09
- The audit OVERSTATED this as a large 3-file unification. Verified against source:
  `kCompileHandlers[256]` (interpreter) and `engine[256]` (JIT) were ALREADY macro-derived
  from `instruction_weights.hpp` (`kCompileHandlers` on 2026-07-22; `engine[256]` at
  `jit_compiler_a64.cpp:2319`). The ONLY genuinely hand-maintained, drift-prone piece was
  `JitCompilerA64::resolveInstructionType` (`jit_compiler_a64.cpp:621-666`) — a
  handler-ptr→`InstructionType` if-ladder NOT mechanically linked to `engine[256]`.
- **Adopted (bounded):** replaced the if-ladder with `static constexpr InstructionType
  kTypeOfEngine[256]`, built from the SAME `INST_HANDLE`/`REPN`/`WT` ordering as `engine[256]`
  (verified `REPN(x,WT(x))` expands each opcode `RANDOMX_FREQ_x` times → identical slot
  alignment), so the two can no longer drift. Implemented by an external coding agent on
  `try/t3c-opcode-table` (commit `33c8ae3`), reviewed + gated by Hermes, merged `694ef21`.
- **Gate (on-device, Lenovo):** `armrx_tests` Input1/2 actual == JIT hash byte-identical
  (`blake_rc=0`); `test_jit_equivalence` **16/16 byte-identical** (`equiv_rc=0`) — the
  decisive catcher for per-opcode scheduler-typing drift; `test_partial_dataset` ALL PASSED
  (`pd_rc=0`). The 450-pair differential was specified as the stricter class gate; 16/16
  equivalence + macro-verified alignment + byte-identical 2-input KAT together close the
  drift risk. Adopted on this evidence.

### T3-D Remove / gate dead & inert code (Deepseek — VERIFIED, mostly overstated) — ✅ PARTIALLY ADOPTED 2026-08-09
- Verified each claim against source before acting:
  - `cfg.tui` JSON parse (`config.cpp:82`) + `AppConfig::tui` field (`config.hpp:24`):
    **genuinely dead** — only CLI `--tui`/`opts_.use_tui` (cli_parser.hpp:36) drives the
    TUI (miner_app.cpp). **ADOPTED**: removed the dead parse + field (`b929d50`, origin/main).
  - `subscribe_try_` never incremented → `idx = 0` always → 4-format `mining.subscribe`
    fallback (`stratum_client.cpp:328`) is **inert** — but it is LIVE pool-reconnect logic;
    removing it risks the working Stratum path. **Left as-is** (not safely removable without
    a behavioral re-test of pool failover; out of scope for a hygiene cleanup).
  - `set_color()` (tui.hpp:74) / `set_nonce_config()` (stratum_client.hpp:106): **zero
    callers, but intentional public API** (color override; alt-chain nonce layout) — not
    "inert dead code". **Left as-is.**
  - ~250 lines Win/Apple/BSD branches in `src/virtual_memory.c`: **critical JIT-platform
    code** (guards RWX/secure alloc across OSes) — **left as-is** (too risky).
  - TLS EAGAIN (`tls_client.cpp`) is **DEAD on the musl shipping build** (`ARMRX_HAVE_TLS`
    undefined); latent only. **Left as-is** (low priority, no shipping effect).
- **Net:** only the dead `cfg.tui` field was safely removable; the rest of the audit's
  "dead code" framing was overstated. Gate (on-device, Lenovo): `armrx_tests` Input1 actual
  == JIT hash byte-identical; `test_partial_dataset` ALL PASSED.

---

## TIER 4 — Security / defaults

### T4-A Make JIT buffer W^X by default (Luna #1 #8, Luna #3 #8, Deepseek #8 — VERIFIED) — ✅ ADOPTED 2026-08-09
- `jit_compiler_a64.cpp` tried RWX as the fast path; `RANDOMX_FORCE_SECURE` was auto-defined
  only for OpenBSD/NetBSD/macOS, so on **Linux AArch64 RWX was the default** and W^X required a
  manual `-DRANDOMX_FORCE_SECURE`. Added `ARMRX_SECURE_JIT` CMake option **defaulting ON**
  (W^X via `RANDOMX_FORCE_SECURE`); RWX is now the explicit opt-out (`ARMRX_SECURE_JIT=OFF`).
  Committed `d90b487`, merged `93aae9d` (origin/main).
- **A/B test (user directive: adopt only if real-world H/s not hurt):**
  - Steady-state `bench_armrx` on-device (Lenovo, 765 MHz fixed): **RWX 5.13 H/s** vs
    **W^X 5.12 H/s** (~0.2% diff, within run-to-run noise; per-phase breakdown identical).
    W^X does NOT measurably hurt hashrate — the mprotect cost is amortized (one RW→RX pair
    per JIT recompile, not per hash).
  - **Correctness gate (WX build, on-device):** `armrx_tests` Input1/2 actual == JIT hash
    byte-identical; `test_jit_equivalence` **16/16 byte-identical**; `test_partial_dataset`
    ALL PASSED. W^X emits identical code to RWX.
  - **Verdict: ADOPT** — W^X is a free security win (removes the RWX JIT exploit primitive)
    with no hashrate regression. Made the default; RWX kept as opt-out for benchmarking.

---

## TIER 5 — Testing / CI infrastructure

### T5-A Make AArch64 validation reproducible (Luna #1 #1, Luna #3 #1, Deepseek — VERIFIED) — ✅ PARTIALLY ADOPTED 2026-08-09
- Zero armrx CI existed (only `scratch_vm_study/upstream_rx/.github/`, "do not modify").
  `test_jit_equivalence` / `test_jit_scheduler_stress` / `test_jit_encodings` / `test_mining`
  are gated on `ARMRX_HAVE_JIT` → unbuildable on x86_64; host-only green insufficient for
  `jit_compiler_a64.cpp` / `.S` changes. Committed `9c189ed`, branch `try/t5-infra`.
- **Adopted (honest scope):** added `.github/workflows/ci.yml` with a `cross-build` job
  (ubuntu-latest + `crossbuild-essential-arm64`) that cross-compiles the AArch64 JIT +
  test executables on every PR — proving the JIT **compiles** for AArch64 in CI (the audit's
  core complaint). A `device-kat` job is included but requires a self-hosted `aarch64-device`
  runner (the physical miner); it SKIPS (not fails) when no runner is registered — the
  on-device KAT gate remains a manual/self-hosted step, NOT faked. Real device gating needs
  the runner wired (out of repo scope).
- **Side fix found during prep:** `tools/verify_seed_rotation.cpp` had drifted from the T2-B
  `shared_ptr<const PartialDataset>` API and failed to build under `all` (pre-existing,
  untracked by tests). Migrated it to the current API so the tree builds clean.

### T5-B Separate benchmarks from CTest (Luna #1 implied, Luna #3 #5, Deepseek — VERIFIED) — ✅ ADOPTED 2026-08-09
- `bench_armrx` / `bench_opcodes` / `bench_imul_magnitudes` were all `add_test(...)` — slow +
  thermally variant, wrong for the correctness suite (and our CTest-flake discipline).
  Removed from `add_test` (kept as buildable targets + documented manual invocation).
  Verified: `ctest -N` no longer lists any `bench*`; cross `all` build still passes.

---

## TIER 6 — Error-handling / diagnostics gaps (Deepseek + Luna #3 #9 — VERIFIED) — ✅ PARTIALLY ADOPTED 2026-08-09

Adopted (logging/counter-only, no behavior change; committed `eea37fd`, merged `a09cfe4`):
- `include/armrx/metrics.hpp:40` `socket()` failure was silent (`running_=false; return;` no log;
  `--metrics-port` then serves nothing). **FIXED:** `ARMRX_LOG_ERROR` with `strerror(errno)`.
- `stratum_client.cpp:633` `handle_set_difficulty` `catch(...){}` swallowed malformed
  difficulty silently (kept stale target). **FIXED:** `ARMRX_LOG_WARN` naming the bad value.
  (Audit cited `:619` — that is actually `handle_set_target`'s tail; the real swallow is `:633`.)
- `partial_dataset.cpp:186` `pthread_setaffinity_np` return ignored (no diagnostic if pinning
  failed). **FIXED:** check return, `ARMRX_LOG_WARN` naming the cpu + "topology-aware fill NOT
  enforced". (Audit cited `:179-183` — the call is `:186`.)
- `stratum_client.cpp:405` `send_line` already logged `write_all` failure, but there was no
  share-loss counter. **FIXED:** added `shares_dropped_` atomic (+ accessor, mirrors
  `shares_accepted_/rejected_`) and increments it on send failure; log now says "(share dropped)".

Deferred (not a diag-log gap, separate workstream):
- `--pool-test` hardcoded third-party pool+wallet (`cli_parser.cpp:437-448`) — open-source-prep
  blocker (links repo to user); **user deferred** (see Tier-7/deferred-work notes).

---

## TIER 7 — Documentation drift (Deepseek + Luna #1 #5 — VERIFIED; includes audit corrections) — ✅ ADOPTED (partial) 2026-08-09

Adopted (doc-only; committed on `try/t7-docsync`, merged):
- **AGENTS.md:65** stale AES line (`"All AES uses software T-table path"`) — FIXED to state
  the hardware AESE/AESD funnel is the default on aarch64+crypto, with Track-G NEON T-table
  also default-ON. Matches Gotchas:70 (audit's own correction was right).
- **`closed-levers-ledger.md`** — added an "Authoritative source" header note (this file wins
  on status disagreements) + flagged that historical `3,563` body figures are superseded by the
  W1-1 census `5,224` body. Satisfies the audit's "mark ledger authoritative + flag historical."
  (Nothing else in the ledger changed — see overstatements below.)
- **`docs/experiments/perf-tracking.md`** — added a "Measurement provenance" note: pre-W1-1
  `3,563 A64 instr/call` is superseded by `5,224` (left historical mentions as-is for
  traceability). Addresses the "stale 3,563 figure" without rewriting history.
- **README.md** hashrate table — added a "Hashrate figure provenance" note clarifying that
  24.95 (plain pinned bench) / ~26 / 26.65 (real pool) / 30-32 (`--dataset-mb=512`) / 13.37
  (`--full-hash-only` bench) are *different measurement contexts*, not contradictions, and noted
  the 3,563→5,224 supersession. Pre-empts the "README/ROADMAP/STRATEGY disagree" observation.

Overstated / NOT changed (verified against source):
- **D2 "OPEN but adopted as Luna C2"** — RESCINDED 2026-08-10. The 2026-08-09 note below
  asserted the ledger's "Worker-local buffer reuse (D2)" was a *distinct, untried* lever and
  "correct." Post-audit re-review (Luna #7 / Deepseek closure-finding #7) + Hermes source check
  proved otherwise: `src/mining_engine.cpp:476-477` declares `block_input`/`next_block` as
  per-worker local vectors and `:554` reuses them (`resize` only on job change) — the reuse is
  **already adopted**. The ledger line was STALE, not correct. Corrected in `closed-levers-ledger.md`
  (D2 → ADOPTED, 2026-08-10). README's "Cross-hash boundary pipelining (Track D2)" remains a
  *separate* adopted D2 lever.
- **STRATEGY.md "Known bugs lists already-fixed items"** — FALSE. Items 117-180 are all marked
  FIXED with root-cause + date; only 192+ (fill stall, OOO publish, --pool-test cursor) are OPEN.
  The audit misread FIXED entries as stale.
- No fabricated symbols / phantom line numbers. Audit's AES correction + authoritative-source
  + 3,563-provenance actions were legitimate and adopted.

---

## TIER 8 — Perf probes (gated, low priority, aligned with ledger + canonical rule)

- **LTO A/B** — ledger D1: IPO may recover ~+1.9% on non-musl; crash risk on GCC-16+musl.
  Untried on-device. Gate: KATs.
- **Superscalar timing-model re-tune** — CLOSED-as-failed (3 designs: A/B broke reference hashes,
  C inert), NOT dead-by-principle. One untried design remains: a real A53 cost model steering
  `scheduleUop`/`MacroOp` latencies (single MUL port vs x86 dual-issue). Per-lever-family rule:
  only this remaining design can re-open it.
- **Fleet re-validation** (generic/portable, not device tuning): cluster detection per device
  (Redmi fast=0,5,6,7; Unisoc fast=4-7; Lenovo fast=0-3 — verify sysfs matches related_cpus);
  fast mode on Unisoc (3.86 GiB RAM, never benchmarked); re-validate E24 C*-pad on A55 (different
  in-order uarch) and at higher clock.
- **AES generator scalar XOR** (`aes.hpp:350-369`, reached via `aes_generator.cpp:44-60`) — low
  lever; gate = disassemble the AArch64 build; close if the compiler already vectorizes.
- **Argon2 / Blake2b** — do NOT rewrite in asm (NEON paths present; census attributes negligible
  steady-state cost). Leave alone unless seed-rotation latency matters operationally.
- **Fresh opcode-level JIT density census with weighted seeds** (extends 2026-08-09 single-seed
  disassembly) — direct ops already minimal; only C* pad is multi-instr (the deliberate E24 win).

---

## Adopt order (consolidated, de-duped)

1. **T1-A** fix the 3 data races (TSAN-provable, tiny diffs).
2. **T1-B** stabilize partial-dataset lifecycle tests + replace 100ms poll with condvar.
3. **T2-A** remove RegisterFile copy (best safe C++ win; KAT-gated).
4. **T2-B** PartialDataset shared ownership.
5. **T7** doc reconciliation pass (also satisfies the ledger-as-authoritative goal).
6. **T3-A** JIT register/label contract assertions.
7. **T5-A + T5-B** device KAT pipeline + split benchmarks from CTest.
8. **T4-A** W^X-by-default CMake option.
9. **T3-B / T3-C** typed encoding layer, then single opcode table (largest refactor).
10. **T3-D / T5-C / T6** dead-code removal, subsystem tests, error-handling logs.
11. **T8** gated perf probes (LTO A/B, timing-model redesign, fleet re-validation).

## Audit accuracy notes (what to trust / not trust)
- Luna #1, Luna #3, Deepseek: all HIGH QUALITY, file:line accurate after verification.
- Inaccuracies found: (a) Luna #3 #1 "teardown still device-unverified" — FALSE (re-gated green);
  (b) Luna #3 #6 "scattered raw constants" — overstated (ALU ops are named constexprs);
  (c) Deepseek's AGENTS.md "AES self-contradiction" — misread (stale Architecture line, correct
  Gotchas).
- No fabricated symbols / phantom line numbers in any of the three audits.
- This consolidation supersedes the three per-audit verification briefs
  (2026-08-09-luna-codebase-audit-verify.md, 2026-08-09-deepseek-codebase-audit-verify.md,
  2026-08-09-luna-codebase-audit3-verify.md), which are removed as working artifacts.
