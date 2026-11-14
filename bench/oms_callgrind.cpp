// Instruction-count profile of the OMS path: same deterministic stream as bench/oms_alloc_bench.cpp, no timing.
// Build: g++ -std=c++20 -O2 -g -Iinclude bench/oms_callgrind.cpp -o /tmp/oms_callgrind
// Run:   valgrind --tool=callgrind --collect-atstart=no ./oms_callgrind oms   (or: book)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <valgrind/callgrind.h>
#include "meridian/oms.hpp"
using namespace meridian;
struct Lcg { uint64_t s; uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>(s >> 33); } };
struct Op { bool add; bool buy; uint32_t price, qty, pick; };
int main(int argc, char** argv) {
    const bool oms_mode = argc > 1 && std::strcmp(argv[1], "oms") == 0;
    const size_t N = 200000;
    std::vector<Op> ops(N);
    Lcg g{12345};
    for (auto& o : ops) { o.add = g.next() % 10 < 7; o.buy = g.next() & 1; o.price = 480 + g.next() % 40; o.qty = 1 + g.next() % 50; o.pick = g.next(); }
    uint64_t sink = 0;
    if (!oms_mode) {
        book::OrderBook b(1u << 18, 1000);
        std::vector<book::Fill> fills; fills.reserve(1024);
        std::vector<uint64_t> live; live.reserve(1u << 18);
        uint64_t id = 0;
        CALLGRIND_START_INSTRUMENTATION; CALLGRIND_TOGGLE_COLLECT;
        for (const Op& o : ops) {
            fills.clear();
            if (o.add || live.empty()) {
                ++id;
                const auto r = b.submit(id, o.buy ? book::Side::Buy : book::Side::Sell, book::TimeInForce::GTC, o.price, o.qty, fills);
                sink += r.filled; if (r.rested > 0) live.push_back(id);
            } else {
                const size_t i = o.pick % live.size();
                const uint64_t t = live[i]; live[i] = live.back(); live.pop_back();
                sink += static_cast<uint64_t>(b.cancel(t));
            }
        }
        CALLGRIND_TOGGLE_COLLECT;
    } else {
        oms::Config c; c.max_orders = 1u << 18; c.max_position = int64_t{1} << 40;
        oms::Oms o(c);
        std::vector<uint64_t> live; live.reserve(1u << 18);
        uint64_t cl = 0;
        CALLGRIND_START_INSTRUMENTATION; CALLGRIND_TOGGLE_COLLECT;
        for (const Op& op : ops) {
            const uint32_t acct = op.buy ? 1 : 2;
            if (op.add || live.empty()) {
                const oms::Request rq{acct, ++cl, op.buy ? book::Side::Buy : book::Side::Sell, oms::Type::Limit, book::TimeInForce::GTC, op.price, op.qty, 0};
                const oms::Result r = o.submit(rq);
                sink += r.filled; if (r.status == oms::Status::Open && r.order_id) live.push_back(r.order_id);
            } else {
                const size_t i = op.pick % live.size();
                const uint64_t target = live[i]; live[i] = live.back(); live.pop_back();
                const oms::OrderRec* rec = o.order(target);
                sink += o.cancel(rec ? rec->account : 1, target).cancelled;
            }
        }
        CALLGRIND_TOGGLE_COLLECT;
    }
    std::printf("%s sink %llu\n", oms_mode ? "oms" : "book", (unsigned long long)sink);
    return 0;
}
