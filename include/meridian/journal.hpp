#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "meridian/order_book.hpp"

namespace meridian::journal {

enum class Type : uint8_t { Add = 1, Cancel = 2 };

// One input event. 32 bytes, host byte order (the file header records the byte order).
// crc covers the first 28 bytes. seq starts at 1 and increases by exactly 1.
struct Record {
    uint64_t seq;
    uint64_t id;
    uint32_t price;
    uint32_t qty;
    Type type;
    book::Side side;
    uint16_t pad;
    uint32_t crc;
};
static_assert(sizeof(Record) == 32, "journal record must be 32 bytes");

inline uint32_t crc32(const void* data, size_t n) noexcept {
    static const auto table = [] {
        struct T { uint32_t v[256]; } t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t.v[i] = c;
        }
        return t;
    }();
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table.v[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline uint32_t record_crc(const Record& r) noexcept { return crc32(&r, offsetof(Record, crc)); }

enum class LoadStatus { Ok, IoError, BadHeader, Truncated, BadCrc, SeqGap };

struct LoadResult {
    LoadStatus status;
    size_t valid;   // number of good records at the start of the file (a usable prefix even on error)
};

class Journal {
public:
    explicit Journal(size_t reserve_records = 0) { recs_.reserve(reserve_records); }

    // Appends never allocate while the reserved capacity is not exceeded.
    void append_add(uint64_t id, book::Side side, uint32_t price, uint32_t qty) {
        push({0, id, price, qty, Type::Add, side, 0, 0});
    }
    void append_cancel(uint64_t id) { push({0, id, 0, 0, Type::Cancel, book::Side::Buy, 0, 0}); }

    const std::vector<Record>& records() const noexcept { return recs_; }
    std::vector<Record>& records() noexcept { return recs_; }
    size_t size() const noexcept { return recs_.size(); }

    bool save(const char* path) const {
        std::FILE* f = std::fopen(path, "wb");
        if (!f) return false;
        Header h = make_header();
        bool ok = std::fwrite(&h, sizeof h, 1, f) == 1;
        if (ok && !recs_.empty()) ok = std::fwrite(recs_.data(), sizeof(Record), recs_.size(), f) == recs_.size();
        ok = (std::fclose(f) == 0) && ok;
        return ok;
    }

    // Loads the longest valid prefix into out. status != Ok means the file was damaged after `valid` records.
    static LoadResult load(const char* path, Journal& out) {
        out.recs_.clear();
        std::FILE* f = std::fopen(path, "rb");
        if (!f) return {LoadStatus::IoError, 0};
        Header h{};
        if (std::fread(&h, sizeof h, 1, f) != 1 || std::memcmp(&h, &kRef, sizeof h) != 0) { std::fclose(f); return {LoadStatus::BadHeader, 0}; }
        Record r;
        for (;;) {
            const size_t got = std::fread(&r, 1, sizeof r, f);
            if (got == 0) { std::fclose(f); return {LoadStatus::Ok, out.recs_.size()}; }
            if (got != sizeof r) { std::fclose(f); return {LoadStatus::Truncated, out.recs_.size()}; }
            if (r.crc != record_crc(r)) { std::fclose(f); return {LoadStatus::BadCrc, out.recs_.size()}; }
            if (r.seq != out.recs_.size() + 1) { std::fclose(f); return {LoadStatus::SeqGap, out.recs_.size()}; }
            out.recs_.push_back(r);
        }
    }

private:
    struct Header { char magic[8]; uint32_t record_size; uint32_t endian; };
    static constexpr Header kRef{{'M', 'R', 'D', 'N', 'J', 'R', 'N', '1'}, sizeof(Record), 0x01020304u};
    static Header make_header() { return kRef; }

    void push(Record r) {
        r.seq = recs_.size() + 1;
        r.crc = record_crc(r);
        recs_.push_back(r);
    }

    std::vector<Record> recs_;
};

}  // namespace meridian::journal
