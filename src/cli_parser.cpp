#include "armrx/cli_parser.hpp"
#include "armrx/config.hpp"
#include "armrx/log.hpp"

#include <algorithm>
#include <iostream>
#include <string_view>
#include <thread>

namespace armrx {

ParsedArgs CommandLineParser::parse(int argc, char** argv) {
    ParsedArgs result;
    MinerOptions& o = result.options;

    o.workers = std::max(1U, std::thread::hardware_concurrency());

    // Load config from file (CLI overrides below)
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        if (a.rfind("--config=", 0) == 0) {
            config_path = std::string{a.substr(9)};
            break;
        }
    }
    {
        auto cfg = armrx::load_config_with_fallback(config_path);
        for (auto& p : cfg.pools) o.pool_list.push_back({p.host, p.port});
        if (!cfg.wallet.empty()) o.pool_wallet = cfg.wallet;
        if (cfg.password != "x") o.pool_password = cfg.password;
        o.pool_tls = cfg.pool_tls;
        if (cfg.workers > 0) o.workers = cfg.workers;
        if (cfg.mode != "auto") {
            o.mode_is_auto = false;
            if (cfg.mode == "light") o.requested_mode = armrx::RandomXMode::light;
            else if (cfg.mode == "fast") o.requested_mode = armrx::RandomXMode::fast;
        }
        o.difficulty = cfg.difficulty;
        o.runtime_seconds = cfg.seconds;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument.rfind("--config=", 0) == 0) {
            // Already consumed by the pre-scan above (config must be loaded
            // before CLI overrides are applied); just skip it here so it
            // doesn't fall through to "Unknown argument" below.
            continue;
        }

        if (argument.rfind("--mode=", 0) == 0) {
            const auto mode_text = argument.substr(7);
            if (mode_text == "auto") {
                o.mode_is_auto = true;
            } else if (mode_text == "light") {
                o.requested_mode = armrx::RandomXMode::light;
                o.mode_is_auto = false;
            } else if (mode_text == "fast") {
                o.requested_mode = armrx::RandomXMode::fast;
                o.mode_is_auto = false;
            } else {
                std::cerr << "Invalid mode: " << mode_text << '\n';
                result.should_exit = true;
                result.exit_code = 64;
                return result;
            }
            continue;
        }

        if (argument.rfind("--workers=", 0) == 0) {
            try {
                o.workers = std::max(1U, static_cast<unsigned>(std::stoul(std::string{argument.substr(10)})));
            } catch (...) {
                std::cerr << "Invalid --workers value: " << argument.substr(10) << '\n';
                result.should_exit = true;
                result.exit_code = 64;
                return result;
            }
            continue;
        }

        if (argument == "--init-cache") {
            if (i + 1 >= argc) {
                std::cerr << "--init-cache requires a key\n";
                result.should_exit = true;
                result.exit_code = 64;
                return result;
            }
            o.should_init_cache = true;
            o.init_cache_key = argv[++i];
            continue;
        }

        if (argument == "--mine") {
            o.should_mine = true;
            continue;
        }

        if (argument.rfind("--difficulty=", 0) == 0) {
            try {
                o.difficulty = std::stoull(std::string{argument.substr(13)});
            } catch (...) {
                std::cerr << "Invalid --difficulty value: " << argument.substr(13) << '\n';
                result.should_exit = true;
                result.exit_code = 64;
                return result;
            }
            continue;
        }

        if (argument.rfind("--seconds=", 0) == 0) {
            try {
                o.runtime_seconds = static_cast<unsigned>(std::stoul(std::string{argument.substr(10)}));
            } catch (...) {
                std::cerr << "Invalid --seconds value: " << argument.substr(10) << '\n';
                result.should_exit = true;
                result.exit_code = 64;
                return result;
            }
            continue;
        }

