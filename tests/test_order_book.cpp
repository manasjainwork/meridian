#include <cstdint>
#include <cstdio>
#include <list>
#include <map>
#include <utility>
#include <vector>
#include "meridian/order_book.hpp"

using namespace meridian::book;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// Obviously-correct reference: std::map of FIFO lists. Slow, but easy to trust.
struct RefBook {
    using Lvl = std::list<std::pair<uint64_t, uint32_t>>;
    std::map<uint32_t, Lvl> bids, asks;

    void add(uint64_t id, Side side, uint32_t price, uint32_t qty, std::vector<Fill>& out) {
        if (side == Side::Buy) {
            while (qty && !asks.empty() && asks.begin()->first <= price) {
                const uint32_t p = asks.begin()->first;
                Lvl& l = asks.begin()->second;
                auto& m = l.front();
                const uint32_t q = m.second < qty ? m.second : qty;
                out.push_back(Fill{m.first, id, p, q});
                m.second -= q; qty -= q;
                if (m.second == 0) l.pop_front();
                if (l.empty()) asks.erase(asks.begin());
            }
            if (qty) bids[price].push_back({id, qty});
        } else {
            while (qty && !bids.empty() && bids.rbegin()->first >= price) {
                const uint32_t p = bids.rbegin()->first;
                Lvl& l = bids.rbegin()->second;
                auto& m = l.front();
                const uint32_t q = m.second < qty ? m.second : qty;
                out.push_back(Fill{m.first, id, p, q});
                m.second -= q; qty -= q;
                if (m.second == 0) l.pop_front();
                if (l.empty()) bids.erase(p);
            }
            if (qty) asks[price].push_back({id, qty});
        }
    }
    bool cancel(uint64_t id) {
        for (auto* side : {&bids, &asks})
            for (auto it = side->begin(); it != side->end(); ++it)
                for (auto o = it->second.begin(); o != it->second.end(); ++o)
                    if (o->first == id) {
                        it->second.erase(o);
                        if (it->second.empty()) side->erase(it);
                        return true;
                    }
        return false;
    }
    uint32_t best_bid() const { return bids.empty() ? 0 : bids.rbegin()->first; }
    uint32_t best_ask() const { return asks.empty() ? 0 : asks.begin()->first; }
    size_t size() const {
        size_t n = 0;
        for (auto& kv : bids) n += kv.second.size();
        for (auto& kv : asks) n += kv.second.size();
        return n;
    }
    uint64_t qty_at(Side s, uint32_t p) const {
        auto& m = (s == Side::Buy) ? bids : asks;
        auto it = m.find(p);
        uint64_t q = 0;
        if (it != m.end()) for (auto& o : it->second) q += o.second;
        return q;
    }
};

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main() {
    std::vector<Fill> f;

    { // resting orders do not trade; best prices tracked
        OrderBook b(16, 1000);
        CHECK(b.add_limit(1, Side::Buy, 100, 10, f) == AddResult::Ok);
        CHECK(b.add_limit(2, Side::Sell, 105, 5, f) == AddResult::Ok);
        CHECK(f.empty() && b.best_bid() == 100 && b.best_ask() == 105 && b.size() == 2);
    }
    { // price-time priority: same price fills oldest first
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Sell, 100, 5, f);
        b.add_limit(2, Side::Sell, 100, 5, f);
        b.add_limit(3, Side::Buy, 100, 7, f);
        CHECK(f.size() == 2);
        CHECK(f[0] == (Fill{1, 3, 100, 5}));
        CHECK(f[1] == (Fill{2, 3, 100, 2}));
        CHECK(b.qty_at(Side::Sell, 100) == 3 && b.size() == 1);
    }
    { // taker gets the maker's (better) price, and walks several levels
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Sell, 101, 4, f);
        b.add_limit(2, Side::Sell, 103, 4, f);
        b.add_limit(3, Side::Buy, 110, 6, f);
        CHECK(f.size() == 2);
        CHECK(f[0] == (Fill{1, 3, 101, 4}));
        CHECK(f[1] == (Fill{2, 3, 103, 2}));
        CHECK(b.best_ask() == 103 && b.best_bid() == 0);
    }
    { // partial fill leaves remainder resting at the limit price
        OrderBook b(16, 1000); f.clear();
        b.add_limit(1, Side::Sell, 100, 3, f);
        b.add_limit(2, Side::Buy, 100, 10, f);
        CHECK(f.size() == 1 && f[0].qty == 3);
        CHECK(b.best_bid() == 100 && b.qty_at(Side::Buy, 100) == 7 && b.best_ask() == 0);
    }
    { // cancel, duplicate ids, bad params, pool exhaustion
        OrderBook b(2, 1000); f.clear();
        CHECK(b.add_limit(1, Side::Buy, 100, 10, f) == AddResult::Ok);
        CHECK(b.add_limit(1, Side::Buy, 100, 10, f) == AddResult::Duplicate);
        CHECK(b.add_limit(9, Side::Buy, 0, 10, f) == AddResult::BadParam);
        CHECK(b.add_limit(9, Side::Buy, 1000, 10, f) == AddResult::BadParam);
        CHECK(b.add_limit(9, Side::Buy, 100, 0, f) == AddResult::BadParam);
        CHECK(b.add_limit(2, Side::Buy, 99, 10, f) == AddResult::Ok);
        CHECK(b.add_limit(3, Side::Buy, 98, 10, f) == AddResult::PoolFull);
        CHECK(b.cancel(1) && !b.cancel(1) && !b.cancel(77));
        CHECK(b.best_bid() == 99 && b.size() == 1);
        CHECK(b.add_limit(3, Side::Buy, 98, 10, f) == AddResult::Ok);   // slot was recycled
    }

    // Differential fuzz: the real book must match the reference after every operation.
    for (int round = 0; round < 20; ++round) {
        OrderBook b(4096, 256);
        RefBook r;
        uint64_t next_id = 1 + static_cast<uint64_t>(round) * 1000000ull;
        const uint64_t first_id = next_id;
        std::vector<Fill> fb, fr;
        int bad = 0;
        for (int i = 0; i < 20000; ++i) {
            fb.clear(); fr.clear();
            if (rnd() % 100 < 65) {
                const uint64_t id = (rnd() % 50 == 0 && next_id > first_id) ? first_id + rnd() % (next_id - first_id) : next_id++;
                const Side s = (rnd() & 1) ? Side::Buy : Side::Sell;
                const uint32_t p = 90 + static_cast<uint32_t>(rnd() % 21);
                const uint32_t q = 1 + static_cast<uint32_t>(rnd() % 20);
                const AddResult res = b.add_limit(id, s, p, q, fb);
                if (res == AddResult::Ok) r.add(id, s, p, q, fr);
                else if (res != AddResult::Duplicate && res != AddResult::PoolFull) ++bad;
            } else if (next_id > first_id) {
                const uint64_t id = first_id + rnd() % (next_id - first_id);
                if (b.cancel(id) != r.cancel(id)) ++bad;
            }
            if (!(fb == fr)) ++bad;
            if (b.best_bid() != r.best_bid() || b.best_ask() != r.best_ask()) ++bad;
            if (b.size() != r.size()) ++bad;
            if (bad) break;
        }
        for (uint32_t p = 85; p < 115 && !bad; ++p)
            if (b.qty_at(Side::Buy, p) != r.qty_at(Side::Buy, p) || b.qty_at(Side::Sell, p) != r.qty_at(Side::Sell, p)) ++bad;
        CHECK(bad == 0);
    }

    std::printf(g_fail ? "%d FAILED\n" : "all order book tests passed\n", g_fail);
    return g_fail != 0;
}
