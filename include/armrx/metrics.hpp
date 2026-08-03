#pragma once

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <unistd.h>
#include "armrx/log.hpp"

namespace armrx {

/// Lightweight Prometheus metrics HTTP endpoint.
/// Listens on localhost:{port}, serves GET /metrics with Prometheus text format.
/// Header-only implementation — no .cpp file needed.
///
/// Ownership note: the listening socket is created, used, and closed entirely
/// inside the worker thread — the constructor hands the fd to the thread and
/// never reads it back. This avoids a data race on the socket fd between the
/// worker thread (which accepts) and the destructor (which would otherwise
/// inspect/close it). The destructor only flips `running_` to false and joins,
/// letting the worker close the socket as it exits the accept loop.
class MetricsExporter {
public:
    using MetricProvider = std::function<std::string()>;

    /// Start the HTTP server on localhost:port.
    /// provider is called on each request to produce the metrics body.
    MetricsExporter(std::uint16_t port, MetricProvider provider)
        : running_(true)
    {
        thread_ = std::thread([this, port, prov = std::move(provider)]() {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) { running_ = false; return; }
            int opt = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = ::htons(port);
            addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
            if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                ARMRX_LOG_WARN << "Metrics bind(" << port << ") failed";
                ::close(fd); running_ = false; return;
            }
            if (::listen(fd, 5) < 0) {
                ARMRX_LOG_WARN << "Metrics listen() failed";
                ::close(fd); running_ = false; return;
            }
            ARMRX_LOG_INFO << "Metrics listening on http://127.0.0.1:" << port << "/metrics";
            while (running_) {
                struct sockaddr_in client{};
                socklen_t client_len = sizeof(client);
                int cfd = ::accept(fd, (struct sockaddr*)&client, &client_len);
                if (cfd < 0) { if (running_) break; break; }
                char req[1024];
                ssize_t n = ::read(cfd, req, sizeof(req) - 1);
                if (n > 0) {
                    req[n] = '\0';
                    std::string r(req);
                    bool ok = r.find("GET /metrics ") == 0;
                    if (ok) {
                        std::string body = prov();
                        std::string resp =
                            "HTTP/1.0 200 OK\r\n"
                            "Content-Type: text/plain\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "Connection: close\r\n\r\n";
                        ::write(cfd, resp.data(), resp.size());
                        ::write(cfd, body.data(), body.size());
                    } else {
                        const char* nf = "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n";
                        ::write(cfd, nf, std::strlen(nf));
                    }
                }
                ::close(cfd);
            }
            // Socket owned solely by this thread: close it here on exit.
            ::close(fd);
        });
    }

    ~MetricsExporter() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    MetricsExporter(const MetricsExporter&) = delete;
    MetricsExporter& operator=(const MetricsExporter&) = delete;

private:
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace armrx
