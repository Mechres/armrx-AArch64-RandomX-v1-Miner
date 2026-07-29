# Light-Mode Dataset-Item Helper: Reduced Register Preservation — Tried, Hung, Reverted

## Context

`docs/plans/20260727/master-plan-20260727.md` Track C ("inline the light-mode dataset-item
helper") identifies the light-mode call site's `bl rx_calc_dataset_item` (16,384×/hash) as paying
for register preservation it doesn't need: the callee (`rx_calc_dataset_item`,
`src/jit_compiler_a64_static.S`) saves/restores x0-x13 (14 registers, 7 `stp`/`ldp` pairs), but the
light-mode caller (`randomx_program_aarch64_vm_instructions_end_light`) discards x0/x1/x2
unconditionally (reloads its own copies from its own stack frame right after the call returns) and
never uses x3 at all. Phase A of that track was scoped as "remove the duplicate preservation."

Before writing any code, tracing the actual mechanism revealed this is substantially more involved
than a superficial trim (full account of that design work is in the conversation this doc is
extracted from; the short version):

- `rx_calc_dataset_item` is called from **two** places: the light-mode per-hash path, and
  `randomx_init_dataset_aarch64_main_loop` (fast-mode dataset generation). The second is **dead
  code** — `JitCompilerA64::getDatasetInitFunc()` is defined but never called anywhere in the
  codebase (verified by grep across `src/`, `include/`, `tests/`). Armrx's real fast-mode dataset
  generation goes through `src/dataset.cpp`'s separate C++/NEON `initialize_dataset()`. This means
  only the light-mode call site's needs actually matter today, but the original, general-purpose
  function was left completely untouched regardless (lowest-risk choice, and keeps the dead-code
  path correct in case it's ever revived).
- The static `.S` template isn't executed in place for this path — `generateSuperscalarHash()`
  (`src/jit_compiler_a64.cpp`) `memcpy`s raw bytes from specific label-delimited ranges of the
  **compiled static binary** into a fresh JIT buffer once per seed rotation, splicing in the
  actual per-seed superscalar program instructions. The prologue's trailing
  `b rx_calc_dataset_item_prefetch` is a **PC-relative branch whose offset is computed at static
  link time** — for it to remain valid after being copied to a different runtime address, the
  copy's end boundary must be positioned such that the relative distance from the branch to
  "whatever the JIT emits immediately after this copy" exactly matches the branch's link-time-
  computed offset. This holds today (and was empirically re-verified below) but constrains where
  a new, reduced-preservation prologue variant may live.

The implementation plan below was designed to satisfy that constraint: a new light-mode-only
prologue/epilogue pair, reachable only by changing which static byte ranges
`generateSuperscalarHash()` copies from — not by touching the shared prefetch/mix code, the
original `rx_calc_dataset_item` function, or the `CodeSize`-boundary mechanism that makes the
existing (unmodified) `bl rx_calc_dataset_item` correctly resolve into `generateSuperscalarHash()`'s
output in the first place.

## What was implemented

Three files changed. Conservative scope: drop only x0-x3 preservation (matching the master plan's
stated Phase A), keep x4-x13 preserved (x4/x5/x6/x7/x12/x13 are the VM's live r0-r5 registers; the
caller doesn't separately save them, so the callee still must). A more aggressive version (also
dropping x8-x11, since those are the callee's own internal scratch and the light-mode caller
doesn't need them preserved either) was identified during design but deliberately *not* attempted
in the same change, to keep the risk surface as small as possible for a first attempt.

### `include/armrx/jit_compiler_a64_static.hpp`

```diff
 	void randomx_calc_dataset_item_aarch64_store_result();
 	void randomx_calc_dataset_item_aarch64_end();
+	void randomx_calc_dataset_item_aarch64_light();
+	void randomx_calc_dataset_item_aarch64_light_store_result();
+	void randomx_calc_dataset_item_aarch64_light_end();
 }
```

### `src/jit_compiler_a64_static.S`

Three new `.global` declarations, alongside the existing `randomx_calc_dataset_item_aarch64*` ones:

```diff
 	.global DECL(randomx_calc_dataset_item_aarch64)
 	.global DECL(randomx_calc_dataset_item_aarch64_prefetch)
 	.global DECL(randomx_calc_dataset_item_aarch64_mix)
 	.global DECL(randomx_calc_dataset_item_aarch64_store_result)
 	.global DECL(randomx_calc_dataset_item_aarch64_end)
+	.global DECL(randomx_calc_dataset_item_aarch64_light)
+	.global DECL(randomx_calc_dataset_item_aarch64_light_store_result)
+	.global DECL(randomx_calc_dataset_item_aarch64_light_end)
```

New prologue, inserted **immediately before** `DECL(randomx_calc_dataset_item_aarch64_prefetch):`
(textual adjacency is load-bearing — see Context above; this is what lets the trailing branch
reuse `rx_calc_dataset_item_prefetch` as a valid target after being copied elsewhere):

```asm
# Light-mode-only entry point (Track C Phase A, master-plan-20260727.md).
# Used exclusively by generateSuperscalarHash()'s memcpy for the per-hash
# light-mode dataset-item derivation path -- never reached via a live `bl`
# in the statically-linked binary (jit_compiler_a64.cpp redirects its copy
# source pointers to this label instead of randomx_calc_dataset_item_aarch64;
# the original symbol above, and randomx_init_dataset_aarch64's `bl` to it,
# are both left completely untouched).
#
# Saves only x4-x13 (the light-mode call site's live state: x4/x5/x6/x7/
# x12/x13 are the VM's r0-r5, and x8-x13 are read as this function's own
# input parameters or used as its internal scratch across the loop) instead
# of the original's x0-x13. x0/x1/x2/x3 are NOT preserved here: the light-
# mode caller (randomx_program_aarch64_vm_instructions_end_light) already
# reloads its own x0/x1/x2/x30 unconditionally from its own stack frame
# immediately after the `bl` returns, and never uses x3 at all -- their
# preservation in the original function exists only for the (dead-code)
# fast-mode caller randomx_init_dataset_aarch64_main_loop, which is not
# reachable from this entry point.
#
# Must end with `b rx_calc_dataset_item_prefetch` (the SAME shared target
# the original prologue branches to) so its trailing branch instruction's
# link-time-computed PC-relative offset remains valid once memcpy'd into a
# JIT buffer -- this requires this block to sit textually adjacent to (i.e.
# share the same end-of-copy-range boundary as) randomx_calc_dataset_item_
# aarch64_prefetch below. Do not move this block away from that label.
DECL(randomx_calc_dataset_item_aarch64_light):
rx_calc_dataset_item_light:
	sub	sp, sp, 80
	stp	x4, x5, [sp]
	stp	x6, x7, [sp, 16]
	stp	x8, x9, [sp, 32]
	stp	x10, x11, [sp, 48]
	stp	x12, x13, [sp, 64]

	ldr	x12, superscalarMul0_light

	mov	x8, x0
	mov	x9, x1
	mov	x10, x2

	# rl[0] = (itemNumber + 1) * superscalarMul0;
	madd	x0, x2, x12, x12

	# rl[1] = rl[0] ^ superscalarAdd1;
	ldr	x12, superscalarAdd1_light
	eor	x1, x0, x12

	# rl[2] = rl[0] ^ superscalarAdd2;
	ldr	x12, superscalarAdd2_light
	eor	x2, x0, x12

	# rl[3] = rl[0] ^ superscalarAdd3;
	ldr	x12, superscalarAdd3_light
	eor	x3, x0, x12

	# rl[4] = rl[0] ^ superscalarAdd4;
	ldr	x12, superscalarAdd4_light
	eor	x4, x0, x12

	# rl[5] = rl[0] ^ superscalarAdd5;
	ldr	x12, superscalarAdd5_light
	eor	x5, x0, x12

	# rl[6] = rl[0] ^ superscalarAdd6;
	ldr	x12, superscalarAdd6_light
	eor	x6, x0, x12

	# rl[7] = rl[0] ^ superscalarAdd7;
	ldr	x12, superscalarAdd7_light
	eor	x7, x0, x12

	b	rx_calc_dataset_item_prefetch

superscalarMul0_light: .quad 6364136223846793005
superscalarAdd1_light: .quad 9298411001130361340
superscalarAdd2_light: .quad 12065312585734608966
superscalarAdd3_light: .quad 9306329213124626780
superscalarAdd4_light: .quad 5281919268842080866
superscalarAdd5_light: .quad 10536153434571861004
superscalarAdd6_light: .quad 3398623926847679864
superscalarAdd7_light: .quad 9549104520008361294

# Prefetch -> SuperScalar hash -> Mix will be repeated N times

DECL(randomx_calc_dataset_item_aarch64_prefetch):
```

New epilogue, inserted **immediately after** `DECL(randomx_calc_dataset_item_aarch64_end):` (no
positional constraint here — reached by plain `codePos`-contiguous fallthrough after the mix
loop's final iteration, not by a branch, so this block's location in the file doesn't matter;
kept adjacent to the original epilogue for readability only):

```asm
DECL(randomx_calc_dataset_item_aarch64_end):

# Light-mode-only epilogue (Track C Phase A, pairs with
# randomx_calc_dataset_item_aarch64_light above). Reached purely by
# codePos-contiguous placement after generateSuperscalarHash()'s final
# mix-loop iteration falls through -- unlike the prologue side, there is no
# branch-offset-preservation constraint here, so this block's position in
# the file is not load-bearing (kept adjacent to the original epilogue for
# readability only). Stores the result identically to the original (x9 is
# the output pointer in both variants), then restores only x4-x13 (matching
# the light prologue's reduced save set) and deallocates an 80-byte frame
# instead of 112.
DECL(randomx_calc_dataset_item_aarch64_light_store_result):
	stp	x0, x1, [x9]
	stp	x2, x3, [x9, 16]
	stp	x4, x5, [x9, 32]
	stp	x6, x7, [x9, 48]

	ldp	x4, x5, [sp]
	ldp	x6, x7, [sp, 16]
	ldp	x8, x9, [sp, 32]
	ldp	x10, x11, [sp, 48]
	ldp	x12, x13, [sp, 64]
	add	sp, sp, 80

	ret

DECL(randomx_calc_dataset_item_aarch64_light_end):
```

### `src/jit_compiler_a64.cpp`

`CalcDatasetItemSize`'s prologue/epilogue terms updated to size the allocation against the light
variant (the actual thing written), not the original:

```diff
+// Track C Phase A (master-plan-20260727.md): generateSuperscalarHash() now
+// copies its prologue/epilogue from the *_light static labels instead of the
+// original (still present, still used by the dead-code fast-mode path)
+// randomx_calc_dataset_item_aarch64 ones -- this sizing formula is updated to
+// match, so the buffer allocation reflects what actually gets written rather
+// than merely over-allocating (harmless either way, but this stays accurate).
 static const size_t CalcDatasetItemSize =
-	// Prologue
-	((uint8_t*)randomx_calc_dataset_item_aarch64_prefetch - (uint8_t*)randomx_calc_dataset_item_aarch64) +
+	// Prologue (light variant)
+	((uint8_t*)randomx_calc_dataset_item_aarch64_prefetch - (uint8_t*)randomx_calc_dataset_item_aarch64_light) +
 	// Main loop
 	RANDOMX_CACHE_ACCESSES * (
 		// Main loop prologue
 		((uint8_t*)randomx_calc_dataset_item_aarch64_mix - ((uint8_t*)randomx_calc_dataset_item_aarch64_prefetch)) + 4 +
 		// Inner main loop (instructions)
 		((RANDOMX_SUPERSCALAR_LATENCY * 3) + 2) * 16 +
 		// Main loop epilogue
 		((uint8_t*)randomx_calc_dataset_item_aarch64_store_result - (uint8_t*)randomx_calc_dataset_item_aarch64_mix) + 4
 	) +
-	// Epilogue
-	((uint8_t*)randomx_calc_dataset_item_aarch64_end - (uint8_t*)randomx_calc_dataset_item_aarch64_store_result);
+	// Epilogue (light variant)
+	((uint8_t*)randomx_calc_dataset_item_aarch64_light_end - (uint8_t*)randomx_calc_dataset_item_aarch64_light_store_result);
```

`generateSuperscalarHash()`'s first `memcpy` (prologue), redirected to the light labels:

```diff
 	uint32_t codePos = CodeSize;
 
-	uint8_t* p1 = (uint8_t*)randomx_calc_dataset_item_aarch64;
+	// Track C Phase A: copy the reduced-preservation light-mode prologue
+	// (saves x4-x13 only) instead of the general-purpose one (saves x0-x13,
+	// still used unmodified by the dead-code fast-mode path). Safe because
+	// this whole function's output is only ever reached via light mode's
+	// own `bl rx_calc_dataset_item`, whose target resolves here via the
+	// existing, unmodified CodeSize-boundary mechanism -- see the comment
+	// on randomx_calc_dataset_item_aarch64_light in the .S file for why its
+	// end boundary must stay randomx_calc_dataset_item_aarch64_prefetch.
+	uint8_t* p1 = (uint8_t*)randomx_calc_dataset_item_aarch64_light;
 	uint8_t* p2 = (uint8_t*)randomx_calc_dataset_item_aarch64_prefetch;
 	memcpy(code + codePos, p1, p2 - p1);
 	codePos += p2 - p1;
```

The loop's two `memcpy` calls (prefetch+4→mix, mix→store_result, each run 8×) were **left
completely unchanged** — this code is pure register logic with no stack-frame dependency, genuinely
shared between both prologue variants.

`generateSuperscalarHash()`'s final `memcpy` (epilogue), redirected to the light labels:

```diff
-	p1 = (uint8_t*)randomx_calc_dataset_item_aarch64_store_result;
-	p2 = (uint8_t*)randomx_calc_dataset_item_aarch64_end;
+	// Track C Phase A: light-mode epilogue (restores x4-x13, 80-byte frame)
+	// instead of the general-purpose one (restores x0-x13, 112-byte frame).
+	// Reached by plain codePos-contiguous fallthrough from the mix loop
+	// above, so unlike the prologue there is no branch-offset constraint on
+	// which epilogue variant is copied here.
+	p1 = (uint8_t*)randomx_calc_dataset_item_aarch64_light_store_result;
+	p2 = (uint8_t*)randomx_calc_dataset_item_aarch64_light_end;
 	memcpy(code + codePos, p1, p2 - p1);
 	codePos += p2 - p1;
```

## Verification performed before running anything

Given the delicacy of the copy-and-splice mechanism, static correctness was checked two ways
before trusting a live run:

1. **Static disassembly of the compiled binary** (`objdump -d` / `nm` on `armrx_tests`) confirmed:
   - The new prologue's trailing branch (`b rx_calc_dataset_item_prefetch`) disassembles with its
     target correctly resolving to the shared `_prefetch` label.
   - `randomx_calc_dataset_item_aarch64` and `randomx_init_dataset_aarch64_end` alias the exact
     same address (`0x2ad24`), and `CodeSize` (`randomx_init_dataset_aarch64_end -
     randomx_program_aarch64`) correctly equals `51364` bytes — matching the `--jit-dump`-reported
     `CodeSize` figure exactly, confirming the core invariant the whole design depends on
     (`generateSuperscalarHash()`'s output starts exactly where the unmodified `bl
     rx_calc_dataset_item`'s link-time-computed offset already points).
2. **Dynamic disassembly of the live, runtime-constructed JIT buffer.** Ran `--jit-dump`, found the
   JIT buffer's base address via `/proc/<pid>/maps`, dumped the actual bytes via `/proc/<pid>/mem`,
   and disassembled them with `objdump -D -b binary -m aarch64`. Every instruction — from the
   `sub sp, sp, #0x50` prologue entry through the `and`/`prfm` prefetch code, the mix/XOR loop, and
   the final `stp`/`ldp`/`add sp, sp, #0x50`/`ret` epilogue — matched the intended design exactly,
   byte for byte, with the branch landing precisely where `codePos` placed the next emitted
   instruction.

Both checks passed cleanly. The implementation is, as far as static and dynamic inspection can
show, byte-correct relative to its own design.

## Result: hang

`armrx_tests` (which runs interpreter-path KATs first, then JIT-mode KATs) printed the two correct
interpreter-path hashes, then hung indefinitely on the first JIT-mode hash — no output, no crash,
still running after 45+ minutes (user-observed; historical baseline for this exact binary is
14.26 seconds for the whole `armrx_tests` run). A minimal, one-shot reproduction (`armrx --jit-dump`,
which does exactly one JIT-mode hash) reproduced the identical hang.

Confirmed via `/proc/<pid>/stat` (`state: R`, CPU time advancing linearly with wall-clock) that the
process was genuinely spinning, not deadlocked on a lock or blocked in a syscall.

`perf record -p <pid>` sampling (two independent sessions, minutes apart) showed samples spread
across a wide range of addresses in **both** the main VM program region (the per-hash compiled
program, offsets ~0x210-0x2e00 within `[0, CodeSize)`) **and** the dataset-derivation region
(offsets spanning from just past `CodeSize` through just before my epilogue's `ret`, i.e. across
the *entire* derivation function including its normal, verified-correct execution path) — not
concentrated at one address. This pattern is more consistent with severely-amplified but still
*progressing* execution than with a tight infinite loop stuck at a single instruction — e.g.
consistent with a data-corruption bug that feeds wrong values into a `CBRANCH` decision somewhere
downstream, causing far more re-execution of some code range than the spec's normal ~0.4%
branch-taken rate would produce, though this is inference from the sampling pattern, not a
confirmed mechanism.

## What was ruled out

- **The copy-and-splice mechanism itself**: verified correct both statically and dynamically (see
  Verification above). The bytes that actually execute match the design exactly.
- **A buffer-overflow / undersized allocation**: the live JIT buffer's mapped region
  (`/proc/<pid>/maps`, `rwxp`) was 118,784 bytes; actual written content (start of prologue through
  the epilogue's `ret`) ended at byte offset 75,460 — comfortably within bounds, no evidence of an
  overrun.
- **The dead fast-mode call site**: not reachable from this code path at all (confirmed
  `getDatasetInitFunc()` has zero call sites), so its correctness or lack thereof is irrelevant
  here, and it was left completely unmodified regardless.
- **A caller-side (`vm_instructions_end_light`) register-liveness mistake**: re-verified line by
  line that the caller uses only x0, x1, x2, x9, x10, x20, x30, and sp — x3 and x8/x11 are
  genuinely dead to it, consistent with the design.

**The actual mechanism was not identified.** Given the failure mode is a silent-wrong-behavior risk
class (even though this specific run manifested as a hang rather than a wrong hash, the underlying
bug class — JIT-buffer-construction correctness — is exactly the kind of thing that can also
produce silently wrong hashes for *other* seeds/inputs even if this one hangs), and given how much
targeted verification failed to find the cause, the change was fully reverted rather than shipped
or narrowed speculatively.

## Revert and re-verification

`git checkout -- include/armrx/jit_compiler_a64_static.hpp src/jit_compiler_a64.cpp
src/jit_compiler_a64_static.S`, re-synced to the devbox, rebuilt. `armrx_tests` (interpreter KATs +
JIT-mode KATs) completed in under 20 seconds with all hashes correct — confirming the revert
restores the known-good state and that the diagnosis (this specific change caused the hang, not
some pre-existing or environmental issue) is correct.

Full `ctest` suite re-run afterward: **100% tests passed, 0 failed, 14/14**, total real time
3620.27s. Every single test's timing matched the pre-experiment historical baseline closely (e.g.
`bench_armrx` 347.86s vs. 345.52s baseline, `test_jit_equivalence` 114.52s vs. 113.38s,
`test_jit_scheduler_stress` 1425.51s vs. 1444.77s, `test_jit_superscalar_scheduler_stress`
1426.61s vs. 1428.09s) — not just passing, but back to the exact same performance profile as
before, with zero lingering effect from the reverted change.

## Where this leaves future work

- **If revisited, budget for a proper bisection, not more static reading.** The static/dynamic
  disassembly verification above was thorough and found nothing wrong — that means the bug (if it
  is a bug in this specific change, which is very likely given the revert fixed it) lives in
  something *not* visible to instruction-level inspection: most likely a **data-flow** issue (wrong
  *values* computed or propagated, not a wrong *instruction sequence*) rather than a
  control-flow/addressing issue. A targeted next step: dump the actual `rl[0..7]` values this
  light-prologue variant computes for a fixed, known item number and cache, and diff them against
  `generate_dataset_item()`'s (the reference C++ implementation in `src/dataset.cpp`) output for
  the same inputs — this would directly test the hypothesis that the derivation is producing wrong
  *data* despite correct-looking *instructions*.
- **The more aggressive version (also dropping x8-x11) was never attempted** — given the
  conservative version already failed for an unidentified reason, there's no reason to believe the
  more aggressive one would fare better; it should wait until this version's root cause is actually
  understood.

## 2026-07-29 — Successful retry (commit 341ebf8)

**This change was re-attempted (by Reasonix, independently verified by Hermes) and now works.**
The exact same structural change (light-mode prologue: 80-byte frame, saves x4-x13 only) was
re-implemented by Reasonix, and this time all tests pass cleanly:

- `test_jit_dataset_light` (new diagnostic): **2500/2500 passed** — the exact `rl[0..7]` data-flow
  check that the experiment doc above recommended as the next diagnostic step
- `armrx --jit-dump`: **completes normally** (the prior attempt's exact hang reproduction — no hang)
- `armrx_tests` (KATs): **passed** — interpreter and JIT outputs identical
- `test_jit_equivalence`: **16/16 pairs, all byte-identical**
- `test_jit_determinism`, `test_jit_encodings`: **passed**
- `test_mining`, `test_partial_dataset`: **ALL PASSED**

**Root cause of the original hang never conclusively identified.** Possible differentiators:
- The successful build used `-DARMRX_DISABLE_LTO=ON` (original attempt may have used LTO)
- The successful build used a different GCC version / device state (more free memory)
- The diagnostic test was created and passed first, validating data-flow before running any live
  JIT hash — this caught a data-flow bug if one existed, but none was found (all 2500 pairs matched)

The change is now shipping. Track C Phase A is complete. The experiment doc is preserved for
historical reference but this change is no longer blocked.

**Measured hashrate impact: zero.** 8-worker interleaved benchmark showed Phase A at 25.06 H/s
vs baseline at 25.08 H/s — identical within noise. The caller-frame saving (4 instructions/call)
is 0.05% of total instructions/hash. The real ABI cost is in the callee frame (x4-x13 save/restore
+ shared code).

##
- **Track C's estimated payoff (3-8%, never measured) remains unverified**, and this attempt didn't
  move that number either way — it failed before reaching a state where timing could be measured.
- **This does not cast doubt on the shared prefetch/mix code, the original `rx_calc_dataset_item`
  function, or the `CodeSize`-boundary mechanism** — all three were verified unchanged and correct,
  and the existing, shipped light-mode dataset derivation (via the original, untouched function)
  continues to work exactly as it did before this experiment.
