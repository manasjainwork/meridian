#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "meridian/price_bitmap.hpp"

namespace meridian::book {

enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class AddResult : uint8_t { Ok, Duplicate, BadParam, PoolFull };
enum class TimeInForce : uint8_t {
    GTC,   // good till cancelled: match what crosses, rest the remainder
    IOC,   // immediate or cancel: match what crosses, cancel the remainder (never rests)
    FOK,   // fill or kill: either the whole quantity trades now, or nothing happens
};
enum class ModifyResult : uint8_t { Ok, NotFound, BadParam };

// What happened to the quantity of one incoming order: qty == filled + rested + cancelled (when status == Ok).
struct ExecReport {
    AddResult status;
    uint32_t filled;
    uint32_t rested;
    uint32_t cancelled;
    bool operator==(const ExecReport& o) const noexcept {
        return status == o.status && filled == o.filled && rested == o.rested && cancelled == o.cancelled;
    }
};

struct Fill {
    uint64_t maker_id;
    uint64_t taker_id;
    uint32_t price;   // always the resting (maker) order's price
    uint32_t qty;
    bool operator==(const Fill& o) const noexcept {
        return maker_id == o.maker_id && taker_id == o.taker_id && price == o.price && qty == o.qty;
    }
};

// Price-time priority limit order book.
// - prices are integer ticks in [1, max_price)
// - all memory is allocated in the constructor; add/cancel never allocate
// - orders live in a fixed pool and are linked per price level (FIFO)
// - order id -> pool slot via open addressing with backward-shift deletion
class OrderBook {
public:
    static constexpr uint32_t kNil = 0xFFFFFFFFu;

    OrderBook(uint32_t max_orders, uint32_t max_price)
        : max_price_(max_price), orders_(max_orders), bids_(max_price), asks_(max_price),
          bid_bm_(max_price), ask_bm_(max_price) {
        for (uint32_t i = 0; i < max_orders; ++i) orders_[i].next = (i + 1 < max_orders) ? i + 1 : kNil;
        free_ = max_orders ? 0 : kNil;
        unsigned bits = 1;
        while ((1ull << bits) < 2ull * max_orders) ++bits;
        shift_ = 64 - bits;
        mask_ = (size_t{1} << bits) - 1;
        keys_.assign(mask_ + 1, 0);
        vals_.assign(mask_ + 1, kNil);
    }

    // Matches against the opposite side first, rests any remainder. Fills are appended to out.
    AddResult add_limit(uint64_t id, Side side, uint32_t price, uint32_t qty, std::vector<Fill>& out) {
        return submit(id, side, TimeInForce::GTC, price, qty, out).status;
    }

    // General entry point for limit orders with a time-in-force.
    ExecReport submit(uint64_t id, Side side, TimeInForce tif, uint32_t price, uint32_t qty, std::vector<Fill>& out) {
        if (qty == 0 || price == 0 || price >= max_price_) return {AddResult::BadParam, 0, 0, 0};
        if (map_find(id) != kNil) return {AddResult::Duplicate, 0, 0, 0};
        if (tif == TimeInForce::GTC && free_ == kNil) return {AddResult::PoolFull, 0, 0, 0};
        if (tif == TimeInForce::FOK && !can_fill_fully(side, price, qty)) return {AddResult::Ok, 0, 0, qty};

        uint32_t rem = qty;
        if (side == Side::Buy) {
            while (rem > 0 && has_ask_ && best_ask_ <= price) rem -= fill_level(Side::Sell, best_ask_, id, rem, out);
        } else {
            while (rem > 0 && has_bid_ && best_bid_ >= price) rem -= fill_level(Side::Buy, best_bid_, id, rem, out);
        }
        const uint32_t filled = qty - rem;
        if (rem == 0) return {AddResult::Ok, filled, 0, 0};
        if (tif != TimeInForce::GTC) return {AddResult::Ok, filled, 0, rem};
        rest(id, side, price, rem);
        return {AddResult::Ok, filled, rem, 0};
    }

