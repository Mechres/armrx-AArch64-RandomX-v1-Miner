/**
 * StratumClient — Monero Stratum V1 protocol implementation.
 *
 * Implements the subset of Stratum used by XMR pools:
 *   mining.subscribe, mining.authorize, mining.set_target,
 *   mining.notify, mining.submit.
 *
 * JSON is handled with a minimal hand-rolled extractor to avoid
 * a heavy library dependency; all values used are simple strings
 * or numbers at fixed positions in well-known protocol messages.
 */

#include "armrx/stratum_client.hpp"

#include "armrx/json.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

// POSIX sockets
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace armrx {

// JSON helpers moved to armrx/json.hpp — stratum_client now uses armrx::json::*

// ─────────────────────────────────────────────────────────────────────────────
// StratumClient — construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

StratumClient::StratumClient(std::string host, std::uint16_t port,
                             std::string wallet, std::string password)
    : host_(std::move(host))
    , port_(port)
    , wallet_(std::move(wallet))
    , password_(std::move(password))
{}

StratumClient::~StratumClient() {
    disconnect();
}

// ─────────────────────────────────────────────────────────────────────────────
// connect / disconnect
// ─────────────────────────────────────────────────────────────────────────────

void StratumClient::close_connection() {
    connected_.store(false);

    // Close the socket FIRST to unblock the reader thread (avoids TLS race)
    if (sockfd_ >= 0) {
        ::shutdown(sockfd_, SHUT_RDWR);
        ::close(sockfd_);
        sockfd_ = -1;
    }

    // Join reader thread before tearing down TLS
    if (reader_thread_.joinable() && std::this_thread::get_id() != reader_thread_.get_id()) {
        reader_thread_.join();
    }
    if (keepalive_thread_.joinable() && std::this_thread::get_id() != keepalive_thread_.get_id()) {
        keepalive_thread_.join();
    }

    // Now safe to tear down TLS
#ifdef ARMRX_HAVE_TLS
    if (tls_) {
        tls_->disconnect();
        tls_.reset();
    }
#endif
}

void StratumClient::connect() {
    if (connected_.load()) return;
    reconnect_enabled_.store(true);
    handshake_in_progress_.store(true);

    // RAII guard: clear handshake flag on scope exit
    struct HandshakeGuard {
        std::atomic<bool>& flag;
        ~HandshakeGuard() { flag.store(false); }
    } guard{handshake_in_progress_};

    // Clean up any stale threads from a previous connection lifecycle
    if (reader_thread_.joinable() && std::this_thread::get_id() != reader_thread_.get_id()) {
        reader_thread_.join();
    }
    if (keepalive_thread_.joinable() && std::this_thread::get_id() != keepalive_thread_.get_id()) {
        keepalive_thread_.join();
    }
    if (reconnect_thread_.joinable() && std::this_thread::get_id() != reconnect_thread_.get_id()) {
        reconnect_thread_.join();
    }

    while (true) {
        // Resolve host
        struct addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        const std::string port_str = std::to_string(port_);
        int rc = ::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res);
        if (rc != 0 || res == nullptr) {
            throw std::runtime_error(
                std::string("StratumClient: DNS resolution failed for ") + host_ +
                ": " + ::gai_strerror(rc));
        }

        // Try each address
        int fd = -1;
        for (auto* p = res; p != nullptr; p = p->ai_next) {
            fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(res);

        if (fd < 0) {
            throw std::runtime_error(
                std::string("StratumClient: could not connect to ") +
                host_ + ":" + port_str);
        }

        // Enable TCP_NODELAY for lower latency on share submissions
        int flag = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&flag), sizeof(flag));

        sockfd_ = fd;

        // Optional TLS wrapping
#ifdef ARMRX_HAVE_TLS
        if (tls_enabled_) {
            tls_ = std::make_unique<TlsClient>();
            tls_->set_verify_peer(tls_verify_peer_);
            if (!tls_->connect(fd, host_)) {
                ::close(fd);
                sockfd_ = -1;
                throw std::runtime_error(
                    std::string("StratumClient: TLS handshake failed for ") + host_);
            }
            std::cout << "[Stratum] TLS enabled\n";
        }
#endif

        connected_.store(true);

        // Reset subscribe handshake state
        subscribe_done_ = std::promise<bool>();
        auto subscribe_future = subscribe_done_.get_future();

        // Start reader thread before handshake so we can receive replies
        reader_thread_ = std::thread(&StratumClient::reader_thread_fn, this);

        const StratumProtocol active_protocol = (protocol_ == StratumProtocol::AUTO) ? StratumProtocol::CRYPTONOTE : protocol_;

        std::string msg;
        if (active_protocol == StratumProtocol::STRATUM_V1) {
            msg = build_subscribe_msg();
        } else {
            msg = build_login_msg();
        }

        std::cout << "[Stratum] >> " << msg.substr(0, msg.size() - 1) << '\n';
        {
            std::lock_guard lock(send_mutex_);
            send_line(msg);
        }

        // Wait for handshake response (up to 10 seconds)
        auto status = subscribe_future.wait_for(std::chrono::seconds(10));
        if (status != std::future_status::ready) {
            close_connection();
            throw std::runtime_error("StratumClient: handshake timed out");
        }

        if (!subscribe_ok_) {
            if (protocol_ == StratumProtocol::AUTO) {
                std::cerr << "[Stratum] Handshake failed with CryptoNote. Falling back to Stratum V1 protocol...\n";
                protocol_ = StratumProtocol::STRATUM_V1;
                close_connection();
                connected_.store(false);
                continue;
            } else {
                close_connection();
                throw std::runtime_error("StratumClient: handshake rejected by pool");
            }
        }

        // Handshake succeeded!
        if (protocol_ == StratumProtocol::AUTO) {
            protocol_ = active_protocol;
        }

        // If STRATUM_V1, send authorize
        if (protocol_ == StratumProtocol::STRATUM_V1) {
            std::lock_guard lock(send_mutex_);
            send_line(build_authorize_msg());
        }

        // Start keepalive thread if CRYPTONOTE
        if (protocol_ == StratumProtocol::CRYPTONOTE) {
            if (keepalive_thread_.joinable() && std::this_thread::get_id() != keepalive_thread_.get_id()) {
                keepalive_thread_.join();
            }
            keepalive_thread_ = std::thread(&StratumClient::keepalive_loop, this);
        }

        break;
    }
}

