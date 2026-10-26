#include <algorithm>
#include <cstdio>
#include <vector>
#include "meridian/order_book.hpp"
#include "meridian/timing.hpp"

using namespace meridian;
using namespace meridian::book;

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

struct Op { uint64_t id; Side side; uint32_t price, qty; bool cancel; };

static void report(const char* name, std::vector<uint64_t>& d, double ghz) {
    if (d.empty()) return;
    std::sort(d.begin(), d.end());
    auto pct = [&](double p) { return d[static_cast<size_t>(p * (d.size() - 1))]; };
    std::printf("%-8s n=%-7zu", name, d.size());
    const double ps[] = {0.0, 0.5, 0.99, 0.999, 1.0};
    for (double p : ps) std::printf(" %8.1f", pct(p) / ghz);
    std::printf("   (min p50 p99 p99.9 max, ns)\n");
}

int main() {
    constexpr size_t N = 400000;
    constexpr uint32_t MID = 1000;
    OrderBook book(1u << 17, 2048);
    std::vector<Op> ops;
    ops.reserve(N);
    uint64_t next_id = 1;
    std::vector<uint64_t> live;
    for (size_t i = 0; i < N; ++i) {
        if (!live.empty() && rnd() % 100 < 30) {                       // cancel
            const size_t k = rnd() % live.size();
            ops.push_back({live[k], Side::Buy, 0, 0, true});
            live[k] = live.back(); live.pop_back();
        } else {                                                        // limit order near the mid
            const Side s = (rnd() & 1) ? Side::Buy : Side::Sell;
            const int64_t off = static_cast<int64_t>(rnd() % 40) - 20;
            const uint32_t p = static_cast<uint32_t>(MID + off + (s == Side::Buy ? -2 : 2));
            ops.push_back({next_id, s, p, 1 + static_cast<uint32_t>(rnd() % 100), false});
            live.push_back(next_id++);
        }
    }
    const double ghz = calibrate_tsc_ghz();
    std::vector<Fill> fills;
    fills.reserve(64);
    std::vector<uint64_t> d_rest, d_match, d_cancel;
    uint64_t sink = 0, nfills = 0;

    for (const Op& o : ops) {
        fills.clear();
        const uint64_t a = rdtsc_begin();
        if (o.cancel) sink += book.cancel(o.id);
        else sink += static_cast<uint64_t>(book.add_limit(o.id, o.side, o.price, o.qty, fills));
        const uint64_t b = rdtsc_end();
        const uint64_t c = (b - a > 32) ? b - a - 32 : 0;
        if (o.cancel) d_cancel.push_back(c);
        else if (fills.empty()) d_rest.push_back(c);
        else d_match.push_back(c);
        nfills += fills.size();
    }
    std::printf("order book, %zu ops, TSC %.3f GHz, timer overhead (32 cycles) subtracted\n", N, ghz);
    report("rest", d_rest, ghz);
    report("match", d_match, ghz);
    report("cancel", d_cancel, ghz);
    std::printf("fills=%llu resting=%zu bid=%u ask=%u (checksum %llu)\n",
                static_cast<unsigned long long>(nfills), book.size(), book.best_bid(), book.best_ask(),
                static_cast<unsigned long long>(sink));
}
