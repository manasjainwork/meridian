// Self-trade index (bug 032): the account price index must give the same answer as the full scan on every call, through random
// submits (limit, market, IOC, FOK, stop, stop-limit), cancels and modifies from a few accounts, and Oms::check() (which also
// rebuilds the index from the order records and compares it) must hold throughout.
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "meridian/oms.hpp"

using namespace meridian;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    uint64_t self_trades = 0, ops_total = 0;
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        oms::Config c;
        c.max_orders = 1u << 16; c.max_price = 1000; c.max_position = 1u << 30; c.crosscheck_self_trade = true;
        oms::Oms o(c);
        std::mt19937_64 g(seed * 7919);
        std::vector<std::pair<uint32_t, uint64_t>> ids;   // (account, order id) seen so far
        uint64_t cl = 0;
        for (int i = 0; i < 40000; ++i) {
            const uint32_t acc = 1 + g() % 4;
            const uint64_t k = g() % 100;
            oms::Result r;
            if (k < 55 || ids.empty()) {
                const auto tif = static_cast<book::TimeInForce>(g() % 3);
                r = o.submit(oms::Request{acc, ++cl, static_cast<book::Side>(g() & 1), oms::Type::Limit, tif, static_cast<uint32_t>(490 + g() % 21), static_cast<uint32_t>(1 + g() % 30), 0});
            } else if (k < 60) {
                r = o.submit(oms::Request{acc, ++cl, static_cast<book::Side>(g() & 1), oms::Type::Market, book::TimeInForce::IOC, 0, static_cast<uint32_t>(1 + g() % 30), 0});
            } else if (k < 68) {
                const auto t = (g() & 1) ? oms::Type::Stop : oms::Type::StopLimit;
                r = o.submit(oms::Request{acc, ++cl, static_cast<book::Side>(g() & 1), t, book::TimeInForce::GTC, static_cast<uint32_t>(490 + g() % 21), static_cast<uint32_t>(1 + g() % 30), static_cast<uint32_t>(490 + g() % 21)});
            } else if (k < 88) {
                const auto& e = ids[g() % ids.size()];
                r = o.cancel((g() % 10) ? e.first : acc, e.second);
            } else {
                const auto& e = ids[g() % ids.size()];
                r = o.modify(e.first, e.second, static_cast<uint32_t>(490 + g() % 21), static_cast<uint32_t>(1 + g() % 30));
            }
            ++ops_total;
            if (r.reject == oms::Reject::SelfTrade) ++self_trades;
            if (r.order_id) ids.emplace_back(acc, r.order_id);
            if (i % 200 == 0) { std::string why; if (!o.check(&why)) { std::printf("check failed at seed %llu op %d: %s\n", (unsigned long long)seed, i, why.c_str()); ++g_fail; break; } }
        }
        CHECK(o.self_trade_mismatches() == 0);
        std::string why; CHECK(o.check(&why));
    }
    CHECK(self_trades > 500);      // the stream really exercises the rejection
    std::printf("self_trade_index: %llu ops, %llu self-trade rejects, %s\n", (unsigned long long)ops_total, (unsigned long long)self_trades, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
