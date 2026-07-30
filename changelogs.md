# armrx — Changelog

> **Post-alpha archive:** For the full retrospective covering all 210 commits (2026-07-13 to 2026-07-30) and 10 performance tracks A–J, see [`RETROSPECTIVE.md`](RETROSPECTIVE.md).
>
> The complete alpha-phase changelog is preserved at
> [`docs/archived/alpha-changelogs.md`](docs/archived/alpha-changelogs.md).

## 2026-08-01
- **Track G — NEON T-table AES enabled by default:** Flipped `ARMRX_ENABLE_NEON_TTABLE_AES` from OFF to ON in CMakeLists.txt. Code already implemented and KAT-verified (10,000-trial parity test, +28.8% AES throughput microbenchmark). Projected ~3.6% hashrate gain. See `docs/audits/combined-audit-20260731.md` (T0-1). (`CMakeLists.txt`)
