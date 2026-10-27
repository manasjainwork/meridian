// Raft log replication and the commit rule, tested at three levels: the node state machine alone, scripted scenarios
// on the deterministic simulator, and randomized chaos (loss, duplication, reordering, crashes, partitions, client
// commands) in which the simulator checks State Machine Safety, Leader Completeness, Log Matching, commit durability
// and more after every event. A last section switches deliberate bugs on and requires the checks to catch each one.
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "meridian/raft_node.hpp"
#include "meridian/raft_sim.hpp"

using namespace meridian::raft;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
using ULL = unsigned long long;

static Message ae(NodeId from, NodeId to, Term term, uint64_t prev, Term prev_term, std::vector<Entry> es, uint64_t commit) {
    Message m{MsgType::AppendEntries, from, to, term, prev, prev_term, false};
    m.leader_commit = commit;
    m.entries = std::move(es);
    return m;
}
static Message ar(NodeId from, NodeId to, Term term, bool ok, uint64_t match) {
    Message m{MsgType::AppendReply, from, to, term, 0, 0, ok};
    m.match_index = match;
    return m;
}

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

// All live nodes hold exactly the leader's log and have committed all of it.
static bool converged(const Sim& s, uint32_t n, NodeId l) {
    for (NodeId i = 0; i < n; ++i) {
        if (!s.alive(i)) continue;
        if (!(s.disk_log(i) == s.disk_log(l))) return false;
        if (s.node(i).commit_index() != s.disk_log(l).size()) return false;
    }
    return true;
}
static bool run_until_converged(Sim& s, uint32_t n, NodeId l, uint64_t max_steps) {
    for (uint64_t i = 0; i < max_steps; ++i) {
        s.step();
        if (!s.ok()) return false;
        if (converged(s, n, l)) return true;
    }
    return false;
}
static bool committed_has(const Sim& s, uint64_t cmd) {
    for (const Entry& e : s.committed()) if (e.cmd == cmd) return true;
    return false;
}

static void chaos_step(Sim& s, uint32_t n, uint32_t propose_per_1000) {
    const uint64_t roll = s.rand() % 100000;
    if (roll < 200) { const NodeId v = static_cast<NodeId>(s.rand() % n); if (s.alive(v)) s.crash(v); }
    else if (roll < 1200) { const NodeId v = static_cast<NodeId>(s.rand() % n); if (!s.alive(v)) s.restart(v); }
    else if (roll < 1500) { std::vector<uint8_t> g(n); for (auto& x : g) x = static_cast<uint8_t>(s.rand() % 3); s.set_groups(g); }
    else if (roll < 2000) s.heal();
    if (s.rand() % 1000 < propose_per_1000) s.propose_any();
    s.step();
}
static void chaos(Sim& s, uint32_t n, uint64_t n_steps, uint32_t propose_per_1000 = 20) {
    for (uint64_t i = 0; i < n_steps && s.ok(); ++i) chaos_step(s, n, propose_per_1000);
}
static NetConfig rough_net() { NetConfig n; n.drop_pct = 15; n.dup_pct = 10; n.delay_min = 1; n.delay_max = 100; return n; }

struct Stats { uint64_t seeds = 0, with_commit = 0, committed = 0, truncated = 0, delivered = 0, proposed = 0; };

static bool chaos_seed(uint32_t n, uint64_t seed, uint32_t bugs, Stats* st, std::string* why = nullptr, uint32_t max_batch = 64, bool noop = true) {
    Config cfg; cfg.n = n; cfg.bugs = bugs; cfg.max_batch = max_batch; cfg.noop = noop;
    Sim s(cfg, seed, rough_net());
    chaos(s, n, 6000);
    if (s.ok()) s.check_now();
    if (st) {
        ++st->seeds;
        uint64_t real = 0;
        for (const Entry& e : s.committed()) real += e.cmd != 0;
        st->with_commit += real > 0;
        st->committed += real;
        st->truncated += s.truncations();
        st->delivered += s.delivered();
        st->proposed += s.proposed();
    }
    if (!s.ok()) { if (why) *why = s.violation(); return false; }
    return true;
}

