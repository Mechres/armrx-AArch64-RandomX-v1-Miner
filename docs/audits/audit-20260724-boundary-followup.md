# Audit Pass — Clean-Room Boundary Follow-up + Fresh-Eyes Review (2026-07-24)

**Scope:** post-`ad5ca31` consistency sweep, verification of previously documented gaps,
a three-track fresh-eyes review (JIT encodings / Stratum parsing / config consumption),
range-validation hardening, and the first static instruction count toward `PLAN.md`
Phase 6 item 14.

---

## 1. Cross-doc consistency after the clean-room boundary (commit `ad5ca31`)

The commit covered `PLAN.md` / `NEXT_STEPS.md` / `ROADMAP.md` / `changelogs.md`, but six
other places still carried the old "inspect XMRig's generated code" framing. All fixed:

| File | Stale content | Fix |
|---|---|---|
| `docs/plans/peephole-jit-plan.md` | Entire methodology is XMRig disassembly (guiding principle 2, Phase 1) | Superseded-banner at top; body kept as unedited historical record |
| `docs/plans/performance-master-plan-20260724.md` (L2 row) | Verbatim old "`--jit-dump` + objdump" XMRig comparison | Reframed to item 14's self-directed reconciliation |
| `docs/plans/performance-master-plan.md` (L1 row) | "3–6 week clean-room effort vs. XMRig disassembly" | Boundary note added |
| `docs/audits/performance-improvement-audit.md` | Same phrasing | Boundary note added |
| `docs/plans/performance-next-agent-handoff.md` (item 11, Stage 4 step 4) | "Broader XMRig emitted-code comparison" as a live lead | Struck through, marked permanently ruled out |
| `HANDOFF_CLAUDE.md` (uncommitted) | "clean-room effort against XMRig's disassembly per prior estimates" | Reworded to boundary + item 14 gate |

Deliberately **not** touched: `OPTIMIZATION_REFERENCE.md`'s "What XMRig Does Differently
(from source analysis)" section and item 3's findings — historical record of already-done
work, kept per the append-only convention.

## 2. Stale "known gaps" — already fixed in committed code, docs corrected

Two gaps listed as open turned out to be already fixed (and marked `[x]` in
`NEXT_STEPS.md:189-190`), but the untracked `CLAUDE.md` still described them as open:

- `config.cpp` numeric config-file fields: try/catch guards present (`src/config.cpp`,
  with explanatory comment) + regression test `tests/test_config.cpp`. ✅ CLAUDE.md updated.
- `MetricsExporter::server_fd_`: already `std::atomic<int>` (`metrics.hpp:91`). ✅ CLAUDE.md updated.
- Light-mode 256 MiB Argon2 cache huge-page verification: **genuinely still open** (top perf lead).

Also updated `CLAUDE.md`'s CTest caveat: the "Not Run / BAD_COMMAND" flake **reproduced
again** this session (7/12 in one `devbox_full` run; immediate filtered re-run passed 5/5)
— it's intermittent, not resolved as the 2026-07-22 note claimed.

## 3. Untracked-files audit

- `build_pgo/` — does not exist (only gitignored `build/`). Non-issue.
- `docs/plans/performance-master-plan.md`, `docs/plans/performance-master-plan-20260724.md`,
  `docs/archived/plan_completed_phases_1-5.md` — all already git-tracked. Non-issue.
