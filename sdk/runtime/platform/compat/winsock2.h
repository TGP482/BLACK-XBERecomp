#pragma once
// Linux builds (and the headless harness): Winsock names on POSIX sockets
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
constexpr long FIONREAD_POSIX = FIONREAD;
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <chrono>
#include <thread>
typedef int SOCKET; typedef int BOOL; typedef uint32_t DWORD; typedef unsigned long u_long; typedef uint16_t WORD;
#define INVALID_SOCKET (-1)
#define FALSE 0
#define MAKEWORD(a, b) ((WORD)(((a) & 0xff) | (((b) & 0xff) << 8)))
struct WSADATA { int x; };
inline int WSAStartup(WORD, WSADATA*) { return 0; }
enum { WSAEWOULDBLOCK = 10035, WSAECONNRESET = 10054, WSAEINVAL = 10022, WSAENOTSOCK = 10038, WSAEAFNOSUPPORT = 10047,
       WSAESOCKTNOSUPPORT = 10044, WSAETIMEDOUT = 10060, WSAHOST_NOT_FOUND = 11001, WSA_IO_PENDING = 997,
       WSA_IO_INCOMPLETE = 996, WSA_OPERATION_ABORTED = 995, WSAEINPROGRESS = 10036, WSAENOTCONN = 10057,
       WSAECONNREFUSED = 10061, WSAEADDRINUSE = 10048, WSAEMSGSIZE = 10040 };
inline int WSAGetLastError() {
    switch (errno) {
        case EAGAIN: case EINPROGRESS: return WSAEWOULDBLOCK;
        case ECONNRESET: return WSAECONNRESET; case ENOTCONN: return WSAENOTCONN; case ECONNREFUSED: return WSAECONNREFUSED;
        case EADDRINUSE: return WSAEADDRINUSE; case EINVAL: return WSAEINVAL; case EMSGSIZE: return WSAEMSGSIZE;
        default: return 10000 + errno;
    }
}
inline int closesocket(SOCKET s) { return close(s); }
// Winsock's FIONBIO and FIONREAD
#undef FIONBIO
#define FIONBIO long(0x8004667E)
inline int ioctlsocket(SOCKET s, long cmd, u_long* v) {
    if (cmd == 0x4004667F) cmd = FIONREAD_POSIX;
    if (cmd == (long)FIONBIO) { int f = fcntl(s, F_GETFL); return fcntl(s, F_SETFL, *v ? f | O_NONBLOCK : f & ~O_NONBLOCK); }
    int n = 0; int r = ioctl(s, cmd, &n); *v = (u_long)n; return r;
}
inline int WSAIoctl(SOCKET, DWORD, void*, DWORD, void*, DWORD, DWORD*, void*, void*) { return 0; }
#ifndef XBE_COMPAT_SLEEP
#define XBE_COMPAT_SLEEP  // windows.h's compat may have it already
inline void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif
struct ws_fd_set { unsigned fd_count; SOCKET fd_array[64]; };
#define fd_set ws_fd_set
#undef FD_ZERO
#undef FD_SET
#undef FD_ISSET
#undef FD_SETSIZE
#define FD_SETSIZE 64
#define FD_ZERO(p) ((p)->fd_count = 0)
#define FD_SET(s, p) ((p)->fd_array[(p)->fd_count++] = (s))
inline bool ws_isset(SOCKET s, const ws_fd_set* p) { for (unsigned i = 0; i < p->fd_count; i++) if (p->fd_array[i] == s) return true; return false; }
#define FD_ISSET(s, p) ws_isset((s), (p))
inline int ws_select(int, ws_fd_set* r, ws_fd_set* w, ws_fd_set* e, timeval* tv) {
    pollfd fds[192]; int n = 0;
    ws_fd_set* sets[3] = {r, w, e}; short ev[3] = {POLLIN, POLLOUT, POLLPRI};
    for (int k = 0; k < 3; k++) if (sets[k]) for (unsigned i = 0; i < sets[k]->fd_count; i++) fds[n++] = {sets[k]->fd_array[i], ev[k], 0};
    int ms = tv ? int(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    int rc = poll(fds, n, ms);
    if (rc < 0) return rc;
    int total = 0, j = 0;
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        ws_fd_set out{}; for (unsigned i = 0; i < sets[k]->fd_count; i++, j++)
            if (fds[j].revents & (ev[k] | POLLERR | POLLHUP)) out.fd_array[out.fd_count++] = fds[j].fd;
        total += out.fd_count; *sets[k] = out;
    }
    return total;
}
#define select ws_select
inline int getsockname(SOCKET s, sockaddr* a, int* l) { socklen_t n = socklen_t(*l); int r = ::getsockname(s, a, &n); *l = int(n); return r; }
inline int getpeername(SOCKET s, sockaddr* a, int* l) { socklen_t n = socklen_t(*l); int r = ::getpeername(s, a, &n); *l = int(n); return r; }
inline ssize_t recvfrom(SOCKET s, char* b, int len, int f, sockaddr* a, int* l) { socklen_t n = socklen_t(*l); ssize_t r = ::recvfrom(s, b, size_t(len), f, a, &n); *l = int(n); return r; }
inline SOCKET accept(SOCKET s, sockaddr* a, int* l) { socklen_t n = l ? socklen_t(*l) : 0; SOCKET r = ::accept(s, a, l ? &n : nullptr); if (l) *l = int(n); return r; }
// Winsock option numbers to POSIX
inline void ws_opt(int& level, int& opt) {
    if (level != 0xFFFF) return;
    level = SOL_SOCKET;
    switch (opt) {
        case 4: opt = SO_REUSEADDR; break; case 0x20: opt = SO_BROADCAST; break; case 0x1001: opt = SO_SNDBUF; break;
        case 0x1002: opt = SO_RCVBUF; break; case 0x1007: opt = SO_ERROR; break; case 0x80: opt = SO_LINGER; break;
        case 8: opt = SO_KEEPALIVE; break; case 0x1008: opt = SO_TYPE; break;
    }
}
inline int setsockopt(SOCKET s, int lv, int o, const char* v, int l) { ws_opt(lv, o); return ::setsockopt(s, lv, o, static_cast<const void*>(v), socklen_t(l)); }
inline int getsockopt(SOCKET s, int lv, int o, char* v, int* l) { ws_opt(lv, o); socklen_t n = socklen_t(*l); int r = ::getsockopt(s, lv, o, v, &n); *l = int(n); return r; }
