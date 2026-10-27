// Order types (GTC / IOC / FOK / market), modify, and the structural invariants of the book.
// Three layers of checking: hand-written scenarios, a differential fuzz against an obviously-correct reference,
// and properties that must hold for every operation no matter what the reference says.
#include <cstdint>
#include <cstdio>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "meridian/order_book.hpp"

using namespace meridian::book;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

// ---------------------------------------------------------------------------------------------------------------
// Reference: std::map of FIFO lists. Slow, but short enough to trust by reading.
struct RefBook {
    using Lvl = std::list<std::pair<uint64_t, uint32_t>>;
    std::map<uint32_t, Lvl> bids, asks;
    uint32_t max_price, capacity;
    RefBook(uint32_t cap, uint32_t mp) : max_price(mp), capacity(cap) {}

    size_t size() const {
        size_t n = 0;
        for (auto& kv : bids) n += kv.second.size();
        for (auto& kv : asks) n += kv.second.size();
        return n;
    }
    bool has(uint64_t id) const {
        for (auto* m : {&bids, &asks}) for (auto& kv : *m) for (auto& o : kv.second) if (o.first == id) return true;
        return false;
    }
    uint64_t available(Side side, uint32_t price) const {      // quantity an order of `side` at `price` could trade against
        uint64_t q = 0;
        if (side == Side::Buy) { for (auto& kv : asks) if (kv.first <= price) for (auto& o : kv.second) q += o.second; }
        else { for (auto& kv : bids) if (kv.first >= price) for (auto& o : kv.second) q += o.second; }
        return q;
    }
    ExecReport submit(uint64_t id, Side side, TimeInForce tif, uint32_t price, uint32_t qty, std::vector<Fill>& out) {
        if (qty == 0 || price == 0 || price >= max_price) return {AddResult::BadParam, 0, 0, 0};
        if (has(id)) return {AddResult::Duplicate, 0, 0, 0};
        if (tif == TimeInForce::GTC && size() >= capacity) return {AddResult::PoolFull, 0, 0, 0};
        if (tif == TimeInForce::FOK && available(side, price) < qty) return {AddResult::Ok, 0, 0, qty};
        uint32_t rem = qty;
        if (side == Side::Buy) {
            while (rem && !asks.empty() && asks.begin()->first <= price) {
                const uint32_t p = asks.begin()->first;
                Lvl& l = asks.begin()->second;
                auto& m = l.front();
                const uint32_t q = m.second < rem ? m.second : rem;
                out.push_back(Fill{m.first, id, p, q});
                m.second -= q; rem -= q;
                if (m.second == 0) l.pop_front();
                if (l.empty()) asks.erase(asks.begin());
            }
        } else {
            while (rem && !bids.empty() && bids.rbegin()->first >= price) {
                const uint32_t p = bids.rbegin()->first;
                Lvl& l = bids.rbegin()->second;
                auto& m = l.front();
                const uint32_t q = m.second < rem ? m.second : rem;
                out.push_back(Fill{m.first, id, p, q});
                m.second -= q; rem -= q;
                if (m.second == 0) l.pop_front();
                if (l.empty()) bids.erase(p);
            }
        }
        const uint32_t filled = qty - rem;
        if (rem == 0) return {AddResult::Ok, filled, 0, 0};
        if (tif != TimeInForce::GTC) return {AddResult::Ok, filled, 0, rem};
        (side == Side::Buy ? bids : asks)[price].push_back({id, rem});
        return {AddResult::Ok, filled, rem, 0};
    }
    ExecReport market(uint64_t id, Side side, uint32_t qty, std::vector<Fill>& out) {
        return submit(id, side, TimeInForce::IOC, side == Side::Buy ? max_price - 1 : 1, qty, out);
    }
    bool cancel(uint64_t id) {
        for (auto* side : {&bids, &asks})
            for (auto it = side->begin(); it != side->end(); ++it)
                for (auto o = it->second.begin(); o != it->second.end(); ++o)
                    if (o->first == id) { it->second.erase(o); if (it->second.empty()) side->erase(it); return true; }
        return false;
    }
    ModifyResult modify(uint64_t id, uint32_t new_price, uint32_t new_qty, std::vector<Fill>& out, ExecReport* rep) {
        if (!has(id)) return ModifyResult::NotFound;
        if (new_qty == 0 || new_price == 0 || new_price >= max_price) return ModifyResult::BadParam;
        for (auto* m : {&bids, &asks}) for (auto& kv : *m) for (auto& o : kv.second) if (o.first == id) {
            if (kv.first == new_price && new_qty <= o.second) { o.second = new_qty; *rep = {AddResult::Ok, 0, new_qty, 0}; return ModifyResult::Ok; }
        }
        const Side side = bids.size() && [&] { for (auto& kv : bids) for (auto& o : kv.second) if (o.first == id) return true; return false; }() ? Side::Buy : Side::Sell;
        cancel(id);
        *rep = submit(id, side, TimeInForce::GTC, new_price, new_qty, out);
        return ModifyResult::Ok;
    }
    uint32_t best_bid() const { return bids.empty() ? 0 : bids.rbegin()->first; }
    uint32_t best_ask() const { return asks.empty() ? 0 : asks.begin()->first; }
    uint64_t qty_at(Side s, uint32_t p) const {
        auto& m = (s == Side::Buy) ? bids : asks;
        auto it = m.find(p);
        uint64_t q = 0;
        if (it != m.end()) for (auto& o : it->second) q += o.second;
        return q;
    }
};

