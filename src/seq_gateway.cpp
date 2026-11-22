// Standalone sequencer server (design M3): the sequencer on one thread, the binary TCP gateway (protocol v2) on loopback.
// Usage: seq_gateway [port, default 9100] [log file, default none]. Runs until SIGINT or SIGTERM, then stops the gateway, drains the
// sequencer, saves the event log if a file was given, and replays the log to check it reproduces the live checksums.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "meridian/event_log.hpp"
#include "meridian/sequencer.hpp"
#include "meridian/tcp_gateway.hpp"

using namespace meridian;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9100;
    const char* log_path = argc > 2 ? argv[2] : nullptr;
    oms::Config cfg;
    cfg.max_orders = 1u << 20;
    constexpr size_t P = 8, N = 1024;
    seq::Sequencer<P, N> sq(cfg, 1u << 20);
    gw::TcpGateway<P, N> gateway(sq);

    std::thread st([&] { sq.run(); });
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
