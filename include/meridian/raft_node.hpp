#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace meridian::raft {

using NodeId = uint32_t;
using Term = uint64_t;
constexpr NodeId kNone = 0xFFFFFFFFu;

enum class Role : uint8_t { Follower, Candidate, Leader };
enum class MsgType : uint8_t { RequestVote, VoteReply, AppendEntries, AppendReply };

// One log entry. cmd 0 is reserved for the no-op a new leader appends.
struct Entry {
    Term term;
    uint64_t cmd;
    Entry(Term t = 0, uint64_t c = 0) : term(t), cmd(c) {}
    bool operator==(const Entry& o) const noexcept { return term == o.term && cmd == o.cmd; }
};

// One wire message. Which fields matter depends on `type`:
//   RequestVote:   last_log_index / last_log_term = the candidate's last entry
//   VoteReply:     ok = vote granted
//   AppendEntries: last_log_index / last_log_term = the entry just BEFORE `entries` (prev), leader_commit, entries
//   AppendReply:   ok = accepted; match_index = (ok) last index verified equal to the leader's log
//                                                (!ok) hint: the follower's log length, bounded by prev - 1
struct Message {
    MsgType type;
    NodeId from;
    NodeId to;
    Term term;
    uint64_t last_log_index;
    Term last_log_term;
    bool ok;
    uint64_t leader_commit = 0;
    uint64_t match_index = 0;
    std::vector<Entry> entries;
};

// Deliberate defects, switched on only by tests to prove the simulator would catch them.
enum Bug : uint32_t {
    kNoBug = 0,
    kVoteTwice = 1,                  // grant several votes in one term
    kForgetVoteOnRestart = 2,        // voted_for is not durable
    kSmallQuorum = 4,                // n/2 votes instead of n/2 + 1
    kSkipLogCheck = 8,               // ignore the "candidate's log must be up to date" rule
    kCountDuplicateVotes = 16,       // a duplicated vote message counts twice
    kCommitOldTerm = 32,             // commit entries of earlier terms by counting replicas (the Figure 8 bug)
    kNoPrevCheck = 64,               // append without checking the previous entry matches
    kTruncateOnAppend = 128,         // delete the log suffix on every AppendEntries, even when it already matches
    kCommitSmallQuorum = 256,        // commit with n/2 replicas instead of n/2 + 1
    kFollowerCommitUnbounded = 512,  // follower sets commit = leader_commit, not min(leader_commit, last new entry)
};

struct Config {
    uint32_t n = 3;
    uint32_t election_min_ms = 150;
    uint32_t election_max_ms = 300;
    uint32_t heartbeat_ms = 50;
    uint32_t max_batch = 64;         // entries per AppendEntries
    bool noop = true;                // a new leader appends a no-op in its term (the paper's variant has none; used to build Figure 8)
    uint32_t bugs = kNoBug;
};

// State that must survive a crash (on a real node: written and fsynced BEFORE any reply is sent).
struct Persistent {
    Term current_term = 0;
    NodeId voted_for = kNone;
    std::vector<Entry> log;          // log[i] is entry i+1
};

// A Raft node as a pure state machine: no clock, no sockets, no threads. Time comes in through tick()/on_message()/
// propose(), messages go out through the `out` vector. That is what makes it testable under a deterministic simulator.
class Node {
public:
    Node(NodeId id, const Config& cfg, Persistent p, uint64_t seed, uint64_t now_ms)
        : id_(id), cfg_(cfg), p_(std::move(p)), rng_(seed * 0x9E3779B97F4A7C15ull + id + 1) {
        if (cfg_.bugs & kForgetVoteOnRestart) p_.voted_for = kNone;
        deadline_ = now_ms + random_timeout();
    }

    void tick(uint64_t now, std::vector<Message>& out) {
        if (role_ == Role::Leader) {
            if (now >= next_hb_) broadcast(now, out);
        } else if (now >= deadline_) {
            start_election(now, out);
        }
    }

    // Client command. Returns the log index it was appended at, or 0 if this node is not the leader.
    uint64_t propose(uint64_t cmd, uint64_t now, std::vector<Message>& out) {
        if (role_ != Role::Leader) return 0;
        p_.log.push_back(Entry{p_.current_term, cmd});
        ++pv_;
        advance_commit();
        broadcast(now, out);
        return p_.log.size();
    }

