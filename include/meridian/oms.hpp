#pragma once
// Order management on top of the order book: accounts, client order ids, risk limits, positions.
//
// What it adds to the bare book:
//  - every request carries (account, cl_ord_id). Sending the same request again returns the first answer with
//    duplicate = true and changes nothing; the same key with different contents is rejected (KeyReuse).
//    A client that times out and retries can therefore never trade twice.
//  - cancel and modify check that the order belongs to the account.
//  - risk limits: max order size, and a position limit that counts the open orders of the same side (worst case).
//  - positions and cash per account, kept as exact integers: position in lots, cash in ticks*lots.
//    equity(mark) = cash + position * mark. Across all accounts position and cash both sum to zero.
// It is a deterministic state machine: the same sequence of requests gives the same state and the same digest.
// No clock, no randomness, ids come from a counter. That is what lets it sit behind a Raft log later.
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "meridian/order_book.hpp"

namespace meridian::oms {

using book::Side;
using book::TimeInForce;

enum class Type : uint8_t { Limit, Market };
enum class Status : uint8_t { Rejected, Open, Filled, Cancelled };
enum class Reject : uint8_t { None, BadParam, QtyLimit, PositionLimit, PoolFull, UnknownOrder, NotOwner, NotOpen, KeyReuse };

struct Request {
    uint32_t account;
    uint64_t cl_ord_id;
    Side side;
    Type type;
    TimeInForce tif;     // ignored for market orders
    uint32_t price;      // ticks, ignored for market orders
    uint32_t qty;        // lots
    bool operator==(const Request& o) const noexcept {
        return account == o.account && cl_ord_id == o.cl_ord_id && side == o.side && type == o.type &&
               (type == Type::Market || (tif == o.tif && price == o.price)) && qty == o.qty;
    }
};

struct Result {
    Reject reject = Reject::None;
    uint64_t order_id = 0;
    Status status = Status::Rejected;
    uint32_t filled = 0, rested = 0, cancelled = 0;
    bool duplicate = false;
    std::vector<book::Fill> fills;
};

// Bit flags that deliberately break the logic, so the tests can prove they would notice (see test_oms).
enum Bug : uint32_t { kNoIdempotency = 1, kCancelAnyAccount = 2, kSkipOpenOnCancel = 4, kTakerNotCredited = 8 };

struct Config {
    uint32_t max_orders = 1u << 16;
    uint32_t max_price = 1000;
    uint32_t max_order_qty = 1000000;
    int64_t max_position = 1000000;   // lots, long or short, counting open orders of the same side
    uint32_t bugs = 0;
};

struct OrderRec {
    uint64_t id;
    uint32_t account;
    uint64_t cl_ord_id;
    Side side;
    Type type;
    TimeInForce tif;
    uint32_t price;
    uint32_t qty;        // filled + remaining (+ what was cancelled at entry)
    uint32_t filled;
    uint32_t remaining;  // resting in the book right now
    Status status;
};

struct Account {
    int64_t position = 0;   // lots, long positive
    int64_t cash = 0;       // ticks * lots, buys subtract and sells add
    int64_t open_buy = 0;   // lots resting on the bid side
    int64_t open_sell = 0;
};

class Oms {
public:
    explicit Oms(const Config& c) : cfg_(c), book_(c.max_orders, c.max_price) { fills_.reserve(256); }

    Result submit(const Request& r) {
        const auto key = std::make_pair(r.account, r.cl_ord_id);
        if (!(cfg_.bugs & kNoIdempotency)) {
            const auto it = seen_.find(key);
            if (it != seen_.end()) {
                Result x;
                if (!(it->second.req == r)) { x.reject = Reject::KeyReuse; return x; }
                x = it->second.res;
                x.duplicate = true;
                return x;
            }
        }
        Result res = do_submit(r);
        seen_[key] = Seen{r, res};
        return res;
    }

    Result cancel(uint32_t account, uint64_t order_id) {
        Result res;
        const auto it = orders_.find(order_id);
        if (it == orders_.end()) return fail(res, Reject::UnknownOrder);
        OrderRec& o = it->second;
        if (o.account != account && !(cfg_.bugs & kCancelAnyAccount)) return fail(res, Reject::NotOwner);
        if (o.status != Status::Open) return fail(res, Reject::NotOpen);
        book_.cancel(order_id);
        if (!(cfg_.bugs & kSkipOpenOnCancel)) add_open(accounts_[o.account], o.side, -static_cast<int64_t>(o.remaining));
        res.order_id = order_id;
        res.status = Status::Cancelled;
        res.cancelled = o.remaining;
        o.remaining = 0;
        o.status = Status::Cancelled;
        return res;
    }

