// Protocol v2 and the binary TCP gateway (design M3): parser (split reads, bad length, bad type, bad version, bad field, fuzzed
// bytes), and real loopback connections into a running sequencer: every request gets exactly one reply in order, the replies
// received over the wire equal the replay of the event log, and a malformed or surplus connection is closed without harming others.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
#include "meridian/event_log.hpp"
#include "meridian/net.hpp"
#include "meridian/protocol2.hpp"
#include "meridian/tcp_gateway.hpp"

using namespace meridian;
using namespace meridian::proto2;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

#ifndef GW_OPS
#define GW_OPS 3000
#endif
static oms::Config cfg() { oms::Config c; c.max_orders = 1u << 18; return c; }

static Submit make_submit(uint64_t n, uint32_t acc, uint64_t cl, uint8_t side, uint8_t type, uint8_t tif, uint32_t price, uint32_t qty, uint32_t stop = 0) {
    Submit m{};
    fill_header(m.h, MsgType::Submit, sizeof m, n);
    m.account.set(acc); m.cl_ord_id.set(cl); m.side = side; m.order_type = type; m.tif = tif; m.price.set(price); m.qty.set(qty); m.stop_price.set(stop);
    return m;
}
static Cancel make_cancel(uint64_t n, uint32_t acc, uint64_t id) { Cancel m{}; fill_header(m.h, MsgType::Cancel, sizeof m, n); m.account.set(acc); m.order_id.set(id); return m; }
static Modify make_modify(uint64_t n, uint32_t acc, uint64_t id, uint32_t px, uint32_t q) { Modify m{}; fill_header(m.h, MsgType::Modify, sizeof m, n); m.account.set(acc); m.order_id.set(id); m.price.set(px); m.qty.set(q); return m; }

template <class T> static std::vector<uint8_t> bytes(const T& m) { const auto* p = reinterpret_cast<const uint8_t*>(&m); return std::vector<uint8_t>(p, p + sizeof m); }

