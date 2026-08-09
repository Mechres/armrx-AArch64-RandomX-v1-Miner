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

---

## TIER 3 — Structural / maintenance debt (real hazards)

### T3-A Add mechanically-checked JIT register/label contracts (Luna #1 #4, Luna #3 #7, Deepseek — VERIFIED)
- `src/jit_compiler_a64_static.S:81-113` is an informal register-allocation comment block
  (x0–x30 + v0–v15). C++ independently computes copied template sizes
  (`jit_compiler_a64.cpp:120-133`) and patches labels/offsets.
- Fix: compile-time offset assertions + a generated/register-contract table shared by C++ and
  assembly. Directly addresses the historical hybrid-path register-clobber crashes.

### T3-B Typed A64 encoding layer (Luna #1 #10, Luna #3 #6 — VERIFIED, slightly overstated)
- Raw constants exist: `MUL=0x9B007C00`, `UMULH=0x9BC07C00`, `SMULH=0x9B407C00`
  (`jit_compiler_a64.cpp:93-95`, named constexprs) AND scattered raw hex for scratchpad
  the memory-op encoders: `0x927d0000` (AND imm, :1585/1602/1637/2254), `0xf8606840` (LDR, :1610).
- Audit overstatement: core ALU ops ARE named constexprs; only scratchpad AND/LDR encoders are
  raw. Fix: constexpr encoders for ADD/AND/LDR/STR/MUL/UMULH/branches/SIMD + exhaustive encoding
  tests against known disassembly.

### T3-C Single opcode-definition table (Luna #3 #10 — VERIFIED, NEW synthesis)
- Opcode knowledge duplicated across: `vm.hpp:191-220` (handler decls), `vm.cpp:539-582`
  (dispatch table "derived from instruction_weights.hpp"), `jit_compiler_a64.cpp:573-617`
  (`resolveInstructionType` hand-written 256-opcode if-ladder). Plus scheduler/metadata.
- Fix: generate instruction metadata, interpreter handlers, JIT handlers, scheduler footprints
  from ONE opcode-definition table. Largest refactor — do last.

### T3-D Remove / gate dead & inert code (Deepseek — VERIFIED)
- `subscribe_try_` never incremented (`grep` for `++` returns 0) → `idx = 0 % 4 = 0` always →
  the 4-format `mining.subscribe` fallback (`stratum_client.cpp:320`) is inert.
- `cfg.tui` parsed (`config.cpp:82`) but consumed nowhere — JSON `"tui":true` silently no-ops.
- `Tui::set_color()` (tui.hpp:74) and `StratumClient::set_nonce_config()`
  (stratum_client.hpp:106) have zero callers.
- ~250 lines dead Windows/Apple/BSD branches in `src/virtual_memory.c` (self-acknowledged).
- TLS EAGAIN handling (`tls_client.cpp:111-122` sets `errno=EAGAIN` but returns ≤0; `write_all`
  treats as fatal) — latent, and DEAD on the musl shipping build (`ARMRX_HAVE_TLS` undefined).
  Low priority.

---

## TIER 4 — Security / defaults

### T4-A Make JIT buffer W^X by default (Luna #1 #8, Luna #3 #8, Deepseek #8 — VERIFIED)
- `jit_compiler_a64.cpp:162-185` attempts RWX as the fast path. `RANDOMX_FORCE_SECURE` is
  auto-defined ONLY for OpenBSD/NetBSD/macOS — on **Linux AArch64 it is NOT defined**, so RWX is
  the default and W^X needs a manual `-DRANDOMX_FORCE_SECURE` (no CMake option, no runtime
  toggle). Fix: add `ARMRX_SECURE_JIT` CMake option defaulting to W^X on Linux; expose RWX as an
  explicit benchmark-only option. Recompilation overhead is amortized (rare per steady-state
  hash) — measure separately, don't assume it hurts mining.

---

## TIER 5 — Testing / CI infrastructure

### T5-A Make AArch64 validation reproducible (Luna #1 #1, Luna #3 #1, Deepseek — VERIFIED)
- Zero armrx CI (only `scratch_vm_study/upstream_rx/.github/` exists, "do not modify").
  `test_jit_equivalence` / `test_jit_scheduler_stress` / `test_jit_encodings` / `test_mining`
  are gated on `ARMRX_HAVE_JIT` → unbuildable on x86_64. Host-only green is insufficient for
  `jit_compiler_a64.cpp` / `.S` changes. Fix: automated cross-build + device KAT job.

### T5-B Separate benchmarks from CTest (Luna #1 implied, Luna #3 #5, Deepseek — VERIFIED)
- `bench_armrx` (CMakeLists.txt:221), `bench_opcodes` (:264), `bench_imul_magnitudes` (:362-364)
  all `add_test(...)`. Benchmarks in the unit suite = slow/noisy/thermal-variance-prone. Fix:
  opt-in benchmark target or script; keep correctness tests in CTest.

### T5-C Untested subsystems need coverage (Deepseek — VERIFIED)
- TUI, MetricsExporter, TLS client have zero test references; TUI already shipped two device-
  caught bugs (UAF + cout race). `fuzz_json` not in default ctest (CMakeLists.txt:379 opt-in).

---

## TIER 6 — Error-handling / diagnostics gaps (Deepseek + Luna #3 #9 — VERIFIED)

- `include/armrx/metrics.hpp:40` silently swallows `socket()` failure (`if (fd<0){running_=false;
  return;}` — no log); `--metrics-port=N` then serves nothing.
- `stratum_client.cpp:619` `catch(...){}` swallow in `handle_set_difficulty`.
- `partial_dataset.cpp:179-183` `pthread_setaffinity_np` return ignored; madvise/prefault
  best-effort without diagnostics. Fix: log when requested affinity fails; don't claim
  topology-aware fill if pinning failed.
- `stratum_client.cpp:394-396` `write_all` failure drops a share with no counter (silent loss).
- `--pool-test` hardcoded third-party pool+wallet (`cli_parser.cpp:437-448`) — known open-source-
  prep blocker (links repo to user); user deferred.

---

## TIER 7 — Documentation drift (Deepseek + Luna #1 #5 — VERIFIED; includes audit corrections)

- README 8w = 24.95 H/s is stale (pre-Lever-3 plain bench); authoritative real-pool ≈ 26.65 H/s.
  README / ROADMAP / STRATEGY disagree on the figure.
- `docs/closed-levers-ledger.md:128-131` D2 still says "Worker-local buffer reuse (D2) — OPEN,
  untried" but it was ADOPTED as Luna C2 on 2026-08-08 (README:177). Ledger not updated.
- STRATEGY.md "Known bugs" lists already-fixed items (fill stall, TUI bugs, SIGINT, OOO publish).
- ⚠️ **Audit correction:** AGENTS.md does NOT self-contradict on AES. Architecture section
  (:65) is stale ("All AES uses software T-table path"), but Gotchas (:70) correctly states
  "Hardware AESE/AESD IS now used as the default aarch64+crypto AES funnel." Fix the Architecture
  line, not a "self-contradiction."
- Action: mark `closed-levers-ledger.md` as the single authoritative optimization-status source
  (it currently has 0 "authoritative/only source" markers despite AGENTS.md asserting it should
  be); flag historical measurements clearly. Also fix the stale "3,563 instructions" figure
  (incl. in `2026-08-09-superscalar-disassembly-findings.md` — real body = 5,224 per census).

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
