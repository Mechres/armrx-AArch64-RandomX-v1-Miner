#pragma once

#include "armrx/mining_common.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace armrx {

/**
 * Stratum V1 protocol client for Monero (RandomX) pool mining.
 *
 * Implements:
 *  - mining.subscribe    (session handshake)
 *  - mining.authorize    (worker authentication)
 *  - mining.set_target   (difficulty update from pool)
 *  - mining.notify       (new job/block template)
 *  - mining.submit       (share submission)
 *
 * The reader runs on a dedicated background thread. Use set_job_callback()
 * to receive new jobs and set_share_callback() to be notified when a share
 * should be submitted (share result from pool is printed to stderr).
 *
 * Thread-safety: connect/disconnect/submit may be called from any thread.
 */
class StratumClient {
public:
    using JobCallback  = std::function<void(const Job&)>;
    using ErrorCallback = std::function<void(const std::string& reason)>;

    StratumClient(std::string host, std::uint16_t port,
                  std::string wallet, std::string password = "x");
    ~StratumClient();

    // Disable copy
    StratumClient(const StratumClient&) = delete;
    StratumClient& operator=(const StratumClient&) = delete;

    /**
     * Establish TCP connection, subscribe, and authorise the worker.
     * Starts the reader thread. Throws std::runtime_error on failure.
     */
    void connect();

    /**
     * Gracefully close the connection and join the reader thread.
     */
    void disconnect();

    /**
     * Submit a found share back to the pool.
     * @param job    The job the nonce was found for.
     * @param nonce  The winning nonce value (little-endian, 4 bytes).
     * @param hash   The resulting 32-byte hash (informational).
     */
    void submit_share(const Job& job, std::uint64_t nonce,
                      const std::array<std::byte, 32>& hash);

    /** Register a callback invoked whenever the pool sends a new job. */
    void set_job_callback(JobCallback cb) { job_callback_ = std::move(cb); }

    /** Register a callback invoked when the connection is lost / errors. */
    void set_error_callback(ErrorCallback cb) { error_callback_ = std::move(cb); }

    [[nodiscard]] bool is_connected() const { return connected_.load(); }

private:
    // --- networking helpers ---
    void reader_thread_fn();

    // Sends a line-terminated JSON string; caller must hold send_mutex_.
    void send_line(const std::string& json_line);

    // Low-level socket write (handles EINTR).
    bool write_all(const char* buf, std::size_t len);

    // Low-level socket read line.
    bool read_line(std::string& out);

    // --- stratum message builders ---
    std::string build_subscribe_msg() const;
    std::string build_authorize_msg() const;
    std::string build_submit_msg(const Job& job, std::uint64_t nonce) const;

    // --- stratum message handlers ---
    void handle_line(const std::string& line);
    void handle_notify(const std::string& line);
    void handle_set_target(const std::string& line);
    void handle_set_difficulty(const std::string& line);
    void handle_reply(const std::string& line);

    // --- helpers ---
    static std::vector<std::byte> hex_to_bytes(const std::string& hex);
    static std::string bytes_to_hex(const std::vector<std::byte>& bytes);
    static std::string nonce_to_hex(std::uint64_t nonce, std::size_t bytes = 4);
    static Target difficulty_to_target(double diff);

    // --- members ---
    std::string host_;
    std::uint16_t port_;
    std::string wallet_;
    std::string password_;

    int sockfd_{-1};
    std::atomic<bool> connected_{false};

    std::string session_id_;
    std::string extra_nonce1_;      // Pool-assigned nonce prefix (hex)
    std::size_t extra_nonce2_size_{4}; // Extra nonce 2 length in bytes

    mutable std::mutex send_mutex_;
    std::atomic<std::uint64_t> request_id_{1};

    // Current target (updated by mining.set_target or mining.set_difficulty)
    mutable std::mutex target_mutex_;
    Target current_target_{};

    JobCallback  job_callback_;
    ErrorCallback error_callback_;

    std::thread reader_thread_;

    // Read buffer (line accumulator)
    std::string read_buf_;
};

} // namespace armrx
