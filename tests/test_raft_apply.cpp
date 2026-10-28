// The replicated log driving real order books. Every node applies its own committed prefix to its own book;
// the test checks, under loss, duplication, reordering, crashes and partitions, that
//   1. a node's book equals the reference book built from the simulator's committed log up to the same index,
//   2. after the network heals every node's book is identical, and equals the book of the whole committed log,
//   3. every book passes the order book's own invariant check.
// Then two wrong ways of applying (applying uncommitted entries, applying out of order) must be caught.
// Honest limit: this checks log agreement plus a deterministic apply. It does not test the order book itself (see test_order_*).
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include "meridian/raft_apply.hpp"
#include "meridian/raft_node.hpp"
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

static Replica build(const std::vector<Op>& script, const std::vector<Entry>& log, uint64_t upto) {
    Replica r;
    for (uint64_t i = 0; i < upto && i < log.size(); ++i) r.apply_cmd(script, log[i].cmd);
    return r;
}

struct Stats { uint64_t seeds = 0, converged = 0, with_ops = 0, ops = 0, trades = 0, prefix_checks = 0; };

// mutant: 0 = correct, 1 = apply the whole disk log (including uncommitted tail), 2 = swap two adjacent committed entries
static bool run_seed(uint64_t seed, int mutant, Stats* st, std::string* why) {
    constexpr uint32_t n = 3;
    const std::vector<Op> script = make_script(seed * 7919 + 1, 4000);
    Config cfg; cfg.n = n;
    Sim s(cfg, seed, rough_net());
    for (uint64_t i = 0; i < 6000 && s.ok(); ++i) chaos_step(s, n, 20);
    if (!s.ok()) { *why = s.violation(); return false; }

    bool caught = false;
    auto cmp_prefix = [&](NodeId i) {
        const uint64_t c = s.node(i).commit_index();
        std::vector<Entry> mine(s.disk_log(i).begin(), s.disk_log(i).begin() + (std::ptrdiff_t)c);   // what Raft says is committed
        uint64_t upto = c;
        if (mutant == 1) { mine = s.disk_log(i); upto = mine.size(); }
        if (mutant == 2 && mine.size() >= 2) std::swap(mine[mine.size() - 2], mine[mine.size() - 1]);
        Replica got = build(script, mine, upto);
        Replica ref = build(script, s.committed(), c);
        if (st) ++st->prefix_checks;
        std::string inv;
        if (!got.book().check_invariants(&inv)) { *why = "book invariant: " + inv; return false; }
        if (got.digest() != ref.digest()) { *why = "node " + std::to_string(i) + " book differs from the reference at commit index " + std::to_string(c); return false; }
        return true;
    };
    for (NodeId i = 0; i < n; ++i) if (s.alive(i)) if (!cmp_prefix(i)) caught = true;
    if (mutant) return !caught;                                           // for a mutant, "true" means it escaped

    // heal, restart everyone, stop proposing, let the cluster settle
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
    Replica full = build(script, s.committed(), s.committed().size());
    for (NodeId i = 0; i < n; ++i) {
        Replica r = build(script, s.disk_log(i), s.node(i).commit_index());
        std::string inv;
        if (!r.book().check_invariants(&inv)) { *why = "book invariant: " + inv; return false; }
        if (r.digest() != full.digest()) { *why = "after settling node " + std::to_string(i) + " has a different book"; return false; }
    }
    if (st) { ++st->converged; st->ops += full.applied(); st->trades += full.trades(); st->with_ops += full.applied() > 0; }
    return true;
}

int main() {
    std::printf("== Replica: same input, same book; digest reacts to change ==\n");
    {
        const auto sc = make_script(1, 5000);
        Replica a, b, c;
        for (const Op& o : sc) { a.apply(o); b.apply(o); }
        CHECK(a.digest() == b.digest());
        CHECK(a.book().check_invariants());
        for (size_t i = 0; i + 1 < sc.size(); ++i) c.apply(sc[i == 100 ? 101 : i == 101 ? 100 : i]);   // two requests swapped
        Replica d;
        for (size_t i = 0; i + 1 < sc.size(); ++i) d.apply(sc[i]);
        CHECK(c.digest() != d.digest());                                   // order matters, and the digest sees it
        Replica e;
        for (size_t i = 0; i + 1 < sc.size(); ++i) e.apply(sc[i]);
        CHECK(e.digest() == d.digest());
        std::printf("   %llu ops applied, %llu trades\n", (ULL)a.applied(), (ULL)a.trades());
    }

    std::printf("== Replicated order books under chaos (300 seeds, 3 nodes) ==\n");
    {
        Stats st; int bad = 0;
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            ++st.seeds;
            std::string why;
            if (!run_seed(seed, 0, &st, &why)) { if (bad++ < 5) std::printf("   seed %llu: %s\n", (ULL)seed, why.c_str()); }
        }
        std::printf("   seeds %llu, settled and identical %llu, seeds with applied orders %llu, orders applied %llu, trades %llu, prefix checks %llu\n",
                    (ULL)st.seeds, (ULL)st.converged, (ULL)st.with_ops, (ULL)st.ops, (ULL)st.trades, (ULL)st.prefix_checks);
        CHECK(bad == 0);
        CHECK(st.with_ops > 250);
    }

    std::printf("== The test must catch wrong ways of applying ==\n");
    for (int mutant = 1; mutant <= 2; ++mutant) {
        int escaped = 0, runs = 0;
        for (uint64_t seed = 1; seed <= 300; ++seed) { ++runs; std::string why; escaped += run_seed(seed, mutant, nullptr, &why) ? 1 : 0; }
        const int caught = runs - escaped;
        std::printf("   %-34s caught in %d of %d seeds\n", mutant == 1 ? "apply uncommitted tail" : "swap last two committed entries", caught, runs);
        CHECK(caught > 0);
    }

    std::printf("== Apply cost alone (no Raft, no network), 3 runs, informational ==\n");
    {
        const auto sc = make_script(42, 400000);
        for (int run = 1; run <= 3; ++run) {
            Replica r;
            const auto t0 = std::chrono::steady_clock::now();
            for (const Op& o : sc) r.apply(o);
            const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
            std::printf("   run %d: %llu ops, %.1f ns/op (mean, includes op decode and fill counting)\n", run, (ULL)r.applied(), ns / (double)sc.size());
        }
    }

    if (g_fail) { std::printf("\n%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("\nall raft-apply checks passed\n");
    return 0;
}
