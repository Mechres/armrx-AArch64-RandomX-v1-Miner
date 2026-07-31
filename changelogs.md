# armrx — Changelog

> **Post-alpha archive:** For the full retrospective covering all 210 commits (2026-07-13 to 2026-07-30) and 10 performance tracks A–J, see [`RETROSPECTIVE.md`](RETROSPECTIVE.md).
>
> The complete alpha-phase changelog is preserved at
> [`docs/archived/alpha-changelogs.md`](docs/archived/alpha-changelogs.md).

## 2026-08-01
- **Track G — NEON T-table AES enabled by default:** Flipped `ARMRX_ENABLE_NEON_TTABLE_AES` from OFF to ON in CMakeLists.txt. Code already implemented and KAT-verified (10,000-trial parity test, +28.8% AES throughput microbenchmark). Projected ~3.6% hashrate gain. See `docs/audits/combined-audit-20260731.md` (T0-1). (`CMakeLists.txt`)
- **isolcpus-aware worker pinning (T2-3):** Worker threads now pin exclusively to isolated cores when `isolcpus=` is active. `detect_core_order()` applies an isolation filter (`filter_to_isolated`) on ALL return paths — including the cpufreq-less fallback that previously returned the raw sequential order (verified bug on MSM8929: worker 0 stole housekeeping core 0, core 7 left idle). Default worker count also capped to isolated-core count (`cli_parser.cpp`). On-device verification: `isolcpus detected — workers pinned to isolated cores: 1, 2, 3, 4, 5, 6, 7`, main thread on core 0, all 7 workers on cores 1-7. See `docs/audits/combined-audit-20260731.md` (T2-3). (`src/mining_engine.cpp`, `src/cli_parser.cpp`, `src/cpu_features.cpp`, `include/armrx/cpu_features.hpp`)
