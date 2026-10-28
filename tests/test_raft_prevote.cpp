// Pre-vote and CheckQuorum on the deterministic simulator.
//  1. The weakness measured in test_raft_election (a cut-off node's term runs away and deposes the healthy leader when it
//     returns) happens without pre-vote and does not happen with it.
//  2. Liveness is kept: a leader is still elected after a crash, in a 1-node cluster, and after a majority returns.
//  3. CheckQuorum: a leader cut off in a minority steps down on its own; a healthy leader does not step down.
//  4. Randomized chaos (loss, duplication, reordering, crashes, partitions, client commands) with both features on: every
//     safety check of the simulator still holds.
//  5. Two wrong implementations (pre-vote granted although a leader was just heard from; granting a pre-vote changes the
//     receiver's term) must be caught by the scenarios of part 1.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "meridian/raft_node.hpp"
#include "meridian/raft_sim.hpp"

using namespace meridian::raft;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
using ULL = unsigned long long;

static bool run_until_stable(Sim& s, uint64_t max_steps, uint32_t hold = 500) {
    uint32_t run = 0;
    for (uint64_t i = 0; i < max_steps; ++i) {
        s.step();
        if (!s.ok()) return false;
        run = s.stable() ? run + 1 : 0;
        if (run >= hold) return true;
    }
    return false;
}
static void steps(Sim& s, uint64_t n) { for (uint64_t i = 0; i < n; ++i) s.step(); }
static NodeId stable_leader(const Sim& s) { NodeId l = kNone; s.stable(&l); return l; }

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

static Config make_cfg(uint32_t n, bool pre, bool cq, uint32_t bugs = 0) {
    Config c; c.n = n; c.prevote = pre; c.check_quorum = cq; c.bugs = bugs; return c;
}

// One node (or, with `pair`, two non-leader nodes that can still talk to each other: a 2-of-5 minority) is cut off for
// 4000 ms, then the network heals.
struct Isolation { bool ran = false, violation = false, unstable = false, disrupted = false; Term t1 = 0, x_term = 0, final_term = 0; };
static Isolation isolate_and_heal(uint64_t seed, const Config& cfg, bool pair = false) {
    Isolation r;
    Sim s(cfg, seed);
    if (!run_until_stable(s, 3000)) { r.violation = !s.ok(); r.unstable = s.ok(); return r; }
    r.ran = true;
    const NodeId l1 = stable_leader(s), x = (l1 + 2) % cfg.n, y = (l1 + 3) % cfg.n;
    r.t1 = s.node(l1).term();
    std::vector<uint8_t> g(cfg.n, 0);
    g[x] = 1;
    if (pair) g[y] = 1;
    s.set_groups(g);
    steps(s, 4000);
    r.x_term = std::max(s.node(x).term(), pair ? s.node(y).term() : 0);
    s.heal();
    if (!run_until_stable(s, 6000)) { r.violation = !s.ok(); r.unstable = s.ok(); return r; }
    r.final_term = s.node(stable_leader(s)).term();
    r.disrupted = stable_leader(s) != l1 || r.final_term != r.t1;
    r.violation = !s.ok();
    return r;
}

