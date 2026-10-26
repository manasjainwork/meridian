#pragma once
#include <cstdint>
#include <vector>
#include "meridian/journal.hpp"
#include "meridian/order_book.hpp"

namespace meridian::journal {

// Order-sensitive 64-bit digest (not cryptographic). Two runs are "the same" iff the digests match.
class Digest {
public:
    void mix(uint64_t x) noexcept {
        h_ ^= x;
        h_ *= 0x9E3779B97F4A7C15ull;
        h_ ^= h_ >> 32;
    }
    uint64_t value() const noexcept { return h_; }

private:
    uint64_t h_ = 0xCBF29CE484222325ull;
};

// The ONE place an input event changes the book. Live processing and replay both call this,
// so they cannot drift apart. Result code and every fill go into the digest.
inline void apply(book::OrderBook& b, const Record& r, std::vector<book::Fill>& fills, Digest& d) {
    fills.clear();
    if (r.type == Type::Add) {
        const auto res = b.add_limit(r.id, r.side, r.price, r.qty, fills);
        d.mix(static_cast<uint64_t>(res));
        for (const auto& f : fills) { d.mix(f.maker_id); d.mix(f.taker_id); d.mix(f.price); d.mix(f.qty); }
    } else {
        d.mix(static_cast<uint64_t>(b.cancel(r.id)) + 100);
    }
}

// Final book state: best prices, live order count, and the resting quantity at every price.
inline void mix_state(const book::OrderBook& b, uint32_t max_price, Digest& d) {
    d.mix(b.best_bid());
    d.mix(b.best_ask());
    d.mix(b.size());
    for (uint32_t p = 1; p < max_price; ++p) { d.mix(b.qty_at(book::Side::Buy, p)); d.mix(b.qty_at(book::Side::Sell, p)); }
}

inline uint64_t replay(const std::vector<Record>& recs, book::OrderBook& b, uint32_t max_price) {
    Digest d;
    std::vector<book::Fill> fills;
    fills.reserve(64);
    for (const Record& r : recs) apply(b, r, fills, d);
    mix_state(b, max_price, d);
    return d.value();
}

}  // namespace meridian::journal
