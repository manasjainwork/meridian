#include <algorithm>
#include <cstdio>
#include <vector>
#include "meridian/journal.hpp"
#include "meridian/replay.hpp"
#include "meridian/replication.hpp"

using namespace meridian;
using namespace meridian::book;
using namespace meridian::journal;
using namespace meridian::repl;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

constexpr uint32_t kPool = 1u << 17, kMaxPrice = 2048;

static Journal make_journal(size_t n) {
    Journal j(n);
    std::vector<uint64_t> live;
    uint64_t next_id = 1;
    for (size_t i = 0; i < n; ++i) {
        if (!live.empty() && rnd() % 100 < 30) {
            const size_t k = rnd() % live.size();
            j.append_cancel(live[k]);
            live[k] = live.back(); live.pop_back();
        } else {
            const Side s = (rnd() & 1) ? Side::Buy : Side::Sell;
            j.append_add(next_id, s, 1000 + static_cast<uint32_t>(rnd() % 41) - 20 + (s == Side::Buy ? -2 : 2), 1 + static_cast<uint32_t>(rnd() % 100));
            live.push_back(next_id++);
        }
    }
    return j;
}

struct Outcome { FollowerResult f; uint64_t acked; };

// Sends the given wire bytes in chunks of random size [1, max_chunk], then half-closes and collects acks.
static Outcome transfer(const std::vector<unsigned char>& wire, uint64_t expected, size_t max_chunk) {
    Follower fol(expected, kPool, kMaxPrice);
    const uint16_t port = fol.listen_loopback();
    CHECK(port != 0);
    fol.start();
    LeaderLink link;
    CHECK(link.connect_loopback(port));
    size_t off = 0;
    while (off < wire.size()) {
        const size_t n = std::min<size_t>(1 + rnd() % max_chunk, wire.size() - off);
        if (!link.send_all(wire.data() + off, n)) break;
        off += n;
        link.poll_acks();
    }
    link.shutdown_write();
    const uint64_t acked = link.wait_acks_until_close();
    return {fol.join(), acked};
}

static std::vector<unsigned char> to_wire(const std::vector<Record>& recs) {
    std::vector<unsigned char> w(recs.size() * sizeof(Record));
    if (!recs.empty()) std::memcpy(w.data(), recs.data(), w.size());
    return w;
}

int main() {
    const size_t N = 100000;
    Journal j = make_journal(N);
    OrderBook ref(kPool, kMaxPrice);
    const uint64_t ref_digest = replay(j.records(), ref, kMaxPrice);

    {   // 1. fragmented stream (1..200 byte chunks): replica fully caught up, same digest as the leader
        const Outcome o = transfer(to_wire(j.records()), N, 200);
        CHECK(o.f.status == Status::Ok && o.f.valid == N);
        CHECK(o.f.digest == ref_digest);
        CHECK(o.acked == N);
    }
    {   // 2. big chunks
        const Outcome o = transfer(to_wire(j.records()), N, 100000);
        CHECK(o.f.status == Status::Ok && o.f.digest == ref_digest && o.acked == N);
    }
    {   // 3. one bit flipped in record 5001 (seq 5001) on the wire: replica stops, never acks past 5000
        std::vector<Record> r = j.records();
        r[5000].id ^= 1;                                     // CRC no longer matches
        const Outcome o = transfer(to_wire(r), N, 500);
        CHECK(o.f.status == Status::BadCrc && o.f.valid == 5000);
        CHECK(o.acked == 5000);
        std::vector<Record> p(j.records().begin(), j.records().begin() + 5000);
        OrderBook b(kPool, kMaxPrice);
        CHECK(o.f.digest == replay(p, b, kMaxPrice));        // replica state == leader state at seq 5000
    }
    {   // 4. record 3000 never sent (gap): detected, valid prefix 2999
        std::vector<Record> r = j.records();
        r.erase(r.begin() + 2999);                           // removes seq 3000
        const Outcome o = transfer(to_wire(r), N, 300);
        CHECK(o.f.status == Status::SeqGap && o.f.valid == 2999 && o.acked == 2999);
    }
    {   // 5. valid CRC but invalid field (type 9): rejected
        std::vector<Record> r = j.records();
        r[100].type = static_cast<Type>(9);
        r[100].crc = record_crc(r[100]);
        const Outcome o = transfer(to_wire(r), N, 300);
        CHECK(o.f.status == Status::BadField && o.f.valid == 100 && o.acked == 100);
    }
    {   // 6. leader disconnects early: replica reports Closed with what it had, acks match
        std::vector<Record> r(j.records().begin(), j.records().begin() + 1000);
        const Outcome o = transfer(to_wire(r), N, 300);
        CHECK(o.f.status == Status::Closed && o.f.valid == 1000 && o.acked == 1000);
    }
    {   // 7. a record cut in half at disconnect: the partial record is not applied or acked
        std::vector<unsigned char> w = to_wire(std::vector<Record>(j.records().begin(), j.records().begin() + 1000));
        w.resize(w.size() - 13);
        const Outcome o = transfer(w, N, 300);
        CHECK(o.f.status == Status::Closed && o.f.valid == 999 && o.acked == 999);
    }
    std::printf(g_fail ? "%d FAILED\n" : "all replication tests passed\n", g_fail);
    return g_fail != 0;
}
