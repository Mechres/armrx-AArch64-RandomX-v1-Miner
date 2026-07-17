#include "armrx/pool_manager.hpp"

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
    return stratum_ ? stratum_->reconnect_attempts() : 0;
}

bool PoolManager::is_connected() const {
    return stratum_ && stratum_->is_connected();
}

void PoolManager::connect_to_current() {
    if (current_idx_ >= pools_.size()) return;
    const auto& entry = pools_[current_idx_];

    // Create a fresh client for this pool
    stratum_ = std::make_unique<StratumClient>(
        entry.host, entry.port, wallet_, password_);

    stratum_->enable_tls(tls_);
    stratum_->set_tls_verify_peer(tls_verify_);

    if (job_cb_) stratum_->set_job_callback(job_cb_);
    if (error_cb_) stratum_->set_error_callback(error_cb_);

    stratum_->set_reconnect_config(5, 1000);

    try {
        stratum_->connect();
    } catch (const std::exception& ex) {
        std::cerr << "[Pool] " << current_pool_name() << ": " << ex.what() << '\n';
    }
}

bool PoolManager::connect() {
    current_idx_ = 0;
    failover_cooldown_ = 0;
    connect_to_current();
    return is_connected();
}

void PoolManager::disconnect() {
    if (stratum_) {
        stratum_->disconnect();
        stratum_.reset();
    }
}

void PoolManager::submit_share(const Job& job, std::uint64_t nonce,
                               const std::array<std::byte, 32>& hash) {
    if (stratum_) stratum_->submit_share(job, nonce, hash);
}

void PoolManager::tick() {
    if (!stratum_) return;

    if (!stratum_->is_connected() && failover_cooldown_ == 0) {
        const auto retries = stratum_->reconnect_attempts();
        if (retries >= 5) {
            // Failover to next pool
            current_idx_ = (current_idx_ + 1) % pools_.size();
            failover_cooldown_ = 2;
            std::cerr << "[Pool] Failing over to " << current_pool_name() << '\n';
        }
    }

    if (failover_cooldown_ > 0) {
        --failover_cooldown_;
        if (failover_cooldown_ == 0) {
            connect_to_current();
        }
    }
}

} // namespace armrx