        if (argument.rfind("--warmup=", 0) == 0) {
            try {
                o.warmup_secs = static_cast<unsigned>(std::stoul(std::string{argument.substr(9)}));
            } catch (...) {
                std::cerr << "Invalid --warmup value: " << argument.substr(9) << '\n';
                result.should_exit = true;
                result.exit_code = 64;
                return result;
            }
            continue;
        }

        if (argument.rfind("--affinity-mode=", 0) == 0) {
            const auto aff_text = argument.substr(16);
            if (aff_text == "all") {
                o.affinity_mode = armrx::AffinityMode::All;
            } else if (aff_text == "unpinned") {
                o.affinity_mode = armrx::AffinityMode::Unpinned;
            } else if (aff_text == "big-only") {
                o.affinity_mode = armrx::AffinityMode::BigOnly;
            } else {
                std::cerr << "Invalid affinity mode: " << aff_text << '\n';
                result.should_exit = true;
                result.exit_code = 64;
                return result;
            }
            continue;
        }

        // ── Pool / stratum arguments ──────────────────────────────────────
        if (argument.rfind("--pool=", 0) == 0) {
            // Format: --pool=host:port  or  --pool=host  (default port 3333)
            // Multiple --pool flags are accepted for failover
            o.should_connect_pool = true;
            std::string addr{argument.substr(7)};
            const auto colon = addr.rfind(':');
            std::string host;
            std::uint16_t port = 3333;
            if (colon != std::string::npos) {
                host = addr.substr(0, colon);
                try {
                    port = static_cast<std::uint16_t>(std::stoul(addr.substr(colon + 1)));
                } catch (...) {
                    std::cerr << "Invalid --pool port: " << addr.substr(colon + 1) << '\n';
                    result.should_exit = true;
                    result.exit_code = 64;
                    return result;
                }
            } else {
                host = std::move(addr);
            }
            o.pool_list.emplace_back(std::move(host), port);
            continue;
        }

        if (argument.rfind("--wallet=", 0) == 0) {
            o.pool_wallet = std::string{argument.substr(9)};
            continue;
        }

        if (argument.rfind("--password=", 0) == 0) {
            o.pool_password = std::string{argument.substr(11)};
            continue;
        }

        if (argument == "--tls") {
            o.pool_tls = true;
            continue;
        }
        if (argument == "--no-tls") {
            o.pool_tls = false;
            continue;
        }
        if (argument == "--no-verify-tls") {
            o.pool_tls_verify = false;
            continue;
        }
        if (argument == "--tui") {
            o.use_tui = true;
            continue;
        }
        if (argument == "--no-tui") {
            o.use_tui = false;
            continue;
        }
        if (argument == "--jit-dump") {
            o.jit_dump_mode = true;
            continue;
        }
        if (argument.rfind("--jit-dump=", 0) == 0) {
            o.jit_dump_mode = true;
            o.jit_dump_key = std::string{argument.substr(11)};
            continue;
        }
        if (argument.rfind("--log-level=", 0) == 0) {
            std::string lv{argument.substr(12)};
            if (lv == "trace") armrx::log::set_level(armrx::log::Level::trace);
            else if (lv == "debug") armrx::log::set_level(armrx::log::Level::debug);
            else if (lv == "info")  armrx::log::set_level(armrx::log::Level::info);
            else if (lv == "warn")  armrx::log::set_level(armrx::log::Level::warn);
            else if (lv == "error") armrx::log::set_level(armrx::log::Level::error);
            else {
                std::cerr << "Invalid --log-level: " << lv << " (choose: trace, debug, info, warn, error)\n";
                result.should_exit = true;
                result.exit_code = 64;
                return result;
            }
            continue;
        }
        if (argument == "--mlock") {
            o.use_mlock = true;
            continue;
        }
        if (argument == "--rt-priority") {
            o.use_rt_priority = true;
            continue;
        }
        if (argument.rfind("--metrics-port=", 0) == 0) {
            try {
                o.metrics_port = static_cast<std::uint16_t>(std::stoul(std::string{argument.substr(15)}));
            } catch (...) {
                std::cerr << "Invalid --metrics-port value: " << argument.substr(15) << '\n';
                result.should_exit = true;
                result.exit_code = 64;
                return result;
            }
            continue;
        }
        if (argument == "--no-color") {
            o.tui_color = false;
            continue;
        }
        if (argument == "--color") {
            o.tui_color = true;
            continue;
        }
        if (argument.rfind("--stagger-ms=", 0) == 0) {
            try {
                o.stagger_ms = static_cast<unsigned>(std::stoul(std::string{argument.substr(13)}));
            } catch (...) {
                std::cerr << "Invalid --stagger-ms value: " << argument.substr(13) << '\n';
                result.should_exit = true;
                result.exit_code = 64;
                return result;
            }
            continue;
        }