static void parser() {
    const std::vector<std::vector<uint8_t>> all = {
        bytes(Heartbeat{[] { Heartbeat h{}; fill_header(h.h, MsgType::Heartbeat, sizeof h, 9); return h.h; }()}),
        bytes(make_submit(1, 5, 77, 1, 3, 2, 500, 10, 490)), bytes(make_cancel(2, 5, 123456789012ULL)), bytes(make_modify(3, 5, 8, 501, 4)),
        bytes(to_wire(seq::Reply{12, 3, 4, oms::Reject::None, oms::Status::Open, 1, 2, 3, true})),
    };
    for (const auto& m : all) {
        bool ok = true;
        for (size_t l = 0; l < m.size(); ++l) ok = ok && parse_one(m.data(), l).status == Status::NeedMore;   // every prefix waits for more
        const ParseResult r = parse_one(m.data(), m.size());
        ok = ok && r.status == Status::Ok && r.consumed == m.size() && r.header == reinterpret_cast<const Header*>(m.data());
        std::vector<uint8_t> two = m; two.insert(two.end(), m.begin(), m.end());                              // back to back
        const ParseResult a = parse_one(two.data(), two.size());
        const ParseResult b = parse_one(two.data() + a.consumed, two.size() - a.consumed);
        ok = ok && a.status == Status::Ok && b.status == Status::Ok && a.consumed + b.consumed == two.size();
        CHECK(ok);
    }
    const auto sub = bytes(make_submit(0x0102030405060708ULL, 7, 99, 1, 2, 1, 1234, 56, 78));
    CHECK(sub[0] == 0x00 && sub[1] == 39 && sub[3] == kVersion && sub[4] == 0x01 && sub[11] == 0x08);          // big-endian, version 2
    const Submit* s = proto2::as<Submit>(parse_one(sub.data(), sub.size()).header);
    CHECK(s->account.get() == 7 && s->cl_ord_id.get() == 99 && s->side == 1 && s->order_type == 2 && s->tif == 1 && s->price.get() == 1234 && s->qty.get() == 56 && s->stop_price.get() == 78);
    const seq::Msg m = to_msg(*s);
    CHECK(m.op == seq::Op::Submit && m.tag == 0x0102030405060708ULL && m.req.account == 7 && m.req.cl_ord_id == 99 && m.req.side == book::Side::Sell &&
          m.req.type == oms::Type::Stop && m.req.tif == book::TimeInForce::IOC && m.req.price == 1234 && m.req.qty == 56 && m.req.stop_price == 78);

    {   // the other two requests map field for field
        const auto cb = bytes(make_cancel(21, 6, 4242));
        const seq::Msg c = to_msg(*proto2::as<Cancel>(parse_one(cb.data(), cb.size()).header));
        CHECK(c.op == seq::Op::Cancel && c.tag == 21 && c.account == 6 && c.order_id == 4242);
        const auto mb = bytes(make_modify(22, 6, 4243, 777, 8));
        const seq::Msg d = to_msg(*proto2::as<Modify>(parse_one(mb.data(), mb.size()).header));
        CHECK(d.op == seq::Op::Modify && d.tag == 22 && d.account == 6 && d.order_id == 4243 && d.price == 777 && d.qty == 8);
    }

    auto status_of = [](std::vector<uint8_t> b, size_t at, uint8_t v) { b[at] = v; return parse_one(b.data(), b.size()).status; };
    CHECK(status_of(sub, 2, 99) == Status::BadType);       // unknown type
    CHECK(status_of(sub, 2, 2) == Status::BadType);        // a version 1 type number is not a version 2 type
    CHECK(status_of(sub, 3, 1) == Status::BadVersion);
    CHECK(status_of(sub, 3, 3) == Status::BadVersion);
    CHECK(status_of(sub, 1, 40) == Status::BadLength);
    CHECK(status_of(sub, 0, 1) == Status::BadLength);
    CHECK(status_of(sub, 24, 2) == Status::BadField);      // side
    CHECK(status_of(sub, 25, 4) == Status::BadField);      // order type
    CHECK(status_of(sub, 26, 3) == Status::BadField);      // time in force

    seq::Reply r{}; r.seq = 7; r.tag = 8; r.order_id = 9; r.reject = oms::Reject::SelfTrade; r.status = oms::Status::Rejected; r.filled = 1; r.rested = 2; r.cancelled = 3; r.duplicate = true;
    CHECK(from_wire(to_wire(r)) == r);

    std::mt19937_64 g(3);   // random bytes never parse past their length, and an Ok message is exactly its type's size
    bool safe = true;
    for (int i = 0; i < 200000; ++i) {
        std::vector<uint8_t> b(g() % 70);
        for (auto& x : b) x = static_cast<uint8_t>(g());
        if (!b.empty() && (g() & 1)) { b[0] = 0; if (b.size() > 1) b[1] = static_cast<uint8_t>(12 + g() % 40); if (b.size() > 2) b[2] = static_cast<uint8_t>((g() & 1) ? 16 + g() % 4 : g() % 2); if (b.size() > 3) b[3] = (g() & 3) ? kVersion : 1; }
        const ParseResult p = parse_one(b.data(), b.size());
        if (p.status == Status::Ok) safe = safe && p.consumed <= b.size() && p.consumed == expected_size(p.header->type);
        else safe = safe && p.consumed == 0 && p.header == nullptr;
    }
    CHECK(safe);
}

static int connect_to(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { ::close(fd); return -1; }
    set_nodelay(fd);
    timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}
