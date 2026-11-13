// Event log v2 (design M2): codec, round trip, truncation at every byte, bad CRC, sequence gap, bad command, and replay of a
// real multi-producer sequencer run (same replies, same book and OMS digest, same fills).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
#include "meridian/event_log.hpp"
#include "meridian/raft_oms.hpp"

using namespace meridian;
using namespace meridian::seq;
using namespace meridian::evlog;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

#ifndef LOG_OPS
#define LOG_OPS 4000
#endif
static oms::Config cfg() { oms::Config c; c.max_orders = 1u << 18; return c; }

static Msg random_msg(std::mt19937_64& g) {
    Msg m; m.tag = g();
    switch (g() % 3) {
        case 0:
            m.op = Op::Submit; m.account = 0;
            m.req = oms::Request{static_cast<uint32_t>(g() % 50), g(), (g() & 1) ? book::Side::Buy : book::Side::Sell,
                                 static_cast<oms::Type>(g() % 4), static_cast<book::TimeInForce>(g() % 3),
                                 static_cast<uint32_t>(g()), static_cast<uint32_t>(g()), static_cast<uint32_t>(g())};
            break;
        case 1: m.op = Op::Cancel; m.account = static_cast<uint32_t>(g()); m.order_id = g(); break;
        default: m.op = Op::Modify; m.account = static_cast<uint32_t>(g()); m.order_id = g(); m.price = static_cast<uint32_t>(g()); m.qty = static_cast<uint32_t>(g()); break;
    }
    return m;
}

static void codec() {
    std::mt19937_64 g(5);
    bool same_as_raft = true, round = true;
    for (int i = 0; i < 3000; ++i) {
        const Msg m = random_msg(g);
        uint8_t a[kCmdBytes];
        encode(m, a);
        Msg d; d.tag = m.tag;
        round = round && decode(a, d);
        uint8_t b[kCmdBytes];
        encode(d, b);
        round = round && std::memcmp(a, b, kCmdBytes) == 0 && d.op == m.op && d.tag == m.tag;
        raft::OmsCmd c{static_cast<raft::OmsCmd::Kind>(m.op), m.req, m.account, m.order_id, m.price, m.qty};   // the Raft entry for the same request
        const std::string e = raft::encode_cmd(c);
        same_as_raft = same_as_raft && e.size() == kCmdBytes && std::memcmp(e.data(), a, kCmdBytes) == 0;
    }
    CHECK(round);
    CHECK(same_as_raft);   // one request format serves the log and the Raft entry (D5)
    Msg m; uint8_t a[kCmdBytes];
    encode(random_msg(g), a);
    for (size_t at : {size_t{0}, size_t{13}, size_t{14}, size_t{15}}) {   // kind, side, type, tif
        uint8_t bad[kCmdBytes]; std::memcpy(bad, a, kCmdBytes); bad[at] = 9;
        CHECK(!decode(bad, m));
    }
}

// One producer thread, as in test_sequencer.
template <typename SeqT, size_t N>
static void producer(SeqT& sq, size_t p, uint64_t seed, int n_ops) {
    std::mt19937_64 g(seed);
    std::vector<uint64_t> mine;
    uint64_t outstanding = 0, cl = 0, tag = 0;
    auto drain = [&] {
        Reply r;
        while (sq.pop_reply(p, r)) { --outstanding; if (r.status == oms::Status::Open && r.order_id) mine.push_back(r.order_id); }
    };
    for (int i = 0; i < n_ops; ++i) {
        while (outstanding + 1 >= N) drain();
        Msg m; m.account = static_cast<uint32_t>(p) + 1; m.tag = ++tag;
        if (i % 4 == 3 && !mine.empty()) { m.op = Op::Cancel; m.order_id = mine.back(); mine.pop_back(); }
        else if (i % 9 == 8 && !mine.empty()) { m.op = Op::Modify; m.order_id = mine.back(); m.price = 480 + g() % 40; m.qty = 1 + g() % 50; }
        else {
            m.op = Op::Submit;
            m.req = oms::Request{m.account, ++cl, (g() & 1) ? book::Side::Buy : book::Side::Sell, oms::Type::Limit,
                                 book::TimeInForce::GTC, static_cast<uint32_t>(480 + g() % 40), static_cast<uint32_t>(1 + g() % 50), 0};
        }
        while (!sq.submit(p, m)) drain();
        ++outstanding;
        drain();
    }
    while (outstanding > 0) drain();
}

static EventLog small_log(size_t n) {
    std::mt19937_64 g(11);
    EventLog l;
    for (size_t i = 0; i < n; ++i) {
        Msg m = random_msg(g);
        m.req.side = static_cast<book::Side>(g() & 1); m.req.type = static_cast<oms::Type>(g() % 4); m.req.tif = static_cast<book::TimeInForce>(g() % 3);
        l.append(m, static_cast<uint32_t>(g() % 4), g(), g());
    }
    return l;
}