int main() {
    // ---------------------------------------------------------------- node-level unit tests
    {
        Config cfg; cfg.n = 5;
        std::vector<Message> out;
        {   // the consistency check on the previous entry, and the hint a refusal carries
            Persistent p; p.current_term = 1; p.log = {Entry{1, 1}, Entry{1, 2}};
            Node f(1, cfg, p, 1, 0);
            f.on_message(ae(0, 1, 1, 3, 1, {}, 0), 10, out);                      // prev beyond our log
            CHECK(!out.back().ok && out.back().match_index == 2);
            f.on_message(ae(0, 1, 1, 2, 2, {}, 0), 11, out);                      // prev exists but with another term
            CHECK(!out.back().ok && out.back().match_index == 1);
            f.on_message(ae(0, 1, 1, 2, 1, {}, 0), 12, out);                      // matches
            CHECK(out.back().ok && out.back().match_index == 2);
            f.on_message(ae(0, 1, 1, 0, 0, {}, 0), 13, out);                      // prev = 0 always matches
            CHECK(out.back().ok && out.back().match_index == 0 && f.persistent().log.size() == 2);
        }
        {   // appending, duplicates, and an older (shorter) message arriving late must never delete anything
            Node f(1, cfg, Persistent{}, 1, 0);
            f.on_message(ae(0, 1, 1, 0, 0, {Entry{1, 10}, Entry{1, 11}, Entry{1, 12}}, 0), 10, out);
            CHECK(out.back().ok && out.back().match_index == 3 && f.persistent().log.size() == 3);
            f.on_message(ae(0, 1, 1, 0, 0, {Entry{1, 10}, Entry{1, 11}, Entry{1, 12}}, 0), 11, out);   // duplicate
            CHECK(f.persistent().log.size() == 3);
            f.on_message(ae(0, 1, 1, 0, 0, {Entry{1, 10}}, 0), 12, out);                                // reordered, older
            CHECK(out.back().ok && out.back().match_index == 1 && f.persistent().log.size() == 3);     // acked only what it verified
        }
        {   // a real conflict deletes the entry and everything after it
            Persistent p; p.current_term = 2; p.log = {Entry{1, 1}, Entry{1, 2}, Entry{2, 3}};
            Node f(1, cfg, p, 1, 0);
            f.on_message(ae(0, 1, 3, 1, 1, {Entry{3, 9}}, 0), 10, out);
            CHECK(out.back().ok && f.persistent().log.size() == 2 && f.persistent().log[1] == Entry(3, 9) && f.term() == 3);
        }
        {   // commit = min(leader_commit, last new entry); never goes backwards
            Node f(1, cfg, Persistent{}, 1, 0);
            f.on_message(ae(0, 1, 1, 0, 0, {Entry{1, 1}, Entry{1, 2}}, 10), 10, out);
            CHECK(f.commit_index() == 2);                                                               // not 10
            f.on_message(ae(0, 1, 1, 0, 0, {}, 5), 11, out);                                            // heartbeat that verifies nothing
            CHECK(f.commit_index() == 2);
        }
        {   // an old-term AppendEntries is refused and changes nothing
            Persistent p; p.current_term = 5; p.log = {Entry{5, 1}};
            Node f(1, cfg, p, 1, 0);
            f.on_message(ae(0, 1, 4, 0, 0, {Entry{4, 99}}, 1), 10, out);
            CHECK(!out.back().ok && out.back().term == 5 && f.persistent().log.size() == 1 && f.commit_index() == 0);
        }
        {   // the commit rule (Raft paper, Figure 8): old-term entries are committed only together with a current-term entry
            Persistent p; p.current_term = 3; p.log = {Entry{1, 5}, Entry{2, 6}};
            Node l(0, cfg, p, 1, 0);
            out.clear();
            l.tick(1000, out);                                                                          // candidate, term 4
            l.on_message(Message{MsgType::VoteReply, 1, 0, 4, 0, 0, true}, 1001, out);
            l.on_message(Message{MsgType::VoteReply, 2, 0, 4, 0, 0, true}, 1001, out);
            CHECK(l.role() == Role::Leader && l.persistent().log.size() == 3 && l.persistent().log[2] == Entry(4, 0));   // no-op
            l.on_message(ar(1, 0, 4, true, 2), 1002, out);
            l.on_message(ar(2, 0, 4, true, 2), 1002, out);
            CHECK(l.commit_index() == 0);                       // entry 2 is on 3 of 5 nodes, but it is from term 2
            l.on_message(ar(1, 0, 4, true, 3), 1003, out);
            l.on_message(ar(2, 0, 4, true, 3), 1003, out);
            CHECK(l.commit_index() == 3);                       // the term-4 no-op reached a majority: everything below commits
        }
        {   // back-off on refusal uses the hint, resends at once, and a late refusal cannot undo progress
            Persistent p; p.current_term = 1; p.log = {Entry{1, 1}, Entry{1, 2}, Entry{1, 3}};
            Node l(0, cfg, p, 1, 0);
            out.clear();
            l.tick(1000, out);
            l.on_message(Message{MsgType::VoteReply, 1, 0, 2, 0, 0, true}, 1001, out);
            l.on_message(Message{MsgType::VoteReply, 2, 0, 2, 0, 0, true}, 1001, out);
            CHECK(l.role() == Role::Leader);                                                            // log: 3 entries + no-op
            out.clear();
            l.on_message(ar(3, 0, 2, false, 0), 1002, out);                                             // follower 3 is empty
            CHECK(out.size() == 1 && out[0].to == 3 && out[0].last_log_index == 0 && out[0].entries.size() == 4);
            l.on_message(ar(1, 0, 2, true, 4), 1003, out);                                              // follower 1 is fully caught up
            out.clear();
            l.on_message(ar(1, 0, 2, false, 0), 1004, out);                                             // a late, stale refusal from 1
            CHECK(out.size() == 1 && out[0].last_log_index == 4 && out[0].entries.empty());             // next never goes below match + 1
        }
        {   // only the leader accepts commands; the command is replicated at once
            Node f(1, cfg, Persistent{}, 1, 0);
            out.clear();
            CHECK(f.propose(7, 5, out) == 0 && out.empty() && f.persistent().log.empty());
            Node l(0, cfg, Persistent{}, 1, 0);
            l.tick(1000, out);
            l.on_message(Message{MsgType::VoteReply, 1, 0, 1, 0, 0, true}, 1001, out);
            l.on_message(Message{MsgType::VoteReply, 2, 0, 1, 0, 0, true}, 1001, out);
            out.clear();
            CHECK(l.propose(7, 1002, out) == 2 && out.size() == 4 && out[0].type == MsgType::AppendEntries);
        }
        {   // batching: at most max_batch entries per message, and the next batch follows an ack without waiting
            Config small = cfg; small.max_batch = 3;
            Persistent p; p.current_term = 1;
            for (uint64_t i = 1; i <= 9; ++i) p.log.push_back(Entry{1, i});
            Node l(0, small, p, 1, 0);
            out.clear();
            l.tick(1000, out);
            l.on_message(Message{MsgType::VoteReply, 1, 0, 2, 0, 0, true}, 1001, out);
            l.on_message(Message{MsgType::VoteReply, 2, 0, 2, 0, 0, true}, 1001, out);
            l.on_message(ar(3, 0, 2, false, 0), 1002, out);
            out.clear();
            l.on_message(ar(3, 0, 2, true, 3), 1003, out);
            CHECK(out.size() == 1 && out[0].last_log_index == 3 && out[0].entries.size() == 3);
        }
        {   // a single-node cluster commits as soon as the command is in its own log
            Config one; one.n = 1;
            Node l(0, one, Persistent{}, 1, 0);
            out.clear();
            l.tick(1000, out);
            CHECK(l.role() == Role::Leader && l.commit_index() == 1);                                   // the no-op
            CHECK(l.propose(5, 1001, out) == 2 && l.commit_index() == 2);
        }
    }

    // ---------------------------------------------------------------- scripted scenarios
    {   // healthy cluster, many commands: everyone ends with the same log, commands committed in order
        for (uint32_t n : {1u, 3u, 5u, 7u}) {
            Config cfg; cfg.n = n;
            Sim s(cfg, 100 + n);
            CHECK(run_until_stable(s, 3000));
            const NodeId l = stable_leader(s);
            for (int k = 0; k < 100; ++k) { CHECK(s.propose_to(l) > 0); s.step(); }
            CHECK(run_until_converged(s, n, l, 3000));
            std::vector<uint64_t> cmds;
            for (const Entry& e : s.committed()) if (e.cmd) cmds.push_back(e.cmd);
            CHECK(cmds.size() == 100);
            for (size_t i = 0; i < cmds.size(); ++i) CHECK(cmds[i] == i + 1);
            CHECK(s.ok());
        }
    }
    {   // the leader dies with commands only partly replicated: committed ones survive, the new leader keeps working
        Config cfg; cfg.n = 5;
        Sim s(cfg, 211);
        CHECK(run_until_stable(s, 3000));
        const NodeId l1 = stable_leader(s);
        for (int k = 0; k < 50; ++k) { s.propose_to(l1); s.step(); }
        steps(s, 12);                                                            // some replicated, some not
        const uint64_t committed_before = s.committed_size();
        const std::vector<Entry> snapshot = s.committed();
        s.crash(l1);
        CHECK(run_until_stable(s, 6000));
        const NodeId l2 = stable_leader(s);
        const uint64_t first = s.next_cmd();
        for (int k = 0; k < 50; ++k) { CHECK(s.propose_to(l2) > 0); s.step(); }
        s.restart(l1);
        CHECK(run_until_converged(s, 5, l2, 6000));
        for (uint64_t c = first; c < first + 50; ++c) CHECK(committed_has(s, c));
        CHECK(s.committed_size() >= committed_before);
        for (size_t i = 0; i < snapshot.size(); ++i) CHECK(s.committed()[i] == snapshot[i]);   // nothing committed was lost or changed
        CHECK(s.ok());
    }
    {   // a follower that missed 500 commands catches up through batched AppendEntries
        Config cfg; cfg.n = 5;
        Sim s(cfg, 311);
        CHECK(run_until_stable(s, 3000));
        const NodeId l = stable_leader(s);
        const NodeId x = (l + 1) % 5;
        std::vector<uint8_t> g(5, 0); g[x] = 1;
        s.set_groups(g);
        for (int k = 0; k < 500; ++k) { s.propose_to(l); s.step(); }
        steps(s, 300);
        CHECK(s.disk_log(x).size() < 10 && s.committed_size() >= 500);          // the others moved on without x
        s.heal();
        CHECK(run_until_stable(s, 6000));
        const NodeId l2 = stable_leader(s);
        CHECK(run_until_converged(s, 5, l2, 6000));
        CHECK(s.disk_log(x).size() >= 500 && s.ok());
    }
    {   // a leader cut off in a minority accepts commands that can never commit; after healing they are overwritten
        Config cfg; cfg.n = 5;
        Sim s(cfg, 411);
        CHECK(run_until_stable(s, 3000));
        const NodeId l1 = stable_leader(s);
        const NodeId buddy = (l1 + 1) % 5;
        std::vector<uint8_t> g(5, 0); g[l1] = 1; g[buddy] = 1;
        s.set_groups(g);
        const uint64_t stale_first = s.next_cmd();
        for (int k = 0; k < 10; ++k) { CHECK(s.propose_to(l1) > 0); s.step(); }
        NodeId l2 = kNone;
        for (int i = 0; i < 6000 && l2 == kNone; ++i) {
            s.step();
            for (NodeId x = 0; x < 5; ++x) if (g[x] == 0 && s.node(x).role() == Role::Leader) l2 = x;
        }
        CHECK(l2 != kNone);
        steps(s, 600);                                                          // the new leader's no-op reaches its majority
        const uint64_t good_first = s.next_cmd();
        for (int k = 0; k < 10; ++k) { CHECK(s.propose_to(l2) > 0); s.step(); }
        steps(s, 1000);
        for (uint64_t c = stale_first; c < stale_first + 10; ++c) CHECK(!committed_has(s, c));   // the minority leader committed nothing
        for (uint64_t c = good_first; c < good_first + 10; ++c) CHECK(committed_has(s, c));
        const uint64_t trunc_before = s.truncations();
        s.heal();
        CHECK(run_until_stable(s, 6000));
        const NodeId l3 = stable_leader(s);
        CHECK(run_until_converged(s, 5, l3, 6000));
        for (uint64_t c = stale_first; c < stale_first + 10; ++c) CHECK(!committed_has(s, c));   // and they are gone for good
        CHECK(s.truncations() > trunc_before);                                  // the stale log really was cut back
        CHECK(s.ok());
    }

    // ---------------------------------------------------------------- randomized safety under chaos
    {
        Stats st;
        int failures = 0;
        std::string why;
        for (uint32_t n : {3u, 5u, 7u})
            for (uint64_t seed = 0; seed < 1000; ++seed)
                if (!chaos_seed(n, seed * 7919 + n, kNoBug, &st, &why)) { ++failures; std::printf("  violation: %s\n", why.c_str()); }
        CHECK(failures == 0);
        CHECK(st.with_commit > st.seeds * 8 / 10);          // not vacuous: most runs really committed client commands
        CHECK(st.truncated > 0);                            // and logs really did get cut back somewhere
        std::printf("  chaos: %llu seeds, %llu commands proposed, %llu committed, %llu log truncations, %llu seeds committed something, %llu messages\n",
                    (ULL)st.seeds, (ULL)st.proposed, (ULL)st.committed, (ULL)st.truncated, (ULL)st.with_commit, (ULL)st.delivered);
    }
    {   // the same chaos with other batch sizes and without the no-op: different message patterns, same safety
        const struct { uint32_t batch; bool noop; } variants[] = {{1, true}, {3, true}, {64, false}, {1, false}};
        for (const auto& v : variants) {
            Stats st;
            int failures = 0;
            std::string why;
            for (uint64_t seed = 0; seed < 1000; ++seed)
                if (!chaos_seed(5, seed * 7919 + 5, kNoBug, &st, &why, v.batch, v.noop)) { ++failures; std::printf("  violation: %s\n", why.c_str()); }
            CHECK(failures == 0);
            CHECK(st.with_commit > st.seeds * 7 / 10);
            std::printf("  chaos (max_batch %u, no-op %s): %llu seeds, %llu committed, %llu log truncations\n", v.batch, v.noop ? "on" : "off", (ULL)st.seeds, (ULL)st.committed, (ULL)st.truncated);
        }
    }
    {   // liveness: once the chaos stops, a stable leader appears and new commands commit and reach every node
        int stuck = 0;
        for (uint32_t n : {3u, 5u, 7u})
            for (uint64_t seed = 0; seed < 150; ++seed) {
                Config cfg; cfg.n = n;
                Sim s(cfg, seed * 104729 + n, rough_net());
                chaos(s, n, 4000);
                s.heal();
                for (NodeId i = 0; i < n; ++i) s.restart(i);
                s.set_net(NetConfig{});
                bool good = s.ok() && run_until_stable(s, 10000);
                if (good) {
                    const NodeId l = stable_leader(s);
                    const uint64_t first = s.next_cmd();
                    for (int k = 0; k < 20; ++k) good = good && s.propose_to(l) > 0;
                    good = good && run_until_converged(s, n, l, 6000);
                    for (uint64_t c = first; good && c < first + 20; ++c) good = committed_has(s, c);
                }
                stuck += !good;
            }
        CHECK(stuck == 0);
    }

    // ---------------------------------------------------------------- determinism
    {
        Config cfg; cfg.n = 5;
        std::set<uint64_t> digests;
        for (uint64_t seed = 0; seed < 100; ++seed) {
            Sim a(cfg, seed, rough_net()), b(cfg, seed, rough_net());
            chaos(a, 5, 2500); chaos(b, 5, 2500);
            CHECK(a.digest() == b.digest() && a.delivered() == b.delivered() && a.committed_size() == b.committed_size());
            digests.insert(a.digest());
        }
        CHECK(digests.size() == 100);
    }

    // ---------------------------------------------------------------- do the checks catch real bugs?
    {
        struct M { const char* name; uint32_t bug; uint32_t batch; };
        const M ms[] = {
            {"commit old-term entries by counting replicas (Figure 8)", kCommitOldTerm, 1},   // needs entries to trickle out one by one
            {"append without checking the previous entry", kNoPrevCheck, 64},
            {"truncate the log on every AppendEntries", kTruncateOnAppend, 64},
            {"commit with n/2 replicas", kCommitSmallQuorum, 64},
            {"follower commit = leader_commit, unbounded", kFollowerCommitUnbounded, 64},
        };
        for (const M& m : ms) {
            uint64_t found = ~0ull;
            std::string why;
            for (uint64_t seed = 0; seed < 4000 && found == ~0ull; ++seed)
                if (!chaos_seed(5, seed * 7919 + 5, m.bug, nullptr, &why, m.batch)) found = seed;
            if (found == ~0ull) std::printf("  mutant '%s': NOT DETECTED\n", m.name);
            else std::printf("  mutant '%s': caught at chaos seed %llu -> %s\n", m.name, (ULL)(found * 7919 + 5), why.c_str());
            CHECK(found != ~0ull);
        }
    }

    std::printf(g_fail ? "%d FAILED\n" : "all raft replication tests passed\n", g_fail);
    return g_fail != 0;
}
