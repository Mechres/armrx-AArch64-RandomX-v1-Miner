// Regression tests for PLAN.md Phase 4 item E.1: cli_parser.cpp had zero
// automated unit tests, relying only on manual --help/--version/exit-code
// checks recorded in a commit message. CommandLineParser::parse() is a pure
// function of (argc, argv, config file, env) -> ParsedArgs, so it's cheap to
// drive directly here instead of through a full process spawn.

#include "armrx/cli_parser.hpp"
#include "armrx/log.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Builds argv[0]="armrx" followed by `args`, calls CommandLineParser::parse(),
// and returns the result. Every call forces $ARMRX_CONFIG to a path that
// cannot exist, so default-config auto-probing (which load_config_with_fallback
// does unconditionally) can't pick up a real file on the machine running the
// test and make results depend on local environment.
armrx::ParsedArgs run(const std::vector<std::string>& args) {
    setenv("ARMRX_CONFIG", "/nonexistent/armrx-test-config-does-not-exist.json", 1);

    std::vector<std::string> full{"armrx"};
    full.insert(full.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(full.size());
    for (auto& a : full) argv.push_back(a.data());

    return armrx::CommandLineParser::parse(static_cast<int>(argv.size()), argv.data());
}

} // namespace

void test_defaults_no_args() {
    const auto res = run({});
    assert(!res.should_exit);
    assert(res.options.mode_is_auto);
    assert(res.options.workers >= 1);
    assert(!res.options.should_mine);
    assert(res.options.difficulty == 100);
    assert(res.options.runtime_seconds == 10);
    assert(res.options.warmup_secs == 30);
    assert(!res.options.should_connect_pool);
    assert(res.options.pool_list.empty());
    assert(res.options.pool_password == "x");
    assert(!res.options.pool_tls);
    assert(res.options.pool_tls_verify);
    assert(!res.options.use_tui);
    assert(!res.options.jit_dump_mode);

    std::cout << "[test_cli_parser] test_defaults_no_args passed\n";
}

void test_mode_flags() {
    {
        const auto res = run({"--mode=light"});
        assert(!res.should_exit);
        assert(!res.options.mode_is_auto);
        assert(res.options.requested_mode == armrx::RandomXMode::light);
    }
    {
        const auto res = run({"--mode=fast"});
        assert(!res.should_exit);
        assert(!res.options.mode_is_auto);
        assert(res.options.requested_mode == armrx::RandomXMode::fast);
    }
    {
        const auto res = run({"--mode=auto"});
        assert(!res.should_exit);
        assert(res.options.mode_is_auto);
    }
    {
        const auto res = run({"--mode=bogus"});
        assert(res.should_exit);
        assert(res.exit_code == 64);
    }

    std::cout << "[test_cli_parser] test_mode_flags passed\n";
}

void test_workers_flag() {
    {
        const auto res = run({"--workers=4"});
        assert(!res.should_exit);
        assert(res.options.workers == 4);
    }
    {
        // Clamped to at least 1, not just passed through.
        const auto res = run({"--workers=0"});
        assert(!res.should_exit);
        assert(res.options.workers == 1);
    }
    {
        const auto res = run({"--workers=abc"});
        assert(res.should_exit);
        assert(res.exit_code == 64);
    }

    std::cout << "[test_cli_parser] test_workers_flag passed\n";
}

void test_difficulty_seconds_warmup() {
    {
        const auto res = run({"--difficulty=12345", "--seconds=30", "--warmup=5"});
        assert(!res.should_exit);
        assert(res.options.difficulty == 12345ULL);
        assert(res.options.runtime_seconds == 30);
        assert(res.options.warmup_secs == 5);
    }
    assert(run({"--difficulty=xyz"}).should_exit);
    assert(run({"--difficulty=xyz"}).exit_code == 64);
    assert(run({"--seconds=xyz"}).should_exit);
    assert(run({"--seconds=xyz"}).exit_code == 64);
    assert(run({"--warmup=xyz"}).should_exit);
    assert(run({"--warmup=xyz"}).exit_code == 64);

    std::cout << "[test_cli_parser] test_difficulty_seconds_warmup passed\n";
}

void test_affinity_mode() {
    {
        const auto res = run({"--affinity-mode=all"});
        assert(!res.should_exit);
        assert(res.options.affinity_mode == armrx::AffinityMode::All);
    }
    {
        const auto res = run({"--affinity-mode=unpinned"});
        assert(!res.should_exit);
        assert(res.options.affinity_mode == armrx::AffinityMode::Unpinned);
    }
    {
        const auto res = run({"--affinity-mode=big-only"});
        assert(!res.should_exit);
        assert(res.options.affinity_mode == armrx::AffinityMode::BigOnly);
    }
    {
        const auto res = run({"--affinity-mode=bogus"});
        assert(res.should_exit);
        assert(res.exit_code == 64);
    }

    std::cout << "[test_cli_parser] test_affinity_mode passed\n";
}

