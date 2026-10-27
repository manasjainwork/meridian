// Raft leader election, tested three ways: direct unit tests of the node state machine, scenario tests on the
// deterministic simulator, and randomized chaos runs (loss, duplication, reordering, crashes, partitions) where
// the simulator checks the safety invariants after every single event. A final section switches deliberate bugs
// on and requires the tests to catch each of them: a test suite that cannot fail proves nothing.
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "meridian/raft_node.hpp"
#include "meridian/raft_sim.hpp"

using namespace meridian::raft;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

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

struct ChaosStats { uint64_t seeds = 0, with_leader = 0, multi_term_leaders = 0, delivered = 0; Term max_term = 0; };

// One millisecond of random faults. Safety only: nothing here promises progress.
static void chaos_step(Sim& s, uint32_t n) {
    const uint64_t roll = s.rand() % 100000;
    if (roll < 200) { const NodeId v = static_cast<NodeId>(s.rand() % n); if (s.alive(v)) s.crash(v); }
    else if (roll < 1200) { const NodeId v = static_cast<NodeId>(s.rand() % n); if (!s.alive(v)) s.restart(v); }
    else if (roll < 1500) { std::vector<uint8_t> g(n); for (auto& x : g) x = static_cast<uint8_t>(s.rand() % 3); s.set_groups(g); }
    else if (roll < 2000) s.heal();
    s.step();
}
static void chaos(Sim& s, uint32_t n, uint64_t n_steps) { for (uint64_t i = 0; i < n_steps && s.ok(); ++i) chaos_step(s, n); }

static NetConfig rough_net() { NetConfig n; n.drop_pct = 15; n.dup_pct = 10; n.delay_min = 1; n.delay_max = 100; return n; }

// Runs one chaos seed. Returns false if a safety invariant broke (and prints why).
static bool chaos_seed(uint32_t n, uint64_t seed, uint32_t bugs, const std::vector<std::vector<Term>>& logs, ChaosStats* st, bool quiet = false) {
    Config cfg; cfg.n = n; cfg.bugs = bugs;
    Sim s(cfg, seed, rough_net(), logs);
    chaos(s, n, 5000);
    if (st) {
        ++st->seeds;
        uint32_t leaders = 0;
        for (NodeId i = 0; i < n; ++i) leaders += s.ever_leader(i);
        st->with_leader += leaders > 0;
        st->multi_term_leaders += leaders > 1;
        st->delivered += s.delivered();
        for (NodeId i = 0; i < n; ++i) if (s.alive(i) && s.node(i).term() > st->max_term) st->max_term = s.node(i).term();
    }
    if (!s.ok()) { if (!quiet) std::printf("  safety violation: %s\n", s.violation().c_str()); return false; }
    return true;
}

