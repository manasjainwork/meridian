#pragma once
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>
#include "meridian/journal.hpp"
#include "meridian/order_book.hpp"
#include "meridian/replay.hpp"

namespace meridian::repl {

using journal::Record;

enum class Status { Ok, BadCrc, SeqGap, BadField, Closed, IoError };

struct FollowerResult {
    Status status;
    uint64_t valid;     // records received, validated and acknowledged (a contiguous prefix 1..valid)
    uint64_t digest;    // digest of the replica book after applying those records (+ final state)
};

// Replica: accepts one connection, validates every record (CRC, field ranges, seq == previous + 1), appends it
// to its log, acknowledges the highest valid seq once per received batch, and only then applies the batch to
// its own OrderBook. It never acknowledges past the first invalid record.
class Follower {
public:
    Follower(uint64_t expected, uint32_t pool, uint32_t max_price) : expected_(expected), pool_(pool), max_price_(max_price) {}
    ~Follower() { if (th_.joinable()) th_.join(); }

    // Binds 127.0.0.1 on an ephemeral port and returns it (0 on failure).
    uint16_t listen_loopback() {
        lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (lfd_ < 0) return 0;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        socklen_t len = sizeof a;
        if (::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd_, 1) != 0 ||
            ::getsockname(lfd_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
        return ntohs(a.sin_port);
    }
    void start(int cpu = -1) { th_ = std::thread([this, cpu] { run(cpu); }); }
    FollowerResult join() { if (th_.joinable()) th_.join(); return res_; }

private:
    void run(int cpu) {
        if (cpu >= 0) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); pthread_setaffinity_np(pthread_self(), sizeof s, &s); }
        const int c = ::accept(lfd_, nullptr, nullptr);
        ::close(lfd_);
        res_ = {Status::IoError, 0, 0};
        if (c < 0) return;
        const int one = 1;
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        book::OrderBook bk(pool_, max_price_);
        journal::Digest d;
        std::vector<book::Fill> fills;
        fills.reserve(64);
        std::vector<Record> log;
        log.reserve(expected_);
        std::vector<unsigned char> buf(1u << 17);
        size_t have = 0, applied = 0;
        uint64_t valid = 0, acked = 0;
        Status st = Status::Ok;

        while (valid < expected_ && st == Status::Ok) {
            const ssize_t n = ::recv(c, buf.data() + have, buf.size() - have, 0);
            if (n == 0) { st = Status::Closed; break; }
            if (n < 0) { if (errno == EINTR) continue; st = Status::IoError; break; }
            have += static_cast<size_t>(n);
            size_t off = 0;
            while (have - off >= sizeof(Record) && valid < expected_) {
                Record r;
                std::memcpy(&r, buf.data() + off, sizeof r);
                if (r.crc != journal::record_crc(r)) { st = Status::BadCrc; break; }
                if ((r.type != journal::Type::Add && r.type != journal::Type::Cancel) ||
                    (r.side != book::Side::Buy && r.side != book::Side::Sell)) { st = Status::BadField; break; }
                if (r.seq != valid + 1) { st = Status::SeqGap; break; }
                log.push_back(r);
                ++valid;
                off += sizeof r;
            }
            if (off) { std::memmove(buf.data(), buf.data() + off, have - off); have -= off; }
            if (valid > acked) {                            // ack first, apply after: the ack never waits for the book
                acked = valid;
                if (!send_all(c, &acked, sizeof acked)) { st = Status::IoError; break; }
            }
            while (applied < valid) journal::apply(bk, log[applied++], fills, d);
        }
        journal::mix_state(bk, max_price_, d);
        res_ = {st, valid, d.value()};
        ::shutdown(c, SHUT_WR);                              // tell the leader we are done, then drain so a close
        unsigned char sink[4096];                            // with unread data cannot reset the connection and
        while (::recv(c, sink, sizeof sink, 0) > 0) {}       // discard acks still in flight
        ::close(c);
    }

    static bool send_all(int fd, const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        while (n) {
            const ssize_t k = ::send(fd, b, n, MSG_NOSIGNAL);
            if (k < 0) { if (errno == EINTR) continue; return false; }
            b += k; n -= static_cast<size_t>(k);
        }
        return true;
    }

    uint64_t expected_;
    uint32_t pool_, max_price_;
    int lfd_ = -1;
    std::thread th_;
    FollowerResult res_{Status::IoError, 0, 0};
};

// Leader side of the connection: send records, read cumulative acks.
class LeaderLink {
public:
    ~LeaderLink() { if (fd_ >= 0) ::close(fd_); }
    bool connect_loopback(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        const int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    }
    bool send_all(const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        while (n) {
            const ssize_t k = ::send(fd_, b, n, MSG_NOSIGNAL);
            if (k < 0) { if (errno == EINTR) continue; return false; }
            b += k; n -= static_cast<size_t>(k);
        }
        return true;
    }
    uint64_t poll_acks() {                                   // non-blocking
        const ssize_t n = ::recv(fd_, abuf_ + ahave_, sizeof abuf_ - ahave_, MSG_DONTWAIT);
        if (n > 0) absorb(static_cast<size_t>(n));
        return acked_;
    }
    uint64_t wait_acks_until_close() {                       // blocking, returns the highest ack seen
        for (;;) {
            const ssize_t n = ::recv(fd_, abuf_ + ahave_, sizeof abuf_ - ahave_, 0);
            if (n > 0) absorb(static_cast<size_t>(n));
            else if (n == 0 || errno != EINTR) break;
        }
        return acked_;
    }
    void shutdown_write() { ::shutdown(fd_, SHUT_WR); }
    uint64_t acked() const noexcept { return acked_; }

private:
    void absorb(size_t n) {
        ahave_ += n;
        size_t off = 0;
        while (ahave_ - off >= sizeof(uint64_t)) {
            uint64_t v;
            std::memcpy(&v, abuf_ + off, sizeof v);
            if (v > acked_) acked_ = v;
            off += sizeof v;
        }
        if (off) { std::memmove(abuf_, abuf_ + off, ahave_ - off); ahave_ -= off; }
    }
    int fd_ = -1;
    uint64_t acked_ = 0;
    unsigned char abuf_[64];
    size_t ahave_ = 0;
};

}  // namespace meridian::repl
