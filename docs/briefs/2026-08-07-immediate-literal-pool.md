# Brief: Re-enable a scheduling-safe inline literal pool for C* superscalar immediates — CLOSED (already done + reverted as regression)

**Date:** 2026-08-07
**Author:** Hermes (autonomous perf pass)
**Status:** CLOSED (2026-08-07) — the proposed 2-instr LDR-pool form was already
implemented (W4 phase-2, `emitCpoolImmediate`, commit `a1ea83c`, measuring 87-90
C* ops/program) and then **deliberately reverted to the 3-instr MOVZ/MOVK form**
because the 2-instr form was *denser than XMRig* and **re-exposed the Cortex-A53's
4-cycle MAC interlock**: consecutive program multiplies landed only 2 instructions
apart, saturating `other_interlock_stall`. The 3-instr form inserts one independent
ALU op between multiplies, hiding multiply latency — measured **+7.1% H/s (4.77→5.11)**
and `other_interlock_stall` 23.3M→6.2M/hash (below XMRig's 10.96M). Commit
`acc7735` (E24). So reducing C* immediate instructions is a **regression on this
in-order core**, not a win. The "instruction-count gap" framing is the exact trap
the W4/E24 work already navigated. **This lever is closed — do not implement.**

LESSON (for future perf passes): on the in-order A53, fewer instructions ≠ faster;
multiply-interlock latency dominates. Any density reduction that packs multiplies
tighter regresses. The gap to XMRig (~+19.5% instr) is real but the single-thread
codegen levers are exhausted/negative.

## New lead (untested, 2026-08-07): dataset-buffer hugepages / TLB thrash

Device `HugePages_Total: 0` (no hugepages configured); THP `[always]` but
`AnonHugePages: 0`. armrx attempts `MAP_HUGETLB` for the Argon2d cache
(`argon2.cpp:270`) and `MADV_HUGEPAGE` for the partial dataset
(`partial_dataset.cpp:43`), but on this device both silently fall back to 4K. The
**RandomX dataset buffer (256 MiB light) is therefore 4K-mapped** → 65,536 TLB
entries needed for the random 16,384 dataset reads/hash, vs a tiny A53 DTLB →
**TLB thrashing on the single hottest structure per hash**. XMRig credits hugepages
with "up to 50%" on this exact access pattern.

**Deployment lever (NOT a code change) — code already supports it.** `allocLargePagesMemory`
(virtual_memory.c:235) uses `MAP_HUGETLB|MAP_POPULATE`; if the system has
configured hugepages, armrx's dataset/Argon2d buffers use them automatically (no
recompile). Blocked on-device: writing `/proc/sys/vm/nr_hugepages` returns
`Permission denied` even as root from the pmOS shell (BusyBox `ash` sysctl-write
quirk / dropped caps), and `HugePages_Total` is 0 at runtime. Same class of
operational win as `isolcpus=1-7` — the user must enable it at boot (init script
or kernel cmdline, e.g. `hugepagesz=2M hugepages=N` / a startup
`sysctl -w vm.nr_hugepages=N` with `CAP_SYS_ADMIN`), exactly like the isolcpus
precedent. Once enabled, armrx picks it up with no code change. **Not measurable
from this agent session** (can't allocate hugepages here); left as a user-actionable
deployment recommendation.

## Web/agent sweep (2026-08-07) — no further code lever found

Searched: XMRig RandomX ARMv8 JIT opt, RandomX prefetch tuning, XMRig 5.1.0
"+6-7%", AArch64 hugepages/TLB. Outcome: the only published non-codegen wins are
x86/Intel-specific (MSR, hardware-prefetcher disable, THP) — not applicable to
AArch64 in-order A53. The canonical RandomX `calc_dataset_item_aarch64` prefetch
geometry is what armrx already mirrors. **No unexploited AArch64 codegen lever
remains.** Single-thread codegen levers are exhausted/negative (E24 proved density
reductions regress the MAC interlock); the only remaining real perf is deployment
(hugepages) + cluster placement (user-ruled-out).

**Conclusion:** the proposed immediate-materialization lever is CLOSED (regression);
hugepages is the one real remaining lever but is a deployment change the user must
enable (like isolcpus), not shippable code. Recommend documenting it in TESTING.md
§8 / RETROSPECTIVE as a deployment prerequisite and stopping codegen pursuit.

armrx is ~+19.5% instructions/hash vs XMRig (118.96M vs 99.57M). The gap is
**instruction-count, not stalls** — IPC is already *better* than XMRig (0.731
superscalar-region, 1.369 overall vs XMRig 0.612 / ~1.0). ~86% of the gap is
attributed to immediate-materialization (`imm`-loading) in the superscalar body.

## 2. What the canonical RandomX AArch64 JIT does (read for ideas, NOT copied)

From `tevador/RandomX/src/jit_compiler_a64.cpp` (`emitMovImmediate`):

- For a 32-bit immediate `< 2^16`: `MOVZ` — 1 instruction (same as armrx).
- For a 32-bit immediate `>= 2^16`: load from a **NEON literal pool** via
  `umov`/`smov` (`emit32(0x0E043C00 | dst | ...)`, pulling a 32-bit lane out of a
  pre-populated vector register `vN`) — **1 instruction** for *any* 32-bit value.
- Fallback (pool exhausted, >64 entries): `MOVZ`/`MOVN` + `MOVK` — 2 instructions.

So the canonical JIT pays **1 instruction** to materialize a 32-bit constant.

## 3. What armrx does today

- `jit_dataset_2way.cpp::emitMovImmediate2Way` **always** uses the `MOVZ`/`MOVN`+`MOVK`
  path (2 instructions) — comment states it deliberately drops the literal-pool
  fast path to stay a "faithful match" to the pinned path in the main JIT.
- `jit_compiler_a64.cpp::generateSuperscalarHash` **pins `num32bitLiterals` at its
  64 cap** (lines ~1256-1266) specifically so `emitMovImmediate`'s pool branch is
  **never taken** for this region — every 32-bit immediate takes `MOVZ`/`MOVK`.

The reason armrx dropped the pool: the NEON-literal-pool branch is
**order-sensitive under instruction scheduling** (a past hazard, flagged
`docs/archived/audits/scheduler-review-2026-07-25.md` #4). If pool slots are
interleaved with reorderable ops, a scheduled program reads the wrong constant.

## 4. The lever (clean, faithful to armrx's own existing pattern)

The `IXOR_C7/8/9` and `IADD_C7/8/9` superscalar ops (the **only** multi-instruction
32-bit-immediate consumers in the superscalar body) currently emit:

```
MOVZ tmp, #imm_hi ; MOVK tmp, #imm_lo ; EOR/ADD dst, dst, tmp   ; = 3 instructions
```

The canonical JIT emits them in 2 (`umov` + `EOR`). armrx can hit 2 **without**
the scheduling hazard by using its *own existing* `IMUL_RCP` pool pattern instead
of the NEON pool:

- Pre-scan the superscalar program, collect the `getImm32()` of every `IXOR_C*`/
  `IADD_C*` op into a **fixed-slot literal pool** (like `IMUL_RCP`'s pool region),
  emitted once with a `B` over it.
- Emit each C* op as `LDR Wtmp, [pool_slot]` + `EOR/ADD dst, dst, tmp` = **2 instructions**.
- The pool is at a *fixed* location per program, referenced by PC-relative offset,
  so it is **order-independent under scheduling** — same safety property as
  `IMUL_RCP` (which already does this today and is scheduling-safe).

This is NOT a reintroduction of the audit hazard: that hazard was pool slots
*interleaved* with reorderable instructions. A fixed, pre-emitted pool referenced
by offset has no ordering dependency.

### Why this is correct (correctness argument)
- The C* immediate is a 32-bit constant. `LDR Wt, [literal]` loads 4 bytes and
  zero-extends into the 64-bit register — exactly the value `MOVZ`/`MOVK` produced.
  `EOR`/`ADD` on the 64-bit register is therefore bit-identical to the current path.
- No register-liveness or cross-instruction hazard: `tmp` (= x12 in single-stream,
  the per-stream temp in 2-way) is written and consumed entirely within the one
  C* op's emission, same as today.

## 5. Expected saving

- **−1 instruction per C* op.** C* ops are a fraction of the ~256-instruction
  superscalar program (they are 9 of the ~20 superscalar opcodes). Real count
  must be measured (Phase 2 below). If ~10–30 C* ops/program × 8 cache accesses
  = ~80–240 fewer instructions/hash. Modest in absolute terms (~0.1–0.2% of
  118.96M) but it directly attacks the dominant cost center and is the *only*
  remaining code-level instruction-count lever after JIT scheduling was closed.
- Code size grows slightly (4-byte pool entry per C* op) — immaterial (ICache,
  and `CalcDatasetItemSize` already carries margin).

## 6. Implementation plan

1. `jit_compiler_a64.cpp::generateSuperscalarHash`: add a pre-scan collecting C*
   immediates into a pool (mirror `IMUL_RCP`: emit pool, `B` over it, track a
   per-program `cLitPos`). In the `IXOR_C*`/`IADD_C*` switch cases, replace
   `emitMovImmediate`+`EOR`/`emitAddImmediate` with `LDR Wtmp,[pool]`+`EOR`/`ADD`.
2. `jit_dataset_2way.cpp::emitSuperscalarInstr`: same, using the per-stream
   `literalPos`-style tracker already present, adding a C* pool alongside the
   existing `IMUL_RCP` pool.
3. Keep `num32bitLiterals` pinning logic as-is for the NEON path (we are NOT
   re-enabling the NEON pool — we use an explicit LDR-Literal pool, which is a
   different, scheduling-safe mechanism).

## 7. Verification gates (TESTING.md §8 — one heavy test/session, fastest-first)

- **G1 (fastest):** `test_jit_equivalence` — 16/16 byte-identical (catches any
  codegen divergence: wrong constant loaded = wrong hash = failure). MUST pass.
- **G2:** `test_mining` — engine lifecycle produces valid shares.
- **G3:** on-device 450-pair + 200-pair stress (differential JIT/interpreter
  byte-identical) — the same gate class that caught prior scheduler regressions.
- **G4 (perf A/B):** `bench_armrx --full-hash-only --workers=N` before/after;
  expect instruction count (perf stat) to drop by ~C*-op-count × 8, hashrate
  flat-to-positive (the saving is instruction-count, not IPC).

## 8. Risk / fallback

- Risk: pool offset computation wrong → crash or silent wrong hash. Mitigated by
  G1/G3 (KAT + differential stress catch it).
- If I cannot get the pool offset/scheduling right after a bounded attempt,
  escalate to **reasonix** (external DeepSeek agent) with a verbatim brief rather
  than shipping a half-fix.
- If this lever proves too small to matter (<0.1% measured), pivot (per standing
  instruction) to web search / another-agent sweep for a larger instruction-count
  lever and continue.
