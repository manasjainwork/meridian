// Sequencer latency, open loop (design M4). Producers send at a fixed schedule whatever the sequencer is doing; every latency is
// measured from the time the request was due, not from when it was actually sent, so a stalled sequencer shows up as delay
// (no coordinated omission). Four timestamps per request: due (T1, producer core), popped from the ring (T2, sequencer core),
// apply finished (T3, sequencer core), reply read (T4, producer core). Intervals: queue = T2 - T1, engine = T3 - T2 (one core,
// exact), ack = T4 - T3, total = T4 - T1 (one core, exact). Queue and ack cross two cores, so they carry the TSC offset
// uncertainty found by tsc_check; a negative value is clamped to 0 and counted. After the run the log is replayed single-threaded
// and the OMS and fills checksums must equal the live ones.
// usage: seq_latency [rate per producer per second] [requests per producer] [producers 1 or 2]
#include <pthread.h>
#include <sched.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <thread>
#include <vector>
#include "meridian/event_log.hpp"
#include "meridian/hdr_histogram.hpp"
#include "meridian/sequencer.hpp"
#include "meridian/timing.hpp"

using namespace meridian;

constexpr size_t kMaxProducers = 2;
constexpr size_t kRing = 1024;

static void pin(int cpu) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); pthread_setaffinity_np(pthread_self(), sizeof s, &s); }
static oms::Config cfg() { oms::Config c; c.max_orders = 1u << 20; return c; }

struct Prod {
    std::vector<uint64_t> due, ack, seq;
    uint64_t busy_spins = 0, flow_waits = 0;
};

static void producer(seq::Sequencer<kMaxProducers, kRing>& sq, size_t p, int core, uint64_t ops, double period_ticks,
                     const std::atomic<uint64_t>& go, Prod& out) {
    pin(core);
    out.due.assign(ops, 0); out.ack.assign(ops, 0); out.seq.assign(ops, 0);
    std::mt19937_64 g(100 + p);
    std::vector<uint64_t> open;
    uint64_t sent = 0, got = 0, cl = 0;
    auto drain = [&] {
        seq::Reply r;
        while (sq.pop_reply(p, r)) {
            const uint64_t t4 = rdtsc_end();
            const uint64_t i = r.tag - 1;
            out.ack[i] = t4; out.seq[i] = r.seq;
            if (r.status == oms::Status::Open && r.order_id) open.push_back(r.order_id);
            ++got;
        }
    };
    uint64_t start;
    while ((start = go.load(std::memory_order_acquire)) == 0) {}
    for (uint64_t i = 0; i < ops; ++i) {
        const uint64_t due = start + static_cast<uint64_t>(static_cast<double>(i) * period_ticks);
        while (rdtsc_begin() < due) drain();
        while (sent - got + 1 >= kRing) { ++out.flow_waits; drain(); }   // contract: fewer than N requests without a reply
        seq::Msg m;
        m.tag = i + 1;
        if (i % 4 == 3 && !open.empty()) {
            m.op = seq::Op::Cancel; m.account = static_cast<uint32_t>(p + 1); m.order_id = open.back(); open.pop_back();
        } else {
            m.op = seq::Op::Submit;
            m.req = oms::Request{static_cast<uint32_t>(p + 1), ++cl, static_cast<book::Side>(g() & 1), oms::Type::Limit,
                                 book::TimeInForce::GTC, static_cast<uint32_t>(480 + g() % 40), static_cast<uint32_t>(1 + g() % 50), 0};
        }
        out.due[i] = due;
        while (!sq.submit(p, m)) { ++out.busy_spins; drain(); }
        ++sent;
    }
    while (got < sent) drain();
}

static void show(const char* name, const Histogram<>& h) {
    std::printf("  %-22s p50 %8llu  p99 %8llu  p99.9 %8llu  max %9llu  (ns)\n", name, (unsigned long long)h.percentile(50),
                (unsigned long long)h.percentile(99), (unsigned long long)h.percentile(99.9), (unsigned long long)h.max());
}

