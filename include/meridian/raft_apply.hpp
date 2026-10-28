#pragma once
// Connects the replicated log to the order book.
// A Raft entry's `cmd` is a client request number k >= 1; the request itself is script[k-1] (cmd 0 is Raft's no-op).
// Every replica applies committed entries, in index order, with the same deterministic Replica::apply.
// What this gives: if two replicas apply the same committed prefix, their books are identical (digest equal).
#include <cstdint>
#include <random>
#include <vector>
#include "meridian/order_book.hpp"

namespace meridian::raft {

struct Op {
    enum Kind : uint8_t { Gtc, Ioc, Fok, Market, Cancel, Modify } kind;
    book::Side side;
    uint32_t price, qty;
    uint64_t id;                 // new order id, or the id to cancel / modify
};

inline std::vector<Op> make_script(uint64_t seed, size_t n) {
    std::mt19937_64 rng(seed);
    std::vector<Op> s;
    s.reserve(n);
    uint32_t mid = 500;
    for (size_t i = 0; i < n; ++i) {
        if ((rng() & 63) == 0) mid = mid + ((rng() & 1) ? 1 : -1);
        const uint64_t id = i + 1;                                       // unique per request
        const uint64_t roll = rng() % 100;
        const book::Side side = (rng() & 1) ? book::Side::Buy : book::Side::Sell;
        const uint32_t price = mid - 5 + (uint32_t)(rng() % 11), qty = 1 + (uint32_t)(rng() % 100);
        const uint64_t target = i ? 1 + rng() % i : 1;                   // an earlier request (may be gone already: that is fine)
        if (roll < 55) s.push_back({Op::Gtc, side, price, qty, id});
        else if (roll < 62) s.push_back({Op::Ioc, side, price, qty, id});
        else if (roll < 67) s.push_back({Op::Fok, side, price, qty, id});
        else if (roll < 72) s.push_back({Op::Market, side, 0, qty, id});
        else if (roll < 90) s.push_back({Op::Cancel, side, 0, 0, target});
        else s.push_back({Op::Modify, side, price, qty, target});
    }
    return s;
}

class Replica {
public:
    explicit Replica(uint32_t max_orders = 1u << 16) : book_(max_orders, 1000) { fills_.reserve(256); }

    void apply(const Op& op) {
        fills_.clear();
        switch (op.kind) {
            case Op::Gtc: book_.submit(op.id, op.side, book::TimeInForce::GTC, op.price, op.qty, fills_); break;
            case Op::Ioc: book_.submit(op.id, op.side, book::TimeInForce::IOC, op.price, op.qty, fills_); break;
            case Op::Fok: book_.submit(op.id, op.side, book::TimeInForce::FOK, op.price, op.qty, fills_); break;
            case Op::Market: book_.submit_market(op.id, op.side, op.qty, fills_); break;
            case Op::Cancel: (void)book_.cancel(op.id); break;
            case Op::Modify: (void)book_.modify(op.id, op.price, op.qty, fills_); break;
        }
        for (const book::Fill& f : fills_) { ++trades_; volume_ += f.qty; }
        ++applied_;
    }
    // cmd 0 = Raft no-op: nothing to apply.
    void apply_cmd(const std::vector<Op>& script, uint64_t cmd) { if (cmd != 0) apply(script[cmd - 1]); }

    uint64_t applied() const noexcept { return applied_; }
    uint64_t trades() const noexcept { return trades_; }
    const book::OrderBook& book() const noexcept { return book_; }

    // Everything visible about the book plus the totals, folded into one number.
    uint64_t digest() const {
        uint64_t h = 0xCBF29CE484222325ull;
        auto mix = [&](uint64_t x) { h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0x100000001B3ull; };
        for (uint32_t p = 1; p < 1000; ++p) { mix(book_.qty_at(book::Side::Buy, p)); mix(book_.qty_at(book::Side::Sell, p) << 1); }
        mix(book_.size()); mix(trades_); mix(volume_); mix(applied_);
        return h;
    }

private:
    book::OrderBook book_;
    std::vector<book::Fill> fills_;
    uint64_t trades_ = 0, volume_ = 0, applied_ = 0;
};

}  // namespace meridian::raft
