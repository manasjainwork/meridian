// Ingress -> SPSC -> engine (OrderBook) -> SPSC -> egress, three pinned threads.
// Open loop: ingress sends at a fixed rate and each order's latency is measured from the time it
// was SUPPOSED to be sent, not from when it was actually pushed. If the engine falls behind, the
// queueing delay shows up in the numbers (no coordinated omission).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>
#include "meridian/order_book.hpp"
#include "meridian/spsc_queue.hpp"
#include "meridian/timing.hpp"

using namespace meridian;
using namespace meridian::book;

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

struct Op { uint64_t id; Side side; uint32_t price, qty; bool cancel; };
struct Msg { Op op; uint64_t t_intended; };
struct Done { uint64_t t_intended; uint64_t nfills; };

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

static std::vector<Op> make_ops(size_t n) {
    constexpr uint32_t MID = 1000;
    std::vector<Op> ops;
    ops.reserve(n);
    uint64_t next_id = 1;
    std::vector<uint64_t> live;
    for (size_t i = 0; i < n; ++i) {
        if (!live.empty() && rnd() % 100 < 30) {
            const size_t k = rnd() % live.size();
            ops.push_back({live[k], Side::Buy, 0, 0, true});
            live[k] = live.back(); live.pop_back();
        } else {
            const Side s = (rnd() & 1) ? Side::Buy : Side::Sell;
            const int64_t off = static_cast<int64_t>(rnd() % 40) - 20;
            const uint32_t p = static_cast<uint32_t>(MID + off + (s == Side::Buy ? -2 : 2));
            ops.push_back({next_id, s, p, 1 + static_cast<uint32_t>(rnd() % 100), false});
            live.push_back(next_id++);
        }
    }
    return ops;
}

static uint64_t apply(OrderBook& book, const Op& o, std::vector<Fill>& fills, uint64_t& nfills) {
    fills.clear();
    uint64_t r;
    if (o.cancel) r = book.cancel(o.id);
    else r = static_cast<uint64_t>(book.add_limit(o.id, o.side, o.price, o.qty, fills));
    nfills += fills.size();
    return r;
}

static void run(const std::vector<Op>& ops, double rate, double ghz, int ca, int cb, int cc,
                uint64_t ref_fills, uint64_t ref_sink) {
    const size_t N = ops.size();
    static SpscQueue<Msg, 4096> q_in;
    static SpscQueue<Done, 4096> q_out;
    OrderBook book(1u << 17, 2048);
    std::vector<uint64_t> lat(N);
    uint64_t eng_sink = 0, eng_fills = 0;
    const double period = ghz * 1e9 / rate;                 // TSC cycles between orders
    const uint64_t t0 = __rdtsc() + static_cast<uint64_t>(ghz * 1e8);   // start 100 ms from now

    std::thread eng([&] {
        pin(cb);
        std::vector<Fill> fills; fills.reserve(64);
        Msg m; Done d;
        for (size_t i = 0; i < N; ++i) {
            while (!q_in.try_pop(m)) {}
            uint64_t nf = 0;
            eng_sink += apply(book, m.op, fills, nf);
            eng_fills += nf;
            d = {m.t_intended, nf};
            while (!q_out.try_push(d)) {}
        }
    });
    std::thread egr([&] {
        pin(cc);
        Done d;
        for (size_t i = 0; i < N; ++i) {
            while (!q_out.try_pop(d)) {}
            lat[i] = rdtsc_end() - d.t_intended;
        }
    });
    pin(ca);
    for (size_t i = 0; i < N; ++i) {
        const uint64_t due = t0 + static_cast<uint64_t>(period * static_cast<double>(i));
        while (__rdtsc() < due) {}
        Msg m{ops[i], due};
        while (!q_in.try_push(m)) {}
    }
    eng.join(); egr.join();

    const size_t warm = 10000;
    std::vector<uint64_t> d(lat.begin() + warm, lat.end());
    std::sort(d.begin(), d.end());
    auto pct = [&](double p) { return d[static_cast<size_t>(p * (d.size() - 1))] / ghz; };
    const bool ok = eng_fills == ref_fills && eng_sink == ref_sink;
    std::printf("%8.0f ops/s  n=%zu  p50 %8.0f  p99 %9.0f  p99.9 %10.0f  max %11.0f ns   %s\n", rate, d.size(),
                pct(0.5), pct(0.99), pct(0.999), pct(1), ok ? "result == single-thread" : "RESULT MISMATCH");
}

int main(int argc, char** argv) {
    const int ca = argc > 1 ? std::atoi(argv[1]) : 0, cb = argc > 2 ? std::atoi(argv[2]) : 2, cc = argc > 3 ? std::atoi(argv[3]) : 4;
    const double ghz = calibrate_tsc_ghz();
    const std::vector<Op> ops = make_ops(400000);

    uint64_t ref_fills = 0, ref_sink = 0;                   // single-thread reference result
    {
        OrderBook book(1u << 17, 2048);
        std::vector<Fill> fills; fills.reserve(64);
        for (const Op& o : ops) ref_sink += apply(book, o, fills, ref_fills);
    }
    std::printf("ingress cpu %d, engine cpu %d, egress cpu %d, TSC %.3f GHz, reference fills %llu\n", ca, cb, cc, ghz,
                static_cast<unsigned long long>(ref_fills));
    std::printf("latency = intended send time -> egress receives the ack\n");
    for (double rate : {500e3, 2e6, 4e6, 8e6, 12e6, 16e6}) run(ops, rate, ghz, ca, cb, cc, ref_fills, ref_sink);
}
