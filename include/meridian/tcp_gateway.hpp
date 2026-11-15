#pragma once
// Binary TCP gateway (sequencer design M3, decisions D2, D7, D9): one thread per connection, one SPSC ring pair per connection.
// Each connection owns a producer slot of the Sequencer. The connection thread reads protocol-v2 requests, submits them to the
// sequencer's ring, and writes the replies it pops from the reply ring. Loopback only. The sequencer must be running (run() on its
// own thread) while the gateway is. A connection with a malformed stream is closed; a connection that arrives when every slot is
// taken is closed at once. Blocking waits are poll() with a timeout while idle and a yield loop while a reply is still owed, so
// an idle connection costs nothing and a busy one adds no sleep to the reply; the cost of that spinning is not measured yet.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
#include "meridian/net.hpp"
#include "meridian/protocol2.hpp"
#include "meridian/sequencer.hpp"

namespace meridian::gw {

struct Stats {
    uint64_t accepted = 0, refused = 0, requests = 0, replies = 0, heartbeats = 0, bad_messages = 0, closed = 0, poisoned_slots = 0;
};

template <size_t P, size_t N>
class TcpGateway {
public:
    explicit TcpGateway(seq::Sequencer<P, N>& sq) : sq_(sq) {}
    TcpGateway(const TcpGateway&) = delete;
    TcpGateway& operator=(const TcpGateway&) = delete;
    ~TcpGateway() { stop(); }

    // port 0 picks a free port. Returns the port, or 0 if it could not listen.
    uint16_t start(uint16_t port = 0) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return 0;
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
        socklen_t len = sizeof a;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || ::listen(listen_fd_, 64) < 0 ||
            ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len) < 0) {
            ::close(listen_fd_); listen_fd_ = -1;
            return 0;
        }
        port_ = ntohs(a.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
        return port_;
    }

    void stop() {
        if (stop_.exchange(true)) return;
        if (acceptor_.joinable()) acceptor_.join();
        for (auto& t : conn_) if (t.joinable()) t.join();
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    }

    Stats stats() const noexcept {
        Stats s;
        s.accepted = accepted_.load(); s.refused = refused_.load(); s.requests = requests_.load(); s.replies = replies_.load();
        s.heartbeats = heartbeats_.load(); s.bad_messages = bad_.load(); s.closed = closed_.load(); s.poisoned_slots = poisoned_.load();
        return s;
    }

private:
    void accept_loop() {
        while (!stop_.load()) {
            pollfd pf{listen_fd_, POLLIN, 0};
            if (::poll(&pf, 1, 20) <= 0) continue;
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) continue;
            size_t p = P;
            for (size_t i = 0; i < P; ++i) if (!busy_[i].exchange(true)) { p = i; break; }
            if (p == P) { ::close(fd); ++refused_; continue; }
            if (conn_[p].joinable()) conn_[p].join();   // the previous connection on this slot has finished
            ++accepted_;
            conn_[p] = std::thread([this, fd, p] { serve(fd, p); });
        }
    }

    void serve(int fd, size_t p) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        set_nodelay(fd);
        constexpr size_t kBuf = 4096;
        std::array<uint8_t, kBuf> in;
        size_t rd = 0, wr = 0;
        std::vector<uint8_t> out;
        size_t out_off = 0;
        uint64_t outstanding = 0;
        seq::Msg pending;
        bool has_pending = false, peer_closed = false, bad = false;
        auto room = [&] { return outstanding + 1 < N; };

        while (!stop_.load(std::memory_order_relaxed) && !bad) {
            bool progress = false;
            seq::Reply r;
            while (sq_.pop_reply(p, r)) {
                const proto2::Reply w = proto2::to_wire(r);
                const auto* b = reinterpret_cast<const uint8_t*>(&w);
                out.insert(out.end(), b, b + sizeof w);
                --outstanding; ++replies_; progress = true;
            }
            if (out_off < out.size()) {
                const ssize_t n = ::send(fd, out.data() + out_off, out.size() - out_off, MSG_NOSIGNAL);
                if (n > 0) { out_off += static_cast<size_t>(n); progress = true; }
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
                if (out_off == out.size()) { out.clear(); out_off = 0; }
            }
            if (has_pending && room() && sq_.submit(p, pending)) { has_pending = false; ++outstanding; progress = true; }
            if (!has_pending && !peer_closed && room() && out.size() < (1u << 16) && wr < kBuf) {
                const ssize_t n = ::recv(fd, in.data() + wr, kBuf - wr, 0);
                if (n > 0) { wr += static_cast<size_t>(n); progress = true; }
                else if (n == 0) peer_closed = true;
                else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
            }
            while (!has_pending && room() && rd < wr) {
                const proto2::ParseResult pr = proto2::parse_one(in.data() + rd, wr - rd);
                if (pr.status == proto2::Status::NeedMore) break;
                if (pr.status != proto2::Status::Ok) { ++bad_; bad = true; break; }
                seq::Msg m;
                switch (static_cast<proto2::MsgType>(pr.header->type)) {
                    case proto2::MsgType::Heartbeat: {
                        proto2::Heartbeat hb{};
                        proto2::fill_header(hb.h, proto2::MsgType::Heartbeat, sizeof hb, pr.header->seq.get());
                        const auto* b = reinterpret_cast<const uint8_t*>(&hb);
                        out.insert(out.end(), b, b + sizeof hb);
                        ++heartbeats_;
                        rd += pr.consumed; progress = true;
                        continue;
                    }
                    case proto2::MsgType::Submit: m = proto2::to_msg(*proto2::as<proto2::Submit>(pr.header)); break;
                    case proto2::MsgType::Cancel: m = proto2::to_msg(*proto2::as<proto2::Cancel>(pr.header)); break;
                    case proto2::MsgType::Modify: m = proto2::to_msg(*proto2::as<proto2::Modify>(pr.header)); break;
                    case proto2::MsgType::Reply: ++bad_; bad = true; break;   // a client never sends a reply
                }
                if (bad) break;
                rd += pr.consumed; progress = true; ++requests_;
                if (sq_.submit(p, m)) ++outstanding; else { pending = m; has_pending = true; }
            }
            if (rd == wr) rd = wr = 0;
            else if (rd > 0 && wr == kBuf) { std::memmove(in.data(), in.data() + rd, wr - rd); wr -= rd; rd = 0; }
            if (peer_closed && outstanding == 0 && !has_pending && out_off == out.size() && rd == wr) break;
            if (!progress) {
                if (outstanding > 0 || has_pending || out_off < out.size()) std::this_thread::yield();
                else { pollfd pf{fd, POLLIN, 0}; ::poll(&pf, 1, 20); }
            }
        }
        // Replies still owed belong to this slot's ring; take them before the slot is reused, or retire the slot.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        seq::Reply r;
        while (outstanding > 0 && std::chrono::steady_clock::now() < deadline) {
            if (sq_.pop_reply(p, r)) --outstanding; else std::this_thread::yield();
        }
        ::close(fd);
        ++closed_;
        if (outstanding == 0 && !has_pending) busy_[p].store(false); else ++poisoned_;   // a retired slot stays busy
    }

    seq::Sequencer<P, N>& sq_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::thread acceptor_;
    std::array<std::thread, P> conn_;
    std::array<std::atomic<bool>, P> busy_{};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> accepted_{0}, refused_{0}, requests_{0}, replies_{0}, heartbeats_{0}, bad_{0}, closed_{0}, poisoned_{0};
};

}  // namespace meridian::gw
