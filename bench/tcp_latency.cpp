// Open-loop latency through the standalone server (design M4, T1 and T4 over TCP). Each connection runs on its own pinned thread against
// a running seq_gateway: requests are due on a fixed schedule whatever the server is doing, and every latency is measured from the due
// time to the moment the reply was read, so a stalled server shows up as delay (no coordinated omission). One clock (the TSC of the
// connection's core), so the total is exact; the split into queue, engine and ack needs stamps inside the server and is not done here.
// The reply time is taken once per read, before the replies in it are parsed, so a read that returns several replies gives them all
// the same time. Latencies include the kernel's loopback TCP path and the gateway thread, not only the sequencer.
// usage: tcp_latency [port] [rate per connection per second] [requests per connection] [connections 1 to 8] [first core] [core step, default 1]
#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <thread>
#include <vector>
#include "meridian/hdr_histogram.hpp"
#include "meridian/net.hpp"
#include "meridian/protocol2.hpp"
#include "meridian/timing.hpp"

using namespace meridian;

constexpr uint64_t kMaxOutstanding = 512;   // the server's rings hold 1024; stay well under

static void pin(int cpu) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); pthread_setaffinity_np(pthread_self(), sizeof s, &s); }

struct Conn {
    Histogram<> h;
    uint64_t by_code[16] = {}, got = 0, dups = 0, rejects = 0, flow_waits = 0, last_ack = 0;
    bool ok = true;
};

