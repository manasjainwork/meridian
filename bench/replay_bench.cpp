#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
#include "meridian/journal.hpp"
#include "meridian/replay.hpp"
#include "meridian/timing.hpp"

using namespace meridian;
using namespace meridian::book;
using namespace meridian::journal;

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main() {
    constexpr size_t N = 400000;
    constexpr uint32_t MAXP = 2048, POOL = 1u << 17;
    const double ghz = calibrate_tsc_ghz();
    Journal j(N);
    std::vector<uint64_t> t_append;
    t_append.reserve(N);
    uint64_t next_id = 1;
    std::vector<uint64_t> ids;
    // Timed: only the journal append (what the engine pays per input event).
    for (size_t i = 0; i < N; ++i) {
        const bool cancel = !ids.empty() && rnd() % 100 < 30;
        uint64_t id = 0; Side s = Side::Buy; uint32_t p = 0, q = 0;
        if (cancel) { const size_t k = rnd() % ids.size(); id = ids[k]; ids[k] = ids.back(); ids.pop_back(); }
        else { s = (rnd() & 1) ? Side::Buy : Side::Sell; p = 1000 + static_cast<uint32_t>(rnd() % 41) - 20 + (s == Side::Buy ? -2 : 2); q = 1 + static_cast<uint32_t>(rnd() % 100); id = next_id++; ids.push_back(id); }
        const uint64_t a = rdtsc_begin();
        if (cancel) j.append_cancel(id); else j.append_add(id, s, p, q);
        const uint64_t b = rdtsc_end();
        t_append.push_back(b - a > 32 ? b - a - 32 : 0);
    }
    std::sort(t_append.begin(), t_append.end());
    auto pct = [&](double p) { return t_append[static_cast<size_t>(p * (t_append.size() - 1))] / ghz; };
    std::printf("journal append  n=%zu  min %.1f  p50 %.1f  p99 %.1f  p99.9 %.1f  max %.1f ns\n", t_append.size(), pct(0), pct(0.5), pct(0.99), pct(0.999), pct(1));

    uint64_t first = 0;
    for (int run = 0; run < 3; ++run) {
        OrderBook b(POOL, MAXP);
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t dig = replay(j.records(), b, MAXP);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (run == 0) first = dig;
        std::printf("replay run %d: %.1f M events/s  digest %016llx  %s\n", run, static_cast<double>(N) / s / 1e6,
                    static_cast<unsigned long long>(dig), dig == first ? "identical" : "DIFFERENT");
    }
    const auto t0 = std::chrono::steady_clock::now();
    const bool saved = j.save("/tmp/meridian_bench.journal");
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("save %zu records (%.1f MB): %s in %.1f ms (page cache, no fsync)\n", j.size(), static_cast<double>(j.size() * 32) / 1e6, saved ? "ok" : "FAILED", s * 1e3);
}
