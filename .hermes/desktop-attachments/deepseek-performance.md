 Performance-specific, out-of-the-box — staying off every closed lever (reordering, cluster pinning, -
  mtune, PGO, PRFM, hugepages, AES). Evidence-framed, hypothesis-labelled. The numbers come from the
  region census (superscalar ≈ 75% of cycles at IPC 0.80; main-VM ≈ 18% at IPC 0.405; AES ≈ 7%).

  1. Dual-hash interleaving — the one big untried code lever (~+15-30% potential)

  The idea: run two independent VMs (two nonces) per worker thread, with their JIT emissions interleaved
  at instruction granularity. While hash A stalls on a multiply latency or a scratchpad load, hash B's
  independent instructions issue into the A53's spare slots.

  Why the evidence says it could work (HYPOTHESIS, backed):

  • E24 proved the superscalar region is multiply-latency-bound with spare issue slots, not throughput-
    bound: adding 3-instr MOVZ padding between muls cut other_interlock_stall 23.3M → 6.18M/hash and won
    +7.1%. The gaps it filled are mul-latency bubbles — and a second, independent instruction stream is
    exactly what fills those bubbles without needing intra-program reordering (which is the closed, W3-2-
    poisoned family).
  • Superscalar IPC is 0.80; the A53 is 2-wide in-order. Two interleaved chains plausibly reach IPC ~1.2-
    1.5 on the dominant 75%-of-cycles region → ~30-40% fewer cycles there ≈ +20-30% total H/s before
    overheads. Register pressure is the risk: two VMs' data regs (~16 live) + addressing/scratch fits 31
    GPRs, tightly.
  • Why it's NOT closed despite appearances: T2-2 was alignment within one program; D2 (Track D2) only
    overlaps the AES/scratchpad-fill (≈0.34% E2E, it's the 1.4% AES portion, not the 86% VM chain); E26
    tried to hoist loads within one hash and diverged. This idea changes no intra-program order — each
    hash's emission stays identical to today's (byte-identical vs interpreter by construction), so the
    hazard-model class that killed W3-2/E26/DAG is structurally avoided. Correctness risk is "two
    independent streams must not clobber each other's registers" — testable with the existing 16/450/200
    gate suite extended 2-way.
  • Honest caveat: if the A53's multiply pipe turns out to be the hard throughput limit (1 IMUL per 2+
    cycles and the derivation is ~35% muls), interleaving gains shrink to roughly nothing on superscalar
    and only the main-VM region (18% of cycles) benefits. That's the falsifiable question: measure mul-
    port throughput first (one -mcpu microbench, the f3-neon-mul-latency-test shape already exists in
    docs/experiments/f3-neon-mul-latency-test.md).

  This is the only idea I'd call genuinely "out of the box" in code that isn't in any closed list, and
  it's a mini-research project (weeks, not days).

  2. Unlock the rated clock via device-tree OPP table (~+40%+, the biggest possible win, hardware not
code)

  The SoC is rated 1.1-1.4 GHz and runs at a firmware-set 765 MHz — the DT carries no operating-points-
  v2/OPP table (CONFIG_CPUFREQ_DT is already y). Adding an OPP table + reflash is the documented way to
  reach ~1.1 GHz. That is ~44% more clock for the same instruction stream — bigger than every code change
  in project history combined. Risks are real: brick/recovery on reflash, and the 60 °C memory-throughput
  cliff (4.7× collapse, discovered via --pool-test) means the thermal budget must be solved first (idea
  3). This is the highest-ROI, highest-risk item on the list; it's the "mine at the rated speed" play.

  3. External cooling to stay under the 60 °C cliff (~avoids a 4.7× collapse, operational)

  The device is thermally self-throttling memory throughput even at fixed clock. A heatsink + fan on the
  board (or even a cool ambient) keeps it under the cliff — zero code, and it's a prerequisite for idea
  2. Cheap, no risk to the repo, measurable via --pool-test per-worker summary.

  4. Main-thread/worker-0 contention in pool mode (+2-4% at 8w, cheap)

  From my audit: the real restatement of the closed "cluster placement" lever. In pool mode the
  main/stratum thread shares core 0 with a pinned worker (isolcpus makes it worse); the fix is
  scheduling-only — deprioritize the main thread or lighten per-second printing. Explicitly left open by
  isolcpus-rt-priority-win.md. Low risk, measurable via the 8w real-pool A/B.

  5. Cross-toolchain LTO (+1-2%, cheap, never actually tested)

  My audit's finding: the "GCC + musl LTO crash" that closed this was root-caused and fixed in
  CMakeLists.txt:379-390 (fortify-headers vs LTO), and perf-tracking.md:569-571 says cross-GCC-16-LTO was
  never A/B'd. When LTO did link on-device it was +1.9%. One build; KATs gate it; revert is one flag.

  6. Clang cross-build A/B (never measured, likely small)

  perf-tracking.md:604: "Clang still untried." The JIT body is hand-emitted (untouched by the compiler),
  so gains are capped to the ~10% C++/AES-glue region — expect ≤1%, but it's a one-build probe with zero
  risk.

  7. Cross-worker derived-item sharing in light mode (architectural runner-up, marginal)

  The superscalar derivation is a pure function of (seed, item index), and 8 workers derive the same 2M
  items redundantly (16K random draws/hash each). A shared L2-resident item cache could hit ~5-6% of
  draws (128K draws/hash vs 2M items) ≈ maybe −3-5% total hash time. But: random access → poor locality,
  2-cluster coherence traffic is precisely this SoC's weak point, and the W3-2 history warns against
  memory-side changes. Expected ROI small, complexity high — listed only for completeness.

  ───────────────────────────────────────────────────────────────────────────────────────────────────────

  What I'd actually do in order: (1) the f3-style mul-throughput microbench to validate idea 1's premise
  (~1 hour, decisive either way) → (4) main-thread deprioritization → (5) LTO A/B → (6) Clang A/B → then,
  if the microbench says latency-bound, commit to dual-hash. Ideas 2+3 are the separate operational track
  with the largest ceiling but real brick/thermal risk. Everything else on the board is measured-closed;
  I'd treat any suggestion of re-opening reordering/-mtune/cluster-pinning as noise.
