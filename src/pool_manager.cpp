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
    if (current_idx_ >= pools_.size()) return "(none)";
    return pools_[current_idx_].host + ":" + std::to_string(pools_[current_idx_].port);
}

unsigned PoolManager::reconnect_attempts() const {
    // Caller should hold stratum_mutex_ if concurrent failover may occur.
    // During normal display-loop usage, no concurrent writes happen.
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
    // Caller should hold stratum_mutex_ if concurrent failover may occur.
    return stratum_ && stratum_->is_connected();
}

void PoolManager::connect_to_current() {
    if (current_idx_ >= pools_.size()) return;
    const auto& entry = pools_[current_idx_];

    {
        std::lock_guard<std::mutex> lock(stratum_mutex_);
        stratum_ = std::make_unique<StratumClient>(
            entry.host, entry.port, wallet_, password_);
    }

    stratum_->enable_tls(tls_);
    stratum_->set_tls_verify_peer(tls_verify_);

    if (job_cb_) stratum_->set_job_callback(job_cb_);
    if (error_cb_) stratum_->set_error_callback(error_cb_);

    stratum_->set_reconnect_config(5, 1000);

    try {
        stratum_->connect();
    } catch (const std::exception& ex) {
        ARMRX_LOG_ERROR << current_pool_name() << ": " << ex.what();
    }
}

bool PoolManager::connect() {
    current_idx_ = 0;
    failover_cooldown_ = 0;
    connect_to_current();
    return is_connected();
}

void PoolManager::disconnect() {
    std::lock_guard<std::mutex> lock(stratum_mutex_);
    if (stratum_) {
        stratum_->disconnect();
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
            if (retries >= 5) {
                // Failover to next pool
                current_idx_ = (current_idx_ + 1) % pools_.size();
                failover_cooldown_ = 2;
                ARMRX_LOG_WARN << "Failing over to " << current_pool_name();
            }
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
