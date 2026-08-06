# docs/audits — ACTIVE AUDITS ONLY

This directory is **intentionally empty**. All prior agent audits, headroom
analyses, and project reviews have been **archived** (not deleted) under
`docs/archived/audits/` for historical reference. They predate recent code
changes (e.g. superscalar timing-model adopted `91f5b2f`) and must NOT be read
as current state.

## The authoritative current-state sources are:

1. **`docs/closed-levers-ledger.md`** — single source of truth for what's been
   tried, what's adopted, and what's dead. READ THIS FIRST before any perf work.
2. **`docs/briefs/`** — briefs for in-flight / adopted / closed optimization
   attempts (each marked OPEN / ADOPTED / CLOSED).
3. **`RETROSPECTIVE.md`** (repo root) — full project retrospective.
4. **`docs/changelogs.md`**, **`README.md`** (Status table) — shipped state.

## Rule
Never drop a new "audit" in here and treat it as current without reconciling it
with `closed-levers-ledger.md`. Historical audits live in `docs/archived/audits/`.