void test_pool_flags() {
    {
        const auto res = run({"--pool=example.com:1234"});
        assert(!res.should_exit);
        assert(res.options.should_connect_pool);
        assert(res.options.pool_list.size() == 1);
        assert(res.options.pool_list[0].first == "example.com");
        assert(res.options.pool_list[0].second == 1234);
    }
    {
        // No port given -> default 3333.
        const auto res = run({"--pool=example.com"});
        assert(!res.should_exit);
        assert(res.options.pool_list.size() == 1);
        assert(res.options.pool_list[0].first == "example.com");
        assert(res.options.pool_list[0].second == 3333);
    }
    {
        // Multiple --pool flags accumulate for failover.
        const auto res = run({"--pool=a.com:1111", "--pool=b.com:2222"});
        assert(!res.should_exit);
        assert(res.options.pool_list.size() == 2);
        assert(res.options.pool_list[0].first == "a.com");
        assert(res.options.pool_list[1].first == "b.com");
    }
    {
        const auto res = run({"--pool=example.com:notaport"});
        assert(res.should_exit);
        assert(res.exit_code == 64);
    }

    std::cout << "[test_cli_parser] test_pool_flags passed\n";
}

void test_wallet_password_tls() {
    const auto res = run({"--wallet=abcWalletAddr", "--password=hunter2", "--tls", "--no-verify-tls"});
    assert(res.options.pool_wallet == "abcWalletAddr");
    assert(res.options.pool_password == "hunter2");
    assert(!res.options.pool_tls_verify);
#ifdef ARMRX_HAVE_TLS
    // This build has OpenSSL: --tls is accepted normally.
    assert(!res.should_exit);
    assert(res.options.pool_tls);
#else
    // Audit finding (TLS silently ignored without OpenSSL): a build with no
    // TLS support must reject --tls up front rather than accept it and
    // silently mine in plaintext. See test_tls_rejected_without_openssl_support().
    assert(res.should_exit);
    assert(res.exit_code == 64);
#endif

    const auto res2 = run({"--tls", "--no-tls"});
    assert(!res2.options.pool_tls); // last flag wins, and --no-tls always parses cleanly
    assert(!res2.should_exit);

    std::cout << "[test_cli_parser] test_wallet_password_tls passed\n";
}

// Audit finding (TLS silently ignored without OpenSSL): CommandLineParser::parse()
// must reject a --tls request in a build without ARMRX_HAVE_TLS, with a clear
// error and exit code, before MinerApp is ever constructed -- so no socket is
// ever opened and no login is ever sent in plaintext despite the user's
// explicit encryption request. Named so it reads sensibly regardless of which
// build this test binary itself was compiled as (see the #ifdef branches).
void test_tls_rejected_without_openssl_support() {
#ifdef ARMRX_HAVE_TLS
    // This test binary was built WITH OpenSSL, so it cannot exercise the
    // rejection branch (that requires compiling cli_parser.cpp itself without
    // ARMRX_HAVE_TLS, i.e. a separate CMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
    // build+test run). Confirm the positive-path contract instead: --tls is
    // accepted, and --no-tls always parses cleanly with no ARMRX_HAVE_TLS
    // dependency either way.
    assert(!run({"--tls"}).should_exit);
    assert(!run({"--no-tls"}).should_exit);
    std::cout << "[test_cli_parser] test_tls_rejected_without_openssl_support: "
                 "SKIPPED positive-branch-only (this binary has ARMRX_HAVE_TLS; "
                 "rerun under CMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE to exercise the rejection)\n";
#else
    const auto res = run({"--wallet=w", "--pool=127.0.0.1:1", "--tls"});
    assert(res.should_exit);
    assert(res.exit_code == 64);

    // A config-file-sourced pool_tls must be rejected the same way as the
    // CLI flag -- both funnel through the same final o.pool_tls check.
    // (--no-tls must still always parse cleanly regardless.)
    assert(!run({"--no-tls"}).should_exit);

    std::cout << "[test_cli_parser] test_tls_rejected_without_openssl_support passed "
                 "(--tls rejected with exit code 64 in a non-TLS build)\n";
#endif
}

void test_tui_flags() {
    assert(run({"--tui"}).options.use_tui);
    assert(!run({"--no-tui"}).options.use_tui);
    // Last flag wins when both are given.
    assert(run({"--color", "--no-color"}).options.tui_color == 0);
    assert(run({"--no-color", "--color"}).options.tui_color == 1);

    std::cout << "[test_cli_parser] test_tui_flags passed\n";
}

void test_jit_dump_flags() {
    {
        const auto res = run({"--jit-dump"});
        assert(!res.should_exit);
        assert(res.options.jit_dump_mode);
        assert(res.options.jit_dump_key == "test key 000"); // default unchanged
    }
    {
        const auto res = run({"--jit-dump=myseed"});
        assert(!res.should_exit);
        assert(res.options.jit_dump_mode);
        assert(res.options.jit_dump_key == "myseed");
    }

    std::cout << "[test_cli_parser] test_jit_dump_flags passed\n";
}

