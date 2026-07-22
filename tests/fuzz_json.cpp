/**
 * LibFuzzer harness for armrx::json (PLAN.md §3.1).
 *
 * Fuzzes the parser's full public API surface directly — not through
 * StratumClient::handle_line (which stays private; the mock Stratum tests in
 * tests/test_pool_protocol.cpp already exercise it with valid protocol
 * messages). This harness's scope is "hostile bytes crash the parser
 * module itself," matching this being the exact code a compromised mining
 * pool's replies (via StratumClient) or a hostile local config file (via
 * config.cpp) would run through.
 *
 * Clang-only (requires -fsanitize=fuzzer); the main GCC-only project build
 * is entirely unaffected — this target only exists when configured with
 * -DARMRX_BUILD_FUZZERS=ON, which the top-level CMakeLists.txt gates on
 * CMAKE_CXX_COMPILER_ID being Clang.
 *
 * Build (standalone, no CMake):
 *   clang++ -std=c++20 -fsanitize=fuzzer,address -I include \
 *     tests/fuzz_json.cpp src/json.cpp -o fuzz_json
 *
 * Run for a bounded smoke period:
 *   ./fuzz_json -max_total_time=60
 *
 * Any crash found should be minimized (libFuzzer does this automatically —
 * see the `crash-*` file it writes) and turned into a deterministic
 * regression test case, per this session's established pattern.
 */

#include "armrx/json.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace {

// Representative keys mirroring what real Stratum V1/CryptoNote messages and
// armrx's own config-file parser actually look up (see stratum_client.cpp /
// config.cpp), so the fuzzer's mutations land on code paths that matter
// rather than always missing every find_key() lookup.
constexpr std::string_view kKeys[] = {
    "method", "params", "result", "error", "id",
    "job_id", "blob", "target", "seed_hash", "jsonrpc",
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);

    for (auto key : kKeys) {
        (void)armrx::json::get_string(input, key);
        (void)armrx::json::get_raw(input, key);
        (void)armrx::json::get_array_first(input, key);
        (void)armrx::json::get_str_array(input, key);
        (void)armrx::json::get_object(input, key);
        for (unsigned idx = 0; idx < 6; ++idx) {
            (void)armrx::json::get_array_element(input, key, idx);
        }
    }

    // escape() takes arbitrary bytes too (used to build outgoing messages
    // from wallet/password/job_id fields) — round-trip it through get_string
    // to also cover the encode-then-decode boundary.
    const auto escaped = armrx::json::escape(input);
    (void)armrx::json::get_string("{\"k\":\"" + escaped + "\"}", "k");

    return 0;
}