static void damage() {
    const EventLog l = small_log(50);
    const std::vector<uint8_t> good = l.to_bytes();
    const size_t H = sizeof(EventLog::Header), R = sizeof(Record);
    EventLog out;
    {   // round trip through bytes and through a file
        LoadResult r = EventLog::parse(good.data(), good.size(), out);
        CHECK(r.status == LoadStatus::Ok && r.valid == 50 && out.size() == 50);
        bool eq = true;
        for (size_t i = 0; i < 50; ++i) eq = eq && std::memcmp(&out.records()[i], &l.records()[i], R) == 0;
        CHECK(eq);
        CHECK(l.save("event_log_test.bin"));
        EventLog f;
        r = EventLog::load("event_log_test.bin", f);
        CHECK(r.status == LoadStatus::Ok && r.valid == 50 && f.to_bytes() == good);
        CHECK(EventLog::load("no_such_event_log.bin", f).status == LoadStatus::IoError);
    }
    {   // cut the file at every byte
        bool ok = true;
        for (size_t cut = 0; cut < good.size(); ++cut) {
            const LoadResult r = EventLog::parse(good.data(), cut, out);
            if (cut < H) ok = ok && r.status == LoadStatus::BadHeader && r.valid == 0;
            else if ((cut - H) % R == 0) ok = ok && r.status == LoadStatus::Ok && r.valid == (cut - H) / R;
            else ok = ok && r.status == LoadStatus::Truncated && r.valid == (cut - H) / R;
        }
        CHECK(ok);
    }
    {   // flip one byte, any byte of a record: always a bad CRC, and the records before it are still usable
        bool ok = true;
        for (size_t rec : {size_t{0}, size_t{25}, size_t{49}})
            for (size_t j = 0; j < R; ++j) {
                std::vector<uint8_t> b = good;
                b[H + rec * R + j] ^= 0x5A;
                const LoadResult r = EventLog::parse(b.data(), b.size(), out);
                ok = ok && r.status == LoadStatus::BadCrc && r.valid == rec && out.size() == rec;
            }
        CHECK(ok);
        std::vector<uint8_t> b = good; b[3] ^= 1;   // magic
        CHECK(EventLog::parse(b.data(), b.size(), out).status == LoadStatus::BadHeader);
        b = good; b[8] ^= 1;                        // record size in the header
        CHECK(EventLog::parse(b.data(), b.size(), out).status == LoadStatus::BadHeader);
    }
    {   // a missing, repeated or swapped record has a valid CRC but breaks the sequence
        std::vector<uint8_t> b = good;
        b.erase(b.begin() + H + 10 * R, b.begin() + H + 11 * R);
        LoadResult r = EventLog::parse(b.data(), b.size(), out);
        CHECK(r.status == LoadStatus::SeqGap && r.valid == 10);
        b = good;
        b.insert(b.begin() + H + 20 * R, good.begin() + H + 19 * R, good.begin() + H + 20 * R);
        r = EventLog::parse(b.data(), b.size(), out);
        CHECK(r.status == LoadStatus::SeqGap && r.valid == 20);
        b = good;
        for (size_t j = 0; j < R; ++j) std::swap(b[H + 5 * R + j], b[H + 6 * R + j]);
        r = EventLog::parse(b.data(), b.size(), out);
        CHECK(r.status == LoadStatus::SeqGap && r.valid == 5);
    }
    {   // an undecodable request with a correct CRC is refused, not applied
        Record r = l.records()[7];
        r.cmd[0] = 9;
        r.crc = record_crc(r);
        std::vector<uint8_t> b = good;
        std::memcpy(b.data() + H + 7 * R, &r, R);
        const LoadResult res = EventLog::parse(b.data(), b.size(), out);
        CHECK(res.status == LoadStatus::BadCmd && res.valid == 7);
    }
}

static void replay_of_a_run() {
    for (int run = 0; run < 3; ++run) {
        Sequencer<4, 64> sq(cfg(), 100000);
        std::thread t([&] { sq.run(); });
        std::vector<std::thread> ps;
        for (size_t p = 0; p < 4; ++p) ps.emplace_back([&, p] { producer<decltype(sq), 64>(sq, p, 300 + p + 10 * run, LOG_OPS); });
        for (auto& x : ps) x.join();
        sq.request_stop(); t.join();

        const EventLog log = EventLog::from_applied(sq.applied());
        CHECK(log.size() == sq.applied().size() && log.size() > 0);
        const std::string path = "event_log_run" + std::to_string(run) + ".bin";
        CHECK(log.save(path.c_str()));
        EventLog loaded;
        CHECK(EventLog::load(path.c_str(), loaded).status == LoadStatus::Ok);
        CHECK(loaded.size() == log.size());

        oms::Oms fresh(cfg());
        ReplayOut out;
        CHECK(replay(loaded, fresh, &out));
        bool same = out.replies.size() == sq.applied().size();
        for (size_t i = 0; same && i < out.replies.size(); ++i) same = out.replies[i] == sq.applied()[i].reply;
        CHECK(same);                                                   // every reply, in order
        CHECK(fresh.digest() == sq.oms().digest());                    // book, accounts, orders
        CHECK(out.fills_digest == sq.fills_digest());                  // every fill
        std::string why; CHECK(fresh.check(&why));

        oms::Oms again(cfg()); ReplayOut out2;                         // replay is repeatable
        CHECK(replay(loaded, again, &out2) && again.digest() == fresh.digest() && out2.fills_digest == out.fills_digest);

        // a log that lost one record replays to a different state: the checks above can fail
        EventLog shorter;
        for (size_t i = 0; i < log.size(); ++i) if (i != log.size() / 2) { Msg m; log.message(i, m); shorter.append(m, log.records()[i].producer); }
        oms::Oms other(cfg()); ReplayOut o3;
        CHECK(replay(shorter, other, &o3) && (other.digest() != fresh.digest() || o3.fills_digest != out.fills_digest));
    }
}

int main() {
    codec();
    damage();
    replay_of_a_run();
    std::printf("%s\n", g_fail ? "FAILED" : "event_log ok");
    return g_fail ? 1 : 0;
}
