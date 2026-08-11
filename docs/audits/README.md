# docs/audits — ACTIVE AUDITS ONLY

This directory is **intentionally empty**. All prior agent audits, headroom
analyses, and project reviews have been **archived** (not deleted) under
`docs/archived/audits/` for historical reference. They predate recent code
changes (e.g. superscalar timing-model — `91f5b2f` was later found to be a
no-op and reverted by `8cdf311`; see `docs/closed-levers-ledger.md`) and must NOT be
read as current state.

## The authoritative current-state sources are:

1. **`docs/closed-levers-ledger.md`** — single source of truth for what's been
   tried, what's adopted, and what's dead. READ THIS FIRST before any perf work.
2. **`docs/archived/briefs/`** — historical one-run optimization briefs
   (marked OPEN / ADOPTED / CLOSED). The live working dir `docs/briefs/` is
   recreated per agent run and intentionally NOT committed (one-time artifacts);
   once a run concludes its brief is moved here for reference.
3. **`RETROSPECTIVE.md`** (repo root) — full project retrospective.
4. **`docs/changelogs.md`**, **`README.md`** (Status table) — shipped state.

## Rule
Never drop a new "audit" in here and treat it as current without reconciling it
with `closed-levers-ledger.md`. Historical audits live in `docs/archived/audits/`.
