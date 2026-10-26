// Open-loop pipeline with the journal replicated to a follower over loopback TCP.
//   mode 0: no replication (baseline)
//   mode 1: async: records are streamed to the follower, acks are NOT held back
//   mode 2: gated: an ack is released only after the follower has acknowledged that record
// Latency = intended send time -> ack released at egress. Every replicated run must end with the follower
// having received all records and its replica book digest equal to the single-thread reference digest.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>
#include "meridian/journal.hpp"
#include "meridian/order_book.hpp"
#include "meridian/replay.hpp"
#include "meridian/replication.hpp"
#include "meridian/spsc_queue.hpp"
#include "meridian/timing.hpp"

using namespace meridian;
using namespace meridian::book;
namespace jr = meridian::journal;

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

struct Op { uint64_t id; Side side; uint32_t price, qty; bool cancel; };
struct Msg { Op op; uint64_t t_intended; uint64_t seq; };
struct Done { uint64_t t_intended; uint64_t seq; };

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

static std::vector<Op> make_ops(size_t n) {
    std::vector<Op> ops;
    ops.reserve(n);
    uint64_t next_id = 1;
    std::vector<uint64_t> live;
    for (size_t i = 0; i < n; ++i) {
        if (!live.empty() && rnd() % 100 < 30) {
            const size_t k = rnd() % live.size();
            ops.push_back({live[k], Side::Buy, 0, 0, true});
            live[k] = live.back(); live.pop_back();
        } else {
            const Side s = (rnd() & 1) ? Side::Buy : Side::Sell;
            const uint32_t p = 1000 + static_cast<uint32_t>(rnd() % 41) - 20 + (s == Side::Buy ? -2 : 2);
            ops.push_back({next_id, s, p, 1 + static_cast<uint32_t>(rnd() % 100), false});
            live.push_back(next_id++);
        }
    }
    return ops;
}

static jr::Record to_record(const Op& o, uint64_t seq) {
    return {seq, o.id, o.price, o.qty, o.cancel ? jr::Type::Cancel : jr::Type::Add, o.side, 0, 0};
}

static void apply(OrderBook& book, const Op& o, std::vector<Fill>& fills) {
    fills.clear();
    if (o.cancel) book.cancel(o.id); else book.add_limit(o.id, o.side, o.price, o.qty, fills);
}

static void run(const std::vector<Op>& ops, int mode, double rate, double ghz, const int cpus[5], uint64_t ref_digest) {
    const size_t N = ops.size();
    static SpscQueue<Msg, 4096> q_in;
    static SpscQueue<Done, 4096> q_out;
    static SpscQueue<jr::Record, 65536> q_repl;
    OrderBook book(1u << 17, 2048);
    std::vector<uint64_t> lat(N);
    std::atomic<uint64_t> acked{0};
    uint64_t repl_full_spins = 0;
    const double period = ghz * 1e9 / rate;

    repl::Follower follower(N, 1u << 17, 2048);
    repl::LeaderLink link;
    if (mode > 0) {
        const uint16_t port = follower.listen_loopback();
        follower.start(cpus[4]);
        if (port == 0 || !link.connect_loopback(port)) { std::printf("replication setup failed\n"); std::exit(1); }
    }
    const uint64_t t0 = __rdtsc() + static_cast<uint64_t>(ghz * 1e8);

    std::thread replicator;
    if (mode > 0) replicator = std::thread([&] {
        pin(cpus[3]);
        jr::Record r;
        unsigned char sb[4096];
        size_t sn = 0;
        while (link.acked() < N) {
            while (sn + sizeof(jr::Record) <= sizeof sb && q_repl.try_pop(r)) {
                r.crc = jr::record_crc(r);                    // CRC is computed here, not on the engine thread
                std::memcpy(sb + sn, &r, sizeof r);
                sn += sizeof r;
            }
            if (sn) { if (!link.send_all(sb, sn)) { std::printf("send failed\n"); std::exit(1); } sn = 0; }
            acked.store(link.poll_acks(), std::memory_order_release);
        }
        link.shutdown_write();
    });
    std::thread eng([&] {
        pin(cpus[1]);
        std::vector<Fill> fills; fills.reserve(64);
        Msg m;
        for (size_t i = 0; i < N; ++i) {
            while (!q_in.try_pop(m)) {}
            apply(book, m.op, fills);
            if (mode > 0) { const jr::Record rec = to_record(m.op, m.seq); while (!q_repl.try_push(rec)) ++repl_full_spins; }
            const Done d{m.t_intended, m.seq};
            while (!q_out.try_push(d)) {}
        }
    });
    std::thread egr([&] {
        pin(cpus[2]);
        Done d;
        for (size_t i = 0; i < N; ++i) {
            while (!q_out.try_pop(d)) {}
            if (mode == 2) while (acked.load(std::memory_order_acquire) < d.seq) {}
            lat[i] = rdtsc_end() - d.t_intended;
        }
    });
    pin(cpus[0]);
    for (size_t i = 0; i < N; ++i) {
        const uint64_t due = t0 + static_cast<uint64_t>(period * static_cast<double>(i));
        while (__rdtsc() < due) {}
        Msg m{ops[i], due, i + 1};
        while (!q_in.try_push(m)) {}
    }
    eng.join(); egr.join();
    if (replicator.joinable()) replicator.join();

    const char* verdict = "-";
    if (mode > 0) {
        link.wait_acks_until_close();
        const repl::FollowerResult fr = follower.join();
        verdict = (fr.status == repl::Status::Ok && fr.valid == N && fr.digest == ref_digest) ? "replica == reference" : "REPLICA MISMATCH";
    }
    std::vector<uint64_t> d(lat.begin() + 10000, lat.end());
    std::sort(d.begin(), d.end());
    auto pct = [&](double p) { return d[static_cast<size_t>(p * (d.size() - 1))] / ghz; };
    static const char* names[] = {"none ", "async", "gated"};
    std::printf("%s %6.1fM/s  p50 %9.0f  p99 %10.0f  p99.9 %11.0f  max %11.0f ns  repl-full spins %-9llu %s\n", names[mode],
                rate / 1e6, pct(0.5), pct(0.99), pct(0.999), pct(1), static_cast<unsigned long long>(repl_full_spins), verdict);
}

int main(int argc, char** argv) {
    int cpus[5] = {0, 2, 4, 6, 8};
    for (int i = 0; i < 5 && i + 1 < argc; ++i) cpus[i] = std::atoi(argv[i + 1]);
    const double ghz = calibrate_tsc_ghz();
    const std::vector<Op> ops = make_ops(400000);
    uint64_t ref_digest;
    {
        jr::Journal ref(ops.size());
        for (const Op& o : ops) { if (o.cancel) ref.append_cancel(o.id); else ref.append_add(o.id, o.side, o.price, o.qty); }
        OrderBook b(1u << 17, 2048);
        ref_digest = jr::replay(ref.records(), b, 2048);
    }
    std::printf("cpus: ingress %d engine %d egress %d replicator %d follower %d, TSC %.3f GHz, reference digest %016llx\n", cpus[0], cpus[1], cpus[2], cpus[3], cpus[4], ghz,
                static_cast<unsigned long long>(ref_digest));
    for (double rate : {500e3, 1e6, 2e6, 4e6})
        for (int mode = 0; mode < 3; ++mode) run(ops, mode, rate, ghz, cpus, ref_digest);
}
