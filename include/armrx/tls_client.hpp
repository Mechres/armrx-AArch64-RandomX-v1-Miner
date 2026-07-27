#pragma once

#include <cstddef>
#include <memory>
#include <string>

struct ssl_st;
struct ssl_ctx_st;
struct bio_st;

namespace armrx {

/**
 * RAII wrapper around OpenSSL for TLS-encrypted pool connections.
 *
 * Usage:
 *   TlsClient tls;
 *   tls.connect(fd, hostname);   // wrap TCP socket + handshake
 *   tls.write(data, len);
 *   tls.read(buf, sizeof(buf));
 *   tls.disconnect();            // tear down SSL only
 */
class TlsClient {
public:
    TlsClient();
    ~TlsClient();

    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    /** Wrap an existing TCP socket fd with TLS and perform the handshake. */
    bool connect(int fd, const std::string& host);

    /** Enable/disable peer certificate verification (default: enabled). */
    void set_verify_peer(bool v);

    /** SSL_read wrapper. Returns bytes read, or <= 0 on error/closed. */
    int read(void* buf, std::size_t len);

    /** SSL_write wrapper. Returns bytes written, or <= 0 on error. */
    int write(const void* buf, std::size_t len);

    /** Shut down the SSL connection (does NOT close the TCP socket). */
    void disconnect();

    /** Returns true if the TLS handshake completed. */
    [[nodiscard]] bool is_connected() const;

private:
    std::unique_ptr<ssl_ctx_st, void(*)(ssl_ctx_st*)> ctx_;
    std::unique_ptr<ssl_st, void(*)(ssl_st*)> ssl_;
    bool verify_peer_ = true;
};

} // namespace armrx
