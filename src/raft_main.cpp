// One Raft node as a real OS process: UDP on localhost, state saved to a file, order book replicated through the log.
//   raft_main --id I [--n 3] [--base-port 7000] [--dir /tmp/meridian-raft]
// stdin:  p N  -> propose N order requests (only the leader accepts)     q -> quit
// stdout: one `STATE ...` line whenever something changes (and once a second), plus EVENT lines.
// The Node class is the same pure state machine the simulator tests; this file only adds a clock, sockets and a disk.
// Persistence rule: state is written (temp file, fdatasync, rename) BEFORE any message produced by the same step is sent.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "meridian/raft_apply.hpp"
#include "meridian/raft_node.hpp"

using namespace meridian::raft;
using clk = std::chrono::steady_clock;

namespace {

uint64_t wall_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---- wire format (host byte order: all nodes run on one machine in this version) ----
void put(std::vector<uint8_t>& b, const void* p, size_t n) { const auto* c = (const uint8_t*)p; b.insert(b.end(), c, c + n); }
std::vector<uint8_t> encode(const Message& m) {
    std::vector<uint8_t> b;
    const uint8_t t = (uint8_t)m.type, ok = m.ok;
    const uint32_t ne = (uint32_t)m.entries.size();
    put(b, &t, 1); put(b, &m.from, 4); put(b, &m.to, 4); put(b, &m.term, 8); put(b, &m.last_log_index, 8);
    put(b, &m.last_log_term, 8); put(b, &ok, 1); put(b, &m.leader_commit, 8); put(b, &m.match_index, 8); put(b, &ne, 4);
    for (const Entry& e : m.entries) { put(b, &e.term, 8); put(b, &e.cmd, 8); }
    return b;
}
bool decode(const uint8_t* p, size_t len, uint32_t n_nodes, Message* m) {
    constexpr size_t kHead = 1 + 4 + 4 + 8 + 8 + 8 + 1 + 8 + 8 + 4;
    if (len < kHead) return false;
    size_t o = 0;
    auto get = [&](void* d, size_t n) { std::memcpy(d, p + o, n); o += n; };
    uint8_t t, ok; uint32_t ne;
    get(&t, 1); get(&m->from, 4); get(&m->to, 4); get(&m->term, 8); get(&m->last_log_index, 8);
    get(&m->last_log_term, 8); get(&ok, 1); get(&m->leader_commit, 8); get(&m->match_index, 8); get(&ne, 4);
    if (t > 5 || m->from >= n_nodes || ne > 4096 || len != kHead + (size_t)ne * 16) return false;
    m->type = (MsgType)t; m->ok = ok != 0;
    m->entries.clear();
    for (uint32_t i = 0; i < ne; ++i) { Entry e; get(&e.term, 8); get(&e.cmd, 8); m->entries.push_back(e); }
    return true;
}

// ---- disk ----
bool save(const std::string& path, const Persistent& s) {
    const std::string tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    std::vector<uint8_t> b;
    const uint64_t cnt = s.log.size();
    put(b, &s.current_term, 8); put(b, &s.voted_for, 4); put(b, &cnt, 8);
    for (const Entry& e : s.log) { put(b, &e.term, 8); put(b, &e.cmd, 8); }
    const bool ok = write(fd, b.data(), b.size()) == (ssize_t)b.size() && fdatasync(fd) == 0;
    close(fd);
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}
Persistent load(const std::string& path) {
    Persistent s;
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return s;
    uint64_t cnt = 0;
    if (read(fd, &s.current_term, 8) == 8 && read(fd, &s.voted_for, 4) == 4 && read(fd, &cnt, 8) == 8) {
        for (uint64_t i = 0; i < cnt; ++i) { Entry e; if (read(fd, &e.term, 8) != 8 || read(fd, &e.cmd, 8) != 8) break; s.log.push_back(e); }
    }
    close(fd);
    return s;
}

volatile std::sig_atomic_t g_stop = 0;

}  // namespace

