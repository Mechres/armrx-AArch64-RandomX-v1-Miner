#include "armrx/tls_client.hpp"
#include "armrx/log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace armrx {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

TlsClient::TlsClient()
    : ctx_(nullptr, &SSL_CTX_free)
    , ssl_(nullptr, &SSL_free)
{
    // One-time library init (safe to call multiple times in >= 1.1.0)
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, nullptr);

    ctx_.reset(SSL_CTX_new(TLS_client_method()));
    if (!ctx_) {
        throw std::runtime_error("TlsClient: failed to create SSL_CTX");
    }

    // Verify peer certificate by default. Pools overwhelmingly use trusted CA
    // certs (Let's Encrypt). Opt out with set_verify_peer(false) for self-signed.
    SSL_CTX_set_verify(ctx_.get(), verify_peer_ ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_min_proto_version(ctx_.get(), TLS1_2_VERSION);

    // Use default system CA paths if available (harmless if absent)
    SSL_CTX_set_default_verify_paths(ctx_.get());
}

TlsClient::~TlsClient() {
    disconnect();
}

// ─────────────────────────────────────────────────────────────────────────────
// connect / disconnect
// ─────────────────────────────────────────────────────────────────────────────

bool TlsClient::connect(int fd, const std::string& host) {
    disconnect();

    ssl_.reset(SSL_new(ctx_.get()));
    if (!ssl_) {
        ARMRX_LOG_ERROR << "SSL_new failed";
        return false;
    }

    SSL_set_fd(ssl_.get(), fd);

    // Set SNI hostname so the pool can present the correct certificate
    if (!host.empty()) {
        SSL_set_tlsext_host_name(ssl_.get(), host.c_str());
        // Also verify the hostname matches the certificate
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl_.get());
        X509_VERIFY_PARAM_set1_host(param, host.c_str(), host.size());
    }

    const int ret = SSL_connect(ssl_.get());
    if (ret != 1) {
        const int ssl_err = SSL_get_error(ssl_.get(), ret);
        const unsigned long sys_err = ERR_get_error();
        std::string reason;
        if (ssl_err == SSL_ERROR_SYSCALL) {
            reason = std::strerror(errno);
        } else if (sys_err != 0) {
            reason = ERR_reason_error_string(sys_err);
        } else {
            reason = "SSL_error=" + std::to_string(ssl_err);
        }
        ARMRX_LOG_ERROR << "Handshake failed: " << reason;
        ssl_.reset();
        return false;
    }

    ARMRX_LOG_INFO << "Connected to " << host
              << " (" << SSL_get_cipher(ssl_.get()) << ")\n";
    return true;
}

void TlsClient::disconnect() {
    if (ssl_) {
        SSL_shutdown(ssl_.get());
        ssl_.reset();
    }
}

bool TlsClient::is_connected() const {
    return ssl_ != nullptr && SSL_get_fd(ssl_.get()) != -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// I/O
// ─────────────────────────────────────────────────────────────────────────────

int TlsClient::read(void* buf, std::size_t len) {
    if (!ssl_) return -1;
    ERR_clear_error();
    const int ret = SSL_read(ssl_.get(), buf, static_cast<int>(len));
    if (ret <= 0) {
        const int err = SSL_get_error(ssl_.get(), ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            errno = EAGAIN;
        }
    }
    return ret;
}

int TlsClient::write(const void* buf, std::size_t len) {
    if (!ssl_) return -1;
    ERR_clear_error();
    const int ret = SSL_write(ssl_.get(), buf, static_cast<int>(len));
    if (ret <= 0) {
        const int err = SSL_get_error(ssl_.get(), ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            errno = EAGAIN;
        }
    }
    return ret;
}

} // namespace armrx
