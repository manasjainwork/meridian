// OMS tests: directed cases, a fuzz that checks every invariant after every operation, determinism, and mutation tests
// (each deliberately broken variant must be caught by the same fuzz).
#include <cstdio>
#include <random>
#include <vector>
#include "meridian/oms.hpp"

using namespace meridian::oms;
using meridian::book::Side;
using meridian::book::TimeInForce;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static Request lim(uint32_t acc, uint64_t cl, Side s, uint32_t price, uint32_t qty, TimeInForce t = TimeInForce::GTC) {
    return {acc, cl, s, Type::Limit, t, price, qty};
}
static Request mkt(uint32_t acc, uint64_t cl, Side s, uint32_t qty) { return {acc, cl, s, Type::Market, TimeInForce::IOC, 0, qty}; }

static void self_trade() {
    Config c; c.max_price = 1000; c.max_order_qty = 500; c.max_position = 1000;
    Oms o(c);
    const Result s1 = o.submit(lim(1, 1, Side::Sell, 500, 10));
    CHECK(o.submit(lim(1, 2, Side::Buy, 500, 5)).reject == Reject::SelfTrade);
    const Result b3 = o.submit(lim(1, 3, Side::Buy, 499, 5));
    CHECK(b3.reject == Reject::None);
    CHECK(o.submit(mkt(1, 4, Side::Buy, 5)).reject == Reject::SelfTrade);
    CHECK(o.submit(lim(2, 1, Side::Buy, 500, 5)).filled == 5);
    CHECK(o.modify(1, b3.order_id, 501, 5).reject == Reject::SelfTrade);
    CHECK(s1.reject == Reject::None && o.check());
}

static void stops() {
    Config c; c.max_price = 1000; c.max_order_qty = 500; c.max_position = 1000;
    Oms o(c);
    o.submit(lim(1, 1, Side::Sell, 500, 10));
    Request sb = lim(2, 1, Side::Buy, 0, 5); sb.type = Type::Stop; sb.stop_price = 500;
    const Result p = o.submit(sb);
    CHECK(p.reject == Reject::None && p.status == Status::Pending && o.account(2)->position == 0 && o.check());
    CHECK(o.submit(sb).duplicate && o.submit(sb).order_id == p.order_id);                    // a retry does not create a second stop
    const Result t = o.submit(lim(3, 1, Side::Buy, 500, 1));                                  // a trade at 500 fires the stop
    CHECK(t.filled == 1 && t.triggered.size() == 1 && t.triggered[0].qty == 5 && t.triggered[0].price == 500);
    CHECK(o.order(p.order_id)->status == Status::Triggered && o.account(2)->position == 5 && o.check());
    Request ss = lim(2, 2, Side::Sell, 0, 3); ss.type = Type::Stop; ss.stop_price = 400;      // below the last price: waits
    const Result q = o.submit(ss);
    CHECK(q.status == Status::Pending);
    CHECK(o.cancel(2, q.order_id).status == Status::Cancelled && o.cancel(2, q.order_id).reject == Reject::NotOpen && o.check());
    Request sb2 = lim(2, 3, Side::Buy, 0, 2); sb2.type = Type::Stop; sb2.stop_price = 450;    // already past its price: fires at once
    CHECK(o.submit(sb2).status == Status::Triggered && o.check());
}

