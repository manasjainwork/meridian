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
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "meridian/order_book.hpp"

namespace meridian::oms {

using book::Side;
using book::TimeInForce;

enum class Type : uint8_t { Limit, Market, Stop, StopLimit };
// Pending: a stop order waiting for its trigger price. Triggered: it fired and became a normal order (see OrderRec::child).
enum class Status : uint8_t { Rejected, Open, Filled, Cancelled, Pending, Triggered };
enum class Reject : uint8_t { None, BadParam, QtyLimit, PositionLimit, PoolFull, UnknownOrder, NotOwner, NotOpen, KeyReuse, SelfTrade };

struct Request {
    uint32_t account;
    uint64_t cl_ord_id;
    Side side;
    Type type;
    TimeInForce tif;     // ignored for market orders
    uint32_t price;      // ticks, ignored for market orders
    uint32_t qty;        // lots
    uint32_t stop_price = 0;   // ticks, only for Stop and StopLimit
    bool operator==(const Request& o) const noexcept {
        return account == o.account && cl_ord_id == o.cl_ord_id && side == o.side && type == o.type &&
               (type == Type::Market || type == Type::Stop || (tif == o.tif && price == o.price)) && qty == o.qty && stop_price == o.stop_price;
    }
};

// A short list of fills that lives inside the Result: up to kInline fills need no allocation, a longer one moves to a vector.
class FillList {
public:
    static constexpr uint32_t kInline = 4;
    FillList() = default;
    FillList(const FillList& o) { assign(o.begin(), o.end()); }
    FillList& operator=(const FillList& o) { if (this != &o) assign(o.begin(), o.end()); return *this; }
    size_t size() const noexcept { return n_; }
    bool empty() const noexcept { return n_ == 0; }
    const book::Fill* begin() const noexcept { return data(); }
    const book::Fill* end() const noexcept { return data() + n_; }
    const book::Fill& operator[](size_t i) const noexcept { return data()[i]; }
    void clear() noexcept { n_ = 0; spilled_ = false; heap_.clear(); }
    void push_back(const book::Fill& f) {
        if (!spilled_) {
            if (n_ < kInline) { inl_[n_++] = f; return; }
            heap_.assign(inl_, inl_ + n_);
            spilled_ = true;
        }
        heap_.push_back(f);
        ++n_;
    }
    template <class It> void append(It b, It e) { for (; b != e; ++b) push_back(*b); }
    template <class It> void assign(It b, It e) { clear(); append(b, e); }
private:
    const book::Fill* data() const noexcept { return spilled_ ? heap_.data() : inl_; }
    uint32_t n_ = 0;
    bool spilled_ = false;
    book::Fill inl_[kInline];
    std::vector<book::Fill> heap_;
};

struct Result {
    Reject reject = Reject::None;
    uint64_t order_id = 0;
    Status status = Status::Rejected;
    uint32_t filled = 0, rested = 0, cancelled = 0;
    bool duplicate = false;
    FillList fills;
    std::vector<book::Fill> triggered;   // fills of stop orders that fired because of this operation, in order
};

// Bit flags that deliberately break the logic, so the tests can prove they would notice (see test_oms).
enum Bug : uint32_t { kNoIdempotency = 1, kCancelAnyAccount = 2, kSkipOpenOnCancel = 4, kTakerNotCredited = 8, kNoSelfTrade = 16, kStopNeverTriggers = 32, kStopWrongWay = 64 };

struct Config {
    uint32_t max_orders = 1u << 16;
    uint32_t max_price = 1000;
    uint32_t max_order_qty = 1000000;
    int64_t max_position = 1000000;   // lots, long or short, counting open orders of the same side
    uint32_t bugs = 0;
    uint32_t order_history = 0;        // order records kept in full, in ids; 0 means max_orders. Older finished orders are dropped.
    uint32_t seen_window = 1u << 16;   // idempotency memory: the last this-many requests; an older duplicate is treated as new
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
    uint32_t stop = 0;   // trigger price in ticks (stop orders)
    uint64_t child = 0;  // the order a stop became when it fired, 0 if it did not become one
};

struct Account {
    int64_t position = 0;   // lots, long positive
    int64_t cash = 0;       // ticks * lots, buys subtract and sells add
    int64_t open_buy = 0;   // lots resting on the bid side
    int64_t open_sell = 0;
};

// Order records in fixed chunks of 1024 ids, so a new order costs no allocation except one chunk per 1024 orders.
// A chunk older than `keep` chunks is freed; the records in it that are still live (open or pending) move to a small
// map, and finished ones are forgotten. order() on a forgotten id returns nullptr. Memory is bounded by the kept
// chunks plus the live orders, not by the number of orders ever placed.
class OrderStore {
public:
    static constexpr uint32_t kChunk = 1024;
    OrderStore(uint32_t keep_chunks, uint32_t sweep_limit) : keep_(keep_chunks ? keep_chunks : 1), sweep_limit_(sweep_limit), base_limit_(sweep_limit) { chunks_.reserve(4096); }
    OrderRec* find(uint64_t id) noexcept {
        const uint64_t c = id / kChunk;
        if (c < chunks_.size() && chunks_[c]) { Chunk& ch = *chunks_[c]; const uint32_t i = static_cast<uint32_t>(id % kChunk); return ch.used[i] ? &ch.r[i] : nullptr; }
        const auto it = old_.find(id);
        return it == old_.end() ? nullptr : &it->second;
    }
    const OrderRec* find(uint64_t id) const noexcept { return const_cast<OrderStore*>(this)->find(id); }
    OrderRec& at(uint64_t id) { OrderRec* p = find(id); if (!p) throw std::out_of_range("no such order"); return *p; }
    const OrderRec& at(uint64_t id) const { const OrderRec* p = find(id); if (!p) throw std::out_of_range("no such order"); return *p; }
    OrderRec& put(uint64_t id, const OrderRec& rec) {
        const uint64_t c = id / kChunk;
        if (c >= chunks_.size()) chunks_.resize(c + 1);
        if (!chunks_[c]) { chunks_[c] = std::make_unique<Chunk>(); retire_older(c); }
        Chunk& ch = *chunks_[c];
        const uint32_t i = static_cast<uint32_t>(id % kChunk);
        ch.r[i] = rec; ch.used[i] = true;
        return ch.r[i];
    }
    void erase(uint64_t id) {
        const uint64_t c = id / kChunk;
        if (c < chunks_.size() && chunks_[c]) chunks_[c]->used[id % kChunk] = false; else old_.erase(id);
    }
    // f(const OrderRec&) -> bool; stops and returns false as soon as f returns false. Order is not defined.
    template <class F> bool all(F f) const {
        for (const auto& kv : old_) if (!f(kv.second)) return false;
        for (const auto& up : chunks_) if (up) for (uint32_t i = 0; i < kChunk; ++i) if (up->used[i] && !f(up->r[i])) return false;
        return true;
    }
    // Ascending id order.
    template <class F> void each_ordered(F f) const {
        std::vector<uint64_t> ids;
        ids.reserve(old_.size());
        for (const auto& kv : old_) ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end());
        for (const uint64_t id : ids) f(old_.at(id));
        for (const auto& up : chunks_) if (up) for (uint32_t i = 0; i < kChunk; ++i) if (up->used[i]) f(up->r[i]);
    }
private:
    struct Chunk { OrderRec r[kChunk]; bool used[kChunk]; };
    static bool live(const OrderRec& o) noexcept { return o.status == Status::Open || o.status == Status::Pending; }
    void retire_older(uint64_t c) {
        if (c < keep_) return;
        for (; retire_ <= c - keep_; ++retire_) {
            auto& up = chunks_[retire_];
            if (!up) continue;
            for (uint32_t i = 0; i < kChunk; ++i) if (up->used[i] && live(up->r[i])) old_.emplace(retire_ * kChunk + i, up->r[i]);
            up.reset();
        }
        if (old_.size() > sweep_limit_) {
            for (auto it = old_.begin(); it != old_.end();) it = live(it->second) ? std::next(it) : old_.erase(it);
            sweep_limit_ = std::max<size_t>(base_limit_, 2 * old_.size());
        }
    }
    uint64_t keep_;
    uint64_t retire_ = 0;
    size_t sweep_limit_, base_limit_;
    std::vector<std::unique_ptr<Chunk>> chunks_;
    std::unordered_map<uint64_t, OrderRec> old_;
};

