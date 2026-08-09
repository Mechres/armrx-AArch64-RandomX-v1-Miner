#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace armrx {

struct PoolConfig {
    std::string host;
    std::uint16_t port = 3333;
    bool tls = false;
};

struct AppConfig {
    std::vector<PoolConfig> pools;
    std::string wallet;
    std::string password = "x";
    bool pool_tls = false;        // global TLS override
    std::string mode = "auto";    // auto, light, fast
    unsigned workers = 0;         // 0 = all cores
    std::uint64_t difficulty = 100;
    unsigned seconds = 10;    // benchmark duration
};

/** Parse config from a JSON file path. Returns defaults if file missing. */
AppConfig load_config(const std::string& path);

/** Try default config locations (~/.config/armrx/config.json, ./armrx.conf). Returns defaults if none found. */
AppConfig load_default_config();

/** Load config from a specific path, or try defaults if path is empty. */
AppConfig load_config_with_fallback(const std::string& explicit_path);

/** Parse --key=value CLI args and override config fields. Returns modified config. */
// CLI argument parsing is handled in main.cpp (argument loop in main() body).

} // namespace armrx
