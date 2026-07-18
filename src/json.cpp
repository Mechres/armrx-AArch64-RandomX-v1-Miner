#include "armrx/json.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace armrx {
namespace json {

// ── Internal helpers ──────────────────────────────────────────────────────

// Count consecutive backslashes before a position. Returns true if the
// character at `pos` is escaped (odd number of preceding backslashes).
static bool is_escaped(const std::string& s, std::size_t pos) {
    if (pos == 0) return false;
    std::size_t count = 0;
    for (std::size_t i = pos; i > 0 && s[i - 1] == '\\'; --i)
        ++count;
    return (count % 2) != 0;
}

// Skip over a quoted string starting at `pos` (which points at the opening `"`).
// Returns the position after the closing `"`, or `npos` on malformed input.
static std::size_t skip_string(const std::string& s, std::size_t pos) {
    if (pos >= s.size() || s[pos] != '"') return std::string::npos;
    ++pos;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\') ++pos; // skip the escaped character
        ++pos;
    }
    if (pos >= s.size()) return std::string::npos; // unterminated string
    return pos + 1; // past the closing quote
}

// Skip whitespace characters.
static void skip_ws(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
        ++pos;
}

// ── Public API ────────────────────────────────────────────────────────────

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // JSON requires all control characters U+0000–U+001F to be escaped
                if (static_cast<unsigned char>(c) < 0x20) {
                    static constexpr const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(static_cast<unsigned char>(c) >> 4) & 0xf];
                    out += hex[static_cast<unsigned char>(c) & 0xf];
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

// ── Key lookup helper ─────────────────────────────────────────────────────

// Find the value for a given key. On success sets `value_pos` to the
// position right after the colon (pointing at the value). Returns true
// if the key was found.
static bool find_key(const std::string& json, std::string_view key,
                     std::size_t& value_pos) {
    std::string search = "\"" + std::string(key) + "\"";
    std::size_t pos = 0;
    while (true) {
        pos = json.find(search, pos);
        if (pos == std::string::npos) return false;

        // Scope check: the key must be at an object-key position,
        // not inside a string value. Check that the character
        // before it (skipping whitespace) is '{' or ','.
        std::size_t before = pos;
        while (before > 0 && (json[before - 1] == ' ' || json[before - 1] == '\t'))
            --before;
        if (before == 0 || json[before - 1] == '{' || json[before - 1] == ',')
            break;

        // Not an object key — skip past this occurrence and retry
        pos += search.size();
    }

    pos += search.size();
    skip_ws(json, pos);
    if (pos >= json.size() || json[pos] != ':') return false;
    ++pos;
    skip_ws(json, pos);
    value_pos = pos;
    return true;
}

std::string get_string(const std::string& json, std::string_view key) {
    std::size_t pos;
    if (!find_key(json, key, pos)) return {};

    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;

    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\') {
            ++pos;
            if (pos >= json.size()) break;
            result += json[pos];
        } else {
            result += json[pos];
        }
        ++pos;
    }
    return result;
}

std::string get_raw(const std::string& json, std::string_view key) {
    std::size_t pos;
    if (!find_key(json, key, pos)) return {};

    std::size_t start = pos;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}'
           && json[pos] != ']' && json[pos] != '\n') {
        ++pos;
    }
    // Trim trailing whitespace
    std::size_t end = pos;
    while (end > start && (json[end - 1] == ' ' || json[end - 1] == '\t'))
        --end;
    return json.substr(start, end - start);
}

std::string get_array_first(const std::string& json, std::string_view key) {
    std::size_t pos;
    if (!find_key(json, key, pos)) return {};

    skip_ws(json, pos);
    if (pos >= json.size() || json[pos] != '[') return {};
    ++pos;
    skip_ws(json, pos);
    if (pos >= json.size()) return {};

    // For string elements: extract the first string
    if (json[pos] == '"') {
        ++pos;
        std::size_t start_content = pos;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\') ++pos;
            ++pos;
        }
        return json.substr(start_content, pos - start_content);
    }
    // Raw token (number/bool)
    std::size_t start = pos;
    while (pos < json.size() && json[pos] != ',' && json[pos] != ']'
           && json[pos] != ' ' && json[pos] != '\t')
        ++pos;
    while (pos > start && (json[pos - 1] == ' ' || json[pos - 1] == '\t'))
        --pos;
    return json.substr(start, pos - start);
}