    // new_qty is the new open quantity (book semantics): a price change or a larger quantity loses time priority
    // and may trade at once; the same price with a smaller quantity keeps its place.
    Result modify(uint32_t account, uint64_t order_id, uint32_t new_price, uint32_t new_qty) {
        Result res;
        const auto it = orders_.find(order_id);
        if (it == orders_.end()) return fail(res, Reject::UnknownOrder);
        OrderRec& o = it->second;
        if (o.account != account && !(cfg_.bugs & kCancelAnyAccount)) return fail(res, Reject::NotOwner);
        if (o.status != Status::Open) return fail(res, Reject::NotOpen);
        if (new_qty == 0 || new_price == 0 || new_price >= cfg_.max_price) return fail(res, Reject::BadParam);
        if (new_qty > cfg_.max_order_qty) return fail(res, Reject::QtyLimit);
        Account& a = accounts_[o.account];
        const int64_t extra = new_qty > o.remaining ? new_qty - o.remaining : 0;
        if (exposure(a, o.side) + extra > cfg_.max_position) return fail(res, Reject::PositionLimit);
        fills_.clear();
        book::ExecReport er{};
        if (book_.modify(order_id, new_price, new_qty, fills_, &er) != book::ModifyResult::Ok) return fail(res, Reject::BadParam);
        add_open(a, o.side, -static_cast<int64_t>(o.remaining));
        apply_fills(fills_);
        o.price = new_price;
        o.filled += er.filled;
        o.remaining = er.rested;
        o.qty = o.filled + o.remaining;
        o.status = o.remaining > 0 ? Status::Open : Status::Filled;
        add_open(a, o.side, o.remaining);
        res.order_id = order_id;
        res.status = o.status;
        res.filled = er.filled;
        res.rested = er.rested;
        res.fills = fills_;
        return res;
    }

    const OrderRec* order(uint64_t id) const { const auto it = orders_.find(id); return it == orders_.end() ? nullptr : &it->second; }
    const Account* account(uint32_t id) const { const auto it = accounts_.find(id); return it == accounts_.end() ? nullptr : &it->second; }
    const std::map<uint32_t, Account>& accounts() const noexcept { return accounts_; }
    const book::OrderBook& book() const noexcept { return book_; }
    const Config& config() const noexcept { return cfg_; }
    uint64_t next_order_id() const noexcept { return next_id_; }

    std::vector<OrderRec> open_orders(uint32_t account) const {
        std::vector<OrderRec> v;
        for (uint64_t id = 1; id < next_id_; ++id) {
            const auto it = orders_.find(id);
            if (it != orders_.end() && it->second.account == account && it->second.status == Status::Open) v.push_back(it->second);
        }
        return v;
    }

    // Internal consistency. Returns false and says why; the tests call this after every operation.
    bool check(std::string* why = nullptr) const {
        auto fail_ = [&](const std::string& m) { if (why) *why = m; return false; };
        if (!book_.check_invariants(why)) return false;
        int64_t pos = 0, cash = 0;
        for (const auto& kv : accounts_) { pos += kv.second.position; cash += kv.second.cash; }
        if (pos != 0) return fail_("positions do not sum to zero");
        if (cash != 0) return fail_("cash does not sum to zero");
        std::map<uint32_t, std::pair<int64_t, int64_t>> open;
        int64_t rest_buy = 0, rest_sell = 0;
        for (const auto& kv : orders_) {
            const OrderRec& o = kv.second;
            if (o.status == Status::Open) {
                if (o.remaining == 0) return fail_("open order with nothing remaining");
                (o.side == Side::Buy ? open[o.account].first : open[o.account].second) += o.remaining;
                (o.side == Side::Buy ? rest_buy : rest_sell) += o.remaining;
            } else if (o.remaining != 0) return fail_("closed order with quantity remaining");
            if (o.filled + o.remaining > o.qty) return fail_("order over-filled");
        }
        for (const auto& kv : accounts_) {
            const auto it = open.find(kv.first);
            const int64_t b = it == open.end() ? 0 : it->second.first, s = it == open.end() ? 0 : it->second.second;
            if (kv.second.open_buy != b || kv.second.open_sell != s) return fail_("open quantity of account " + std::to_string(kv.first) + " is wrong");
        }
        int64_t book_buy = 0, book_sell = 0;
        for (uint32_t p = 1; p < cfg_.max_price; ++p) { book_buy += book_.qty_at(Side::Buy, p); book_sell += book_.qty_at(Side::Sell, p); }
        if (book_buy != rest_buy || book_sell != rest_sell) return fail_("resting quantity in the book differs from the open orders");
        return true;
    }