class Oms {
public:
    explicit Oms(const Config& c) : cfg_(c), book_(c.max_orders, c.max_price),
          orders_((std::max(c.order_history ? c.order_history : c.max_orders, 1024u) + OrderStore::kChunk - 1) / OrderStore::kChunk + 1, 2 * c.max_orders + 1024),
          seen_(c.seen_window ? c.seen_window : 1) {
        fills_.reserve(256);
        uint32_t b = 1;
        while (b < seen_.size()) b <<= 1;
        seen_head_.assign(b, kNil);
    }

    Result submit(const Request& r) {
        if (!(cfg_.bugs & kNoIdempotency)) {
            const Seen* sp = seen_find(r.account, r.cl_ord_id);
            if (sp) {
                Result x;
                if (!(sp->req == r)) { x.reject = Reject::KeyReuse; return x; }
                x = sp->res;
                x.duplicate = true;
                return x;
            }
        }
        Result res = do_submit(r);
        run_triggers(res.triggered);
        if (res.status == Status::Pending) res.status = orders_.at(res.order_id).status;   // a stop already past its price fired at once
        seen_put(r, res);
        return res;
    }

    Result cancel(uint32_t account, uint64_t order_id) {
        Result res;
        OrderRec* op = orders_.find(order_id);
        if (!op) return fail(res, Reject::UnknownOrder);
        OrderRec& o = *op;
        if (o.account != account && !(cfg_.bugs & kCancelAnyAccount)) return fail(res, Reject::NotOwner);
        if (o.status == Status::Pending) {                       // a stop order that has not fired: nothing is in the book
            pending_.erase(order_id);
            res.order_id = order_id; res.status = Status::Cancelled; res.cancelled = o.remaining;
            o.remaining = 0; o.status = Status::Cancelled;
            return res;
        }
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
        OrderRec* op = orders_.find(order_id);
        if (!op) return fail(res, Reject::UnknownOrder);
        OrderRec& o = *op;
        if (o.account != account && !(cfg_.bugs & kCancelAnyAccount)) return fail(res, Reject::NotOwner);
        if (o.status != Status::Open) return fail(res, Reject::NotOpen);
        if (new_qty == 0 || new_price == 0 || new_price >= cfg_.max_price) return fail(res, Reject::BadParam);
        if (new_qty > cfg_.max_order_qty) return fail(res, Reject::QtyLimit);
        if (would_self_trade(o.account, o.side, Type::Limit, new_price, order_id)) return fail(res, Reject::SelfTrade);
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
        res.fills.assign(fills_.begin(), fills_.end());
        run_triggers(res.triggered);
        return res;
    }

