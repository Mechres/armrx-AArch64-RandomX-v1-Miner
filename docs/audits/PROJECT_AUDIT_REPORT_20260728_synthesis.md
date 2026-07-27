# Project Audit — 2026-07-28 Synthesis

Third full-repo audit pass, run as four parallel subsystem reviews (JIT/assembly, networking/pool,
crypto core, application layer/concurrency), each instructed to read the prior 2026-07-25 audits
(`PROJECT_AUDIT_REPORT_*_25072026.md` / `_Deepseek.md`, plus the scheduler-specific and boundary-
followup docs) first and report only new findings. This doc consolidates their output; each
subsystem's full report is preserved in the session transcript, this is the ranked synthesis.

## High severity — real, live bugs

### 1. Config-file-only pool mining is a silent no-op
`src/cli_parser.cpp` — the config-loading block populates `o.pool_list`, wallet, workers,
difficulty, etc. from `config.json` (or `$ARMRX_CONFIG` / `~/.config/armrx/config.json` /
`./armrx.conf`), but never sets `o.should_connect_pool = true`. That flag is set in exactly one
place: the `--pool=` CLI-flag handler. `MinerApp::run()` (`src/miner_app.cpp:511`) gates all pool
mining behind this flag. **A user who configures everything via config file and passes no
`--pool=` flag gets a fully-populated but unused `pool_list` — the process prints its banner and
exits without connecting, mining, or emitting any error.** `tests/test_cli_parser.cpp`'s
`test_config_file_cli_precedence` asserts the other fields get populated from the config file but
never asserts `should_connect_pool`, so the existing suite doesn't catch this.

**Recommendation:** set `should_connect_pool = true` when the config-loading path yields a
non-empty `pool_list`, and add the missing assertion to `test_config_file_cli_precedence`.

### 2. `PoolManager` unsynchronized cross-thread reads
`src/pool_manager.cpp` — `is_connected()` (:55-58) and `current_pool_name()` (:34-37) read
`stratum_`/`current_idx_` without taking `stratum_mutex_`, unlike `shares_accepted()`/
`shares_rejected()` (:45-53) which do lock. This is exploitable today, not theoretical:
- `is_connected()` is called from `MetricsExporter`'s background thread (`miner_app.cpp:316`)
  concurrently with `tick()` → `connect_to_current()` (:60-83), which destroys and reassigns
  `stratum_` under lock during failover — a metrics scrape mid-failover races an unsynchronized
  pointer read against a locked destroy+reassign (UB, plausible use-after-free).
- `current_pool_name()` is called from `StratumClient`'s own reader/reconnect threads via the
  error-callback lambda (`miner_app.cpp:347-350`, `stratum_client.cpp:428-430,736-738`), racing
  `tick()`'s locked mutation of `current_idx_` on the main thread.

**Recommendation:** take `stratum_mutex_` in both accessors, matching the pattern already used by
the share counters.

## Medium severity

### 3. TLS `set_verify_peer(false)` is a silent no-op
`src/tls_client.cpp:26-38` — `SSL_CTX_set_verify()` is only called once, in the constructor, using
`verify_peer_`'s default (`true`). `set_verify_peer()` updates the member but never re-applies it
to `ctx_`. `--pool-tls-verify=false` therefore has zero effect. Direction is fail-safe (can't
accidentally weaken verification), so not a MITM exposure — but a user targeting a self-signed-cert
pool gets a hard connection failure instead of the behavior the flag promises, and the CLI surface
is dead code today.

**Recommendation:** either make `set_verify_peer()` re-call `SSL_CTX_set_verify()` on `ctx_`, or
remove the flag and document that peer verification is unconditionally on.

### 4. Interrupted live dataset rebuild can commit a partially-initialized dataset
`src/mining_engine.cpp:210-231,263-266` — in `set_job()`'s fast-mode live-rebuild path, if `stop()`
races in before all worker chunks finish, the wait on `dataset_init_cv_` unblocks via the
`!running_` branch, but `shared_dataset_ = new_dataset; current_seed_key_ = job.seed_key;` runs
unconditionally regardless of whether `dataset_init_remaining_` actually reached zero. Any worker
that hadn't observed the generation bump before `running_` went false leaves its slice
uninitialized, yet the seed key is recorded as already-built. **Currently dormant** — `MinerApp`
only calls `start()`/`stop()` once each per process — but real, and would produce silently wrong
hashes if the engine is ever reused across a job resubmission without a process restart (e.g. a
future reconnect-without-restart feature).

