// Meridian demo: shows the matching engine at work.
//   demo                      a narrated walk-through: price-time priority, partial fills, sweeps, cancels
//   demo --random N [--seed S] [--quiet] [--mix]
//                             N random orders; prints every trade (unless --quiet), the final book and a summary.
//                             --mix also uses IOC, FOK and market orders and modifies
// Uses only the public OrderBook API. Nothing here is a benchmark: wall-clock numbers include whatever is printed.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "meridian/order_book.hpp"

using namespace meridian::book;

static constexpr uint32_t kMaxPrice = 1000;

static void print_fills(const std::vector<Fill>& fills) {
    for (const Fill& f : fills)
        std::printf("   TRADE  %u @ %u   (resting #%llu was hit by incoming #%llu)\n", f.qty, f.price,
                    (unsigned long long)f.maker_id, (unsigned long long)f.taker_id);
    if (fills.empty()) std::printf("   (no trade)\n");
}

// Top `levels` price levels on each side, asks above bids, with a bar per level.
static void print_book(const OrderBook& b, int levels = 5) {
    std::vector<std::pair<uint32_t, uint64_t>> asks, bids;
    if (b.best_ask())
        for (uint32_t p = b.best_ask(); p < kMaxPrice && (int)asks.size() < levels && p < b.best_ask() + 400; ++p)
            if (uint64_t q = b.qty_at(Side::Sell, p)) asks.push_back({p, q});
    if (b.best_bid())
        for (uint32_t p = b.best_bid(); p >= 1 && (int)bids.size() < levels && p + 400 > b.best_bid(); --p)
            if (uint64_t q = b.qty_at(Side::Buy, p)) bids.push_back({p, q});
    auto bar = [](uint64_t q) { return std::string(std::min<uint64_t>(q / 5 + 1, 40), '#'); };
    std::printf("   ------------- BOOK -------------\n");
    for (int i = (int)asks.size() - 1; i >= 0; --i)
        std::printf("   SELL %5u  %6llu  %s\n", asks[i].first, (unsigned long long)asks[i].second, bar(asks[i].second).c_str());
    if (b.best_bid() && b.best_ask()) std::printf("   --------- spread %u ---------\n", b.best_ask() - b.best_bid());
    else std::printf("   --------- (one-sided) ---------\n");
    for (const auto& l : bids)
        std::printf("   BUY  %5u  %6llu  %s\n", l.first, (unsigned long long)l.second, bar(l.second).c_str());
    std::printf("   --------------------------------\n\n");
}

static void say(const char* s) { std::printf("\n>> %s\n", s); }

static void add(OrderBook& b, uint64_t id, Side side, uint32_t price, uint32_t qty) {
    std::vector<Fill> fills;
    std::printf("   %s #%llu: %s %u @ %u\n", side == Side::Buy ? "BUY " : "SELL", (unsigned long long)id,
                side == Side::Buy ? "buy" : "sell", qty, price);
    const AddResult r = b.add_limit(id, side, price, qty, fills);
    if (r != AddResult::Ok) std::printf("   rejected (code %d)\n", (int)r);
    print_fills(fills);
}

static void print_report(const ExecReport& r) {
    if (r.status != AddResult::Ok) { std::printf("   rejected (code %d)\n", (int)r.status); return; }
    std::printf("   report: filled %u, rested %u, cancelled %u\n", r.filled, r.rested, r.cancelled);
}

static const char* tif_name(TimeInForce t) { return t == TimeInForce::GTC ? "GTC" : t == TimeInForce::IOC ? "IOC" : "FOK"; }

static void order(OrderBook& b, uint64_t id, Side side, TimeInForce tif, uint32_t price, uint32_t qty) {
    std::vector<Fill> fills;
    std::printf("   #%llu: %s %s %u @ %u\n", (unsigned long long)id, tif_name(tif), side == Side::Buy ? "buy" : "sell", qty, price);
    const ExecReport r = b.submit(id, side, tif, price, qty, fills);
    print_fills(fills);
    print_report(r);
}

static void market(OrderBook& b, uint64_t id, Side side, uint32_t qty) {
    std::vector<Fill> fills;
    std::printf("   #%llu: MARKET %s %u (any price)\n", (unsigned long long)id, side == Side::Buy ? "buy" : "sell", qty);
    const ExecReport r = b.submit_market(id, side, qty, fills);
    print_fills(fills);
    print_report(r);
}

static void modify(OrderBook& b, uint64_t id, uint32_t price, uint32_t qty) {
    std::vector<Fill> fills;
    std::printf("   modify #%llu -> %u @ %u\n", (unsigned long long)id, qty, price);
    const ModifyResult m = b.modify(id, price, qty, fills);
    if (m != ModifyResult::Ok) std::printf("   modify failed (code %d)\n", (int)m);
    print_fills(fills);
}