std::vector<std::string> get_str_array(const std::string& json, std::string_view key) {
    std::vector<std::string> result;
    std::size_t pos;
    if (!find_key(json, key, pos)) return result;

    skip_ws(json, pos);
    if (pos >= json.size() || json[pos] != '[') return result;
    ++pos;

    while (pos < json.size() && json[pos] != ']') {
        skip_ws(json, pos);
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] == ',') { ++pos; continue; }
        if (json[pos] == '"') {
            ++pos;
            std::string elem;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\') { ++pos; if (pos < json.size()) elem += json[pos]; }
                else { elem += json[pos]; }
                ++pos;
            }
            if (pos < json.size()) ++pos; // skip closing quote
            result.push_back(std::move(elem));
        } else {
            // Skip non-string element
            while (pos < json.size() && json[pos] != ',' && json[pos] != ']'
                   && json[pos] != ' ' && json[pos] != '\t')
                ++pos;
        }
    }
    return result;
}

std::string get_array_element(const std::string& json, std::string_view key, unsigned index) {
    std::size_t pos;
    if (!find_key(json, key, pos)) return {};

    skip_ws(json, pos);
    if (pos >= json.size() || json[pos] != '[') return {};
    ++pos;

    unsigned cur = 0;
    while (pos < json.size() && json[pos] != ']') {
        skip_ws(json, pos);
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] == ',') { ++pos; continue; }

        // Found our target element
        if (cur == index) {
            if (json[pos] == '"') {
                // String value
                ++pos;
                std::string val;
                while (pos < json.size() && json[pos] != '"') {
                    if (json[pos] == '\\') { ++pos; if (pos < json.size()) val += json[pos]; }
                    else { val += json[pos]; }
                    ++pos;
                }
                return val;
            } else {
                // Raw token (number, bool)
                std::size_t start = pos;
                while (pos < json.size() && json[pos] != ',' && json[pos] != ']'
                       && json[pos] != ' ' && json[pos] != '\t')
                    ++pos;
                while (pos > start && (json[pos - 1] == ' ' || json[pos - 1] == '\t'))
                    --pos;
                return json.substr(start, pos - start);
            }
        }

        // Skip this element
        if (json[pos] == '"') {
            pos = skip_string(json, pos);
            if (pos == std::string::npos) return {};
        } else {
            while (pos < json.size() && json[pos] != ',' && json[pos] != ']'
                   && json[pos] != ' ' && json[pos] != '\t')
                ++pos;
        }
        ++cur;
    }
    return {};
}

std::string get_object(const std::string& json, std::string_view key) {
    std::size_t pos;
    if (!find_key(json, key, pos)) return {};

    if (pos >= json.size()) return {};
    char open = json[pos];
    char close;
    if (open == '{') close = '}';
    else if (open == '[') close = ']';
    else return {};

    int depth = 1;
    std::size_t start = pos;
    ++pos;
    bool in_string = false;
    while (pos < json.size() && depth > 0) {
        char c = json[pos];
        if (c == '"' && !is_escaped(json, pos)) {
            in_string = !in_string;
        } else if (!in_string) {
            if (c == open) ++depth;
            else if (c == close) --depth;
        }
        ++pos;
    }
    return json.substr(start, pos - start);
}

std::string rpc_envelope(std::uint64_t id, const std::string& method,
                         const std::string& params) {
    return "{\"id\":" + std::to_string(id) +
           ",\"method\":\"" + method + "\"" +
           ",\"params\":" + params + "}\n";
}

} // namespace json
} // namespace armrx