    // Market order: sweeps the opposite side at any price; whatever cannot trade is cancelled, it never rests.
    ExecReport submit_market(uint64_t id, Side side, uint32_t qty, std::vector<Fill>& out) {
        return submit(id, side, TimeInForce::IOC, side == Side::Buy ? max_price_ - 1 : 1, qty, out);
    }

    bool cancel(uint64_t id) {
        const size_t slot = map_slot(id);
        if (slot == kNoSlot) return false;
        remove_slot(slot);
        return true;
    }

    // Cancel/replace. new_qty is the new open quantity. Same price and a smaller (or equal) quantity keeps the
    // order's place in the queue. Anything else (price change, quantity increase) loses time priority: the order
    // is removed and re-entered at the back, and if the new price crosses the book it trades as an aggressor.
    ModifyResult modify(uint64_t id, uint32_t new_price, uint32_t new_qty, std::vector<Fill>& out, ExecReport* report = nullptr) {
        const size_t slot = map_slot(id);
        if (slot == kNoSlot) return ModifyResult::NotFound;
        if (new_qty == 0 || new_price == 0 || new_price >= max_price_) return ModifyResult::BadParam;
        Order& o = orders_[vals_[slot]];
        if (new_price == o.price && new_qty <= o.qty) {
            level(o.side, o.price).qty -= o.qty - new_qty;
            o.qty = new_qty;
            if (report) *report = {AddResult::Ok, 0, new_qty, 0};
            return ModifyResult::Ok;
        }
        const Side side = o.side;
        remove_slot(slot);
        const ExecReport r = submit(id, side, TimeInForce::GTC, new_price, new_qty, out);
        if (report) *report = r;
        return ModifyResult::Ok;
    }

    uint32_t best_bid() const noexcept { return has_bid_ ? best_bid_ : 0; }
    uint32_t best_ask() const noexcept { return has_ask_ ? best_ask_ : 0; }
    uint64_t qty_at(Side s, uint32_t price) const noexcept {
        return price < max_price_ ? (s == Side::Buy ? bids_[price].qty : asks_[price].qty) : 0;
    }
    size_t size() const noexcept { return live_; }

    // Full structural self-check, O(book size). For tests and debug builds, never the hot path.
    bool check_invariants(std::string* why = nullptr) const {
        auto fail = [&](const char* m) { if (why) *why = m; return false; };
        size_t seen = 0;
        bool hb = false, ha = false;
        uint32_t best_b = 0, best_a = 0;
        for (int sd = 0; sd < 2; ++sd) {
            const Side side = sd == 0 ? Side::Buy : Side::Sell;
            const std::vector<Level>& lv = sd == 0 ? bids_ : asks_;
            for (uint32_t p = 1; p < max_price_; ++p) {
                const Level& L = lv[p];
#ifndef MERIDIAN_BOOK_LINEAR_SCAN
                const bool bit = (sd == 0 ? bid_bm_ : ask_bm_).test(p);
#endif
                if (L.head == kNil) {
                    if (L.tail != kNil || L.qty != 0) return fail("empty level has stale tail/qty");
#ifndef MERIDIAN_BOOK_LINEAR_SCAN
                    if (bit) return fail("bitmap bit set for an empty level");
#endif
                    continue;
                }
#ifndef MERIDIAN_BOOK_LINEAR_SCAN
                if (!bit) return fail("bitmap bit clear for a non-empty level");
#endif
                uint64_t sum = 0;
                uint32_t prev = kNil;
                for (uint32_t idx = L.head; idx != kNil; idx = orders_[idx].next) {
                    const Order& o = orders_[idx];
                    if (++seen > orders_.size()) return fail("cycle in a level list");
                    if (o.prev != prev) return fail("prev link mismatch");
                    if (o.price != p || o.side != side) return fail("order stored at the wrong level");
                    if (o.qty == 0) return fail("zero-quantity resting order");
                    if (map_find(o.id) != idx) return fail("id map does not point at the order");
                    sum += o.qty;
                    prev = idx;
                }
                if (prev != L.tail) return fail("tail mismatch");
                if (sum != L.qty) return fail("level qty != sum of order qty");
                if (sd == 0) { hb = true; if (p > best_b) best_b = p; }
                else { if (!ha || p < best_a) best_a = p; ha = true; }
            }
        }
        if (seen != live_) return fail("live count != orders reachable from levels");
        if (hb != has_bid_ || (hb && best_b != best_bid_)) return fail("best bid wrong");
        if (ha != has_ask_ || (ha && best_a != best_ask_)) return fail("best ask wrong");
        if (has_bid_ && has_ask_ && best_bid_ >= best_ask_) return fail("book is crossed");
        size_t used = 0;
        for (uint32_t v : vals_) used += (v != kNil);
        if (used != live_) return fail("id map entry count != live count");
        size_t freed = 0;
        for (uint32_t i = free_; i != kNil; i = orders_[i].next)
            if (++freed > orders_.size()) return fail("cycle in free list");
        if (freed + live_ != orders_.size()) return fail("pool slots leaked or double-freed");
        return true;
    }

private:
    struct Order {
        uint64_t id = 0;
        uint32_t price = 0;
        uint32_t qty = 0;
        uint32_t prev = kNil;
        uint32_t next = kNil;
        Side side = Side::Buy;
    };
    struct Level {
        uint32_t head = kNil;
        uint32_t tail = kNil;
        uint64_t qty = 0;
    };
    static constexpr size_t kNoSlot = ~size_t{0};

