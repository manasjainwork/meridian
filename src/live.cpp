// Meridian live view: a steady stream of orders into the matching engine, one status line per second.
//   live [--seconds S] [--seed N] [--mix]
// Each line shows throughput and the latency of the engine call itself (rdtsc, single thread, closed loop):
// that is SERVICE time per operation, not end-to-end latency, and it is not a substitute for the benchmarks in bench/.
// Percentiles come from a log-linear histogram (16 sub-buckets per power of two): values are accurate to about 6%.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include "meridian/order_book.hpp"
#include "meridian/timing.hpp"

using namespace meridian;
using namespace meridian::book;

namespace {

struct Hist {
    static constexpr int kGroups = 44, kSub = 16;
    uint64_t c[kGroups * kSub] = {};
    uint64_t n = 0, max = 0;
    void add(uint64_t ns) {
        ++n;
        if (ns > max) max = ns;
        const uint64_t v = ns + 1;
        const int g = 63 - __builtin_clzll(v);
        const int sub = g >= 4 ? (int)((v >> (g - 4)) & 15) : (int)((v << (4 - g)) & 15);
        c[std::min(g, kGroups - 1) * kSub + sub]++;
    }
    uint64_t pct(double p) const {                       // lower edge of the bucket holding the p-th percentile
        if (!n) return 0;
        const uint64_t want = (uint64_t)((double)n * p / 100.0);
        uint64_t seen = 0;
        for (int i = 0; i < kGroups * kSub; ++i) {
            seen += c[i];
            if (seen > want) {
                const int g = i / kSub, sub = i % kSub;
                const uint64_t base = g >= 4 ? ((16ull + sub) << (g - 4)) : ((16ull + sub) >> (4 - g));
                return base - 1;
            }
        }
        return max;
    }
    void clear() { std::memset(c, 0, sizeof c); n = 0; max = 0; }
};

}  // namespace

