#pragma once
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace meridian {

inline bool send_all(int fd, const void* p, size_t n) {
    auto* c = static_cast<const uint8_t*>(p);
    while (n) {
        const ssize_t w = ::send(fd, c, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        c += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

inline bool recv_all(int fd, void* p, size_t n) {
    auto* c = static_cast<uint8_t*>(p);
    while (n) {
        const ssize_t r = ::recv(fd, c, n, 0);
        if (r == 0) return false;
        if (r < 0) { if (errno == EINTR) continue; return false; }
        c += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

inline void set_nodelay(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

}  // namespace meridian
