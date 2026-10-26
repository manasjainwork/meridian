#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include "meridian/spsc_queue.hpp"
#include "meridian/timing.hpp"

using namespace meridian;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

struct MutexQueue {
    bool try_push(uint64_t v) { std::lock_guard<std::mutex> g(m); if (d.size() >= 1024) return false; d.push_back(v); return true; }
    bool try_pop(uint64_t& v) { std::lock_guard<std::mutex> g(m); if (d.empty()) return false; v = d.front(); d.pop_front(); return true; }
    std::mutex m; std::deque<uint64_t> d;
};

template <typename Q>
static double throughput(Q& q, uint64_t count, int cpu_a, int cpu_b) {
    uint64_t sum = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread prod([&] { pin(cpu_a); for (uint64_t i = 1; i <= count; ++i) while (!q.try_push(i)) {} });
    std::thread cons([&] { pin(cpu_b); uint64_t v, n = 0; while (n < count) if (q.try_pop(v)) { sum += v; ++n; } });
    prod.join(); cons.join();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (sum != count * (count + 1) / 2) std::printf("CHECKSUM MISMATCH\n");
    return static_cast<double>(count) / s / 1e6;
}

// Ping-pong: A sends i, B echoes it back. Round trip = 2 one-way hops, includes both cores' cache transfers.
template <typename Q>
static void pingpong(const char* name, double ghz, int cpu_a, int cpu_b) {
    constexpr uint64_t N = 200000;
    static Q ab, ba;
    std::vector<uint64_t> d;
    d.reserve(N);
    std::thread echo([&] { pin(cpu_b); uint64_t v; for (uint64_t i = 0; i < N + 1000; ++i) { while (!ab.try_pop(v)) {} while (!ba.try_push(v)) {} } });
    pin(cpu_a);
    uint64_t v;
    for (uint64_t i = 0; i < N + 1000; ++i) {
        const uint64_t t0 = rdtsc_begin();
        while (!ab.try_push(i)) {}
        while (!ba.try_pop(v)) {}
        const uint64_t t1 = rdtsc_end();
        if (i >= 1000) d.push_back(t1 - t0 > 32 ? t1 - t0 - 32 : 0);   // first 1000 are warm-up
    }
    echo.join();
    std::sort(d.begin(), d.end());
    auto pct = [&](double p) { return d[static_cast<size_t>(p * (d.size() - 1))] / ghz; };
    std::printf("%-8s RTT  min %7.1f  p50 %7.1f  p99 %8.1f  p99.9 %9.1f  max %10.1f ns\n", name, pct(0), pct(0.5), pct(0.99), pct(0.999), pct(1));
}

int main(int argc, char** argv) {
    const int a = argc > 1 ? std::atoi(argv[1]) : 0, b = argc > 2 ? std::atoi(argv[2]) : 1;
    std::printf("producer cpu %d, consumer cpu %d, hw threads %u\n", a, b, std::thread::hardware_concurrency());
    const double ghz = calibrate_tsc_ghz();
    constexpr uint64_t COUNT = 5'000'000;
    { static SpscQueue<uint64_t, 1024> q; std::printf("throughput spsc  : %7.1f M items/s\n", throughput(q, COUNT, a, b)); }
    { static MutexQueue q;                std::printf("throughput mutex : %7.1f M items/s\n", throughput(q, COUNT, a, b)); }
    pingpong<SpscQueue<uint64_t, 1024>>("spsc", ghz, a, b);
    pingpong<MutexQueue>("mutex", ghz, a, b);
}
