#include <algorithm>
#include <cstdio>
#include <vector>
#include "meridian/timing.hpp"

using namespace meridian;

static void rep(const char* name, std::vector<uint64_t>& d, double ghz) {
    std::sort(d.begin(), d.end());
    auto pct = [&](double p) { return d[static_cast<size_t>(p * (d.size() - 1))]; };
    std::printf("%-14s n=%-7zu", name, d.size());
    for (double p : {0.0, 0.5, 0.99, 0.999, 1.0}) std::printf(" %9.1f", pct(p) / ghz);
    std::printf("   (min p50 p99 p99.9 max, ns)\n");
}

int main() {
    constexpr size_t N = 400000;
    const double ghz = calibrate_tsc_ghz();
    std::vector<uint64_t> empty, cached, cold;
    empty.reserve(N); cached.reserve(N); cold.reserve(N);

    for (size_t i = 0; i < N; ++i) {                      // nothing between the timers
        const uint64_t a = rdtsc_begin();
        const uint64_t b = rdtsc_end();
        empty.push_back(b - a > 32 ? b - a - 32 : 0);
    }
    volatile uint64_t small[8] = {1, 2, 3, 4, 5, 6, 7, 8};  // one L1-resident load
    uint64_t sink = 0;
    for (size_t i = 0; i < N; ++i) {
        const uint64_t a = rdtsc_begin();
        sink += small[i & 7];
        const uint64_t b = rdtsc_end();
        cached.push_back(b - a > 32 ? b - a - 32 : 0);
    }
    std::vector<uint64_t> big(1u << 24, 1);                 // 128 MB, random loads miss cache
    uint64_t x = 88172645463325252ULL;
    for (size_t i = 0; i < N; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        const uint64_t a = rdtsc_begin();
        sink += big[x & (big.size() - 1)];
        const uint64_t b = rdtsc_end();
        cold.push_back(b - a > 32 ? b - a - 32 : 0);
    }
    std::printf("TSC %.3f GHz (sink %llu)\n", ghz, static_cast<unsigned long long>(sink));
    rep("empty", empty, ghz);
    rep("L1 load", cached, ghz);
    rep("DRAM load", cold, ghz);
}
