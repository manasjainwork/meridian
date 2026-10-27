#pragma once
#include <cstdint>
#include <vector>

namespace meridian::raft {

using NodeId = uint32_t;
using Term = uint64_t;
constexpr NodeId kNone = 0xFFFFFFFFu;

enum class Role : uint8_t { Follower, Candidate, Leader };
enum class MsgType : uint8_t { RequestVote, VoteReply, Heartbeat, HeartbeatReply };

// One wire message. Which fields matter depends on `type`:
//   RequestVote: last_log_index / last_log_term describe the candidate's log
//   VoteReply, HeartbeatReply: ok = vote granted / heartbeat accepted
struct Message {
    MsgType type;
    NodeId from;
    NodeId to;
    Term term;
    uint64_t last_log_index;
    Term last_log_term;
    bool ok;
};

// Deliberate defects, switched on only by tests to prove the simulator would catch them.
enum Bug : uint32_t {
    kNoBug = 0,
    kVoteTwice = 1,              // grant several votes in one term
    kForgetVoteOnRestart = 2,    // voted_for is not durable
    kSmallQuorum = 4,            // n/2 votes instead of n/2 + 1
    kSkipLogCheck = 8,           // ignore the "candidate's log must be up to date" rule
    kCountDuplicateVotes = 16,   // a duplicated vote message counts twice
};

struct Config {
    uint32_t n = 3;
    uint32_t election_min_ms = 150;
    uint32_t election_max_ms = 300;
    uint32_t heartbeat_ms = 50;
    uint32_t bugs = kNoBug;
};

// State that must survive a crash (on a real node: written and fsynced BEFORE any reply is sent).
// log[i] is the term of entry i+1. Only the terms matter for elections.
struct Persistent {
    Term current_term = 0;
    NodeId voted_for = kNone;
    std::vector<Term> log;
};

// A Raft node as a pure state machine: no clock, no sockets, no threads. Time comes in through tick()/on_message(),
// messages go out through the `out` vector. That is what makes it testable under a deterministic simulator.
class Node {
public:
    Node(NodeId id, const Config& cfg, Persistent p, uint64_t seed, uint64_t now_ms)
        : id_(id), cfg_(cfg), p_(std::move(p)), rng_(seed * 0x9E3779B97F4A7C15ull + id + 1) {
        if (cfg_.bugs & kForgetVoteOnRestart) p_.voted_for = kNone;
        deadline_ = now_ms + random_timeout();
    }

    void tick(uint64_t now, std::vector<Message>& out) {
        if (role_ == Role::Leader) {
            if (now >= next_hb_) send_heartbeats(now, out);
        } else if (now >= deadline_) {
            start_election(now, out);
        }
    }

    void on_message(const Message& m, uint64_t now, std::vector<Message>& out) {
        if (m.term > p_.current_term) {                      // a newer term always wins
            p_.current_term = m.term;
            p_.voted_for = kNone;
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
        case MsgType::Heartbeat:
            if (m.term < p_.current_term) {
                out.push_back({MsgType::HeartbeatReply, id_, m.from, p_.current_term, 0, 0, false});
            } else {                                         // same term: m.from is the legitimate leader
                role_ = Role::Follower;
                leader_ = m.from;
                deadline_ = now + random_timeout();
                out.push_back({MsgType::HeartbeatReply, id_, m.from, p_.current_term, 0, 0, true});
            }
            break;
        case MsgType::HeartbeatReply:
            break;
        }
    }

    NodeId id() const noexcept { return id_; }
    Role role() const noexcept { return role_; }
    Term term() const noexcept { return p_.current_term; }
    NodeId leader() const noexcept { return leader_; }
    const Persistent& persistent() const noexcept { return p_; }

private:
    uint32_t quorum() const noexcept { return (cfg_.bugs & kSmallQuorum) ? cfg_.n / 2 : cfg_.n / 2 + 1; }

    bool candidate_log_up_to_date(const Message& m) const noexcept {
        const Term my_last_term = p_.log.empty() ? 0 : p_.log.back();
        const uint64_t my_last_index = p_.log.size();
        return m.last_log_term > my_last_term || (m.last_log_term == my_last_term && m.last_log_index >= my_last_index);
    }

    void start_election(uint64_t now, std::vector<Message>& out) {
        ++p_.current_term;
        p_.voted_for = id_;
        role_ = Role::Candidate;
        leader_ = kNone;
        vote_mask_ = 1ull << id_;
        votes_ = 1;
        deadline_ = now + random_timeout();
        const Term last_term = p_.log.empty() ? 0 : p_.log.back();
        for (NodeId peer = 0; peer < cfg_.n; ++peer)
            if (peer != id_) out.push_back({MsgType::RequestVote, id_, peer, p_.current_term, p_.log.size(), last_term, false});
        if (votes_ >= quorum()) become_leader(now, out);
    }

    void become_leader(uint64_t now, std::vector<Message>& out) {
        role_ = Role::Leader;
        leader_ = id_;
        send_heartbeats(now, out);
    }

    void send_heartbeats(uint64_t now, std::vector<Message>& out) {
        next_hb_ = now + cfg_.heartbeat_ms;
        for (NodeId peer = 0; peer < cfg_.n; ++peer)
            if (peer != id_) out.push_back({MsgType::Heartbeat, id_, peer, p_.current_term, 0, 0, false});
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
};

}  // namespace meridian::raft
