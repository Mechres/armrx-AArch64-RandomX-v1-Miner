# Dual-hash interleaving — premise measurement (2026-08-07)

## Question
The 5-agent sweep flagged "dual-hash interleaving" as a novel code lever not yet
closed. Premise: on the in-order Cortex-A53 the integer-multiply (MAC) port has
4-cycle result latency but might be under-utilized during a single RandomX hash,
so folding a 2nd independent hash's multiply stream into the same issue window
could raise per-thread throughput (fill the interlock gaps).

## Method
`tools/bench_dualhash_premise.cpp` (experiment tool, NOT shipped) drives two
independent `VirtualMachine`s (seed A / seed B, each own cache+scratchpad) in
ONE thread, either serially (`single`) or alternated (`interleaved`), and reports
H/s. Each mode wrapped in `perf stat -e cycles:u,instructions:u,other_interlock_stall:u`
on-device (lenovo, aarch64, cross-built, 300 iters/mode).

Decision rule: interleaved H/s > single AND other_interlock_stall/iter lower →
port has slack, dual-hash worth implementing in the JIT. Else → dead.

## Result (device, 300 iters/mode)

| metric            | single      | interleaved  | Δ        |
|-------------------|-------------|--------------|----------|
| H/s (total)       | 5.049       | 5.034        | −0.3%    |
| cycles            | 56.83B      | 57.85B       | +1.8%    |
| instructions      | 37.16B      | 37.17B       | +0.04%   |
| other_interlock_stall | 3.470B (11.57M/iter) | 3.430B (11.43M/iter) | −1.2% |

## Verdict: DEAD as a lever
Interleaving a 2nd independent hash stream into one thread produced **no
throughput gain** (5.03 vs 5.05 H/s, within noise) and **no reduction in
other_interlock_stall** per hash (11.43M vs 11.57M, flat). The MAC port is
already saturated by the single hash's multiply dependency chain — there is no
spare port throughput to exploit. (The earlier ~17%-busy estimate from
instructions/hash ÷ cycles/hash was wrong: in the FULL hash pipeline, not the
execute-only loop, the MAC is the bottleneck and fully utilized.)

Host (x86_64) sanity also showed single ≈ interleaved (~8.7 H/s) — expected,
x86 has wide OoO and no A53 MAC constraint, so it cannot falsify the premise;
only the device measurement is authoritative.

## Consequence
Dual-hash interleaving is CLOSED (honest negative). The only remaining CODE lever
that attacks the partial-dataset fill speed (the ~164s dead-start) is
**Tier 2(b): vectorize ACROSS dataset items** (process 2–4 independent items
simultaneously in NEON lanes — items have no inter-dependency). That is a
separate lever and still open. Multi-worker scaling (E16) and the hardware
clock/OPP unlock remain the larger ceilings.
