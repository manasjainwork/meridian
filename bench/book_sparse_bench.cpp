#include <algorithm>
#include <cstdio>
#include <vector>
#include "meridian/order_book.hpp"
#include "meridian/timing.hpp"

using namespace meridian;
using namespace meridian::book;

// Worst case for a linear best-price scan: a far resting order keeps the book sparse, and every
// sweep empties the best level, forcing a search across the gap to the next one.
int main() {
    constexpr uint32_t MAXP = 65536, NEAR = 1000, FAR = 60000;
    constexpr size_t N = 20000;
    OrderBook book(1u << 12, MAXP);
    std::vector<Fill> fills;
    fills.reserve(8);
    book.add_limit(1, Side::Sell, FAR, 1000000, fills);   // far ask
    book.add_limit(2, Side::Buy, 100, 1000000, fills);    // far bid
    const double ghz = calibrate_tsc_ghz();
    std::vector<uint64_t> d_ask, d_bid;
    uint64_t id = 10, sink = 0;
    for (size_t i = 0; i < N; ++i) {
        fills.clear();
        book.add_limit(id++, Side::Sell, NEAR, 1, fills);   // new best ask
        const uint64_t a = rdtsc_begin();
        book.add_limit(id++, Side::Buy, NEAR, 1, fills);  // sweeps it
        sink += fills.size();
        const uint64_t b = rdtsc_end();
        d_ask.push_back(b - a > 32 ? b - a - 32 : 0);

        fills.clear();
        book.add_limit(id++, Side::Buy, FAR - 1, 1, fills);  // new best bid
        const uint64_t c = rdtsc_begin();
        book.add_limit(id++, Side::Sell, FAR - 1, 1, fills);
        const uint64_t e = rdtsc_end();
        sink += fills.size();
        d_bid.push_back(e - c > 32 ? e - c - 32 : 0);
    }
    auto rep = [&](const char* name, std::vector<uint64_t>& d) {
        std::sort(d.begin(), d.end());
        auto pct = [&](double p) { return d[static_cast<size_t>(p * (d.size() - 1))]; };
        std::printf("%-12s n=%-6zu", name, d.size());
        const double ps[] = {0.0, 0.5, 0.99, 0.999, 1.0};
        for (double p : ps) std::printf(" %9.1f", pct(p) / ghz);
        std::printf("   (min p50 p99 p99.9 max, ns)\n");
    };
    std::printf("sparse sweep, gap ~59000 ticks, TSC %.3f GHz\n", ghz);
    rep("sweep ask", d_ask);
    rep("sweep bid", d_bid);
    std::printf("best bid=%u ask=%u (fills checksum %llu)\n", book.best_bid(), book.best_ask(), static_cast<unsigned long long>(sink));
}
