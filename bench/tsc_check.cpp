// TSC check (sequencer design M4): can timestamps taken on different cores be compared? Run before any per-order timing.
// 1. CPU flags and clock source. 2. Calibration repeated: does the TSC rate stay the same? 3. For each pair of cores, an NTP-style
// ping-pong estimates the offset between their TSCs, with its uncertainty (half the best round trip), twice, one second apart.
#include <pthread.h>
#include <sched.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "meridian/timing.hpp"

using namespace meridian;

static bool pin(int cpu) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); return pthread_setaffinity_np(pthread_self(), sizeof s, &s) == 0; }

static std::string first_line(const char* path) { std::ifstream f(path); std::string l; std::getline(f, l); return l; }

static void flags() {
    std::ifstream f("/proc/cpuinfo");
    std::string l, fl;
    while (std::getline(f, l)) if (l.rfind("flags", 0) == 0) { fl = l; break; }
    for (const char* k : {"constant_tsc", "nonstop_tsc", "tsc_reliable", "tsc_known_freq", "rdtscp", "hypervisor"})
        std::printf("  flag %-15s %s\n", k, fl.find(std::string(" ") + k) != std::string::npos ? "yes" : "no");
    std::printf("  clocksource: %s\n", first_line("/sys/devices/system/clocksource/clocksource0/current_clocksource").c_str());
}

struct Est { double offset_ticks; double rtt_ticks; };

// Offset of core b's TSC relative to core a's (positive: b is ahead), taken from the iteration with the smallest round trip.
static bool estimate(int a, int b, int iters, Est& out) {
    alignas(64) std::atomic<uint64_t> flag_a{0};
    alignas(64) std::atomic<uint64_t> flag_b{0};
    alignas(64) uint64_t t2 = 0, t3 = 0;
    bool ok_b = true;
    std::thread tb([&] {
        if (!pin(b)) { ok_b = false; flag_b.store(~0ull); return; }
        for (int i = 0; i < iters; ++i) {
            while (flag_a.load(std::memory_order_acquire) != uint64_t(i) + 1) {}
            t2 = rdtsc_begin();
            t3 = rdtsc_end();
            flag_b.store(uint64_t(i) + 1, std::memory_order_release);
        }
    });
    bool ok_a = pin(a);
    double best_rtt = 1e300, best_off = 0;
    for (int i = 0; i < iters && ok_a && ok_b; ++i) {
        const uint64_t t1 = rdtsc_begin();
        flag_a.store(uint64_t(i) + 1, std::memory_order_release);
        uint64_t v;
        while ((v = flag_b.load(std::memory_order_acquire)) != uint64_t(i) + 1) if (v == ~0ull) { ok_b = false; break; }
        if (!ok_b) break;
        const uint64_t t4 = rdtsc_end();
        const double d1 = double(int64_t(t2 - t1)), d2 = double(int64_t(t3 - t4));
        const double rtt = double(int64_t(t4 - t1)) - double(int64_t(t3 - t2));
        if (rtt > 0 && rtt < best_rtt) { best_rtt = rtt; best_off = (d1 + d2) / 2; }
    }
    if (!ok_a || !ok_b) flag_a.store(0);
    tb.join();
    if (!ok_a || !ok_b) return false;
    out = {best_off, best_rtt};
    return true;
}

int main() {
    std::printf("TSC check\n");
    flags();

    std::printf("calibration (200 ms windows against steady_clock):\n");
    std::vector<double> g;
    for (int i = 0; i < 5; ++i) { g.push_back(calibrate_tsc_ghz(200)); std::printf("  run %d: %.6f GHz\n", i + 1, g.back()); }
    double lo = g[0], hi = g[0], sum = 0;
    for (double x : g) { lo = std::min(lo, x); hi = std::max(hi, x); sum += x; }
    const double ghz = sum / g.size();
    std::printf("  spread %.1f ppm of the mean\n", (hi - lo) / ghz * 1e6);

    cpu_set_t set; CPU_ZERO(&set);
    sched_getaffinity(0, sizeof set, &set);
    std::vector<int> cores;
    for (int c = 0; c < CPU_SETSIZE; ++c) if (CPU_ISSET(c, &set)) cores.push_back(c);
    std::printf("cores available: %zu\n", cores.size());
    if (cores.size() < 2) { std::printf("fewer than 2 cores: cross-core check not possible\n"); return 0; }

    int bad = 0, total = 0;
    std::printf("cross-core offset (core b minus core a), best of 20000 round trips; time = ticks / %.4f GHz\n", ghz);
    for (size_t k = 1; k < cores.size() && k <= 3; ++k) {
        Est e1{}, e2{};
        if (!estimate(cores[0], cores[k], 20000, e1)) { std::printf("  cores %d,%d: could not pin\n", cores[0], cores[k]); continue; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!estimate(cores[0], cores[k], 20000, e2)) { std::printf("  cores %d,%d: could not pin\n", cores[0], cores[k]); continue; }
        const double unc1 = e1.rtt_ticks / 2 / ghz, unc2 = e2.rtt_ticks / 2 / ghz;
        const double o1 = e1.offset_ticks / ghz, o2 = e2.offset_ticks / ghz;
        const bool zero = std::fabs(o1) <= unc1 && std::fabs(o2) <= unc2;
        ++total; if (!zero) ++bad;
        std::printf("  cores %d,%d: offset %+.0f ns (+/- %.0f), after 1 s %+.0f ns (+/- %.0f), change %+.0f ns/s -> %s\n",
                    cores[0], cores[k], o1, unc1, o2, unc2, o2 - o1, zero ? "consistent with zero offset" : "OFFSET NOT ZERO");
    }
    std::printf("summary: %d of %d core pairs consistent with zero offset (uncertainty = half the best round trip); %s\n", total - bad, total,
                bad == 0 && total > 0 ? "cross-core timestamps may be compared to within that uncertainty" : "do not compare timestamps across cores here");
    return 0;
}
