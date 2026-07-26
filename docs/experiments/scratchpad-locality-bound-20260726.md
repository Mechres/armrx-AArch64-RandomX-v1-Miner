# Scratchpad locality bound — main VM program IPC penalty is mostly not recoverable (2026-07-26)

**Status: closed.** This is `docs/plans/performance-plan-20260725.md` Step 1, run to completion.
Result: small recoverable gap (+6.07% IPC / +5.44% on replication), which per that plan's own gate
closes Steps 2 and 3 without needing to attempt either.

## Background

`docs/archived/plan_phase7_completed.md` and `tools/jit_correlate.py`'s region-split found the
main per-hash VM program region carries ~9% of dynamic instructions but ~20% of cycles — a
~2.2× IPC penalty relative to the rest of the pipeline. The region's opcodes are dominated by
memory operands (`*_M`, `ISTORE`) doing scratchpad reads/writes, so the working hypothesis was a
memory-latency stall. Every attempt to fix this by *hiding* the latency (extending the emitter
scheduler to memory-load opcodes) or *reducing instruction count* around it (CSEL, Newton-Raphson,
NEON AES, superscalar literal-pool relayout, `IMUL_RCP` literal-load elimination) had already
failed or regressed. Before spending more effort, Step 1 asked a cheaper question first: how much
of the 2.2× penalty is even recoverable, if scratchpad memory latency were fully eliminated?

## Method

Rather than shrinking the actual scratchpad buffer (which would break the JIT's compile-time
address masks — `kScratchpadL1Mask`/`L2Mask`/`L3Mask`, which assume specific buffer sizes up to
the full 2 MiB), a small 16 KiB `memfd` is tiled 128× across the same 2 MiB virtual address range
the JIT already computes offsets into (`AliasedScratchpad` in `tests/bench_armrx.cpp`). Every
address the compiled program can produce still lands somewhere in that 2 MiB virtual range and
gets masked exactly as before — but physically, every one of those 128 tiles backs onto the same
16 KiB, small enough to be L1-resident on this Cortex-A53. Zero changes to the JIT compiler or
its masking logic; the only difference is what physical memory the virtual range resolves to.

Two new pieces made this measurable in isolation:
- `VirtualMachine::run_execute_only()` (`src/vm.cpp`) re-invokes the JIT program compiled by the
  most recent `run()` call directly, without recompiling — isolating pure execute-against-
  scratchpad cycles from JIT compile overhead (which `ARMRX_JIT_PROFILE` builds already show is a
  separate, small cost).
- `VirtualMachine::override_scratchpad_for_bench()` swaps the scratchpad pointer used by both of
  the above — bench-only, not used by production mining.

`bench_armrx --scratchpad-real` / `--scratchpad-l1` (new flags) build a normal light-mode JIT VM,
prime it with one real hash, optionally swap in the aliased buffer, then call
`run_execute_only()` 2000 times in a loop. Both conditions wrapped in
`perf stat -e cycles,instructions`, `taskset -c 0`, on-device (real Cortex-A53 hardware, not the
x86 interpreted dev sandbox).

## Results

| Condition | Cycles | Instructions | IPC |
|---|---|---|---|
| Real 2 MiB scratchpad | 44,694,130,884 | 29,373,692,608 | 0.6572 |
| L1-aliased (16 KiB) | 42,135,298,579 | 29,373,699,356 | 0.6971 |

Instruction counts match to 5 decimal places (0.00002% apart) — expected, since both conditions
execute the identical compiled program the identical number of times, and a strong sign this is a
clean, well-controlled comparison rather than an artifact of two different code paths. The only
thing that changed between the two runs is what physical memory the scratchpad's virtual
addresses resolve to.

Forcing near-zero scratchpad latency bought **+6.07% IPC** (cycles fell 5.73% for identical
completed work). Per the plan's own gate ("if the difference is small... there is very little
room to improve — the penalty is architectural, not fixable by code changes"), **6% is small**
against the region's overall ~2.2× IPC penalty.

### Replication (2026-07-26, isolcpus removed, independent verification)