static void directed() {
    Config c; c.max_price = 1000; c.max_order_qty = 500; c.max_position = 1000;
    Oms o(c);
    // 1 sells 100 @ 500, 2 buys 60 @ 505: trades 60 at the resting price 500
    Result a = o.submit(lim(1, 1, Side::Sell, 500, 100));
    CHECK(a.reject == Reject::None && a.status == Status::Open && a.rested == 100);
    Result b = o.submit(lim(2, 1, Side::Buy, 505, 60));
    CHECK(b.status == Status::Filled && b.filled == 60 && b.fills.size() == 1 && b.fills[0].price == 500);
    CHECK(o.account(1)->position == -60 && o.account(1)->cash == 30000);
    CHECK(o.account(2)->position == 60 && o.account(2)->cash == -30000);
    CHECK(o.order(a.order_id)->remaining == 40 && o.order(a.order_id)->status == Status::Open);
    CHECK(o.account(1)->open_sell == 40);
    // equity at mark 510: seller 30000 - 60*510 = -600, buyer +600
    CHECK(o.account(1)->cash + o.account(1)->position * 510 == -600);
    // market order eats the rest and the remainder is cancelled
    Result m = o.submit(mkt(2, 2, Side::Buy, 100));
    CHECK(m.filled == 40 && m.cancelled == 60 && m.status == Status::Cancelled);
    // IOC and FOK
    Result i = o.submit(lim(1, 2, Side::Sell, 600, 10, TimeInForce::IOC));
    CHECK(i.status == Status::Cancelled && i.cancelled == 10 && o.account(1)->open_sell == 0);
    Result f = o.submit(lim(1, 3, Side::Sell, 600, 10, TimeInForce::FOK));
    CHECK(f.status == Status::Cancelled && f.filled == 0);
    // rejects
    CHECK(o.submit(lim(1, 4, Side::Buy, 0, 10)).reject == Reject::BadParam);
    CHECK(o.submit(lim(1, 5, Side::Buy, 1000, 10)).reject == Reject::BadParam);
    CHECK(o.submit(lim(1, 6, Side::Buy, 500, 0)).reject == Reject::BadParam);
    CHECK(o.submit(lim(1, 7, Side::Buy, 500, 501)).reject == Reject::QtyLimit);
    CHECK(o.check());
    // position limit counts open orders of the same side: 900 resting + 200 more would be 1100 > 1000
    Oms p(c);
    CHECK(p.submit(lim(3, 1, Side::Buy, 100, 500)).status == Status::Open);
    CHECK(p.submit(lim(3, 2, Side::Buy, 100, 400)).status == Status::Open);
    CHECK(p.submit(lim(3, 3, Side::Buy, 100, 200)).reject == Reject::PositionLimit);
    CHECK(p.submit(lim(3, 4, Side::Sell, 900, 200)).status == Status::Open);    // the other side has its own room
    CHECK(p.check());
    // cancel and modify
    Oms q(c);
    const Result r1 = q.submit(lim(1, 1, Side::Buy, 100, 50));
    CHECK(q.cancel(2, r1.order_id).reject == Reject::NotOwner);
    CHECK(q.cancel(1, 999).reject == Reject::UnknownOrder);
    const Result mo = q.modify(1, r1.order_id, 100, 20);                           // smaller, same price: keeps its place
    CHECK(mo.status == Status::Open && q.order(r1.order_id)->remaining == 20 && q.account(1)->open_buy == 20);
    q.submit(lim(2, 1, Side::Sell, 110, 30));
    const Result mx = q.modify(1, r1.order_id, 110, 20);                           // new price crosses: trades as aggressor
    CHECK(mx.filled == 20 && mx.status == Status::Filled && q.account(1)->position == 20 && q.account(1)->open_buy == 0);
    CHECK(q.cancel(1, r1.order_id).reject == Reject::NotOpen);
    CHECK(q.check());
}

