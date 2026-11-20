// Virtual-clock sequencer (design M5): the order of applied messages is fixed by (virtual arrival time, agent id, agent message number)
// and by nothing else. Checks: a hand-made case for every rule; the same messages pushed in different cross-agent orders give a
// byte-identical event log; replay reproduces the live checksums; sending into the past is refused; and four deliberately broken
// versions of the ordering rule are each caught.
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "meridian/virtual_sequencer.hpp"

using namespace meridian;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static oms::Config cfg() { oms::Config c; c.max_orders = 1u << 16; c.max_position = int64_t{1} << 40; return c; }

static seq::Msg sub(uint64_t tag, uint32_t account, bool buy, uint32_t price, uint32_t qty) {
    seq::Msg m;
    m.op = seq::Op::Submit; m.tag = tag;
    m.req = oms::Request{account, tag, buy ? book::Side::Buy : book::Side::Sell, oms::Type::Limit, book::TimeInForce::GTC, price, qty, 0};
    return m;
}

// The hand-made case. Returns the tags in the order they were applied.
static std::vector<uint64_t> hand_made(uint32_t bugs) {
    vseq::VirtualSequencer v(cfg(), bugs);
    v.send(2, 100, 50, sub(1, 2, true, 500, 1));    // arrives 150
    v.send(1, 100, 50, sub(2, 1, true, 500, 1));    // arrives 150, tie with agent 2: agent 1 first
    v.send(3, 120, 10, sub(3, 3, true, 500, 1));    // arrives 130, before everything: a smaller delay wins
    v.send(3, 125, 0, sub(4, 3, true, 500, 1));     // would arrive 125, but may not overtake tag 3: arrives 130, after it
    v.send(1, 100, 50, sub(5, 1, true, 500, 1));    // arrives 150, same agent as tag 2: after it
    v.run_all();
    std::vector<uint64_t> tags;
    for (const auto& a : v.applied()) tags.push_back(a.msg.tag);
    return tags;
}

struct Plan { uint32_t agent; uint64_t t, d; seq::Msg m; };

static std::vector<std::vector<Plan>> make_plans(size_t agents, size_t per_agent, uint64_t seed) {
    std::mt19937_64 g(seed);
    std::vector<std::vector<Plan>> plans(agents);
    uint64_t tag = 0;
    for (size_t a = 0; a < agents; ++a) {
        uint64_t t = 0;
        for (size_t i = 0; i < per_agent; ++i) {
            t += 10 * (g() % 3);                                        // many equal send times
            const uint64_t d = 10 * (g() % 4);                          // many equal arrival times
            seq::Msg m;
            if (g() % 5 == 0) { m.op = seq::Op::Cancel; m.tag = ++tag; m.account = static_cast<uint32_t>(a + 1); m.order_id = 1 + g() % 4000; }
            else m = sub(++tag, static_cast<uint32_t>(a + 1), g() & 1, static_cast<uint32_t>(490 + g() % 21), static_cast<uint32_t>(1 + g() % 30));
            plans[a].push_back({static_cast<uint32_t>(a + 1), t, d, m});
        }
    }
    return plans;
}

// Pushes the plans in an order that keeps each agent's own messages in order but mixes the agents in a way chosen by `mix_seed`.
static std::vector<uint8_t> run_plans(const std::vector<std::vector<Plan>>& plans, uint64_t mix_seed, uint32_t bugs, uint64_t* digest, uint64_t* fdigest, size_t* ties) {
    vseq::VirtualSequencer v(cfg(), bugs);
    std::mt19937_64 g(mix_seed);
    std::vector<size_t> next(plans.size(), 0);
    size_t left = 0;
    for (auto& p : plans) left += p.size();
    while (left) {
        size_t a = g() % plans.size();
        while (next[a] == plans[a].size()) a = (a + 1) % plans.size();
        const Plan& p = plans[a][next[a]++];
        v.send(p.agent, p.t, p.d, p.m);
        --left;
    }
    v.run_all();
    if (ties) { *ties = 0; for (size_t i = 1; i < v.times().size(); ++i) if (v.times()[i] == v.times()[i - 1]) ++*ties; }
    if (digest) *digest = v.oms().digest();
    if (fdigest) *fdigest = v.fills_digest();
    return v.event_log().to_bytes();
}

