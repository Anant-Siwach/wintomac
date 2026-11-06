// Winsock 2 (ws2_32.dll / wsock32.dll) on BSD sockets.
//
// SOCKET values are host file descriptors. Everything that crosses the boundary is converted:
//   * sockaddr: Windows { u16 family; ... } vs BSD { u8 len; u8 family; ... }; AF_INET6 is 23 vs 30;
//   * option levels/names (SOL_SOCKET = 0xFFFF, SO_* numbering, IPPROTO_* options that differ);
//   * errors: WSAGetLastError() codes (WSAEWOULDBLOCK 10035, ...), and non-blocking connect reporting
//     WSAEWOULDBLOCK where BSD says EINPROGRESS;
//   * fd_set: Windows { u32 count; SOCKET array[64] } -> implemented with poll();
//   * addrinfo/hostent: Windows layouts, allocated here and freed with freeaddrinfo.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "win32.h"

#define INVALID_SOCKET (~0ull)
#define SOCKET_ERROR ((u64)(u32)-1)
#define W_AF_INET 2
#define W_AF_INET6 23
#define W_AF_UNSPEC 0
static __thread u32 wsa_err;

static u32 map_errno(int e) {
    switch (e) {
    case EWOULDBLOCK: return 10035;   // WSAEWOULDBLOCK (EAGAIN == EWOULDBLOCK on macOS)
    case EINPROGRESS: return 10035;   // Windows reports a pending connect as WSAEWOULDBLOCK
    case EALREADY: return 10037; case ENOTSOCK: return 10038; case EDESTADDRREQ: return 10039; case EMSGSIZE: return 10040;
    case EPROTOTYPE: return 10041; case ENOPROTOOPT: return 10042; case EPROTONOSUPPORT: return 10043;
    case EOPNOTSUPP: return 10045; case EAFNOSUPPORT: return 10047; case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049; case ENETDOWN: return 10050; case ENETUNREACH: return 10051; case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054; case ENOBUFS: return 10055; case EISCONN: return 10056; case ENOTCONN: return 10057;
    case ESHUTDOWN: return 10058; case ETIMEDOUT: return 10060; case ECONNREFUSED: return 10061; case EHOSTUNREACH: return 10065;
    case EBADF: return 10009; case EACCES: return 10013; case EFAULT: return 10014; case EINVAL: return 10022; case EMFILE: return 10024;
    case EINTR: return 10004; case EPIPE: return 10054;
    default: return 10022;
    }
}
static u64 fail(void) { wsa_err = map_errno(errno); return SOCKET_ERROR; }
#define FD(s) ((int)(s))

// ---- sockaddr conversion ---------------------------------------------------------------------
static socklen_t to_host(u64 w, i32 wlen, struct sockaddr_storage *h) {
    memset(h, 0, sizeof *h);
    if (!w) return 0;
    u16 fam = LD16(w);
    if (fam == W_AF_INET && wlen >= 16) {
        struct sockaddr_in *a = (struct sockaddr_in *)h;
        a->sin_len = sizeof *a; a->sin_family = AF_INET; memcpy(&a->sin_port, (void *)(w + 2), 2); memcpy(&a->sin_addr, (void *)(w + 4), 4);
        return sizeof *a;
    }
    if (fam == W_AF_INET6 && wlen >= 28) {   // { family, port, flowinfo, addr[16], scope_id }
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)h;
        a->sin6_len = sizeof *a; a->sin6_family = AF_INET6; memcpy(&a->sin6_port, (void *)(w + 2), 2);
        memcpy(&a->sin6_flowinfo, (void *)(w + 4), 4); memcpy(&a->sin6_addr, (void *)(w + 8), 16); memcpy(&a->sin6_scope_id, (void *)(w + 24), 4);
        return sizeof *a;
    }
    if (fam == W_AF_UNSPEC) { h->ss_family = AF_UNSPEC; return 16; }
    return 0;
}
// Writes a Windows sockaddr; *wlen is in/out like Winsock (needed size reported on overflow).
static int to_win(const struct sockaddr *h, u64 w, u64 wlenp) {
    u8 tmp[28] = { 0 }; i32 need;
    if (h->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)h;
        ST16((u64)tmp, W_AF_INET); memcpy(tmp + 2, &a->sin_port, 2); memcpy(tmp + 4, &a->sin_addr, 4); need = 16;
    } else if (h->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)h;
        ST16((u64)tmp, W_AF_INET6); memcpy(tmp + 2, &a->sin6_port, 2); memcpy(tmp + 4, &a->sin6_flowinfo, 4);
        memcpy(tmp + 8, &a->sin6_addr, 16); memcpy(tmp + 24, &a->sin6_scope_id, 4); need = 28;
    } else { need = 16; ST16((u64)tmp, h->sa_family); }
    if (!w || !wlenp) return 1;
    i32 have = (i32)LD32(wlenp);
    ST32(wlenp, (u32)need);
    if (have < need) { wsa_err = 10014; return 0; }   // WSAEFAULT
    memcpy((void *)w, tmp, need);
    return 1;
}
static int fam_to_host(u32 f) { return f == W_AF_INET6 ? AF_INET6 : f == W_AF_INET ? AF_INET : f == W_AF_UNSPEC ? AF_UNSPEC : -1; }
static u32 fam_to_win(int f) { return f == AF_INET6 ? W_AF_INET6 : f == AF_INET ? W_AF_INET : W_AF_UNSPEC; }