    Level& level(Side s, uint32_t p) noexcept { return s == Side::Buy ? bids_[p] : asks_[p]; }

    void unlink(Level& L, const Order& o) noexcept {
        if (o.prev != kNil) orders_[o.prev].next = o.next; else L.head = o.next;
        if (o.next != kNil) orders_[o.next].prev = o.prev; else L.tail = o.prev;
    }

    void remove_slot(size_t slot) noexcept {
        const uint32_t idx = vals_[slot];
        Order& o = orders_[idx];
        Level& L = level(o.side, o.price);
        unlink(L, o);
        L.qty -= o.qty;
        const Side s = o.side;
        const uint32_t p = o.price;
        map_erase_slot(slot);
        release(idx);
        if (L.head == kNil) level_emptied(s, p);
    }

    // First non-empty ask level with price >= p, or kNil.
    uint32_t next_ask_ge(uint32_t p) const noexcept {
#ifdef MERIDIAN_BOOK_LINEAR_SCAN
        for (; p < max_price_; ++p) if (asks_[p].head != kNil) return p;
        return kNil;
#else
        const size_t n = ask_bm_.next_ge(p);
        return n == PriceBitmap::npos ? kNil : static_cast<uint32_t>(n);
#endif
    }
    // Last non-empty bid level with price <= p, or kNil.
    uint32_t prev_bid_le(uint32_t p) const noexcept {
#ifdef MERIDIAN_BOOK_LINEAR_SCAN
        for (; p >= 1; --p) if (bids_[p].head != kNil) return p;
        return kNil;
#else
        const size_t n = bid_bm_.prev_le(p);
        return n == PriceBitmap::npos ? kNil : static_cast<uint32_t>(n);
#endif
    }

    // Is there enough resting quantity at prices that cross `price` to fill `qty` completely?
    bool can_fill_fully(Side side, uint32_t price, uint32_t qty) const noexcept {
        uint64_t need = qty;
        if (side == Side::Buy) {
            for (uint32_t p = has_ask_ ? best_ask_ : kNil; p != kNil && p <= price; p = next_ask_ge(p + 1)) {
                if (asks_[p].qty >= need) return true;
                need -= asks_[p].qty;
            }
        } else {
            for (uint32_t p = has_bid_ ? best_bid_ : kNil; p != kNil && p >= price; p = (p > 1 ? prev_bid_le(p - 1) : kNil)) {
                if (bids_[p].qty >= need) return true;
                need -= bids_[p].qty;
            }
        }
        return false;
    }

    void release(uint32_t idx) noexcept {
        orders_[idx].next = free_;
        free_ = idx;
        --live_;
    }

