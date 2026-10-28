// Meridian web view, part 1: engine thread + tiny HTTP server (localhost only, no dependencies).
//   web [--port P] [--seed N] [--page web/index.html]
//   GET /       -> the page file
//   GET /data   -> JSON snapshot: stats of the last ~100 ms window, 25 price levels, last trades
// The engine thread publishes a snapshot every 100 ms under a mutex. The hot loop itself never takes the lock
// and never touches a socket. Latency is service time inside the engine call (rdtsc), same as `live`.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "meridian/order_book.hpp"
#include "meridian/timing.hpp"

using namespace meridian;
using namespace meridian::book;

namespace {

struct Hist {
    static constexpr int kGroups = 44, kSub = 16;
    uint64_t c[kGroups * kSub] = {};
    uint64_t n = 0, max = 0;
    void add(uint64_t ns) {
        ++n; if (ns > max) max = ns;
        const uint64_t v = ns + 1;
        const int g = 63 - __builtin_clzll(v);
        const int sub = g >= 4 ? (int)((v >> (g - 4)) & 15) : (int)((v << (4 - g)) & 15);
        c[std::min(g, kGroups - 1) * kSub + sub]++;
    }
    uint64_t pct(double p) const {
        if (!n) return 0;
        const uint64_t want = (uint64_t)((double)n * p / 100.0);
        uint64_t seen = 0;
        for (int i = 0; i < kGroups * kSub; ++i) {
            seen += c[i];
            if (seen > want) {
                const int g = i / kSub, sub = i % kSub;
                return (g >= 4 ? ((16ull + sub) << (g - 4)) : ((16ull + sub) >> (4 - g))) - 1;
            }
        }
        return max;
    }
    void clear() { std::memset(c, 0, sizeof c); n = 0; max = 0; }
};

struct Trade { uint64_t seq; uint32_t price, qty; bool buyer_aggr; };

std::mutex g_mu;
std::string g_json = "{}";
std::atomic<bool> g_stop{false};

void engine(uint64_t seed) {
    const double ghz = calibrate_tsc_ghz();
    uint64_t overhead;
    {
        std::vector<uint64_t> d(20001);
        for (auto& x : d) { const uint64_t a = rdtsc_begin(); const uint64_t b = rdtsc_end(); x = b - a; }
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        overhead = d[d.size() / 2];
    }
    auto to_ns = [&](uint64_t cy) { return (uint64_t)((double)(cy > overhead ? cy - overhead : 0) / ghz); };

    OrderBook book(1u << 20, 1000);
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> live; live.reserve(1u << 20);
    std::vector<Fill> fills; fills.reserve(256);
    std::vector<Trade> recent;                        // last 20 trades
    Hist win;
    uint64_t id = 0, ops = 0, trades = 0, volume = 0, w_ops = 0, w_trades = 0, tseq = 0;
    uint32_t mid = 500;
    using clk = std::chrono::steady_clock;
    auto t_pub = clk::now();
    const auto t_start = t_pub;
    while (!g_stop.load(std::memory_order_relaxed)) {
        for (int b = 0; b < 1024; ++b) {
            fills.clear();
            if ((rng() & 127) == 0) mid = std::clamp<int>((int)mid + ((rng() & 1) ? 1 : -1), 450, 550);
            const unsigned roll = (unsigned)(rng() % 100);
            uint64_t t0, t1;
            Side aggr = Side::Buy;
            if (live.size() > 150000 || (!live.empty() && roll < 22)) {
                const size_t k = rng() % live.size();
                const uint64_t v = live[k]; live[k] = live.back(); live.pop_back();
                t0 = rdtsc_begin(); (void)book.cancel(v); t1 = rdtsc_end();
            } else {
                aggr = (rng() & 1) ? Side::Buy : Side::Sell;
                const uint32_t price = mid - 5 + (uint32_t)(rng() % 11);
                const uint32_t qty = 1 + (uint32_t)(rng() % 100);
                const uint64_t oid = ++id;
                t0 = rdtsc_begin();
                ExecReport rep = book.submit(oid, aggr, TimeInForce::GTC, price, qty, fills);
                t1 = rdtsc_end();
                if (rep.status == AddResult::Ok && rep.rested) live.push_back(oid);
            }
            win.add(to_ns(t1 - t0));
            ++ops; ++w_ops;
            for (const Fill& f : fills) {
                ++trades; ++w_trades; volume += f.qty;
                recent.push_back({++tseq, f.price, f.qty, aggr == Side::Buy});
                if (recent.size() > 20) recent.erase(recent.begin());
            }
        }
        const auto now = clk::now();
        if (now - t_pub >= std::chrono::milliseconds(100)) {
            const double dt = std::chrono::duration<double>(now - t_pub).count();
            std::ostringstream o;
            o << "{\"t\":" << std::chrono::duration<double>(now - t_start).count()
              << ",\"ops_s\":" << (uint64_t)((double)w_ops / dt) << ",\"trades_s\":" << (uint64_t)((double)w_trades / dt)
              << ",\"p50\":" << win.pct(50) << ",\"p99\":" << win.pct(99) << ",\"p999\":" << win.pct(99.9) << ",\"max\":" << win.max
              << ",\"ops\":" << ops << ",\"trades\":" << trades << ",\"volume\":" << volume << ",\"resting\":" << book.size()
              << ",\"bid\":" << book.best_bid() << ",\"ask\":" << book.best_ask() << ",\"levels\":[";
            const uint32_t bb = book.best_bid(), ba = book.best_ask();
            const uint32_t ctr = (bb && ba) ? (bb + ba) / 2 : mid;      // centre the view on the book, not on the generator
            const uint32_t lo = ctr > 12 ? ctr - 12 : 1;
            bool first = true;
            for (uint32_t p = ctr + 12; p >= lo; --p) {            // [price, bid qty, ask qty]
                o << (first ? "" : ",") << "[" << p << "," << book.qty_at(Side::Buy, p) << "," << book.qty_at(Side::Sell, p) << "]";
                first = false;
            }
            o << "],\"recent\":[";
            for (size_t i = 0; i < recent.size(); ++i)
                o << (i ? "," : "") << "[" << recent[i].seq << "," << recent[i].price << "," << recent[i].qty << "," << (recent[i].buyer_aggr ? 1 : 0) << "]";
            o << "]}";
            { std::lock_guard<std::mutex> lk(g_mu); g_json = o.str(); }
            win.clear(); w_ops = 0; w_trades = 0; t_pub = now;
        }
    }
}

std::string read_file(const std::string& p) { std::ifstream f(p, std::ios::binary); std::ostringstream s; s << f.rdbuf(); return s.str(); }

void reply(int fd, const char* status, const char* type, const std::string& body) {
    std::string h = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    (void)!write(fd, h.data(), h.size());
    (void)!write(fd, body.data(), body.size());
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8080; uint64_t seed = 1; std::string page = "web/index.html";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--page") && i + 1 < argc) page = argv[++i];
        else { std::fprintf(stderr, "usage: web [--port P] [--seed N] [--page FILE]\n"); return 2; }
    }
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, [](int) { g_stop = true; });
    const int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 16) != 0) { std::perror("bind/listen"); return 1; }
    std::thread eng(engine, seed);
    std::printf("Meridian web view: open http://localhost:%d  (Ctrl-C to stop)\n", port);
    std::fflush(stdout);
    timeval tv{0, 200000}; setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (!g_stop.load()) {
        const int fd = accept(ls, nullptr, nullptr);
        if (fd < 0) continue;
        timeval rt{1, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
        char buf[2048]; const ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            if (!std::strncmp(buf, "GET /data ", 10)) { std::string j; { std::lock_guard<std::mutex> lk(g_mu); j = g_json; } reply(fd, "200 OK", "application/json", j); }
            else if (!std::strncmp(buf, "GET / ", 6)) {
                const std::string html = read_file(page);
                if (html.empty()) reply(fd, "404 Not Found", "text/plain", "page file not found: " + page + " (run from the repo root or pass --page)");
                else reply(fd, "200 OK", "text/html; charset=utf-8", html);
            } else reply(fd, "404 Not Found", "text/plain", "not found");
        }
        close(fd);
    }
    g_stop = true; eng.join(); close(ls);
    return 0;
}