// True if the sequencer behaves as the rule says, for the hand-made case and for four different push orders.
static bool behaves(uint32_t bugs) {
    if (hand_made(bugs) != std::vector<uint64_t>{3, 4, 2, 5, 1}) return false;
    const auto plans = make_plans(6, 1500, 42);
    const std::vector<uint8_t> ref = run_plans(plans, 1, 0, nullptr, nullptr, nullptr);
    for (uint64_t mix : {1ull, 2ull, 3ull, 4ull})
        if (run_plans(plans, mix, bugs, nullptr, nullptr, nullptr) != ref) return false;
    return true;
}

int main() {
    CHECK(hand_made(0) == (std::vector<uint64_t>{3, 4, 2, 5, 1}));

    // Same messages, several cross-agent push orders: identical bytes, and the run really contains ties.
    const auto plans = make_plans(6, 1500, 42);
    uint64_t dg0 = 0, fg0 = 0; size_t ties = 0;
    const std::vector<uint8_t> ref = run_plans(plans, 1, 0, &dg0, &fg0, &ties);
    CHECK(ties > 1000);
    for (uint64_t mix : {1ull, 2ull, 3ull, 4ull, 5ull, 6ull}) {
        uint64_t dg = 0, fg = 0;
        CHECK(run_plans(plans, mix, 0, &dg, &fg, nullptr) == ref);
        CHECK(dg == dg0 && fg == fg0);
    }

    // The log replays to the same state.
    {
        vseq::VirtualSequencer v(cfg());
        for (auto& pl : plans) for (const Plan& p : pl) v.send(p.agent, p.t, p.d, p.m);
        v.run_all();
        const evlog::EventLog log = v.event_log();
        oms::Oms fresh(cfg());
        evlog::ReplayOut out;
        CHECK(evlog::replay(log, fresh, &out));
        CHECK(fresh.digest() == v.oms().digest() && out.fills_digest == v.fills_digest() && log.size() == 6 * 1500);
        bool replies_equal = out.replies.size() == v.applied().size();
        for (size_t i = 0; replies_equal && i < out.replies.size(); ++i) replies_equal = out.replies[i] == v.applied()[i].reply;
        CHECK(replies_equal);
        bool times_ok = true;
        for (size_t i = 0; i < v.times().size(); ++i) times_ok = times_ok && log.records()[i].t_ingress == v.times()[i] && (i == 0 || v.times()[i] >= v.times()[i - 1]);
        CHECK(times_ok);
        std::string why; CHECK(v.oms().check(&why));
    }

    // Sending into the past is refused and changes nothing.
    {
        vseq::VirtualSequencer v(cfg());
        CHECK(v.send(1, 100, 50, sub(1, 1, true, 500, 1)));
        CHECK(v.run_until(149) == 0 && v.pending() == 1);
        CHECK(v.run_until(150) == 1 && v.now() == 150);
        CHECK(!v.send(2, 100, 10, sub(2, 2, true, 500, 1)));
        CHECK(v.late_rejects() == 1 && v.pending() == 0 && v.applied().size() == 1);
        CHECK(v.send(2, 100, 50, sub(3, 2, true, 500, 1)));
    }

    // Each broken ordering rule is caught.
    CHECK(behaves(0));
    CHECK(!behaves(vseq::kTieByInsertion));
    CHECK(!behaves(vseq::kIgnoreDelay));
    CHECK(!behaves(vseq::kOvertake));
    CHECK(!behaves(vseq::kHighestAgentFirst));

    std::printf("%s\n", g_fail ? "FAILED" : "virtual_sequencer ok");
    return g_fail ? 1 : 0;
}
