#include "armrx/config.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdlib>

namespace armrx {
namespace {

// Minimal JSON string value extractor (same pattern as stratum_client.cpp)
std::string json_str(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\') { ++pos; if (pos >= json.size()) break; }
        val.push_back(json[pos++]);
    }
    return val;
}

std::string json_bool(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && json[pos] == ' ') ++pos;
    auto end = json.find_first_of(",}\n", pos);
    return json.substr(pos, end - pos);
}

std::string json_num(const std::string& json, const std::string& key) {
    return json_bool(json, key); // same logic for numbers
}

// Extract string array elements from "key":["v1","v2",...]
std::vector<std::string> json_str_array(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return result;
    ++pos;
    while (pos < json.size()) {
        while (pos < json.size() && json[pos] != '"' && json[pos] != ']') ++pos;
        if (pos >= json.size() || json[pos] == ']') break;
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            val.push_back(json[pos++]);
        }
        if (!val.empty()) result.push_back(val);
        if (pos < json.size()) ++pos;
        // skip comma
        while (pos < json.size() && (json[pos] == ',' || json[pos] == ' ')) ++pos;
    }
    return result;
}

// Parse "host:port" string
PoolConfig parse_pool_str(const std::string& s) {
    PoolConfig pc;
    auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        pc.host = s.substr(0, colon);
        pc.port = static_cast<std::uint16_t>(std::stoul(s.substr(colon + 1)));
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
    auto pool_str = json_str(json, "pool");
    if (!pool_str.empty()) {
        cfg.pools.push_back(parse_pool_str(pool_str));
    }
    auto pools_arr = json_str_array(json, "pools");
    for (auto& s : pools_arr) {
        cfg.pools.push_back(parse_pool_str(s));
    }

    cfg.wallet     = json_str(json, "wallet");
    cfg.password   = json_str(json, "password");
    if (cfg.password.empty()) cfg.password = "x";
    cfg.mode       = json_str(json, "mode");
    if (cfg.mode.empty()) cfg.mode = "auto";

    auto tls_str = json_bool(json, "tls");
    cfg.pool_tls = (tls_str == "true");

    auto tui_str = json_bool(json, "tui");
    cfg.tui = (tui_str == "true");

    auto workers_str = json_num(json, "workers");
    if (!workers_str.empty()) cfg.workers = static_cast<unsigned>(std::stoul(workers_str));

    auto diff_str = json_num(json, "difficulty");
    if (!diff_str.empty()) cfg.difficulty = std::stoull(diff_str);

    auto sec_str = json_num(json, "seconds");
    if (!sec_str.empty()) cfg.seconds = static_cast<unsigned>(std::stoul(sec_str));

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

AppConfig apply_cli_overrides(AppConfig cfg, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg.rfind("--pool=", 0) == 0) {
            cfg.pools.push_back(parse_pool_str(arg.substr(7)));
        } else if (arg.rfind("--wallet=", 0) == 0) {
            cfg.wallet = arg.substr(9);
        } else if (arg.rfind("--password=", 0) == 0) {
            cfg.password = arg.substr(11);
        } else if (arg == "--tls") {
            cfg.pool_tls = true;
        } else if (arg == "--no-tls") {
            cfg.pool_tls = false;
        } else if (arg.rfind("--mode=", 0) == 0) {
            cfg.mode = arg.substr(7);
        } else if (arg.rfind("--workers=", 0) == 0) {
            cfg.workers = static_cast<unsigned>(std::stoul(arg.substr(10)));
        } else if (arg.rfind("--difficulty=", 0) == 0) {
            cfg.difficulty = std::stoull(arg.substr(13));
        } else if (arg.rfind("--seconds=", 0) == 0) {
            cfg.seconds = static_cast<unsigned>(std::stoul(arg.substr(10)));
        } else if (arg == "--tui") {
            cfg.tui = true;
        } else if (arg == "--no-tui") {
            cfg.tui = false;
        }
    }
    return cfg;
}

} // namespace armrx
