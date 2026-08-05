Here are “out of the box” directions I’d consider, ranked by leverage and how well they fit the project’s evidence-driven constraints.
1. Productize the project around embedded RandomX fleet mining
The strongest differentiator is already there: it runs on weak, memory-constrained ARM devices where XMRig is awkward or inefficient. Lean into that.
Ideas:
- Single static binary + one config file for cheap Android/Termux/postmarketOS devices
- Watchdog mode: auto-restart on pool disconnects, thermal slowdowns, or memory pressure
- Thermal governor: reduce worker count or stagger workers when temperature rises
- Battery-aware mode: mine only when charging / above a battery threshold
- Mobile-first dashboard: simple web UI instead of terminal TUI
Why this is promising:
- The project already knows how to survive low RAM, fixed clocks, weak clusters, and flaky devices.
- Competing with XMRig on desktop is hard; beating it on edge devices is much more plausible.
2. Build a device-aware scheduler, not just a JIT
Right now the miner mostly discovers CPUs and pins workers. A bigger win may come from making the miner understand the device’s runtime behavior:
- detect fast vs slow clusters by a short calibration hash burst
- detect thermal throttling onset
- detect memory bandwidth contention
- adapt worker count dynamically instead of fixed 8/7/1 configurations
This turns the miner into a closed-loop system:
- measure
- place workers
- throttle
- rebalance
That’s more defensible than trying another emission reorder.
3. Add a cluster/thermal benchmark mode as a first-class tool
A diagnostic binary or subcommand that answers:
- which cores are fast?
- which cores share L2 / interconnect?
- what is the safe sustained worker count?
- where does memory bandwidth fall off?
This could be:
- armrx --probe
- outputs JSON
- used by CI, device onboarding, and future scheduler logic
Why it matters:
- The project repeatedly discovers topology quirks by hand.
- A built-in probe would make future tuning automatic.
4. Create a fleet benchmark / regression ledger
Instead of ad hoc logs and markdown measurements, keep a structured record of runs:
- device model
- kernel
- compiler
- CPU topology
- temperature
- worker count
- hash rate
- IPC / cycles / instructions
Then track whether a change helps or hurts across device classes.
This is not glamorous, but it’s probably the highest-value infrastructure upgrade if the project keeps evolving.
5. Add a safe degraded mode for correctness under stress
The project has several failure classes around timing, threads, and TUI. A useful feature would be a deliberate “safe mode”:
- fewer workers
- no TUI
- no metrics server
- reduced memory mode
- strict logging
- conservative pool reconnect
Think of it as:
- armrx --safe-mode
Why:
- It gives operators a fallback when a device is flaky.
- It also simplifies debugging bug reports.
6. Make the TUI a separate process or a read-only consumer
The current TUI is coupled too tightly to the miner’s stdout and lifecycle. A cleaner design:
- miner emits structured status to a local pipe/socket/file
- TUI reads snapshots only
- TUI can crash without taking the miner down
Benefits:
- fixes a whole class of rendering bugs
- enables richer UIs later
- keeps the hot path simpler
This is a strong architectural move, not just a bug fix.
7. Add remote observability beyond Prometheus
Prometheus metrics exist, but this project is the kind that benefits from richer operational visibility:
- per-worker hash-rate history
- reconnect counts
- accepted/rejected share timeline
- thermal trend
- cluster placement history
- last N log lines
A tiny local HTTP dashboard or JSON endpoint could be enough.
8. Explore dataset residency strategies more aggressively
Since light mode dominates, the interesting question is how much of the dataset should be cached locally and how it should be managed.
Ideas:
- adaptive partial dataset sizing based on RAM/thermals
- opportunistic background fill when idle
- pause fill when mining rate drops
- reserve memory explicitly for dataset prefix growth
- use a “hot prefix” strategy if reuse patterns support it
This is more innovative than another JIT tweak and fits the project’s memory-latency reality.
9. Build a multi-device orchestrator
If the user has one ARM box now and another coming, the natural next step is orchestrating several devices:
- one controller
- multiple workers
- aggregate shares
- per-device health
- per-device benchmark normalization
- automatic config rollout
This turns armrx from “a miner” into “a mining appliance”.
10. Try a cluster-aware process model instead of threads
Threads are convenient, but a process-per-worker model can be better on odd devices:
- isolate crashes
- avoid one bad worker poisoning the whole process
- simplify signal handling
- pin processes to clusters explicitly
- restart individual workers on failure
Downside:
- more IPC complexity
- harder shared dataset management
Still, for a fragile embedded target, it may be worth evaluating.
11. Add a profile database for known devices
Instead of guessing flags per device:
- detect model/SoC
- load a profile:
- worker count
- affinity
- stagger
- TUI on/off
- metrics on/off
- expected sustained H/s
- thermal limits
This makes the project much easier to deploy and much less brittle.
12. Package it as a library + CLI, not just a CLI
The mining core could become reusable:
- armrx_core as a clean library
- CLI as a thin wrapper
- future UIs, orchestrators, and test harnesses built on top
This would make the codebase easier to evolve and reduce the temptation to keep adding modes directly into main.cpp.
13. Add mutation/fuzzing for pool protocol parsing
The stratum/client code is doing a lot of hand-rolled parsing. A low-risk, high-value idea:
- fuzz JSON/line parsing
- fuzz reconnect logic
- fuzz partial responses
- fuzz malformed job notifications
This doesn’t improve hash rate, but it hardens the project against weird pool behavior and edge cases.
14. Consider share-lottery / solo-mining modes carefully
This is more speculative, but if the project wants a distinctive feature set:
- local lottery mining against a node
- educational “solo RandomX appliance” mode
- share accounting for long-tail mining
Not necessarily high priority, but it is a real differentiator from generic pool miners.
15. Use AI-assisted log triage internally, not as a product gimmick
A practical idea: make benchmark and failure logs machine-consumable enough that a script can classify them:
- thermal throttle
- pool disconnect
- TUI issue
- memory pressure
- scheduler contention
- invalid job
That would save a lot of manual debugging time.
My honest top 5
If I had to pick the best future directions:
1. Device-aware scheduler
2. Cluster/thermal probe mode
3. TUI decoupling into a snapshot consumer
4. Fleet benchmark ledger
5. Safe degraded mode
