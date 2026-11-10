#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include "meridian/sequencer.hpp"

using namespace meridian;
using namespace meridian::seq;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

#ifndef SEQ_OPS
#define SEQ_OPS 15000
#endif
static oms::Config cfg() { oms::Config c; c.max_orders = 1u << 18; return c; }

struct Lcg { uint64_t s; uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>(s >> 33); } };

// One producer thread: submits, reads replies as they arrive, now and then cancels an order it was told is its own.
// Records every reply and how many requests it sent. Keeps fewer than N un-replied (the sequencer's contract).
template <typename SeqT, size_t N>
static void producer(SeqT& sq, size_t p, uint64_t seed, int n_ops, std::vector<Reply>& got, uint64_t& sent) {
    Lcg g{seed};
    std::vector<uint64_t> mine;
    uint64_t outstanding = 0, cl = 0, tag = 0;
    auto drain = [&] {
        Reply r;
        while (sq.pop_reply(p, r)) {
            got.push_back(r); --outstanding;
            if (r.status == oms::Status::Open && r.order_id) mine.push_back(r.order_id);
        }
    };
    for (int i = 0; i < n_ops; ++i) {
        while (outstanding + 1 >= N) drain();
        Msg m; m.account = static_cast<uint32_t>(p) + 1; m.tag = ++tag;
        if (i % 4 == 3 && !mine.empty()) {
            m.op = Op::Cancel; m.order_id = mine.back(); mine.pop_back();
        } else {
            m.op = Op::Submit;
            m.req = oms::Request{m.account, ++cl, (g.next() & 1) ? book::Side::Buy : book::Side::Sell, oms::Type::Limit,
                                 book::TimeInForce::GTC, 480 + g.next() % 40, 1 + g.next() % 50, 0};
        }
        while (!sq.submit(p, m)) drain();   // busy: reply ring may need draining; the request was not accepted
        ++sent; ++outstanding;
        drain();
    }
    while (outstanding > 0) drain();
}

int main() {
    {   // 1 producer: same digest and same replies as feeding the same requests straight into an Oms
        Sequencer<1, 64> sq(cfg(), 100000);
        std::vector<Reply> got; uint64_t sent = 0;
        std::thread t([&] { sq.run(); });
        producer<decltype(sq), 64>(sq, 0, 7, SEQ_OPS, got, sent);
        sq.request_stop(); t.join();
        oms::Oms ref(cfg());
        bool same = sq.applied().size() == got.size();
        for (size_t i = 0; same && i < sq.applied().size(); ++i) {
            const Reply want = make_reply(i + 1, sq.applied()[i].msg.tag, apply_msg(ref, sq.applied()[i].msg));
            same = (want == got[i]) && sq.applied()[i].reply.seq == i + 1;
        }
        CHECK(same);
        CHECK(sq.oms().digest() == ref.digest());
        CHECK(sent == got.size());
        std::string why; CHECK(sq.oms().check(&why));
    }
    for (int run = 0; run < 3; ++run) {   // 4 producers: replaying the recorded order single-threaded gives the same digest and replies
        Sequencer<4, 64> sq(cfg(), 100000);
        std::vector<Reply> got[4]; uint64_t sent[4] = {};
        std::thread t([&] { sq.run(); });
        std::vector<std::thread> ps;
        for (size_t p = 0; p < 4; ++p) ps.emplace_back([&, p] { producer<decltype(sq), 64>(sq, p, 100 + p + 10 * run, SEQ_OPS, got[p], sent[p]); });
        for (auto& x : ps) x.join();
        sq.request_stop(); t.join();
        oms::Oms ref(cfg());
        bool same = true;
        for (size_t i = 0; same && i < sq.applied().size(); ++i) {
            const Reply want = make_reply(i + 1, sq.applied()[i].msg.tag, apply_msg(ref, sq.applied()[i].msg));
            same = (want == sq.applied()[i].reply);
        }
        CHECK(same);
        CHECK(sq.oms().digest() == ref.digest());
        uint64_t total_sent = 0, total_got = 0;
        for (int p = 0; p < 4; ++p) { total_sent += sent[p]; total_got += got[p].size(); }
        CHECK(total_sent == 4u * SEQ_OPS && total_got == 4u * SEQ_OPS && sq.applied().size() == 4u * SEQ_OPS);   // exactly one outcome per accepted request
        for (size_t i = 0; i < sq.applied().size(); ++i) CHECK(sq.applied()[i].reply.seq == i + 1);
        std::printf("run %d: applied %zu, busy %llu/%llu/%llu/%llu, reply_stalls %llu, digest %016llx\n", run, sq.applied().size(),
                    (unsigned long long)sq.busy(0), (unsigned long long)sq.busy(1), (unsigned long long)sq.busy(2), (unsigned long long)sq.busy(3),
                    (unsigned long long)sq.reply_stalls(), (unsigned long long)sq.oms().digest());
    }
    {   // ring full: nothing running, so the ring fills; the extra request is refused and counted, never lost silently
        Sequencer<2, 8> sq(cfg());
        Msg m; m.op = Op::Submit; m.account = 1;
        int accepted = 0, refused = 0;
        for (uint64_t i = 1; i <= 12; ++i) {
            m.tag = i; m.req = oms::Request{1, i, book::Side::Buy, oms::Type::Limit, book::TimeInForce::GTC, 500, 1, 0};
            (sq.submit(0, m) ? accepted : refused)++;
        }
        CHECK(accepted == 8 && refused == 4 && sq.busy(0) == 4 && sq.busy(1) == 0);
        while (sq.poll_all() > 0) {}
        Reply r; int replies = 0; uint64_t last_tag = 0;
        while (sq.pop_reply(0, r)) { ++replies; CHECK(r.tag == last_tag + 1); last_tag = r.tag; }
        CHECK(replies == accepted && sq.applied().size() == 8);
    }
    {   // rotation: with equal load on all rings each ring is served in turn
        Sequencer<3, 8> sq(cfg());
        Msg m; m.op = Op::Submit;
        for (uint32_t p = 0; p < 3; ++p) for (uint64_t i = 1; i <= 4; ++i) {
            m.account = p + 1; m.tag = i; m.req = oms::Request{p + 1, i, book::Side::Buy, oms::Type::Limit, book::TimeInForce::GTC, 500, 1, 0};
            CHECK(sq.submit(p, m));
        }
        while (sq.poll_all() > 0) {}
        CHECK(sq.applied().size() == 12);
        int first[3] = {};
        for (size_t i = 0; i + 2 < sq.applied().size(); i += 3) first[sq.applied()[i].producer]++;
        CHECK(first[0] > 0 && first[1] > 0 && first[2] > 0);   // the first slot of each pass is not always ring 0
    }
    std::printf(g_fail ? "%d FAILED\n" : "all sequencer tests passed\n", g_fail);
    return g_fail != 0;
}
