// HTTP gateway in front of the OMS and the order book.  127.0.0.1 only.
//   gateway [--port 8081] [--price-decimals 1] [--qty-decimals 5] [--max-price 2000000]
//           [--max-qty 1000000] [--max-position 10000000] [--seed-mid TICKS] [--bug N  (tests only)]
// Prices are integer ticks and quantities integer lots, so nothing is ever rounded. GET /config says what a tick and a lot are.
//   GET  /health
//   GET  /config
//   GET  /book?depth=10
//   GET  /trades
//   GET  /account?account=1
//   POST /orders   {"account":1,"cl_ord_id":42,"side":"buy","type":"limit","tif":"gtc","price":800000,"qty":10000}
//   POST /cancel   {"account":1,"order_id":5}
//   POST /modify   {"account":1,"order_id":5,"price":800010,"qty":5000}
//   GET  /debug/check   runs the full internal consistency check (slow, for tests)
// One thread per connection, one mutex around the OMS (the engine itself is single-threaded by design).
// Limits: 16 KB of headers, 8 KB of body, 5 s to send a request, 64 connections at once, Connection: close.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include "meridian/oms.hpp"

using namespace meridian;
using namespace meridian::oms;

namespace {

std::mutex g_mu;
Oms* g_oms = nullptr;
int g_price_dec = 1, g_qty_dec = 5;
uint32_t g_max_price = 2000000;
struct TradeRec { uint64_t seq; uint32_t price, qty; const char* aggressor; };
std::deque<TradeRec> g_trades;
uint64_t g_trade_seq = 0;
std::atomic<int> g_active{0};
constexpr int kMaxConn = 64;
constexpr size_t kMaxHeader = 16384, kMaxBody = 8192;

// ---- tiny JSON: one flat object whose values are strings or plain tokens (numbers, true, false, null) ----
struct Flat {
    std::map<std::string, std::string> kv;
};

bool parse_flat(const std::string& s, Flat& out) {
    size_t i = 0;
    auto ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; };
    auto str = [&](std::string& r) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') return false;          // no escapes in this API
            r += s[i++];
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    };
    ws();
    if (i >= s.size() || s[i] != '{') return false;
    ++i; ws();
    if (i < s.size() && s[i] == '}') return true;
    while (true) {
        ws();
        std::string k, v;
        if (!str(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        ++i; ws();
        if (i < s.size() && s[i] == '"') { if (!str(v)) return false; }
        else {
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') v += s[i++];
            if (v.empty()) return false;
        }
        if (out.kv.count(k)) return false;           // duplicate key
        out.kv[k] = v;
        ws();
        if (i >= s.size()) return false;
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == '}') { ++i; ws(); return i == s.size(); }
        return false;
    }
}