The experiment was re-run later the same day after `isolcpus=1-7 rcu_nocbs=1-7` was removed from
the kernel boot cmdline and the device rebooted. Same binary, same `taskset -c 0` pinning, same
2000-iteration protocol:

| Condition | Cycles | Instructions | IPC |
|---|---|---|---|
| Real 2 MiB scratchpad | 44,274,135,385 | 29,373,688,412 | 0.6635 |
| L1-aliased (16 KiB) | 41,983,677,277 | 29,370,537,475 | 0.6996 |

Instruction counts match to 0.011% — a slightly looser match than the first run (normal run-to-run
CBRANCH path variance from scratchpad state divergence), still confirming both conditions execute
the same compiled program. Without `isolcpus`, core 0 carries less kernel overhead, so both
conditions run slightly faster (real IPC 0.6635 vs 0.6572; L1 IPC 0.6996 vs 0.6971). The
recoverable IPC gap is **+5.44%** (cycle reduction 5.17%) — nearly identical to the first run's
+6.07%. The conclusion is robust to the system's isolation configuration.

## Conclusion

**The main VM program region's stall is mostly not a memory-latency problem.** Even with the
scratchpad made effectively latency-free, IPC only moved from 0.657 to 0.697 — nowhere near
closing a 2.2× gap. This closes both remaining steps of the gated plan without attempting either:

- **Step 2 (`PRFM` prefetch insertion)** — a prefetch hint only pays off if there's latency to
  hide. With just 6% of the penalty attributable to latency, there isn't enough left for a
  prefetch to meaningfully recover, and `PRFM` itself costs an issue slot on this in-order core.
- **Step 3 (bisect the reverted memory-op scheduler hazard)** — carries real correctness risk
  (silent wrong hashes, per `docs/experiments/memory-op-scheduler-attempt.md`'s unresolved
  divergence). Not worth that risk to chase a 6% ceiling that a much safer approach (Step 2)
  wouldn't have been worth chasing either.

The residual penalty reads as architectural — in-order pipeline depth, dependency chains between
consecutive scratchpad-dependent instructions, or similar — not something further code-level work
can chase. This matches the pattern of every other lead this project has closed on this region:
instruction-count reduction doesn't help a stall-bound workload, and now, latency-hiding doesn't
either, because the stall mostly isn't latency in the first place.

## Caveat worth recording

Per-instruction `*_M` addresses in the *real* condition are already masked to a 16 KiB
(`kScratchpadL1Mask`, when the opcode's mod-mem bit is set) or 256 KiB (`kScratchpadL2Mask`,
otherwise) window — only the once-per-program `mx`/`ma` accumulator address (`vm.cpp`) uses the
full 2 MiB `kScratchpadL3Mask64`. So the real baseline already has more locality than a naive
"uniformly random across 2 MiB" mental model would suggest. This is consistent with — and likely
part of the reason for — the recoverable gap turning out to be small rather than a methodology
artifact: a meaningful fraction of real scratchpad traffic was probably already landing in cache
some of the time before this experiment ever ran.

## Tooling landed along the way

- `tests/bench_armrx.cpp`: `--scratchpad-real`/`--scratchpad-l1` flags, `AliasedScratchpad`,
  `bench_scratchpad_locality()`. AArch64/JIT-only (`#ifdef ARMRX_HAVE_JIT`), no-ops with a message
  on the x86 interpreted dev sandbox.
- `src/vm.cpp`/`include/armrx/vm.hpp`: `VirtualMachine::run_execute_only()`,
  `override_scratchpad_for_bench()`. Bench-only, not called by production mining code paths.
- `tools/devbox/devbox_mcp.py`: `devbox_build`/`devbox_test` now prefix their remote commands with
  `taskset -c "$(cat /sys/devices/system/cpu/online)"` — found while running this experiment that
  on-device builds and parallel `ctest` runs were silently serializing onto core 0 under
  `isolcpus` (same root mechanism as the worker-count/worker-placement bugs in
  `docs/experiments/isolcpus-rt-priority-win.md`, just hitting the build tooling instead of
  mining workers). `taskset` explicitly overrides the isolcpus-restricted default affinity and is
  a harmless no-op when isolcpus isn't set.