static int story() {
    OrderBook b(1024, kMaxPrice);
    std::printf("Meridian matching engine: narrated demo\n");
    std::printf("Rules: best price first; at the same price, earlier order first (price-time priority).\n");
    std::printf("A trade always happens at the price of the order that was already resting.\n");

    say("1. Liquidity arrives. Sellers post at 101, 102, 103; buyers post at 99, 98, 97.");
    add(b, 1, Side::Sell, 101, 30);
    add(b, 2, Side::Sell, 102, 20);
    add(b, 3, Side::Sell, 103, 40);
    add(b, 4, Side::Buy, 99, 25);
    add(b, 5, Side::Buy, 98, 30);
    add(b, 6, Side::Buy, 97, 50);
    print_book(b);

    say("2. An aggressive buyer wants 45 and will pay up to 102. It eats 101 first, then part of 102.");
    add(b, 7, Side::Buy, 102, 45);
    print_book(b);

    say("3. Time priority. Buyer #8 joins the queue at 99 BEHIND #4. A seller then hits 99 for 30.");
    add(b, 8, Side::Buy, 99, 10);
    add(b, 9, Side::Sell, 99, 30);
    std::printf("   #4 arrived first, so it is filled first (25), then #8 gets the rest (5).\n");
    print_book(b);

    say("4. A buyer changes their mind: order #5 (98) is cancelled.");
    std::printf("   cancel #5 -> %s\n", b.cancel(5) ? "cancelled" : "not found");
    std::printf("   cancel #5 again -> %s\n", b.cancel(5) ? "cancelled" : "not found (already gone)");
    print_book(b);

    say("5. A big seller sweeps the bids: sell 100 down to 97. What does not trade rests as an ask.");
    add(b, 10, Side::Sell, 97, 100);
    print_book(b);

    std::printf("\n================ order types ================\n");
    OrderBook c(1024, kMaxPrice);
    say("6. Fresh book. Sellers at 101 (10), 102 (10), 104 (30). Buyers at 99 (20 from #14, then 20 from #24) and 98 (20).");
    add(c, 11, Side::Sell, 101, 10);
    add(c, 12, Side::Sell, 102, 10);
    add(c, 13, Side::Sell, 104, 30);
    add(c, 14, Side::Buy, 99, 20);
    add(c, 24, Side::Buy, 99, 20);
    add(c, 15, Side::Buy, 98, 20);
    print_book(c);

    say("7. MARKET buy 15: no price limit. It takes the best prices until it has 15.");
    market(c, 20, Side::Buy, 15);
    print_book(c);

    say("8. IOC buy 50 @ 102 (immediate-or-cancel): trades what it can, the rest is cancelled, never rests.");
    order(c, 21, Side::Buy, TimeInForce::IOC, 102, 50);
    print_book(c);

    say("9. FOK buy 40 @ 104 (fill-or-kill): only 30 is available up to 104, so NOTHING happens.");
    order(c, 22, Side::Buy, TimeInForce::FOK, 104, 40);
    std::printf("   Now FOK buy 30 @ 104: exactly enough, so it fills completely.\n");
    order(c, 23, Side::Buy, TimeInForce::FOK, 104, 30);
    print_book(c);

    say("10. Modify and priority. #14 is first in the queue at 99, #24 is behind it.");
    std::printf("   Smaller quantity, same price: #14 keeps its place. Then a seller hits 99 for 5:\n");
    modify(c, 14, 99, 10);
    order(c, 25, Side::Sell, TimeInForce::GTC, 99, 5);
    std::printf("   Bigger quantity: #14 goes to the back of the queue. Then a seller hits 99 for 10:\n");
    modify(c, 14, 99, 30);
    order(c, 26, Side::Sell, TimeInForce::GTC, 99, 10);
    std::printf("   #24 was filled, not #14.\n");
    print_book(c);

    std::printf("Done. Try:  demo --random 20 --seed 1   or   demo --random 30 --seed 2 --mix\n");
    return 0;
}