// ---- startup / sockets ------------------------------------------------------------------------
SHIM(WSAStartup) {  // (version, WSADATA*): WSADATA x64 = { u16 ver, high; u16 maxsock, maxudp; char *vendor; char desc[257], status[129] }
    u64 d = A2;
    if (d) {
        memset((void *)d, 0, 408);
        ST16(d, (u16)A1 ? (u16)A1 : 0x0202); ST16(d + 2, 0x0202); ST16(d + 4, 0); ST16(d + 6, 0);
        strcpy((char *)d + 16, "WinSock 2.0 (wintomac)"); strcpy((char *)d + 16 + 257, "Running");
    }
    signal(SIGPIPE, SIG_IGN);   // Windows send() on a broken connection returns an error instead of killing the process
    RET(0);
}
SHIM(WSACleanup) { RET(0); }
SHIM(WSAGetLastError) { RET(wsa_err); }
SHIM(WSASetLastError) { wsa_err = (u32)A1; RET(0); }

static u64 make_socket(u32 af, u32 type, u32 proto) {
    int fam = fam_to_host(af);
    if (fam < 0) { wsa_err = 10047; return INVALID_SOCKET; }
    int fd = socket(fam == AF_UNSPEC ? AF_INET : fam, (int)type, (int)proto);
    if (fd < 0) { fail(); return INVALID_SOCKET; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return (u64)fd;
}
SHIM(socket) { RET(make_socket((u32)A1, (u32)A2, (u32)A3)); }
SHIM(WSASocketW) { RET(make_socket((u32)A1, (u32)A2, (u32)A3)); }   // (af, type, proto, info, group, flags): overlapped flag ignored
SHIM(WSASocketA) { RET(make_socket((u32)A1, (u32)A2, (u32)A3)); }
SHIM(closesocket) { RET(close(FD(A1)) ? fail() : 0); }
SHIM(bind) { struct sockaddr_storage h; socklen_t n = to_host(A2, (i32)A3, &h); if (!n) { wsa_err = 10047; RET(SOCKET_ERROR); } RET(bind(FD(A1), (struct sockaddr *)&h, n) ? fail() : 0); }
SHIM(listen) { RET(listen(FD(A1), (int)(i32)A2 > 0 ? (int)(i32)A2 : 128) ? fail() : 0); }
SHIM(connect) {
    struct sockaddr_storage h; socklen_t n = to_host(A2, (i32)A3, &h);
    if (!n) { wsa_err = 10047; RET(SOCKET_ERROR); }
    RET(connect(FD(A1), (struct sockaddr *)&h, n) ? fail() : 0);
}
SHIM(accept) {
    struct sockaddr_storage h; socklen_t n = sizeof h;
    int fd = accept(FD(A1), (struct sockaddr *)&h, &n);
    if (fd < 0) { fail(); RET(INVALID_SOCKET); }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    if (A2 && !to_win((struct sockaddr *)&h, A2, A3)) { close(fd); RET(INVALID_SOCKET); }
    RET((u64)fd);
}
SHIM(send) { ssize_t r = send(FD(A1), (void *)A2, (size_t)(i32)A3, 0); RET(r < 0 ? fail() : (u64)(u32)r); }
SHIM(recv) {
    int fl = ((u32)A4 & 1 ? MSG_OOB : 0) | ((u32)A4 & 2 ? MSG_PEEK : 0) | ((u32)A4 & 8 ? MSG_WAITALL : 0);
    ssize_t r = recv(FD(A1), (void *)A2, (size_t)(i32)A3, fl);
    RET(r < 0 ? fail() : (u64)(u32)r);
}
SHIM(sendto) {  // (s, buf, len, flags, to, tolen)
    struct sockaddr_storage h; socklen_t n = AN(5) ? to_host(AN(5), (i32)AN(6), &h) : 0;
    ssize_t r = sendto(FD(A1), (void *)A2, (size_t)(i32)A3, 0, n ? (struct sockaddr *)&h : NULL, n);
    RET(r < 0 ? fail() : (u64)(u32)r);
}
SHIM(recvfrom) {  // (s, buf, len, flags, from, *fromlen)
    struct sockaddr_storage h; socklen_t n = sizeof h;
    ssize_t r = recvfrom(FD(A1), (void *)A2, (size_t)(i32)A3, ((u32)A4 & 2) ? MSG_PEEK : 0, (struct sockaddr *)&h, &n);
    if (r < 0) RET(fail());
    if (AN(5) && n && !to_win((struct sockaddr *)&h, AN(5), AN(6))) RET(SOCKET_ERROR);
    RET((u64)(u32)r);
}
SHIM(shutdown) { RET(shutdown(FD(A1), (int)(u32)A2) ? fail() : 0); }   // SD_RECEIVE/SEND/BOTH == SHUT_RD/WR/RDWR
SHIM(getsockname) { struct sockaddr_storage h; socklen_t n = sizeof h; if (getsockname(FD(A1), (struct sockaddr *)&h, &n)) RET(fail()); RET(to_win((struct sockaddr *)&h, A2, A3) ? 0 : SOCKET_ERROR); }
SHIM(getpeername) { struct sockaddr_storage h; socklen_t n = sizeof h; if (getpeername(FD(A1), (struct sockaddr *)&h, &n)) RET(fail()); RET(to_win((struct sockaddr *)&h, A2, A3) ? 0 : SOCKET_ERROR); }

SHIM(ioctlsocket) {  // (s, cmd, u32 *arg)
    u32 cmd = (u32)A2;
    if (cmd == 0x8004667E) {  // FIONBIO
        int fl = fcntl(FD(A1), F_GETFL);
        if (fl < 0) RET(fail());
        RET(fcntl(FD(A1), F_SETFL, LD32(A3) ? fl | O_NONBLOCK : fl & ~O_NONBLOCK) ? fail() : 0);
    }
    if (cmd == 0x4004667F) { int n = 0; if (ioctl(FD(A1), FIONREAD, &n)) RET(fail()); ST32(A3, (u32)n); RET(0); }   // FIONREAD
    wsa_err = 10045; RET(SOCKET_ERROR);
}

// ---- options ------------------------------------------------------------------------------------
static int map_opt(u32 level, u32 name, int *hl, int *hn) {
    if (level == 0xFFFF) {   // SOL_SOCKET
        *hl = SOL_SOCKET;
        switch (name) {
        case 0x0004: *hn = SO_REUSEADDR; return 1; case 0x0008: *hn = SO_KEEPALIVE; return 1; case 0x0020: *hn = SO_BROADCAST; return 1;
        case 0x0080: *hn = SO_LINGER; return 1; case 0x1001: *hn = SO_SNDBUF; return 1; case 0x1002: *hn = SO_RCVBUF; return 1;
        case 0x1005: *hn = SO_SNDTIMEO; return 1; case 0x1006: *hn = SO_RCVTIMEO; return 1; case 0x1007: *hn = SO_ERROR; return 1;
        case 0x1008: *hn = SO_TYPE; return 1; case 0x0002: *hn = SO_ACCEPTCONN; return 1;
        case 0xFFFFFFFBu: *hn = SO_REUSEADDR; return 1;   // SO_EXCLUSIVEADDRUSE (~SO_REUSEADDR): closest host behaviour
        default: return 0;
        }
    }
    if (level == 6 && name == 1) { *hl = IPPROTO_TCP; *hn = TCP_NODELAY; return 1; }
    if (level == 0) {       // IPPROTO_IP
        *hl = IPPROTO_IP;
        switch (name) { case 4: *hn = IP_TTL; return 1; case 9: *hn = IP_MULTICAST_IF; return 1; case 10: *hn = IP_MULTICAST_TTL; return 1;
                        case 11: *hn = IP_MULTICAST_LOOP; return 1; case 12: *hn = IP_ADD_MEMBERSHIP; return 1; case 13: *hn = IP_DROP_MEMBERSHIP; return 1;
                        default: return 0; }
    }
    if (level == 41) {      // IPPROTO_IPV6
        *hl = IPPROTO_IPV6;
        if (name == 27) { *hn = IPV6_V6ONLY; return 1; }
        return 0;
    }
    return 0;
}
SHIM(setsockopt) {  // (s, level, name, val, len)
    int hl, hn;
    if (!map_opt((u32)A2, (u32)A3, &hl, &hn)) { if (getenv("WINTOMAC_VERBOSE")) rt_log("setsockopt(%x,%x) unsupported: ignored", (u32)A2, (u32)A3); RET(0); }
    if (hn == SO_LINGER) { struct linger l = { LD16(A4), LD16(A4 + 2) }; RET(setsockopt(FD(A1), hl, hn, &l, sizeof l) ? fail() : 0); }
    if (hn == SO_SNDTIMEO || hn == SO_RCVTIMEO) { u32 ms = LD32(A4); struct timeval tv = { ms / 1000, (ms % 1000) * 1000 }; RET(setsockopt(FD(A1), hl, hn, &tv, sizeof tv) ? fail() : 0); }
    int v = (i32)AN(5) >= 4 ? (int)LD32(A4) : (int)LD8(A4);   // BOOL options may be passed as 1 byte
    if (hn == IP_ADD_MEMBERSHIP || hn == IP_DROP_MEMBERSHIP || hn == IP_MULTICAST_IF) RET(setsockopt(FD(A1), hl, hn, (void *)A4, (socklen_t)(i32)AN(5)) ? fail() : 0);
    if (hn == IP_MULTICAST_TTL || hn == IP_MULTICAST_LOOP) { u8 b = (u8)v; RET(setsockopt(FD(A1), hl, hn, &b, 1) ? fail() : 0); }
    RET(setsockopt(FD(A1), hl, hn, &v, sizeof v) ? fail() : 0);
}
SHIM(getsockopt) {  // (s, level, name, val, *len)
    int hl, hn;
    if (!map_opt((u32)A2, (u32)A3, &hl, &hn)) { wsa_err = 10042; RET(SOCKET_ERROR); }
    if (hn == SO_LINGER) { struct linger l; socklen_t n = sizeof l; if (getsockopt(FD(A1), hl, hn, &l, &n)) RET(fail()); ST16(A4, (u16)l.l_onoff); ST16(A4 + 2, (u16)l.l_linger); ST32(AN(5), 4); RET(0); }
    if (hn == SO_SNDTIMEO || hn == SO_RCVTIMEO) { struct timeval tv; socklen_t n = sizeof tv; if (getsockopt(FD(A1), hl, hn, &tv, &n)) RET(fail()); ST32(A4, (u32)(tv.tv_sec * 1000 + tv.tv_usec / 1000)); ST32(AN(5), 4); RET(0); }
    int v = 0; socklen_t n = sizeof v;
    if (getsockopt(FD(A1), hl, hn, &v, &n)) RET(fail());
    if (hn == SO_ERROR) v = v ? (int)map_errno(v) : 0;
    if (hn == SO_TYPE) v = v == SOCK_STREAM ? 1 : v == SOCK_DGRAM ? 2 : v;
    ST32(A4, (u32)v); if (AN(5)) ST32(AN(5), 4);
    RET(0);
}

// ---- select: Windows fd_set = { u32 fd_count; SOCKET fd_array[] } ---------------------------------
SHIM(select) {  // (nfds ignored, read, write, except, const timeval*)
    u64 sets[3] = { A2, A3, A4 };
    struct pollfd pf[3 * 1024]; int np = 0;
    static const short ev[3] = { POLLIN, POLLOUT, POLLPRI };
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        u32 n = LD32(sets[k]);
        for (u32 i = 0; i < n && np < 3 * 1024; i++) { pf[np].fd = FD(LD64(sets[k] + 8 + 8ull * i)); pf[np].events = ev[k]; pf[np].revents = 0; np++; }
    }
    int ms = -1;
    if (AN(5)) { i32 sec = (i32)LD32(AN(5)), usec = (i32)LD32(AN(5) + 4); ms = sec * 1000 + usec / 1000; }
    int r = poll(pf, (nfds_t)np, ms);
    if (r < 0) RET(fail());
    int total = 0, idx = 0;
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        u32 n = LD32(sets[k]), out = 0;
        for (u32 i = 0; i < n; i++, idx++) {
            short re = pf[idx].revents;
            int ready = k == 0 ? (re & (POLLIN | POLLHUP | POLLERR)) != 0 : k == 1 ? (re & (POLLOUT | POLLERR | POLLHUP)) != 0 : (re & POLLPRI) != 0;
            if (k == 1 && (re & POLLERR)) ready = 0;     // a failed connect is reported in exceptfds, not writefds
            if (k == 2 && (pf[idx].revents & POLLERR)) ready = 1;
            if (ready) ST64(sets[k] + 8 + 8ull * out++, LD64(sets[k] + 8 + 8ull * i));
        }
        ST32(sets[k], out); total += (int)out;
    }
    RET((u32)total);
}
SHIM(__WSAFDIsSet) { u32 n = LD32(A2); for (u32 i = 0; i < n; i++) if (LD64(A2 + 8 + 8ull * i) == A1) RET(1); RET(0); }
SHIM(WSAPoll) {  // (WSAPOLLFD { SOCKET; i16 events; i16 revents } *, n, timeout): same flag bits as POSIX for the common ones
    u32 n = (u32)A2; struct pollfd *pf = calloc(n ? n : 1, sizeof *pf);
    for (u32 i = 0; i < n; i++) {
        i16 e = (i16)LD16(A1 + 16ull * i + 8);
        pf[i].fd = FD(LD64(A1 + 16ull * i));
        pf[i].events = (short)((e & 0x0300 ? POLLIN : 0) | (e & 0x0010 ? POLLOUT : 0) | (e & 0x0400 ? POLLPRI : 0));  // POLLRDNORM|RDBAND, WRNORM, PRI
    }
    int r = poll(pf, n, (int)(i32)A3);
    for (u32 i = 0; i < n; i++) {
        short re = pf[i].revents;
        ST16(A1 + 16ull * i + 10, (u16)((re & POLLIN ? 0x0100 : 0) | (re & POLLOUT ? 0x0010 : 0) | (re & POLLERR ? 0x0001 : 0) |
                                        (re & POLLHUP ? 0x0002 : 0) | (re & POLLNVAL ? 0x0004 : 0)));
    }
    free(pf);
    RET(r < 0 ? fail() : (u64)(u32)r);
}