bool get_u64(const Flat& f, const char* key, uint64_t& out) {
    const auto it = f.kv.find(key);
    if (it == f.kv.end() || it->second.empty() || it->second.size() > 19) return false;
    uint64_t v = 0;
    for (char c : it->second) { if (c < '0' || c > '9') return false; v = v * 10 + static_cast<uint64_t>(c - '0'); }
    out = v;
    return true;
}
bool get_u32(const Flat& f, const char* key, uint32_t& out) {
    uint64_t v;
    if (!get_u64(f, key, v) || v > 0xFFFFFFFFull) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

const char* status_name(Status s) {
    switch (s) { case Status::Open: return "open"; case Status::Filled: return "filled"; case Status::Cancelled: return "cancelled"; default: return "rejected"; }
}
const char* reject_name(Reject r) {
    switch (r) {
        case Reject::None: return "none"; case Reject::BadParam: return "bad_param"; case Reject::QtyLimit: return "qty_limit";
        case Reject::PositionLimit: return "position_limit"; case Reject::SelfTrade: return "self_trade"; case Reject::PoolFull: return "pool_full"; case Reject::UnknownOrder: return "unknown_order";
        case Reject::NotOwner: return "not_owner"; case Reject::NotOpen: return "not_open"; case Reject::KeyReuse: return "key_reuse";
    }
    return "?";
}

struct Reply { int code; std::string body; };

Reply error(int code, const std::string& msg) { return {code, "{\"ok\":false,\"error\":\"" + msg + "\"}"}; }

// Called with g_mu held.
void record_trades(const Result& r) {
    for (const book::Fill& f : r.fills) {
        const OrderRec* tk = g_oms->order(f.taker_id);
        g_trades.push_back({++g_trade_seq, f.price, f.qty, tk && tk->side == Side::Buy ? "buy" : "sell"});
        if (g_trades.size() > 200) g_trades.pop_front();
    }
}

std::string result_json(const Result& r) {
    std::ostringstream o;
    o << "{\"ok\":" << (r.reject == Reject::None ? "true" : "false") << ",\"reject\":\"" << reject_name(r.reject) << "\",\"order_id\":" << r.order_id
      << ",\"status\":\"" << status_name(r.status) << "\",\"filled\":" << r.filled << ",\"rested\":" << r.rested << ",\"cancelled\":" << r.cancelled
      << ",\"duplicate\":" << (r.duplicate ? "true" : "false") << ",\"fills\":[";
    for (size_t i = 0; i < r.fills.size(); ++i)
        o << (i ? "," : "") << "{\"maker\":" << r.fills[i].maker_id << ",\"taker\":" << r.fills[i].taker_id << ",\"price\":" << r.fills[i].price << ",\"qty\":" << r.fills[i].qty << "}";
    o << "]}";
    return o.str();
}

Reply reject_or_ok(const Result& r) { return {r.reject == Reject::None ? 200 : 422, result_json(r)}; }

Reply post_orders(const std::string& body) {
    Flat f;
    if (!parse_flat(body, f)) return error(400, "body is not a flat JSON object");
    Request r{};
    uint64_t cl;
    if (!get_u32(f, "account", r.account) || r.account == 0 || !get_u64(f, "cl_ord_id", cl) || !get_u32(f, "qty", r.qty)) return error(400, "account, cl_ord_id and qty must be non-negative integers (account > 0)");
    r.cl_ord_id = cl;
    const std::string side = f.kv.count("side") ? f.kv["side"] : "", type = f.kv.count("type") ? f.kv["type"] : "limit", tif = f.kv.count("tif") ? f.kv["tif"] : "gtc";
    if (side == "buy") r.side = Side::Buy; else if (side == "sell") r.side = Side::Sell; else return error(400, "side must be buy or sell");
    if (type == "limit") r.type = Type::Limit; else if (type == "market") r.type = Type::Market; else return error(400, "type must be limit or market");
    if (tif == "gtc") r.tif = book::TimeInForce::GTC; else if (tif == "ioc") r.tif = book::TimeInForce::IOC; else if (tif == "fok") r.tif = book::TimeInForce::FOK; else return error(400, "tif must be gtc, ioc or fok");
    r.price = 0;
    if (r.type == Type::Limit && !get_u32(f, "price", r.price)) return error(400, "a limit order needs an integer price in ticks");
    std::lock_guard<std::mutex> lk(g_mu);
    const Result res = g_oms->submit(r);
    if (!res.duplicate) record_trades(res);
    return reject_or_ok(res);
}

Reply post_cancel(const std::string& body) {
    Flat f;
    uint32_t acc; uint64_t id;
    if (!parse_flat(body, f) || !get_u32(f, "account", acc) || !get_u64(f, "order_id", id)) return error(400, "need integer account and order_id");
    std::lock_guard<std::mutex> lk(g_mu);
    return reject_or_ok(g_oms->cancel(acc, id));
}

Reply post_modify(const std::string& body) {
    Flat f;
    uint32_t acc, price, qty; uint64_t id;
    if (!parse_flat(body, f) || !get_u32(f, "account", acc) || !get_u64(f, "order_id", id) || !get_u32(f, "price", price) || !get_u32(f, "qty", qty)) return error(400, "need integer account, order_id, price and qty");
    std::lock_guard<std::mutex> lk(g_mu);
    const Result res = g_oms->modify(acc, id, price, qty);
    record_trades(res);
    return reject_or_ok(res);
}

std::string query_value(const std::string& q, const std::string& key) {
    size_t i = 0;
    while (i < q.size()) {
        const size_t e = std::min(q.find('&', i), q.size());
        const std::string part = q.substr(i, e - i);
        const size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == key) return part.substr(eq + 1);
        i = e + 1;
    }
    return "";
}
uint32_t to_u32(const std::string& s, uint32_t dflt) {
    if (s.empty() || s.size() > 9) return dflt;
    uint32_t v = 0;
    for (char c : s) { if (c < '0' || c > '9') return dflt; v = v * 10 + static_cast<uint32_t>(c - '0'); }
    return v;
}