    const OrderRec* order(uint64_t id) const { return orders_.find(id); }
    const Account* account(uint32_t id) const { const auto it = accounts_.find(id); return it == accounts_.end() ? nullptr : &it->second; }
    const std::map<uint32_t, Account>& accounts() const noexcept { return accounts_; }
    const book::OrderBook& book() const noexcept { return book_; }
    const Config& config() const noexcept { return cfg_; }
    uint64_t next_order_id() const noexcept { return next_id_; }
    uint32_t last_price() const noexcept { return last_px_; }   // price of the latest fill, 0 before the first
    std::vector<OrderRec> pending_stops(uint32_t account) const {
        std::vector<OrderRec> v;
        for (const uint64_t id : pending_) { const OrderRec& o = orders_.at(id); if (o.account == account) v.push_back(o); }
        return v;
    }

    std::vector<OrderRec> open_orders(uint32_t account) const {
        std::vector<OrderRec> v;
        orders_.each_ordered([&](const OrderRec& o) { if (o.account == account && o.status == Status::Open) v.push_back(o); });
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
        const bool orders_ok = orders_.all([&](const OrderRec& o) {
            if (o.status == Status::Open) {
                if (o.remaining == 0) return fail_("open order with nothing remaining");
                (o.side == Side::Buy ? open[o.account].first : open[o.account].second) += o.remaining;
                (o.side == Side::Buy ? rest_buy : rest_sell) += o.remaining;
            } else if (o.status != Status::Pending && o.remaining != 0) return fail_("closed order with quantity remaining");
            if (o.status == Status::Pending && stop_hit(o.side, o.stop, last_px_, false)) return fail_("a stop order that should have fired is still pending");
            if (o.filled + o.remaining > o.qty) return fail_("order over-filled");
            return true;
        });
        if (!orders_ok) return false;
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
        orders_.each_ordered([&](const OrderRec& o) {
            mix(o.id); mix(o.account); mix(o.filled); mix(o.remaining); mix(static_cast<uint64_t>(o.status)); mix(o.stop);
        });
        mix(next_id_);
        return h;
    }

private:
    static constexpr uint32_t kNil = 0xFFFFFFFFu;
    struct Seen { Request req{}; Result res; uint32_t next = kNil; bool used = false; };
    // Fixed ring of the last seen_window requests, indexed by a chained hash made of two flat arrays: no node allocations.
    static uint32_t hkey(uint32_t a, uint64_t c) noexcept {
        uint64_t x = c * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(a) << 32 | a);
        x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
        return static_cast<uint32_t>(x);
    }
    const Seen* seen_find(uint32_t a, uint64_t c) const noexcept {
        for (uint32_t i = seen_head_[hkey(a, c) & (seen_head_.size() - 1)]; i != kNil; i = seen_[i].next)
            if (seen_[i].req.account == a && seen_[i].req.cl_ord_id == c) return &seen_[i];
        return nullptr;
    }
    void seen_put(const Request& r, const Result& res) {
        const uint32_t slot = static_cast<uint32_t>(seen_count_++ % seen_.size());
        Seen& s = seen_[slot];
        if (s.used) {   // evict the oldest entry from its chain
            uint32_t* link = &seen_head_[hkey(s.req.account, s.req.cl_ord_id) & (seen_head_.size() - 1)];
            while (*link != slot) link = &seen_[*link].next;
            *link = s.next;
        }
        s.req = r; s.res = res; s.used = true;   // copy-assign reuses the vectors' capacity once warm
        uint32_t& head = seen_head_[hkey(r.account, r.cl_ord_id) & (seen_head_.size() - 1)];
        s.next = head; head = slot;
    }

    static Result& fail(Result& r, Reject why) { r.reject = why; r.status = Status::Rejected; return r; }
    static void add_open(Account& a, Side s, int64_t d) { (s == Side::Buy ? a.open_buy : a.open_sell) += d; }
    // Self-trade prevention, "reject the incoming order": refused if it could match one of the same account's own
    // resting orders. A market order is refused if the account has any resting order on the other side.
    bool would_self_trade(uint32_t account, Side side, Type type, uint32_t price, uint64_t ignore_id) const {
        if (cfg_.bugs & kNoSelfTrade) return false;
        const auto ai = accounts_.find(account);
        if (ai == accounts_.end() || (side == Side::Buy ? ai->second.open_sell : ai->second.open_buy) == 0) return false;
        return !orders_.all([&](const OrderRec& o) {
            if (o.account != account || o.status != Status::Open || o.remaining == 0 || o.side == side || o.id == ignore_id) return true;
            if (type == Type::Market) return false;
            return !(side == Side::Buy ? o.price <= price : o.price >= price);
        });
    }
    static int64_t exposure(const Account& a, Side s) { return s == Side::Buy ? a.position + a.open_buy : -a.position + a.open_sell; }

    // The maker's record shrinks, and both sides' position and cash move. The taker's own record is set by the caller
    // from the execution report.
    void apply_fills(const std::vector<book::Fill>& fills) {
        for (const book::Fill& f : fills) {
            last_px_ = f.price;
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

    // Stop orders sit outside the book. A buy stop fires when the last trade price is >= its stop price, a sell stop when it is
    // <= . Stop: becomes a market order. StopLimit: becomes a limit order at its price. At the trigger the order goes through
    // the same checks as any new order (position limit, self-trade); if refused it is cancelled. Entry checks size and limit only.
    static bool stop_hit(Side s, uint32_t stop, uint32_t last, bool wrong_way) {
        if (last == 0) return false;
        return ((s == Side::Buy) != wrong_way) ? last >= stop : last <= stop;
    }

    Result place_stop(const Request& r) {
        Result res;
        if (r.qty == 0 || r.stop_price == 0 || r.stop_price >= cfg_.max_price || (r.type == Type::StopLimit && (r.price == 0 || r.price >= cfg_.max_price))) return fail(res, Reject::BadParam);
        if (r.qty > cfg_.max_order_qty) return fail(res, Reject::QtyLimit);
        if (exposure(accounts_[r.account], r.side) + r.qty > cfg_.max_position) return fail(res, Reject::PositionLimit);
        const uint64_t id = next_id_++;
        OrderRec rec{id, r.account, r.cl_ord_id, r.side, r.type, TimeInForce::GTC, r.type == Type::StopLimit ? r.price : 0u, r.qty, 0, r.qty, Status::Pending};
        rec.stop = r.stop_price;
        orders_.put(id, rec);
        pending_.insert(id);
        res.order_id = id;
        res.status = Status::Pending;
        return res;
    }

    // Fires every pending stop that the last price has reached, lowest order id first; a fired stop's trades can fire more.
    void run_triggers(std::vector<book::Fill>& out) {
        if (cfg_.bugs & kStopNeverTriggers) return;
        for (;;) {
            uint64_t pick = 0;
            for (const uint64_t id : pending_) {
                const OrderRec& o = orders_.at(id);
                if (stop_hit(o.side, o.stop, last_px_, (cfg_.bugs & kStopWrongWay) != 0)) { pick = id; break; }
            }
            if (pick == 0) return;
            pending_.erase(pick);
            const OrderRec st = orders_.at(pick);
            Request child{st.account, 0, st.side, st.type == Type::Stop ? Type::Market : Type::Limit, TimeInForce::GTC, st.price, st.qty};
            const Result cr = do_submit(child);
            OrderRec& s2 = orders_.at(pick);
            s2.remaining = 0;
            s2.status = cr.reject == Reject::None ? Status::Triggered : Status::Cancelled;
            s2.child = cr.reject == Reject::None ? cr.order_id : 0;
            out.insert(out.end(), cr.fills.begin(), cr.fills.end());
        }
    }

    Result do_submit(const Request& r) {
        Result res;
        if (r.type == Type::Stop || r.type == Type::StopLimit) return place_stop(r);
        if (r.qty == 0 || (r.type == Type::Limit && (r.price == 0 || r.price >= cfg_.max_price))) return fail(res, Reject::BadParam);
        if (r.qty > cfg_.max_order_qty) return fail(res, Reject::QtyLimit);
        if (would_self_trade(r.account, r.side, r.type, r.price, 0)) return fail(res, Reject::SelfTrade);
        Account& a = accounts_[r.account];
        if (exposure(a, r.side) + r.qty > cfg_.max_position) return fail(res, Reject::PositionLimit);
        const uint64_t id = next_id_++;
        OrderRec rec{id, r.account, r.cl_ord_id, r.side, r.type, r.type == Type::Market ? TimeInForce::IOC : r.tif, r.type == Type::Market ? 0u : r.price, r.qty, 0, r.qty, Status::Open};
        orders_.put(id, rec);
        fills_.clear();
        const book::ExecReport er = r.type == Type::Market ? book_.submit_market(id, r.side, r.qty, fills_)
                                                           : book_.submit(id, r.side, r.tif, r.price, r.qty, fills_);
        if (er.status != book::AddResult::Ok) {
            orders_.erase(id);
            return fail(res, er.status == book::AddResult::PoolFull ? Reject::PoolFull : Reject::BadParam);
        }
        apply_fills(fills_);
        OrderRec& o = orders_.at(id);
        o.filled = er.filled;
        o.remaining = er.rested;
        o.status = er.rested > 0 ? Status::Open : (er.filled == r.qty ? Status::Filled : Status::Cancelled);
        add_open(a, r.side, er.rested);
        res.order_id = id;
        res.status = o.status;
        res.filled = er.filled;
        res.rested = er.rested;
        res.cancelled = er.cancelled;
        res.fills.assign(fills_.begin(), fills_.end());
        return res;
    }

    Config cfg_;
    book::OrderBook book_;
    std::vector<book::Fill> fills_;
    OrderStore orders_;
    std::map<uint32_t, Account> accounts_;
    std::vector<Seen> seen_;
    std::vector<uint32_t> seen_head_;
    uint64_t seen_count_ = 0;
    uint64_t next_id_ = 1;
    uint32_t last_px_ = 0;
    std::set<uint64_t> pending_;   // ids of stop orders waiting, ascending
};

}  // namespace meridian::oms
