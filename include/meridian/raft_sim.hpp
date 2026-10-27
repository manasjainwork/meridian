#pragma once
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <vector>
#include "meridian/raft_node.hpp"

namespace meridian::raft {

struct NetConfig {
    uint32_t drop_pct = 0;
    uint32_t dup_pct = 0;
    uint32_t delay_min = 1;     // ms, >= 1
    uint32_t delay_max = 10;
};

// Deterministic cluster simulator. Everything that could differ between runs (time, message delay, loss,
// duplication, reordering, crashes, partitions, client commands) is driven by one seed. A failing seed replays exactly.
// Safety is checked with checks that do NOT trust the nodes' own bookkeeping:
//   election:      a node never votes for two candidates in one term (even across a crash); at most one leader per
//                  term; a leader had a real majority of votes; a node's term never goes down
//   replication:   State Machine Safety   - no two nodes ever commit different entries at the same index
//                  Leader Completeness    - a new leader's log contains every entry committed so far
//                  Log Matching           - same index and term in two logs => identical prefixes
//                  Commit durability      - every committed entry is on the disks of a majority of nodes
//                  Leader Append-Only     - a leader never shrinks its log
//                  commit never goes backwards, never beyond the log; no command committed twice; no invented command
class Sim {
public:
    Sim(const Config& cfg, uint64_t seed, NetConfig net = {}, std::vector<std::vector<Term>> initial_logs = {})
        : cfg_(cfg), net_(net), seed_(seed), rng_(seed ^ 0xD1B54A32D192ED03ull) {
        nodes_.resize(cfg.n);
        disk_.resize(cfg.n);
        pv_.assign(cfg.n, 0);
        group_.assign(cfg.n, 0);
        last_term_.assign(cfg.n, 0);
        last_commit_.assign(cfg.n, 0);
        leader_len_.assign(cfg.n, 0);
        was_leader_.assign(cfg.n, false);
        for (NodeId i = 0; i < cfg.n; ++i) {
            if (i < initial_logs.size()) {
                for (Term t : initial_logs[i]) disk_[i].log.push_back(Entry{t, 0});
                disk_[i].current_term = disk_[i].log.empty() ? 0 : disk_[i].log.back().term;   // currentTerm >= last log term
            }
            nodes_[i].emplace(i, cfg_, disk_[i], seed_, 0);
        }
    }

    void step() {
        ++now_;
        while (!q_.empty() && q_.top().at <= now_) {
            const Env e = q_.top();
            q_.pop();
            deliver(e);
        }
        for (NodeId i = 0; i < cfg_.n; ++i) {
            if (!nodes_[i]) continue;
            const Role before = nodes_[i]->role();
            out_.clear();
            nodes_[i]->tick(now_, out_);
            after(i, before);
        }
        if (now_ % 16 == 0) check_global();
    }

    void crash(NodeId i) { nodes_[i].reset(); }
    void restart(NodeId i) {
        if (nodes_[i]) return;
        nodes_[i].emplace(i, cfg_, disk_[i], seed_ + now_, now_);
        pv_[i] = 0;                                          // force a re-read of the node's persistent state
        last_term_[i] = nodes_[i]->term();
        last_commit_[i] = 0;                                 // commit index is volatile
    }
    bool alive(NodeId i) const { return nodes_[i].has_value(); }
    void set_groups(const std::vector<uint8_t>& g) { group_ = g; }
    void heal() { group_.assign(cfg_.n, 0); }
    void set_net(const NetConfig& n) { net_ = n; }

    // Client command to node i (accepted only if it is a leader). Returns the log index or 0.
    uint64_t propose(NodeId i, uint64_t cmd) {
        if (!nodes_[i]) return 0;
        const Role before = nodes_[i]->role();
        out_.clear();
        in_flight_ = cmd;                                    // a single-node leader commits it inside this very call
        const uint64_t idx = nodes_[i]->propose(cmd, now_, out_);
        after(i, before);
        in_flight_ = 0;
        return idx;
    }
    // Propose the next fresh command to a random node that believes it is leader. Returns false if there is none.
    bool propose_any() {
        std::vector<NodeId> leaders;
        for (NodeId i = 0; i < cfg_.n; ++i) if (nodes_[i] && nodes_[i]->role() == Role::Leader) leaders.push_back(i);
        if (leaders.empty()) return false;
        const NodeId l = leaders[next_rand() % leaders.size()];
        if (!propose(l, next_cmd_)) return false;
        ++next_cmd_;
        return true;
    }
    // Same, but to a specific node (e.g. a stale leader). Returns the log index or 0.
    uint64_t propose_to(NodeId i) {
        const uint64_t idx = propose(i, next_cmd_);
        if (idx) ++next_cmd_;
        return idx;
    }
    uint64_t next_cmd() const noexcept { return next_cmd_; }
    uint64_t proposed() const noexcept { return next_cmd_ - 1; }

