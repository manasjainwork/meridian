// OMS cost against book size (bug 032): the same order stream as seq_latency (random limit orders around 480 to 520, every fourth
// request cancels the newest open order), one account or many, timed in blocks of 40,000 requests. If the time per block grows, the
// cost of one request depends on how many orders exist. usage: oms_scale [1 = one account, 0 = fifty accounts]
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
#include "meridian/oms.hpp"
using namespace meridian;
int main(int argc, char** argv) {
    const bool one_account = argc > 1 && argv[1][0] == '1';
    oms::Config c; c.max_orders = 1u << 20;
    oms::Oms o(c);
    std::mt19937_64 g(100);
    std::vector<uint64_t> open;
    uint64_t cl = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < 200000; ++i) {
        oms::Result r;
        if (i % 4 == 3 && !open.empty()) { r = o.cancel(1, open.back()); open.pop_back(); }
        else {
            const uint32_t acc = one_account ? 1 : 1 + (g() % 50);
            r = o.submit(oms::Request{acc, ++cl, static_cast<book::Side>(g() & 1), oms::Type::Limit, book::TimeInForce::GTC, static_cast<uint32_t>(480 + g() % 40), static_cast<uint32_t>(1 + g() % 50), 0});
            if (r.status == oms::Status::Open && r.order_id) open.push_back(r.order_id);
        }
        if ((i + 1) % 40000 == 0) { auto t1 = std::chrono::steady_clock::now(); std::printf("  requests %6llu: last 40000 took %.1f ms\n", (unsigned long long)(i + 1), std::chrono::duration<double, std::milli>(t1 - t0).count()); t0 = t1; }
    }
}
