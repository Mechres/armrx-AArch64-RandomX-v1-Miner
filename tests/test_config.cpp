// Regression test for PLAN.md Phase 4 item B: config.cpp's numeric
// config-file fields (workers/difficulty/seconds/pool port) previously had
// no exception guards around their std::stoul/std::stoull conversions,
// unlike cli_parser.cpp's equivalent CLI flags, which already catch and
// report malformed values cleanly. load_config_with_fallback() runs
// unconditionally on every launch (auto-probing $ARMRX_CONFIG /
// ~/.config/armrx/config.json / ./armrx.conf even with no --config= flag),
// so a malformed default config used to crash the whole process via an
// unhandled std::invalid_argument/std::out_of_range.

#include "armrx/config.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {

// Writes `contents` to a temp file, calls load_config() on it, then removes
// the file. Returns the parsed config.
armrx::AppConfig load_from_temp_json(const std::string& name, const std::string& contents) {
    const std::string path = "test_config_tmp_" + name + ".json";
    {
        std::ofstream out(path);
        out << contents;
    }
    auto cfg = armrx::load_config(path);
    std::remove(path.c_str());
    return cfg;
}

} // namespace

void test_malformed_numeric_fields_dont_crash() {
    // Malformed workers/difficulty/seconds and a bad pool port — all in one
    // config, matching a real-world "someone fat-fingered the file" case.
    const auto cfg = load_from_temp_json("malformed",
        R"({"pool":"example.com:notaport","workers":"abc","difficulty":"xyz","seconds":"nope"})");

    // Must not have thrown (load_config() returning at all proves this, but
    // assert on the actual field values too): malformed fields are left at
    // AppConfig's defaults rather than propagating a bogus parse.
    assert(cfg.pools.size() == 1);
    assert(cfg.pools[0].host == "example.com");
    assert(cfg.pools[0].port == 3333); // default, since "notaport" failed to parse
    assert(cfg.workers == 0);          // default (0 = all cores)
    assert(cfg.difficulty == 100);     // default
    assert(cfg.seconds == 10);         // default

    std::cout << "[test_config] test_malformed_numeric_fields_dont_crash passed\n";
}

void test_valid_numeric_fields_still_parse() {
    // Same fields, valid this time — confirms the try/catch guards didn't
    // break normal parsing.
    const auto cfg = load_from_temp_json("valid",
        R"({"pool":"example.com:1111","workers":4,"difficulty":12345,"seconds":30})");

    assert(cfg.pools.size() == 1);
    assert(cfg.pools[0].host == "example.com");
    assert(cfg.pools[0].port == 1111);
    assert(cfg.workers == 4);
    assert(cfg.difficulty == 12345ULL);
    assert(cfg.seconds == 30);

    std::cout << "[test_config] test_valid_numeric_fields_still_parse passed\n";
}

void test_missing_file_returns_defaults() {
    // load_config() must not throw on a nonexistent path either — this was
    // already correct, but worth pinning down since load_config_with_fallback()
    // relies on exactly this behavior when probing default locations.
    const auto cfg = armrx::load_config("test_config_definitely_does_not_exist.json");
    assert(cfg.pools.empty());
    assert(cfg.wallet.empty());

    std::cout << "[test_config] test_missing_file_returns_defaults passed\n";
}

void test_parse_pool_str() {
    // Test normal host without port
    auto p1 = armrx::parse_pool_str("pool.example.com");
    assert(p1.host == "pool.example.com");
    assert(p1.port == 3333); // default

    // Test normal host with valid port
    auto p2 = armrx::parse_pool_str("pool.example.com:4444");
    assert(p2.host == "pool.example.com");
    assert(p2.port == 4444);

    // Test invalid port: non-numeric
    auto p3 = armrx::parse_pool_str("pool.example.com:notaport");
    assert(p3.host == "pool.example.com");
    assert(p3.port == 3333); // default

    // Test invalid port: out-of-bounds (exceeds 65535)
    auto p4 = armrx::parse_pool_str("pool.example.com:65536");
    assert(p4.host == "pool.example.com");
    assert(p4.port == 3333); // default

    // Test invalid port: negative
    auto p5 = armrx::parse_pool_str("pool.example.com:-1");
    assert(p5.host == "pool.example.com");
    assert(p5.port == 3333); // default

    // Test invalid port: empty
    auto p6 = armrx::parse_pool_str("pool.example.com:");
    assert(p6.host == "pool.example.com");
    assert(p6.port == 3333); // default

    // IPv4 address with port
    auto p7 = armrx::parse_pool_str("127.0.0.1:8080");
    assert(p7.host == "127.0.0.1");
    assert(p7.port == 8080);

    // IPv6 without brackets but using colons (this simple parser splits at the *last* colon,
    // which is not fully robust for raw IPv6 without brackets, but we test current behavior).
    auto p8 = armrx::parse_pool_str("[::1]:9999");
    assert(p8.host == "[::1]");
    assert(p8.port == 9999);

    std::cout << "[test_config] test_parse_pool_str passed\n";
}

void test_parse_bounded_ull_error_paths() {
    // Valid cases
    assert(armrx::parse_bounded_ull("100", 100) == 100);
    assert(armrx::parse_bounded_ull("  42  ", 100) == 42);
    assert(armrx::parse_bounded_ull("0", 10) == 0);

    // Negative inputs
    bool caught_negative = false;
    try {
        armrx::parse_bounded_ull("-1", 100);
    } catch (const std::out_of_range& e) {
        caught_negative = true;
    }
    assert(caught_negative);

    bool caught_negative_spaces = false;
    try {
        armrx::parse_bounded_ull("   -42", 100);
    } catch (const std::out_of_range& e) {
        caught_negative_spaces = true;
    }
    assert(caught_negative_spaces);

    // Value exceeding max
    bool caught_exceeding = false;
    try {
        armrx::parse_bounded_ull("101", 100);
    } catch (const std::out_of_range& e) {
        caught_exceeding = true;
    }
    assert(caught_exceeding);

    // Non-numeric garbage (should throw invalid_argument via stoull)
    bool caught_invalid = false;
    try {
        armrx::parse_bounded_ull("abc", 100);
    } catch (const std::invalid_argument& e) {
        caught_invalid = true;
    }
    assert(caught_invalid);

    std::cout << "[test_config] test_parse_bounded_ull_error_paths passed\n";
}

int main() {
    test_malformed_numeric_fields_dont_crash();
    test_valid_numeric_fields_still_parse();
    test_missing_file_returns_defaults();
    test_parse_pool_str();
    test_parse_bounded_ull_error_paths();
    std::cout << "ALL CONFIG TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