    void on_message(const Message& m, uint64_t now, std::vector<Message>& out) {
        if (m.term > p_.current_term) {                      // a newer term always wins
            p_.current_term = m.term;
            p_.voted_for = kNone;
            ++pv_;
            role_ = Role::Follower;
            leader_ = kNone;
        }
        switch (m.type) {
        case MsgType::RequestVote: {
            bool grant = false;
            if (m.term == p_.current_term) {
                const bool free_vote = p_.voted_for == kNone || p_.voted_for == m.from || (cfg_.bugs & kVoteTwice);
                const bool log_ok = (cfg_.bugs & kSkipLogCheck) || candidate_log_up_to_date(m);
                if (free_vote && log_ok) {
                    grant = true;
                    p_.voted_for = m.from;
                    ++pv_;
                    deadline_ = now + random_timeout();
                }
            }
            out.push_back({MsgType::VoteReply, id_, m.from, p_.current_term, 0, 0, grant});
            break;
        }
        case MsgType::VoteReply:
            if (role_ == Role::Candidate && m.term == p_.current_term && m.ok) {
                if (cfg_.bugs & kCountDuplicateVotes) {
                    ++votes_;
                } else if (!(vote_mask_ & (1ull << m.from))) {
                    vote_mask_ |= 1ull << m.from;
                    ++votes_;
                }
                if (votes_ >= quorum()) become_leader(now, out);
            }
            break;
        case MsgType::AppendEntries:
            on_append(m, now, out);
            break;
        case MsgType::AppendReply:
            on_append_reply(m, now, out);
            break;
        }
    }

    NodeId id() const noexcept { return id_; }
    Role role() const noexcept { return role_; }
    Term term() const noexcept { return p_.current_term; }
    NodeId leader() const noexcept { return leader_; }
    uint64_t commit_index() const noexcept { return commit_; }
    const Persistent& persistent() const noexcept { return p_; }
    uint64_t persist_version() const noexcept { return pv_; }   // changes whenever persistent() changes

private:
    uint32_t quorum() const noexcept { return (cfg_.bugs & kSmallQuorum) ? cfg_.n / 2 : cfg_.n / 2 + 1; }
    uint32_t commit_quorum() const noexcept { return (cfg_.bugs & kCommitSmallQuorum) ? cfg_.n / 2 : cfg_.n / 2 + 1; }
    Term term_at(uint64_t index) const noexcept { return index == 0 ? 0 : p_.log[index - 1].term; }

    bool candidate_log_up_to_date(const Message& m) const noexcept {
        const Term my_last_term = term_at(p_.log.size());
        const uint64_t my_last_index = p_.log.size();
        return m.last_log_term > my_last_term || (m.last_log_term == my_last_term && m.last_log_index >= my_last_index);
    }

    void start_election(uint64_t now, std::vector<Message>& out) {
        ++p_.current_term;
        p_.voted_for = id_;
        ++pv_;
        role_ = Role::Candidate;
        leader_ = kNone;
        vote_mask_ = 1ull << id_;
        votes_ = 1;
        deadline_ = now + random_timeout();
        const Term last_term = term_at(p_.log.size());
        for (NodeId peer = 0; peer < cfg_.n; ++peer)
            if (peer != id_) out.push_back({MsgType::RequestVote, id_, peer, p_.current_term, p_.log.size(), last_term, false});
        if (votes_ >= quorum()) become_leader(now, out);
    }

    void become_leader(uint64_t now, std::vector<Message>& out) {
        role_ = Role::Leader;
        leader_ = id_;
        // A no-op in the new term: entries of earlier terms can only be committed together with a current-term entry.
        if (cfg_.noop) { p_.log.push_back(Entry{p_.current_term, 0}); ++pv_; }
        next_.assign(cfg_.n, cfg_.noop ? p_.log.size() : p_.log.size() + 1);   // optimistic: send only what is new
        match_.assign(cfg_.n, 0);
        advance_commit();
        broadcast(now, out);
    }

    void broadcast(uint64_t now, std::vector<Message>& out) {
        next_hb_ = now + cfg_.heartbeat_ms;
        for (NodeId peer = 0; peer < cfg_.n; ++peer)
            if (peer != id_) send_append(peer, out);
    }

