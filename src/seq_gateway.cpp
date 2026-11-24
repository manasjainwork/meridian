// Standalone sequencer server (design M3): the sequencer on one thread, the binary TCP gateway (protocol v2) on loopback.
// Usage: seq_gateway [port, default 9100] [log file or -, default none] [cpu for the sequencer thread, default none] [cpus for the gateway threads, e.g. 6,8,10, default any] [records to reserve for the log, default 1048576]. Runs until SIGINT or SIGTERM, then stops the gateway, drains the
// sequencer, saves the event log if a file was given, and replays the log to check it reproduces the live checksums.
#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "meridian/event_log.hpp"
#include "meridian/sequencer.hpp"
#include "meridian/tcp_gateway.hpp"

using namespace meridian;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9100;
    const char* log_path = argc > 2 && argv[2][0] != '-' ? argv[2] : nullptr;
    const int cpu = argc > 3 ? std::atoi(argv[3]) : -1;
    oms::Config cfg;
    cfg.max_orders = 1u << 20;
    constexpr size_t P = 8, N = 1024;
    const size_t reserve = argc > 5 ? static_cast<size_t>(std::strtoull(argv[5], nullptr, 10)) : (size_t{1} << 20);
    seq::Sequencer<P, N> sq(cfg, reserve);
    gw::TcpGateway<P, N> gateway(sq);

    std::thread st([&] {
        if (cpu >= 0) { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(cpu, &cs); pthread_setaffinity_np(pthread_self(), sizeof cs, &cs); }
        sq.run();
    });
    if (argc > 4) {   // threads the gateway creates inherit this thread's cpu set
        cpu_set_t gs; CPU_ZERO(&gs);
        for (char* t = std::strtok(argv[4], ","); t; t = std::strtok(nullptr, ",")) CPU_SET(std::atoi(t), &gs);
        pthread_setaffinity_np(pthread_self(), sizeof gs, &gs);
    }
    const uint16_t bound = gateway.start(port);
    if (bound == 0) {
        std::fprintf(stderr, "cannot listen on port %u\n", port);
        sq.request_stop(); st.join();
        return 1;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::printf("seq_gateway listening on 127.0.0.1:%u (up to %zu connections)\n", bound, P);
    std::fflush(stdout);
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    gateway.stop();
    sq.request_stop();
    st.join();
    const gw::Stats s = gateway.stats();
    std::printf("connections %llu (refused %llu), requests %llu, replies %llu, heartbeats %llu, bad messages %llu, applied %zu\n",
                (unsigned long long)s.accepted, (unsigned long long)s.refused, (unsigned long long)s.requests, (unsigned long long)s.replies,
                (unsigned long long)s.heartbeats, (unsigned long long)s.bad_messages, sq.applied().size());

    const evlog::EventLog log = evlog::EventLog::from_applied(sq.applied());
    oms::Oms fresh(cfg);
    evlog::ReplayOut out;
    const bool replay_ok = evlog::replay(log, fresh, &out) && fresh.digest() == sq.oms().digest() && out.fills_digest == sq.fills_digest();
    std::printf("replay of the event log reproduces the live checksums: %s\n", replay_ok ? "yes" : "NO");
    if (log_path) std::printf("event log (%zu records) %s %s\n", log.size(), log.save(log_path) ? "saved to" : "COULD NOT BE SAVED to", log_path);
    return replay_ok ? 0 : 2;
}
