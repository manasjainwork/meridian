#pragma once
// Event log version 2 (sequencer design M2, decisions D5 and D6).
// One record per request the sequencer applied: the 48-byte request (the same bytes as a Raft entry, doc 027), the sequence
// number, the producer, the producer's tag and two timestamps (0 until the timing milestone). The log holds inputs only;
// outputs (replies, fills) are reproduced by replay(). Host byte order; the file header records the byte order.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "meridian/journal.hpp"
#include "meridian/sequencer.hpp"

namespace meridian::evlog {

constexpr size_t kCmdBytes = 48;

// 88 bytes. crc covers the first 84 bytes. seq starts at 1 and increases by exactly 1.
struct Record {
    uint64_t seq;
    uint64_t tag;
    uint64_t t_ingress;   // TSC ticks at arrival, 0 until M4
    uint64_t t_apply;     // TSC ticks at the end of apply, 0 until M4
    uint8_t cmd[kCmdBytes];
    uint32_t producer;
    uint32_t crc;
};
static_assert(sizeof(Record) == 88, "event log record must be 88 bytes");

// Same field order and sizes as raft::encode_cmd: kind u8 | account u32 | cl_ord_id u64 | side u8 | type u8 | tif u8 |
// price u32 | qty u32 | stop u32 | cancel/modify account u32 | order_id u64 | price u32 | qty u32
inline void encode(const seq::Msg& m, uint8_t out[kCmdBytes]) noexcept {
    size_t o = 0;
    auto put = [&](const auto& v) { std::memcpy(out + o, &v, sizeof v); o += sizeof v; };
    put(static_cast<uint8_t>(m.op)); put(m.req.account); put(m.req.cl_ord_id);
    put(static_cast<uint8_t>(m.req.side)); put(static_cast<uint8_t>(m.req.type)); put(static_cast<uint8_t>(m.req.tif));
    put(m.req.price); put(m.req.qty); put(m.req.stop_price);
    put(m.account); put(m.order_id); put(m.price); put(m.qty);
}

// False for an unknown enum value. The tag is not part of the 48 bytes; the caller sets it from the record.
inline bool decode(const uint8_t in[kCmdBytes], seq::Msg& m) noexcept {
    size_t o = 0;
    auto get = [&](auto& v) { std::memcpy(&v, in + o, sizeof v); o += sizeof v; };
    uint8_t op, side, type, tif;
    get(op); get(m.req.account); get(m.req.cl_ord_id); get(side); get(type); get(tif);
    get(m.req.price); get(m.req.qty); get(m.req.stop_price);
    get(m.account); get(m.order_id); get(m.price); get(m.qty);
    if (op > static_cast<uint8_t>(seq::Op::Modify) || side > 1 || type > static_cast<uint8_t>(oms::Type::StopLimit) ||
        tif > static_cast<uint8_t>(book::TimeInForce::FOK)) return false;
    m.op = static_cast<seq::Op>(op);
    m.req.side = static_cast<book::Side>(side); m.req.type = static_cast<oms::Type>(type); m.req.tif = static_cast<book::TimeInForce>(tif);
    return true;
}

inline uint32_t record_crc(const Record& r) noexcept { return journal::crc32(&r, offsetof(Record, crc)); }

enum class LoadStatus { Ok, IoError, BadHeader, Truncated, BadCrc, SeqGap, BadCmd };
struct LoadResult {
    LoadStatus status;
    size_t valid;   // good records at the start of the log: a usable prefix even on error
};

class EventLog {
public:
    struct Header { char magic[8]; uint32_t record_size; uint32_t endian; };
    static constexpr Header kRef{{'M', 'R', 'D', 'N', 'L', 'O', 'G', '2'}, sizeof(Record), 0x01020304u};

    explicit EventLog(size_t reserve_records = 0) { recs_.reserve(reserve_records); }