static int random_flow(uint64_t n, uint64_t seed, bool quiet, bool mix) {
    OrderBook b(200000, kMaxPrice);
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> live;
    uint64_t trades = 0, volume = 0, cancels_try = 0, cancels_ok = 0, rejected = 0;
    uint64_t n_ioc = 0, n_fok_full = 0, n_fok_kill = 0, n_mkt = 0, n_mod = 0, cancelled_qty = 0;
    std::vector<Fill> fills;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t id = 1; id <= n; ++id) {
        fills.clear();
        if (mix && !live.empty() && rng() % 100 < 8) {              // with --mix: 8% modify a random order
            const uint64_t target = live[rng() % live.size()];
            const uint32_t price = 480 + (uint32_t)(rng() % 41);
            const uint32_t qty = 1 + (uint32_t)(rng() % 100);
            ExecReport rep{};
            const ModifyResult m = b.modify(target, price, qty, fills, &rep);
            if (m == ModifyResult::Ok) ++n_mod;
            if (!quiet) std::printf("#%llu modify #%llu -> %u @ %u (%s)\n", (unsigned long long)id, (unsigned long long)target, qty, price, m == ModifyResult::Ok ? "ok" : "not found");
            for (const Fill& f : fills) {
                ++trades; volume += f.qty;
                if (!quiet) std::printf("    TRADE %u @ %u  (#%llu x #%llu)\n", f.qty, f.price, (unsigned long long)f.maker_id, (unsigned long long)f.taker_id);
            }
            continue;
        }
        if (!live.empty() && rng() % 100 < 25) {                    // 25%: cancel a random order
            const size_t k = rng() % live.size();
            ++cancels_try;
            if (b.cancel(live[k])) { ++cancels_ok; if (!quiet) std::printf("#%llu cancel #%llu\n", (unsigned long long)id, (unsigned long long)live[k]); }
            live[k] = live.back();
            live.pop_back();
            continue;
        }
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        const uint32_t price = 480 + (uint32_t)(rng() % 41);        // 480 .. 520
        const uint32_t qty = 1 + (uint32_t)(rng() % 100);
        TimeInForce tif = TimeInForce::GTC;
        bool is_market = false;
        if (mix) {
            const unsigned k = (unsigned)(rng() % 100);
            if (k < 15) tif = TimeInForce::IOC; else if (k < 25) tif = TimeInForce::FOK; else if (k < 30) is_market = true;
        }
        if (!quiet) {
            if (is_market) std::printf("#%llu MARKET %s %u\n", (unsigned long long)id, side == Side::Buy ? "BUY " : "SELL", qty);
            else std::printf("#%llu %s %s %u @ %u\n", (unsigned long long)id, tif_name(tif), side == Side::Buy ? "BUY " : "SELL", qty, price);
        }
        const ExecReport rep = is_market ? b.submit_market(id, side, qty, fills) : b.submit(id, side, tif, price, qty, fills);
        if (rep.status != AddResult::Ok) { ++rejected; continue; }
        cancelled_qty += rep.cancelled;
        if (is_market) ++n_mkt; else if (tif == TimeInForce::IOC) ++n_ioc;
        else if (tif == TimeInForce::FOK) (rep.filled ? n_fok_full : n_fok_kill)++;
        if (rep.rested) live.push_back(id);
        for (const Fill& f : fills) {
            ++trades;
            volume += f.qty;
            if (!quiet) std::printf("    TRADE %u @ %u  (#%llu x #%llu)\n", f.qty, f.price, (unsigned long long)f.maker_id, (unsigned long long)f.taker_id);
        }
        if (!quiet && rep.cancelled) std::printf("    cancelled unfilled quantity: %u\n", rep.cancelled);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n== summary (seed %llu) ==\n", (unsigned long long)seed);
    std::printf("messages: %llu   trades: %llu   volume: %llu   cancels ok/tried: %llu/%llu   rejected: %llu\n",
                (unsigned long long)n, (unsigned long long)trades, (unsigned long long)volume,
                (unsigned long long)cancels_ok, (unsigned long long)cancels_try, (unsigned long long)rejected);
    if (mix)
        std::printf("order types: IOC %llu, FOK filled %llu / killed %llu, market %llu, modifies %llu, quantity cancelled unfilled %llu\n",
                    (unsigned long long)n_ioc, (unsigned long long)n_fok_full, (unsigned long long)n_fok_kill,
                    (unsigned long long)n_mkt, (unsigned long long)n_mod, (unsigned long long)cancelled_qty);
    std::printf("wall time: %.2f ms (%s)\n\n", ms, quiet ? "no printing in the loop, still not a benchmark" : "includes printing, not a benchmark");
    print_book(b, 8);
    return 0;
}

int main(int argc, char** argv) {
    uint64_t n = 0, seed = 1;
    bool quiet = false, random = false, mix = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--random") && i + 1 < argc) { random = true; n = std::strtoull(argv[++i], nullptr, 10); }
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--mix")) mix = true;
        else { std::fprintf(stderr, "usage: demo | demo --random N [--seed S] [--quiet] [--mix]\n"); return 2; }
    }
    return random ? random_flow(n, seed, quiet, mix) : story();
}
