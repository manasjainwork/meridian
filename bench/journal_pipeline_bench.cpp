// Same open-loop pipeline as pipeline_bench, plus a journal writer thread fed by the engine.
//   mode 0: no journal (baseline)
//   mode 1: journal async: engine hands each input to a writer thread (CRC, write(), fdatasync every 10 ms);
//           acks are NOT held back
//   mode 2: journal gated: same writer, but an ack is released only after its record has been fdatasync'd
// Latency = intended send time -> egress releases the ack. After every journaled run the file is loaded,
// checked complete, and replayed; its digest must equal the digest of a single-thread reference run.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>
#include "meridian/journal.hpp"
#include "meridian/journal_writer.hpp"
#include "meridian/order_book.hpp"
#include "meridian/replay.hpp"
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

static void run(const std::vector<Op>& ops, int mode, double rate, double ghz, const int cpus[4], const char* path, uint64_t ref_digest) {
    const size_t N = ops.size();
    static SpscQueue<Msg, 4096> q_in;
    static SpscQueue<Done, 4096> q_out;
    static SpscQueue<jr::Record, 65536> q_jrn;
    OrderBook book(1u << 17, 2048);
    std::vector<uint64_t> lat(N);
    std::atomic<uint64_t> durable{0};
    uint64_t jrn_full_spins = 0;
    const double period = ghz * 1e9 / rate;
    const uint64_t t0 = __rdtsc() + static_cast<uint64_t>(ghz * 1e8);
    jr::FileWriter* fw = nullptr;
    if (mode > 0) fw = new jr::FileWriter(path);
    if (fw && !fw->ok()) { std::printf("cannot open %s\n", path); std::exit(1); }

    std::thread writer;
    if (mode > 0) writer = std::thread([&] {
        pin(cpus[3]);
        jr::Record r;
        uint64_t n = 0, pending = 0;
        auto last = std::chrono::steady_clock::now();
        while (n < N) {
            if (q_jrn.try_pop(r)) {
                fw->append(r);
                ++n; ++pending;
                if (mode == 1) {
                    if ((n & 1023) == 0) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - last >= std::chrono::milliseconds(10)) { fw->sync(); last = now; pending = 0; durable.store(n, std::memory_order_release); }
                    }
                }
            } else if (mode == 2 && pending > 0) {         // group commit: sync whatever has accumulated
                fw->sync();
                durable.store(n, std::memory_order_release);
                pending = 0;
            }
        }
        fw->sync();
        durable.store(N, std::memory_order_release);
    });
    std::thread eng([&] {
        pin(cpus[1]);
        std::vector<Fill> fills; fills.reserve(64);
        Msg m;
        for (size_t i = 0; i < N; ++i) {
            while (!q_in.try_pop(m)) {}
            apply(book, m.op, fills);
            if (mode > 0) { const jr::Record rec = to_record(m.op, m.seq); while (!q_jrn.try_push(rec)) ++jrn_full_spins; }
            const Done d{m.t_intended, m.seq};
            while (!q_out.try_push(d)) {}
        }
    });
    std::thread egr([&] {
        pin(cpus[2]);
        Done d;
        for (size_t i = 0; i < N; ++i) {
            while (!q_out.try_pop(d)) {}
            if (mode == 2) while (durable.load(std::memory_order_acquire) < d.seq) {}
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
    if (writer.joinable()) writer.join();
    delete fw;

    const char* verdict = "-";
    if (mode > 0) {
        jr::Journal loaded;
        const jr::LoadResult lr = jr::Journal::load(path, loaded);
        OrderBook b(1u << 17, 2048);
        const bool ok = lr.status == jr::LoadStatus::Ok && lr.valid == N && jr::replay(loaded.records(), b, 2048) == ref_digest;
        verdict = ok ? "file replays == reference" : "JOURNAL MISMATCH";
    }
    std::vector<uint64_t> d(lat.begin() + 10000, lat.end());
    std::sort(d.begin(), d.end());
    auto pct = [&](double p) { return d[static_cast<size_t>(p * (d.size() - 1))] / ghz; };
    static const char* names[] = {"none ", "async", "gated"};
    std::printf("%s %6.1fM/s  p50 %9.0f  p99 %10.0f  p99.9 %11.0f  max %11.0f ns  journal-full spins %-9llu %s\n", names[mode],
                rate / 1e6, pct(0.5), pct(0.99), pct(0.999), pct(1), static_cast<unsigned long long>(jrn_full_spins), verdict);
}

int main(int argc, char** argv) {
    int cpus[4] = {0, 2, 4, 6};
    for (int i = 0; i < 4 && i + 1 < argc; ++i) cpus[i] = std::atoi(argv[i + 1]);
    const char* path = argc > 5 ? argv[5] : "/tmp/meridian_pipe.journal";
    const double ghz = calibrate_tsc_ghz();
    const std::vector<Op> ops = make_ops(400000);
    uint64_t ref_digest;
    {
        jr::Journal ref(ops.size());
        for (const Op& o : ops) { if (o.cancel) ref.append_cancel(o.id); else ref.append_add(o.id, o.side, o.price, o.qty); }
        OrderBook b(1u << 17, 2048);
        ref_digest = jr::replay(ref.records(), b, 2048);
    }
    std::printf("cpus: ingress %d engine %d egress %d writer %d, file %s, TSC %.3f GHz, reference digest %016llx\n", cpus[0], cpus[1], cpus[2], cpus[3], path, ghz,
                static_cast<unsigned long long>(ref_digest));
    for (double rate : {500e3, 2e6, 4e6, 8e6})
        for (int mode = 0; mode < 3; ++mode) run(ops, mode, rate, ghz, cpus, path, ref_digest);
}
