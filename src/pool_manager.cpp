#include "armrx/pool_manager.hpp"
#include "armrx/log.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace armrx {

PoolManager::PoolManager(const std::vector<PoolConfig>& pools,
                         const std::string& wallet,
                         const std::string& password,
                         bool tls, bool tls_verify)
    : wallet_(wallet), password_(password), tls_(tls), tls_verify_(tls_verify)
{
    pools_.reserve(pools.size());
    for (const auto& p : pools) {
        pools_.push_back({p.host, p.port});
    }
    if (pools_.empty()) {
        throw std::invalid_argument("PoolManager: at least one pool required");
    }
}

void PoolManager::set_job_callback(StratumClient::JobCallback cb) {
    job_cb_ = std::move(cb);
}

void PoolManager::set_error_callback(StratumClient::ErrorCallback cb) {
    error_cb_ = std::move(cb);
}

std::string PoolManager::current_pool_name() const {
    std::lock_guard<std::mutex> lock(stratum_mutex_);
    if (current_idx_ >= pools_.size()) return "(none)";
    return pools_[current_idx_].host + ":" + std::to_string(pools_[current_idx_].port);
}

unsigned PoolManager::reconnect_attempts() const {
    std::lock_guard<std::mutex> lock(stratum_mutex_);
    return stratum_ ? stratum_->reconnect_attempts() : 0;
}

std::uint64_t PoolManager::shares_accepted() const {
    std::lock_guard<std::mutex> lock(stratum_mutex_);
    return stratum_ ? stratum_->shares_accepted() : 0;
}

std::uint64_t PoolManager::shares_rejected() const {
    std::lock_guard<std::mutex> lock(stratum_mutex_);
    return stratum_ ? stratum_->shares_rejected() : 0;
}

bool PoolManager::is_connected() const {
    std::lock_guard<std::mutex> lock(stratum_mutex_);
    return stratum_ && stratum_->is_connected();
}

void PoolManager::connect_to_current() {
    if (current_idx_ >= pools_.size()) return;
    const auto& entry = pools_[current_idx_];

    StratumClient* raw_stratum = nullptr;
    {
        std::lock_guard<std::mutex> lock(stratum_mutex_);
        stratum_ = std::make_unique<StratumClient>(
            entry.host, entry.port, wallet_, password_);
        raw_stratum = stratum_.get();

        // Fast configuration calls under the lock (not blocking like connect())
        raw_stratum->enable_tls(tls_);
        raw_stratum->set_tls_verify_peer(tls_verify_);
        if (job_cb_) raw_stratum->set_job_callback(job_cb_);
        if (error_cb_) raw_stratum->set_error_callback(error_cb_);
        raw_stratum->set_reconnect_config(5, 1000);
    }

    // Slow blocking connect() runs unlocked to avoid holding the lock
    // across a potentially-long TCP/TLS handshake.
    try {
        raw_stratum->connect();
    } catch (const std::exception& ex) {
        ARMRX_LOG_ERROR << current_pool_name() << ": " << ex.what();
    }
}

bool PoolManager::connect() {
    current_idx_ = 0;
    failover_cooldown_ = 0;
    sync_retry_count_ = 0;
    connect_to_current();
    return is_connected();
}

void PoolManager::disconnect() {
    // Take the client pointer out from under the lock, then tear it down
    // WITHOUT holding stratum_mutex_. StratumClient::disconnect() joins the
    // reader thread, and that thread's teardown path invokes error_callback_,
    // which calls current_pool_name() — a stratum_mutex_ reader. Holding the
    // mutex across the join would self-deadlock: this thread waits on the
    // reader thread to finish, while the reader thread blocks on the very
    // mutex we hold. This was the actual SIGINT-on-`--pool` hang: hashing
    // stopped (engine.stop() joins workers cleanly) but the process never
    // returned to the prompt because disconnect() wedged in the reader join.
    StratumClient* raw = nullptr;
    {
        std::lock_guard<std::mutex> lock(stratum_mutex_);
        raw = stratum_.get();
    }
    if (raw) {
        raw->disconnect();
    }
    {
        std::lock_guard<std::mutex> lock(stratum_mutex_);
        stratum_.reset();
    }
}

void PoolManager::submit_share(const Job& job, std::uint64_t nonce,
                               const std::array<std::byte, 32>& hash) {
    std::lock_guard<std::mutex> lock(stratum_mutex_);
    if (stratum_) stratum_->submit_share(job, nonce, hash);
}

void PoolManager::tick() {
    // connect_to_current() acquires stratum_mutex_ itself (its slow blocking
    // stratum_->connect() call runs deliberately unlocked). It must never be
    // called while already holding that mutex — std::mutex is non-recursive,
    // so relocking it here would self-deadlock this thread permanently the
    // first time a real failover cooldown elapsed. Read/mutate the failover
    // state under the lock, but defer the actual reconnect call until after
    // the lock_guard's scope ends.
    bool should_reconnect_now = false;
    {
        std::lock_guard<std::mutex> lock(stratum_mutex_);
        if (!stratum_) return;

        if (!stratum_->is_connected() && failover_cooldown_ == 0) {
            const auto retries = stratum_->reconnect_attempts();
            if (retries > 0 || stratum_->reconnect_loop_active()) {
                // The async reconnect_loop() is either live and counting its
                // own retries, or alive but hasn't incremented yet (still in
                // its very first backoff sleep) — either way, a connection
                // that succeeded once and then dropped. Let it drive failover
                // on its own real backoff schedule; don't race ahead of it.
                sync_retry_count_ = 0;
                if (retries >= 5) {
                    current_idx_ = (current_idx_ + 1) % pools_.size();
                    failover_cooldown_ = 2;
                    ARMRX_LOG_WARN << "Failing over to " << pool_name_nolock();
                }
            } else {
                // No async reconnect_loop has ever run for this connection
                // attempt — the pool was unreachable synchronously in
                // connect_to_current() (DNS failure, connection refused).
                // reconnect_loop() is only ever armed by the reader thread
                // noticing a previously live connection go down, so a pool
                // dead from process startup would otherwise never fail over.
                // Drive our own retry count for this case with the same
                // 5-retries/2s-cooldown policy.
                ++sync_retry_count_;
                if (sync_retry_count_ >= 5) {
                    current_idx_ = (current_idx_ + 1) % pools_.size();
                    failover_cooldown_ = 2;
                    sync_retry_count_ = 0;
                    ARMRX_LOG_WARN << "Failing over to " << pool_name_nolock();
                } else {
                    // Retry the same (still-current) pool after a short cooldown.
                    failover_cooldown_ = 2;
                }
            }
        } else if (stratum_->is_connected()) {
            sync_retry_count_ = 0;
        }

        if (failover_cooldown_ > 0) {
            --failover_cooldown_;
            if (failover_cooldown_ == 0) {
                should_reconnect_now = true;
            }
        }
    }

    if (should_reconnect_now) {
        connect_to_current();
    }
}

} // namespace armrx
