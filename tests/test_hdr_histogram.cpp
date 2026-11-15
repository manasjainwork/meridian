// Histogram (design M4): exact for small values, within 2^-7 of the true value for large ones, percentiles never below the true value,
// min and max exact, merging two histograms equals recording everything in one.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
#include "meridian/hdr_histogram.hpp"

using namespace meridian;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    {   // empty
        Histogram<> h;
        CHECK(h.count() == 0 && h.percentile(50) == 0 && h.min() == 0 && h.max() == 0);
    }
    {   // small values are exact
        Histogram<> h;
        for (uint64_t v = 0; v < 256; ++v) h.record(v);
        CHECK(h.count() == 256 && h.min() == 0 && h.max() == 255);
        CHECK(h.percentile(100) == 255 && h.percentile(50) == 127 && h.percentile(0) == 0);
    }
    {   // against a sorted copy of the same data, for several shapes
        std::mt19937_64 g(11);
        for (int shape = 0; shape < 3; ++shape) {
            Histogram<> h;
            std::vector<uint64_t> v;
            for (int i = 0; i < 200000; ++i) {
                uint64_t x;
                if (shape == 0) x = g() % 1000;
                else if (shape == 1) x = uint64_t(std::exp(std::normal_distribution<double>(8.0, 2.0)(g)));
                else x = (g() % 50 == 0) ? (g() >> (g() % 40)) : 500 + g() % 200;
                v.push_back(x); h.record(x);
            }
            std::sort(v.begin(), v.end());
            CHECK(h.count() == v.size() && h.min() == v.front() && h.max() == v.back());
            bool ok = true, mono = true;
            uint64_t prev = 0;
            for (double p : {0.0, 1.0, 25.0, 50.0, 90.0, 99.0, 99.9, 99.99, 100.0}) {
                const uint64_t truth = v[Histogram<>::rank_of(v.size(), p) - 1];
                const uint64_t got = h.percentile(p);
                ok = ok && got >= truth && got - truth <= truth / 128 + 1;      // never low; high by under 2^-7
                mono = mono && got >= prev;
                prev = got;
            }
            CHECK(ok);
            CHECK(mono);
        }
    }
    {   // extreme values do not overflow the table
        Histogram<> h;
        h.record(0); h.record(~uint64_t(0)); h.record(uint64_t(1) << 63);
        CHECK(h.count() == 3 && h.max() == ~uint64_t(0) && h.percentile(100) == ~uint64_t(0));
    }
    {   // merge equals one histogram
        std::mt19937_64 g(5);
        Histogram<> a, b, all;
        for (int i = 0; i < 50000; ++i) { const uint64_t x = g() % 100000; (i % 3 ? a : b).record(x); all.record(x); }
        a.add(b);
        bool same = a.count() == all.count() && a.min() == all.min() && a.max() == all.max();
        for (double p : {50.0, 99.0, 99.9, 100.0}) same = same && a.percentile(p) == all.percentile(p);
        CHECK(same);
    }
    std::printf("%s\n", g_fail ? "FAILED" : "hdr_histogram ok");
    return g_fail ? 1 : 0;
}
