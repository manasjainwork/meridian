#pragma once
// Connects the replicated log to the order management layer (accounts, positions, cash, idempotency, stops).
// Like raft_apply.hpp, a Raft entry's `cmd` is a client request number k >= 1 and the request itself is script[k-1]
// (cmd 0 is Raft's no-op). This step proves the state machine part: if replicas apply the same committed prefix in
// index order, their OMS state is identical. Carrying the request bytes inside the log entry is the next step.
#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include "meridian/oms.hpp"

namespace meridian::raft {

struct OmsCmd {
    enum Kind : uint8_t { Submit, Cancel, Modify } kind;
    oms::Request req;            // Submit
    uint32_t account;            // Cancel, Modify
    uint64_t order_id;           // Cancel, Modify
    uint32_t price, qty;         // Modify
};

inline oms::Config oms_replica_config(uint32_t bugs = 0) {
    oms::Config c;
    c.max_orders = 1u << 13; c.max_price = 200; c.max_order_qty = 60; c.max_position = 100000; c.bugs = bugs;
    return c;
}

// A mix of what real clients do: limit, market, IOC, FOK and stop orders from 16 accounts, cancels and modifies of earlier
// orders, client retries (same request again), and a few reused keys with different contents.
inline std::vector<OmsCmd> make_oms_script(uint64_t seed, size_t n) {
    using oms::Type; using book::Side; using book::TimeInForce;
    std::mt19937_64 rng(seed);
    std::vector<OmsCmd> s;
    s.reserve(n);
    uint64_t cl = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t roll = rng() % 100;
        const uint32_t acc = 1 + static_cast<uint32_t>(rng() % 16);
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        const uint32_t price = 97 + static_cast<uint32_t>(rng() % 7), qty = 1 + static_cast<uint32_t>(rng() % 60);
        const uint64_t target = 1 + rng() % (i + 1);                      // an earlier order id guess; may not exist, that is fine
        if (roll < 50) {
            const TimeInForce t = (rng() % 4 == 0) ? TimeInForce::IOC : (rng() % 12 == 0 ? TimeInForce::FOK : TimeInForce::GTC);
            s.push_back({OmsCmd::Submit, {acc, ++cl, side, Type::Limit, t, price, qty}, 0, 0, 0, 0});
        } else if (roll < 56) {
            s.push_back({OmsCmd::Submit, {acc, ++cl, side, Type::Market, TimeInForce::IOC, 0, qty}, 0, 0, 0, 0});
        } else if (roll < 66) {
            oms::Request r{acc, ++cl, side, (rng() & 1) ? Type::Stop : Type::StopLimit, TimeInForce::GTC, price, qty};
            r.stop_price = 97 + static_cast<uint32_t>(rng() % 7);
            s.push_back({OmsCmd::Submit, r, 0, 0, 0, 0});
        } else if (roll < 76 && !s.empty()) {                             // the client retries an earlier submit
            for (int tries = 0; tries < 8; ++tries) {
                const OmsCmd& e = s[rng() % s.size()];
                if (e.kind != OmsCmd::Submit) continue;
                OmsCmd c = e;
                if (rng() % 8 == 0) c.req.qty = c.req.qty % 60 + 1;       // same key, different contents: must be refused
                s.push_back(c);
                break;
            }
        } else if (roll < 90) {
            s.push_back({OmsCmd::Cancel, {}, 1 + static_cast<uint32_t>(rng() % 16), target, 0, 0});
        } else {
            s.push_back({OmsCmd::Modify, {}, 1 + static_cast<uint32_t>(rng() % 16), target, price, qty});
        }
    }
    return s;
}

class OmsReplica {
public:
    explicit OmsReplica(uint32_t bugs = 0) : oms_(oms_replica_config(bugs)) {}

    void apply(const OmsCmd& c) {
        oms::Result r;
        switch (c.kind) {
            case OmsCmd::Submit: r = oms_.submit(c.req); break;
            case OmsCmd::Cancel: r = oms_.cancel(c.account, c.order_id); break;
            case OmsCmd::Modify: r = oms_.modify(c.account, c.order_id, c.price, c.qty); break;
        }
        ++applied_;
        if (r.duplicate) ++dups_;
        if (r.reject != oms::Reject::None) ++rejects_;
        trades_ += r.fills.size() + r.triggered.size();
    }
    void apply_cmd(const std::vector<OmsCmd>& script, uint64_t cmd) { if (cmd != 0) apply(script[cmd - 1]); }

    // Everything the OMS shows, plus how many retries were answered from memory and how many requests were refused.
    uint64_t digest() const { return oms_.digest() * 0x9E3779B97F4A7C15ull ^ (dups_ << 40) ^ (rejects_ << 20) ^ applied_; }
    bool check(std::string* why = nullptr) const { return oms_.check(why); }
    const oms::Oms& oms() const noexcept { return oms_; }
    uint64_t applied() const noexcept { return applied_; }
    uint64_t trades() const noexcept { return trades_; }
    uint64_t dups() const noexcept { return dups_; }

private:
    oms::Oms oms_;
    uint64_t applied_ = 0, trades_ = 0, dups_ = 0, rejects_ = 0;
};

}  // namespace meridian::raft