    void rest(uint64_t id, Side side, uint32_t price, uint32_t qty) {
        const uint32_t idx = free_;
        Order& o = orders_[idx];
        free_ = o.next;
        o.id = id; o.price = price; o.qty = qty; o.side = side;
        Level& L = level(side, price);
        o.prev = L.tail;
        o.next = kNil;
        if (L.tail != kNil) orders_[L.tail].next = idx; else L.head = idx;
        L.tail = idx;
        L.qty += qty;
#ifndef MERIDIAN_BOOK_LINEAR_SCAN
        (side == Side::Buy ? bid_bm_ : ask_bm_).set(price);
#endif
        map_insert(id, idx);
        ++live_;
        if (side == Side::Buy) {
            if (!has_bid_ || price > best_bid_) { best_bid_ = price; has_bid_ = true; }
        } else {
            if (!has_ask_ || price < best_ask_) { best_ask_ = price; has_ask_ = true; }
        }
    }

    uint32_t fill_level(Side maker_side, uint32_t price, uint64_t taker, uint32_t want, std::vector<Fill>& out) {
        Level& L = level(maker_side, price);
        uint32_t done = 0;
        while (done < want && L.head != kNil) {
            const uint32_t idx = L.head;
            Order& m = orders_[idx];
            const uint32_t q = (m.qty < want - done) ? m.qty : want - done;
            out.push_back(Fill{m.id, taker, price, q});
            m.qty -= q;
            L.qty -= q;
            done += q;
            if (m.qty == 0) {
                L.head = m.next;
                if (L.head != kNil) orders_[L.head].prev = kNil; else L.tail = kNil;
                map_erase(m.id);
                release(idx);
            }
        }
        if (L.head == kNil) level_emptied(maker_side, price);
        return done;
    }

    // A level just emptied: if it was the best one, find the next non-empty level.
    // Default: hierarchical bitmap. MERIDIAN_BOOK_LINEAR_SCAN keeps the old linear scan for comparison.
    void level_emptied(Side s, uint32_t price) noexcept {
#ifndef MERIDIAN_BOOK_LINEAR_SCAN
        (s == Side::Buy ? bid_bm_ : ask_bm_).clear(price);
#endif
        if (s == Side::Sell) {
            if (!has_ask_ || price != best_ask_) return;
            const uint32_t n = next_ask_ge(price + 1);
            if (n == kNil) has_ask_ = false; else best_ask_ = n;
        } else {
            if (!has_bid_ || price != best_bid_) return;
            const uint32_t n = price > 1 ? prev_bid_le(price - 1) : kNil;
            if (n == kNil) has_bid_ = false; else best_bid_ = n;
        }
    }

    // --- open-addressing id map ---
    size_t hash(uint64_t id) const noexcept { return static_cast<size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_); }

    uint32_t map_find(uint64_t id) const noexcept {
        for (size_t i = hash(id);; i = (i + 1) & mask_) {
            if (vals_[i] == kNil) return kNil;
            if (keys_[i] == id) return vals_[i];
        }
    }
    size_t map_slot(uint64_t id) const noexcept {
        for (size_t i = hash(id);; i = (i + 1) & mask_) {
            if (vals_[i] == kNil) return kNoSlot;
            if (keys_[i] == id) return i;
        }
    }
    void map_insert(uint64_t id, uint32_t idx) noexcept {
        size_t i = hash(id);
        while (vals_[i] != kNil) i = (i + 1) & mask_;
        keys_[i] = id;
        vals_[i] = idx;
    }
    void map_erase(uint64_t id) noexcept { map_erase_slot(map_slot(id)); }
    void map_erase_slot(size_t i) noexcept {
        size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (vals_[j] == kNil) break;
            const size_t k = hash(keys_[j]);
            const bool k_in_gap = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (!k_in_gap) { keys_[i] = keys_[j]; vals_[i] = vals_[j]; i = j; }
        }
        vals_[i] = kNil;
    }

    uint32_t max_price_;
    std::vector<Order> orders_;
    std::vector<Level> bids_, asks_;
    PriceBitmap bid_bm_, ask_bm_;
    std::vector<uint64_t> keys_;
    std::vector<uint32_t> vals_;
    size_t mask_ = 0;
    unsigned shift_ = 0;
    uint32_t free_ = kNil;
    size_t live_ = 0;
    uint32_t best_bid_ = 0, best_ask_ = 0;
    bool has_bid_ = false, has_ask_ = false;
};

}  // namespace meridian::book