Reply get_book(const std::string& query) {
    const uint32_t depth = std::min<uint32_t>(std::max<uint32_t>(to_u32(query_value(query, "depth"), 10), 1), 50);
    std::lock_guard<std::mutex> lk(g_mu);
    const book::OrderBook& b = g_oms->book();
    std::ostringstream o;
    o << "{\"ok\":true,\"bids\":[";
    uint32_t found = 0, scanned = 0;
    for (uint32_t p = b.best_bid(); p >= 1 && found < depth && scanned < 20000; --p, ++scanned) {
        const uint64_t q = b.qty_at(Side::Buy, p);
        if (q) { o << (found ? "," : "") << "[" << p << "," << q << "]"; ++found; }
        if (p == 1) break;
    }
    o << "],\"asks\":[";
    found = scanned = 0;
    if (b.best_ask()) for (uint32_t p = b.best_ask(); p < g_max_price && found < depth && scanned < 20000; ++p, ++scanned) {
        const uint64_t q = b.qty_at(Side::Sell, p);
        if (q) { o << (found ? "," : "") << "[" << p << "," << q << "]"; ++found; }
    }
    o << "]}";
    return {200, o.str()};
}

Reply get_trades() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::ostringstream o;
    o << "{\"ok\":true,\"trades\":[";
    bool first = true;
    for (auto it = g_trades.rbegin(); it != g_trades.rend() && std::distance(g_trades.rbegin(), it) < 50; ++it) {
        o << (first ? "" : ",") << "{\"seq\":" << it->seq << ",\"price\":" << it->price << ",\"qty\":" << it->qty << ",\"aggressor\":\"" << it->aggressor << "\"}";
        first = false;
    }
    o << "]}";
    return {200, o.str()};
}

