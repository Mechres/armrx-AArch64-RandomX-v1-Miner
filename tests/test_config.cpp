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

int main() {
    test_malformed_numeric_fields_dont_crash();
    test_valid_numeric_fields_still_parse();
    test_missing_file_returns_defaults();
    std::cout << "ALL CONFIG TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