static void client(uint16_t port, size_t p, int core, uint64_t ops, double period, double ghz, uint64_t id_base, const std::atomic<uint64_t>& go, Conn& out) {
    pin(core);
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { out.ok = false; return; }
    set_nodelay(fd);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    std::mt19937_64 g(100 + p);
    std::vector<uint64_t> due(ops, 0), open;
    uint8_t rb[4096];
    size_t rn = 0;
    uint8_t pend[64];
    size_t plen = 0, poff = 0;
    uint64_t sent = 0;
    const uint32_t account = static_cast<uint32_t>(p + 1);

    auto flush = [&] {
        while (poff < plen) {
            const ssize_t n = ::send(fd, pend + poff, plen - poff, MSG_NOSIGNAL);
            if (n > 0) poff += static_cast<size_t>(n);
            else { if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) out.ok = false; break; }
        }
        if (poff == plen) plen = poff = 0;
    };
    auto drain = [&] {
        const ssize_t n = ::recv(fd, rb + rn, sizeof rb - rn, 0);
        const uint64_t t4 = rdtsc_end();
        if (n > 0) rn += static_cast<size_t>(n);
        else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { out.ok = false; return; }
        size_t off = 0;
        while (rn - off >= sizeof(proto2::Reply)) {
            const auto* w = reinterpret_cast<const proto2::Reply*>(rb + off);
            if (w->h.type != static_cast<uint8_t>(proto2::MsgType::Reply)) { out.ok = false; return; }
            const seq::Reply r = proto2::from_wire(*w);
            off += sizeof(proto2::Reply);
            if (r.tag == 0 || r.tag > ops) { out.ok = false; return; }
            const int64_t t = static_cast<int64_t>(t4 - due[r.tag - 1]);
            out.h.record(t < 0 ? 0 : static_cast<uint64_t>(static_cast<double>(t) / ghz));
            if (r.duplicate) ++out.dups;
            if (r.reject != oms::Reject::None) { ++out.rejects; ++out.by_code[static_cast<uint8_t>(r.reject) & 15]; }
            if (r.status == oms::Status::Open && r.order_id) open.push_back(r.order_id);
            out.last_ack = t4;
            ++out.got;
        }
        if (off) { std::memmove(rb, rb + off, rn - off); rn -= off; }
    };
    auto pump = [&] { flush(); drain(); };

    uint64_t start;
    while ((start = go.load(std::memory_order_acquire)) == 0) {}
    for (uint64_t i = 0; i < ops && out.ok; ++i) {
        const uint64_t d = start + static_cast<uint64_t>(static_cast<double>(i) * period);
        while (rdtsc_begin() < d && out.ok) pump();
        while ((plen != 0 || sent - out.got >= kMaxOutstanding) && out.ok) { ++out.flow_waits; pump(); }
        due[i] = d;
        if (i % 4 == 3 && !open.empty()) {
            proto2::Cancel c{};
            proto2::fill_header(c.h, proto2::MsgType::Cancel, sizeof c, i + 1);
            c.account.set(account); c.order_id.set(open.back()); open.pop_back();
            std::memcpy(pend, &c, sizeof c); plen = sizeof c;
        } else {
            proto2::Submit s{};
            proto2::fill_header(s.h, proto2::MsgType::Submit, sizeof s, i + 1);
            s.account.set(account); s.cl_ord_id.set(id_base + i + 1); s.side = static_cast<uint8_t>(g() & 1); s.order_type = 0; s.tif = 0;
            s.price.set(static_cast<uint32_t>(480 + g() % 40)); s.qty.set(static_cast<uint32_t>(1 + g() % 50)); s.stop_price.set(0);
            std::memcpy(pend, &s, sizeof s); plen = sizeof s;
        }
        poff = 0;
        flush();
        ++sent;
    }
    const uint64_t deadline = rdtsc_begin() + static_cast<uint64_t>(ghz * 20e9);   // give the server 20 s to answer what is left
    while (out.ok && out.got < sent && rdtsc_begin() < deadline) pump();
    ::close(fd);
}

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9100;
    const double rate = argc > 2 ? std::atof(argv[2]) : 50000;
    const uint64_t ops = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 100000;
    const size_t nc = std::min<size_t>(argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 1, 8);
    const int first_core = argc > 5 ? std::atoi(argv[5]) : 2;
    const int step = argc > 6 ? std::atoi(argv[6]) : 1;
    const double ghz = calibrate_tsc_ghz(200);
    const double period = ghz * 1e9 / rate;
    const uint64_t id_base = (static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) & 0xffffffffull) << 24;

    std::atomic<uint64_t> go{0};
    std::vector<Conn> cs(nc);
    std::vector<std::thread> ts;
    for (size_t p = 0; p < nc; ++p) ts.emplace_back(client, port, p, first_core + step * static_cast<int>(p), ops, period, ghz, id_base, std::cref(go), std::ref(cs[p]));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t t0 = rdtsc_begin() + static_cast<uint64_t>(ghz * 1e6);
    go.store(t0, std::memory_order_release);
    for (auto& t : ts) t.join();

    Histogram<> all;
    uint64_t got = 0, dups = 0, rej = 0, flow = 0, last = 0, by_code[16] = {};
    bool ok = true;
    for (auto& c : cs) { all.add(c.h); got += c.got; dups += c.dups; rej += c.rejects; flow += c.flow_waits; for (int k = 0; k < 16; ++k) by_code[k] += c.by_code[k]; last = std::max(last, c.last_ack); ok = ok && c.ok; }
    const double secs = last > t0 ? static_cast<double>(last - t0) / ghz / 1e9 : 0;
    std::printf("connections %zu, offered %.0f req/s each (%.0f total), requests %llu each\n", nc, rate, rate * static_cast<double>(nc), (unsigned long long)ops);
    std::printf("replies %llu of %llu%s, duplicates %llu, rejects %llu, flow-control waits %llu\n", (unsigned long long)got,
                (unsigned long long)(ops * nc), ok ? "" : "  (a connection failed)", (unsigned long long)dups, (unsigned long long)rej, (unsigned long long)flow);
    for (int k = 0; k < 16; ++k) if (by_code[k]) std::printf("  reject code %d: %llu\n", k, (unsigned long long)by_code[k]);
    std::printf("achieved %.0f req/s over %.2f s\n", secs > 0 ? static_cast<double>(got) / secs : 0.0, secs);
    std::printf("  %-22s p50 %8llu  p99 %8llu  p99.9 %8llu  max %9llu  (ns)\n", "total (due to reply)", (unsigned long long)all.percentile(50),
                (unsigned long long)all.percentile(99), (unsigned long long)all.percentile(99.9), (unsigned long long)all.max());
    return ok && got == ops * nc ? 0 : 1;
}
