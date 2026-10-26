#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include "meridian/ids.hpp"

using namespace meridian;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    const uint64_t id = 1300000000026053ULL;   // 16 digits, below 2^53
    const std::string want = "1300000000026053";
    const double d = static_cast<double>(id);

    std::printf("--- experiment: how do formatters treat the double %s? ---\n", want.c_str());
    { std::ostringstream o; o << d;
      std::printf("ostream default        : %s\n", o.str().c_str());
      CHECK(o.str() != want); }
    std::printf("std::to_string(double) : %s\n", std::to_string(d).c_str());
    { std::ostringstream o; o << std::fixed << std::setprecision(0) << d;
      std::printf("fixed, precision 0     : %s\n", o.str().c_str());
      CHECK(o.str() == want); }
    char b[64];
    std::snprintf(b, sizeof b, "%.0f", d);
    std::printf("snprintf %%.0f          : %s\n", b);
    CHECK(want == b);

    std::printf("--- the real limit: doubles hold integers exactly only up to 2^53 ---\n");
    const uint64_t limit = 1ULL << 53;
    CHECK(static_cast<uint64_t>(static_cast<double>(limit)) == limit);
    CHECK(static_cast<uint64_t>(static_cast<double>(limit + 1)) != limit + 1);
    std::printf("2^53     = %llu\n", static_cast<unsigned long long>(limit));
    std::printf("2^53 + 1 -> double -> uint64 = %llu (silently wrong)\n",
                static_cast<unsigned long long>(static_cast<uint64_t>(static_cast<double>(limit + 1))));
    std::printf("our id %llu is %.1f%% of 2^53\n", static_cast<unsigned long long>(id),
                100.0 * static_cast<double>(id) / static_cast<double>(limit));

    std::printf("--- uint64 path ---\n");
    CHECK(to_decimal(id) == want);
    CHECK(ExchangeOrderId(id).v == id);

    std::printf(g_fail ? "%d FAILED\n" : "all order id tests passed\n", g_fail);
    return g_fail != 0;
}