// Returns true if anything wrong was seen. With bugs == 0 that must never happen.
static bool fuzz(uint64_t seed, uint32_t bugs, int ops) {
    Config c; c.max_price = 200; c.max_order_qty = 60; c.max_position = 150; c.bugs = bugs;
    Oms o(c);
    std::mt19937_64 rng(seed);
    std::vector<Request> sent;
    uint64_t cl = 0;
    std::string why;
    for (int i = 0; i < ops; ++i) {
        const int roll = static_cast<int>(rng() % 100);
        if (roll < 55) {
            Request r = lim(1 + rng() % 4, ++cl, (rng() & 1) ? Side::Buy : Side::Sell, 90 + rng() % 21, 1 + rng() % 60,
                            (rng() % 10 == 0) ? TimeInForce::IOC : (rng() % 12 == 0 ? TimeInForce::FOK : TimeInForce::GTC));
            if (rng() % 7 == 0) { r.type = (rng() & 1) ? Type::Stop : Type::StopLimit; r.stop_price = 90 + rng() % 21; }
            if (rng() % 9 == 0) r = mkt(r.account, r.cl_ord_id, r.side, r.qty);
            const Result sr = o.submit(r);
            for (const auto& f : sr.fills) if (o.order(f.maker_id)->account == o.order(f.taker_id)->account) return true;   // traded with itself
            for (const auto& f : sr.triggered) if (o.order(f.maker_id)->account == o.order(f.taker_id)->account) return true;
            sent.push_back(r);
        } else if (roll < 68 && !sent.empty()) {                       // the client retries: identical request
            const Request& r = sent[rng() % sent.size()];
            const Result first = o.submit(r);                          // answered from memory (or, with the bug, trades again)
            if (!first.duplicate) return true;
            const uint64_t d = o.digest();
            const Result again = o.submit(r);
            if (!again.duplicate || o.digest() != d) return true;
        } else if (roll < 72 && !sent.empty()) {                       // same key, different contents: must be refused
            Request r = sent[rng() % sent.size()];
            r.qty = r.qty % 60 + 1;                                    // always a different quantity
            const uint64_t d = o.digest();
            const Result x = o.submit(r);
            if (x.reject != Reject::KeyReuse || o.digest() != d) return true;
        } else if (roll < 85) {                                        // cancel by some account
            const uint64_t id = 1 + rng() % (o.next_order_id() ? o.next_order_id() : 1);
            const OrderRec* rec = o.order(id);
            const bool was_open = rec && rec->status == Status::Open;  // copy now: rec points at the live record and changes
            const uint32_t owner = rec ? rec->account : 0;
            const uint32_t acc = 1 + rng() % 4;
            const uint64_t d = o.digest();
            const Result x = o.cancel(acc, id);
            if (was_open && owner != acc && x.reject == Reject::None) return true;                                    // someone else's order cancelled
            if (x.reject != Reject::None && o.digest() != d) return true;                                              // a refused cancel changed state
        } else {                                                       // modify
            const uint64_t id = 1 + rng() % (o.next_order_id() ? o.next_order_id() : 1);
            const OrderRec* rec = o.order(id);
            const uint32_t acc = rec ? rec->account : 1;
            const uint64_t d = o.digest();
            const Result x = o.modify(acc, id, 90 + rng() % 21, 1 + rng() % 60);
            if (x.reject != Reject::None && o.digest() != d) return true;
            for (const auto& f : x.fills) if (o.order(f.maker_id)->account == o.order(f.taker_id)->account) return true;
        }
        if (!o.check(&why)) return true;
    }
    return false;
}

static void determinism() {
    for (uint64_t seed = 1; seed <= 5; ++seed) {
        Config c; c.max_price = 200; c.max_order_qty = 60; c.max_position = 150;
        Oms a(c), b(c);
        std::mt19937_64 r1(seed), r2(seed);
        for (int i = 0; i < 2000; ++i) {
            const Request x = lim(1 + r1() % 4, i + 1, (r1() & 1) ? Side::Buy : Side::Sell, 90 + r1() % 21, 1 + r1() % 60);
            const Request y = lim(1 + r2() % 4, i + 1, (r2() & 1) ? Side::Buy : Side::Sell, 90 + r2() % 21, 1 + r2() % 60);
            a.submit(x); b.submit(y);
        }
        CHECK(a.digest() == b.digest());
    }
}

int main() {
    directed();
    self_trade();
    stops();
    determinism();
    int bad = 0;
    for (uint64_t s = 1; s <= 120; ++s) if (fuzz(s, 0, 1200)) ++bad;
    std::printf("fuzz: 120 seeds x 1200 operations, invariants checked after every operation: %d seeds with a problem\n", bad);
    CHECK(bad == 0);
    const struct { uint32_t bit; const char* name; } mutants[] = {
        {kNoIdempotency, "retry executes again (no idempotency)"},
        {kCancelAnyAccount, "any account can cancel any order"},
        {kSkipOpenOnCancel, "open quantity not reduced on cancel"},
        {kTakerNotCredited, "taker position and cash not updated"},
        {kNoSelfTrade, "an account can trade with its own order"},
        {kStopNeverTriggers, "stop orders never fire"},
        {kStopWrongWay, "stop orders fire in the wrong direction"},
    };
    for (const auto& m : mutants) {
        int caught = 0;
        for (uint64_t s = 1; s <= 40; ++s) if (fuzz(s, m.bit, 1200)) ++caught;
        std::printf("mutant \"%s\": caught in %d of 40 seeds\n", m.name, caught);
        CHECK(caught > 0);
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