- Real working-tree state: modified `HANDOFF_CLAUDE.md` + untracked `CLAUDE.md` (both
  intentional, now including this session's edits).

## 4. Fresh-eyes review (3 parallel subagent tracks) + fixes

### 4a. JIT compiler encodings (`src/jit_compiler_a64.cpp`, `.hpp`, `static.S`)
- **0 encoding errors, 0 W^X issues, 0 branch off-by-one errors.** Spot-checks of the
  tricky encodings (UBFX, MOVZ/MOVN/MOVK hw shifts, IROL negation, CBRANCH TST/B.cond,
  smov/umov lanes) all correct.
- **MEDIUM (fixed):** stale buffer-layout comment claimed `.fill` reserves 16 words/insn
  (6144 slots); `static.S` actually reserves **32** (12288), and the fast div/sqrt path
  emits ~20 words — "correcting" the `.fill` to match the comment would have overflowed.
  Comment rewritten with an explicit do-not-shrink warning.
- **LOW (fixed):** `h_IMUL_RCP`'s LDR-literal offset lacked the 19-bit imm19 mask the
  superscalar path already applied. Safe under current layout (literals sit above code, so
  offset is always small-positive), but a hypothetical negative offset would have spilled
  into opcode bits [31:24] via the `<<5` shift. Mask added for defensive parity.

### 4b. Stratum/pool network input (`stratum_client.cpp`, `pool_manager.cpp`)
- **No memory-safety or UB findings.** All indexing/substr bounded; the one throwing parse
  (`stod` in set_difficulty) is guarded; div-by-zero guarded in `difficulty_to_target`.
- Two informational items (not fixed, documented here): `hex_to_bytes` silently coerces
  malformed hex (non-hex→0, odd length truncates) instead of rejecting; no early
  blob-length sanity check (safely caught downstream). Neither is exploitable.

### 4c. Config consumption path (`config.cpp`, `cli_parser.cpp`, `json.cpp`)
Two real gaps the existing try/catch guards **cannot** catch — both fixed:

- **M1 — silent narrowing truncation:** `stoul` succeeds for huge values, then the cast
  truncates: `"port": 65539` → port 3; `workers: 4294967297` → 1.
- **M2 — negative wrap:** `std::stoul("-1")` is valid per the standard →
  `workers: -1` = 4,294,967,295 → ~4B-thread spawn attempt (resource-exhaustion DoS from a
  hostile auto-probed `./armrx.conf`).

**Fix:** shared `parse_bounded_ull()` helper (rejects leading `-`; enforces per-field
maxima: ports ≤ 65535, workers ≤ 4096, stagger-ms ≤ 60000; throws `std::out_of_range` so
existing catch blocks handle it) applied at **all 10 numeric parse sites** across
`src/config.cpp` (4) and `src/cli_parser.cpp` (7 incl. warmup). Error policies preserved:
config file warns + continues, CLI exits 64.

Low-severity items noted but not fixed (same trust domain, cosmetic, or robustness-only):
JSON key-in-string-value spoofing heuristic, `get_string` escape mangling (`\n`→`n`),
quoted-boolean `"true"` mismatch in `get_raw`, unescaped config values echoed to terminal,
IPv6 `rfind(':')` mis-split, `password: "x"` sentinel collision.

## 5. Item 14 — first static instruction count (self-analysis only, inside the boundary)

Counted per-region instructions directly from `jit_compiler_a64_static.S` labels and the
`generateSuperscalarHash()` emission code:

- Fixed wrapper per `rx_calc_dataset_item` call: entry 36 + 8 rounds × (AND 1 + prefetch 3
  + jump 1 + mix 12 + reg-update 1) + store 13 ≈ **~185 insns/call → ~3.0M/hash (~2.3%)**.
- Light-mode main-loop fixed per-iteration overhead (~270/iter incl. `v2_FE_mix_soft_aes`
  at 189) ≈ **~4.4M/hash**.

**Key result:** item 13's 58.4M (variable superscalar) + ~7.4M (all fixed JIT wrappers) +
~4–5M (main VM program body) ≈ 70M of the measured ~132.93M — leaving **~60M insns/hash
unaccounted in emitted code entirely**. The remainder almost certainly lives in the C++
side (software T-table AES scratchpad fill/hash, Blake2b, superscalar-adjacent C++).
**Implication:** item 14's next step is `perf record` symbol-level self-attribution of
armrx's own binary, not more JIT instrumentation. Recorded in `PLAN.md` item 14.

## 6. Verification

- **x86 host:** clean build; `ctest` **7/7 passed** (incl. `test_config`,
  `test_cli_parser`, full KATs).
- **Device (Cortex-A53):** sync + build clean (rev `ad5ca31` + local edits); all 12 tests
  green — 7 initially showed the known BAD_COMMAND CTest flake, filtered re-run of
  `armrx_tests`/`test_mining`/`test_jit_encodings`/`test_jit_determinism`/
  `test_jit_equivalence` passed **5/5** (covers the imm19-mask change on real hardware).
- **Targeted behavioral checks (on-device + local):** `--workers=-1`,
  `--workers=4294967297`, `--metrics-port=65539`, `--pool=host:65539`, `--difficulty=-1`,
  `--stagger-ms=999999` all exit 64 with clear messages; `--workers=8` unaffected; hostile
  config file via `$ARMRX_CONFIG` warns and continues without crashing.

## Files changed

**Code:** `src/config.cpp`, `src/cli_parser.cpp`, `src/jit_compiler_a64.cpp`
**Docs:** `docs/plans/peephole-jit-plan.md`, `docs/plans/performance-master-plan.md`,
`docs/plans/performance-master-plan-20260724.md`, `docs/audits/performance-improvement-audit.md`,
`docs/plans/performance-next-agent-handoff.md`, `HANDOFF_CLAUDE.md`, `CLAUDE.md`, `PLAN.md`,
`changelogs.md`, plus this file.
