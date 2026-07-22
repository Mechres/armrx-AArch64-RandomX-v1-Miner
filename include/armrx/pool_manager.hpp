#pragma once

#include "armrx/stratum_client.hpp"
#include "armrx/config.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace armrx {

/// Manages pool connections with automatic failover.
/// Owns the StratumClient lifecycle and rotates between pools
/// when the current pool exhausts its reconnect retries.
class PoolManager {
public:
    PoolManager(const std::vector<PoolConfig>& pools,
                const std::string& wallet,
                const std::string& password,
                bool tls = false,
                bool tls_verify = true);

    /// Register the callback for new mining jobs from the pool.
    void set_job_callback(StratumClient::JobCallback cb);

    /// Register the callback for connection errors / status messages.
    void set_error_callback(StratumClient::ErrorCallback cb);

    /// Connect to the first/current pool.
    /// Returns true on successful handshake.
    bool connect();

    /// Disconnect from the current pool and stop any reconnect threads.
    void disconnect();

    /// Returns true if the current pool connection is active.
    bool is_connected() const;

    /// Called periodically (e.g. once per second from the main loop).
    /// Handles failover: after 5 reconnect retries, rotates to the next pool
    /// with a 2-second cooldown.
    void tick();

    /// Human-readable name of the current pool ("host:port").
    std::string current_pool_name() const;

    /// Number of reconnect attempts on the current pool.
    unsigned reconnect_attempts() const;

    /// Number of shares accepted by the pool.
    std::uint64_t shares_accepted() const;

    /// Number of shares rejected by the pool.
    std::uint64_t shares_rejected() const;

    /// Submit a found share to the current pool.
    void submit_share(const Job& job, std::uint64_t nonce,
                      const std::array<std::byte, 32>& hash);

private:
    struct PoolEntry {
        std::string host;
        std::uint16_t port;
    };

    void connect_to_current();

    std::vector<PoolEntry> pools_;
    unsigned current_idx_ = 0;
    unsigned failover_cooldown_ = 0;
    // Retry counter for pools that are unreachable from the very first connect
    // attempt (DNS failure, connection refused) — StratumClient::reconnect_attempts()
    // stays 0 forever in that case, since reconnect_loop() is only ever armed by
    // the reader thread noticing a *previously live* connection drop. Tracked here
    // so a pool that's dead from process startup still eventually fails over.
    unsigned sync_retry_count_ = 0;

    std::string wallet_;
    std::string password_;
    bool tls_ = false;
    bool tls_verify_ = true;

    std::unique_ptr<StratumClient> stratum_;

    StratumClient::JobCallback job_cb_;
    StratumClient::ErrorCallback error_cb_;
    mutable std::mutex stratum_mutex_;
};

} // namespace armrx