    void append(const seq::Msg& m, uint32_t producer, uint64_t t_ingress = 0, uint64_t t_apply = 0) {
        Record r{};
        r.seq = recs_.size() + 1; r.tag = m.tag; r.t_ingress = t_ingress; r.t_apply = t_apply; r.producer = producer;
        encode(m, r.cmd);
        r.crc = record_crc(r);
        recs_.push_back(r);
    }
    static EventLog from_applied(const std::vector<seq::Applied>& a) {
        EventLog l(a.size());
        for (const seq::Applied& x : a) l.append(x.msg, x.producer);
        return l;
    }

    const std::vector<Record>& records() const noexcept { return recs_; }
    size_t size() const noexcept { return recs_.size(); }
    bool message(size_t i, seq::Msg& m) const noexcept { m = seq::Msg{}; m.tag = recs_[i].tag; return decode(recs_[i].cmd, m); }

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> b(sizeof(Header) + recs_.size() * sizeof(Record));
        std::memcpy(b.data(), &kRef, sizeof(Header));
        if (!recs_.empty()) std::memcpy(b.data() + sizeof(Header), recs_.data(), recs_.size() * sizeof(Record));
        return b;
    }

    // Parses the longest valid prefix into out. status != Ok means the log was damaged after `valid` records.
    static LoadResult parse(const uint8_t* p, size_t n, EventLog& out) {
        out.recs_.clear();
        if (n < sizeof(Header) || std::memcmp(p, &kRef, sizeof(Header)) != 0) return {LoadStatus::BadHeader, 0};
        size_t off = sizeof(Header);
        for (;;) {
            if (off == n) return {LoadStatus::Ok, out.recs_.size()};
            if (n - off < sizeof(Record)) return {LoadStatus::Truncated, out.recs_.size()};
            Record r;
            std::memcpy(&r, p + off, sizeof r);
            if (r.crc != record_crc(r)) return {LoadStatus::BadCrc, out.recs_.size()};
            if (r.seq != out.recs_.size() + 1) return {LoadStatus::SeqGap, out.recs_.size()};
            seq::Msg m;
            if (!decode(r.cmd, m)) return {LoadStatus::BadCmd, out.recs_.size()};
            out.recs_.push_back(r);
            off += sizeof r;
        }
    }

    bool save(const char* path) const {
        std::FILE* f = std::fopen(path, "wb");
        if (!f) return false;
        const std::vector<uint8_t> b = to_bytes();
        bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
        return (std::fclose(f) == 0) && ok;
    }
    static LoadResult load(const char* path, EventLog& out) {
        std::FILE* f = std::fopen(path, "rb");
        if (!f) return {LoadStatus::IoError, 0};
        std::vector<uint8_t> b;
        uint8_t buf[65536];
        for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) b.insert(b.end(), buf, buf + got);
        const bool err = std::ferror(f) != 0;
        std::fclose(f);
        if (err) return {LoadStatus::IoError, 0};
        return parse(b.data(), b.size(), out);
    }

private:
    std::vector<Record> recs_;
};

// What a replay produced: the reply for every record, in order, and a checksum over every fill (including stop-triggered fills).
struct ReplayOut {
    std::vector<seq::Reply> replies;
    uint64_t fills_digest = seq::kFillsDigestInit;
};

// Applies the log to o, which must be a fresh Oms built with the same Config. False if a record does not decode.
inline bool replay(const EventLog& log, oms::Oms& o, ReplayOut* out = nullptr) {
    if (out) { out->replies.clear(); out->replies.reserve(log.size()); out->fills_digest = seq::kFillsDigestInit; }
    for (size_t i = 0; i < log.size(); ++i) {
        seq::Msg m;
        if (!log.message(i, m)) return false;
        const oms::Result r = seq::apply_msg(o, m);
        if (out) {
            out->replies.push_back(seq::make_reply(log.records()[i].seq, log.records()[i].tag, r));
            out->fills_digest = seq::fills_mix(out->fills_digest, r);
        }
    }
    return true;
}

}  // namespace meridian::evlog
