#include <cstdint>
#include <cstdio>
#include <thread>
#include "meridian/spsc_queue.hpp"

using meridian::SpscQueue;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    {   // single thread: empty, FIFO, full, wraparound
        SpscQueue<uint64_t, 8> q;
        uint64_t v = 0;
        CHECK(!q.try_pop(v));
        for (uint64_t i = 0; i < 8; ++i) CHECK(q.try_push(i));
        CHECK(!q.try_push(99));                      // full: exactly N items fit
        for (uint64_t i = 0; i < 8; ++i) { CHECK(q.try_pop(v)); CHECK(v == i); }
        CHECK(!q.try_pop(v));
        for (uint64_t round = 0; round < 1000; ++round) {   // wrap many times, odd fill levels
            for (uint64_t i = 0; i < 5; ++i) CHECK(q.try_push(round * 5 + i));
            for (uint64_t i = 0; i < 5; ++i) { CHECK(q.try_pop(v)); CHECK(v == round * 5 + i); }
        }
    }
    {   // two threads: every value arrives once, in order, nothing lost (small ring forces full/empty races)
        constexpr uint64_t COUNT = 5'000'000;
        SpscQueue<uint64_t, 64> q;
        uint64_t bad = 0, sum = 0;
        std::thread prod([&] { for (uint64_t i = 1; i <= COUNT; ++i) while (!q.try_push(i)) {} });
        std::thread cons([&] {
            uint64_t expect = 1, v = 0;
            while (expect <= COUNT) { if (q.try_pop(v)) { if (v != expect) ++bad; sum += v; ++expect; } }
        });
        prod.join(); cons.join();
        CHECK(bad == 0);
        CHECK(sum == COUNT * (COUNT + 1) / 2);
    }
    std::printf(g_fail ? "%d FAILED\n" : "all spsc tests passed\n", g_fail);
    return g_fail != 0;
}
