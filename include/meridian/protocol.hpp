#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>

namespace meridian::proto {

static_assert(std::endian::native == std::endian::little, "little-endian host assumed");

inline uint16_t bswap(uint16_t v) noexcept { return __builtin_bswap16(v); }
inline uint32_t bswap(uint32_t v) noexcept { return __builtin_bswap32(v); }
inline uint64_t bswap(uint64_t v) noexcept { return __builtin_bswap64(v); }

#pragma pack(push, 1)

// Integer stored in network byte order (big-endian) on the wire.
template <class T>
struct BE {
    T raw;
    T get() const noexcept { return bswap(raw); }
    void set(T v) noexcept { raw = bswap(v); }
};

enum class MsgType : uint8_t { Heartbeat = 0, NewOrder = 1, OrderAck = 2 };
enum class Side : uint8_t { Buy = 0, Sell = 1 };

struct Header {            // 12 bytes
    BE<uint16_t> length;   // total message size, header included
    uint8_t type;
    uint8_t version;
    BE<uint64_t> seq;
};

struct Heartbeat { Header h; };

struct NewOrder {          // 38 bytes
    Header h;
    BE<uint64_t> client_order_id;
    BE<uint32_t> instrument_id;
    uint8_t side;
    uint8_t order_type;
    BE<uint64_t> price_ticks;   // integer ticks, never double
    BE<uint32_t> qty;
};

struct OrderAck {          // 29 bytes
    Header h;
    BE<uint64_t> client_order_id;
    BE<uint64_t> exchange_order_id;
    uint8_t status;
};

#pragma pack(pop)

static_assert(sizeof(Header) == 12);
static_assert(sizeof(Heartbeat) == 12);
static_assert(sizeof(NewOrder) == 38);
static_assert(sizeof(OrderAck) == 29);
static_assert(offsetof(NewOrder, price_ticks) == 26);
static_assert(offsetof(NewOrder, qty) == 34);

constexpr uint8_t kVersion = 1;

inline size_t expected_size(uint8_t type) noexcept {
    switch (static_cast<MsgType>(type)) {
        case MsgType::Heartbeat: return sizeof(Heartbeat);
        case MsgType::NewOrder:  return sizeof(NewOrder);
        case MsgType::OrderAck:  return sizeof(OrderAck);
    }
    return 0;
}

inline void fill_header(Header& h, MsgType t, size_t len, uint64_t seq) noexcept {
    h.length.set(static_cast<uint16_t>(len));
    h.type = static_cast<uint8_t>(t);
    h.version = kVersion;
    h.seq.set(seq);
}

inline NewOrder make_new_order(uint64_t seq, uint64_t client_id, uint32_t instrument,
                               Side side, uint64_t price_ticks, uint32_t qty) noexcept {
    NewOrder m{};
    fill_header(m.h, MsgType::NewOrder, sizeof(NewOrder), seq);
    m.client_order_id.set(client_id);
    m.instrument_id.set(instrument);
    m.side = static_cast<uint8_t>(side);
    m.order_type = 0;
    m.price_ticks.set(price_ticks);
    m.qty.set(qty);
    return m;
}

enum class Status { Ok, NeedMore, BadLength, BadType };

struct ParseResult {
    Status status;
    size_t consumed;       // bytes to drop from the stream when Ok
    const Header* header;  // points INTO the receive buffer (zero-copy)
};

// TCP is a byte stream: a read can end mid-message. NeedMore means "wait for more bytes".
inline ParseResult parse_one(const uint8_t* buf, size_t len) noexcept {
    if (len < sizeof(Header)) return {Status::NeedMore, 0, nullptr};
    const auto* h = reinterpret_cast<const Header*>(buf);
    const size_t want = expected_size(h->type);
    if (want == 0) return {Status::BadType, 0, nullptr};
    if (h->length.get() != want) return {Status::BadLength, 0, nullptr};
    if (len < want) return {Status::NeedMore, 0, nullptr};
    return {Status::Ok, want, h};
}

template <class T>
inline const T* as(const Header* h) noexcept { return reinterpret_cast<const T*>(h); }

}  // namespace meridian::proto