    // Everything visible, folded into one number: equal digests mean equal state.
    uint64_t digest() const {
        uint64_t h = 0xCBF29CE484222325ull;
        auto mix = [&](uint64_t x) { h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0x100000001B3ull; };
        for (uint32_t p = 1; p < cfg_.max_price; ++p) { mix(book_.qty_at(Side::Buy, p)); mix(book_.qty_at(Side::Sell, p) << 1); }
        for (const auto& kv : accounts_) { mix(kv.first); mix(static_cast<uint64_t>(kv.second.position)); mix(static_cast<uint64_t>(kv.second.cash)); mix(static_cast<uint64_t>(kv.second.open_buy)); mix(static_cast<uint64_t>(kv.second.open_sell)); }
        for (uint64_t id = 1; id < next_id_; ++id) {
            const auto it = orders_.find(id);
            if (it == orders_.end()) continue;
            const OrderRec& o = it->second;
            mix(o.id); mix(o.account); mix(o.filled); mix(o.remaining); mix(static_cast<uint64_t>(o.status));
        }
        mix(next_id_);
        return h;
    }

private:
    struct Seen { Request req; Result res; };

    static Result& fail(Result& r, Reject why) { r.reject = why; r.status = Status::Rejected; return r; }
    static void add_open(Account& a, Side s, int64_t d) { (s == Side::Buy ? a.open_buy : a.open_sell) += d; }
    static int64_t exposure(const Account& a, Side s) { return s == Side::Buy ? a.position + a.open_buy : -a.position + a.open_sell; }

    // The maker's record shrinks, and both sides' position and cash move. The taker's own record is set by the caller
    // from the execution report.
    void apply_fills(const std::vector<book::Fill>& fills) {
        for (const book::Fill& f : fills) {
            OrderRec& mk = orders_.at(f.maker_id);
            const OrderRec& tk = orders_.at(f.taker_id);
            mk.filled += f.qty;
            mk.remaining -= f.qty;
            add_open(accounts_[mk.account], mk.side, -static_cast<int64_t>(f.qty));
            if (mk.remaining == 0) mk.status = Status::Filled;
            const uint32_t buyer = mk.side == Side::Buy ? mk.account : tk.account;
            const uint32_t seller = mk.side == Side::Buy ? tk.account : mk.account;
            const int64_t notional = static_cast<int64_t>(f.price) * f.qty;
            Account& b = accounts_[buyer];
            Account& s = accounts_[seller];
            const bool skip_taker = (cfg_.bugs & kTakerNotCredited) != 0;
            if (!(skip_taker && buyer == tk.account)) { b.position += f.qty; b.cash -= notional; }
            if (!(skip_taker && seller == tk.account)) { s.position -= f.qty; s.cash += notional; }
        }
    }

    Result do_submit(const Request& r) {
        Result res;
        if (r.qty == 0 || (r.type == Type::Limit && (r.price == 0 || r.price >= cfg_.max_price))) return fail(res, Reject::BadParam);
        if (r.qty > cfg_.max_order_qty) return fail(res, Reject::QtyLimit);
        Account& a = accounts_[r.account];
        if (exposure(a, r.side) + r.qty > cfg_.max_position) return fail(res, Reject::PositionLimit);
        const uint64_t id = next_id_++;
        OrderRec rec{id, r.account, r.cl_ord_id, r.side, r.type, r.type == Type::Market ? TimeInForce::IOC : r.tif, r.type == Type::Market ? 0u : r.price, r.qty, 0, r.qty, Status::Open};
        orders_[id] = rec;
        fills_.clear();
        const book::ExecReport er = r.type == Type::Market ? book_.submit_market(id, r.side, r.qty, fills_)
                                                           : book_.submit(id, r.side, r.tif, r.price, r.qty, fills_);
        if (er.status != book::AddResult::Ok) {
            orders_.erase(id);
            return fail(res, er.status == book::AddResult::PoolFull ? Reject::PoolFull : Reject::BadParam);
        }
        apply_fills(fills_);
        OrderRec& o = orders_[id];
        o.filled = er.filled;
        o.remaining = er.rested;
        o.status = er.rested > 0 ? Status::Open : (er.filled == r.qty ? Status::Filled : Status::Cancelled);
        add_open(a, r.side, er.rested);
        res.order_id = id;
        res.status = o.status;
        res.filled = er.filled;
        res.rested = er.rested;
        res.cancelled = er.cancelled;
        res.fills = fills_;
        return res;
    }

    Config cfg_;
    book::OrderBook book_;
    std::vector<book::Fill> fills_;
    std::unordered_map<uint64_t, OrderRec> orders_;
    std::map<uint32_t, Account> accounts_;
    std::map<std::pair<uint32_t, uint64_t>, Seen> seen_;
    uint64_t next_id_ = 1;
};

}  // namespace meridian::oms