int main() {
    // ---------------------------------------------------------------- node-level unit tests
    {
        Config cfg; cfg.n = 5;
        std::vector<Message> out;
        {   // a vote already given in this term is not given to someone else, even after a restart
            Persistent p; p.current_term = 5; p.voted_for = 0;
            Node n(1, cfg, p, 1, 0);
            n.on_message({MsgType::RequestVote, 2, 1, 5, 0, 0, false}, 10, out);
            CHECK(out.size() == 1 && out.back().type == MsgType::VoteReply && !out.back().ok);
            n.on_message({MsgType::RequestVote, 0, 1, 5, 0, 0, false}, 11, out);        // the same candidate again: fine
            CHECK(out.back().ok);
            n.on_message({MsgType::RequestVote, 2, 1, 6, 0, 0, false}, 12, out);        // newer term: a fresh vote
            CHECK(out.back().ok && n.term() == 6 && n.persistent().voted_for == 2);
        }
        {   // stale-term request is refused and carries our newer term
            Persistent p; p.current_term = 7;
            Node n(1, cfg, p, 1, 0);
            n.on_message({MsgType::RequestVote, 2, 1, 4, 0, 0, false}, 10, out);
            CHECK(!out.back().ok && out.back().term == 7 && n.term() == 7 && n.persistent().voted_for == kNone);
        }
        {   // newer term but stale log: term is adopted, vote is refused, voted_for stays empty
            Persistent p; p.current_term = 2; p.log = {1, 2};
            Node n(1, cfg, p, 1, 0);
            n.on_message({MsgType::RequestVote, 0, 1, 3, 5, 1, false}, 10, out);        // candidate: longer log but older last term
            CHECK(!out.back().ok && n.term() == 3 && n.persistent().voted_for == kNone);
            n.on_message({MsgType::RequestVote, 0, 1, 3, 1, 2, false}, 11, out);        // same last term, shorter log: still behind
            CHECK(!out.back().ok && n.persistent().voted_for == kNone);
            n.on_message({MsgType::RequestVote, 3, 1, 3, 2, 2, false}, 12, out);        // equal log: ok
            CHECK(out.back().ok && n.persistent().voted_for == 3);
        }
        {   // a duplicated vote reply must not be counted twice
            Node n(0, cfg, Persistent{}, 1, 0);
            out.clear();
            n.tick(1000, out);                                                          // election timeout: becomes candidate, term 1
            CHECK(n.role() == Role::Candidate && n.term() == 1 && out.size() == 4);
            n.on_message({MsgType::VoteReply, 1, 0, 1, 0, 0, true}, 1001, out);
            n.on_message({MsgType::VoteReply, 1, 0, 1, 0, 0, true}, 1001, out);         // duplicate
            CHECK(n.role() == Role::Candidate);                                         // 2 of 5 votes: not enough
            n.on_message({MsgType::VoteReply, 2, 0, 1, 0, 0, true}, 1002, out);
            CHECK(n.role() == Role::Leader);
        }
        {   // vote replies from an old term, or refusals, do not help
            Node n(0, cfg, Persistent{}, 1, 0);
            out.clear();
            n.tick(1000, out);
            n.on_message({MsgType::VoteReply, 1, 0, 0, 0, 0, true}, 1001, out);
            n.on_message({MsgType::VoteReply, 2, 0, 1, 0, 0, false}, 1001, out);
            n.on_message({MsgType::VoteReply, 3, 0, 1, 0, 0, false}, 1001, out);
            CHECK(n.role() == Role::Candidate);
        }
        {   // heartbeats: old term refused; same term makes a candidate step down; a higher term wins over anything
            Node n(0, cfg, Persistent{}, 1, 0);
            out.clear();
            n.tick(1000, out);                                                          // candidate, term 1
            n.on_message({MsgType::AppendEntries, 3, 0, 0, 0, 0, false}, 1001, out);
            CHECK(n.role() == Role::Candidate && !out.back().ok && out.back().term == 1);
            n.on_message({MsgType::AppendEntries, 3, 0, 1, 0, 0, false}, 1002, out);
            CHECK(n.role() == Role::Follower && n.leader() == 3 && out.back().ok);
            n.on_message({MsgType::AppendEntries, 4, 0, 9, 0, 0, false}, 1003, out);
            CHECK(n.term() == 9 && n.leader() == 4);
        }
        {   // a leader that sees a higher term steps down
            Node n(0, cfg, Persistent{}, 1, 0);
            out.clear();
            n.tick(1000, out);
            n.on_message({MsgType::VoteReply, 1, 0, 1, 0, 0, true}, 1001, out);
            n.on_message({MsgType::VoteReply, 2, 0, 1, 0, 0, true}, 1001, out);
            CHECK(n.role() == Role::Leader);
            n.on_message({MsgType::RequestVote, 4, 0, 2, 0, 0, false}, 1002, out);
            CHECK(n.role() == Role::Follower && n.term() == 2);
        }
        {   // single-node cluster elects itself immediately
            Config one; one.n = 1;
            Node n(0, one, Persistent{}, 1, 0);
            out.clear();
            n.tick(1000, out);
            CHECK(n.role() == Role::Leader && n.term() == 1);
        }
    }

    // ---------------------------------------------------------------- scenarios on the simulator
    {   // a healthy cluster elects exactly one leader quickly and keeps it
        for (uint32_t n : {1u, 3u, 5u, 7u, 9u}) {
            Config cfg; cfg.n = n;
            Sim s(cfg, 11 + n);
            CHECK(run_until_stable(s, 3000));
            const NodeId l = stable_leader(s);
            const Term t = s.node(l).term();
            CHECK(t <= 3);                                                             // no needless extra elections
            steps(s, 20000);
            CHECK(s.ok() && s.stable() && stable_leader(s) == l && s.node(l).term() == t);   // heartbeats keep everyone calm
        }
    }
    {   // leader crash -> new leader in a higher term; the old leader returns as a follower without disturbing anyone
        Config cfg; cfg.n = 5;
        Sim s(cfg, 21);
        CHECK(run_until_stable(s, 3000));
        const NodeId l1 = stable_leader(s);
        const Term t1 = s.node(l1).term();
        s.crash(l1);
        CHECK(run_until_stable(s, 5000));
        const NodeId l2 = stable_leader(s);
        const Term t2 = s.node(l2).term();
        CHECK(l2 != l1 && t2 > t1);
        s.restart(l1);
        steps(s, 5000);
        CHECK(s.ok() && s.stable() && stable_leader(s) == l2 && s.node(l2).term() == t2);
        CHECK(s.node(l1).role() == Role::Follower && s.node(l1).term() == t2 && s.node(l1).leader() == l2);
    }
    {   // leader in a minority partition: the majority elects a new one; after healing the stale leader steps down
        Config cfg; cfg.n = 5;
        Sim s(cfg, 31);
        CHECK(run_until_stable(s, 3000));
        const NodeId l1 = stable_leader(s);
        const NodeId buddy = (l1 + 1) % 5;
        const Term t1 = s.node(l1).term();
        std::vector<uint8_t> g(5, 0);
        g[l1] = 1; g[buddy] = 1;
        s.set_groups(g);
        NodeId l2 = kNone;
        for (int i = 0; i < 6000 && l2 == kNone; ++i) {
            s.step();
            for (NodeId x = 0; x < 5; ++x) if (g[x] == 0 && s.node(x).role() == Role::Leader && s.node(x).term() > t1) l2 = x;
        }
        CHECK(l2 != kNone);
        CHECK(s.node(l1).role() == Role::Leader && s.node(l1).term() == t1);           // stale leader does not know yet (no CheckQuorum yet)
        s.heal();
        CHECK(run_until_stable(s, 5000));
        CHECK(stable_leader(s) == l2 && s.node(l1).role() == Role::Follower && s.ok());
    }
    {   // CHARACTERISATION of a known weakness: a follower cut off for a while keeps raising its term; when it comes back
        // it forces the healthy leader to step down. This is what pre-vote (next step) removes; the assertion flips then.
        Config cfg; cfg.n = 5;
        Sim s(cfg, 41);
        CHECK(run_until_stable(s, 3000));
        const NodeId l1 = stable_leader(s);
        const NodeId x = (l1 + 2) % 5;
        const Term t1 = s.node(l1).term();
        std::vector<uint8_t> g(5, 0);
        g[x] = 1;
        s.set_groups(g);
        steps(s, 4000);
        CHECK(s.node(l1).role() == Role::Leader && s.node(l1).term() == t1);           // the cluster itself is undisturbed
        CHECK(s.node(x).term() >= t1 + 8);                                              // the isolated node's term ran away
        const Term runaway = s.node(x).term();
        s.heal();
        CHECK(run_until_stable(s, 6000));
        CHECK(s.node(stable_leader(s)).term() >= runaway);                              // disruption: everyone jumped to the runaway term
        std::printf("  characterisation: isolated node pushed term %llu -> %llu, healthy leader was deposed on heal (pre-vote will fix)\n",
                    static_cast<unsigned long long>(t1), static_cast<unsigned long long>(runaway));
    }
    {   // without a majority nobody can win; with a majority again, a leader appears
        Config cfg; cfg.n = 5;
        Sim s(cfg, 51);
        CHECK(run_until_stable(s, 3000));
        const NodeId l = stable_leader(s);
        s.crash(l); s.crash((l + 1) % 5); s.crash((l + 2) % 5);
        for (int i = 0; i < 8000; ++i) {
            s.step();
            for (NodeId x : {(l + 3) % 5, (l + 4) % 5}) CHECK(s.node(x).role() != Role::Leader);
            if (g_fail) break;
        }
        CHECK(s.ok());
        s.restart((l + 1) % 5);
        CHECK(run_until_stable(s, 6000));
    }
    {   // election restriction: a node whose log is behind can never be elected while a majority is ahead of it
        const std::vector<std::vector<std::vector<Term>>> cases = {
            {{1}, {1, 2, 2}, {1, 2, 2}, {1, 2, 2}, {1, 2, 2}},    // shorter log, older last term
            {{1, 1, 1}, {1, 2}, {1, 2}, {1, 2}, {1, 2}},          // LONGER log but older last term: term beats length
        };
        for (const auto& logs : cases) {
            int elected_elsewhere = 0;
            for (uint64_t seed = 0; seed < 300; ++seed) {
                Config cfg; cfg.n = 5;
                Sim s(cfg, 1000 + seed, rough_net(), logs);
                // Node 0 must not win while its log is behind. Once a leader has overwritten or extended its log it is no
                // longer behind, and may legitimately win later, so the check stops at that moment.
                const std::vector<Entry> initial = s.disk_log(0);
                for (int i = 0; i < 3000 && s.ok(); ++i) {
                    chaos_step(s, 5);
                    if (s.ever_leader(0)) { CHECK(!"stale node became leader"); break; }
                    if (!(s.disk_log(0) == initial)) break;
                }
                CHECK(s.ok());
                if (g_fail) break;
            }
            for (uint64_t seed = 0; seed < 50; ++seed) {
                Config cfg; cfg.n = 5;
                Sim s(cfg, 5000 + seed, {}, logs);
                CHECK(run_until_stable(s, 4000));
                elected_elsewhere += stable_leader(s) != 0;
            }
            CHECK(elected_elsewhere == 50);                       // and the cluster still makes progress without node 0
        }
    }

    // ---------------------------------------------------------------- randomized safety under chaos
    {
        ChaosStats st;
        int failures = 0;
        for (uint32_t n : {3u, 5u, 7u})
            for (uint64_t seed = 0; seed < 1200; ++seed)
                if (!chaos_seed(n, seed * 7919 + n, kNoBug, {}, &st)) ++failures;
        CHECK(failures == 0);
        // The runs must not be vacuous: leaders were really elected, repeatedly, under loss/duplication/partitions.
        CHECK(st.with_leader > st.seeds * 9 / 10);
        CHECK(st.multi_term_leaders > st.seeds / 3);
        std::printf("  chaos: %llu seeds, %llu messages delivered, %llu seeds elected a leader, %llu saw several leaders over time, max term %llu\n",
                    static_cast<unsigned long long>(st.seeds), static_cast<unsigned long long>(st.delivered), static_cast<unsigned long long>(st.with_leader),
                    static_cast<unsigned long long>(st.multi_term_leaders), static_cast<unsigned long long>(st.max_term));
    }
    {   // liveness: after the chaos stops and the network is healthy again, every seed converges to one stable leader
        int stuck = 0;
        for (uint32_t n : {3u, 5u, 7u})
            for (uint64_t seed = 0; seed < 300; ++seed) {
                Config cfg; cfg.n = n;
                Sim s(cfg, seed * 104729 + n, rough_net());
                chaos(s, n, 4000);
                s.heal();
                for (NodeId i = 0; i < n; ++i) s.restart(i);
                s.set_net(NetConfig{});
                if (!s.ok() || !run_until_stable(s, 10000)) ++stuck;
            }
        CHECK(stuck == 0);
    }

    // ---------------------------------------------------------------- determinism
    {
        Config cfg; cfg.n = 5;
        std::set<uint64_t> digests;
        for (uint64_t seed = 0; seed < 200; ++seed) {
            Sim a(cfg, seed, rough_net()), b(cfg, seed, rough_net());
            chaos(a, 5, 2000); chaos(b, 5, 2000);
            CHECK(a.digest() == b.digest() && a.delivered() == b.delivered());       // same seed, same history, bit for bit
            digests.insert(a.digest());
        }
        CHECK(digests.size() == 200);                                                // different seeds really differ
    }

    // ---------------------------------------------------------------- do the tests catch real bugs?
    {
        struct M { const char* name; uint32_t bug; uint32_t n; };
        const M ms[] = {
            {"vote twice in one term", kVoteTwice, 5},
            {"forget the vote after a restart", kForgetVoteOnRestart, 5},
            {"quorum of n/2 instead of n/2+1", kSmallQuorum, 5},
            {"count duplicated votes twice", kCountDuplicateVotes, 5},
        };
        for (const M& m : ms) {
            uint64_t found = ~0ull;
            for (uint64_t seed = 0; seed < 3000 && found == ~0ull; ++seed)
                if (!chaos_seed(m.n, seed * 7919 + m.n, m.bug, {}, nullptr, true)) found = seed;
            std::printf("  mutant '%s': %s\n", m.name, found == ~0ull ? "NOT DETECTED" : ("caught at chaos seed " + std::to_string(found * 7919 + m.n)).c_str());
            CHECK(found != ~0ull);
        }
        {   // skipping the log up-to-date check lets a node with a stale log win
            const std::vector<std::vector<Term>> logs = {{1}, {1, 2, 2}, {1, 2, 2}, {1, 2, 2}, {1, 2, 2}};
            bool caught = false;
            for (uint64_t seed = 0; seed < 300 && !caught; ++seed) {
                Config cfg; cfg.n = 5; cfg.bugs = kSkipLogCheck;
                Sim s(cfg, 1000 + seed, rough_net(), logs);
                const std::vector<Entry> initial = s.disk_log(0);
                for (int i = 0; i < 3000 && s.ok() && !caught; ++i) {      // same stop rule as the clean test above
                    chaos_step(s, 5);
                    if (s.ever_leader(0)) caught = true;
                    else if (!(s.disk_log(0) == initial)) break;
                }
            }
            std::printf("  mutant 'skip the log up-to-date check': %s\n", caught ? "caught (stale node won an election)" : "NOT DETECTED");
            CHECK(caught);
        }
        {   // the unit test for duplicate votes also catches the duplicate-counting bug on its own
            Config cfg; cfg.n = 5; cfg.bugs = kCountDuplicateVotes;
            Node n(0, cfg, Persistent{}, 1, 0);
            std::vector<Message> out;
            n.tick(1000, out);
            n.on_message({MsgType::VoteReply, 1, 0, 1, 0, 0, true}, 1001, out);
            n.on_message({MsgType::VoteReply, 1, 0, 1, 0, 0, true}, 1001, out);
            CHECK(n.role() == Role::Leader);                                           // the buggy node wrongly wins: the unit test would fail
        }
    }

    std::printf(g_fail ? "%d FAILED\n" : "all raft election tests passed\n", g_fail);
    return g_fail != 0;
}
