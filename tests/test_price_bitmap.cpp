#include <cstdint>
#include <cstdio>
#include <iterator>
#include <set>
#include "meridian/price_bitmap.hpp"

using meridian::PriceBitmap;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main() {
    const size_t sizes[] = {1, 63, 64, 65, 2048, 4096, 4097, 100000, 262144};
    for (size_t n : sizes) {
        PriceBitmap bm(n);
        std::set<size_t> ref;
        int bad = 0;
        for (int i = 0; i < 40000 && !bad; ++i) {
            const size_t x = rnd() % n;
            if (rnd() % 100 < 55) { bm.set(x); ref.insert(x); }
            else { bm.clear(x); ref.erase(x); }
            const size_t q = rnd() % n;
            auto ge = ref.lower_bound(q);
            const size_t want_ge = ge == ref.end() ? PriceBitmap::npos : *ge;
            size_t want_le = PriceBitmap::npos;
            auto up = ref.upper_bound(q);
            if (up != ref.begin()) want_le = *std::prev(up);
            if (bm.next_ge(q) != want_ge) ++bad;
            if (bm.prev_le(q) != want_le) ++bad;
            if (bm.test(x) != (ref.count(x) == 1)) ++bad;
        }
        CHECK(bad == 0);
    }
    PriceBitmap e(1000);
    CHECK(e.next_ge(0) == PriceBitmap::npos && e.prev_le(999) == PriceBitmap::npos);
    e.set(999); e.set(0);
    CHECK(e.next_ge(1) == 999 && e.prev_le(998) == 0);
    std::printf(g_fail ? "%d FAILED\n" : "all price bitmap tests passed\n", g_fail);
    return g_fail != 0;
}