    void send_append(NodeId peer, std::vector<Message>& out) {
        const uint64_t prev = next_[peer] - 1;
        Message m{MsgType::AppendEntries, id_, peer, p_.current_term, prev, term_at(prev), false};
        m.leader_commit = commit_;
        const uint64_t last = std::min<uint64_t>(p_.log.size(), prev + cfg_.max_batch);
        for (uint64_t i = prev + 1; i <= last; ++i) m.entries.push_back(p_.log[i - 1]);
        out.push_back(std::move(m));
    }

    void on_append(const Message& m, uint64_t now, std::vector<Message>& out) {
        Message r{MsgType::AppendReply, id_, m.from, p_.current_term, 0, 0, false};
        if (m.term < p_.current_term) { out.push_back(std::move(r)); return; }
        if (role_ == Role::Leader) return;                   // cannot happen: one leader per term
        role_ = Role::Follower;
        leader_ = m.from;
        deadline_ = now + random_timeout();
        const uint64_t prev = m.last_log_index;
        const bool match = (cfg_.bugs & kNoPrevCheck) || prev == 0 || (prev <= p_.log.size() && term_at(prev) == m.last_log_term);
        if (!match) {
            r.match_index = std::min<uint64_t>(p_.log.size(), prev - 1);
            out.push_back(std::move(r));
            return;
        }
        uint64_t idx = prev;
        for (const Entry& e : m.entries) {
            ++idx;
            if (idx <= p_.log.size()) {
                const bool conflict = p_.log[idx - 1].term != e.term;
                if (!conflict && !(cfg_.bugs & kTruncateOnAppend)) continue;   // already have it: never delete on a match
                p_.log.resize(idx - 1);
            }
            p_.log.push_back(e);
            ++pv_;
        }
        const uint64_t last_new = prev + m.entries.size();
        const uint64_t c = (cfg_.bugs & kFollowerCommitUnbounded) ? m.leader_commit : std::min(m.leader_commit, last_new);
        if (c > commit_) commit_ = c;
        r.ok = true;
        r.match_index = last_new;
        out.push_back(std::move(r));
    }

    void on_append_reply(const Message& m, uint64_t, std::vector<Message>& out) {
        if (role_ != Role::Leader || m.term != p_.current_term) return;
        if (m.ok) {
            match_[m.from] = std::max(match_[m.from], m.match_index);
            next_[m.from] = std::max(next_[m.from], match_[m.from] + 1);
            advance_commit();
            if (next_[m.from] <= p_.log.size()) send_append(m.from, out);       // more to send: do not wait for the heartbeat
        } else {
            uint64_t nx = std::min<uint64_t>(next_[m.from] - 1, m.match_index + 1);
            nx = std::max<uint64_t>({nx, match_[m.from] + 1, 1});
            next_[m.from] = nx;
            send_append(m.from, out);
        }
    }

    // Highest N replicated on a majority whose entry is from the CURRENT term (unless the bug is on).
    void advance_commit() {
        match_[id_] = p_.log.size();
        for (uint64_t n = p_.log.size(); n > commit_; --n) {
            if (!(cfg_.bugs & kCommitOldTerm) && p_.log[n - 1].term != p_.current_term) continue;
            uint32_t count = 0;
            for (NodeId i = 0; i < cfg_.n; ++i) count += match_[i] >= n;
            if (count >= commit_quorum()) { commit_ = n; break; }
        }
    }

    uint32_t random_timeout() {
        const uint32_t span = cfg_.election_max_ms - cfg_.election_min_ms;
        return cfg_.election_min_ms + static_cast<uint32_t>(next_rand() % (span ? span : 1));
    }
    uint64_t next_rand() {                                   // splitmix64
        rng_ += 0x9E3779B97F4A7C15ull;
        uint64_t z = rng_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    NodeId id_;
    Config cfg_;
    Persistent p_;
    uint64_t rng_;
    Role role_ = Role::Follower;
    NodeId leader_ = kNone;
    uint64_t deadline_ = 0, next_hb_ = 0;
    uint64_t vote_mask_ = 0;
    uint32_t votes_ = 0;
    uint64_t commit_ = 0;                                    // volatile: rebuilt after a restart
    uint64_t pv_ = 1;
    std::vector<uint64_t> next_, match_;                     // leader only
};

}  // namespace meridian::raft