static uint64_t total_qty(const OrderBook& b, uint32_t max_p) {
    uint64_t t = 0;
    for (uint32_t p = 1; p < max_p; ++p) t += b.qty_at(Side::Buy, p) + b.qty_at(Side::Sell, p);
    return t;
}

static bool inv(const OrderBook& b) {
    std::string why;
    if (b.check_invariants(&why)) return true;
    std::printf("INVARIANT BROKEN: %s\n", why.c_str());
    return false;
}

int main() {
    std::vector<Fill> f;

    { // market order sweeps levels in price order, leftover is cancelled and never rests
        OrderBook b(16, 1000);
        b.add_limit(1, Side::Sell, 101, 4, f); b.add_limit(2, Side::Sell, 103, 4, f); b.add_limit(3, Side::Sell, 103, 4, f);
        f.clear();
        const ExecReport r = b.submit_market(9, Side::Buy, 20, f);
        CHECK(r == (ExecReport{AddResult::Ok, 12, 0, 8}));
        CHECK(f.size() == 3 && f[0] == (Fill{1, 9, 101, 4}) && f[1] == (Fill{2, 9, 103, 4}) && f[2] == (Fill{3, 9, 103, 4}));
        CHECK(b.size() == 0 && b.best_ask() == 0 && b.best_bid() == 0 && inv(b));
    }
    { // market order on an empty book: nothing trades, everything is cancelled
        OrderBook b(16, 1000); f.clear();
        CHECK(b.submit_market(1, Side::Sell, 5, f) == (ExecReport{AddResult::Ok, 0, 0, 5}));
        CHECK(f.empty() && b.size() == 0 && inv(b));
    }
    { // market sell hits the best bid first
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Buy, 100, 3, f); b.add_limit(2, Side::Buy, 99, 3, f);
        const ExecReport r = b.submit_market(9, Side::Sell, 4, f);
        CHECK(r == (ExecReport{AddResult::Ok, 4, 0, 0}));
        CHECK(f.size() == 2 && f[0] == (Fill{1, 9, 100, 3}) && f[1] == (Fill{2, 9, 99, 1}));
        CHECK(b.best_bid() == 99 && b.qty_at(Side::Buy, 99) == 2 && inv(b));
    }
    { // IOC: partial fill, remainder cancelled, nothing rests
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Sell, 100, 3, f);
        const ExecReport r = b.submit(2, Side::Buy, TimeInForce::IOC, 100, 10, f);
        CHECK(r == (ExecReport{AddResult::Ok, 3, 0, 7}));
        CHECK(b.size() == 0 && b.best_bid() == 0 && inv(b));
        CHECK(b.submit(3, Side::Buy, TimeInForce::IOC, 99, 5, f) == (ExecReport{AddResult::Ok, 0, 0, 5}));   // does not cross
        CHECK(b.size() == 0);
    }
    { // FOK: not enough liquidity -> nothing happens at all
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Sell, 100, 3, f); b.add_limit(2, Side::Sell, 102, 3, f);
        const ExecReport r = b.submit(9, Side::Buy, TimeInForce::FOK, 102, 7, f);
        CHECK(r == (ExecReport{AddResult::Ok, 0, 0, 7}));
        CHECK(f.empty() && b.size() == 2 && b.qty_at(Side::Sell, 100) == 3 && b.qty_at(Side::Sell, 102) == 3 && inv(b));
    }
    { // FOK: enough across levels -> fills completely; exact size works; limit price excludes deeper liquidity
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Sell, 100, 3, f); b.add_limit(2, Side::Sell, 102, 3, f); b.add_limit(3, Side::Sell, 105, 50, f);
        CHECK(b.submit(9, Side::Buy, TimeInForce::FOK, 102, 7, f) == (ExecReport{AddResult::Ok, 0, 0, 7}));   // 105 is above the limit
        CHECK(b.submit(9, Side::Buy, TimeInForce::FOK, 102, 6, f) == (ExecReport{AddResult::Ok, 6, 0, 0}));
        CHECK(f.size() == 2 && b.best_ask() == 105 && inv(b));
        f.clear();
        b.add_limit(4, Side::Buy, 90, 5, f); b.add_limit(5, Side::Buy, 95, 5, f);
        CHECK(b.submit(10, Side::Sell, TimeInForce::FOK, 90, 10, f) == (ExecReport{AddResult::Ok, 10, 0, 0}));
        CHECK(b.best_bid() == 0 && inv(b));
    }
    { // FOK and IOC do not need a free pool slot because they never rest; GTC does
        OrderBook b(1, 1000); f.clear();
        b.add_limit(1, Side::Sell, 100, 5, f);
        CHECK(b.submit(2, Side::Buy, TimeInForce::IOC, 100, 2, f).filled == 2);
        CHECK(b.submit(3, Side::Buy, TimeInForce::FOK, 100, 3, f).filled == 3);
        CHECK(b.add_limit(4, Side::Sell, 100, 1, f) == AddResult::Ok);
        CHECK(b.add_limit(5, Side::Sell, 100, 1, f) == AddResult::PoolFull);
    }
    { // a live id cannot be reused by a market/IOC/FOK order either
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Sell, 100, 5, f);
        CHECK(b.submit_market(1, Side::Buy, 1, f).status == AddResult::Duplicate);
        CHECK(b.submit(1, Side::Buy, TimeInForce::FOK, 100, 1, f).status == AddResult::Duplicate);
        CHECK(b.submit_market(7, Side::Buy, 0, f).status == AddResult::BadParam);
    }
    { // modify: smaller quantity at the same price keeps the queue position
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Sell, 100, 10, f); b.add_limit(2, Side::Sell, 100, 10, f);
        CHECK(b.modify(1, 100, 4, f) == ModifyResult::Ok);
        b.add_limit(3, Side::Buy, 100, 5, f);
        CHECK(f.size() == 2 && f[0] == (Fill{1, 3, 100, 4}) && f[1] == (Fill{2, 3, 100, 1}));   // order 1 still first
        CHECK(inv(b));
    }
    { // modify: larger quantity loses priority
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Sell, 100, 5, f); b.add_limit(2, Side::Sell, 100, 5, f);
        CHECK(b.modify(1, 100, 6, f) == ModifyResult::Ok);
        b.add_limit(3, Side::Buy, 100, 5, f);
        CHECK(f.size() == 1 && f[0] == (Fill{2, 3, 100, 5}));                                   // order 2 now first
        CHECK(b.qty_at(Side::Sell, 100) == 6 && inv(b));
    }
    { // modify: price change moves the order to the new level and loses priority; equal qty at same price is a no-op
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Buy, 100, 5, f); b.add_limit(2, Side::Buy, 101, 5, f);
        CHECK(b.modify(1, 101, 5, f) == ModifyResult::Ok);
        CHECK(b.qty_at(Side::Buy, 100) == 0 && b.qty_at(Side::Buy, 101) == 10 && b.best_bid() == 101);
        b.add_limit(3, Side::Sell, 101, 5, f);
        CHECK(f.size() == 1 && f[0].maker_id == 2);                                              // 2 was ahead of the moved order
        CHECK(b.modify(1, 101, 5, f) == ModifyResult::Ok && b.qty_at(Side::Buy, 101) == 5 && inv(b));
    }
    { // modify to a crossing price trades as an aggressor; the rest rests
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Sell, 105, 4, f); b.add_limit(2, Side::Buy, 100, 10, f);
        ExecReport r{};
        CHECK(b.modify(2, 105, 10, f, &r) == ModifyResult::Ok);
        CHECK(r == (ExecReport{AddResult::Ok, 4, 6, 0}));
        CHECK(f.size() == 1 && f[0] == (Fill{1, 2, 105, 4}));
        CHECK(b.best_bid() == 105 && b.qty_at(Side::Buy, 105) == 6 && b.best_ask() == 0 && inv(b));
    }
    { // modify errors, and a failed modify changes nothing
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Buy, 100, 5, f);
        CHECK(b.modify(9, 100, 5, f) == ModifyResult::NotFound);
        CHECK(b.modify(1, 100, 0, f) == ModifyResult::BadParam);
        CHECK(b.modify(1, 0, 5, f) == ModifyResult::BadParam);
        CHECK(b.modify(1, 1000, 5, f) == ModifyResult::BadParam);
        CHECK(b.qty_at(Side::Buy, 100) == 5 && b.size() == 1 && inv(b));
    }

    // Differential fuzz + properties. Dense rounds trade a lot; sparse rounds test far-apart levels.
    uint64_t ops_run = 0, rejected = 0, fok_full = 0, fok_none = 0, mods = 0;
    for (int round = 0; round < 60; ++round) {
        const bool sparse = round % 3 == 2;
        const uint32_t max_p = sparse ? 2048 : 256;
        const uint32_t lo = sparse ? 1 : 90;
        const uint32_t span = sparse ? 2000 : 21;
        const uint32_t cap = (round % 5 == 4) ? 40 : 4096;       // some rounds run against a tiny pool
        OrderBook b(cap, max_p);
        RefBook r(cap, max_p);
        uint64_t next_id = 1 + static_cast<uint64_t>(round) * 10000000ull;
        const uint64_t first_id = next_id;
        std::vector<Fill> fb, fr;
        int bad = 0;
        for (int i = 0; i < 4000 && !bad; ++i, ++ops_run) {
            fb.clear(); fr.clear();
            const unsigned pick = static_cast<unsigned>(rnd() % 100);
            const uint64_t before = total_qty(b, max_p);
            const auto any_id = [&] { return (next_id > first_id) ? first_id + rnd() % (next_id - first_id) : first_id; };
            // 80% of the time pick an order that is really resting (so cancels and modifies mostly hit), else any id ever issued
            const auto old_id = [&]() -> uint64_t {
                if (rnd() % 10 < 8 && r.size() > 0) {
                    size_t k = rnd() % r.size();
                    for (auto* m : {&r.bids, &r.asks}) for (auto& kv : *m) for (auto& o : kv.second) if (k-- == 0) return o.first;
                }
                return any_id();
            };
            if (pick < 62) {                                     // new order of some kind
                const uint64_t id = (rnd() % 40 == 0) ? any_id() : next_id++;
                const Side s = (rnd() & 1) ? Side::Buy : Side::Sell;
                const uint32_t p = lo + static_cast<uint32_t>(rnd() % span);
                const uint32_t q = (rnd() % 60 == 0) ? 0 : 1 + static_cast<uint32_t>(rnd() % 25);
                const unsigned kind = static_cast<unsigned>(rnd() % 10);
                ExecReport eb, er;
                TimeInForce tif = TimeInForce::GTC;
                if (kind < 5) tif = TimeInForce::GTC; else if (kind < 7) tif = TimeInForce::IOC; else if (kind < 9) tif = TimeInForce::FOK;
                const bool is_market = kind == 9;
                if (is_market) { eb = b.submit_market(id, s, q, fb); er = r.market(id, s, q, fr); }
                else { eb = b.submit(id, s, tif, p, q, fb); er = r.submit(id, s, tif, p, q, fr); }
                if (!(eb == er)) ++bad;
                if (eb.status != AddResult::Ok) ++rejected;
                if (eb.status == AddResult::Ok) {
                    if (eb.filled + eb.rested + eb.cancelled != q) ++bad;                    // every unit is accounted for
                    uint64_t fsum = 0;
                    uint32_t last = 0;
                    for (const Fill& x : fb) {
                        fsum += x.qty;
                        if (x.taker_id != id || x.maker_id == id || x.qty == 0) ++bad;
                        if (!is_market) { if (s == Side::Buy ? x.price > p : x.price < p) ++bad; }   // never worse than the limit
                        if (last && (s == Side::Buy ? x.price < last : x.price > last)) ++bad;      // walks the book outward
                        last = x.price;
                    }
                    if (fsum != eb.filled) ++bad;
                    if (tif != TimeInForce::GTC || is_market) { if (eb.rested != 0) ++bad; }         // only GTC rests
                    if (tif == TimeInForce::FOK && !is_market) {
                        if (eb.filled != 0 && eb.filled != q) ++bad;                                 // all or nothing
                        (eb.filled ? fok_full : fok_none)++;
                    }
                    if (total_qty(b, max_p) != before + eb.rested - eb.filled) ++bad;               // quantity is conserved
                }
            } else if (pick < 80) {                              // cancel
                const uint64_t id = old_id();
                if (b.cancel(id) != r.cancel(id)) ++bad;
            } else {                                             // modify
                const uint64_t id = old_id();
                const uint32_t p = (rnd() % 2) ? lo + static_cast<uint32_t>(rnd() % span) : 0;   // 0 = "keep price" below
                const uint32_t q = (rnd() % 50 == 0) ? 0 : 1 + static_cast<uint32_t>(rnd() % 25);
                // keep-price modify: look the price up through the reference
                uint32_t np = p;
                if (np == 0) { for (auto* m : {&r.bids, &r.asks}) for (auto& kv : *m) for (auto& o : kv.second) if (o.first == id) np = kv.first; if (np == 0) np = lo; }
                ExecReport eb{}, er{};
                const ModifyResult mb = b.modify(id, np, q, fb, &eb);
                const ModifyResult mr = r.modify(id, np, q, fr, &er);
                if (mb != mr) ++bad;
                if (mb == ModifyResult::Ok) { ++mods; if (!(eb == er)) ++bad; }
            }
            if (fb.size() != fr.size()) ++bad; else for (size_t k = 0; k < fb.size(); ++k) if (!(fb[k] == fr[k])) ++bad;
            if (b.best_bid() != r.best_bid() || b.best_ask() != r.best_ask() || b.size() != r.size()) ++bad;
            if (!inv(b)) ++bad;
            if (bad) std::printf("fuzz round %d op %d diverged\n", round, i);
        }
        for (uint32_t p = 1; p < max_p && !bad; ++p)
            if (b.qty_at(Side::Buy, p) != r.qty_at(Side::Buy, p) || b.qty_at(Side::Sell, p) != r.qty_at(Side::Sell, p)) ++bad;
        CHECK(bad == 0);
    }
    std::printf("fuzz: %llu operations, %llu rejected, %llu accepted modifies, FOK %llu filled / %llu killed\n",
                static_cast<unsigned long long>(ops_run), static_cast<unsigned long long>(rejected), static_cast<unsigned long long>(mods),
                static_cast<unsigned long long>(fok_full), static_cast<unsigned long long>(fok_none));
    CHECK(fok_full > 100 && fok_none > 100 && mods > 10000);      // the fuzz really exercised the interesting paths

    std::printf(g_fail ? "%d FAILED\n" : "all order type tests passed\n", g_fail);
    return g_fail != 0;
}
