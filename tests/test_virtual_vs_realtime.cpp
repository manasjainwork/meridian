// Virtual-clock sequencer against the real-time sequencer (design M5 gate): three producer threads run against the real sequencer; the
// order it applied them in is then fed to the virtual sequencer (one virtual microsecond apart, no delay). Both must produce the same
// replies, OMS digest and fills digest. This shows the two share the apply path, not that real-time ordering is reproducible (it is not:
// which producer wins a race changes from run to run). A feed with two entries swapped must differ, so the comparison can fail.
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
#include "meridian/sequencer.hpp"
#include "meridian/virtual_sequencer.hpp"

using namespace meridian;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static oms::Config cfg() { oms::Config c; c.max_orders = 1u << 18; return c; }
constexpr size_t P = 3, N = 64;
constexpr size_t kNoSwap = ~size_t{0};

struct Out { std::vector<seq::Reply> replies; uint64_t digest, fills; };

static Out feed(const std::vector<seq::Applied>& a, size_t swap_at) {
    vseq::VirtualSequencer v(cfg());
    std::vector<size_t> idx(a.size());
    for (size_t i = 0; i < a.size(); ++i) idx[i] = i;
    if (swap_at != kNoSwap && swap_at + 1 < a.size()) std::swap(idx[swap_at], idx[swap_at + 1]);
    for (size_t i = 0; i < idx.size(); ++i) v.send(a[idx[i]].producer, 1000 * (i + 1), 0, a[idx[i]].msg);
    v.run_all();
    Out o;
    for (const auto& x : v.applied()) o.replies.push_back(x.reply);
    o.digest = v.oms().digest(); o.fills = v.fills_digest();
    return o;
}

int main() {
    for (int run = 0; run < 3; ++run) {
        seq::Sequencer<P, N> sq(cfg(), 30000);
        std::thread st([&] { sq.run(); });
        std::vector<std::thread> ps;
        for (size_t p = 0; p < P; ++p)
            ps.emplace_back([&, p] {
                std::mt19937_64 g(100 + run * 10 + p);
                uint64_t sent = 0, got = 0;
                seq::Reply r;
                for (uint64_t i = 0; i < 10000; ++i) {
                    seq::Msg m;
                    m.tag = i + 1;
                    if (i > 20 && g() % 6 == 0) { m.op = seq::Op::Cancel; m.account = static_cast<uint32_t>(p + 1); m.order_id = 1 + g() % (i * 3); }
                    else {
                        m.op = seq::Op::Submit;
                        m.req = oms::Request{static_cast<uint32_t>(p + 1), i + 1, static_cast<book::Side>(g() & 1), oms::Type::Limit, book::TimeInForce::GTC,
                                             static_cast<uint32_t>(480 + g() % 40), static_cast<uint32_t>(1 + g() % 50), 0};
                    }
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

        const Out o = feed(sq.applied(), kNoSwap);
        bool same = o.replies.size() == sq.applied().size();
        for (size_t i = 0; same && i < o.replies.size(); ++i) same = o.replies[i] == sq.applied()[i].reply;
        CHECK(same);
        CHECK(o.digest == sq.oms().digest() && o.fills == sq.fills_digest());

        // Swap two neighbouring entries from different producers somewhere in the middle: the state must differ.
        size_t at = 5000;
        while (at + 1 < sq.applied().size() && sq.applied()[at].producer == sq.applied()[at + 1].producer) ++at;
        const Out w = feed(sq.applied(), at);
        bool differs = w.digest != o.digest || w.fills != o.fills;
        for (size_t i = 0; !differs && i < w.replies.size(); ++i) differs = !(w.replies[i] == o.replies[i]);
        CHECK(differs);
    }
    std::printf("%s\n", g_fail ? "FAILED" : "virtual_vs_realtime ok");
    return g_fail ? 1 : 0;
}
