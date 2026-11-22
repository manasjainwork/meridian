// Delay models for the virtual-clock sequencer (design M5): draws are a pure function of the seed, stay in range, follow the measured
// samples, and per-agent generators make an episode independent of the order in which agents' messages are pushed. A shared generator
// (the mistake this design avoids) is shown to break that independence.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
#include "meridian/delay_model.hpp"
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

// One episode: `agents` agents, each sending `n` orders at times that are multiples of 10, delays drawn from the model.
// order 0 pushes agent 1 fully, then agent 2, ...; order 1 pushes them round-robin. shared_rng = true is the mistake to detect.
static std::vector<uint8_t> episode(uint64_t seed, const vseq::DelayModel& dm, int order, bool shared_rng, size_t agents = 5, size_t n = 800) {
    vseq::VirtualSequencer v(cfg());
    std::vector<std::mt19937_64> rng;
    for (size_t a = 0; a < agents; ++a) rng.push_back(vseq::agent_rng(seed, static_cast<uint32_t>(a + 1)));
    std::mt19937_64 shared(seed);
    std::vector<uint64_t> t(agents, 0);
    auto one = [&](size_t a, size_t i) {
        std::mt19937_64& g = rng[a];
        t[a] += 10 * (g() % 3);
        const uint64_t d = dm.draw(shared_rng ? shared : g);
        const uint64_t tag = a * 100000 + i + 1;
        v.send(static_cast<uint32_t>(a + 1), t[a], d, sub(tag, static_cast<uint32_t>(a + 1), g() & 1, static_cast<uint32_t>(495 + g() % 11), static_cast<uint32_t>(1 + g() % 20)));
    };
    if (order == 0) { for (size_t a = 0; a < agents; ++a) for (size_t i = 0; i < n; ++i) one(a, i); }
    else { for (size_t i = 0; i < n; ++i) for (size_t a = 0; a < agents; ++a) one(a, i); }
    v.run_all();
    return v.event_log().to_bytes();
}

int main() {
    // Ranges and samples.
    {
        std::mt19937_64 g(7);
        const auto f = vseq::DelayModel::fixed(42);
        const auto u = vseq::DelayModel::uniform(100, 130);
        const auto e = vseq::DelayModel::empirical({5, 900, 17});
        bool ok = true; uint64_t umin = ~0ull, umax = 0; bool seen[3] = {false, false, false};
        for (int i = 0; i < 20000; ++i) {
            ok = ok && f.draw(g) == 42;
            const uint64_t x = u.draw(g); umin = std::min(umin, x); umax = std::max(umax, x);
            const uint64_t y = e.draw(g);
            ok = ok && (y == 5 || y == 900 || y == 17);
            seen[y == 5 ? 0 : y == 900 ? 1 : 2] = true;
        }
        CHECK(ok && umin == 100 && umax == 130 && seen[0] && seen[1] && seen[2]);
        CHECK(vseq::DelayModel::empirical({}).draw(g) == 0);
        CHECK(vseq::DelayModel::uniform(9, 3).draw(g) == 9);
    }

    // A generator is a pure function of (seed, agent); a different agent or seed gives a different stream.
    {
        auto a1 = vseq::agent_rng(1, 1), a2 = vseq::agent_rng(1, 1), b = vseq::agent_rng(1, 2), c = vseq::agent_rng(2, 1);
        CHECK(a1() == a2() && a1() == a2());
        CHECK(vseq::agent_rng(1, 1)() != b() && vseq::agent_rng(1, 1)() != c());
    }

    // Same seed gives the same bytes on repeated runs and for both push orders; another seed gives other bytes.
    const auto dm = vseq::DelayModel::uniform(0, 30);
    const auto ref = episode(11, dm, 0, false);
    for (int i = 0; i < 3; ++i) { CHECK(episode(11, dm, 0, false) == ref); CHECK(episode(11, dm, 1, false) == ref); }
    CHECK(episode(12, dm, 0, false) != ref);
    // Empirical delay model also runs, is order-independent, and differs from the uniform one.
    const auto emp = vseq::DelayModel::empirical({0, 10, 10, 20, 400});
    CHECK(episode(11, emp, 0, false) == episode(11, emp, 1, false));
    CHECK(episode(11, emp, 0, false) != ref);
    // The mistake: one generator shared by all agents makes the result depend on the push order. The test must see that.
    CHECK(episode(11, dm, 0, true) != episode(11, dm, 1, true));

    std::printf("%s\n", g_fail ? "FAILED" : "delay_model ok");
    return g_fail ? 1 : 0;
}
