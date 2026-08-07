/**
 * Mock Stratum protocol integration tests (PLAN.md §3.2).
 *
 * Runs a minimal loopback TCP server (raw POSIX sockets, no new dependency)
 * that scripts realistic pool message sequences against a real StratumClient
 * / PoolManager, covering the wire-protocol paths that had zero test
 * coverage before this file: Stratum V1 and CryptoNote handshakes, the
 * CryptoNote-first-then-fallback-to-V1 AUTO negotiation, reconnect with
 * exponential backoff, multi-pool failover, and malformed-input robustness.
 *
 * Uses the polling-with-timeout pattern established in tests/test_mining.cpp
 * rather than fixed sleeps, since that session found fixed-sleep guesses
 * produce flaky tests under real hardware/scheduling variance.
 */

#include "armrx/stratum_client.hpp"
#include "armrx/pool_manager.hpp"
#include "armrx/config.hpp"
#include "armrx/log.hpp"
#include "armrx/json.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ── Minimal server-side socket helpers (deliberately separate from
//    StratumClient's own read_line/write_all — this is the "hostile/real
//    pool" side of the wire, so it must not share implementation with the
//    code under test). ──────────────────────────────────────────────────

int listen_on_ephemeral_port(std::uint16_t& out_port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // kernel picks an ephemeral port
    int rc = ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    assert(rc == 0);
    rc = ::listen(fd, 4);
    assert(rc == 0);
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
    out_port = ::ntohs(addr.sin_port);
    return fd;
}

int accept_one(int listen_fd, int timeout_ms = 5000) {
    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in client{};
    socklen_t client_len = sizeof(client);
    return ::accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client), &client_len);
}

// Reads one newline-terminated line from a connected client socket.
// Returns false on EOF/error/timeout.
bool server_recv_line(int fd, std::string& out, int timeout_ms = 5000) {
    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string buf;
    char tmp[4096];
    while (true) {
        auto nl = buf.find('\n');
        if (nl != std::string::npos) {
            out = buf.substr(0, nl);
            return true;
        }
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<std::size_t>(n));
    }
}