        if (argument == "--version" || argument == "-V") {
            std::cout << "armrx " << ARMRX_VERSION
                      << " (" << ARMRX_GIT_SHA << ", built " << ARMRX_BUILD_DATE << ")\n"
                      << "  AArch64 JIT: "
#ifdef ARMRX_HAVE_JIT
                      << "enabled"
#else
                      << "disabled (interpreted VM only)"
#endif
                      << "\n"
                      << "  TLS: "
#ifdef ARMRX_HAVE_TLS
                      << "enabled"
#else
                      << "disabled"
#endif
                      << "\n";
            result.should_exit = true;
            result.exit_code = 0;
            return result;
        }

        if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: armrx [options]\n"
                << "Options:\n"
                << "  --mode=auto|light|fast     Select execution mode (default: auto)\n"
                << "  --workers=N                Set thread count (default: all online cores)\n"
                << "  --init-cache <key>         Perform Argon2d cache initialization benchmark\n"
                << "\n"
                << "Local benchmark:\n"
                << "  --mine                     Start local RandomX miner benchmark\n"
                << "  --difficulty=N             Set miner target difficulty (default: 100)\n"
                << "  --seconds=S                Duration to run benchmark in seconds, 0 for infinite (default: 10)\n"
                << "  --warmup=W                 Warmup seconds before steady-state measurement (default: 30)\n"
                << "\n"
                << "Pool mining (Stratum V1):\n"
                << "  --pool=host[:port]         Pool address (default port: 3333); multiple allowed for failover\n"
                << "  --wallet=<address>         Monero wallet address (worker login)\n"
                << "  --password=<pw>            Worker password (default: x)\n"
                << "  --tls / --no-tls          Enable TLS encryption (default: off, requires OpenSSL)\n"
                << "  --no-verify-tls          Skip TLS certificate verification (default: verify)\n"
                << "  --config=<path>           Config file path (default: ~/.config/armrx/config.json)\n"
                << "  --tui / --no-tui          Terminal UI dashboard (default: off)\n"
                << "  --no-color                Disable ANSI color in TUI output\n"
                << "  --color                   Force ANSI color even when piped\n"
                << "  --version, -V             Print version and build info\n"
                << "  --mlock                   Lock all pages into RAM (prevents swapping)\n"
                << "  --rt-priority             Set SCHED_FIFO real-time priority for workers\n"
                << "  --stagger-ms=<ms>         Startup stagger per worker (ms) to reduce memory contention\n"
                << "  --metrics-port=<port>     Prometheus HTTP metrics endpoint (default: disabled)\n"
                << "  --log-level=<level>       Log verbosity: trace, debug, info, warn, error (default: info)\n"
                << "\n"
                << "  --help, -h               Display this help menu\n"
                << "\n"
                << "JIT introspection:\n"
                << "  --jit-dump[=<seed>]      Compile one program and dump JIT code with opcode boundaries\n";
            result.should_exit = true;
            result.exit_code = 0;
            return result;
        }

        std::cerr << "Unknown argument: " << argument << '\n';
        result.should_exit = true;
        result.exit_code = 64;
        return result;
    }

    return result;
}

} // namespace armrx
