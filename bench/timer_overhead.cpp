#include <algorithm>
#include <cstdio>
#include <vector>
#include "meridian/timing.hpp"

int main() {
    using namespace meridian;
    constexpr size_t N = 1'000'000;
    const double ghz = calibrate_tsc_ghz();

    for (int i = 0; i < 10000; ++i) {  // warmup
        volatile uint64_t a = rdtsc_begin();
        volatile uint64_t b = rdtsc_end();
        (void)a; (void)b;
    }

    std::vector<uint64_t> d(N);
    for (size_t i = 0; i < N; ++i) {
        const uint64_t a = rdtsc_begin();
        const uint64_t b = rdtsc_end();
        d[i] = b - a;
    }
    std::sort(d.begin(), d.end());

    auto pct = [&](double p) { return d[static_cast<size_t>(p * (N - 1))]; };
    std::printf("TSC: %.3f GHz, samples: %zu\n", ghz, N);
    std::printf("%-8s %8s %10s\n", "pct", "cycles", "ns");
    const double ps[] = {0.0, 0.5, 0.99, 0.999, 1.0};
    const char* names[] = {"min", "p50", "p99", "p99.9", "max"};
    for (int i = 0; i < 5; ++i) {
        const uint64_t c = pct(ps[i]);
        std::printf("%-8s %8llu %10.1f\n", names[i], static_cast<unsigned long long>(c), c / ghz);
    }
}