bool server_send_line(int fd, const std::string& line) {
    std::string out = line;
    if (out.empty() || out.back() != '\n') out.push_back('\n');
    std::size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Fixed test fixture values, reused across scenarios.
const std::string kBlobHex(152, '0');  // 76 zero bytes, matching main.cpp's benchmark job convention
const std::string kSeedHex(64, '1');   // 32-byte dummy seed
const char* kTargetHex = "ffffffff";   // compact target: accept-everything

/// Poll `cond` until it returns true or `timeout` elapses. Returns whether it became true.
template <typename Cond>
bool wait_until(Cond cond, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return cond();
}

// ── Scenario 1: Stratum V1 full flow, reached via the AUTO protocol's
//    CryptoNote-first negotiation rejecting on the first connection and
//    falling back to Stratum V1 on a second connection — this is the exact
//    path real incompatible-protocol pools (e.g. herominers on the wrong
//    port) trigger, per AGENTS.md's documented CryptoNote-first behavior. ──

void test_stratum_v1_full_flow_via_auto_fallback() {
    std::uint16_t port = 0;
    int listen_fd = listen_on_ephemeral_port(port);

    std::atomic<bool> notify_sent{false};
    std::thread server([&] {
        // Connection 1: client tries CryptoNote `login` first (AUTO default).
        // Reject it so the client falls back to Stratum V1.
        int c1 = accept_one(listen_fd);
        assert(c1 >= 0);
        std::string line;
        bool got = server_recv_line(c1, line);
        assert(got);
        assert(line.find("\"method\":\"login\"") != std::string::npos);
        const auto id1 = armrx::json::get_raw(line, "id");
        server_send_line(c1, "{\"id\":" + id1 + ",\"jsonrpc\":\"2.0\","
                              "\"error\":{\"code\":-1,\"message\":\"unsupported\"}}");
        ::close(c1);

        // Connection 2: client retries with Stratum V1 `mining.subscribe`.
        int c2 = accept_one(listen_fd);
        assert(c2 >= 0);
        got = server_recv_line(c2, line);
        assert(got);
        assert(line.find("\"method\":\"mining.subscribe\"") != std::string::npos);
        const auto id2 = armrx::json::get_raw(line, "id");
        server_send_line(c2, "{\"id\":" + id2 + ",\"error\":null,"
                              "\"result\":[[[\"mining.notify\",\"subid1\"]],\"deadbeef\",4]}");

        got = server_recv_line(c2, line);
        assert(got);
        assert(line.find("\"method\":\"mining.authorize\"") != std::string::npos);
        const auto id3 = armrx::json::get_raw(line, "id");
        server_send_line(c2, "{\"id\":" + id3 + ",\"error\":null,\"result\":true}");

        // Push an unsolicited mining.notify (server-initiated, no reply expected).
        // Spec order: [job_id, blob, target, seed_hash, clean_jobs] — the seed
        // hash sits at index 3, clean_jobs at index 4 (corrected from an older
        // fixture that placed the seed at index 4, matching the pre-fix parse).
        server_send_line(c2,
            "{\"method\":\"mining.notify\",\"params\":[\"job1\",\"" + kBlobHex + "\",\"" +
            std::string(kTargetHex) + "\",\"" + kSeedHex + "\",0]}");
        notify_sent.store(true);

        // Keep the connection open briefly so the client can process the
        // notify before we tear down.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ::close(c2);
    });

    armrx::StratumClient client("127.0.0.1", port, "test_wallet", "x");
    std::mutex job_mutex;
    armrx::Job received_job;
    std::atomic<bool> job_received{false};
    client.set_job_callback([&](const armrx::Job& j) {
        std::lock_guard<std::mutex> lock(job_mutex);
        received_job = j;
        job_received.store(true);
    });

    client.connect();
    assert(client.is_connected());

    bool ok = wait_until([&] { return job_received.load(); }, std::chrono::seconds(5));
    assert(ok);
    {
        std::lock_guard<std::mutex> lock(job_mutex);
        assert(received_job.job_id == "job1");
        assert(received_job.block_template.size() == 76);
        // The seed hash must be parsed from params[3] (CryptoNote notify
        // order [job_id, blob, target, seed_hash, clean_jobs]). A parser
        // reading params[4] would get the clean_jobs flag ("0") here and
        // produce an empty/garbage seed_key — which this assertion catches.
        assert(received_job.seed_key.size() == 32);
        // kSeedHex = 64 '1' chars → each "11" pair decodes to 0x11.
        assert(std::memcmp(received_job.seed_key.data(),
                           std::vector<std::byte>(32, std::byte{0x11}).data(), 32) == 0);
    }

    client.disconnect();
    server.join();
    ::close(listen_fd);
    std::cout << "[test_pool_protocol] test_stratum_v1_full_flow_via_auto_fallback passed\n";
}

// ── Scenario 2: CryptoNote full flow (login succeeds on the first try). ──

void test_cryptonote_full_flow() {
    std::uint16_t port = 0;
    int listen_fd = listen_on_ephemeral_port(port);

    std::thread server([&] {
        int c = accept_one(listen_fd);
        assert(c >= 0);
        std::string line;
        bool got = server_recv_line(c, line);
        assert(got);
        assert(line.find("\"method\":\"login\"") != std::string::npos);
        const auto id = armrx::json::get_raw(line, "id");

        // Successful login reply, with a job bundled in the result (as real
        // CryptoNote pools do) to exercise process_cryptonote_job() via the
        // handshake reply path, not just the standalone "job" notification.
        server_send_line(c,
            "{\"id\":" + id + ",\"jsonrpc\":\"2.0\",\"error\":null,"
            "\"result\":{\"id\":\"sess123\",\"job\":{"
            "\"job_id\":\"cnjob1\",\"blob\":\"" + kBlobHex + "\","
            "\"target\":\"" + std::string(kTargetHex) + "\","
            "\"seed_hash\":\"" + kSeedHex + "\"}}}");

        // Client should not send anything else immediately for CryptoNote
        // (no separate authorize step); give it time to process, then close.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ::close(c);
    });

    armrx::StratumClient client("127.0.0.1", port, "test_wallet", "x");
    std::mutex job_mutex;
    armrx::Job received_job;
    std::atomic<bool> job_received{false};
    client.set_job_callback([&](const armrx::Job& j) {
        std::lock_guard<std::mutex> lock(job_mutex);
        received_job = j;
        job_received.store(true);
    });

    client.connect();
    assert(client.is_connected());

    bool ok = wait_until([&] { return job_received.load(); }, std::chrono::seconds(5));
    assert(ok);
    {
        std::lock_guard<std::mutex> lock(job_mutex);
        assert(received_job.job_id == "cnjob1");
        assert(received_job.block_template.size() == 76);
    }

    client.disconnect();
    server.join();
    ::close(listen_fd);
    std::cout << "[test_pool_protocol] test_cryptonote_full_flow passed\n";
}

// ── Scenario 3: mid-session disconnect triggers reconnect_loop() with
//    exponential backoff, exhausting after a small configured retry count.
//    Uses StratumClient's own public set_reconnect_config() with a short
//    base delay so this completes in well under a second, rather than
//    waiting through PoolManager's hardcoded 1s-base/5-retry production
//    config (see test_pool_failover() below for why that one is slow). ──

void test_reconnect_backoff_exhaustion() {
    std::uint16_t port = 0;
    int listen_fd = listen_on_ephemeral_port(port);

    std::thread server([&] {
        // Accept once, complete a real CryptoNote handshake, then drop the
        // connection to simulate a mid-session disconnect.
        int c = accept_one(listen_fd);
        assert(c >= 0);
        std::string line;
        bool got = server_recv_line(c, line);
        assert(got);
        const auto id = armrx::json::get_raw(line, "id");
        server_send_line(c, "{\"id\":" + id + ",\"jsonrpc\":\"2.0\",\"error\":null,"
                             "\"result\":{\"id\":\"sess1\"}}");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ::close(c); // drop mid-session

        // No listener for subsequent reconnect attempts — they must fail
        // fast (connection refused), exercising pure backoff timing rather
        // than handshake timing.
        ::close(listen_fd);
    });

    armrx::StratumClient client("127.0.0.1", port, "test_wallet", "x");
    client.set_reconnect_config(/*max_retries=*/2, /*base_delay_ms=*/50);

    std::atomic<bool> gave_up{false};
    std::string last_error;
    std::mutex error_mutex;
    client.set_error_callback([&](const std::string& reason) {
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error = reason;
        if (reason.find("max retries exhausted") != std::string::npos) {
            gave_up.store(true);
        }
    });

    client.connect();
    assert(client.is_connected());
    server.join();

    bool ok = wait_until([&] { return gave_up.load(); }, std::chrono::seconds(5));
    assert(ok);
    assert(client.reconnect_attempts() >= 2);

    client.disconnect();
    std::cout << "[test_pool_protocol] test_reconnect_backoff_exhaustion passed\n";
}

// ── Scenario 4: PoolManager multi-pool failover.
//
// This test originally caught the tick()/connect_to_current() self-deadlock
// (docs/postmortems/pool-failover-deadlock-postmortem.md) and, while investigating it,
// surfaced two further gaps — both since fixed (2026-07-22):
//
// 1. A pool whose very first connect() attempt fails outright (nothing
//    listening, refused) never used to trigger failover: reconnect_loop() is
//    only armed by reader_thread_fn() noticing a connection that WAS UP go
//    down, so reconnect_attempts() stayed 0 forever for a pool dead from
//    process startup. Fixed in PoolManager::tick() by tracking a separate
//    sync_retry_count_ for this case, using StratumClient::reconnect_loop_active()
//    (not reconnect_attempts()==0, which can't distinguish "never armed" from
//    "armed but hasn't incremented yet") to tell the two cases apart. Covered
//    by test_failover_from_pool_dead_at_startup() below.
// 2. connect_to_current() replacing the old StratumClient via
//    `stratum_ = std::make_unique<...>()` destroys the OLD object first, and
//    ~StratumClient() joins its reconnect_thread_ — which, at the moment
//    failover triggers, is very likely mid-sleep for a doomed retry. Used to
//    block up to kMaxBackoffMs (30s) since reconnect_enabled_.store(false)
//    doesn't wake a thread blocked in plain sleep_for(). Fixed by switching
//    reconnect_loop()'s sleep to a condition_variable::wait_for() that wakes
//    immediately when disconnect() notifies it. Covered by
//    test_disconnect_interrupts_reconnect_backoff() below.
//
// Real production backoff timing still applies here (1s,2s,4s,8s,16s delays
// before each of the 5 attempts, ~31s cumulative — PoolManager::connect_to_current()
// hardcodes set_reconnect_config(5, 1000) with no override), so this remains
// the slowest scenario in the file by design, not a bug — it's just no longer
// inflated by the ~30s stale-join delay on top.

void test_pool_failover() {
    std::uint16_t first_port = 0;
    int first_listen_fd = listen_on_ephemeral_port(first_port);

    std::uint16_t good_port = 0;
    int good_listen_fd = listen_on_ephemeral_port(good_port);

    std::thread first_pool_server([&] {
        // Accept once, complete a real handshake so the client is fully
        // connected (starting its reader thread), then drop — this is what
        // actually arms reconnect_loop(), unlike a refused-from-the-start pool.
        int c = accept_one(first_listen_fd);
        if (c < 0) return;
        std::string line;
        if (server_recv_line(c, line)) {
            const auto id = armrx::json::get_raw(line, "id");
            server_send_line(c, "{\"id\":" + id + ",\"jsonrpc\":\"2.0\",\"error\":null,"
                                 "\"result\":{\"id\":\"sess0\"}}");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ::close(c); // drop mid-session — arms the reconnect_loop
        ::close(first_listen_fd); // subsequent reconnect attempts fail fast (refused)
    });

    std::atomic<bool> good_pool_connected{false};
    std::thread good_pool_server([&] {
        // ~31s real backoff + handshake + on-device scheduling margin. No
        // longer needs to absorb the stale-thread-join delay on top (fixed —
        // see the comment above).
        int c = accept_one(good_listen_fd, /*timeout_ms=*/60000);
        if (c < 0) return; // test will fail via the wait_until below
        std::string line;
        bool got = server_recv_line(c, line);
        if (!got) { ::close(c); return; }
        const auto id = armrx::json::get_raw(line, "id");
        server_send_line(c, "{\"id\":" + id + ",\"jsonrpc\":\"2.0\",\"error\":null,"
                             "\"result\":{\"id\":\"sess1\"}}");
        good_pool_connected.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ::close(c);
    });

    std::vector<armrx::PoolConfig> pools = {
        {"127.0.0.1", first_port, false},
        {"127.0.0.1", good_port, false},
    };
    armrx::PoolManager mgr(pools, "test_wallet", "x");
    mgr.connect(); // succeeds against the first pool

    first_pool_server.join();

    // Drive tick() until failover completes or a generous timeout elapses
    // (real ~31s backoff + handshake + on-device scheduling margin).
    bool failed_over = wait_until([&] {
        mgr.tick();
        return good_pool_connected.load();
    }, std::chrono::seconds(60));

    assert(failed_over);
    assert(mgr.current_pool_name() == ("127.0.0.1:" + std::to_string(good_port)));

    mgr.disconnect();
    good_pool_server.join();
    ::close(good_listen_fd);
    std::cout << "[test_pool_protocol] test_pool_failover passed\n";
}

// ── Scenario 5: a pool that's unreachable from the very first connect()
//    attempt (connection refused at process startup — never completes even
//    one handshake, unlike test_pool_failover's connect-then-drop scenario)
//    must still eventually fail over to the next pool. Regression test for
//    gap #1 in the comment above test_pool_failover(). ──

void test_failover_from_pool_dead_at_startup() {
    // "Dead" pool: bind + listen, then close immediately so nothing is
    // listening on dead_port — connect() gets ECONNREFUSED synchronously,
    // exactly like a pool that's down from the moment the miner starts.
    std::uint16_t dead_port = 0;
    {
        int fd = listen_on_ephemeral_port(dead_port);
        ::close(fd);
    }

    std::uint16_t good_port = 0;
    int good_listen_fd = listen_on_ephemeral_port(good_port);

    std::atomic<bool> good_pool_connected{false};
    std::thread good_pool_server([&] {
        int c = accept_one(good_listen_fd, /*timeout_ms=*/10000);
        if (c < 0) return; // test will fail via the wait_until below
        std::string line;
        bool got = server_recv_line(c, line);
        if (!got) { ::close(c); return; }
        const auto id = armrx::json::get_raw(line, "id");
        server_send_line(c, "{\"id\":" + id + ",\"jsonrpc\":\"2.0\",\"error\":null,"
                             "\"result\":{\"id\":\"sess0\"}}");
        good_pool_connected.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ::close(c);
    });

    std::vector<armrx::PoolConfig> pools = {
        {"127.0.0.1", dead_port, false},
        {"127.0.0.1", good_port, false},
    };
    armrx::PoolManager mgr(pools, "test_wallet", "x");
    mgr.connect(); // fails synchronously against the dead pool
    assert(!mgr.is_connected());

    bool failed_over = wait_until([&] {
        mgr.tick();
        return good_pool_connected.load();
    }, std::chrono::seconds(10));

    assert(failed_over);
    assert(mgr.current_pool_name() == ("127.0.0.1:" + std::to_string(good_port)));

    mgr.disconnect();
    good_pool_server.join();
    ::close(good_listen_fd);
    std::cout << "[test_pool_protocol] test_failover_from_pool_dead_at_startup passed\n";
}

// ── Scenario 6: disconnect() wakes reconnect_loop() immediately instead of
//    blocking for the remainder of its current backoff sleep. Regression
//    test for gap #2 in the comment above test_pool_failover(). ──

void test_disconnect_interrupts_reconnect_backoff() {
    std::uint16_t port = 0;
    int listen_fd = listen_on_ephemeral_port(port);

    std::thread server([&] {
        int c = accept_one(listen_fd);
        if (c < 0) return;
        std::string line;
        if (server_recv_line(c, line)) {
            const auto id = armrx::json::get_raw(line, "id");
            server_send_line(c, "{\"id\":" + id + ",\"jsonrpc\":\"2.0\",\"error\":null,"
                                 "\"result\":{\"id\":\"sess0\"}}");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ::close(c); // drop mid-session — arms reconnect_loop()
        ::close(listen_fd); // subsequent reconnect attempts fail fast (refused)
    });

    armrx::StratumClient client("127.0.0.1", port, "test_wallet", "x");
    // Long base delay so the backoff sleep this test interrupts is clearly
    // longer than the time budget asserted below.
    client.set_reconnect_config(/*max_retries=*/10, /*base_delay_ms=*/5000);
    client.connect();
    server.join();

    // Wait for reader_thread_fn() to notice the drop and arm reconnect_loop(),
    // then let it get well into its 5s backoff sleep.
    bool armed = wait_until([&] { return client.reconnect_loop_active(); },
                            std::chrono::seconds(2));
    assert(armed);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto start = std::chrono::steady_clock::now();
    client.disconnect();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Without the fix, disconnect()'s join() would block for most of the
    // remaining ~4.7s of the backoff sleep. With it, the condition_variable
    // notify wakes reconnect_loop() immediately.
    assert(elapsed < std::chrono::seconds(2));
    std::cout << "[test_pool_protocol] test_disconnect_interrupts_reconnect_backoff passed\n";
}

// ── Scenario 7: malformed/partial JSON from the server doesn't crash the
//    client or wedge the reader thread — a subsequent valid message must
//    still be processed correctly afterward. ──

void test_malformed_input_robustness() {
    std::uint16_t port = 0;
    int listen_fd = listen_on_ephemeral_port(port);

    std::thread server([&] {
        int c = accept_one(listen_fd);
        assert(c >= 0);
        std::string line;
        bool got = server_recv_line(c, line);
        assert(got);
        const auto id = armrx::json::get_raw(line, "id");
        server_send_line(c, "{\"id\":" + id + ",\"jsonrpc\":\"2.0\",\"error\":null,"
                             "\"result\":{\"id\":\"sess1\"}}");

        // A grab-bag of hostile/malformed lines: unterminated string, stray
        // braces, empty line, binary-ish junk with embedded quotes, huge
        // nesting, truncated escape sequence.
        server_send_line(c, "{\"method\":\"mining.notify\",\"params\":[\"unterminated");
        server_send_line(c, "}}}}}}}}}}");
        server_send_line(c, "");
        server_send_line(c, "{\"method\":\"job\",\"params\":{\"blob\":\"\\");
        server_send_line(c, std::string(2000, '{'));
        server_send_line(c, "not json at all, just bytes \x01\x02\x03");

        // Now a real, valid job notification — must still be processed.
        server_send_line(c,
            "{\"method\":\"job\",\"params\":{\"job_id\":\"recover1\",\"blob\":\"" +
            kBlobHex + "\",\"target\":\"" + std::string(kTargetHex) + "\","
            "\"seed_hash\":\"" + kSeedHex + "\"}}");

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ::close(c);
    });

    armrx::StratumClient client("127.0.0.1", port, "test_wallet", "x");
    std::mutex job_mutex;
    armrx::Job received_job;
    std::atomic<bool> job_received{false};
    client.set_job_callback([&](const armrx::Job& j) {
        std::lock_guard<std::mutex> lock(job_mutex);
        received_job = j;
        job_received.store(true);
    });

    client.connect();
    assert(client.is_connected());

    bool ok = wait_until([&] { return job_received.load(); }, std::chrono::seconds(5));
    assert(ok); // proves the reader thread survived the malformed-input barrage
    {
        std::lock_guard<std::mutex> lock(job_mutex);
        assert(received_job.job_id == "recover1");
    }
    assert(client.is_connected()); // still healthy, not wedged

    client.disconnect();
    server.join();
    ::close(listen_fd);
    std::cout << "[test_pool_protocol] test_malformed_input_robustness passed\n";
}

} // namespace

int main() {
    armrx::log::set_level(armrx::log::Level::warn); // quiet the expected handshake-reject/reconnect noise

    test_stratum_v1_full_flow_via_auto_fallback();
    test_cryptonote_full_flow();
    test_reconnect_backoff_exhaustion();
    test_malformed_input_robustness();
    test_disconnect_interrupts_reconnect_backoff();
    test_failover_from_pool_dead_at_startup();
    test_pool_failover(); // slowest scenario (~30s real backoff) — run last

    std::cout << "ALL POOL PROTOCOL TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
