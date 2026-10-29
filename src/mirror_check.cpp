// Mirror a recorded exchange order book inside the Meridian matching engine and check it against the exchange.
//   mirror_check events.ev [--max-price N] [--naive]
// events.ev comes from tools/feed/normalize.py. For every exchange message the tool
//   1. updates a plain std::map copy of the exchange book (the "shadow"), cuts it to the subscribed depth,
//   2. brings the engine's OrderBook to the same state with cancel and submit calls (removals first, so the book never crosses),
//   3. reads the top 10 levels back OUT OF THE ENGINE, builds Kraken's checksum string and compares its CRC32 with the one the
//      exchange sent, and compares every level quantity and the best bid and ask with the shadow.
// One exchange price level is one resting order per 2^32-1 lots (an engine order holds a 32-bit quantity).
// --naive applies new levels before removals: it exists only to show why the order matters (the book crosses and trades).
// Timing is informational: engine calls only, rdtsc, one thread, WSL2.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "meridian/order_book.hpp"
#include "meridian/timing.hpp"

using namespace meridian;
using namespace meridian::book;
using ULL = unsigned long long;

namespace {

uint32_t crc32(const std::string& s) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

constexpr uint64_t kChunk = 0xFFFFFFFFull;            // lots per engine order

struct Mirror {
    OrderBook book;
    uint32_t max_price;
    std::map<uint64_t, uint64_t> mine[2];             // [0]=bids [1]=asks: what the engine currently holds, ticks -> lots
    std::vector<Fill> fills;
    std::vector<uint64_t> call_ns;
    uint64_t unexpected_fills = 0, rejects = 0, calls = 0;
    double ghz;
    uint64_t overhead;

    Mirror(uint32_t mp, double g, uint64_t oh) : book(1u << 14, mp), max_price(mp), ghz(g), overhead(oh) { fills.reserve(64); }

    static uint64_t id_of(int side, uint64_t ticks, uint64_t chunk) { return (((ticks << 1) | (uint64_t)side) << 8 | chunk) + 1; }
    uint64_t ns(uint64_t cy) const { return (uint64_t)((double)(cy > overhead ? cy - overhead : 0) / ghz); }

    void remove_level(int side, uint64_t ticks) {
        auto it = mine[side].find(ticks);
        if (it == mine[side].end()) return;
        const uint64_t chunks = (it->second + kChunk - 1) / kChunk;
        for (uint64_t c = 0; c < chunks; ++c) {
            const uint64_t a = rdtsc_begin(); const bool ok = book.cancel(id_of(side, ticks, c)); const uint64_t b = rdtsc_end();
            call_ns.push_back(ns(b - a)); ++calls;
            if (!ok) ++rejects;
        }
        mine[side].erase(it);
    }
    void add_level(int side, uint64_t ticks, uint64_t lots) {
        uint64_t left = lots, c = 0;
        while (left) {
            const uint32_t q = (uint32_t)std::min<uint64_t>(left, kChunk);
            fills.clear();
            const uint64_t a = rdtsc_begin();
            const ExecReport r = book.submit(id_of(side, ticks, c), side == 0 ? Side::Buy : Side::Sell, TimeInForce::GTC, (uint32_t)ticks, q, fills);
            const uint64_t b = rdtsc_end();
            call_ns.push_back(ns(b - a)); ++calls;
            if (r.status != AddResult::Ok) ++rejects;
            unexpected_fills += fills.size();
            left -= q; ++c;
        }
        mine[side][ticks] = lots;
    }
    void clear() {
        for (int s = 0; s < 2; ++s) { std::vector<uint64_t> ps; for (auto& kv : mine[s]) ps.push_back(kv.first); for (uint64_t p : ps) remove_level(s, p); }
    }