int main(int argc, char** argv) {
    unsigned seconds = 10;
    uint64_t seed = 1;
    bool mix = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--mix")) mix = true;
        else { std::fprintf(stderr, "usage: live [--seconds S] [--seed N] [--mix]\n"); return 2; }
    }

    const double ghz = calibrate_tsc_ghz();
    uint64_t overhead = ~0ull;                           // cost of an empty begin/end pair, median of many
    {
        std::vector<uint64_t> d(20001);
        for (auto& x : d) { const uint64_t a = rdtsc_begin(); const uint64_t b = rdtsc_end(); x = b - a; }
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        overhead = d[d.size() / 2];
    }
    auto to_ns = [&](uint64_t cycles) { return (uint64_t)((double)(cycles > overhead ? cycles - overhead : 0) / ghz); };

    constexpr uint32_t kMaxPrice = 1000;
    OrderBook book(1u << 20, kMaxPrice);
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> live;
    live.reserve(1u << 20);
    std::vector<Fill> fills;
    fills.reserve(256);
    Hist win, all_add, all_match, all_cancel, all_modify;
    uint64_t ops = 0, trades = 0, volume = 0, rejected = 0, id = 0;
    uint32_t mid = 500;

    std::printf("Meridian live view: TSC %.3f GHz, timer overhead %llu cycles subtracted. %us, seed %llu%s.\n", ghz,
                (unsigned long long)overhead, seconds, (unsigned long long)seed, mix ? ", mixed order types" : "");
    std::printf("Latency = time inside the engine call (service time), one thread, as fast as it can go.\n\n");
    std::printf("%4s %10s %9s %8s %8s %9s %9s %6s %6s %8s\n", "sec", "ops/s", "trades/s", "p50 ns", "p99 ns", "p99.9 ns", "max ns", "bid", "ask", "resting");

    using clk = std::chrono::steady_clock;
    const auto start = clk::now();
    auto next_report = start + std::chrono::seconds(1);
    unsigned sec = 0;
    uint64_t w_ops = 0, w_trades = 0;
    for (;;) {
        for (int batch = 0; batch < 4096; ++batch) {
            fills.clear();
            if ((rng() & 127) == 0) mid = std::clamp<int>((int)mid + ((rng() & 1) ? 1 : -1), 450, 550);   // slow random walk
            const unsigned roll = (unsigned)(rng() % 100);
            uint64_t t0, t1;
            if (live.size() > 150000 || (!live.empty() && roll < 22)) {                 // cancel
                const size_t k = rng() % live.size();
                const uint64_t victim = live[k];
                live[k] = live.back();
                live.pop_back();
                t0 = rdtsc_begin(); (void)book.cancel(victim); t1 = rdtsc_end();
                all_cancel.add(to_ns(t1 - t0));
                win.add(to_ns(t1 - t0));
            } else if (mix && !live.empty() && roll < 30) {                              // modify
                const uint64_t target = live[rng() % live.size()];
                const uint32_t price = mid - 6 + (uint32_t)(rng() % 13);
                const uint32_t qty = 1 + (uint32_t)(rng() % 100);
                t0 = rdtsc_begin(); (void)book.modify(target, price, qty, fills); t1 = rdtsc_end();
                all_modify.add(to_ns(t1 - t0));
                win.add(to_ns(t1 - t0));
            } else {                                                                     // new order
                const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
                const uint32_t price = mid - 5 + (uint32_t)(rng() % 11);
                const uint32_t qty = 1 + (uint32_t)(rng() % 100);
                const uint64_t oid = ++id;
                ExecReport rep;
                unsigned kind = 0;                                                       // 0 GTC, 1 IOC, 2 FOK, 3 market
                if (mix) { const unsigned k = (unsigned)(rng() % 100); kind = k < 70 ? 0 : k < 82 ? 1 : k < 92 ? 2 : 3; }
                t0 = rdtsc_begin();
                if (kind == 3) rep = book.submit_market(oid, side, qty, fills);
                else rep = book.submit(oid, side, kind == 0 ? TimeInForce::GTC : kind == 1 ? TimeInForce::IOC : TimeInForce::FOK, price, qty, fills);
                t1 = rdtsc_end();
                if (rep.status != AddResult::Ok) ++rejected;
                else if (rep.rested) live.push_back(oid);
                (fills.empty() ? all_add : all_match).add(to_ns(t1 - t0));
                win.add(to_ns(t1 - t0));
            }
            ++ops; ++w_ops;
            for (const Fill& f : fills) { ++trades; ++w_trades; volume += f.qty; }
        }
        const auto now = clk::now();
        if (now >= next_report) {
            ++sec;
            const double dt = std::chrono::duration<double>(now - (next_report - std::chrono::seconds(1))).count();
            std::printf("%4u %10.0f %9.0f %8llu %8llu %9llu %9llu %6u %6u %8zu\n", sec, (double)w_ops / dt, (double)w_trades / dt,
                        (unsigned long long)win.pct(50), (unsigned long long)win.pct(99), (unsigned long long)win.pct(99.9),
                        (unsigned long long)win.max, book.best_bid(), book.best_ask(), book.size());
            std::fflush(stdout);
            win.clear(); w_ops = 0; w_trades = 0;
            next_report += std::chrono::seconds(1);
            if (sec >= seconds) break;
        }
    }

    std::printf("\nWhole run: %llu operations, %llu trades, volume %llu, rejected %llu\n", (unsigned long long)ops, (unsigned long long)trades,
                (unsigned long long)volume, (unsigned long long)rejected);
    std::printf("%-26s %12s %8s %8s %9s %9s\n", "operation", "count", "p50 ns", "p99 ns", "p99.9 ns", "max ns");
    auto row = [&](const char* name, const Hist& h) {
        std::printf("%-26s %12llu %8llu %8llu %9llu %9llu\n", name, (unsigned long long)h.n, (unsigned long long)h.pct(50),
                    (unsigned long long)h.pct(99), (unsigned long long)h.pct(99.9), (unsigned long long)h.max);
    };
    row("new order, no trade", all_add);
    row("new order, traded", all_match);
    row("cancel", all_cancel);
    if (mix) row("modify", all_modify);
    std::printf("\nNote: WSL2 timing, one run. Not a claim. For numbers you can quote, run the benchmarks in bench/ at least 3 times.\n");
    return 0;
}
