#pragma once
// Sequencer (design M1): the one thread that orders every request and applies it to the OMS.
// Producers (gateway threads) each own one SPSC ring into the sequencer and one SPSC ring back.
// The sequencer polls the rings from a rotating start index, assigns seq = 1, 2, 3, ... and applies each
// request to the Oms. Every applied request is kept in applied() so a run can be replayed single-threaded.
// Contract: a producer keeps fewer than N requests without a reply, otherwise the sequencer waits for it to drain
// replies (counted in reply_stalls()). A full request ring is an explicit "busy" for the producer (busy(p)).
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>
#include "meridian/oms.hpp"
#include "meridian/spsc_queue.hpp"

namespace meridian::seq {

enum class Op : uint8_t { Submit, Cancel, Modify };

struct Msg {
    Op op = Op::Submit;
    uint32_t account = 0;
    uint64_t order_id = 0;   // Cancel, Modify
    uint32_t price = 0;      // Modify
    uint32_t qty = 0;        // Modify
    oms::Request req{};      // Submit
    uint64_t tag = 0;        // chosen by the producer, echoed in the reply
};

struct Reply {
    uint64_t seq = 0, tag = 0, order_id = 0;
    oms::Reject reject = oms::Reject::None;
    oms::Status status = oms::Status::Rejected;
    uint32_t filled = 0, rested = 0, cancelled = 0;
    bool duplicate = false;
    bool operator==(const Reply& o) const noexcept {
        return seq == o.seq && tag == o.tag && order_id == o.order_id && reject == o.reject && status == o.status &&
               filled == o.filled && rested == o.rested && cancelled == o.cancelled && duplicate == o.duplicate;
    }
};

struct Applied { uint32_t producer; Msg msg; Reply reply; };

inline Reply make_reply(uint64_t seq, uint64_t tag, const oms::Result& r) {
    Reply x;
    x.seq = seq; x.tag = tag; x.order_id = r.order_id; x.reject = r.reject; x.status = r.status;
    x.filled = r.filled; x.rested = r.rested; x.cancelled = r.cancelled; x.duplicate = r.duplicate;
    return x;
}

inline oms::Result apply_msg(oms::Oms& o, const Msg& m) {
    switch (m.op) {
        case Op::Submit: return o.submit(m.req);
        case Op::Cancel: return o.cancel(m.account, m.order_id);
        case Op::Modify: return o.modify(m.account, m.order_id, m.price, m.qty);
    }
    return {};
}

// Bit flags that deliberately break the sequencer so the mutation tests can prove they would notice (see test_sequencer_mutants).
enum Bug : uint32_t { kDropRequest = 1, kSkipSeq = 2, kNoRotation = 4, kSwallowReply = 8, kStaleReply = 16, kReorder = 32, kApplyTwice = 64 };

template <size_t P, size_t N>
class Sequencer {
public:
    explicit Sequencer(const oms::Config& c, size_t reserve_applied = 0, uint32_t bugs = 0) : oms_(c), bugs_(bugs) { applied_.reserve(reserve_applied); }

    // producer side (producer p only): false = ring full, counted as busy; the request was NOT accepted
    bool submit(size_t p, const Msg& m) noexcept {
        if (in_[p].try_push(m)) return true;
        busy_[p].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bool pop_reply(size_t p, Reply& out) noexcept { return out_[p].try_pop(out); }
    uint64_t busy(size_t p) const noexcept { return busy_[p].load(std::memory_order_relaxed); }

    // sequencer side: run on one thread; returns after request_stop() once every ring has been seen empty
    void run() {
        while (true) {
            const bool stop = stop_.load(std::memory_order_acquire);
            const size_t n = poll_all();
            if (stop && n == 0) return;
        }
    }
    void request_stop() noexcept { stop_.store(true, std::memory_order_release); }

    // one pass over all rings from the rotating start; applies at most one request per ring; returns how many
    size_t poll_all() {
        size_t done = 0;
        const size_t start = rot_;
        for (size_t i = 0; i < P; ++i) {
            const size_t p = (start + i) % P;
            Msg m;
            if (!in_[p].try_pop(m)) continue;
            ++popped_;
            ++done;
            if ((bugs_ & kDropRequest) && popped_ % 97 == 0) continue;
            if ((bugs_ & kReorder) && !has_held_ && popped_ % 61 == 0) { held_ = m; held_p_ = p; has_held_ = true; continue; }
            const bool flush = has_held_ && held_p_ == p;   // release the held request after that producer's next one
            process(p, m);
            if (flush) { has_held_ = false; process(held_p_, held_); }
        }
        rot_ = (bugs_ & kNoRotation) ? start : (start + 1) % P;
        return done;
    }

    // read these only after run() has returned
    const std::vector<Applied>& applied() const noexcept { return applied_; }
    const oms::Oms& oms() const noexcept { return oms_; }
    uint64_t seq() const noexcept { return seq_; }
    uint64_t reply_stalls() const noexcept { return reply_stalls_.load(std::memory_order_relaxed); }

private:
    void process(size_t p, const Msg& m) {
        const oms::Result r = apply_msg(oms_, m);
        if ((bugs_ & kApplyTwice) && popped_ % 53 == 0) apply_msg(oms_, m);
        if ((bugs_ & kSkipSeq) && popped_ % 71 == 0) ++seq_;
        const oms::Result& shown = ((bugs_ & kStaleReply) && popped_ % 67 == 0) ? prev_r_ : r;
        const Reply rep = make_reply(++seq_, m.tag, shown);
        prev_r_ = r;
        applied_.push_back({static_cast<uint32_t>(p), m, rep});
        if ((bugs_ & kSwallowReply) && popped_ % 89 == 0) return;
        while (!out_[p].try_push(rep)) reply_stalls_.fetch_add(1, std::memory_order_relaxed);
    }

    oms::Oms oms_;
    uint32_t bugs_ = 0;
    uint64_t popped_ = 0;
    Msg held_{};
    size_t held_p_ = 0;
    bool has_held_ = false;
    oms::Result prev_r_{};
    std::array<SpscQueue<Msg, N>, P> in_;
    std::array<SpscQueue<Reply, N>, P> out_;
    std::array<std::atomic<uint64_t>, P> busy_{};
    std::vector<Applied> applied_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> reply_stalls_{0};
    uint64_t seq_ = 0;
    size_t rot_ = 0;
};

}  // namespace meridian::seq
