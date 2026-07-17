#pragma once

/// Minimal JSON utility module — no external dependency.
/// Provides safe escaping for output and a correct tokenizer for input,
/// replacing the duplicated hand-rolled parsers in stratum_client.cpp
/// and config.cpp.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace armrx {
namespace json {

/// JSON-escape a string: `\` → `\\`, `"` → `\"`, control chars → `\n`/`\r`/`\t`.
[[nodiscard]] std::string escape(std::string_view s);

/// Extract a string value by key: given `"key":"value"`, returns `"value"`.
/// Returns empty string on missing key.
/// Correctly handles `\\"` inside quoted strings (unlike the old parsers).
[[nodiscard]] std::string get_string(const std::string& json, std::string_view key);

/// Extract a raw token (number, bool, null) by key.
/// Returns empty string on missing key.
[[nodiscard]] std::string get_raw(const std::string& json, std::string_view key);

/// Extract the first element of an array by key: given `"key":["elem",...]`,
/// returns the first element (string or raw token).
[[nodiscard]] std::string get_array_first(const std::string& json, std::string_view key);

/// Extract all string elements from an array by key: `"key":["a","b","c"]`
/// returns `{"a","b","c"}`.
[[nodiscard]] std::vector<std::string> get_str_array(const std::string& json, std::string_view key);

/// Extract a nested object or array by key, with correct brace/bracket matching.
/// Returns the substring including braces/brackets.
[[nodiscard]] std::string get_object(const std::string& json, std::string_view key);

/// Build a JSON-RPC 2.0 request envelope (without "jsonrpc" field, as
/// required by Stratum V1 subscribe/authorize/submit messages).
/// `method` and `params` are inserted **unescaped** — the caller is responsible
/// for escaping any user-controlled content via `escape()` before passing.
[[nodiscard]] std::string rpc_envelope(std::uint64_t id, const std::string& method, const std::string& params);

} // namespace json
} // namespace armrx
