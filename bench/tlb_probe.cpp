#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <vector>
#include "meridian/timing.hpp"

using namespace meridian;

static void run(size_t bytes, bool huge, double ghz) {
    constexpr size_t N = 200000, ALIGN = 2u << 20;
    const size_t sz = (bytes + ALIGN - 1) / ALIGN * ALIGN;
    auto* a = static_cast<uint64_t*>(std::aligned_alloc(ALIGN, sz));
    madvise(a, sz, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    const size_t n = sz / 8;
    for (size_t i = 0; i < n; ++i) a[i] = i;               // touch every page
    uint64_t x = 88172645463325252ULL, sink = 0;
    std::vector<uint64_t> d;
    d.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        const uint64_t t0 = rdtsc_begin();
        sink += a[x % n];
        const uint64_t t1 = rdtsc_end();
        d.push_back(t1 - t0 > 32 ? t1 - t0 - 32 : 0);
    }
    std::sort(d.begin(), d.end());
    auto pct = [&](double p) { return d[static_cast<size_t>(p * (d.size() - 1))] / ghz; };
    std::printf("%6zu KB %-5s p50 %7.1f  p99 %7.1f  p99.9 %7.1f   (sink %llu)\n", sz / 1024,
                huge ? "huge" : "4k", pct(0.5), pct(0.99), pct(0.999), static_cast<unsigned long long>(sink & 1));
    std::free(a);
}

int main() {
    const double ghz = calibrate_tsc_ghz();
    for (size_t kb : {2048, 8192, 32768, 131072, 524288}) {
        run(kb * 1024, false, ghz);
        run(kb * 1024, true, ghz);
    }
}