**Recommendation:** guard the commit with a check that the rebuild actually completed, or discard
`new_dataset` and keep the prior one if interrupted by shutdown.

## Low / informational — hardening leads, not live defects

- **`src/randomx_config.hpp`'s `kRandomXDatasetBytes`** is 64 bytes (one dataset item) larger than
  upstream RandomX's `RANDOMX_DATASET_EXTRA_SIZE`. `vm.cpp:239` independently hardcodes the
  spec-correct `524288` literal for the actual read-address bound, so this is **not a live
  consensus bug** — only one superfluous dataset item is computed and stored, wasted but never
  read. Landmine for any future refactor that derives bounds from the header constant instead of
  `vm.cpp`'s own correct literal.
- **Scheduler window-widening (`jit_compiler_a64.cpp:687-702`, commit `96f7acf`, 2026-07-26)**
  postdates all three independent scheduler reviews and hasn't received the same review rigor this
  project's own stated bar requires for scheduler changes. Manual tracing found it hazard-safe and
  it passes the 450-pair stress test, but that test is generic/random rather than purpose-built for
  the rare shapes this specific widening could expose.
- **Superscalar/dataset-item JIT buffer (`CalcDatasetItemSize`, `jit_compiler_a64.cpp:118-131`)**
  has zero safety margin for the theoretical all-`IMUL_RCP` worst case (16 bytes/instruction ×
  512 exactly), unlike the main-program buffer's ~3.8× margin. Not currently exploitable
  (`generate_superscalar()` hard-clamps program size), but any future per-instruction emission
  growth (e.g. a defensive mask like the one already added to the main-path `h_IMUL_RCP`) would
  silently overflow it.
- **`rx_calc_dataset_item`'s register-preservation contract** (`jit_compiler_a64_static.S:836-845`)
  isn't documented at the risk site itself — the constraint that this function must never touch
  x14/x15/x20-x28 under light mode's reduced-save call site only exists in experiment docs. Same
  class of gap that caused two reverted attempts (Track C, superscalar register pre-assignment).
- **`instruction_weights.hpp`'s `engine[256]` completeness** depends on `RANDOMX_FREQ_*` summing to
  256, hand-verified correct but not `static_assert`-enforced; a future drift would fall through to
  a null function-pointer call past the release-mode-noop `ARMRX_ASSERT`.
- **`VirtualMachine::hash_and_fill()`** (`vm.cpp:928-937`) is dead code with zero callers and no
  test coverage — fine as-is (the live path uses `get_final_result()` correctly), but would need
  its own tests before ever being wired in.
- **`mining_engine.cpp`'s `AffinityMode::All` branch** (:329-335) lacks the `!core_order_.empty()`
  guard its `BigOnly` sibling has, a latent modulo-by-zero if `detect_core_order()`'s
  always-non-empty guarantee ever changes.
- **`MetricsExporter`** silently swallows `socket()` failure (unlike `bind()`/`listen()`, which log)
  — diagnostic gap only.
- **`StratumClient`'s `extra_nonce1_`/`extra_nonce2_size_`** are populated but never read anywhere
  — dead state, not a correctness bug since nothing consumes it.

## Verified clean (no new issues; reconfirms prior audits)

Argon2d prehash/H'/reference-index formulas, Blake2b (param block, sigma table, endianness,
block-boundary handling), AES T-tables and round structure, the VM interpreter's CFROUND/rounding-
mode scoping, CBRANCH/IADD_RS/FSCAL_R/FDIV_M semantics, main-loop step ordering, and NEON dataset/
superscalar lane-equivalence were all cross-checked directly against the upstream RandomX spec
(`tevador/RandomX`) rather than assumed from memory, and matched. `shared_cache_`/`shared_dataset_`
access in `mining_engine.cpp` is consistently mutex-guarded on both producer and consumer sides —
no races found there. `config.cpp`/`cli_parser.cpp` numeric-field exception guards, `metrics.hpp`'s
thread-join-on-destroy and atomic `server_fd_`, and `pool_manager.cpp`'s two previously-fixed
failover gaps were all spot-checked against current source and confirmed still correct.

## Recommended next action

Findings 1 and 2 are real, user-facing/correctness-affecting bugs with small, well-scoped fixes —
worth fixing directly rather than just tracking. Findings 3-4 are lower urgency (3 is fail-safe
today; 4 is dormant). The informational items are process/hardening leads for whoever next touches
those specific files, not action items on their own.
