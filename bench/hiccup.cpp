// Hiccup probe: one thread per listed cpu spins on the TSC and records every gap between two reads longer than 50 us, i.e. the times
// the thread was not running. It runs no engine code, so gaps here come from the machine (the scheduler, the hypervisor, the host).
// usage: hiccup [seconds] [cpu list, e.g. 2,4,6,8,10]
#include <pthread.h>
#include <sched.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>
#include "meridian/timing.hpp"

using namespace meridian;

struct Res { int cpu; uint64_t over50us = 0, over1ms = 0, over10ms = 0, over100ms = 0, max_ns = 0; };

static void probe(int cpu, double secs, double ghz, Res& out) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); pthread_setaffinity_np(pthread_self(), sizeof s, &s);
    out.cpu = cpu;
    const uint64_t end = rdtsc_begin() + static_cast<uint64_t>(secs * 1e9 * ghz);
    uint64_t prev = rdtsc_begin();
    for (;;) {
        const uint64_t now = rdtsc_begin();
        if (now >= end) break;
        const double gap = static_cast<double>(now - prev) / ghz;
        if (gap > 50e3) {
            ++out.over50us;
            if (gap > 1e6) ++out.over1ms;
            if (gap > 10e6) ++out.over10ms;
            if (gap > 100e6) ++out.over100ms;
            if (static_cast<uint64_t>(gap) > out.max_ns) out.max_ns = static_cast<uint64_t>(gap);
        }
        prev = now;
    }
}

int main(int argc, char** argv) {
    const double secs = argc > 1 ? std::atof(argv[1]) : 10;
    std::vector<int> cpus;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", argc > 2 ? argv[2] : "2,4,6,8,10");
    for (char* t = std::strtok(buf, ","); t; t = std::strtok(nullptr, ",")) cpus.push_back(std::atoi(t));
    const double ghz = calibrate_tsc_ghz(200);
    std::vector<Res> rs(cpus.size());
    std::vector<std::thread> ts;
    for (size_t i = 0; i < cpus.size(); ++i) ts.emplace_back(probe, cpus[i], secs, ghz, std::ref(rs[i]));
    for (auto& t : ts) t.join();
    std::printf("%.0f s per cpu; gaps between two reads (ns)\n", secs);
    for (const Res& r : rs)
        std::printf("  cpu %2d: >50us %6llu  >1ms %5llu  >10ms %4llu  >100ms %3llu  max %llu\n", r.cpu, (unsigned long long)r.over50us,
                    (unsigned long long)r.over1ms, (unsigned long long)r.over10ms, (unsigned long long)r.over100ms, (unsigned long long)r.max_ns);
    return 0;
}