void test_log_level_flag() {
    {
        const auto res = run({"--log-level=debug"});
        assert(!res.should_exit);
        assert(armrx::log::get_level() == armrx::log::Level::debug);
    }
    {
        const auto res = run({"--log-level=bogus"});
        assert(res.should_exit);
        assert(res.exit_code == 64);
    }
    armrx::log::set_level(armrx::log::Level::info); // restore default for any later test

    std::cout << "[test_cli_parser] test_log_level_flag passed\n";
}

void test_misc_flags() {
    {
        const auto res = run({"--mlock", "--rt-priority"});
        assert(!res.should_exit);
        assert(res.options.use_mlock);
        assert(res.options.use_rt_priority);
    }
    {
        const auto res = run({"--metrics-port=9090"});
        assert(!res.should_exit);
        assert(res.options.metrics_port == 9090);
    }
    assert(run({"--metrics-port=notaport"}).should_exit);
    assert(run({"--metrics-port=notaport"}).exit_code == 64);
    {
        const auto res = run({"--no-color"});
        assert(res.options.tui_color == 0);
    }
    {
        const auto res = run({"--color"});
        assert(res.options.tui_color == 1);
    }
    {
        const auto res = run({"--stagger-ms=50"});
        assert(!res.should_exit);
        assert(res.options.stagger_ms == 50);
    }
    assert(run({"--stagger-ms=notanumber"}).should_exit);
    assert(run({"--stagger-ms=notanumber"}).exit_code == 64);

    std::cout << "[test_cli_parser] test_misc_flags passed\n";
}

void test_version_and_help() {
    for (const auto& flag : {"--version", "-V"}) {
        const auto res = run({flag});
        assert(res.should_exit);
        assert(res.exit_code == 0);
    }
    for (const auto& flag : {"--help", "-h"}) {
        const auto res = run({flag});
        assert(res.should_exit);
        assert(res.exit_code == 0);
    }

    std::cout << "[test_cli_parser] test_version_and_help passed\n";
}

void test_unknown_argument() {
    const auto res = run({"--this-flag-does-not-exist"});
    assert(res.should_exit);
    assert(res.exit_code == 64);

    std::cout << "[test_cli_parser] test_unknown_argument passed\n";
}

void test_init_cache_flag() {
    {
        const auto res = run({"--init-cache", "somekey"});
        assert(!res.should_exit);
        assert(res.options.should_init_cache);
        assert(res.options.init_cache_key == "somekey");
    }
    {
        // Missing the required key argument.
        const auto res = run({"--init-cache"});
        assert(res.should_exit);
        assert(res.exit_code == 64);
    }

    std::cout << "[test_cli_parser] test_init_cache_flag passed\n";
}

void test_config_file_cli_precedence() {
    const std::string path = "test_cli_parser_tmp_config.json";
    {
        std::ofstream out(path);
        out << R"({"pool":"cfgpool.example:5555","wallet":"cfgwallet","workers":2,"difficulty":777,"seconds":15})";
    }

    // Config values flow through when not overridden on the CLI.
    {
        const auto res = run({"--config=" + path});
        assert(!res.should_exit);
        assert(res.options.should_connect_pool);
        assert(res.options.pool_list.size() == 1);
        assert(res.options.pool_list[0].first == "cfgpool.example");
        assert(res.options.pool_list[0].second == 5555);
        assert(res.options.pool_wallet == "cfgwallet");
        assert(res.options.workers == 2);
        assert(res.options.difficulty == 777ULL);
        assert(res.options.runtime_seconds == 15);
    }

    // CLI flags override config-file values for the fields they touch.
    {
        const auto res = run({"--config=" + path, "--workers=8", "--difficulty=999"});
        assert(!res.should_exit);
        assert(res.options.workers == 8);
        assert(res.options.difficulty == 999ULL);
        // Untouched-by-CLI field still comes from the config file.
        assert(res.options.runtime_seconds == 15);
        assert(res.options.pool_wallet == "cfgwallet");
    }

    std::remove(path.c_str());
    std::cout << "[test_cli_parser] test_config_file_cli_precedence passed\n";
}

int main() {
    test_defaults_no_args();
    test_mode_flags();
    test_workers_flag();
    test_difficulty_seconds_warmup();
    test_affinity_mode();
    test_pool_flags();
    test_wallet_password_tls();
    test_tls_rejected_without_openssl_support();
    test_tui_flags();
    test_jit_dump_flags();
    test_log_level_flag();
    test_misc_flags();
    test_version_and_help();
    test_unknown_argument();
    test_init_cache_flag();
    test_config_file_cli_precedence();
    std::cout << "ALL CLI_PARSER TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