int main() {
    constexpr int kSeeds = 40;

    std::printf("== 1. The runaway-term weakness: without and with pre-vote (5 nodes, %d seeds, one node cut off and a 2-node minority) ==\n", kSeeds);
    {
        int off_dis = 0, on_dis = 0, on_ran = 0, on_term_moved = 0, bad = 0;
        Term worst_off = 0;
        for (uint64_t seed = 1; seed <= kSeeds; ++seed) {
            for (bool pair : {false, true}) {
                const Isolation a = isolate_and_heal(seed, make_cfg(5, false, false), pair);
                const Isolation b = isolate_and_heal(seed, make_cfg(5, true, false), pair);
                bad += a.violation + b.violation + a.unstable + b.unstable;
                off_dis += a.disrupted;
                if (a.x_term - a.t1 > worst_off) worst_off = a.x_term - a.t1;
                on_ran += b.ran;
                on_dis += b.disrupted;
                on_term_moved += b.x_term != b.t1;
            }
        }
        std::printf("   without pre-vote: healthy leader deposed after the heal in %d of %d runs (isolated node's term ran up to +%llu)\n", off_dis, 2 * kSeeds, (ULL)worst_off);
        std::printf("   with pre-vote:    healthy leader deposed in %d of %d runs; an isolated node's term moved in %d runs\n", on_dis, on_ran, on_term_moved);
        CHECK(bad == 0);
        CHECK(off_dis > 0);
        CHECK(on_ran == 2 * kSeeds && on_dis == 0 && on_term_moved == 0);
    }

    std::printf("== 2. Liveness is kept ==\n");
    {
        int elected = 0, runs = 0;
        for (uint32_t n : {3u, 5u}) for (uint64_t seed = 1; seed <= 30; ++seed) {
            Sim s(make_cfg(n, true, true), seed);
            if (!run_until_stable(s, 3000)) continue;
            ++runs;
            const NodeId l = stable_leader(s);
            s.crash(l);
            elected += run_until_stable(s, 4000);
        }
        std::printf("   leader crashed, new leader elected again: %d of %d runs\n", elected, runs);
        CHECK(runs == 60 && elected == runs);
        Sim one(make_cfg(1, true, true), 7);
        CHECK(run_until_stable(one, 2000));
        Sim five(make_cfg(5, true, true), 51);
        CHECK(run_until_stable(five, 3000));
        const NodeId l = stable_leader(five);
        five.crash(l); five.crash((l + 1) % 5); five.crash((l + 2) % 5);
        for (int i = 0; i < 6000; ++i) { five.step(); for (NodeId x : {(l + 3) % 5, (l + 4) % 5}) CHECK(five.node(x).role() != Role::Leader); if (g_fail) break; }
        CHECK(five.ok());
        five.restart((l + 1) % 5);
        CHECK(run_until_stable(five, 6000));
        std::printf("   1-node cluster elects itself; no leader without a majority; a leader once a majority is back\n");
    }

    std::printf("== 3. CheckQuorum ==\n");
    {
        int stepped = 0, still = 0, runs = 0; uint64_t worst = 0;
        for (uint64_t seed = 1; seed <= 30; ++seed) {
            for (bool cq : {true, false}) {
                Sim s(make_cfg(5, true, cq), seed);
                if (!run_until_stable(s, 3000)) continue;
                const NodeId l1 = stable_leader(s), buddy = (l1 + 1) % 5;
                std::vector<uint8_t> g(5, 0);
                g[l1] = 1; g[buddy] = 1;
                s.set_groups(g);
                uint64_t when = 0;
                for (uint64_t i = 1; i <= 1500 && !when; ++i) { s.step(); if (s.node(l1).role() != Role::Leader) when = i; }
                if (cq) { ++runs; stepped += when > 0; if (when > worst) worst = when; }
                else still += when == 0;
            }
        }
        std::printf("   minority leader with CheckQuorum on: stepped down in %d of %d runs (slowest after %llu ms); off: still leader after 1500 ms in %d runs\n", stepped, runs, (ULL)worst, still);
        CHECK(runs > 0 && stepped == runs && worst <= 1000);
        CHECK(still == runs);
        int flapped = 0, n_ok = 0;
        for (uint64_t seed = 1; seed <= 30; ++seed) {
            Sim s(make_cfg(5, true, true), seed);
            if (!run_until_stable(s, 3000)) continue;
            ++n_ok;
            const NodeId l = stable_leader(s); const Term t = s.node(l).term();
            steps(s, 10000);
            flapped += !(s.node(l).role() == Role::Leader && s.node(l).term() == t);
            CHECK(s.ok());
        }
        std::printf("   healthy cluster, 10 s on a perfect network: leader changed in %d of %d seeds\n", flapped, n_ok);
        CHECK(flapped == 0);
    }

    std::printf("== 4. Chaos with pre-vote and CheckQuorum on (safety checks of the simulator) ==\n");
    for (uint32_t n : {3u, 5u}) {
        int bad = 0, with_commit_on = 0, with_commit_off = 0; uint64_t c_on = 0, c_off = 0;
        for (uint64_t seed = 1; seed <= 150; ++seed) {
            for (bool on : {true, false}) {
                Sim s(make_cfg(n, on, on), seed, rough_net());
                for (uint64_t i = 0; i < 6000 && s.ok(); ++i) chaos_step(s, n, 20);
                if (s.ok()) s.check_now();
                if (!s.ok()) { if (bad++ < 3) std::printf("   seed %llu: %s\n", (ULL)seed, s.violation().c_str()); continue; }
                uint64_t real = 0;
                for (const Entry& e : s.committed()) real += e.cmd != 0;
                (on ? with_commit_on : with_commit_off) += real > 0;
                (on ? c_on : c_off) += real;
            }
        }
        std::printf("   %u nodes, 150 seeds: violations %d; seeds with a commit: on %d / off %d; commands committed: on %llu / off %llu\n", n, bad, with_commit_on, with_commit_off, (ULL)c_on, (ULL)c_off);
        CHECK(bad == 0);
        CHECK(with_commit_on > 100);
    }

    std::printf("== 5. The scenarios of part 1 must catch wrong implementations ==\n");
    for (uint32_t bug : {(uint32_t)kNoLeaderStickiness, (uint32_t)kPreVoteAdoptsTerm}) {
        int dis = 0;
        for (uint64_t seed = 1; seed <= kSeeds; ++seed) for (bool pair : {false, true}) dis += isolate_and_heal(seed, make_cfg(5, true, false, bug), pair).disrupted;
        std::printf("   mutant '%s': healthy leader deposed in %d of %d runs\n",
                    bug == kNoLeaderStickiness ? "grant pre-vote although a leader was just heard" : "granting a pre-vote changes the term", dis, 2 * kSeeds);
        CHECK(dis > 0);
    }

    if (g_fail) { std::printf("\n%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("\nall pre-vote / check-quorum tests passed\n");
    return 0;
}
