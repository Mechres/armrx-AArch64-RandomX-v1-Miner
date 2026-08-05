
  Here are out-of-the-box directions for armrx, deliberately not on the closed-lever map (no JIT-emission
  reordering, no cluster pinning, no -mtune). Grouped by how far they go from the current "codegen
  optimization" frame. Speculative claims are marked — verify before building on them.

  A. Correctness & knowledge plays (fits the project's DNA, highest leverage)

  1. Differential-fuzz the JIT scheduler. The one OPEN correctness mystery is the W3-2 class: a memory-op
     reorder that diverged JIT/interpreter and was never root-caused ("mechanism not identified"). That
     is precisely what a libFuzzer harness with the interpreter as oracle is built to find: mutate
     RandomX programs/seeds, run JIT vs interpreter under ASAN, minimize. This could finally explain the
     W3-2 trap, the DAG+TUI segfault (bug a), and any latent hazard-model hole — one harness, three
     mysteries. The 450/200 stress suites are static; a fuzzer makes the gate continuous.
  2. Runtime scheduler-invariant assertion (debug builds only). In the compile path, re-derive the
     emitted order from the hazard model and validate CBRANCH replay geometry per hash
     (reg_changed_offset vs the anchor set). Catches future scheduler changes on the host before a 20–35-
     minute device stress run is spent. Cheap, and it turns the "trust the gates" discipline into "trust
     the invariant".
  3. Reconcile the "41% pool-mode idle" anomaly (TESTING.md:31). Docs say pool workers idle ~41% waiting
     on stratum — yet the 8w pool number (26.65 H/s) exceeds the last 8w bench (24.95). Either the doc is
     stale (then TESTING.md's measurement warning is misleading), or there's real pipeline idle that a
     non-codegen change (job prefetch, nonce batching) could reclaim — potentially the single biggest
     hidden lever in the project. 30 minutes of investigation; no build needed.

  B. Workload/pipeline levers (different dimension from codegen)

  1. Stop the seed-change init from stealing worker-0 cycles. On seed change, set_job runs the ~7.5 s
     Argon2d cache init synchronously on the reader thread (verified pattern in mining_engine.cpp:219-
     310; workers keep hashing the old job — good). The residual waste: the reader thread lands on
     whatever core the OS gives it, likely worker 0's — pin the init to a weak-cluster core or a
     dedicated housekeeping core (the isolcpus knowledge already tells you which). Expected low single-
     digit % at 8w pool, zero correctness risk.
  2. Thermal-cliff governor. --pool-test discovered a hard 60 °C memory-throughput cliff (4.7× collapse:
     3.19 → 0.68 H/s). Out of the box: a governor that reads /sys thermal zones and pre-emptively reduces
     workers / increases stagger at ~55-58 °C instead of riding into the cliff and back. Turns a
     catastrophic nonlinearity into a smooth tradeoff; "eco mode" (mine only while charging + idle +
     under temp) is the phone-product version.
  3. Dynamic worker-count auto-tuner. The marginal H/s of weak-cluster workers is known-low (0.53×); a
     runtime hill-climber over worker count × stagger × thermal state maximizes sustained H/s instead of
     assuming 8 is always best. Different from closed "cluster pinning" (which was about placement, not
     count).

  C. Product/ecosystem angles

  1. Ship the AArch64 JIT as a standalone library. A verified, clean-room, W^X-correct RandomX JIT for
     AArch64 is genuinely rare — XMRig has no ARM JIT (interpreter fallback). armrx_core already is one;
     export a stable API and it becomes useful to wallet validators, exchange share-checkers, and future
     PoW tools. The project's KAT + differential-gate discipline is the moat.
  2. "RandomX share validation as a service." Exchanges/services that accept Monero need to validate
     shares against the 2 GiB dataset; armrx's light-mode cache works with 256 MiB. A small HTTP
     validator built on armrx_core is a real, useful product that also dogfoods the library idea.
  3. The recycled-phone mining cluster. armrx is the phone-SoC miner. Package it: systemd/daemon mode,
     fleet Prometheus metrics (the endpoint already exists), single static musl binary, Alpine/Termux
     builds (the 2nd device is already Termux). Budget-sensitive student + 2 devices + cheap old Androids
     = a genuinely novel deployment story ("mine on e-waste").
  4. Automated nightly device CI. The docs' history is full of stale-baseline traps (contaminated
     divisors, burst-vs-sustained isolcpus numbers, clock-jump builds). A cron'd cross-build → B-M-B-M →
     committed measurement DB on the device eliminates the entire class. Not flashy, but every future
     decision gets honest numbers.

  D. Longer-horizon tech

  1. Silicon-adaptive codegen dispatch. The project's own history shows AES paths vary by silicon (T-
     table vs AESE funnel vs NEON-TTABLE: +28.8% primitive but +1.68% E2E, and the funnel is byte-
     identical). A startup microbenchmark that picks among pre-compiled variants (one binary, runtime
     select) future-proofs armrx across the user's two devices and any ARMv8-variant successors. Standard
     -march=armv8-a+crypto is the floor; newer cores have headroom this project never tested.
  2. Position the repo as a memory-hard-PoW JIT toolkit, not a miner. RandomX v1 will eventually be
     replaced by Monero's next PoW; the durable assets are the clean-room interpreter+JIT pair, the
     differential-gate methodology, and the gated measurement harness. Keeping the VM core algorithm-
     agnostic (the V2 flag already exists) means the next Monero PoW transition costs a port, not a
     rewrite — and the "conformance implementation" angle (armrx as the reference AArch64 RandomX
     implementation, feeding findings back to RandomX/XMRig) gives the project academic credibility
     beyond hashrate.
  3. H/s-per-watt as a first-class metric. On a phone SoC with a thermal cliff, energy efficiency is the
     differentiator vs XMRig. Publishing an honest W-measured efficiency table (the battery sysfs exists)
     changes the project's frame from "95% of XMRig" to "the mobile-mining reference".

  Deliberately excluded: anything reopening JIT emission reordering, cluster pinning, -mtune, PGO, PRFM —
  all closed with evidence, and the audit's lever analysis found only multi-worker bench_armrx and cross-
  toolchain LTO worth sessions.
