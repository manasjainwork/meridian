#pragma once
// Connects the replicated log to the order management layer (accounts, positions, cash, idempotency, stops).
// A Raft entry carries the client request itself in Entry::data (a fixed 48-byte record, see encode_cmd). `cmd` stays a
// unique request number (cmd 0 is Raft's no-op) so the simulator can still check "never invented, never committed twice".
// If replicas apply the same committed prefix in index order, their OMS state is identical.
// apply_cmd(script, cmd) is the older path where the request came from a shared script indexed by cmd (docs 017, 026).
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "meridian/oms.hpp"
#include "meridian/raft_node.hpp"

namespace meridian::raft {

struct OmsCmd {
    enum Kind : uint8_t { Submit, Cancel, Modify } kind;
    oms::Request req;            // Submit
    uint32_t account;            // Cancel, Modify
    uint64_t order_id;           // Cancel, Modify
    uint32_t price, qty;         // Modify
};

// Wire form of a request inside a log entry: fixed 48 bytes, host byte order (all replicas here share one machine type).
// kind u8 | account u32 | cl_ord_id u64 | side u8 | type u8 | tif u8 | price u32 | qty u32 | stop u32 | cancel/modify account u32 | order_id u64 | price u32 | qty u32
constexpr size_t kCmdBytes = 48;
inline std::string encode_cmd(const OmsCmd& c) {
    std::string b(kCmdBytes, '\0');
    size_t o = 0;
    auto put = [&](const auto& v) { std::memcpy(&b[o], &v, sizeof v); o += sizeof v; };
    put(static_cast<uint8_t>(c.kind)); put(c.req.account); put(c.req.cl_ord_id);
    put(static_cast<uint8_t>(c.req.side)); put(static_cast<uint8_t>(c.req.type)); put(static_cast<uint8_t>(c.req.tif));
    put(c.req.price); put(c.req.qty); put(c.req.stop_price);
    put(c.account); put(c.order_id); put(c.price); put(c.qty);
    return b;
}
// Returns false for anything that is not exactly a well-formed record (wrong length, unknown enum value).
inline bool decode_cmd(const std::string& b, OmsCmd& c) {
    if (b.size() != kCmdBytes) return false;
    size_t o = 0;
    auto get = [&](auto& v) { std::memcpy(&v, &b[o], sizeof v); o += sizeof v; };
    uint8_t kind, side, type, tif;
    get(kind); get(c.req.account); get(c.req.cl_ord_id); get(side); get(type); get(tif);
    get(c.req.price); get(c.req.qty); get(c.req.stop_price);
    get(c.account); get(c.order_id); get(c.price); get(c.qty);
    if (kind > OmsCmd::Modify || side > 1 || type > static_cast<uint8_t>(oms::Type::StopLimit) || tif > static_cast<uint8_t>(book::TimeInForce::FOK)) return false;
    c.kind = static_cast<OmsCmd::Kind>(kind);
    c.req.side = static_cast<book::Side>(side); c.req.type = static_cast<oms::Type>(type); c.req.tif = static_cast<book::TimeInForce>(tif);
    return true;
}

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
    // Apply a committed log entry: the request is decoded from the entry's own bytes. The no-op (cmd 0) is skipped; bytes that
    // do not decode are counted and skipped (every replica skips the same entries, so they stay identical).
    void apply_entry(const Entry& e) {
        if (e.cmd == 0) return;
        OmsCmd c{};
        if (!decode_cmd(e.data, c)) { ++undecodable_; return; }
        apply(c);
    }
    uint64_t undecodable() const noexcept { return undecodable_; }

    // Everything the OMS shows, plus how many retries were answered from memory and how many requests were refused.
    uint64_t digest() const { return oms_.digest() * 0x9E3779B97F4A7C15ull ^ (dups_ << 40) ^ (rejects_ << 20) ^ (undecodable_ << 52) ^ applied_; }
    bool check(std::string* why = nullptr) const { return oms_.check(why); }
    const oms::Oms& oms() const noexcept { return oms_; }
    uint64_t applied() const noexcept { return applied_; }
    uint64_t trades() const noexcept { return trades_; }
    uint64_t dups() const noexcept { return dups_; }

private:
    oms::Oms oms_;
    uint64_t applied_ = 0, trades_ = 0, dups_ = 0, rejects_ = 0, undecodable_ = 0;
};

}  // namespace meridian::raft
