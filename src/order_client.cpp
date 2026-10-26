#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "meridian/net.hpp"
#include "meridian/protocol.hpp"
#include "meridian/timing.hpp"

using namespace meridian;
using namespace meridian::proto;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static uint64_t rng = 88172645463325252ULL;
static uint64_t next_rand() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static bool read_ack(int fd, OrderAck& out) {
    uint8_t b[sizeof(OrderAck)];
    if (!recv_all(fd, b, sizeof b)) return false;
    const auto r = parse_one(b, sizeof b);
    if (r.status != Status::Ok || r.header->type != static_cast<uint8_t>(MsgType::OrderAck)) return false;
    std::memcpy(&out, b, sizeof b);
    return true;
}

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9000;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) { std::perror("connect"); return 1; }
    set_nodelay(fd);

    uint64_t seq = 0;

    // Phase 0: heartbeat echo
    Heartbeat hb{};
    fill_header(hb.h, MsgType::Heartbeat, sizeof hb, ++seq);
    uint8_t hbuf[sizeof(Heartbeat)];
    CHECK(send_all(fd, &hb, sizeof hb));
    CHECK(recv_all(fd, hbuf, sizeof hbuf));
    CHECK(parse_one(hbuf, sizeof hbuf).status == Status::Ok);
    CHECK(hbuf[2] == static_cast<uint8_t>(MsgType::Heartbeat));

    // Phase 1: ping-pong round trip over loopback
    constexpr size_t N = 50000;
    const double ghz = calibrate_tsc_ghz();
    std::vector<uint64_t> d(N);
    for (size_t i = 0; i < N; ++i) {
        const NewOrder m = make_new_order(++seq, i, 7, Side::Buy, 10000 + i % 100, 100);
        OrderAck ack{};
        const uint64_t a = rdtsc_begin();
        const bool ok = send_all(fd, &m, sizeof m) && read_ack(fd, ack);
        const uint64_t b = rdtsc_end();
        if (!ok) { std::printf("FAIL: lost connection at order %zu\n", i); return 1; }
        if (ack.client_order_id.get() != i) { CHECK(ack.client_order_id.get() == i); break; }
        d[i] = b - a;
    }
    std::sort(d.begin(), d.end());
    auto pct = [&](double p) { return d[static_cast<size_t>(p * (N - 1))]; };
    std::printf("loopback ping-pong RTT, %zu orders, TSC %.3f GHz\n", N, ghz);
    const double ps[] = {0.0, 0.5, 0.99, 0.999, 1.0};
    const char* names[] = {"min", "p50", "p99", "p99.9", "max"};
    for (int i = 0; i < 5; ++i) {
        const uint64_t c = pct(ps[i]);
        std::printf("%-6s %9.1f us\n", names[i], c / ghz / 1000.0);
    }

    // Phase 2: one big byte stream sent in random 1..40 byte chunks
    constexpr size_t M = 1000;
    std::vector<uint8_t> stream(M * sizeof(NewOrder));
    for (size_t j = 0; j < M; ++j) {
        const NewOrder m = make_new_order(++seq, 1000000 + j, 7, Side::Sell, 20000 + j % 50, 10);
        std::memcpy(stream.data() + j * sizeof(NewOrder), &m, sizeof m);
    }
    for (size_t off = 0; off < stream.size();) {
        const size_t chunk = std::min<size_t>(1 + next_rand() % 40, stream.size() - off);
        CHECK(send_all(fd, stream.data() + off, chunk));
        off += chunk;
    }
    uint64_t prev_xid = 0;
    for (size_t j = 0; j < M; ++j) {
        OrderAck ack{};
        if (!read_ack(fd, ack)) { std::printf("FAIL: missing ack %zu\n", j); ++g_fail; break; }
        CHECK(ack.client_order_id.get() == 1000000 + j);
        const uint64_t x = ack.exchange_order_id.get();
        if (j > 0) CHECK(x == prev_xid + 1);
        prev_xid = x;
    }
    std::printf("fragmented stream: %zu orders sent in 1..40 byte chunks, last exchange id %llu\n",
                M, static_cast<unsigned long long>(prev_xid));

    ::close(fd);
    std::printf(g_fail ? "%d FAILED\n" : "all tcp tests passed\n", g_fail);
    return g_fail != 0;
}