    // Top 10 levels read from the ENGINE (not from the shadow).
    std::string engine_checksum_string(std::vector<std::pair<uint64_t, uint64_t>>* top_b = nullptr) const {
        std::string a, b;
        int found = 0;
        for (uint64_t p = book.best_ask(); p && p < max_price && found < 10; ++p) {
            const uint64_t q = book.qty_at(Side::Sell, (uint32_t)p);
            if (q) { a += std::to_string(p) + std::to_string(q); ++found; }
        }
        found = 0;
        for (uint64_t p = book.best_bid(); p >= 1 && found < 10; --p) {
            const uint64_t q = book.qty_at(Side::Buy, (uint32_t)p);
            if (q) { b += std::to_string(p) + std::to_string(q); ++found; if (top_b) top_b->push_back({p, q}); }
        }
        return a + b;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: mirror_check events.ev [--max-price N] [--naive]\n"); return 2; }
    uint32_t max_price = 2000000; bool naive = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--max-price") && i + 1 < argc) max_price = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--naive")) naive = true;
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    std::ifstream in(argv[1]);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }

    const double ghz = calibrate_tsc_ghz();
    uint64_t overhead;
    { std::vector<uint64_t> d(20001); for (auto& x : d) { const uint64_t a = rdtsc_begin(); const uint64_t b = rdtsc_end(); x = b - a; }
      std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end()); overhead = d[d.size() / 2]; }
    Mirror m(max_price, ghz, overhead);

    std::map<uint64_t, uint64_t> shadow[2];
    uint64_t depth = 10, snaps = 0, upds = 0, snap_ok = 0, snap_bad = 0, upd_ok = 0, upd_bad = 0, level_bad = 0, best_bad = 0, inv_bad = 0, range_bad = 0;
    std::vector<std::string> notes;
    std::string line;
    uint64_t line_no = 0, msgs = 0;
    while (std::getline(in, line)) {
        ++line_no;
        std::istringstream ss(line);
        std::string t; ss >> t;
        if (t == "D") { ss >> depth; }
        else if (t == "R") { shadow[0].clear(); shadow[1].clear(); m.clear(); ++snaps; }
        else if (t == "L") {
            std::string sd; uint64_t p, q; ss >> sd >> p >> q;
            const int s = sd == "b" ? 0 : 1;
            if (p == 0 || p >= max_price) { ++range_bad; continue; }
            if (q == 0) shadow[s].erase(p); else shadow[s][p] = q;
        }
        else if (t == "E") {
            std::string kind; uint64_t want, rx; ss >> kind >> want >> rx;
            ++msgs;
            if (kind == "u") ++upds;
            // cut to depth: bids keep the highest, asks the lowest
            while (shadow[0].size() > depth) shadow[0].erase(shadow[0].begin());
            while (shadow[1].size() > depth) shadow[1].erase(std::prev(shadow[1].end()));
            // sync the engine to the shadow: removals and changes first, then additions
            for (int pass = 0; pass < 2; ++pass) {
                const bool removal_pass = naive ? pass == 1 : pass == 0;
                for (int s = 0; s < 2; ++s) {
                    if (removal_pass) {
                        std::vector<uint64_t> gone;
                        for (auto& kv : m.mine[s]) { auto it = shadow[s].find(kv.first); if (it == shadow[s].end() || it->second != kv.second) gone.push_back(kv.first); }
                        for (uint64_t p : gone) m.remove_level(s, p);
                    } else {
                        for (auto& kv : shadow[s]) if (!m.mine[s].count(kv.first)) m.add_level(s, kv.first, kv.second);
                    }
                }
            }
            // read back from the engine
            std::string cs = m.engine_checksum_string();
            const bool match = crc32(cs) == (uint32_t)want;
            (kind == "s" ? (match ? snap_ok : snap_bad) : (match ? upd_ok : upd_bad))++;
            if (!match && notes.size() < 3) notes.push_back("line " + std::to_string(line_no) + ": exchange " + std::to_string(want) + ", engine " + std::to_string(crc32(cs)) + ", string " + cs.substr(0, 120));
            for (int s = 0; s < 2; ++s)
                for (auto& kv : shadow[s]) if (m.book.qty_at(s == 0 ? Side::Buy : Side::Sell, (uint32_t)kv.first) != kv.second) ++level_bad;
            const uint32_t sb = shadow[0].empty() ? 0 : (uint32_t)shadow[0].rbegin()->first, sa = shadow[1].empty() ? 0 : (uint32_t)shadow[1].begin()->first;
            if (m.book.best_bid() != sb || m.book.best_ask() != sa) ++best_bad;
            if (msgs % 50 == 0 || kind == "s") { std::string why; if (!m.book.check_invariants(&why)) { ++inv_bad; if (notes.size() < 3) notes.push_back("invariant: " + why); } }
        }
    }
    std::string why;
    const bool final_inv = m.book.check_invariants(&why);
    std::sort(m.call_ns.begin(), m.call_ns.end());
    auto pct = [&](double p) { return m.call_ns.empty() ? 0ull : (ULL)m.call_ns[std::min<size_t>(m.call_ns.size() - 1, (size_t)(p / 100.0 * m.call_ns.size()))]; };
    std::printf("depth %llu, snapshots %llu, updates %llu%s\n", (ULL)depth, (ULL)snaps, (ULL)upds, naive ? " (NAIVE order: additions before removals)" : "");
    std::printf("checksum read from the engine: snapshots match %llu / mismatch %llu, updates match %llu / mismatch %llu\n", (ULL)snap_ok, (ULL)snap_bad, (ULL)upd_ok, (ULL)upd_bad);
    std::printf("level quantity differences vs the exchange book: %llu; best bid/ask differences: %llu; prices out of range: %llu\n", (ULL)level_bad, (ULL)best_bad, (ULL)range_bad);
    std::printf("engine: unexpected fills %llu, rejected calls %llu, invariant failures %llu (final: %s)\n", (ULL)m.unexpected_fills, (ULL)m.rejects, (ULL)inv_bad, final_inv ? "ok" : why.c_str());
    std::printf("engine calls %llu (cancel + submit), service time per call: p50 %llu ns, p99 %llu ns, max %llu ns (informational: WSL2, one run)\n", (ULL)m.calls, pct(50), pct(99), m.call_ns.empty() ? 0ull : (ULL)m.call_ns.back());
    for (auto& n : notes) std::printf("  %s\n", n.c_str());
    const bool pass = snap_bad + upd_bad + level_bad + best_bad + inv_bad + range_bad + m.unexpected_fills + m.rejects == 0 && final_inv && snap_ok + upd_ok > 0;
    std::printf(pass ? "\nPASS\n" : "\nFAIL\n");
    return pass ? 0 : 1;
}