Reply get_account(const std::string& query) {
    const uint32_t acc = to_u32(query_value(query, "account"), 0);
    if (acc == 0) return error(400, "account=N is required");
    std::lock_guard<std::mutex> lk(g_mu);
    const Account* a = g_oms->account(acc);
    const book::OrderBook& b = g_oms->book();
    const int64_t bb = b.best_bid(), ba = b.best_ask();
    const bool have_mark = bb > 0 && ba > 0;
    const int64_t mark = have_mark ? (bb + ba) / 2 : 0;
    const int64_t pos = a ? a->position : 0, cash = a ? a->cash : 0;
    std::ostringstream o;
    o << "{\"ok\":true,\"account\":" << acc << ",\"position\":" << pos << ",\"cash\":" << cash << ",\"open_buy\":" << (a ? a->open_buy : 0) << ",\"open_sell\":" << (a ? a->open_sell : 0)
      << ",\"mark\":" << (have_mark ? std::to_string(mark) : "null") << ",\"equity\":" << (have_mark ? std::to_string(cash + pos * mark) : "null") << ",\"open_orders\":[";
    bool first = true;
    for (const OrderRec& r : g_oms->open_orders(acc)) {
        o << (first ? "" : ",") << "{\"order_id\":" << r.id << ",\"cl_ord_id\":" << r.cl_ord_id << ",\"side\":\"" << (r.side == Side::Buy ? "buy" : "sell") << "\",\"price\":" << r.price
          << ",\"qty\":" << r.qty << ",\"filled\":" << r.filled << ",\"remaining\":" << r.remaining << "}";
        first = false;
    }
    o << "]}";
    return {200, o.str()};
}
Reply route(const std::string& method, const std::string& target, const std::string& body) {
    const size_t qm = target.find('?');
    const std::string path = target.substr(0, qm), query = qm == std::string::npos ? "" : target.substr(qm + 1);
    const bool get = method == "GET", post = method == "POST";
    if (path == "/health") return get ? Reply{200, "{\"ok\":true}"} : error(405, "use GET");
    if (path == "/config") {
        if (!get) return error(405, "use GET");
        std::ostringstream o;
        o << "{\"ok\":true,\"price_decimals\":" << g_price_dec << ",\"qty_decimals\":" << g_qty_dec << ",\"max_price_ticks\":" << g_max_price
          << ",\"max_order_qty_lots\":" << g_oms->config().max_order_qty << ",\"max_position_lots\":" << g_oms->config().max_position << "}";
        return {200, o.str()};
    }
    if (path == "/book") return get ? get_book(query) : error(405, "use GET");
    if (path == "/trades") return get ? get_trades() : error(405, "use GET");
    if (path == "/account") return get ? get_account(query) : error(405, "use GET");
    if (path == "/orders") return post ? post_orders(body) : error(405, "use POST");
    if (path == "/cancel") return post ? post_cancel(body) : error(405, "use POST");
    if (path == "/modify") return post ? post_modify(body) : error(405, "use POST");
    if (path == "/debug/check") {
        if (!get) return error(405, "use GET");
        std::lock_guard<std::mutex> lk(g_mu);
        std::string why;
        const bool ok = g_oms->check(&why);
        return {ok ? 200 : 500, std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"why\":\"" + (ok ? "" : why) + "\"}"};
    }
    return error(404, "no such path");
}

bool send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

void reply(int fd, const Reply& r) {
    const char* text = r.code == 200 ? "OK" : r.code == 400 ? "Bad Request" : r.code == 404 ? "Not Found" : r.code == 405 ? "Method Not Allowed" : r.code == 408 ? "Request Timeout"
                     : r.code == 413 ? "Payload Too Large" : r.code == 422 ? "Unprocessable Entity" : r.code == 503 ? "Service Unavailable" : "Error";
    std::ostringstream o;
    o << "HTTP/1.1 " << r.code << " " << text << "\r\nContent-Type: application/json\r\nContent-Length: " << r.body.size()
      << "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n\r\n" << r.body;
    send_all(fd, o.str());
}

std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

