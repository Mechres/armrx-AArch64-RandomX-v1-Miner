 the audit gave me enough evidence to be specific. If I could re-run this project's history
  with one steering wheel, here's what I'd do differently, in order of how much I believe it would have
  changed the outcome:

  1. Never accept a mechanism that contradicts the code you wrote

  The DAG regression is the poster child: the changelog's root cause ("the reorder forces a longer
  emitted sequence") is contradicted by the code (per-opcode emission length is order-invariant) and by
  the data already sitting in the logs (sys time tripled = heap churn, not emission). The team spent
  extra runs and wrote the 20-min settle plan to re-verify a misdiagnosis. The rule I'd adopt: when a
  measurement disagrees with the code model, the measurement is telling you something about your model —
  bisect with perf until the story is consistent, or don't write a root cause. This would have also
  caught the "emitted +13.6M instructions" phrasing for what it was: the planner executing, not code
  being emitted.

  2. Lock the measurement harness before the first optimization — and treat a known pitfall as a hard
error

  The contaminated-divisor artifact (89.47M) that inflated AES into "the largest win in history" happened
  twice, even though TESTING.md had already flagged it as "the project's own flagged pitfall." A known
  pitfall that still bites is a process failure. I'd have made the gated 500-hash --perf-ready window the
  only number a claim may cite, wired into a committed measurements/ DB with the raw logs beside it, and
  added a CI check that refuses any perf write-up whose math doesn't divide by exactly 500. The B-M-B-M
  discipline was right; the harness enforcement was missing.

  3. Root-cause the W3-2 divergence before the next scheduler experiment

  The *_M scheduler attempt diverged JIT/interpreter and was reverted with "mechanism not identified" —
  then E26, DAG, and the TUI+DAG segfault all orbited the same unexplained hazard-model hole. I'd have
  spent the first differential-fuzzer session (interpreter as oracle, ASAN, seed mutation) closing that
  hole before touching the scheduler again. Every later experiment inherited the uncertainty and cost
  more than the fuzzer would have. This is the one place where short-term patience buys long-term speed.

  4. Fix the trivial correctness bugs first — they're contaminating the perf work

  The dangling string_view UAF in the TUI (my D1) is a one-line fix that a single ASAN build or code
  review would have caught. Instead it's been live, producing "TUI garbage" and muddying the DAG+TUI
  segfault attribution (the docs blamed "DAG emission order" for what may partly be this UAF). Three OPEN
  bugs are all in TUI/signal/teardown — non-perf paths that were never prioritized because the project is
  perf-first. I'd flip it: correctness debt is compounding interest; a TUI that doesn't segfault makes
  every future on-device observation trustworthy, which the perf program needs more than +1%.

  5. Keep only one source of truth for "what's true at HEAD"

  README still ships the retracted AES "−16.7%, below XMRig" claim while STRATEGY.md says −5.8% and ~7%
  heavier. The cpool header comment describes machinery that no longer exists; the changelog says it was
  "removed." When docs disagree with each other, every future agent (or the user, asleep) must burn time
  deciding which is stale. I'd have made the correction commits update all top-level docs in one pass
  (the 2026-08-06 correction did update four docs — but missed README), and deleted gated negative
  results (DAG, dead cpool machinery) instead of keeping them in-tree, because git history preserves them
  and dead code only rots the reader's model.

  6. Measure the environment before optimizing against it

  The clock story (1.2 GHz believed → 763-765 MHz fixed, no cpufreq) changed the meaning of every early
  baseline, and the 60 °C memory cliff (discovered late, via --pool-test) retroactively confounded
  E15/E16 absolute numbers. Both were environmental facts discoverable in an afternoon of reading /sys
  and thermal zones at the start. I'd have spent day one on: clock, thermal envelope, cluster topology,
  memory bandwidth under contention — then optimized against that picture, not against a guessed one.

  ───────────────────────────────────────────────────────────────────────────────────────────────────────

  The one-sentence version: the project's measurement and gate discipline is genuinely good — but I'd
  have enforced the harness as the only number source, fuzzed the hazard model before building more on
  it, fixed the trivial UAFs before chasing IPC, and never let a root cause ship that the code
  contradicted. Most of the "perf" spent was actually spent re-paying uncertainty debt that earlier
  process would have retired cheaply.