    void check_now() { check_global(); }

    uint64_t now() const noexcept { return now_; }
    uint64_t rand() { return next_rand(); }
    const Node& node(NodeId i) const { return *nodes_[i]; }
    const std::vector<Entry>& disk_log(NodeId i) const { return disk_[i].log; }
    const std::string& violation() const noexcept { return violation_; }
    bool ok() const noexcept { return violation_.empty(); }
    uint64_t digest() const noexcept { return digest_; }
    bool ever_leader(NodeId i) const { return was_leader_[i]; }
    uint64_t delivered() const noexcept { return delivered_; }
    uint64_t committed_size() const noexcept { return committed_.size(); }
    const std::vector<Entry>& committed() const noexcept { return committed_; }
    uint64_t truncations() const noexcept { return truncations_; }

    // One leader, and a majority of live nodes are its followers in the same term.
    bool stable(NodeId* leader_out = nullptr) const {
        NodeId leader = kNone;
        for (NodeId i = 0; i < cfg_.n; ++i)
            if (nodes_[i] && nodes_[i]->role() == Role::Leader) {
                if (leader != kNone && nodes_[i]->term() == nodes_[leader]->term()) return false;
                if (leader == kNone || nodes_[i]->term() > nodes_[leader]->term()) leader = i;
            }
        if (leader == kNone) return false;
        uint32_t followers = 1;
        for (NodeId i = 0; i < cfg_.n; ++i)
            if (i != leader && nodes_[i] && nodes_[i]->role() == Role::Follower && nodes_[i]->term() == nodes_[leader]->term() &&
                nodes_[i]->leader() == leader) ++followers;
        if (followers < cfg_.n / 2 + 1) return false;
        if (leader_out) *leader_out = leader;
        return true;
    }

private:
    struct Env {
        uint64_t at, seq;
        Message m;
        bool operator>(const Env& o) const { return at != o.at ? at > o.at : seq > o.seq; }
    };

    void deliver(const Env& e) {
        const Message& m = e.m;
        if (!nodes_[m.to] || group_[m.from] != group_[m.to]) { mix(0xDEAD); return; }   // crashed target or partition
        ++delivered_;
        mix(e.at); mix(static_cast<uint64_t>(m.type)); mix(m.from); mix(m.to); mix(m.term); mix(m.ok); mix(m.entries.size());
        const Role before = nodes_[m.to]->role();
        out_.clear();
        nodes_[m.to]->on_message(m, now_, out_);
        after(m.to, before);
    }

    // Called after every node action: persist, check invariants, put outgoing messages on the network.
    void after(NodeId i, Role before) {
        const Node& n = *nodes_[i];
        if (n.persist_version() != pv_[i]) {                 // durable before messages leave
            const auto& fresh = n.persistent().log;
            const auto& old = disk_[i].log;
            bool cut = fresh.size() < old.size();            // statistic: did an existing entry get removed or replaced?
            for (size_t k = 0; !cut && k < old.size(); ++k) cut = !(old[k] == fresh[k]);
            truncations_ += cut;
            disk_[i] = n.persistent();
            pv_[i] = n.persist_version();
        }
        const Persistent& P = disk_[i];
        const Term t = n.term();
        if (t < last_term_[i]) fail("node " + std::to_string(i) + " term went backwards");
        last_term_[i] = t;
        const NodeId v = P.voted_for;
        if (v != kNone) {
            auto& row = ledger_.try_emplace(t, std::vector<NodeId>(cfg_.n, kNone)).first->second;
            if (row[i] == kNone) row[i] = v;
            else if (row[i] != v) fail("node " + std::to_string(i) + " voted for " + std::to_string(row[i]) + " and then " + std::to_string(v) + " in term " + std::to_string(t));
        }
        const uint64_t len = P.log.size();

        if (n.role() == Role::Leader && before != Role::Leader) {
            was_leader_[i] = true;
            mix(0xBEEF); mix(i); mix(t);
            auto it = leader_of_.find(t);
            if (it != leader_of_.end() && it->second != i) fail("two leaders in term " + std::to_string(t) + ": " + std::to_string(it->second) + " and " + std::to_string(i));
            leader_of_.emplace(t, i);
            uint32_t votes = 0;
            const auto row = ledger_.find(t);
            if (row != ledger_.end()) for (NodeId x : row->second) votes += (x == i);
            if (votes < cfg_.n / 2 + 1) fail("node " + std::to_string(i) + " became leader of term " + std::to_string(t) + " with only " + std::to_string(votes) + " votes");
            // Leader Completeness: everything committed so far must already be in the new leader's log.
            for (uint64_t k = 0; k < committed_.size(); ++k)
                if (k >= len || !(P.log[k] == committed_[k])) { fail("leader " + std::to_string(i) + " of term " + std::to_string(t) + " is missing committed entry " + std::to_string(k + 1)); break; }
            leader_len_[i] = len;
        } else if (n.role() == Role::Leader && before == Role::Leader) {
            if (len < leader_len_[i]) fail("leader " + std::to_string(i) + " shrank its log");
            leader_len_[i] = len;
        }

        // Commit index checks and State Machine Safety.
        const uint64_t c = n.commit_index();
        if (c < last_commit_[i]) fail("node " + std::to_string(i) + " commit index went backwards");
        if (c > len) {
            fail("node " + std::to_string(i) + " commit index " + std::to_string(c) + " is beyond its log (" + std::to_string(len) + ")");
        } else {
            for (uint64_t idx = last_commit_[i] + 1; idx <= c; ++idx) {
                const Entry& e = P.log[idx - 1];
                if (idx <= committed_.size()) {
                    if (!(committed_[idx - 1] == e)) fail("State Machine Safety: node " + std::to_string(i) + " committed a different entry at index " + std::to_string(idx));
                } else {                                     // idx == committed_.size() + 1
                    committed_.push_back(e);
                    mix(0xC0); mix(idx); mix(e.term); mix(e.cmd);
                    if (e.cmd != 0) {
                        if (e.cmd >= next_cmd_ && e.cmd != in_flight_) fail("command " + std::to_string(e.cmd) + " was committed but never proposed");
                        if (!committed_cmds_.insert(e.cmd).second) fail("command " + std::to_string(e.cmd) + " committed twice");
                    }
                }
            }
        }
        if (c > last_commit_[i]) last_commit_[i] = c;

        for (const Message& m : out_) send(m);
    }

