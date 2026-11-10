// Mutation tests for the sequencer: each deliberate bug (see Bug in sequencer.hpp) must be caught by at least one property.
// Synchronous and deterministic: no threads, the test thread submits, polls and drains replies.
// Properties: count (every request has one reply and one applied record), seq (1, 2, 3, ... with no gap),
// replay (applying the recorded order to a fresh Oms gives the same replies and digest), fifo (each producer's requests
// are applied in the order it sent them), fair (the first ring served in a pass rotates).
#include <cstdint>
#include <cstdio>
#include <vector>
#include "meridian/sequencer.hpp"

using namespace meridian;
using namespace meridian::seq;

struct Lcg { uint64_t s; uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>(s >> 33); } };
struct Outcome { bool count, seq, replay, fifo, fair; bool all() const { return count && seq && replay && fifo && fair; } };

static Outcome run(uint32_t bugs) {
    oms::Config c; c.max_orders = 1u << 16;
    Sequencer<4, 64> sq(c, 20000, bugs);
    std::vector<Msg> sent[4];
    std::vector<Reply> got[4];
    std::vector<uint64_t> mine[4];
    uint64_t cl[4] = {}, tag[4] = {};
    int first[4] = {};
    int passes = 0;
    Lcg g{42};
    auto drain = [&] {
        for (size_t p = 0; p < 4; ++p) {
            Reply r;
            while (sq.pop_reply(p, r)) { got[p].push_back(r); if (r.status == oms::Status::Open && r.order_id) mine[p].push_back(r.order_id); }
        }
    };
    for (int round = 0; round < 1500; ++round) {
        for (size_t p = 0; p < 4; ++p) {
            Msg m; m.account = static_cast<uint32_t>(p) + 1; m.tag = ++tag[p];
            if (round % 4 == 3 && !mine[p].empty()) { m.op = Op::Cancel; m.order_id = mine[p].back(); mine[p].pop_back(); }
            else m.req = oms::Request{m.account, ++cl[p], (g.next() & 1) ? book::Side::Buy : book::Side::Sell, oms::Type::Limit,
                                      book::TimeInForce::GTC, 490 + g.next() % 20, 1 + g.next() % 20, 0};
            if (sq.submit(p, m)) sent[p].push_back(m);
        }
        const size_t before = sq.applied().size();
        sq.poll_all();
        ++passes;
        if (sq.applied().size() > before) first[sq.applied()[before].producer]++;
        drain();
    }
    while (sq.poll_all() > 0) drain();
    drain();

    Outcome o{true, true, true, true, true};
    size_t total_sent = 0;
    for (size_t p = 0; p < 4; ++p) { total_sent += sent[p].size(); if (got[p].size() != sent[p].size()) o.count = false; }
    if (sq.applied().size() != total_sent) o.count = false;

    oms::Oms ref(c);
    for (size_t i = 0; i < sq.applied().size(); ++i) {
        const Applied& a = sq.applied()[i];
        if (a.reply.seq != i + 1) o.seq = false;
        if (!(make_reply(i + 1, a.msg.tag, apply_msg(ref, a.msg)) == a.reply)) o.replay = false;
    }
    if (ref.digest() != sq.oms().digest()) o.replay = false;

    uint64_t next_tag[4] = {1, 1, 1, 1};
    for (const Applied& a : sq.applied()) { if (a.msg.tag != next_tag[a.producer]++) o.fifo = false; }

    const double mean = passes / 4.0;
    for (int p = 0; p < 4; ++p) if (first[p] < 0.8 * mean || first[p] > 1.2 * mean) o.fair = false;
    return o;
}

int main() {
    int bad = 0;
    const Outcome base = run(0);
    std::printf("%-14s count=%d seq=%d replay=%d fifo=%d fair=%d\n", "no bug", base.count, base.seq, base.replay, base.fifo, base.fair);
    if (!base.all()) { std::printf("FAIL: the unmutated sequencer breaks a property\n"); ++bad; }
    struct M { const char* name; uint32_t flag; bool must_be_caught; };
    const M ms[] = {{"DropRequest", kDropRequest, true}, {"SkipSeq", kSkipSeq, true}, {"NoRotation", kNoRotation, true},
                    {"SwallowReply", kSwallowReply, true}, {"StaleReply", kStaleReply, true}, {"Reorder", kReorder, true},
                    {"ApplyTwice", kApplyTwice, false}};
    for (const M& m : ms) {
        const Outcome o = run(m.flag);
        std::printf("%-14s count=%d seq=%d replay=%d fifo=%d fair=%d  -> %s\n", m.name, o.count, o.seq, o.replay, o.fifo, o.fair,
                    o.all() ? "NOT caught" : "caught");
        if (m.must_be_caught && o.all()) { std::printf("FAIL: %s was not caught\n", m.name); ++bad; }
        if (!m.must_be_caught && !o.all()) { std::printf("FAIL: %s was expected to be absorbed (OMS idempotency)\n", m.name); ++bad; }
    }
    std::printf(bad ? "%d FAILED\n" : "all sequencer mutants behave as expected\n", bad);
    return bad != 0;
}
