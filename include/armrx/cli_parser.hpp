#pragma once

#include "armrx/randomx_config.hpp"
#include "armrx/mining_engine.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace armrx {

// Fully-resolved miner configuration: config-file values with CLI overrides
// applied, in the same precedence order the original main() used (config
// file first, then CLI flags override it).
struct MinerOptions {
    bool mode_is_auto = true;
    RandomXMode requested_mode = RandomXMode::light;
    unsigned workers = 0;

    bool should_init_cache = false;
    std::string init_cache_key;

    bool should_mine = false;
    std::uint64_t difficulty = 100;
    unsigned runtime_seconds = 10;
    unsigned warmup_secs = 30;

    // Pool / stratum options
    bool should_connect_pool = false;
    std::vector<std::pair<std::string, std::uint16_t>> pool_list;
    std::string pool_wallet;
    std::string pool_password = "x";
    bool pool_tls = false;
    bool pool_tls_verify = true;
    bool use_tui = false;
    int tui_color = -1; // -1 = auto-detect, 0 = no-color, 1 = color
    bool use_mlock = false;
    bool use_rt_priority = false;
    unsigned stagger_ms = 0;
    std::uint16_t metrics_port = 0; // 0 = disabled
    AffinityMode affinity_mode = AffinityMode::All;

    // Pool self-terminating test mode: makes `run_pool_mining` honor
    // --seconds (self-terminate, no Ctrl+C/kill -9 needed) and print a
    // per-worker steady-state summary at the end. Useful for reproducible
    // A/B measurement sweeps on the real pool workload. Mining is unchanged
    // (still connects + submits shares); only the run length + reporting differ.
    bool pool_test = false;

    bool jit_dump_mode = false;
    std::string jit_dump_key = "test key 000";

    // Partial dataset (Track B): size in MiB, 0 = disabled
    std::size_t dataset_mb = 0;
};

// Outcome of parsing: either a fully-resolved MinerOptions to run with, or
// an early-exit request (--help, --version, or an invalid argument) that
// already printed its message and carries the process exit code to use.
struct ParsedArgs {
    MinerOptions options;
    bool should_exit = false;
    int exit_code = 0;
};

/// Parses argv into a MinerOptions, after first loading defaults from the
/// config file (--config=<path>, or the default search locations). CLI
/// flags override config-file values. Handles --help/--version/invalid
/// arguments by printing directly and requesting an early exit.
class CommandLineParser {
public:
    static ParsedArgs parse(int argc, char** argv);
};

} // namespace armrx
