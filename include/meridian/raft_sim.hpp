#pragma once
#include <map>
#include <optional>
#include <queue>
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
// duplication, reordering, crashes, partitions) is driven by one seed. A failing seed replays exactly.
// Safety is checked after every event with checks that do NOT trust the nodes' own bookkeeping:
//   - a node never votes for two candidates in one term (even across a crash)
//   - at most one leader per term
//   - a leader had votes from a real majority, according to the simulator's own ledger of granted votes
//   - a node's term never goes down
class Sim {
public:
    Sim(const Config& cfg, uint64_t seed, NetConfig net = {}, std::vector<std::vector<Term>> initial_logs = {})
        : cfg_(cfg), net_(net), seed_(seed), rng_(seed ^ 0xD1B54A32D192ED03ull) {
        nodes_.resize(cfg.n);
        disk_.resize(cfg.n);
        group_.assign(cfg.n, 0);
        last_term_.assign(cfg.n, 0);
        was_leader_.assign(cfg.n, false);
        for (NodeId i = 0; i < cfg.n; ++i) {
            if (i < initial_logs.size()) {
                disk_[i].log = initial_logs[i];
                disk_[i].current_term = disk_[i].log.empty() ? 0 : disk_[i].log.back();   // currentTerm >= last log term
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
    }

    void crash(NodeId i) { nodes_[i].reset(); }
    void restart(NodeId i) {
        if (nodes_[i]) return;
        nodes_[i].emplace(i, cfg_, disk_[i], seed_ + now_, now_);
        last_term_[i] = nodes_[i]->term();
    }
    bool alive(NodeId i) const { return nodes_[i].has_value(); }
    void set_groups(const std::vector<uint8_t>& g) { group_ = g; }
    void heal() { group_.assign(cfg_.n, 0); }
    void set_net(const NetConfig& n) { net_ = n; }

    uint64_t now() const noexcept { return now_; }
    uint64_t rand() { return next_rand(); }
    const Node& node(NodeId i) const { return *nodes_[i]; }
    const std::string& violation() const noexcept { return violation_; }
    bool ok() const noexcept { return violation_.empty(); }
    uint64_t digest() const noexcept { return digest_; }
    bool ever_leader(NodeId i) const { return was_leader_[i]; }
    uint64_t delivered() const noexcept { return delivered_; }

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
        mix(e.at); mix(static_cast<uint64_t>(m.type)); mix(m.from); mix(m.to); mix(m.term); mix(m.ok);
        const Role before = nodes_[m.to]->role();
        out_.clear();
        nodes_[m.to]->on_message(m, now_, out_);
        after(m.to, before);
    }

    // Called after every node action: persist, check invariants, put outgoing messages on the network.
    void after(NodeId i, Role before) {
        const Node& n = *nodes_[i];
        disk_[i] = n.persistent();                           // the node's state is durable before its messages leave
        const Term t = n.term();
        if (t < last_term_[i]) fail("node " + std::to_string(i) + " term went backwards");
        last_term_[i] = t;
        const NodeId v = n.persistent().voted_for;
        if (v != kNone) {
            auto& row = ledger_.try_emplace(t, std::vector<NodeId>(cfg_.n, kNone)).first->second;
            if (row[i] == kNone) row[i] = v;
            else if (row[i] != v) fail("node " + std::to_string(i) + " voted for " + std::to_string(row[i]) + " and then " + std::to_string(v) + " in term " + std::to_string(t));
        }
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
        }
        for (const Message& m : out_) send(m);
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
    std::vector<std::optional<Node>> nodes_;
    std::vector<Persistent> disk_;
    std::vector<uint8_t> group_;
    std::vector<Term> last_term_;
    std::vector<bool> was_leader_;
    std::priority_queue<Env, std::vector<Env>, std::greater<Env>> q_;
    std::vector<Message> out_;
    std::map<Term, std::vector<NodeId>> ledger_;             // term -> who each node voted for (simulator's own record)
    std::map<Term, NodeId> leader_of_;
    std::string violation_;
};

}  // namespace meridian::raft
