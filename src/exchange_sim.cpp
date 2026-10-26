#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "meridian/net.hpp"
#include "meridian/protocol.hpp"

using namespace meridian;
using namespace meridian::proto;

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9000;
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { std::perror("socket"); return 1; }
    int one = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) { std::perror("bind"); return 1; }
    if (::listen(ls, 1) < 0) { std::perror("listen"); return 1; }
    std::printf("exchange_sim listening on 127.0.0.1:%u\n", port);
    std::fflush(stdout);

    const int fd = ::accept(ls, nullptr, nullptr);
    if (fd < 0) { std::perror("accept"); return 1; }
    set_nodelay(fd);

    uint8_t buf[65536];
    size_t have = 0;
    uint64_t out_seq = 0, in_seq = 0, gaps = 0, orders = 0, heartbeats = 0;
    uint64_t next_xid = 1300000000000000ULL;  // 16-digit exchange order ids, as uint64
    bool bad = false;

    while (!bad) {
        const ssize_t n = ::recv(fd, buf + have, sizeof buf - have, 0);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; std::perror("recv"); break; }
        have += static_cast<size_t>(n);

        size_t off = 0;
        for (;;) {
            const auto r = parse_one(buf + off, have - off);
            if (r.status == Status::NeedMore) break;
            if (r.status != Status::Ok) { std::fprintf(stderr, "protocol error, closing\n"); bad = true; break; }

            const uint64_t seq = r.header->seq.get();
            if (seq != in_seq + 1) ++gaps;
            in_seq = seq;

            if (r.header->type == static_cast<uint8_t>(MsgType::NewOrder)) {
                const auto* o = as<NewOrder>(r.header);
                OrderAck ack{};
                fill_header(ack.h, MsgType::OrderAck, sizeof ack, ++out_seq);
                ack.client_order_id = o->client_order_id;
                ack.exchange_order_id.set(next_xid++);
                ack.status = 0;
                if (!send_all(fd, &ack, sizeof ack)) { bad = true; break; }
                ++orders;
            } else if (r.header->type == static_cast<uint8_t>(MsgType::Heartbeat)) {
                Heartbeat hb{};
                fill_header(hb.h, MsgType::Heartbeat, sizeof hb, ++out_seq);
                if (!send_all(fd, &hb, sizeof hb)) { bad = true; break; }
                ++heartbeats;
            }
            off += r.consumed;
        }
        if (off < have) std::memmove(buf, buf + off, have - off);
        have -= off;
    }
    std::printf("exchange_sim done: orders=%llu heartbeats=%llu seq_gaps=%llu\n",
                static_cast<unsigned long long>(orders), static_cast<unsigned long long>(heartbeats),
                static_cast<unsigned long long>(gaps));
    ::close(fd);
    ::close(ls);
    return bad ? 1 : 0;
}