void StratumClient::disconnect() {
    reconnect_enabled_.store(false);
    close_connection();
    if (reconnect_thread_.joinable() && std::this_thread::get_id() != reconnect_thread_.get_id()) {
        reconnect_thread_.join();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Share submission
// ─────────────────────────────────────────────────────────────────────────────

void StratumClient::submit_share(const Job& job, std::uint64_t nonce,
                                 const std::array<std::byte, 32>& hash) {
    if (!connected_.load()) return;
    const std::string msg = build_submit_msg(job, nonce, hash);
    std::lock_guard lock(send_mutex_);
    send_line(msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reconnect configuration
// ─────────────────────────────────────────────────────────────────────────────

void StratumClient::set_reconnect_config(unsigned max_retries, unsigned base_delay_ms) {
    max_retries_   = max_retries;
    base_delay_ms_ = base_delay_ms;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stratum message builders
// ─────────────────────────────────────────────────────────────────────────────

std::string StratumClient::build_subscribe_msg() const {
    const auto id = request_id_.fetch_add(1);
    handshake_req_id_ = id;
    static const char* formats[] = {
        "[\"armrx/1.0\"]",
        "[\"armrx/1.0\",\"monero\"]",
        "[]",
        "[\"XMRig/6.21.0\"]",
    };
    static const char* methods[] = {
        "mining.subscribe",
        "mining.subscribe",
        "mining.subscribe",
        "mining.subscribe",
    };
    unsigned num_formats = 4;
    unsigned idx = subscribe_try_ % num_formats;
    return armrx::json::rpc_envelope(id, methods[idx], formats[idx]);
}

std::string StratumClient::build_login_msg() const {
    const auto id = request_id_.fetch_add(1);
    handshake_req_id_ = id;
    return "{\"id\":" + std::to_string(id) +
           ",\"jsonrpc\":\"2.0\"" +
           ",\"method\":\"login\"" +
           ",\"params\":{" +
             "\"login\":\"" + armrx::json::escape(wallet_) + "\"," +
             "\"pass\":\"" + armrx::json::escape(password_) + "\"," +
             "\"agent\":\"armrx/1.0\"," +
             "\"rigid\":\"\"," +
             "\"algo\":[\"rx/0\"]" +
           "}}\n";
}

std::string StratumClient::build_authorize_msg() const {
    const auto id = request_id_.fetch_add(1);
    authorize_req_id_ = id;
    return armrx::json::rpc_envelope(id, "mining.authorize",
                    "[\"" + armrx::json::escape(wallet_) + "\",\"" + armrx::json::escape(password_) + "\"]");
}

std::string StratumClient::build_submit_msg(const Job& job, std::uint64_t nonce,
                                            const std::array<std::byte, 32>& hash) const {
    const auto id = request_id_.fetch_add(1);
    const std::string nonce_hex = nonce_to_hex(nonce, 4);

    if (protocol_ == StratumProtocol::CRYPTONOTE) {
        std::vector<std::byte> hash_vec(hash.begin(), hash.end());
        const std::string result_hex = bytes_to_hex(hash_vec);
        return "{\"id\":" + std::to_string(id) +
               ",\"jsonrpc\":\"2.0\"" +
               ",\"method\":\"submit\"" +
               ",\"params\":{" +
                 "\"id\":\"" + armrx::json::escape(session_id_) + "\"," +
                 "\"job_id\":\"" + armrx::json::escape(job.job_id) + "\"," +
                 "\"nonce\":\"" + nonce_hex + "\"," +
                 "\"result\":\"" + result_hex + "\"" +
               "}}\n";
    } else {
        return armrx::json::rpc_envelope(id, "mining.submit",
                        "[\"" + armrx::json::escape(wallet_) + "\",\"" + armrx::json::escape(job.job_id) + "\",\"" +
                        nonce_hex + "\"]");
    }
}

void StratumClient::send_line(const std::string& json_line) {
    // Ensure line ends with '\n'
    std::string line = json_line;
    if (line.empty() || line.back() != '\n') line.push_back('\n');
    if (!write_all(line.c_str(), line.size())) {
        std::cerr << "[Stratum] send_line failed: " << std::strerror(errno) << '\n';
    }
}

bool StratumClient::write_all(const char* buf, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        ssize_t n;
#ifdef ARMRX_HAVE_TLS
        if (tls_) {
            n = tls_->write(buf + sent, len - sent);
        } else {
            n = ::send(sockfd_, buf + sent, len - sent, MSG_NOSIGNAL);
        }
#else
        n = ::send(sockfd_, buf + sent, len - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool StratumClient::read_line(std::string& out) {
    while (true) {
        auto nl = read_buf_.find('\n');
        if (nl != std::string::npos) {
            out = read_buf_.substr(0, nl);
            // Strip optional trailing \r (handles \r\n line endings)
            if (!out.empty() && out.back() == '\r') out.pop_back();
            read_buf_.erase(0, nl + 1);
            return true;
        }
        char tmp[4096];
        ssize_t n;
#ifdef ARMRX_HAVE_TLS
        if (tls_) {
            n = tls_->read(tmp, sizeof(tmp));
        } else {
            n = ::recv(sockfd_, tmp, sizeof(tmp), 0);
        }
#else
        n = ::recv(sockfd_, tmp, sizeof(tmp), 0);
#endif
        if (n <= 0) {
            // Connection closed or error — return any buffered data
            // (some servers close the connection immediately after sending
            // a response without a trailing newline, e.g. herominers on
            // unsupported protocol errors).
            if (!read_buf_.empty()) {
                out = std::move(read_buf_);
                read_buf_.clear();
                return true;
            }
            return false;
        }
        read_buf_.append(tmp, static_cast<std::size_t>(n));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Reader thread
// ─────────────────────────────────────────────────────────────────────────────

void StratumClient::reader_thread_fn() {
    while (connected_.load()) {
        std::string line;
        if (!read_line(line)) break;
        if (!line.empty()) {
            handle_line(line);
        }
    }

    // Connection lost — try to reconnect
    connected_.store(false);
    if (reconnect_enabled_.load() && !handshake_in_progress_.load()) {
        if (reconnect_thread_.joinable() && std::this_thread::get_id() != reconnect_thread_.get_id()) {
            reconnect_thread_.join();
        }
        reconnect_thread_ = std::thread(&StratumClient::reconnect_loop, this);
    } else {
        if (error_callback_) {
            error_callback_("connection closed");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stratum message dispatch
// ─────────────────────────────────────────────────────────────────────────────

void StratumClient::handle_line(const std::string& line) {
    std::cout << "[Stratum] << " << line << '\n';
    // Distinguish notifications (have "method") from replies (have "result")
    const bool has_method = line.find("\"method\"") != std::string::npos;
    if (has_method) {
        const auto method = armrx::json::get_string(line, "method");
        if (method == "mining.notify")           handle_notify(line);
        else if (method == "mining.set_target")  handle_set_target(line);
        else if (method == "mining.set_difficulty") handle_set_difficulty(line);
        // mining.set_extranonce: update extra_nonce1 / extra_nonce2_size
        else if (method == "mining.set_extranonce") {
            const auto en1 = armrx::json::get_array_first(line, "params");
            if (!en1.empty()) extra_nonce1_ = en1;
        }
        else if (method == "job") {
            // CryptoNote job notification
            const auto params_obj = armrx::json::get_object(line, "params");
            const auto job_id = armrx::json::get_string(params_obj, "job_id");
            const auto blob_hex = armrx::json::get_string(params_obj, "blob");
            const auto target_hex = armrx::json::get_string(params_obj, "target");
            const auto seed_hex = armrx::json::get_string(params_obj, "seed_hash");
            process_cryptonote_job(job_id, blob_hex, target_hex, seed_hex);
        }
    } else {
        handle_reply(line);
    }
}

void StratumClient::handle_notify(const std::string& line) {
    const auto job_id   = armrx::json::get_array_element(line, "params", 0);
    const auto blob_hex = armrx::json::get_array_element(line, "params", 1);
    const auto tgt_hex  = armrx::json::get_array_element(line, "params", 2);
    const auto seed_hex = armrx::json::get_array_element(line, "params", 4);

    if (job_id.empty() || blob_hex.empty()) return;

    Job job;
    job.job_id        = job_id;
    job.block_template = hex_to_bytes(blob_hex);
    job.nonce_offset  = 39; // Monero: nonce at byte 39 of blob
    job.nonce_size    = 4;

    if (!seed_hex.empty()) {
        job.seed_key = hex_to_bytes(seed_hex);
    }

    // Target: if per-notify target is given, use it; else use last set_target
    if (!tgt_hex.empty() && tgt_hex.size() == 8) {
        // 4-byte compact target (expanded little-endian to 32 bytes)
        const auto compact = hex_to_bytes(tgt_hex);
        std::fill(job.target.bytes.begin(), job.target.bytes.end(), std::byte{0xFF});
        if (compact.size() == 4) {
            std::copy(compact.begin(), compact.end(),
                      job.target.bytes.end() - 4);
        }
    } else {
        std::lock_guard lock(target_mutex_);
        job.target = current_target_;
    }

    std::cout << "[Stratum] New job: " << job_id
              << " blob=" << blob_hex.size() / 2 << " bytes\n";

    if (job_callback_) job_callback_(job);
}

void StratumClient::handle_set_target(const std::string& line) {
    // params: ["<64-char-hex-target>"]
    const auto tgt_hex = armrx::json::get_array_first(line, "params");
    if (tgt_hex.size() != 64) return;
    const auto bytes = hex_to_bytes(tgt_hex);
    Target t{};
    if (bytes.size() == 32) {
        std::copy(bytes.begin(), bytes.end(), t.bytes.begin());
    }
    {
        std::lock_guard lock(target_mutex_);
        current_target_ = t;
    }
    std::cout << "[Stratum] Target updated (set_target)\n";
}

void StratumClient::handle_set_difficulty(const std::string& line) {
    // Older Stratum v1: params: [<numeric difficulty>]
    const auto diff_str = armrx::json::get_array_first(line, "params");
    if (diff_str.empty()) return;
    try {
        const double diff = std::stod(diff_str);
        const Target t = difficulty_to_target(diff);
        std::lock_guard lock(target_mutex_);
        current_target_ = t;
        std::cout << "[Stratum] Difficulty updated to " << diff << '\n';
    } catch (...) {}
}

void StratumClient::handle_reply(const std::string& line) {
    const auto error  = armrx::json::get_object(line, "error");
    const auto result = armrx::json::get_object(line, "result");
    const auto id_str = armrx::json::get_raw(line, "id");

    // Determine the active handshake protocol from the response content.
    // CryptoNote responses include "jsonrpc":"2.0"; Stratum V1 responses do not.
    const bool is_cryptonote_reply = (line.find("\"jsonrpc\":\"2.0\"") != std::string::npos);

    if (id_str == std::to_string(handshake_req_id_)) {
        if (is_cryptonote_reply) {
            if (!error.empty() && error != "null") {
                std::cerr << "[Stratum] Login failed: " << error << '\n';
                subscribe_ok_ = false;
                subscribe_done_.set_value(false);
                return;
            }
            session_id_ = armrx::json::get_string(result, "id");
            if (session_id_.empty()) {
                std::cerr << "[Stratum] Warning: session ID is empty in login reply\n";
            }
            std::cout << "[Stratum] Login successful, session ID: " << session_id_ << "\n";
            
            subscribe_ok_ = true;
            subscribe_done_.set_value(true);

            const auto job_obj = armrx::json::get_object(result, "job");
            if (!job_obj.empty()) {
                const auto job_id = armrx::json::get_string(job_obj, "job_id");
                const auto blob_hex = armrx::json::get_string(job_obj, "blob");
                const auto target_hex = armrx::json::get_string(job_obj, "target");
                const auto seed_hex = armrx::json::get_string(job_obj, "seed_hash");
                process_cryptonote_job(job_id, blob_hex, target_hex, seed_hex);
            }
            return;
        } else {
            // Subscribe reply — check for success or error
            if (!error.empty() && error != "null") {
                std::cerr << "[Stratum] Subscribe rejected: " << error << '\n';
                subscribe_ok_ = false;
                subscribe_done_.set_value(false);
                return;
            }
            // Successful subscribe — extract extranonce1
            subscribe_ok_ = true;
            auto en1_pos = line.find("\"result\"");
            if (en1_pos != std::string::npos) {
                // Find the second element at depth 1 (the extranonce1 string)
                auto outer_bracket = line.find('[', en1_pos);
                if (outer_bracket != std::string::npos) {
                    auto inner_bracket = line.find('[', outer_bracket + 1);
                    auto comma_after_inner = line.find(']', inner_bracket != std::string::npos ? inner_bracket : outer_bracket);
                    if (comma_after_inner != std::string::npos) {
                        auto comma = line.find(',', comma_after_inner);
                        if (comma != std::string::npos) {
                            ++comma;
                            while (comma < line.size() && line[comma] == ' ') ++comma;
                            if (comma < line.size() && line[comma] == '"') {
                                ++comma;
                                std::string en1;
                                while (comma < line.size() && line[comma] != '"') {
                                    en1.push_back(line[comma++]);
                                }
                                if (!en1.empty()) {
                                    extra_nonce1_ = en1;
                                    std::cout << "[Stratum] Subscribe OK; extra_nonce1=" << en1 << '\n';
                                }
                            }
                        }
                    }
                }
            }
            subscribe_done_.set_value(true);
            return;
        }
    }

    if (id_str == std::to_string(authorize_req_id_)) {
        if (!error.empty() && error != "null") {
            std::cerr << "[Stratum] Authorize failed: " << error << '\n';
        } else {
            const bool ok = line.find("\"result\":true") != std::string::npos ||
                            line.find("\"result\":\"true\"") != std::string::npos;
            std::cout << "[Stratum] Authorize " << (ok ? "OK" : "FAILED") << '\n';
        }
        return;
    }

    // Keepalive response or duplicate handshake
    if (line.find("KEEPALIVED") != std::string::npos ||
        line.find("\"status\":\"KEEPALIVED\"") != std::string::npos) {
        return;
    }

    // Submit reply
    const bool share_ok = line.find("\"result\":true") != std::string::npos ||
                          line.find("\"result\":\"true\"") != std::string::npos ||
                          line.find("\"status\":\"OK\"") != std::string::npos;
    if (share_ok) {
        std::cout << "[Stratum] Share accepted!\n";
    } else if (!result.empty() && result != "null") {
        std::cerr << "[Stratum] Share rejected: " << result << '\n';
    }
}

void StratumClient::process_cryptonote_job(const std::string& job_id,
                                           const std::string& blob_hex,
                                           const std::string& target_hex,
                                           const std::string& seed_hex) {
    if (job_id.empty() || blob_hex.empty()) return;

    Job job;
    job.job_id = job_id;
    job.block_template = hex_to_bytes(blob_hex);
    job.nonce_offset = 39; // Monero: nonce at byte 39 of blob
    job.nonce_size = 4;

    if (!seed_hex.empty()) {
        job.seed_key = hex_to_bytes(seed_hex);
    }

    if (target_hex.size() == 8) {
        const auto compact = hex_to_bytes(target_hex);
        std::fill(job.target.bytes.begin(), job.target.bytes.end(), std::byte{0xFF});
        if (compact.size() == 4) {
            std::copy(compact.begin(), compact.end(),
                      job.target.bytes.end() - 4);
        }
    } else if (target_hex.size() == 64) {
        const auto bytes = hex_to_bytes(target_hex);
        if (bytes.size() == 32) {
            std::copy(bytes.begin(), bytes.end(), job.target.bytes.begin());
        }
    } else {
        std::lock_guard lock(target_mutex_);
        job.target = current_target_;
    }

    {
        std::lock_guard lock(target_mutex_);
        current_target_ = job.target;
    }

    std::cout << "[Stratum] New job (CryptoNote): " << job_id
              << " blob=" << job.block_template.size() << " bytes\n";

    if (job_callback_) job_callback_(job);
}

void StratumClient::keepalive_loop() {
    while (connected_.load()) {
        for (int i = 0; i < 30 && connected_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!connected_.load()) break;

        if (protocol_ == StratumProtocol::CRYPTONOTE) {
            const auto id = request_id_.fetch_add(1);
            std::string msg = "{\"id\":" + std::to_string(id) +
                              ",\"jsonrpc\":\"2.0\"" +
                              ",\"method\":\"keepalived\"" +
                              ",\"params\":{" +
                                "\"id\":\"" + armrx::json::escape(session_id_) + "\"" +
                              "}}\n";
            std::lock_guard lock(send_mutex_);
            send_line(msg);
        }
    }
}

void StratumClient::reconnect_loop() {
    unsigned delay = base_delay_ms_;
    while (reconnect_enabled_.load()) {
        // Cap delay at max backoff
        if (delay > kMaxBackoffMs) delay = kMaxBackoffMs;

        std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        if (!reconnect_enabled_.load()) break;

        ++reconnect_attempts_;

        // Check if we've exhausted max retries (0 = forever)
        if (max_retries_ > 0 && reconnect_attempts_ > max_retries_) {
            if (error_callback_) {
                error_callback_("reconnect: max retries exhausted");
            }
            break;
        }

        std::cerr << "[Stratum] Reconnecting (attempt " << reconnect_attempts_
                  << ", delay " << delay << " ms)...\n";

        try {
            connect();
            // Success — reset state and exit
            reconnect_attempts_ = 0;
            std::cerr << "[Stratum] Reconnected successfully\n";
            return;
        } catch (const std::exception& ex) {
            std::cerr << "[Stratum] Reconnect failed: " << ex.what() << '\n';
        }

        // Exponential backoff (doubles each attempt)
        delay *= 2;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Static helpers
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::byte> StratumClient::hex_to_bytes(const std::string& hex) {
    std::vector<std::byte> result;
    result.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const auto hi = hex[i];
        const auto lo = hex[i + 1];
        auto from_hex = [](char c) -> std::uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        result.push_back(static_cast<std::byte>((from_hex(hi) << 4) | from_hex(lo)));
    }
    return result;
}

std::string StratumClient::bytes_to_hex(const std::vector<std::byte>& bytes) {
    static constexpr char h[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        const auto v = static_cast<std::uint8_t>(b);
        result.push_back(h[v >> 4]);
        result.push_back(h[v & 0xf]);
    }
    return result;
}

std::string StratumClient::nonce_to_hex(std::uint64_t nonce, std::size_t bytes) {
    // Little-endian byte order as used by Monero Stratum
    std::string result;
    result.reserve(bytes * 2);
    static constexpr char h[] = "0123456789abcdef";
    for (std::size_t i = 0; i < bytes; ++i) {
        const std::uint8_t byte = static_cast<std::uint8_t>(nonce >> (i * 8));
        result.push_back(h[byte >> 4]);
        result.push_back(h[byte & 0xf]);
    }
    return result;
}

Target StratumClient::difficulty_to_target(double diff) {
    // Convert difficulty D to 256-bit target: T = 2^256 / D
    // We store it as a 32-byte little-endian value.
    // For practical difficulties (D < 2^64) only the upper bytes are affected.
    Target t{};
    if (diff <= 0.0) {
        std::fill(t.bytes.begin(), t.bytes.end(), std::byte{0xFF});
        return t;
    }
    // Use the same division approach as main.cpp::difficulty_to_target
    std::uint64_t d = static_cast<std::uint64_t>(diff);
    if (d == 0) d = 1;
    std::uint64_t remainder = 0;
    for (int i = 31; i >= 0; --i) {
        const std::uint64_t val = (remainder << 8) | 0xff;
        t.bytes[static_cast<std::size_t>(i)] = static_cast<std::byte>(val / d);
        remainder = val % d;
    }
    return t;
}

} // namespace armrx