// ---- names and addresses -------------------------------------------------------------------------
// Windows ADDRINFOA: { int flags, family, socktype, protocol; size_t addrlen; char *canonname; sockaddr *addr; ADDRINFOA *next }
static u64 make_ai(const struct addrinfo *a, int wide) {
    u8 *w = calloc(1, 48 + 28);
    ST32((u64)w, (u32)a->ai_flags); ST32((u64)w + 4, fam_to_win(a->ai_family)); ST32((u64)w + 8, (u32)a->ai_socktype); ST32((u64)w + 12, (u32)a->ai_protocol);
    u64 sa = (u64)w + 48; u32 len = 28;
    to_win(a->ai_addr, sa, (u64)&len);
    ST64((u64)w + 16, len); ST64((u64)w + 32, sa);
    if (a->ai_canonname) {
        if (wide) { size_t n = strlen(a->ai_canonname); u16 *c = calloc(n + 1, 2); for (size_t i = 0; i < n; i++) c[i] = (u8)a->ai_canonname[i]; ST64((u64)w + 24, (u64)c); }
        else ST64((u64)w + 24, (u64)strdup(a->ai_canonname));
    }
    return (u64)w;
}
static u32 getaddrinfo_common(const char *node, const char *service, u64 hints, u64 out, int wide) {
    struct addrinfo h = { 0 }, *res = NULL;
    if (hints) {
        h.ai_flags = (int)LD32(hints) & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);   // same bit values for these
        if (LD32(hints) & 0x00000400) h.ai_flags |= AI_ADDRCONFIG;
        h.ai_family = fam_to_host(LD32(hints + 4)); if (h.ai_family < 0) return 10047;
        h.ai_socktype = (int)LD32(hints + 8); h.ai_protocol = (int)LD32(hints + 12);
    }
    int r = getaddrinfo(node, service, hints ? &h : NULL, &res);
    if (r) {
        ST64(out, 0);
        return r == EAI_NONAME ? 11001 : r == EAI_AGAIN ? 11002 : r == EAI_FAMILY ? 10047 : r == EAI_SERVICE ? 10109 : r == EAI_MEMORY ? 8 : 11003;
    }
    u64 head = 0, tail = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
        u64 n = make_ai(a, wide);
        if (!head) head = n; else ST64(tail + 40, n);
        tail = n;
    }
    freeaddrinfo(res);
    ST64(out, head);
    return head ? 0 : 11001;   // WSAHOST_NOT_FOUND
}
static char *from_w(u64 w) { if (!w) return NULL; size_t n = 0; while (LD16(w + 2 * n)) n++; char *s = malloc(n + 1); for (size_t i = 0; i < n; i++) s[i] = (char)LD16(w + 2 * i); s[n] = 0; return s; }
SHIM(getaddrinfo) { u32 r = getaddrinfo_common((const char *)A1, (const char *)A2, A3, A4, 0); wsa_err = r; RET(r); }
SHIM(GetAddrInfoW) { char *n = from_w(A1), *s = from_w(A2); u32 r = getaddrinfo_common(n, s, A3, A4, 1); free(n); free(s); wsa_err = r; RET(r); }
static void free_ai(u64 a) { while (a) { u64 next = LD64(a + 40); free((void *)LD64(a + 24)); free((void *)a); a = next; } }
SHIM(freeaddrinfo) { free_ai(A1); RET(0); }
SHIM(FreeAddrInfoW) { free_ai(A1); RET(0); }