    // Cross-node checks, run every 16 ms and on demand. They look at what is on disk, i.e. at every node, up or down.
    void check_global() {
        for (NodeId a = 0; a < cfg_.n && ok(); ++a)
            for (NodeId b = a + 1; b < cfg_.n && ok(); ++b) {
                const auto& x = disk_[a].log;
                const auto& y = disk_[b].log;
                for (uint64_t k = std::min(x.size(), y.size()); k > 0; --k)
                    if (x[k - 1].term == y[k - 1].term) {    // Log Matching: from here down the logs must be identical
                        for (uint64_t j = 0; j < k; ++j)
                            if (!(x[j] == y[j])) { fail("Log Matching: nodes " + std::to_string(a) + " and " + std::to_string(b) + " agree at index " + std::to_string(k) + " but differ at " + std::to_string(j + 1)); break; }
                        break;
                    }
            }
        for (uint64_t k = 0; k < committed_.size() && ok(); ++k) {   // Commit durability
            uint32_t have = 0;
            for (NodeId i = 0; i < cfg_.n; ++i) have += disk_[i].log.size() > k && disk_[i].log[k] == committed_[k];
            if (have < cfg_.n / 2 + 1) fail("committed entry " + std::to_string(k + 1) + " is on only " + std::to_string(have) + " of " + std::to_string(cfg_.n) + " disks");
        }
    }

    void send(const Message& m) {
        if (next_rand() % 100 < net_.drop_pct) { mix(0xD0D0); return; }
        const int copies = (next_rand() % 100 < net_.dup_pct) ? 2 : 1;
        for (int c = 0; c < copies; ++c) {
            const uint32_t span = net_.delay_max - net_.delay_min + 1;
            q_.push({now_ + net_.delay_min + static_cast<uint32_t>(next_rand() % span), ++seq_, m});
        }
    }

    void fail(const std::string& why) { if (violation_.empty()) violation_ = "t=" + std::to_string(now_) + "ms seed=" + std::to_string(seed_) + ": " + why; }
    void mix(uint64_t x) { digest_ ^= x; digest_ *= 0x9E3779B97F4A7C15ull; digest_ ^= digest_ >> 32; }
    uint64_t next_rand() {
        rng_ += 0x9E3779B97F4A7C15ull;
        uint64_t z = rng_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    Config cfg_;
    NetConfig net_;
    uint64_t seed_, rng_, now_ = 0, seq_ = 0, delivered_ = 0, digest_ = 0xCBF29CE484222325ull;
    uint64_t next_cmd_ = 1, in_flight_ = 0, truncations_ = 0;
    std::vector<std::optional<Node>> nodes_;
    std::vector<Persistent> disk_;
    std::vector<uint64_t> pv_, last_commit_, leader_len_;
    std::vector<uint8_t> group_;
    std::vector<Term> last_term_;
    std::vector<bool> was_leader_;
    std::priority_queue<Env, std::vector<Env>, std::greater<Env>> q_;
    std::vector<Message> out_;
    std::map<Term, std::vector<NodeId>> ledger_;             // term -> who each node voted for (simulator's own record)
    std::map<Term, NodeId> leader_of_;
    std::vector<Entry> committed_;                           // the simulator's own record of what was committed, by index
    std::set<uint64_t> committed_cmds_;
    std::string violation_;
};

}  // namespace meridian::raft
