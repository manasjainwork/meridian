// The replicated log driving the order management layer (accounts, positions, cash, idempotency, stops).
// Same method as test_raft_apply: under loss, duplication, reordering, crashes and partitions
//   1. a node's OMS equals the reference OMS built from the simulator's committed log up to the same index,
//   2. after the network heals every node's OMS is identical and equals the OMS of the whole committed log,
//   3. every OMS passes its own full consistency check.
// Then wrong ways of applying must be caught: applying the uncommitted tail, swapping two committed entries, and one node
// running an OMS with a deliberate bug (taker not credited / stops never fire).
// The request travels inside the log entry (Entry::data, 48 bytes). The script is only used to generate requests; replicas decode them from the entries.
// Also checked: the codec round-trips, rejects malformed bytes, and a node that applies a corrupted entry is caught.
#include <cstdio>
#include <string>
#include <vector>
#include "meridian/raft_node.hpp"
#include "meridian/raft_oms.hpp"
#include "meridian/raft_sim.hpp"

using namespace meridian::raft;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
using ULL = unsigned long long;
static void chaos_step(Sim& s, uint32_t n, uint32_t propose_per_1000) {
    const uint64_t roll = s.rand() % 100000;
    if (roll < 200) { const NodeId v = static_cast<NodeId>(s.rand() % n); if (s.alive(v)) s.crash(v); }
    else if (roll < 1200) { const NodeId v = static_cast<NodeId>(s.rand() % n); if (!s.alive(v)) s.restart(v); }
    else if (roll < 1500) { std::vector<uint8_t> g(n); for (auto& x : g) x = static_cast<uint8_t>(s.rand() % 3); s.set_groups(g); }
    else if (roll < 2000) s.heal();
    if (s.rand() % 1000 < propose_per_1000) s.propose_any();
    s.step();
}
static NetConfig rough_net() { NetConfig n; n.drop_pct = 15; n.dup_pct = 10; n.delay_min = 1; n.delay_max = 100; return n; }

static OmsReplica build(const std::vector<Entry>& log, uint64_t upto, uint32_t bugs = 0) {
    OmsReplica r(bugs);
    for (uint64_t i = 0; i < upto && i < log.size(); ++i) r.apply_entry(log[i]);
    return r;
}

struct Stats { uint64_t seeds = 0, converged = 0, with_ops = 0, ops = 0, trades = 0, dups = 0, prefix_checks = 0; };

// mutant: 0 correct, 1 apply uncommitted tail, 2 swap two committed entries, 3 node 0 runs "taker not credited", 4 node 0 runs "stops never fire",
//         5 node 0 applies its last committed entry with one request byte flipped
static bool run_seed(uint64_t seed, int mutant, Stats* st, std::string* why) {
    constexpr uint32_t n = 3;
    const std::vector<OmsCmd> script = make_oms_script(seed * 7919 + 1, 3000);
    Config cfg; cfg.n = n;
    Sim s(cfg, seed, rough_net());
    s.payload = [&script](uint64_t cmd) { return encode_cmd(script[cmd - 1]); };   // the request goes into the log entry
    for (uint64_t i = 0; i < 6000 && s.ok(); ++i) chaos_step(s, n, 20);
    if (!s.ok()) { *why = s.violation(); return false; }

    bool caught = false;
    auto cmp_prefix = [&](NodeId i) {
        const uint64_t c = s.node(i).commit_index();
        std::vector<Entry> mine(s.disk_log(i).begin(), s.disk_log(i).begin() + (std::ptrdiff_t)c);
        uint64_t upto = c;
        if (mutant == 1) { mine = s.disk_log(i); upto = mine.size(); }
        if (mutant == 2 && mine.size() >= 2) std::swap(mine[mine.size() - 2], mine[mine.size() - 1]);
        if (i == 0 && mutant == 5 && !mine.empty() && !mine.back().data.empty()) mine.back().data[0] ^= 0x01;
        uint32_t bugs = 0;
        if (i == 0 && mutant == 3) bugs = meridian::oms::kTakerNotCredited;
        if (i == 0 && mutant == 4) bugs = meridian::oms::kStopNeverTriggers;
        OmsReplica got = build(mine, upto, bugs);
        OmsReplica ref = build(s.committed(), c);
        if (st) ++st->prefix_checks;
        std::string inv;
        if (!ref.check(&inv)) { *why = "reference OMS invariant: " + inv; return false; }
        if (!got.check(&inv)) { *why = "OMS invariant: " + inv; return false; }
        if (got.digest() != ref.digest()) { *why = "node " + std::to_string(i) + " OMS differs from the reference at commit index " + std::to_string(c); return false; }
        return true;
    };
    for (NodeId i = 0; i < n; ++i) if (s.alive(i)) if (!cmp_prefix(i)) caught = true;
    if (mutant) return !caught;

    s.heal();
    for (NodeId i = 0; i < n; ++i) if (!s.alive(i)) s.restart(i);
    NodeId leader = kNone;
    bool settled = false;
    for (uint64_t i = 0; i < 40000 && s.ok(); ++i) {
        s.step();
        if (s.stable(&leader)) {
            bool all = true;
            for (NodeId k = 0; k < n; ++k) all = all && s.alive(k) && s.disk_log(k) == s.disk_log(leader) && s.node(k).commit_index() == s.disk_log(leader).size();
            if (all) { settled = true; break; }
        }
    }
    if (!s.ok()) { *why = s.violation(); return false; }
    if (!settled) { *why = "cluster did not settle"; return false; }
    s.check_now();
    if (!s.ok()) { *why = s.violation(); return false; }
    for (const Entry& e : s.committed()) if (e.cmd != 0 && e.data.size() != kCmdBytes) { *why = "a committed entry carries no request bytes"; return false; }
    OmsReplica full = build(s.committed(), s.committed().size());
    if (full.undecodable() != 0) { *why = "a committed entry did not decode"; return false; }
    std::string inv;
    if (!full.check(&inv)) { *why = "OMS invariant: " + inv; return false; }
    for (NodeId i = 0; i < n; ++i) {
        OmsReplica r = build(s.disk_log(i), s.node(i).commit_index());
        if (!r.check(&inv)) { *why = "OMS invariant: " + inv; return false; }
        if (r.digest() != full.digest()) { *why = "after settling node " + std::to_string(i) + " has a different OMS"; return false; }
    }
    if (st) { ++st->converged; st->ops += full.applied(); st->trades += full.trades(); st->dups += full.dups(); st->with_ops += full.applied() > 0; }
    return true;
}