int main(int argc, char** argv) {
    uint32_t id = 0, n = 3; int base = 7000; std::string dir = "/tmp/meridian-raft"; bool have_id = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--id") && i + 1 < argc) { id = (uint32_t)std::atoi(argv[++i]); have_id = true; }
        else if (!std::strcmp(argv[i], "--n") && i + 1 < argc) n = (uint32_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--base-port") && i + 1 < argc) base = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else { std::fprintf(stderr, "usage: raft_main --id I [--n 3] [--base-port 7000] [--dir DIR]\n"); return 2; }
    }
    if (!have_id || id >= n) { std::fprintf(stderr, "need --id in [0, n)\n"); return 2; }
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::string cmd = "mkdir -p '" + dir + "'";
    if (std::system(cmd.c_str()) != 0) { std::fprintf(stderr, "cannot create %s\n", dir.c_str()); return 1; }
    const std::string path = dir + "/node" + std::to_string(id) + ".state";

    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in me{}; me.sin_family = AF_INET; me.sin_port = htons((uint16_t)(base + id)); me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, (sockaddr*)&me, sizeof me) != 0) { std::perror("bind"); return 1; }
    std::vector<sockaddr_in> peer(n);
    for (uint32_t j = 0; j < n; ++j) { peer[j] = me; peer[j].sin_port = htons((uint16_t)(base + j)); }

    Config cfg; cfg.n = n; cfg.prevote = true; cfg.check_quorum = true;
    const auto t0 = clk::now();
    auto now_ms = [&] { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count(); };
    Persistent disk = load(path);
    Node node(id, cfg, disk, (uint64_t)getpid() * 1000003ull + wall_ms(), now_ms());
    uint64_t saved_pv = node.persist_version();
    const std::vector<Op> script = make_script(1, 200000);
    Replica rep;
    uint64_t applied = 0;
    std::printf("EVENT %llu node %u started: term %llu, log %zu entries loaded from %s\n", (unsigned long long)wall_ms(), id,
                (unsigned long long)node.term(), node.persistent().log.size(), path.c_str());
    std::fflush(stdout);

    std::vector<Message> out;
    auto flush = [&] {                                       // 1) persist if changed, 2) only then send
        if (node.persist_version() != saved_pv) {
            if (!save(path, node.persistent())) { std::fprintf(stderr, "FATAL: cannot save state\n"); std::exit(1); }
            saved_pv = node.persist_version();
        }
        for (const Message& m : out) {
            const auto b = encode(m);
            if (m.to < n) sendto(sock, b.data(), b.size(), 0, (const sockaddr*)&peer[m.to], sizeof(sockaddr_in));
        }
        out.clear();
    };

    uint64_t last_print = 0; std::string last_line;
    Role prev_role = node.role();
    bool stdin_open = true;
    while (!g_stop) {
        pollfd fds[2] = {{sock, POLLIN, 0}, {0, POLLIN, 0}};
        poll(fds, stdin_open ? 2 : 1, 5);
        if (fds[0].revents & POLLIN) {
            for (int k = 0; k < 256; ++k) {
                uint8_t buf[70000];
                const ssize_t len = recv(sock, buf, sizeof buf, MSG_DONTWAIT);
                if (len < 0) break;
                Message m;
                if (decode(buf, (size_t)len, n, &m) && m.to == id) node.on_message(m, now_ms(), out);
                flush();
            }
        }
        if (stdin_open && (fds[1].revents & (POLLIN | POLLHUP))) {
            char line[256];
            if (!std::fgets(line, sizeof line, stdin)) stdin_open = false;
            else if (line[0] == 'q') break;
            else if (line[0] == 'p') {
                const int cnt = std::atoi(line + 1) > 0 ? std::atoi(line + 1) : 1;
                for (int k = 0; k < cnt; ++k) {
                    uint64_t mx = 0;
                    for (const Entry& e : node.persistent().log) mx = std::max(mx, e.cmd);
                    if (mx >= script.size()) { std::printf("EVENT script exhausted\n"); break; }
                    const uint64_t idx = node.propose(mx + 1, now_ms(), out);
                    if (!idx) { std::printf("EVENT %llu node %u not leader (leader=%d)\n", (unsigned long long)wall_ms(), id, node.leader() == kNone ? -1 : (int)node.leader()); break; }
                    flush();
                }
                std::fflush(stdout);
            }
        }
        node.tick(now_ms(), out);
        flush();
        while (applied < node.commit_index() && applied < node.persistent().log.size()) {          // apply committed entries in order
            rep.apply_cmd(script, node.persistent().log[applied].cmd);
            ++applied;
        }
        if (node.role() != prev_role) {
            std::printf("EVENT %llu node %u became %s in term %llu\n", (unsigned long long)wall_ms(), id,
                        node.role() == Role::Leader ? "LEADER" : node.role() == Role::Candidate ? "candidate" : "follower", (unsigned long long)node.term());
            prev_role = node.role();
        }
        char line[256];
        std::snprintf(line, sizeof line, "STATE id=%u role=%c term=%llu leader=%d log=%zu commit=%llu applied=%llu orders=%llu resting=%zu trades=%llu digest=%016llx",
                      id, node.role() == Role::Leader ? 'L' : node.role() == Role::Candidate ? 'C' : 'F', (unsigned long long)node.term(),
                      node.leader() == kNone ? -1 : (int)node.leader(), node.persistent().log.size(), (unsigned long long)node.commit_index(),
                      (unsigned long long)applied, (unsigned long long)rep.applied(), rep.book().size(), (unsigned long long)rep.trades(),
                      (unsigned long long)rep.digest());
        if (line != last_line || now_ms() - last_print >= 5000) {
            std::printf("%s\n", line); std::fflush(stdout);
            last_line = line; last_print = now_ms();
        }
    }
    return 0;
}
