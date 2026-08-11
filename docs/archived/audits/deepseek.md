Audit verification — docs/briefs/2026-08-09-audit-consolidated.md
  ─────────────────────────────────────────────────────────────────

  Back on main (ef419f1, in sync with origin/main). Every adoption claim in Tiers 1–7 was checked against the actual repo — all verified present and
  merged. Tier 8 confirmed not done (the only unexecuted part), exactly as the doc's heading implies.

  Tiers 1–7: all adopted & merged ✅

  Tier        │ Claim            │ Verified on main
  ────────────┼──────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────
  T1-A 3 data │ cc014e7 → merged │ ✅ extra_nonce_mutex_ (hpp:193 + R/W sites cpp:384,541); sockfd_ → std::atomic<int> (hpp:179);
  races       │ 6af9744          │ PoolManager::connect() wrapped in stratum_mutex_, released before connect_to_current() (pool_manager.cpp:80-100)
  T1-B        │ a1967ae → merged │ ✅ fill_cv_/fill_cv_mutex_, wait_until_published present (partial_dataset.cpp)
  lifecycle   │ 39f8464          │
  tests +     │                  │
  condvar     │                  │
  T2-A        │ 881f243 → merged │ ✅ std::span<const std::byte> over reg_ directly in the 7-chain BLAKE2b loop (vm.cpp:966,976)
  RegisterFil │ 9662ea0          │
  e copies    │                  │
  T2-B shared │ 23b0b4b → merged │ ✅ std::shared_ptr<const PartialDataset> partial_dataset_ (vm.hpp:237); engine/miner_app take shared_ptr
  ownership   │ 7ded65c          │ (mining_engine.hpp:105,204)
  T2-C lost-  │ merged 79a7db2   │ ✅ fill_cv_.wait_for(lk, kPoll) polling loop replacing notify-only wait (partial_dataset.cpp:277-281); comment
  wakeup      │ (try/fix-        │ documents the TSan/lost-wakeup analysis
  deadlock    │ partial-publish- │
              │ race)            │
  T3-A JIT    │ d19bcb6 + fixup  │ ✅ include/armrx/jit_contract.h exists; [[gnu::constructor]] ValidateJitLabelContract checks kExpected* deltas
  contract    │ ec7be96          │ (jit_compiler_a64.cpp:162-177)
  T3-B typed  │ c770b57          │ ✅ LDR_64_REG/AND_IMM_64/UBFX named constexprs used at emit sites
  encoders    │                  │
  T3-C single │ 33c8ae3 → merged │ ✅ kTypeOfEngine[256] + return kTypeOfEngine[opcode]; on main
  opcode      │ 694ef21          │
  table       │                  │
  T3-D dead   │ b929d50          │ ✅ zero cfg.tui references remain
  cfg.tui     │                  │
  T4-A W^X    │ d90b487 → merged │ ✅ ARMRX_SECURE_JIT option default ON (CMakeLists:43), RANDOMX_FORCE_SECURE wiring (jit_compiler_a64.cpp:212)
  default     │ 93aae9d          │
  T5-A CI     │ 9c189ed → merged │ ✅ .github/workflows/ci.yml with cross-build job + self-hosted aarch64-device job that SKIPs when no runner
  cross-build │ 4d740e3          │ (lines 70-71); verify_seed_rotation.cpp migrated to std::make_shared<PartialDataset> API (tools:42-43)
  T5-B        │ same commit      │ ✅ ctest -N = 15 tests, no bench\* entries
  benchmarks  │                  │
  out of      │                  │
  CTest       │                  │
  T6 diag     │ eea37fd → merged │ ✅ metrics socket() failure logs strerror (metrics.hpp:40-43); difficulty catch(...) → ARMRX_LOG_WARN
  logging     │ a09cfe4          │ (stratum_client.cpp:634-635); affinity failure logged "topology-aware fill NOT enforced"
              │                  │ (partial_dataset.cpp:187-190); shares_dropped_ atomic + accessor + fetch_add (stratum_client.hpp:91,238 /
              │                  │ cpp:406)
  T7 docs     │ merged 6acba99   │ ✅ AGENTS.md:65 AES line corrected (hardware funnel default); ledger "Authoritative source" note (line 3); README
              │                  │ "Hashrate figure provenance" note (line 62)

  Tier 8 — confirmed NOT done (the one omitted tier) ✅

  • No new tier-8 code on main: all git log main hits for LTO/timing-model/fleet/argon2/census are pre-dating the audit (2026-08-07 doc commits or
    device docs), no adoption commits.
  • All tier-8 evidence branches remain unmerged: try/superscalar-a53-mul-model, try/superscalar-mul-chain-penalty, try/superscalar-mul-decomp,
    try/tier2b-{8wide,neon-mulh,cacheline}.
  • include/armrx/aes.hpp scalar XOR untouched since Item 1 (2026-08-03); ARMRX_DISABLE_LTO still a default-OFF option (CMakeLists:418) — the LTO A/B
    probe remains unrun.

  Tree coherence (strongest check I can do without the device)

  Forced recompile of the three most-changed files (jit_compiler_a64.cpp, stratum_client.cpp, partial_dataset.cpp) + full all cross-build: 0 errors,
  100% built including verify_seed_rotation, time_partial_fill, all 15 test targets, all bench targets.

  Audit-internal notes

  • T3-A merge isn't labeled "Merge T3-A" — it landed as direct commit d19bcb6 + fixup ec7be96 (no merge commit). Content is present; branch try/jit-
    contract was apparently not kept (only 35 try/ branches remain, no jit-contract).
  • No discrepancies found between the doc's claims and the tree. The doc's audit-accuracy notes (Luna #3 teardown claim, T3-D overstatement,
    STRATEGY.md misread) also hold against source.

  Conclusion: the audit backlog is fully executed as documented — Tiers 1–7 shipped and merged, Tier 8 (gated perf probes) intentionally deferred. No
  missing work found.
  
  
   grounded assessment of what's next, based on the verified state (Tiers 1–7 shipped, Tier 8 deferred, fleet now 3 devices, H/s parity
  earned).

  Where the project stands

  • Perf axis: exhausted on evidence at the JIT/codegen level (H/s parity with XMRig, every reordering/vectorization lever closed in the ledger).
    Remaining perf work is narrow and gated.
  • Quality axis: the 2026-08-09 audit backlog is fully executed — 3 data races, ownership, W^X-by-default, CI cross-build, diag logging all shipped.
  • Fleet: Lenovo (primary, fixed 765 MHz), Redmi 7A/pine (DVFS, stable 1708 MHz, ~80 H/s — now the best measurement platform), Unisoc SC9863A (A55, 6
    reachable cores, 3.86 GiB RAM — never benchmarked, only device capable of fast mode).

  Next / future — prioritized

  1. Fleet re-validation (Tier 8 #3) — pure measurement, no code risk, highest info-per-effort

  • Unisoc: first-ever benchmark + first fast-mode (full 2 GiB dataset) run on any device — exercises the dataset path that light mode has hidden
    since forever.
  • Verify count_top_frequency_cores() / sysfs cluster detection on Redmi (real DVFS → BigOnly now works) and Unisoc — core numbering is inverted
    across the fleet (Redmi fast=0,5,6,7; Unisoc fast=4–7; Lenovo fast=0–3).
  • Re-validate the E24 C*-pad and scheduler assumptions on A55 (different in-order uarch) and at 2.2× clock.

  2. LTO A/B (Tier 8 #1) — the single untried codegen probe

  • ~+1.9% upside per prior on-device measurement; crash risk on GCC-16+musl (revert, one session); KAT-gated. Best done on Redmi (thermal noise ≪
    Lenovo).

  3. Wire the on-device CI runner (T5-A's missing half)

  • device-kat job exists but SKIPs without a self-hosted aarch64-device runner. Registering Lenovo/Redmi as the runner turns the manual on-device
    gate into CI. Highest-value infra item — every future code change is gated by it.

  4. Close the small open-bug backlog (all pre-verified, tiny diffs)

  • --pool-test --tui skips cursor restore (std::_Exit(0) path) — documented OPEN-LOW.
  • Decide subscribe_try_: the 4-format mining.subscribe fallback is inert (never incremented) — either fix or delete; currently misleading.
  • Add tests for the three untested subsystems (TUI, metrics, TLS) — the TUI already shipped two device-caught bugs.

  5. Open-source prep (if publishing is intended — your call)

  • Remove the hardcoded third-party pool+wallet default in --pool-test (flagged as the release blocker; user-deferred so far).
  • License file, version tag, stale-branch cleanup (feature/dag-scheduler still on remote), secrets hygiene (already OK — devbox.json gitignored).

  6. Low-odds / optional perf

  • Superscalar timing-model redesign (the last untried A53-cost-model design) — be aware 2 of 3 designs already failed at the reference-hash gate;
    honest odds are low.
  • AES generator scalar XOR disassembly check, weighted-seed density census — micro-levers.

  What I would NOT do (evidence-closed, per the ledger)

  • Re-open JIT scheduling, dual-hash, hybrid fill, tier-2(b) vectorization, PGO, NEON AES — all measured negative or silicon-bound.
  • Chasing isolcpus/clock-pin as code — they're operational (and a real +14% / +1708 MHz, but your kernel/deploy call).

  My recommendation: start with #1 (fleet re-validation) — it's zero-risk, unblocks fast-mode testing, and gives the first A55/E24 data — then #2
  (LTO) in the same device sessions, and #4 as cheap desktop work. #3 pays off before any further code changes.
  
  Everything in the archive is either closed-on-evidence or already on my list — but scanning it against the current fleet (3 devices, different
  uarch/clock) surfaces a few angles that are genuinely new, plus honest leftovers. Here's the full picture:

  Genuinely new ideas (not in any doc)

  1. Startup JIT autotune — self-tuning constants per device ⭐ (my top pick) Every empirical constant was tuned on one device: the E24 C*-pad (3-
  instr form), the scheduler window, swap budget, dual-issue alignment. Those ship blindly to Redmi (A53 @ 1708 MHz, 2.2× clock) and Unisoc (A55 — a
  different in-order uarch). Idea: a ~1–2 s on-first-start microbenchmark that A/Bs 2–3 variants (pad width, scheduler window) and caches the winner
  (~/.armrx/autotune.json). The machinery is cheap — the pad variant already exists (ARMRX_NO_E24_PAD), and timing 500 hashes of one program is a
  solved problem in bench_armrx. Low risk, fully portable, and it converts "tuned for Lenovo@765" into "tuned for whatever it's running on." 2.
  Cluster-aware JIT emission variants — Lenovo's weak cluster (cores 4–7) loses ~50% throughput to interconnect arbitration. Nobody has tried emitting
  a weak-cluster variant (e.g., extra padding / different interleave for contended cores) for workers pinned there, while fast-cluster workers get the
  current codegen. The E24 pad exists precisely because interlock stalls hurt on A53 — the arbitration adds a different stall class. Testable purely
  with taskset A/B + --perf-ready. Medium risk, real potential on the 8w number. 3. PRFM on the fast-mode dataset reads — the T2-1 PRFM experiment
  (measured regression) targeted scratchpad LDRs in light mode (L1/L2-class, hint issued in the same window as the dependent load). Fast-mode dataset
  reads are DRAM-class — a completely different latency profile, and fast mode has never been run on any device (Unisoc is the first capable host). A
  PRFM PLDL1KEEP/PLDL2 hint ahead of dataset-item loads is untried and may behave differently. Zero-risk to measure, and it's gated on the Tier-8
  fast-mode bring-up anyway. 4. Cross-worker superscalar JIT dedup at seed rotation — all 8 workers compile the identical per-seed superscalar
  programs at each job rotation. A shared read-only compiled-program cache (one worker compiles, others map the same buffer) cuts rotation latency ~8×
  and removes 8 redundant compile bursts. Steady-state gain is small (compile is 1.76%), but rotation latency is a real UX/pool item and the change is
  low-risk (the compiled buffer is already immutable post-emission). 5. Live per-worker H/s health rebalancing — Redmi has real DVFS + thermal; a
  worker can silently throttle while others don't. The miner already measures per-worker H/s (--perf-ready/live Speed). An optional loop that detects
  a stuck worker and re-pins it to a cooler core (or swaps its affinity slot) is new, Redmi-specific, and testable — but operational-flavored, so low
  priority.

  Documented but never actually tried (honest leftovers)

  • IMUL_RCP register pre-assignment bisect (mid-high-risk #1) — first attempt diverged JIT/interpreter, mechanism unidentified; the bisection plan
    was never executed. One saved LDR per IMUL_RCP in the superscalar body (80% of instructions).
  • BOLT (post-link binary layout) — never attempted; toolchain-level, would need a non-musl build to be practical.
  • Weighted-seed opcode census (extends the single-seed 2026-08-09 disassembly) — pure measurement.

  Explicitly NOT worth proposing (closed with evidence)

  • PRFM on scratchpad (regression), SVE/SVE2 (no such hardware), LSE/outline-atomics (A53/A55 are ARMv8.0 — no LSE), Adreno/OpenCL offload (A53-class
    GPU, out of a CPU miner's scope), kernel module for JIT buffers (out of scope), NEON AES retries (closed), hybrid-fill variants (3 designs
    failed), superscalar timing-model redesign (2 of 3 designs already died at the reference-hash gate).

     Here's the re-audit of the "closed on evidence" levers — verdict per lever, and where the closures were actually wrong or
  overreaching.

  Verdict summary

  Lever                     │ Closure quality                                       │ Verdict
  ──────────────────────────┼───────────────────────────────────────────────────────┼────────────────────────────────────────────────────────────────
  PRFM scratchpad hints     │ Sound (2 designs, mechanism-level)                    │ ✅ Correctly closed
  (T2-1)                    │                                                       │
  CBRANCH CSEL              │ Best-in-class (premise falsified: 2.4% not 31%)       │ ✅ Correctly closed
  -mtune=cortex-a53         │ Measured null (E18, clean A/B) — and already ships in │ ✅ Correctly closed (the 08-06 brainstorm proposal was based on
                            │ the toolchain                                         │ a false premise)
  PGO                       │ Measured null twice incl. over-training control       │ ✅ Correctly closed
  Argon2 copy-elimination   │ Exemplar methodology (instr ↓2.86%, cycles ↑0.35% →   │ ✅ Correctly closed
                            │ reverted)                                             │
  Dual-hash interleaving    │ Binary premise, conclusive                            │ ✅ Correctly closed
  D2 pipelining             │ Not a closure (code live, E2E never A/B'd)            │ ⚠️ Unmeasured, not closed
  Tier 2(b) per-family (3   │ Family rule respected; branches verified unmerged     │ 🟡 Correct but Design B closed on an implementation bug, not a
  designs)                  │                                                       │ measured negative
  FDIV/FSQRT                │ Weakest measurement (single-pair, PGO-era) + latent   │ 🔴 Closure defensible, evidence thin
                            │ correctness bug                                       │
  DAG scheduler             │ Root cause wrong in 2 docs; family closure            │ 🔴 Reopenable
                            │ overreaches                                           │

  The two real findings

  1. 🔴 Hybrid --dataset-mb>0 correctness — the repo contradicts itself, on the recommended config. Three records in the tree, all at HEAD:

  • Commit 3272735c (08-05): message says "Two coupled fixes that make the hybrid partial-dataset path correct on-device" — but the same commit's test
    comment (tests/test_mining.cpp:443-445) says the hybrid consumption path "is currently NOT producing correct end-to-end hashes on-device (a single
    non-rotated job with --dataset-mb>0 is also wrong vs a light reference)".
  • README:167 (08-06 reconciliation): "✅ Correct and recommended as of 2026-08-07... verified on-device same-nonce" — and references a verification
    that does not exist in the tree (no same-nonce end-to-end KAT; the test deliberately bypasses the engine hash and only byte-compares generated
    items).

  Both cannot be true, and --dataset-mb=512 is the README's recommended real-world config. The pool would reject wrong shares, so this is either fine-
  but-undocumented or a live wrong-hash bug. This is the highest-value follow-up I found: write the missing same-nonce end-to-end KAT (hash with --
  dataset-mb>0 vs light reference, same nonce) and settle it on-device.

  2. 🔴 DAG scheduler closure overreaches + stale wrong root cause. The brief (docs/briefs/2026-08-06-dag-scheduler-attempt.md:13) and ROADMAP.md both
  still state "reorder forces a longer emitted sequence" — which the 2026-08-07 correction (changelogs.md:418,429) proved wrong: emitted length is
  order-invariant; the −14.3% was per-hash planner runtime (O(n²) hazard matrix + ~9 heap allocs/hash, sys time tripled). So "JIT scheduler reordering
  is exhaustively closed" is stronger than the data: the DAG concept was never isolated from its implementation cost. A cheap planner (bitmap hazard
  matrix, schedule reuse) is a legitimate, documented-reopening variant — small expected ROI (E20's 0.73% legal-move rate), but the closure label
  overstates the evidence.

  The latent bug I confirmed in the "opt-in" code

  FDIV/FSQRT NR path clobbers live literal registers (independent audit's claim, now verified in code): emitMovImmediate's 32-bit immediate path reads
  literals from NEON v0–v15 via smov/umov (jit_compiler_a64.cpp:1462-1466), but h_FDIV_M/h_FSQRT_R use v0–v2 as scratch (:2074-2118) with a comment
  claiming they're "safe — not used by other instruction handlers" — which is wrong. Flipping ARMRX_ENABLE_JIT_FAST_DIV_SQRT=ON can silently produce
  wrong hashes. The flag is OFF by default, so shipping is unaffected — but the "kept as opt-in" posture is unsafe, and the perf closure itself rests
  on a single-pair PGO-era A/B. Either fix the register allocation or delete the path.

  Could-have-done-differently (per lever)

  • Tier 2(b) Design B — the only variant attacking the actual bottleneck (scalar multiplies) died on a vuzp/carry bug, not a measured perf result;
    the corrected limb-decomposition recipe was explicitly known but never attempted. Closure rests on the physics argument (2-wide saturates A53
    dual-issue), which is plausible but the raw fill-time deltas (164.7 vs 163.3 s gate) are all single runs inside thermal noise.
  • Window-widening adoption (the adopted sibling) — weakest protocol in the set: single pre-change baseline, no interleaving/md5; +0.156% IPC is at
    the condition-spread noise floor. Only a re-measure concern, not a closure.
  • Ledger hygiene — the "authoritative" ledger has no rows for PGO / FDIV/FSQRT / -mtune (they live in perf-tracking.md), and the PRFM row records
    only 1 of 2 designs tried. Minor.
  • Correctly done (worth noting): *_M scheduler (instrument ARMRX_MAX_SWAPS kept for exactly the reopening case, honest "Maybe" on the ledger),
    tier2b family rule respected with evidence branches retained, CBRANCH premise falsification, E18 -mtune isolation.

  What I'd do with this

  1. Resolve the hybrid contradiction — one device session, same-nonce end-to-end KAT. If wrong: --dataset-mb silently mines rejected shares; if
     right: delete the stale test comment and add the KAT permanently.
  2. Fix or delete the NR FDIV/FSQRT path (register allocation or removal) so the "opt-in" flag is safe.
  3. Correct the DAG root cause in the brief + ROADMAP and downgrade "exhaustively closed" to "this implementation failed" in the ledger.
