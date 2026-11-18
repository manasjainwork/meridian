// Sequencer timestamps (design M4): with timing on there is one Stamp per applied request, in the same order; T2 <= T3 for each,
// and each request is popped only after the one before it finished (T3 of one <= T2 of the next), because one thread applies them.
// With timing off nothing is recorded. Timing must not change what the sequencer does: the live checksums equal a single-thread replay.
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
#include "meridian/event_log.hpp"
#include "meridian/sequencer.hpp"

using namespace meridian;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static oms::Config cfg() { oms::Config c; c.max_orders = 1u << 18; return c; }
constexpr size_t P = 3, N = 64;

static void run(bool timing, uint64_t seed) {
    seq::Sequencer<P, N> sq(cfg(), 30000);
    if (timing) sq.enable_timing(30000);
    std::thread st([&] { sq.run(); });
    std::vector<std::thread> ps;
    for (size_t p = 0; p < P; ++p)
        ps.emplace_back([&, p] {
            std::mt19937_64 g(seed + p);
            uint64_t sent = 0, got = 0;
            seq::Reply r;
            for (uint64_t i = 0; i < 10000; ++i) {
                seq::Msg m;
                m.tag = i + 1; m.op = seq::Op::Submit;
                m.req = oms::Request{static_cast<uint32_t>(p + 1), i + 1, static_cast<book::Side>(g() & 1), oms::Type::Limit, book::TimeInForce::GTC,
                                     static_cast<uint32_t>(480 + g() % 40), static_cast<uint32_t>(1 + g() % 50), 0};
                while (sent - got + 1 >= N) while (sq.pop_reply(p, r)) ++got;
                while (!sq.submit(p, m)) while (sq.pop_reply(p, r)) ++got;
                ++sent;
            }
            while (got < sent) while (sq.pop_reply(p, r)) ++got;
        });
    for (auto& t : ps) t.join();
    sq.request_stop();
    st.join();

    CHECK(sq.applied().size() == P * 10000);
    if (!timing) { CHECK(sq.stamps().empty()); }
    else {
        const auto& s = sq.stamps();
        CHECK(s.size() == sq.applied().size());
        bool ordered = true;
        for (size_t i = 0; i < s.size(); ++i) {
            ordered = ordered && s[i].pop > 0 && s[i].pop <= s[i].apply;
            if (i) ordered = ordered && s[i - 1].apply <= s[i].pop;
        }
        CHECK(ordered);
    }
    const evlog::EventLog log = evlog::EventLog::from_applied(sq.applied());
    oms::Oms fresh(cfg());
    evlog::ReplayOut out;
    CHECK(evlog::replay(log, fresh, &out));
    CHECK(fresh.digest() == sq.oms().digest() && out.fills_digest == sq.fills_digest());
}

int main() {
    for (int i = 0; i < 3; ++i) { run(false, 10 + i); run(true, 20 + i); }
    std::printf("%s\n", g_fail ? "FAILED" : "seq_timing ok");
    return g_fail ? 1 : 0;
}