// getnameinfo(sa, salen, host, hostlen, serv, servlen, flags): NI_* flags share values for the common ones
SHIM(getnameinfo) {
    struct sockaddr_storage h; socklen_t n = to_host(A1, (i32)A2, &h);
    if (!n) RET(10047);
    int r = getnameinfo((struct sockaddr *)&h, n, (char *)A3, (socklen_t)(u32)A4, (char *)AN(5), (socklen_t)(u32)AN(6), (int)(u32)AN(7));
    RET(r ? 11001 : 0);
}

// hostent (Windows x64): { char *h_name; char **h_aliases; short h_addrtype; short h_length; char **h_addr_list }
static __thread u8 he_buf[512];
static u64 make_hostent(const char *name, const struct in_addr *addrs, int n) {
    u8 *p = he_buf; memset(p, 0, sizeof he_buf);
    char *nm = (char *)p + 64; strncpy(nm, name, 127);
    u64 *aliases = (u64 *)(p + 192);
    u64 *list = (u64 *)(p + 200);
    u8 *data = p + 280;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) { memcpy(data + 4 * i, &addrs[i], 4); list[i] = (u64)(data + 4 * i); }
    ST64((u64)p, (u64)nm); ST64((u64)p + 8, (u64)aliases); ST16((u64)p + 16, W_AF_INET); ST16((u64)p + 18, 4); ST64((u64)p + 24, (u64)list);
    return (u64)p;
}
SHIM(gethostbyname) {
    struct addrinfo h = { 0 }, *res = NULL; h.ai_family = AF_INET;
    if (getaddrinfo((const char *)A1, NULL, &h, &res) || !res) { wsa_err = 11001; RET(0); }
    struct in_addr a[8]; int n = 0;
    for (struct addrinfo *r = res; r && n < 8; r = r->ai_next) {
        struct in_addr x = ((struct sockaddr_in *)r->ai_addr)->sin_addr; int dup = 0;
        for (int i = 0; i < n; i++) if (a[i].s_addr == x.s_addr) dup = 1;
        if (!dup) a[n++] = x;
    }
    freeaddrinfo(res);
    RET(make_hostent((const char *)A1, a, n));
}
SHIM(gethostname) { RET(gethostname((char *)A1, (size_t)(i32)A2) ? fail() : 0); }
SHIM(inet_addr) { struct in_addr a; RET(inet_aton((const char *)A1, &a) ? a.s_addr : 0xFFFFFFFFu); }
SHIM(inet_ntoa) { static __thread char b[16]; struct in_addr a = { (u32)A1 }; strncpy(b, inet_ntoa(a), 15); RET(b); }
SHIM(inet_pton) { int f = fam_to_host((u32)A1); if (f <= 0) { wsa_err = 10047; RET((u64)-1); } RET((u32)inet_pton(f, (const char *)A2, (void *)A3)); }
SHIM(inet_ntop) { int f = fam_to_host((u32)A1); if (f <= 0) { wsa_err = 10047; RET(0); } RET(inet_ntop(f, (void *)A2, (char *)A3, (socklen_t)A4)); }
SHIM(htons) { RET(htons((u16)A1)); }
SHIM(ntohs) { RET(ntohs((u16)A1)); }
SHIM(htonl) { RET(htonl((u32)A1)); }
SHIM(ntohl) { RET(ntohl((u32)A1)); }
SHIM(WSAIoctl) {   // (s, code, in, inlen, out, outlen, *bytesReturned, overlapped, completion)
    u32 code = (u32)A2;
    if (code == 0x9800000Cu || code == 0x9800000Fu) {   // SIO_UDP_CONNRESET / SIO_UDP_NETRESET: Windows-only UDP quirks, nothing to do (AnyPS5's UDP sockets set it)
        if (AN(7)) ST32(AN(7), 0);
        RET(0);
    }
    if (code == 0x8004667Eu && A3 && (u32)A4 >= 4) {   // FIONBIO
        int fl = fcntl(FD(A1), F_GETFL, 0);
        if (fl >= 0 && fcntl(FD(A1), F_SETFL, LD32(A3) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0) { if (AN(7)) ST32(AN(7), 0); RET(0); }
    }
    wsa_err = 10045; RET(SOCKET_ERROR);
}

// Ordinal-only imports (wsock32/ws2_32 export most functions by ordinal too; games often import that way).
const char *rt_winsock_ordinal(u16 ord) {
    switch (ord) {
    case 1: return "accept"; case 2: return "bind"; case 3: return "closesocket"; case 4: return "connect"; case 5: return "getpeername";
    case 6: return "getsockname"; case 7: return "getsockopt"; case 8: return "htonl"; case 9: return "htons"; case 10: return "ioctlsocket";
    case 11: return "inet_addr"; case 12: return "inet_ntoa"; case 13: return "listen"; case 14: return "ntohl"; case 15: return "ntohs";
    case 16: return "recv"; case 17: return "recvfrom"; case 18: return "select"; case 19: return "send"; case 20: return "sendto";
    case 21: return "setsockopt"; case 22: return "shutdown"; case 23: return "socket"; case 52: return "gethostbyname";
    case 57: return "gethostname"; case 111: return "WSAGetLastError"; case 112: return "WSASetLastError"; case 115: return "WSAStartup";
    case 116: return "WSACleanup"; case 151: return "__WSAFDIsSet";
    default: return NULL;
    }
}

const Shim shims_winsock[] = {
#define E(n) { NULL, #n, sh_##n }
    E(WSAStartup), E(WSACleanup), E(WSAGetLastError), E(WSASetLastError), E(socket), E(WSASocketW), E(WSASocketA), E(closesocket),
    E(bind), E(listen), E(connect), E(accept), E(send), E(recv), E(sendto), E(recvfrom), E(shutdown), E(getsockname), E(getpeername),
    E(ioctlsocket), E(setsockopt), E(getsockopt), E(select), E(__WSAFDIsSet), E(WSAPoll), E(getaddrinfo), E(GetAddrInfoW),
    E(freeaddrinfo), E(FreeAddrInfoW), E(getnameinfo), E(gethostbyname), E(gethostname), E(inet_addr), E(inet_ntoa), E(inet_pton),
    E(inet_ntop), E(htons), E(ntohs), E(htonl), E(ntohl), E(WSAIoctl),
#undef E
    { 0, 0, 0 }
};
