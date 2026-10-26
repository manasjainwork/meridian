#include <cstdio>
#include <cstring>
#include <vector>
#include "meridian/journal.hpp"
#include "meridian/replay.hpp"

using namespace meridian;
using namespace meridian::book;
using namespace meridian::journal;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

constexpr uint32_t kMaxPrice = 2048, kPool = 1u << 17;
static const char* kPath = "/tmp/meridian_test.journal";

// Live run: process random input, journal every event, return the live digest.
static uint64_t live_run(Journal& j, size_t n) {
    OrderBook book(kPool, kMaxPrice);
    Digest d;
    std::vector<Fill> fills;
    fills.reserve(64);
    std::vector<uint64_t> live;
    uint64_t next_id = 1;
    for (size_t i = 0; i < n; ++i) {
        if (!live.empty() && rnd() % 100 < 30) {
            const size_t k = rnd() % live.size();
            j.append_cancel(live[k]);
            live[k] = live.back(); live.pop_back();
        } else {
            const Side s = (rnd() & 1) ? Side::Buy : Side::Sell;
            const uint32_t p = 1000 + static_cast<uint32_t>(rnd() % 41) - 20 + (s == Side::Buy ? -2 : 2);
            j.append_add(next_id, s, p, 1 + static_cast<uint32_t>(rnd() % 100));
            live.push_back(next_id++);
        }
        apply(book, j.records().back(), fills, d);
    }
    mix_state(book, kMaxPrice, d);
    return d.value();
}

int main() {
    Journal j(300000);
    const uint64_t live = live_run(j, 200000);

    {   // 1. save -> load -> replay reproduces the live run exactly
        CHECK(j.save(kPath));
        Journal loaded;
        const LoadResult lr = Journal::load(kPath, loaded);
        CHECK(lr.status == LoadStatus::Ok && lr.valid == j.size());
        OrderBook b(kPool, kMaxPrice);
        CHECK(replay(loaded.records(), b, kMaxPrice) == live);
    }
    {   // 2. replay twice -> same digest
        OrderBook b1(kPool, kMaxPrice), b2(kPool, kMaxPrice);
        CHECK(replay(j.records(), b1, kMaxPrice) == replay(j.records(), b2, kMaxPrice));
    }
    {   // 3. replay on a book whose internal layout was scrambled by earlier churn: same externally visible result
        OrderBook b(kPool, kMaxPrice);
        std::vector<Fill> f;
        for (uint64_t i = 0; i < 5000; ++i) b.add_limit((1ull << 40) + i, (i & 1) ? Side::Buy : Side::Sell, 500 + static_cast<uint32_t>(i % 700) + ((i & 1) ? 0 : 800), 1 + static_cast<uint32_t>(i % 9), f);
        for (uint64_t i = 0; i < 5000; i += 3) b.cancel((1ull << 40) + i);
        for (uint64_t i = 0; i < 5000; ++i) b.cancel((1ull << 40) + i);
        CHECK(b.size() == 0);
        CHECK(replay(j.records(), b, kMaxPrice) == live);
    }
    {   // 4. every record's CRC is valid and sequence numbers are 1..N
        bool ok = true;
        for (size_t i = 0; i < j.size(); ++i) ok = ok && j.records()[i].seq == i + 1 && j.records()[i].crc == record_crc(j.records()[i]);
        CHECK(ok);
    }
    {   // 5. corruption: flip one bit at 300 random places in the file; load must never return Ok
        std::FILE* f = std::fopen(kPath, "rb");
        std::vector<unsigned char> img;
        unsigned char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) img.insert(img.end(), buf, buf + n);
        std::fclose(f);
        int undetected = 0;
        for (int t = 0; t < 300; ++t) {
            std::vector<unsigned char> bad = img;
            const size_t pos = rnd() % bad.size();
            bad[pos] ^= static_cast<unsigned char>(1u << (rnd() % 8));
            std::FILE* w = std::fopen("/tmp/meridian_bad.journal", "wb");
            std::fwrite(bad.data(), 1, bad.size(), w);
            std::fclose(w);
            Journal l;
            const LoadResult lr = Journal::load("/tmp/meridian_bad.journal", l);
            if (lr.status == LoadStatus::Ok) ++undetected;
        }
        CHECK(undetected == 0);
    }
    {   // 6. truncated file (crash mid-write): detected, and the valid prefix replays to the same digest as that prefix
        std::FILE* f = std::fopen(kPath, "rb");
        std::fseek(f, 0, SEEK_END);
        const long full = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<unsigned char> img(static_cast<size_t>(full));
        CHECK(std::fread(img.data(), 1, img.size(), f) == img.size());
        std::fclose(f);
        const size_t cut = 16 + 32 * 123456 + 11;            // header + 123456 records + 11 stray bytes
        std::FILE* w = std::fopen("/tmp/meridian_cut.journal", "wb");
        std::fwrite(img.data(), 1, cut, w);
        std::fclose(w);
        Journal l;
        const LoadResult lr = Journal::load("/tmp/meridian_cut.journal", l);
        CHECK(lr.status == LoadStatus::Truncated && lr.valid == 123456);
        OrderBook b(kPool, kMaxPrice);
        std::vector<Record> prefix(j.records().begin(), j.records().begin() + 123456);
        OrderBook b2(kPool, kMaxPrice);
        CHECK(replay(l.records(), b, kMaxPrice) == replay(prefix, b2, kMaxPrice));
    }
    {   // 7. a record removed from the middle is caught as a sequence gap
        Journal l = j;
        l.records().erase(l.records().begin() + 5000);
        CHECK(l.save("/tmp/meridian_gap.journal"));
        Journal l2;
        const LoadResult lr = Journal::load("/tmp/meridian_gap.journal", l2);
        CHECK(lr.status == LoadStatus::SeqGap && lr.valid == 5000);
    }
    {   // 8. one input event changed (CRC recomputed, so only the digest can notice): digest must differ
        std::vector<Record> alt = j.records();
        Record& r = alt[100000];
        if (r.type == Type::Add) r.qty += 1; else r.id += 1;
        r.crc = record_crc(r);
        OrderBook b(kPool, kMaxPrice);
        CHECK(replay(alt, b, kMaxPrice) != live);
    }
    std::printf(g_fail ? "%d FAILED\n" : "all journal/replay tests passed\n", g_fail);
    return g_fail != 0;
}
