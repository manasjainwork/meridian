#pragma once
// Virtual-clock sequencer (design M5, decision D4). No threads, no rings, no wall clock: every message carries the virtual time at which
// its agent sent it and a one-way delay, and the sequencer applies messages in the order of their arrival time in virtual time.
// Ties are broken by agent id (lowest first), then by the agent's own message number. One agent's messages never overtake each other
// (a connection is FIFO): an arrival is never earlier than that agent's previous arrival. The order therefore depends only on the
// messages, not on how fast the machine is, which thread pushed first, or in which order different agents' send() calls were made.
// The applied log is the same record format as the real-time sequencer's, with the virtual arrival time in t_ingress and t_apply,
// so a replay reproduces replies, OMS digest and fills digest.
#include <algorithm>
#include <cstdint>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "meridian/event_log.hpp"
#include "meridian/sequencer.hpp"

namespace meridian::vseq {

// Bit flags that deliberately break the ordering rule, so the mutation tests can prove they would notice.
enum Bug : uint32_t { kTieByInsertion = 1, kIgnoreDelay = 2, kOvertake = 4, kHighestAgentFirst = 8 };

class VirtualSequencer {
public:
    explicit VirtualSequencer(const oms::Config& c, uint32_t bugs = 0) : oms_(c), bugs_(bugs) {}

    // The agent sends m at virtual time t_send; it reaches the sequencer after `delay`. Returns false (and counts it) if it would arrive
    // before a message that was already applied: the caller is trying to send into the past.
    bool send(uint32_t agent, uint64_t t_send, uint64_t delay, const seq::Msg& m) {
        uint64_t arrive = (bugs_ & kIgnoreDelay) ? t_send : t_send + delay;
        uint64_t& last = last_arrive_[agent];
        if (!(bugs_ & kOvertake)) arrive = std::max(arrive, last);
        if (applied_count_ > 0 && arrive < now_) { ++late_; return false; }
        last = arrive;
        const uint64_t n = ++sent_[agent];
        const uint64_t ins = ++inserted_;
        const bool by_ins = (bugs_ & kTieByInsertion) != 0;
        const uint32_t a = (bugs_ & kHighestAgentFirst) ? ~agent : agent;
        pending_.emplace(Key{arrive, by_ins ? 0u : a, by_ins ? ins : n}, Item{agent, m});
        return true;
    }

    // Applies every message that arrives at or before t, in order. Returns how many.
    size_t run_until(uint64_t t) {
        size_t n = 0;
        while (!pending_.empty() && std::get<0>(pending_.begin()->first) <= t) {
            const uint64_t arrive = std::get<0>(pending_.begin()->first);
            const Item it = pending_.begin()->second;
            pending_.erase(pending_.begin());
            now_ = arrive;
            const oms::Result r = seq::apply_msg(oms_, it.m);
            fills_digest_ = seq::fills_mix(fills_digest_, r);
            const seq::Reply rep = seq::make_reply(++seq_, it.m.tag, r);
            applied_.push_back({it.agent, it.m, rep});
            times_.push_back(arrive);
            ++applied_count_;
            ++n;
        }
        return n;
    }
    size_t run_all() { return run_until(~uint64_t{0}); }

    const std::vector<seq::Applied>& applied() const noexcept { return applied_; }
    const std::vector<uint64_t>& times() const noexcept { return times_; }
    const oms::Oms& oms() const noexcept { return oms_; }
    uint64_t fills_digest() const noexcept { return fills_digest_; }
    uint64_t now() const noexcept { return now_; }
    size_t pending() const noexcept { return pending_.size(); }
    uint64_t late_rejects() const noexcept { return late_; }

    evlog::EventLog event_log() const {
        evlog::EventLog l(applied_.size());
        for (size_t i = 0; i < applied_.size(); ++i) l.append(applied_[i].msg, applied_[i].producer, times_[i], times_[i]);
        return l;
    }

private:
    using Key = std::tuple<uint64_t, uint32_t, uint64_t>;   // arrival time, agent id, the agent's message number
    struct Item { uint32_t agent; seq::Msg m; };

    oms::Oms oms_;
    uint32_t bugs_;
    std::map<Key, Item> pending_;
    std::unordered_map<uint32_t, uint64_t> last_arrive_, sent_;
    std::vector<seq::Applied> applied_;
    std::vector<uint64_t> times_;
    uint64_t now_ = 0, seq_ = 0, inserted_ = 0, late_ = 0, applied_count_ = 0;
    uint64_t fills_digest_ = seq::kFillsDigestInit;
};

}  // namespace meridian::vseq
