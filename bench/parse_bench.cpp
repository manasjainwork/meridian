#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include "meridian/protocol.hpp"
#include "meridian/timing.hpp"

using namespace meridian;
using namespace meridian::proto;

int main() {
    constexpr size_t N = 100000;
    std::vector<uint8_t> stream(N * sizeof(NewOrder));
    for (size_t i = 0; i < N; ++i) {
        const NewOrder m = make_new_order(i + 1, i, 7, (i & 1) ? Side::Sell : Side::Buy, 10000 + i % 100, 100);
        std::memcpy(stream.data() + i * sizeof(NewOrder), &m, sizeof m);
    }
    const double ghz = calibrate_tsc_ghz();
    std::vector<uint64_t> d(N);
    uint64_t sink = 0;

    for (int pass = 0; pass < 2; ++pass) {  // pass 0 warms caches, pass 1 is recorded
        for (size_t i = 0; i < N; ++i) {
            const uint8_t* p = stream.data() + i * sizeof(NewOrder);
            const uint64_t a = rdtsc_begin();
            const auto r = parse_one(p, sizeof(NewOrder));
            const auto* o = as<NewOrder>(r.header);
            sink += o->price_ticks.get() + o->qty.get();
            const uint64_t b = rdtsc_end();
            d[i] = b - a;
        }
    }
    std::sort(d.begin(), d.end());
    auto pct = [&](double p) { return d[static_cast<size_t>(p * (N - 1))]; };
    std::printf("parse+read NewOrder, %zu msgs, TSC %.3f GHz (timer overhead ~32 cycles included)\n", N, ghz);
    const double ps[] = {0.0, 0.5, 0.99, 0.999, 1.0};
    const char* names[] = {"min", "p50", "p99", "p99.9", "max"};
    for (int i = 0; i < 5; ++i) {
        const uint64_t c = pct(ps[i]);
        std::printf("%-6s %6llu cycles %8.1f ns\n", names[i], static_cast<unsigned long long>(c), c / ghz);
    }
    std::printf("(checksum %llu)\n", static_cast<unsigned long long>(sink));
}
