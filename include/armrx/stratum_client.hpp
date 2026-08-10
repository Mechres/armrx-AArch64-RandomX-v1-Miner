#pragma once

#include "armrx/mining_common.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <future>
#include <string>
#include <thread>
#include <vector>

#ifdef ARMRX_HAVE_TLS
#include "armrx/tls_client.hpp"
#endif

namespace armrx {

enum class StratumProtocol {
    AUTO,
    STRATUM_V1,
    CRYPTONOTE
};

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

    /**
    /**
     * Enable TLS encryption for the pool connection.
     * Must be called before connect(). Requires OpenSSL at build time.
     */
    void enable_tls(bool enabled) { tls_enabled_ = enabled; }
    void set_tls_verify_peer(bool v) { tls_verify_peer_ = v; }
    [[nodiscard]] std::uint64_t shares_accepted() const { return shares_accepted_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t shares_rejected() const { return shares_rejected_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t shares_dropped() const { return shares_dropped_.load(std::memory_order_relaxed); }

    /**
     * Configure automatic reconnection on disconnect.
     * @param max_retries  Maximum reconnection attempts before giving up
     *                     (0 = retry forever, default 10).
     * @param base_delay_ms  Initial backoff delay in milliseconds (default 1000).
     *                       Each retry doubles up to 30 s cap.
     */
    void set_reconnect_config(unsigned max_retries = 10, unsigned base_delay_ms = 1000);

    /**
     * Override where the pool-supplied nonce lives within the block template blob.
     * Defaults to Monero's layout (offset 39, 4 bytes). Only needed for alternative
     * RandomX-based chains with a different blob format.
     */
    void set_nonce_config(std::size_t offset, std::size_t size) {
        nonce_offset_ = offset;
        nonce_size_ = size;
    }

    [[nodiscard]] bool is_connected() const { return connected_.load(); }

    /** Returns the number of consecutive reconnect attempts since last clean connect. */
    [[nodiscard]] unsigned reconnect_attempts() const { return reconnect_attempts_; }

    /**
     * Whether the background reconnect_loop() is currently running for this
     * connection attempt. reconnect_attempts() alone can't distinguish "the
     * loop hasn't incremented its counter yet" (still in its first backoff
     * sleep) from "the loop never started at all" (e.g. connect() threw
     * synchronously and no reader thread ever ran to notice a drop and arm
     * it) — callers that need that distinction (PoolManager's failover logic)
     * should check this instead of inferring it from reconnect_attempts()==0.
     */
    [[nodiscard]] bool reconnect_loop_active() const { return reconnect_loop_active_.load(); }

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
    std::string build_login_msg() const;
    std::string build_authorize_msg() const;
    std::string build_submit_msg(const Job& job, std::uint64_t nonce,
                                 const std::array<std::byte, 32>& hash) const;

    // --- stratum message handlers ---
    void handle_line(const std::string& line);
    void handle_notify(const std::string& line);
    void handle_set_target(const std::string& line);
    void handle_set_difficulty(const std::string& line);
    void handle_reply(const std::string& line);

    // --- helpers ---
    void close_connection();
    void process_cryptonote_job(const std::string& job_id,
                                const std::string& blob_hex,
                                const std::string& target_hex,
                                const std::string& seed_hex);
    void keepalive_loop();

    static std::vector<std::byte> hex_to_bytes(const std::string& hex);
    static std::string bytes_to_hex(const std::vector<std::byte>& bytes);
    static std::string nonce_to_hex(std::uint64_t nonce, std::size_t bytes = 4);
    static Target difficulty_to_target(double diff);

    // --- reconnect logic ---
    void reconnect_loop();

    // --- members ---
    std::string host_;
    std::uint16_t port_;
    std::string wallet_;
    std::string password_;

    // Socket fd. std::atomic so the reader thread's recv() and close_connection()'s
    // write of -1 cannot race on a plain int (TSan-flagged data race).
    std::atomic<int> sockfd_{-1};
    std::atomic<bool> connected_{false};

    std::string session_id_;
    // Parsed from mining.subscribe / mining.set_extranonce but INFORMATIONAL
    // ONLY for Monero mining: Monero embeds the nonce as a fixed field inside
    // the template blob (offset 39, 4 bytes), so there is no extranonce1/2
    // concatenation. A non-empty value on the Stratum V1 path indicates a
    // Bitcoin-style pool whose 4-param mining.submit armrx cannot satisfy —
    // surfaced once via warned_extranonce_v1_ in build_submit_msg().
    std::string extra_nonce1_;      // Pool-assigned nonce prefix (hex)
    // Guards extra_nonce1_ (written by the reader thread on mining.set_extranonce,
    // read by build_submit_msg on the worker submit path). Separate from send_mutex_
    // because build_submit_msg runs before send_mutex_ is acquired in submit_share.
    mutable std::mutex extra_nonce_mutex_;
    std::size_t extra_nonce2_size_{4}; // Extra nonce 2 length in bytes
    mutable bool warned_extranonce_v1_{false}; // log-once guard (see above)

    // Nonce field location within the block template blob. Monero defaults;
    // overridable via set_nonce_config() for other RandomX-based chains.
    std::size_t nonce_offset_{39};
    std::size_t nonce_size_{4};

    mutable std::mutex send_mutex_;
    mutable std::atomic<std::uint64_t> request_id_{1};

    // Current target (updated by mining.set_target or mining.set_difficulty)
    mutable std::mutex target_mutex_;
    Target current_target_{};

    JobCallback  job_callback_;
    ErrorCallback error_callback_;

    std::thread reader_thread_;

    // Read buffer (line accumulator)
    std::string read_buf_;

    // Handshake synchronization
    std::promise<bool> subscribe_done_;
    std::atomic<bool> subscribe_ok_{false};

    // Reconnect configuration
    unsigned max_retries_{10};
    unsigned base_delay_ms_{1000};
    std::atomic<unsigned> reconnect_attempts_{0};
    std::atomic<bool> reconnect_enabled_{true};
    std::atomic<bool> reconnect_loop_active_{false};
    std::thread reconnect_thread_;
    // Lets disconnect() wake reconnect_loop() immediately instead of leaving it
    // blocked in sleep_for() for up to kMaxBackoffMs before the destructor's
    // join() can return.
    std::mutex reconnect_cv_mutex_;
    std::condition_variable reconnect_cv_;

    // Share result counters (written by reader thread, read by main thread)
    std::atomic<std::uint64_t> shares_accepted_{0};
    std::atomic<std::uint64_t> shares_rejected_{0};
    std::atomic<std::uint64_t> shares_dropped_{0};  // send failures (share lost)

    // TLS
    bool tls_enabled_{false};
    bool tls_verify_peer_{true};
#ifdef ARMRX_HAVE_TLS
    std::unique_ptr<TlsClient> tls_;
#endif

    // CryptoNote and fallback tracking
    StratumProtocol protocol_{StratumProtocol::AUTO};
    mutable std::atomic<std::uint64_t> handshake_req_id_{0};
    mutable std::atomic<std::uint64_t> authorize_req_id_{0};
    std::thread keepalive_thread_;
    std::atomic<bool> fallback_in_progress_{false};
    std::atomic<bool> handshake_in_progress_{false};
};

/** Default reconnect config constant. */
inline constexpr unsigned kDefaultMaxRetries = 10;
inline constexpr unsigned kDefaultBaseDelayMs = 1000;
inline constexpr unsigned kMaxBackoffMs = 30000;

} // namespace armrx
