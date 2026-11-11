// Allocation count, per-op time and memory growth: the bare OrderBook against the Oms around it.
// Same deterministic stream of 70% limit adds and 30% cancels, 200k operations, single thread.
// Counts every call to operator new inside the timed loop and the bytes still live afterwards.
// This is service time, not latency under load. On WSL2 the numbers are observations, not a benchmark.
// Build with -Wno-mismatched-new-delete -Wno-free-nonheap-object (GCC reports false positives for a counting operator new in system headers).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
#include "meridian/oms.hpp"
#include "meridian/timing.hpp"

static uint64_t g_allocs = 0, g_live = 0;
void* operator new(size_t n) {
    void* p = std::malloc(n + 16);
    if (!p) throw std::bad_alloc();
    *static_cast<size_t*>(p) = n; ++g_allocs; g_live += n;
    return static_cast<char*>(p) + 16;
}
void operator delete(void* p) noexcept {
    if (!p) return;
    char* b = static_cast<char*>(p) - 16;
    g_live -= *reinterpret_cast<size_t*>(b);
    std::free(b);
}
void operator delete(void* p, size_t) noexcept { operator delete(p); }

using namespace meridian;
struct Lcg { uint64_t s; uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>(s >> 33); } };
struct Op { bool add; bool buy; uint32_t price, qty, pick; };

static void report(const char* name, std::vector<uint32_t>& cyc, double ghz, uint64_t allocs, int64_t live_growth, size_t ops) {
    std::sort(cyc.begin(), cyc.end());
    auto q = [&](double f) { return cyc[static_cast<size_t>(f * (cyc.size() - 1))] / ghz; };
    std::printf("%-6s ops %zu  ns p50 %.0f p99 %.0f p99.9 %.0f max %.0f | allocations %llu (%.3f per op) | live bytes grew %lld (%.1f per op)\n",
                name, ops, q(0.5), q(0.99), q(0.999), cyc.back() / ghz, (unsigned long long)allocs, double(allocs) / ops,
                (long long)live_growth, double(live_growth) / ops);
}

int main() {
    const size_t N = 200000;
    std::vector<Op> ops(N);
    Lcg g{12345};
    for (auto& o : ops) { o.add = g.next() % 10 < 7; o.buy = g.next() & 1; o.price = 480 + g.next() % 40; o.qty = 1 + g.next() % 50; o.pick = g.next(); }
    const double ghz = calibrate_tsc_ghz();
    std::printf("TSC %.3f GHz; cancels target a random order that was added earlier (it may already be filled)\n", ghz);

    {   // bare book
        book::OrderBook b(1u << 18, 1000);
        std::vector<book::Fill> fills; fills.reserve(1024);
        std::vector<uint64_t> live; live.reserve(1u << 18);
        std::vector<uint32_t> cyc; cyc.reserve(N);
        uint64_t id = 0;
        const uint64_t a0 = g_allocs; const uint64_t l0 = g_live;
        for (const Op& o : ops) {
            fills.clear();
            if (o.add || live.empty()) {
                ++id;
                const uint64_t t0 = rdtsc_begin();
                const auto r = b.submit(id, o.buy ? book::Side::Buy : book::Side::Sell, book::TimeInForce::GTC, o.price, o.qty, fills);
                const uint64_t t1 = rdtsc_end();
                cyc.push_back(static_cast<uint32_t>(t1 - t0));
                if (r.rested > 0) live.push_back(id);
            } else {
                const size_t i = o.pick % live.size();
                const uint64_t target = live[i]; live[i] = live.back(); live.pop_back();
                const uint64_t t0 = rdtsc_begin();
                (void)b.cancel(target);
                const uint64_t t1 = rdtsc_end();
                cyc.push_back(static_cast<uint32_t>(t1 - t0));
            }
        }
        report("book", cyc, ghz, g_allocs - a0, int64_t(g_live) - int64_t(l0), N);
    }
    {   // Oms around the same book
        oms::Config c; c.max_orders = 1u << 18; c.max_position = int64_t{1} << 40;
        oms::Oms o(c);
        std::vector<uint64_t> live; live.reserve(1u << 18);
        std::vector<uint32_t> cyc; cyc.reserve(N);
        uint64_t cl = 0;
        const uint64_t a0 = g_allocs; const uint64_t l0 = g_live;
        for (const Op& op : ops) {
            const uint32_t acct = op.buy ? 1 : 2;   // two accounts so crossing orders are not self-trades
            if (op.add || live.empty()) {
                const oms::Request rq{acct, ++cl, op.buy ? book::Side::Buy : book::Side::Sell, oms::Type::Limit, book::TimeInForce::GTC, op.price, op.qty, 0};
                const uint64_t t0 = rdtsc_begin();
                const oms::Result r = o.submit(rq);
                const uint64_t t1 = rdtsc_end();
                cyc.push_back(static_cast<uint32_t>(t1 - t0));
                if (r.status == oms::Status::Open && r.order_id) live.push_back(r.order_id);
            } else {
                const size_t i = op.pick % live.size();
                const uint64_t target = live[i]; live[i] = live.back(); live.pop_back();
                const oms::OrderRec* rec = o.order(target);
                const uint32_t owner = rec ? rec->account : 1;
                const uint64_t t0 = rdtsc_begin();
                (void)o.cancel(owner, target);
                const uint64_t t1 = rdtsc_end();
                cyc.push_back(static_cast<uint32_t>(t1 - t0));
            }
        }
        report("oms", cyc, ghz, g_allocs - a0, int64_t(g_live) - int64_t(l0), N);
        std::string why; std::printf("oms invariants ok: %d\n", o.check(&why) ? 1 : 0);
    }
    return 0;
}
