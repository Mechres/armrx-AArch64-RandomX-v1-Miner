#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace armrx {
namespace log {

// ── Log levels ────────────────────────────────────────────────────────────
enum class Level : std::uint8_t {
    trace = 0,
    debug = 1,
    info  = 2,
    warn  = 3,
    error = 4,
};

constexpr const char* level_name(Level lv) noexcept {
    switch (lv) {
        case Level::trace: return "TRACE";
        case Level::debug: return "DEBUG";
        case Level::info:  return "INFO";
        case Level::warn:  return "WARN";
        case Level::error: return "ERROR";
    }
    return "????";
}

// ── Global configuration ──────────────────────────────────────────────────

/// Minimum level to emit. Messages below this are dropped.
inline std::atomic<Level> g_min_level{Level::info};

/// When true, console output is suppressed and messages go to a ring buffer
/// that the TUI can read from. Default false.
inline std::atomic<bool> g_tui_mode{false};

inline Level get_level() noexcept { return g_min_level.load(std::memory_order_relaxed); }
inline void set_level(Level lv)  { g_min_level.store(lv, std::memory_order_relaxed); }
inline bool tui_mode() noexcept  { return g_tui_mode.load(std::memory_order_relaxed); }
inline void set_tui_mode(bool m) { g_tui_mode.store(m, std::memory_order_relaxed); }

// ── Ring buffer for TUI mode ──────────────────────────────────────────────
// Stores the last N log lines so the TUI can display them without racing
// on std::cout.

inline std::mutex& ring_mutex() {
    static std::mutex m;
    return m;
}

inline std::vector<std::string>& ring_buffer() {
    static std::vector<std::string> buf;
    return buf;
}

inline constexpr std::size_t kRingCapacity = 256;

/// Append a line to the ring buffer, evicting oldest if full.
inline void ring_push(const std::string& line) {
    std::lock_guard<std::mutex> lock(ring_mutex());
    auto& buf = ring_buffer();
    if (buf.size() >= kRingCapacity)
        buf.erase(buf.begin());
    buf.push_back(line);
}

/// Snapshot the current ring buffer contents.
inline std::vector<std::string> ring_snapshot() {
    std::lock_guard<std::mutex> lock(ring_mutex());
    return ring_buffer();
}

// ── Write sink ────────────────────────────────────────────────────────────

inline std::mutex& sink_mutex() {
    static std::mutex m;
    return m;
}

/// Core write: formats and outputs a log line.
inline void write(Level lv, const std::string& msg) {
    // Build the line
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count() % 1000;

    std::ostringstream oss;
    oss << '[' << level_name(lv) << ']';
    // Timestamp (thread-safe gmtime_r on POSIX)
    char tb[32];
    struct tm tm_buf;
    ::gmtime_r(&tt, &tm_buf);
    std::strftime(tb, sizeof(tb), "%H:%M:%S", &tm_buf);
    oss << ' ' << tb << '.' << std::setw(3) << std::setfill('0') << ms;
    oss << ' ' << msg;

    std::string line = oss.str();

    if (tui_mode()) {
        ring_push(line);
    } else {
        std::lock_guard<std::mutex> lock(sink_mutex());
        if (lv >= Level::warn)
            std::cerr << line << std::endl;
        else
            std::cout << line << std::endl;
    }
}

// ── Stream-style helper ───────────────────────────────────────────────────

class LogStream {
public:
    explicit LogStream(Level lv) : lv_(lv) {}
    ~LogStream() {
        if (!std::uncaught_exceptions())
            write(lv_, buf_.str());
    }

    template <typename T>
    LogStream& operator<<(const T& val) {
        buf_ << val;
        return *this;
    }

    // Support for std::endl and other manipulators
    LogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
        manip(buf_);
        return *this;
    }

private:
    Level lv_;
    std::ostringstream buf_;
};

} // namespace log
} // namespace armrx

// ── Convenience macros ────────────────────────────────────────────────────
// These check g_min_level before doing any work (including formatting),
// making hot-path log sites cheap when the level is filtered.

#define ARMRX_LOG_TRACE \
    if (armrx::log::get_level() > armrx::log::Level::trace) ; \
    else armrx::log::LogStream(armrx::log::Level::trace)

#define ARMRX_LOG_DEBUG \
    if (armrx::log::get_level() > armrx::log::Level::debug) ; \
    else armrx::log::LogStream(armrx::log::Level::debug)

#define ARMRX_LOG_INFO \
    if (armrx::log::get_level() > armrx::log::Level::info) ; \
    else armrx::log::LogStream(armrx::log::Level::info)

#define ARMRX_LOG_WARN \
    if (armrx::log::get_level() > armrx::log::Level::warn) ; \
    else armrx::log::LogStream(armrx::log::Level::warn)

#define ARMRX_LOG_ERROR \
    if (armrx::log::get_level() > armrx::log::Level::error) ; \
    else armrx::log::LogStream(armrx::log::Level::error)