int main(int argc, char** argv) {
    const double rate = argc > 1 ? std::atof(argv[1]) : 100000;
    const uint64_t ops = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 100000;
    const size_t np = std::min<size_t>(argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 2, kMaxProducers);
    const double ghz = calibrate_tsc_ghz(200);
    const double period = ghz * 1e9 / rate;

    uint64_t pair_lo = ~0ull; std::vector<uint64_t> pair;       // cost of one T2/T3 stamp pair, back to back
    for (int i = 0; i < 100000; ++i) { const uint64_t a = rdtsc_begin(); const uint64_t b = rdtsc_end(); pair.push_back(b - a); pair_lo = std::min(pair_lo, b - a); }
    std::sort(pair.begin(), pair.end());

    seq::Sequencer<kMaxProducers, kRing> sq(cfg(), np * ops);
    sq.enable_timing(np * ops);
    std::thread seq_thread([&] { pin(0); sq.run(); });
    std::atomic<uint64_t> go{0};
    std::vector<Prod> pr(np);
    std::vector<std::thread> ts;
    for (size_t p = 0; p < np; ++p) ts.emplace_back(producer, std::ref(sq), p, static_cast<int>(p + 1), ops, period, std::cref(go), std::ref(pr[p]));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t t0 = rdtsc_begin();
    go.store(t0 + static_cast<uint64_t>(ghz * 1e6), std::memory_order_release);   // common start 1 ms from now
    for (auto& t : ts) t.join();
    sq.request_stop();
    seq_thread.join();

    Histogram<> hq, he, ha, ht;
    uint64_t neg_q = 0, neg_a = 0, last_ack = 0;
    const auto& st = sq.stamps();
    for (auto& r : pr)
        for (uint64_t i = 0; i < ops; ++i) {
            const seq::Stamp& s = st[r.seq[i] - 1];
            const int64_t q = static_cast<int64_t>(s.pop - r.due[i]), e = static_cast<int64_t>(s.apply - s.pop);
            const int64_t a = static_cast<int64_t>(r.ack[i] - s.apply), t = static_cast<int64_t>(r.ack[i] - r.due[i]);
            if (q < 0) ++neg_q;
            if (a < 0) ++neg_a;
            hq.record(q < 0 ? 0 : static_cast<uint64_t>(q / ghz)); he.record(static_cast<uint64_t>(e / ghz));
            ha.record(a < 0 ? 0 : static_cast<uint64_t>(a / ghz)); ht.record(static_cast<uint64_t>(t / ghz));
            last_ack = std::max(last_ack, r.ack[i]);
        }

    const evlog::EventLog log = evlog::EventLog::from_applied(sq.applied());
    oms::Oms fresh(cfg());
    evlog::ReplayOut out;
    const bool replayed = evlog::replay(log, fresh, &out);
    const bool same = replayed && fresh.digest() == sq.oms().digest() && out.fills_digest == sq.fills_digest() && log.size() == np * ops;

    uint64_t busy = 0, flow = 0;
    for (size_t p = 0; p < np; ++p) { busy += pr[p].busy_spins; flow += pr[p].flow_waits; }
    const double secs = static_cast<double>(last_ack - (t0 + static_cast<uint64_t>(ghz * 1e6))) / ghz / 1e9;
    std::printf("seq_latency: %zu producers x %llu requests, offered %.0f/s each (%.0f/s total), TSC %.4f GHz\n", np, (unsigned long long)ops, rate, rate * np, ghz);
    std::printf("  completed %.0f/s total over %.3f s; ring-full retries %llu, flow-control waits %llu, reply stalls %llu\n",
                static_cast<double>(np * ops) / secs, secs, (unsigned long long)busy, (unsigned long long)flow, (unsigned long long)sq.reply_stalls());
    show("queue  (T2 - due)", hq); show("engine (T3 - T2)", he); show("ack    (T4 - T3)", ha); show("total  (T4 - due)", ht);
    std::printf("  stamp pair cost: min %.0f ns, median %.0f ns (a floor and a bias on engine); negative cross-core values clamped: queue %llu, ack %llu\n",
                pair_lo / ghz, pair[pair.size() / 2] / ghz, (unsigned long long)neg_q, (unsigned long long)neg_a);
    std::printf("  checksum: replay %s (OMS digest, fills digest, %zu records)\n", same ? "equal" : "DIFFERENT", log.size());
    return same ? 0 : 1;
}
