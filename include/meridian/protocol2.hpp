#pragma once
// Binary protocol version 2 (sequencer design M3, decision D8): reaches everything the OMS can do.
// Same 12-byte header and network byte order as version 1, so a stream can be framed the same way; the version byte is 2.
//   Submit  39 bytes: account, cl_ord_id, side, order type, time in force, price, qty, stop price
//   Cancel  24 bytes: account, order id
//   Modify  32 bytes: account, order id, new price, new open quantity
//   Reply   43 bytes: client message number, order id, reject code, status, filled, rested, cancelled, duplicate flag
// header.seq in a request is the client's message number; the reply echoes it as client_seq. header.seq in a reply is the
// sequencer's global sequence number. Prices are integer ticks, never floating point.
#include <cstddef>
#include <cstdint>
#include "meridian/protocol.hpp"
#include "meridian/sequencer.hpp"

namespace meridian::proto2 {

using proto::BE;
using proto::Header;

enum class MsgType : uint8_t { Heartbeat = 0, Submit = 16, Cancel = 17, Modify = 18, Reply = 19 };
constexpr uint8_t kVersion = 2;

#pragma pack(push, 1)
struct Heartbeat { Header h; };
struct Submit {
    Header h;
    BE<uint32_t> account;
    BE<uint64_t> cl_ord_id;
    uint8_t side;          // 0 buy, 1 sell
    uint8_t order_type;    // 0 limit, 1 market, 2 stop, 3 stop-limit
    uint8_t tif;           // 0 GTC, 1 IOC, 2 FOK
    BE<uint32_t> price;
    BE<uint32_t> qty;
    BE<uint32_t> stop_price;
};
struct Cancel { Header h; BE<uint32_t> account; BE<uint64_t> order_id; };
struct Modify { Header h; BE<uint32_t> account; BE<uint64_t> order_id; BE<uint32_t> price; BE<uint32_t> qty; };
struct Reply {
    Header h;
    BE<uint64_t> client_seq;
    BE<uint64_t> order_id;
    uint8_t reject;        // oms::Reject
    uint8_t status;        // oms::Status
    BE<uint32_t> filled;
    BE<uint32_t> rested;
    BE<uint32_t> cancelled;
    uint8_t duplicate;
};
#pragma pack(pop)

static_assert(sizeof(Heartbeat) == 12 && sizeof(Submit) == 39 && sizeof(Cancel) == 24 && sizeof(Modify) == 32 && sizeof(Reply) == 43);

enum class Status { Ok, NeedMore, BadLength, BadType, BadVersion, BadField };

struct ParseResult {
    Status status;
    size_t consumed;       // bytes to drop from the stream when Ok
    const Header* header;  // points into the receive buffer
};

inline size_t expected_size(uint8_t type) noexcept {
    switch (static_cast<MsgType>(type)) {
        case MsgType::Heartbeat: return sizeof(Heartbeat);
        case MsgType::Submit:    return sizeof(Submit);
        case MsgType::Cancel:    return sizeof(Cancel);
        case MsgType::Modify:    return sizeof(Modify);
        case MsgType::Reply:     return sizeof(Reply);
    }
    return 0;
}

inline void fill_header(Header& h, MsgType t, size_t len, uint64_t seq) noexcept {
    h.length.set(static_cast<uint16_t>(len));
    h.type = static_cast<uint8_t>(t);
    h.version = kVersion;
    h.seq.set(seq);
}

// TCP is a byte stream: a read can end mid-message; NeedMore means wait for more bytes. A message is checked as soon as its
// header is complete (length, type, version), and its fields once the whole message is there. Anything else is an error and
// the stream cannot be resynchronised, so the caller closes the connection.
inline ParseResult parse_one(const uint8_t* buf, size_t len) noexcept {
    if (len < sizeof(Header)) return {Status::NeedMore, 0, nullptr};
    const auto* h = reinterpret_cast<const Header*>(buf);
    const size_t want = expected_size(h->type);
    if (want == 0) return {Status::BadType, 0, nullptr};
    if (h->version != kVersion) return {Status::BadVersion, 0, nullptr};
    if (h->length.get() != want) return {Status::BadLength, 0, nullptr};
    if (len < want) return {Status::NeedMore, 0, nullptr};
    if (h->type == static_cast<uint8_t>(MsgType::Submit) && len >= sizeof(Submit)) {   // len >= want is already known; stated again so the compiler can see it
        const auto* m = reinterpret_cast<const Submit*>(buf);
        if (m->side > 1 || m->order_type > static_cast<uint8_t>(oms::Type::StopLimit) || m->tif > static_cast<uint8_t>(book::TimeInForce::FOK))
            return {Status::BadField, 0, nullptr};
    }
    return {Status::Ok, want, h};
}

template <class T>
inline const T* as(const Header* h) noexcept { return reinterpret_cast<const T*>(h); }

// Requests to sequencer messages; the client's message number becomes the tag.
inline seq::Msg to_msg(const Submit& m) noexcept {
    seq::Msg x;
    x.op = seq::Op::Submit; x.tag = m.h.seq.get();
    x.req = oms::Request{m.account.get(), m.cl_ord_id.get(), static_cast<book::Side>(m.side), static_cast<oms::Type>(m.order_type),
                         static_cast<book::TimeInForce>(m.tif), m.price.get(), m.qty.get(), m.stop_price.get()};
    return x;
}
inline seq::Msg to_msg(const Cancel& m) noexcept {
    seq::Msg x;
    x.op = seq::Op::Cancel; x.tag = m.h.seq.get(); x.account = m.account.get(); x.order_id = m.order_id.get();
    return x;
}
inline seq::Msg to_msg(const Modify& m) noexcept {
    seq::Msg x;
    x.op = seq::Op::Modify; x.tag = m.h.seq.get(); x.account = m.account.get(); x.order_id = m.order_id.get();
    x.price = m.price.get(); x.qty = m.qty.get();
    return x;
}

inline Reply to_wire(const seq::Reply& r) noexcept {
    Reply w{};
    fill_header(w.h, MsgType::Reply, sizeof(Reply), r.seq);
    w.client_seq.set(r.tag); w.order_id.set(r.order_id);
    w.reject = static_cast<uint8_t>(r.reject); w.status = static_cast<uint8_t>(r.status);
    w.filled.set(r.filled); w.rested.set(r.rested); w.cancelled.set(r.cancelled);
    w.duplicate = r.duplicate ? 1 : 0;
    return w;
}
inline seq::Reply from_wire(const Reply& w) noexcept {
    seq::Reply r;
    r.seq = w.h.seq.get(); r.tag = w.client_seq.get(); r.order_id = w.order_id.get();
    r.reject = static_cast<oms::Reject>(w.reject); r.status = static_cast<oms::Status>(w.status);
    r.filled = w.filled.get(); r.rested = w.rested.get(); r.cancelled = w.cancelled.get();
    r.duplicate = w.duplicate != 0;
    return r;
}

}  // namespace meridian::proto2
