// Order records of finished orders are forgotten after a while; open orders never are, and nothing else changes.
#include <cstdio>
#include "meridian/oms.hpp"

using namespace meridian::oms;
using meridian::book::Side;
using meridian::book::TimeInForce;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static Request lim(uint32_t acc, uint64_t cl, Side s, uint32_t price, uint32_t qty) {
    return {acc, cl, s, Type::Limit, TimeInForce::GTC, price, qty};
}

int main() {
    Config c; c.max_orders = 4096;
    Oms o(c);
    // Order 1 stays open the whole time: it is far from the market and never trades.
    const Result keep = o.submit(lim(1, 1, Side::Buy, 1, 1));
    CHECK(keep.status == Status::Open && keep.order_id == 1);
    uint64_t cl = 1;
    const int rounds = 100000;               // 200,000 orders, all of them fill at once
    for (int i = 0; i < rounds; ++i) {
        o.submit(lim(1, ++cl, Side::Buy, 500, 1));
        o.submit(lim(2, ++cl, Side::Sell, 500, 1));
        if (i % 5000 == 0) { std::string why; CHECK(o.check(&why)); }
    }
    std::string why;
    CHECK(o.check(&why));
    CHECK(o.order(1) != nullptr && o.order(1)->status == Status::Open);   // the old open order is still there
    CHECK(o.order(3) == nullptr);                                         // an old finished one is forgotten
    CHECK(o.order(o.next_order_id() - 1) != nullptr);                     // a recent one is kept
    CHECK(o.open_orders(1).size() == 1);
    const Result cx = o.cancel(1, 1);                                     // and it can still be cancelled
    CHECK(cx.status == Status::Cancelled && cx.cancelled == 1);
    CHECK(o.cancel(1, 3).reject == Reject::UnknownOrder);                 // a forgotten id is unknown, not "not open"
    CHECK(o.check(&why));
    // Idempotency memory is a window: a recent duplicate is replayed, a very old one is treated as new.
    const Result d = o.submit(lim(1, cl - 1, Side::Buy, 500, 1));
    CHECK(d.duplicate || d.reject == Reject::KeyReuse);
    const Result old = o.submit(lim(1, 2, Side::Buy, 500, 1));
    CHECK(!old.duplicate);
    std::printf("%s\n", g_fail ? "FAILED" : "oms_retire ok");
    return g_fail ? 1 : 0;
}
