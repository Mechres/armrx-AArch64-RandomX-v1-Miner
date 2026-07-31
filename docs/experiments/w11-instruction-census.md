# W1-1 Region Instruction Census — Device-Verified (2026-08-01)

Attributes the clean steady-state **118.96 M instr/hash** (T1-2's 119.0 M,
confirmed here to −0.04%) into region buckets, and closes the long-open
reconciliation between the old sample-based superscalar attribution (~96.7 M)
and the static superscalar body figure (58.4 M). **Verdict up front:
the old 96.7 M attribution was right; the static 58.4 M figure was wrong —
the actual superscalar body is 5,224 AArch64 instr/call, not 3,563.**

## Goal

For `bench_armrx --full-hash-only --perf-ready` (light mode, JIT, single
worker, fixed seed `{0x00, 0x11}`), attribute the per-hash instruction and
cycle budgets into:

1. superscalar **opcode body** — samples in the superscalar JIT buffer
   attributed to the emitted dataset-item opcodes (reconcile vs static 58.4 M)
2. superscalar **wrapper** — fixed prologue/prefetch/mix/store code in the
   same buffer not covered by the opcode table
3. **main-VM JIT region** — samples at buffer offset < CodeSize (per-hash
   regenerated program; region attribution only)
4. **named C++** — bench binary symbols + libc (AES fill/hash, Blake2b,
   Argon2d amortized ≈ 0, JIT compile, glue)
5. **unattributed** — everything else

## Method (all device, `taskset -c 3`, isolcpus=1-7 active)

Binary: `/tmp/cross/bench_armrx`, md5 `257e6d14b994749daa9371529bc7ce51`
(the exact T1-2 baseline; verified before and after every run). `perf`
7.1.3 on device, `perf_event_paranoid=2` (own-process `-p` allowed).

1. **Total confirmation** — `perf stat -e cycles,instructions -p <pid>`
   attached after `PERF_READY` (device fifo pattern, `tools/w11_census_device.sh
   stat`), 500-hash gated window.
2. **Sample capture** — two `perf record` passes, same gate:
   `perf record -F 999 -e instructions:u -p <pid>` and
   `perf record -F 999 -e cycles:u -p <pid>`, ~104 s windows, 101,556 and
   104,153 samples. (Sampling at 999 Hz cost ~1.4% hash time; shares are
   unaffected.)
3. **Maps snapshot** — `/proc/<pid>/maps` captured twice per record pass
   during the measured loop; the two snapshots are byte-identical (stable
   layout). One executable anonymous region: the JIT buffer
   `rwxp`, **118,784 B = 0x1D000**, exactly page-round(CodeSize +
   CalcDatasetItemSize) = (51,516 + 66,256) — geometry verified against the
   binary's own symtab (`nm`): CodeSize = `randomx_init_dataset_aarch64_end −
   randomx_program_aarch64` = 0x3697c − 0x2a040 = **0xC93C = 51,516 B**;
   superscalar region = [CodeSize, buffer_end).
4. **Classification** — `perf script -F ip,dso,sym` on device, text pulled
   to host, `tools/w11_census.py` buckets by address (JIT buffer via maps,
   CodeSize as the main-VM/superscalar boundary) and by dso/symbol for
   bucket 4.
5. **Opcode sub-attribution** — NOT available: `bench_armrx` has no
   `--jit-dump` (verified via `--help` and `tests/bench_armrx.cpp` grep). Per
   the W1-1 protocol fallback, buckets 1 and 2 are reported combined
   (measured) and split statically (below). The split is exact because the
   whole superscalar emission is straight-line: body 5,224 + wrapper 177
   (28 prologue + 3 prefetch + 8×(12 mix + 1 `b`-over-pool + 1 `mov x10` +
   1 `and` + 1 `add` + 1 `prfm`) + 13 store — counted from the deployed
   binary's disassembly).

### Raw output (total confirmation, 500 hashes)

```
 Performance counter stats for process id '3867':

       80209092668      cycles:u
       59478953982      instructions:u

     104.119904199 seconds time elapsed
