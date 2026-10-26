#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include "meridian/protocol.hpp"

using namespace meridian::proto;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// Layer 1: compile-time guards
static_assert(!std::is_convertible_v<uint16_t, BE<uint16_t>>, "raw ints must not silently become BE");
static_assert(std::is_trivially_copyable_v<NewOrder>);
static_assert(std::is_standard_layout_v<NewOrder>);
static_assert(alignof(NewOrder) == 1);

static uint64_t rng = 88172645463325252ULL;
static uint64_t next_rand() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

struct NaiveBookType { uint16_t book_type; };  // the classic bug: host order on the wire

int main() {
    // The bug itself: book_type = 1 written in host (little-endian) order.
    NaiveBookType n{1};
    uint8_t nb[2];
    std::memcpy(nb, &n, 2);
    CHECK(nb[0] == 0x01 && nb[1] == 0x00);
    const unsigned exchange_reads_naive = (nb[0] << 8) | nb[1];  // exchange reads big-endian
    CHECK(exchange_reads_naive == 256);                           // "BookType 256" -> rejected

    // The fix: BE<> stores the same value in network order.
    BE<uint16_t> good{};
    good.set(1);
    uint8_t gb[2];
    std::memcpy(gb, &good, 2);
    CHECK(gb[0] == 0x00 && gb[1] == 0x01);
    CHECK(((gb[0] << 8) | gb[1]) == 1u);

    // Layer 2: property test, byte k on the wire must be byte k from the top of the value
    int bad = 0;
    for (int i = 0; i < 1000000; ++i) {
        const uint64_t v = next_rand();
        BE<uint64_t> a{}; a.set(v);
        BE<uint32_t> b{}; b.set(static_cast<uint32_t>(v));
        BE<uint16_t> c{}; c.set(static_cast<uint16_t>(v));
        if (a.get() != v || b.get() != static_cast<uint32_t>(v) || c.get() != static_cast<uint16_t>(v)) ++bad;
        uint8_t x[8]; std::memcpy(x, &a, 8);
        for (int k = 0; k < 8; ++k) if (x[k] != static_cast<uint8_t>(v >> (56 - 8 * k))) ++bad;
        uint8_t y[4]; std::memcpy(y, &b, 4);
        for (int k = 0; k < 4; ++k) if (y[k] != static_cast<uint8_t>(static_cast<uint32_t>(v) >> (24 - 8 * k))) ++bad;
        uint8_t z[2]; std::memcpy(z, &c, 2);
        for (int k = 0; k < 2; ++k) if (z[k] != static_cast<uint8_t>(static_cast<uint16_t>(v) >> (8 - 8 * k))) ++bad;
    }
    CHECK(bad == 0);

    // Layer 3: golden bytes. These 38 bytes are the spec; they never change silently.
    const uint8_t golden[38] = {
        0x00, 0x26, 0x01, 0x01,                          // length=38, type=NewOrder, version=1
        0, 0, 0, 0, 0, 0, 0, 1,                          // seq=1
        0, 0, 0, 0, 0, 0, 0, 2,                          // client_order_id=2
        0, 0, 0, 3,                                      // instrument_id=3
        0x01, 0x00,                                      // side=Sell, order_type=0
        0, 0, 0, 0, 0, 0, 0x27, 0x42,                    // price_ticks=10050
        0, 0, 0, 0x64                                    // qty=100
    };
    const NewOrder m = make_new_order(1, 2, 3, Side::Sell, 10050, 100);
    CHECK(std::memcmp(&m, golden, sizeof golden) == 0);

    std::printf(g_fail ? "%d FAILED\n" : "all endianness tests passed\n", g_fail);
    return g_fail != 0;
}
