#include "armrx/config.hpp"
#include "armrx/json.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <stdexcept>

namespace armrx {
namespace {

// Parse "host:port" string
PoolConfig parse_pool_str(const std::string& s) {
    PoolConfig pc;
    auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        pc.host = s.substr(0, colon);
        try {
            pc.port = static_cast<std::uint16_t>(std::stoul(s.substr(colon + 1)));
        } catch (const std::exception& ex) {
            std::cerr << "[Config] Warning: invalid pool port in \"" << s
                      << "\" (" << ex.what() << "), using default port " << pc.port << '\n';
        }
    } else {
        pc.host = s;
    }
    pc.tls = false;
    return pc;
}

} // namespace

AppConfig load_config(const std::string& path) {
    AppConfig cfg;
    std::ifstream file(path);
    if (!file.is_open()) {
        // Silently skip default paths; only warn for explicitly requested configs
// std::cerr << "[Config] Warning: could not open " << path << '\n';
        return cfg;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const auto json = ss.str();

    // Pool(s)
    auto pool_str = armrx::json::get_string(json, "pool");
    if (!pool_str.empty()) {
        cfg.pools.push_back(parse_pool_str(pool_str));
    }
    auto pools_arr = armrx::json::get_str_array(json, "pools");
    for (auto& s : pools_arr) {
        cfg.pools.push_back(parse_pool_str(s));
    }

    cfg.wallet     = armrx::json::get_string(json, "wallet");
    cfg.password   = armrx::json::get_string(json, "password");
    if (cfg.password.empty()) cfg.password = "x";
    cfg.mode       = armrx::json::get_string(json, "mode");
    if (cfg.mode.empty()) cfg.mode = "auto";

    auto tls_str = armrx::json::get_raw(json, "tls");
    cfg.pool_tls = (tls_str == "true");

    auto tui_str = armrx::json::get_raw(json, "tui");
    cfg.tui = (tui_str == "true");

    // load_config_with_fallback() runs unconditionally on every launch (it
    // auto-probes $ARMRX_CONFIG / ~/.config/armrx/config.json / ./armrx.conf
    // even with no --config= flag), so a malformed value in any of these
    // fields must not crash the whole process the way an unguarded
    // std::stoul/std::stoull would — cli_parser.cpp already guards the
    // equivalent CLI flags the same way, for the same reason.
    auto workers_str = armrx::json::get_raw(json, "workers");
    if (!workers_str.empty()) {
        try {
            cfg.workers = static_cast<unsigned>(std::stoul(workers_str));
        } catch (const std::exception& ex) {
            std::cerr << "[Config] Warning: invalid \"workers\" value \"" << workers_str
                      << "\" (" << ex.what() << "), ignoring\n";
        }
    }

    auto diff_str = armrx::json::get_raw(json, "difficulty");
    if (!diff_str.empty()) {
        try {
            cfg.difficulty = std::stoull(diff_str);
        } catch (const std::exception& ex) {
            std::cerr << "[Config] Warning: invalid \"difficulty\" value \"" << diff_str
                      << "\" (" << ex.what() << "), ignoring\n";
        }
    }

    auto sec_str = armrx::json::get_raw(json, "seconds");
    if (!sec_str.empty()) {
        try {
            cfg.seconds = static_cast<unsigned>(std::stoul(sec_str));
        } catch (const std::exception& ex) {
            std::cerr << "[Config] Warning: invalid \"seconds\" value \"" << sec_str
                      << "\" (" << ex.what() << "), ignoring\n";
        }
    }

    std::cout << "[Config] Loaded " << path << " (" << cfg.pools.size() << " pool(s))\n";
    return cfg;
}

AppConfig load_config_with_fallback(const std::string& explicit_path) {
    if (!explicit_path.empty()) {
        auto cfg = load_config(explicit_path);
        // Show warning only for explicitly requested paths
        return cfg;
    }
    return load_default_config();
}

AppConfig load_default_config() {
    // Check ARMRX_CONFIG env var
    const char* env = std::getenv("ARMRX_CONFIG");
    if (env) {
        auto cfg = load_config(env);
        if (!cfg.pools.empty() || !cfg.wallet.empty()) return cfg;
    }

    // Check ~/.config/armrx/config.json
    const char* home = std::getenv("HOME");
    if (home) {
        std::string path = std::string(home) + "/.config/armrx/config.json";
        auto cfg = load_config(path);
        if (!cfg.pools.empty() || !cfg.wallet.empty()) return cfg;
    }

    // Check ./armrx.conf
    auto cfg = load_config("./armrx.conf");
    if (!cfg.pools.empty() || !cfg.wallet.empty()) return cfg;

    return {};
}

// CLI argument parsing is handled in main.cpp. If extending flags, edit
// the parser there (src/main.cpp, argument loop) -- not this file.


} // namespace armrx
