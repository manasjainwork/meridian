#include <cstdio>
#include <cstring>
#include "meridian/protocol.hpp"

using namespace meridian::proto;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    const NewOrder m = make_new_order(0x0102030405060708ULL, 42, 7, Side::Sell, 10050, 100);
    uint8_t b[sizeof(NewOrder)];
    std::memcpy(b, &m, sizeof m);

    // wire format is big-endian: length 38 = 0x0026, seq bytes 01..08
    CHECK(b[0] == 0x00 && b[1] == 0x26);
    CHECK(b[4] == 0x01 && b[11] == 0x08);

    auto r = parse_one(b, sizeof b);
    CHECK(r.status == Status::Ok && r.consumed == sizeof(NewOrder));
    CHECK(r.header == reinterpret_cast<const Header*>(b));  // zero-copy: same address
    const auto* o = as<NewOrder>(r.header);
    CHECK(o->h.seq.get() == 0x0102030405060708ULL);
    CHECK(o->client_order_id.get() == 42);
    CHECK(o->instrument_id.get() == 7);
    CHECK(o->side == static_cast<uint8_t>(Side::Sell));
    CHECK(o->price_ticks.get() == 10050);
    CHECK(o->qty.get() == 100);

    CHECK(parse_one(b, 5).status == Status::NeedMore);    // less than a header
    CHECK(parse_one(b, 20).status == Status::NeedMore);   // mid-message

    uint8_t two[2 * sizeof(NewOrder)];                     // back-to-back messages
    std::memcpy(two, &m, sizeof m);
    std::memcpy(two + sizeof m, &m, sizeof m);
    auto r1 = parse_one(two, sizeof two);
    auto r2 = parse_one(two + r1.consumed, sizeof two - r1.consumed);
    CHECK(r1.status == Status::Ok && r2.status == Status::Ok);
    CHECK(r1.consumed + r2.consumed == sizeof two);

    uint8_t bad[sizeof(NewOrder)];
    std::memcpy(bad, b, sizeof b);
    bad[2] = 99;
    CHECK(parse_one(bad, sizeof bad).status == Status::BadType);
    std::memcpy(bad, b, sizeof b);
    bad[1] = 0x10;
    CHECK(parse_one(bad, sizeof bad).status == Status::BadLength);

    std::printf(g_fail ? "%d FAILED\n" : "all protocol tests passed\n", g_fail);
    return g_fail != 0;
}
