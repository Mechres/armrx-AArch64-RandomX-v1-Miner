 These are opinions informed by the code, not verdicts; I’ve labeled what’s proven vs judgment.
1. I would never let the TUI share std::cout with the miner
This one I’d call a design mistake, not just a bug.
The current architecture has at least four writers racing on one stream:
TUI frames (src/tui.cpp:157-160)
share callbacks (src/miner_app.cpp:384-390)
the logger sink (include/armrx/log.hpp:112-117)
pool status lines (src/miner_app.cpp:465-485)
And there’s already a built-in escape hatch — log::set_tui_mode() + the ring buffer (include/armrx/log.hpp:37-117) — that nobody ever calls. So the project built the right tool and didn’t wire it up.
What I’d do differently:
miner core emits a structured status snapshot (plain data, maybe JSON or a struct over a pipe/ring)
TUI is a pure consumer that renders snapshots
logs go to the ring buffer, always, when TUI is active
nothing but the TUI ever writes the terminal while TUI is on
Why it’s better:
kills the entire class of “garbage bytes / overlapping lines / segfault-looking artifacts” bugs at the root, instead of patching each symptom (the dangling string_view I found is one symptom; the unsynchronized writes are another)
the TUI can crash or be buggy without ever touching the mining process
makes a future remote/web UI trivial — same snapshot stream, different renderer
The project keeps treating TUI bugs as rendering bugs. They’re actually an architecture bug: presentation owns the same channel as production.
2. I would replace the fixed “8 workers, one policy” model with a calibrate-then-commit model
Judgment call, but well-grounded in your own measurements.
The project has repeatedly rediscovered the same lesson manually: MSM8929’s two clusters are same-frequency but ~2× different under contention, isolcpus gave +14%, and worker count/placement moved more hash rate than any code change ever did. Yet the shipped behavior is still essentially “detect cores by frequency, pin 1:1, hope.” And on this device frequency detection provably can’t work — count_top_frequency_cores() returns 8 because all cores report the same max freq (src/mining_engine.cpp:144-169).
What I’d do differently:
on startup (or --probe), run a ~2-second calibration burst per core and per cluster, measure actual per-core H/s
build the worker plan from measured capacity, not sysfs guesses
commit to asymmetric plans: e.g., 4 workers on the fast cluster, maybe 2 on the slow, not 8
re-probe on thermal events
Why it’s better:
converts the project’s biggest known win (placement) from an operational accident (isolcpus boot flags) into a shipped feature
every future device gets tuned automatically instead of needing a human to rediscover its topology
based on your own E15/isolcpus data, this is plausibly worth 10–30% at 8w — an order of magnitude more than any remaining code-level lever
The project optimized instructions for months when the largest measured wins were all placement/toolchain. I’d have built the autotuner before the fifth emission experiment.
3. I would enforce a strict “hot path / support path” separation
Softer opinion — the code works, but it accreted.
Over time, measurement hooks, TUI, metrics, partial-dataset fill, JIT profiling, and logging all grew into the mining engine and run loops. Examples:
per-hash std::vector allocation in the pipelined path (src/mining_engine.cpp:583-585)
share callbacks doing string formatting + hex conversion inline in worker threads
PartialDataset fill logic whose publish boundary has the out-of-order correctness bug I flagged — itself a symptom of fill being bolted onto the engine rather than staged as a separate component with a clean handoff
What I’d do differently:
worker hot loop: hash, check generation counter, buffer shares, flush counters — nothing else
everything else (share formatting/submission, fill, metrics, logging) lives in dedicated threads fed by queues
a documented invariant: no allocation, no I/O, no locks in the per-hash path
Why it’s better:
the hot path stays auditable and its instruction count stays explainable — which matters enormously for a project whose whole methodology is per-hash accounting
prevents the slow drift where each feature adds “just a little” overhead to the critical loop until nobody knows what the 1.8% C++ share actually is
bugs like the PartialDataset publish bug become structurally harder to write
What I would not do differently
To be fair to the project:
The JIT work itself was done correctly. The hazard modeling, the stress gates, the disciplined revert-on-failure culture, and the honest negative-result documentation are genuinely good engineering. The DAG closure was the right call.
The cross-build + one-test-per-session discipline is right for this device and was learned the hard way for good reason.
Chasing instruction count vs XMRig was worth doing once, and E24 (+7.1%) was a real win found exactly by that route.
My criticism is not “the project optimized wrong things badly” — it’s that the ordering was inverted: the cheapest, largest structural wins (decouple TUI, calibrate placement, protect the hot path) were available earlier and would have made several of the painful bug hunts and measurement confounds unnecessary.
If I had to pick just one
#1 — decouple the TUI. It’s the smallest change with the largest correctness payoff, it removes two of your three OPEN bugs at the root, and it’s a precondition for the project ever having a UI that doesn’t fight the miner. Placement autotuning (#2) is the biggest performance win, but #1 is the best engineering one.