int main() {
    std::printf("== OmsReplica: same input, same state; digest reacts to change ==\n");
    {
        const auto sc = make_oms_script(1, 3000);
        OmsReplica a, b, d, e;
        for (const OmsCmd& o : sc) { a.apply(o); b.apply(o); }
        CHECK(a.digest() == b.digest());
        CHECK(a.check());
        for (size_t i = 0; i + 1 < sc.size(); ++i) d.apply(sc[i]);
        for (size_t i = 0; i + 1 < sc.size(); ++i) e.apply(sc[i]);
        CHECK(e.digest() == d.digest());
        bool swap_seen = false;                                            // some neighbouring pair of requests must not commute
        for (size_t k = 100; k < 400 && !swap_seen; ++k) {
            OmsReplica c;
            for (size_t i = 0; i + 1 < sc.size(); ++i) c.apply(sc[i == k ? k + 1 : i == k + 1 ? k : i]);
            swap_seen = c.digest() != d.digest();
        }
        CHECK(swap_seen);                                                  // order matters, and the digest sees it
        std::printf("   %llu commands applied, %llu fills, %llu retries answered from memory\n", (ULL)a.applied(), (ULL)a.trades(), (ULL)a.dups());
        CHECK(a.dups() > 0 && a.trades() > 0);
    }

    std::printf("== Request bytes inside a log entry ==\n");
    {
        const auto sc = make_oms_script(1, 3000);
        size_t same = 0, tried = 0, rejected_short = 0, rejected_enum = 0;
        for (const OmsCmd& o : sc) {
            const std::string b = encode_cmd(o);
            OmsCmd back{};
            ++tried;
            if (b.size() == kCmdBytes && decode_cmd(b, back) && encode_cmd(back) == b) ++same;
        }
        CHECK(same == tried);
        // the same requests applied through bytes give the same state as applying them directly
        OmsReplica direct, viabytes;
        for (size_t i = 0; i < sc.size(); ++i) { direct.apply(sc[i]); viabytes.apply_entry(Entry{1, i + 1, encode_cmd(sc[i])}); }
        CHECK(direct.digest() == viabytes.digest());
        const std::string good = encode_cmd(sc[0]);
        OmsCmd tmp{};
        for (size_t len = 0; len < kCmdBytes; ++len) if (!decode_cmd(good.substr(0, len), tmp)) ++rejected_short;
        CHECK(rejected_short == kCmdBytes);                                // every truncation is refused
        CHECK(!decode_cmd(good + "x", tmp));                               // so is a longer record
        std::string bad = good; bad[0] = 9;  if (!decode_cmd(bad, tmp)) ++rejected_enum;   // unknown kind
        bad = good; bad[13] = 7;             if (!decode_cmd(bad, tmp)) ++rejected_enum;   // unknown side
        CHECK(rejected_enum == 2);
        OmsReplica junk; junk.apply_entry(Entry{1, 5, "not a request"});   // undecodable bytes are counted, state untouched
        CHECK(junk.undecodable() == 1 && junk.applied() == 0);
        std::printf("   %llu of %llu requests round-trip; all %llu truncations and 2 bad enum values refused\n", (ULL)same, (ULL)tried, (ULL)rejected_short);
    }

    std::printf("== Replicated OMS under chaos (300 seeds, 3 nodes) ==\n");
    {
        Stats st; int bad = 0;
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            ++st.seeds;
            std::string why;
            if (!run_seed(seed, 0, &st, &why)) { if (bad++ < 5) std::printf("   seed %llu: %s\n", (ULL)seed, why.c_str()); }
        }
        std::printf("   seeds %llu, settled and identical %llu, seeds with applied commands %llu, commands applied %llu, fills %llu, retries answered from memory %llu, prefix checks %llu\n",
                    (ULL)st.seeds, (ULL)st.converged, (ULL)st.with_ops, (ULL)st.ops, (ULL)st.trades, (ULL)st.dups, (ULL)st.prefix_checks);
        CHECK(bad == 0);
        CHECK(st.with_ops > 250);
    }

    std::printf("== The test must catch wrong ways of applying ==\n");
    const char* names[] = {"", "apply uncommitted tail", "swap last two committed entries", "one node: taker not credited", "one node: stops never fire", "one node: a request byte flipped"};
    for (int mutant = 1; mutant <= 5; ++mutant) {
        int escaped = 0, runs = 0;
        for (uint64_t seed = 1; seed <= 300; ++seed) { ++runs; std::string why; escaped += run_seed(seed, mutant, nullptr, &why) ? 1 : 0; }
        const int caught = runs - escaped;
        std::printf("   %-34s caught in %d of %d seeds\n", names[mutant], caught, runs);
        CHECK(caught > 0);
    }

    if (g_fail) { std::printf("\n%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("\nall raft-oms checks passed\n");
    return 0;
}