void serve(int fd) {
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string buf;
    char tmp[4096];
    size_t hend = std::string::npos;
    while (hend == std::string::npos) {
        if (buf.size() > kMaxHeader) { reply(fd, error(413, "headers too large")); close(fd); return; }
        const ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) { if (!buf.empty()) reply(fd, error(408, "incomplete request")); close(fd); return; }
        buf.append(tmp, static_cast<size_t>(n));
        hend = buf.find("\r\n\r\n");
    }
    const std::string head = buf.substr(0, hend);
    const size_t le = head.find("\r\n");
    std::istringstream rl(head.substr(0, le));
    std::string method, target, ver;
    rl >> method >> target >> ver;
    if (method.empty() || target.empty() || target[0] != '/' || ver.rfind("HTTP/1.", 0) != 0) { reply(fd, error(400, "bad request line")); close(fd); return; }
    size_t clen = 0;
    for (size_t p = le; p != std::string::npos && p < head.size();) {
        const size_t e = std::min(head.find("\r\n", p + 2), head.size());
        const std::string line = lower(head.substr(p + 2, e - p - 2));
        if (line.rfind("content-length:", 0) == 0) {
            const std::string v = line.substr(15);
            uint64_t n = 0; bool any = false;
            for (char c : v) { if (c == ' ') continue; if (c < '0' || c > '9' || n > kMaxBody) { n = kMaxBody + 1; break; } n = n * 10 + static_cast<uint64_t>(c - '0'); any = true; }
            if (!any || n > kMaxBody) { reply(fd, error(413, "bad or too large Content-Length")); close(fd); return; }
            clen = static_cast<size_t>(n);
        }
        p = e == head.size() ? std::string::npos : e;
    }
    std::string body = buf.substr(hend + 4);
    while (body.size() < clen) {
        const ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) { reply(fd, error(408, "incomplete body")); close(fd); return; }
        body.append(tmp, static_cast<size_t>(n));
        if (body.size() > kMaxBody) { reply(fd, error(413, "body too large")); close(fd); return; }
    }
    body.resize(clen);
    if (method == "OPTIONS") reply(fd, {200, "{}"});
    else reply(fd, route(method, target, body));
    close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8081;
    uint32_t max_qty = 1000000, seed_mid = 0, bugs = 0;
    int64_t max_pos = 10000000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", name); std::exit(2); } return argv[++i]; };
        if (a == "--port") port = std::atoi(next("--port"));
        else if (a == "--price-decimals") g_price_dec = std::atoi(next("--price-decimals"));
        else if (a == "--qty-decimals") g_qty_dec = std::atoi(next("--qty-decimals"));
        else if (a == "--max-price") g_max_price = static_cast<uint32_t>(std::strtoul(next("--max-price"), nullptr, 10));
        else if (a == "--max-qty") max_qty = static_cast<uint32_t>(std::strtoul(next("--max-qty"), nullptr, 10));
        else if (a == "--max-position") max_pos = std::strtoll(next("--max-position"), nullptr, 10);
        else if (a == "--seed-mid") seed_mid = static_cast<uint32_t>(std::strtoul(next("--seed-mid"), nullptr, 10));
        else if (a == "--bug") bugs = static_cast<uint32_t>(std::strtoul(next("--bug"), nullptr, 10));   // test only: switches on a deliberate OMS bug
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (port < 1 || port > 65535 || g_max_price < 100) { std::fprintf(stderr, "bad --port or --max-price\n"); return 2; }
    Config c;
    c.max_orders = 1u << 16; c.max_price = g_max_price; c.max_order_qty = max_qty; c.max_position = max_pos; c.bugs = bugs;
    Oms oms(c);
    g_oms = &oms;
    if (seed_mid > 20 && seed_mid + 100 < g_max_price) {            // a small ladder from account 100 so there is something to trade against
        uint64_t cl = 1;
        const uint32_t lots = 100000;                                // 1 BTC at 5 quantity decimals
        for (uint32_t i = 1; i <= 10; ++i) {
            oms.submit({100, cl++, Side::Sell, Type::Limit, book::TimeInForce::GTC, seed_mid + i * 5, lots});
            oms.submit({100, cl++, Side::Buy, Type::Limit, book::TimeInForce::GTC, seed_mid - i * 5, lots});
        }
    }
    signal(SIGPIPE, SIG_IGN);
    const int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(ls, 64) != 0) { std::perror("bind/listen"); return 1; }
    std::printf("gateway listening on http://127.0.0.1:%d  (price = ticks of 10^-%d, quantity = lots of 10^-%d)\n", port, g_price_dec, g_qty_dec);
    std::fflush(stdout);
    while (true) {
        const int fd = accept(ls, nullptr, nullptr);
        if (fd < 0) { if (errno == EINTR) continue; std::perror("accept"); break; }
        if (g_active.load() >= kMaxConn) { reply(fd, error(503, "too many connections")); close(fd); continue; }
        ++g_active;
        std::thread([fd] { serve(fd); --g_active; }).detach();
    }
    return 0;
}