// A connection is usable once the gateway echoes a heartbeat; a slot that is still being released can refuse it, so retry.
static int connect_ready(uint16_t port) {
    for (int i = 0; i < 400; ++i) {
        const int fd = connect_to(port);
        if (fd >= 0) {
            Heartbeat hb{}; fill_header(hb.h, MsgType::Heartbeat, sizeof hb, 1);
            uint8_t back[sizeof(Heartbeat)];
            if (send_all(fd, &hb, sizeof hb) && recv_all(fd, back, sizeof back) && parse_one(back, sizeof back).status == Status::Ok && back[2] == 0) return fd;
            ::close(fd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return -1;
}
static bool read_reply(int fd, seq::Reply& out) {
    uint8_t b[sizeof(Reply)];
    if (!recv_all(fd, b, sizeof b)) return false;
    const ParseResult r = parse_one(b, sizeof b);
    if (r.status != Status::Ok || r.header->type != static_cast<uint8_t>(MsgType::Reply)) return false;
    out = from_wire(*proto2::as<Reply>(r.header));
    return true;
}
static bool closed_by_peer(int fd) { uint8_t c; return ::recv(fd, &c, 1, 0) == 0; }

struct ClientResult { std::vector<seq::Reply> replies; uint64_t sent = 0; bool in_order = true; };

// One agent: pipelines up to 16 requests, cancels and modifies orders it was told are open.
static void client(uint16_t port, uint32_t account, uint64_t seed, int n_ops, ClientResult& res) {
    const int fd = connect_ready(port);
    if (fd < 0) { res.in_order = false; return; }
    std::mt19937_64 g(seed);
    std::vector<uint64_t> mine;
    uint64_t n = 1, cl = 0, got = 0;   // heartbeat used message number 1
    auto take = [&] {
        seq::Reply r;
        if (!read_reply(fd, r)) { res.in_order = false; return false; }
        if (r.tag != ++got + 1) res.in_order = false;
        if (r.status == oms::Status::Open && r.order_id) mine.push_back(r.order_id);
        res.replies.push_back(r);
        return true;
    };
    for (int i = 0; i < n_ops && res.in_order; ++i) {
        ++n;
        bool ok;
        if (i % 4 == 3 && !mine.empty()) { const uint64_t id = mine.back(); mine.pop_back(); const Cancel m = make_cancel(n, account, id); ok = send_all(fd, &m, sizeof m); }
        else if (i % 9 == 8 && !mine.empty()) { const Modify m = make_modify(n, account, mine.back(), 480 + g() % 40, 1 + g() % 50); ok = send_all(fd, &m, sizeof m); }
        else { const Submit m = make_submit(n, account, ++cl, g() & 1, 0, 0, 480 + g() % 40, 1 + g() % 50); ok = send_all(fd, &m, sizeof m); }
        if (!ok) { res.in_order = false; break; }
        ++res.sent;
        if (res.sent - got >= 16 && !take()) break;
    }
    while (res.in_order && got < res.sent) if (!take()) break;
    ::close(fd);
}

static void over_the_wire() {
    constexpr size_t kProducers = 4;
    for (int run = 0; run < 3; ++run) {
        seq::Sequencer<kProducers, 64> sq(cfg(), 100000);
        std::thread seq_thread([&] { sq.run(); });
        gw::TcpGateway<kProducers, 64> gate(sq);
        const uint16_t port = gate.start();
        CHECK(port != 0);

        ClientResult res[3];
        std::vector<std::thread> ts;
        for (uint32_t c = 0; c < 3; ++c) ts.emplace_back([&, c] { client(port, c + 1, 500 + c + 10 * run, GW_OPS, res[c]); });
        for (auto& t : ts) t.join();

        // a malformed stream is closed; the gateway keeps serving others
        {
            const int bad = connect_ready(port);
            CHECK(bad >= 0);
            uint8_t junk[12] = {0, 12, 77, 2, 0, 0, 0, 0, 0, 0, 0, 1};   // unknown type
            CHECK(send_all(bad, junk, sizeof junk));
            CHECK(closed_by_peer(bad));
            ::close(bad);
            const int good = connect_ready(port);
            CHECK(good >= 0);
            if (good >= 0) ::close(good);
        }
        // every slot taken: the next connection is closed at once
        {
            std::vector<int> held;
            for (size_t i = 0; i < kProducers; ++i) { const int fd = connect_ready(port); CHECK(fd >= 0); if (fd >= 0) held.push_back(fd); }
            const int extra = connect_to(port);
            CHECK(extra >= 0 && closed_by_peer(extra));
            if (extra >= 0) ::close(extra);
            for (int fd : held) ::close(fd);
        }
        gate.stop();
        sq.request_stop();
        seq_thread.join();

        uint64_t sent = 0;
        bool ordered = true;
        for (auto& r : res) { sent += r.sent; ordered = ordered && r.in_order && r.replies.size() == r.sent; }
        CHECK(ordered);                                                      // one reply per request, in order, numbered as sent
        CHECK(sent == 3 * static_cast<uint64_t>(GW_OPS));
        const gw::Stats st = gate.stats();
        CHECK(st.requests == sent && st.bad_messages == 1 && st.refused >= 1 && st.poisoned_slots == 0);

        const evlog::EventLog log = evlog::EventLog::from_applied(sq.applied());
        CHECK(log.size() == sent);                                           // nothing lost, nothing invented
        oms::Oms fresh(cfg());
        evlog::ReplayOut out;
        CHECK(evlog::replay(log, fresh, &out));
        bool same = out.replies.size() == sent;
        uint64_t checked = 0;
        for (auto& r : res)
            for (const seq::Reply& w : r.replies) {                          // the reply a client read equals the replay's reply for that sequence number
                same = same && w.seq >= 1 && w.seq <= out.replies.size() && w == out.replies[w.seq - 1];
                ++checked;
            }
        CHECK(same && checked == sent);
        CHECK(fresh.digest() == sq.oms().digest());
        CHECK(out.fills_digest == sq.fills_digest());
        std::string why; CHECK(fresh.check(&why));
    }
}

int main() {
    parser();
    over_the_wire();
    std::printf("%s\n", g_fail ? "FAILED" : "tcp_gateway ok");
    return g_fail ? 1 : 0;
}
