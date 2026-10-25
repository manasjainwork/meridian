#pragma once
#include <chrono>
#include <cstdint>
#include <x86intrin.h>

namespace meridian {

// lfence keeps the CPU from reordering rdtsc around the code we measure.
inline uint64_t rdtsc_begin() noexcept {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

inline uint64_t rdtsc_end() noexcept {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

// Cold path only: TSC cycles per nanosecond (= GHz), measured against steady_clock.
inline double calibrate_tsc_ghz(int window_ms = 200) {
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    const uint64_t c0 = rdtsc_begin();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count() < window_ms) {}
    const uint64_t c1 = rdtsc_end();
    const auto t1 = clk::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return static_cast<double>(c1 - c0) / ns;
}

}  // namespace meridian
