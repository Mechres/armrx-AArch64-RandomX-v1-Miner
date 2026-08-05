I’d frame them around the project’s real bottleneck profile:
not more JIT emission reordering
not more “generic ARM tuning”
instead: reduce work per hash, reduce memory stalls per hash, or change execution placement/shape
The most interesting “out of the box” performance ideas
1. Cluster-aware, asymmetric worker counts
Instead of always running 8 workers:
run 4 workers on the fast cluster
optionally 1-2 workers on the slow cluster
leave housekeeping/system work breathing room
Why this is promising:
the device is asymmetric even though both clusters are A53
more workers does not mean more useful throughput
6 well-placed workers may beat 8 poorly placed ones
This is a very practical performance idea and much more likely to pay off than another compiler flag.
2. Worker count as a thermal controller
Right now thermal behavior is mostly observed, not actively managed.
Idea:
monitor temperature
if temperature crosses a threshold, reduce worker count or introduce stagger
if it cools, restore workers
Why:
on this class of device, sustained throughput can be better with fewer hot workers
this turns “thermal problem” into “performance policy”
This is especially useful for embedded hardware where the nominal max is not the sustained max.
3. A “benchmark-to-policy” autotuner
Add a startup or on-demand autotune mode that tries a few safe configs and picks the best:
worker count: 1, 2, 4, 6, 8
stagger: 0 / small / medium
affinity: all / fast-cluster-only / unpinned
dataset mode if applicable
Then store the best config per device.
Why:
the project already discovered that topology and contention dominate
manual tuning is expensive and error-prone
automated search may find a better steady-state point than intuition
This is one of the highest-leverage ideas because it converts tribal knowledge into repeatable policy.
4. Reduce per-hash allocation/copies in the mining engine hot path
One concrete idea I found in source:
worker_loop() allocates/copies a fresh std::vector<std::byte> next_block each pipelined hash
A persistent worker-local buffer could remove that repeated allocation/copy.
Expected ROI:
probably small
but cheap to test
not a consensus/JIT change
easy to gate and measure
This is a good “boring but real” candidate.
5. Batch pool/share overhead separately from hashing
Pool logic and share submission are currently intertwined with the run loop and logging paths.
Ideas:
queue shares from workers into a dedicated submission thread
decouple network I/O from the hash loop more aggressively
reduce lock traffic around share reporting
Why:
the miner should never lose hash time to pool chatter
on weak devices, tiny synchronization overheads can become visible
This is more of a throughput hygiene idea than a magic win.
6. Split “measurement mode” from “mining mode” more aggressively
The project has accumulated many features that may cost time or branches in production paths.
Out-of-the-box idea:
make a build or runtime mode with:
no metrics
no TUI
no logging in steady state
no optional instrumentation hooks
minimal share path overhead
A stripped “race mode” could expose how much the support code actually costs.
This is useful even if the answer is “not much,” because it cleanly bounds overhead.
7. Explore memory placement, not instruction placement
If the residual gap is memory behavior, the next lever may be:
where buffers live
how they are mapped
which cores touch which memory
whether scratchpads/datasets should be partitioned by cluster
Concrete experiments:
pin memory allocation policy to a cluster
isolate fast-cluster scratchpads from slow-cluster traffic
use worker-local arenas more aggressively
test whether hugepage behavior differs per cluster under contention
This is a much better direction than more JIT reshuffling.
8. Adaptive dataset-prefix sizing
If partial dataset mode matters, don’t treat the prefix size as fixed.
Idea:
grow the cached dataset prefix only while it improves effective H/s
pause fill when contention worsens
shrink expectations when thermals or memory pressure rise
In other words:
dataset caching as a feedback-controlled optimization, not a static feature
This could be a real edge-device win.
9. Measure “hash rate per watt” as an optimization target
On embedded devices, the best raw H/s is not always the best operating point.
Idea:
track H/s alongside temperature and estimated power proxy
optimize for sustained H/s per degree or per watt-like proxy
This could lead to a different best config than “max workers, max speed”.
It’s unconventional, but appropriate for this hardware.
10. Cooperative yield strategy for non-mining threads
The miner already has pool I/O, metrics, logging, and fill threads competing with workers.
A simple idea:
run non-mining threads at much lower priority
keep them off fast cores
isolate them to housekeeping core(s)
reduce their wakeup frequency
This is not glamorous, but on a weak shared-memory device it can matter.
What I would not prioritize
These are tempting, but the evidence says they’re weak:
more JIT reordering
more “peephole” emission changes
another -mtune retry
more density tricks in the same style as the closed experiments
Those lanes look exhausted.
My ranked shortlist
If I had to pick the best next experiments:
Cluster-aware worker placement/count
Autotuned worker/stagger/affinity search
Thermal-aware worker reduction
Worker-local buffer reuse in worker_loop()
Memory placement/contention experiments by cluster
If you want, I can turn this into a concrete ranked list with:
expected ROI
risk
difficulty
required measurements
whether it’s device-only or cross-device safe