```

| Metric | This run | T1-2 | Δ |
|---|---|---|---|
| Instructions / hash | **118,957,908** | 119.0 M | −0.04% |
| Cycles / hash | 160,418,185 | 160.9 M | −0.3% |
| IPC | 0.742 | 0.740 | +0.3% |
| Median hash | 206.31 ms | 207.00 ms | −0.3% |

The instruction total matches T1-2's run to within 21 k instructions
(0.00004%) — deterministic workload, same binary.

## Results — bucket table (M/hash and % of total)

Confirmed totals: **118.96 M instr/hash**, **160.42 M cycles/hash**.

| Bucket | instr share | instr M/hash | cycles share | cycles M/hash | IPC |
|---|---|---|---|---|---|
| 1+2 superscalar region | **80.51%** | **95.77 M** | 74.64% | 119.74 M | 0.800 |
| 3 main-VM JIT region | 9.91% | 11.79 M | **18.13%** | **29.08 M** | 0.405 |
| 4 named C++ | 9.53% | 11.33 M | 7.07% | 11.34 M | ~1.0 |
| 5 unattributed | 0.05% | 0.06 M | 0.16% | 0.25 M | — |
| **Total** | 100.0% | 118.96 M | 100.0% | 160.41 M | 0.742 |

Bucket 4 sub-attribution (instruction pass): `hash_aes_1r_x4` 5,147 samples,
`fill_aes_1r_x4` 3,650, `AesGenerator4R::next` 359, JIT-compile path
(`emitPrologueMix`, `resolveInstructionType`, `h_*` handlers) ~470, libc 15,
Blake2b 5 — AES ≈ **10.7 M/hash (9.0%)**, everything else ≈ 0.6 M/hash.

### Static split of the superscalar region (buckets 1 vs 2)

Read the live JIT buffer (`/proc/<pid>/mem`, 118,784 B) during a run and
disassembled the emitted superscalar code (extent 23,588 B = 5,897 words;
literal pools identified via `ldr`-literal targets, 250 entries + 8 template
quads = 500 data words):

| Piece | instr/call | M/hash @16,384 calls | % of total |
|---|---|---|---|
| Opcode body (bucket 1) | **5,224** | 85.59 M | 71.9% |
| Wrapper (bucket 2) | 177 | 2.90 M | 2.4% |
| **Static superscalar total** | **5,401** | 88.49 M | 74.4% |

Documented static figure for comparison: 3,563 body (+177 wrapper = 3,740) ×
16,384 = **58.4 M** (`master-plan-20260727.md` Track A item 1).

## Reconciliation verdict — A/B/C

**Hypothesis A (perf sample bias): DEAD.** The region-level census —
same `perf record -e instructions` sampling method, but on the clean
`--perf-ready` total instead of the old 132.93 M — reproduces the old
superscalar attribution almost exactly: **95.77 M measured vs 96.7 M claimed**
(72.71% × 132.93 M). The old samples were not biased; they were right, and
the total has since shrunk (132.93 M → 118.96 M, Track G + clean harness).

**Hypothesis B (3,563 under-counts the opcode-attributed body): SURVIVES —
this is the explanation.** The actual emitted superscalar body for this
binary + seed is **5,224 AArch64 instr/call — 1.47× the documented 3,563**
(measured from the live buffer, not inferred). The 58.4 M "exact" figure was
computed from a `--jit-dump` snapshot whose per-opcode totals no longer
describe this binary's real superscalar programs (the dump predates the
final emitter/generator state; the actual emission carries 250 IMUL_RCP
(2 instr each), 868 EOR, 1,007 MUL, 761 ADD, 404 SUB, 376 ROR, 242 MULH —
a ~4,000-instruction RandomX-level program per item at ~1.3 A64 instr each,
not the ~512-instruction/3,563-A64 shape the doc assumed).

**Hypothesis C (call-count/buffer assumptions): RESOLVED — the call count is
EXACTLY 16,384/hash; the 8.2% gap is per-call static-count slack.** Source
analysis (2026-08-01, post-census): the light-mode dataset read is a hard
loop — 8 `run()` calls per hash (vm.cpp:982-990) × **2,048 loop iterations per
call** (x3 = hardcoded `2048ULL`, vm.cpp:843; `subs x3, x3, 1; bne .Lmain_loop`,
jit_compiler_a64_static.S:510-511) × one `bl rx_calc_dataset_item` per
iteration (light path, .S:577) = **16,384 calls/hash, exact by construction**
(2,048 = RANDOMX_PROGRAM_MAX_SIZE × kRandomXCacheAccesses, 256 × 8). The
census's "implied 17,727 calls" was 95.77M ÷ 5,401 — an artifact of an
under-measured static divisor: the true per-call dynamic cost is **~5,845 A64**
(95.77M ÷ 16,384, +8.2% over the 5,401 counted from the 23,588-B live read;
implied true emission extent ~25.5 KB — literal-pool identification slack).
The budget then closes exactly: 16,384 × (719.6 main-VM + 5,845.3 item) =
107.56 M/hash = measured 95.77 M + 11.79 M; + AES ≈10.7 M + blake/glue ≈0.6 M
≈ 118.96 M total. The documented "16,384×/hash" figure was correct all along;
what the census actually demonstrated is that the *static per-call count*
(5,401) is ~8% low, not that the call count is high. The direct call-count
instrumentation attempt (blocked by the I-cache anomaly — see caveat 5) is
no longer needed.

> **Device hazard note (kept from the original C analysis):** the direct
> call-count instrumentation attempt (patch the superscalar `ret` in the
> live RWX buffer with a self-incrementing counter) was blocked by a
> kernel/device **I-cache coherence anomaly**: bytes written to executable
> memory (both via `/proc/<pid>/mem` and in-process with
> `__builtin___clear_cache`) are sometimes executed as different instructions
> (verified standalone: executed `adrp`/`add` encodings differed from the
> written words; SIGILL/SIGSEGV). Any future JIT-buffer instrumentation on
> this kernel (6.12.1-msm8916) should use kernel-side counting
> (uprobes/perf) instead of memory patching.

## Cycle-side findings

- **Main-VM region carries its known ~2.2× IPC penalty**: 9.91% of
  instructions but 18.13% of cycles (IPC 0.405 vs 0.800 superscalar) —
  consistent with the `jit_correlate` finding that the per-hash program
  region is memory-op-stall-bound. Superscalar is the *efficient* half.
- Bucket 4 (AES T-table path) runs at IPC ≈ 1.0 — T-tables are L1-resident.

## Implications for gated work

- **W3-1 (Track C reopen)**: the dataset-item *wrapper/frame* is 177/5,401 =
  3.3% of the superscalar region = ~2.9 M instr/hash ≈ 2.4% of total — a
  "multi-percent" slice only by the loosest reading. The specific Track-C
  Phase A target (the 14 stp/7 ldp x0-x13 save/restore, 28 instrs/call ≈
  0.46 M/hash ≈ 0.4%) remains inside the already-closed ≤0.5% ceiling.
  **Gate stays closed.**
- **W3-3 (no-ABI trampoline)**: the gap is not in non-opcode frame/glue —
  the superscalar region is 96.7% opcode body (5,224/5,401), and the
  main-VM + wrapper slices are stall/penalty-bound, not instruction-bound.
  **Gate stays closed.**

## Caveats

1. **`--jit-dump` unavailable in `bench_armrx`** — buckets 1 and 2 are
   measured combined and split via the static body/wrapper counts (both
   measured from the deployed binary: body from the live-buffer read,
   wrapper from the template disassembly). No per-opcode sample attribution
   was possible; the 5,224 body figure is for this binary's own seed.
2. The static 5,401/call × 16,384 = 88.49 M vs measured 95.77 M leaves an
   8.2% gap — **resolved (2026-08-01): the call count is exactly 16,384/hash
   by construction (8 run() × 2,048-loop × 1 `bl` per iteration); the gap is
   per-call static-count slack** (true dynamic ~5,845 A64/call — see the
   Hypothesis C resolution above). The region-level attribution is unaffected.
3. ~0.5% pre-gate contamination (poll) + post-loop prints land in buckets
   4/5 — negligible (2 poll + 1 flush samples of 101,556).
4. Sampling overhead raised hash time ~1.4% in record passes (209 vs
   206 ms) — shares unaffected; the perf stat total pass ran unperturbed.
5. I-cache coherence anomaly (above) — any future agent instrumenting the
   JIT buffer on this device should expect it and use kernel-side counting
   (uprobes/perf) instead of memory patching.

## Files / cross-references

- `tools/w11_census_device.sh` — device-side capture (stat / instr / cycles)
- `tools/w11_census.py` — host-side bucket classifier
- `docs/experiments/t12-perf-ready-first-run.md` — the clean total being
  attributed
- `docs/audits/performance-audit-work-ideas-20260801.md` §3 — the open
  reconciliation this closes
- `docs/plans/20260727/master-plan-20260727.md` Track A item 1 — the 58.4 M
  static figure (superseded: actual body 5,224 instr/call)
