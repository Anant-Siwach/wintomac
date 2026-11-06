// Winsock 2 over loopback: TCP server thread + client, UDP round trip, non-blocking connect completed via select,
// WSAECONNREFUSED, getaddrinfo, inet_pton/ntop, socket options, FIONREAD.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static SOCKET listener; static u_short port;
static DWORD WINAPI server(LPVOID a) {
    (void)a;
    struct sockaddr_in peer; int plen = sizeof peer;
    SOCKET s = accept(listener, (struct sockaddr *)&peer, &plen);
    char buf[64]; int n = recv(s, buf, sizeof buf, 0);
    for (int i = 0; i < n; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
    send(s, buf, n, 0);
    printf("server: peer family=%d len=%d loopback=%s\n", peer.sin_family, plen, peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) ? "yes" : "no");
    closesocket(s);
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    WSADATA wd; int sr = WSAStartup(MAKEWORD(2, 2), &wd);
    printf("WSAStartup: %d version=%d.%d\n", sr, LOBYTE(wd.wVersion), HIBYTE(wd.wVersion));

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL yes = TRUE; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof yes);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(listener, (struct sockaddr *)&a, sizeof a); listen(listener, 4);
    int alen = sizeof a; getsockname(listener, (struct sockaddr *)&a, &alen); port = ntohs(a.sin_port);
    int ra = 0, rl = sizeof ra; getsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (char *)&ra, &rl);
    printf("listening: port assigned=%s reuseaddr=%s\n", port ? "yes" : "no", ra ? "on" : "off");
    HANDLE th = CreateThread(NULL, 0, server, NULL, 0, NULL);

    SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
    u_long nb = 1; ioctlsocket(c, FIONBIO, &nb);
    int r = connect(c, (struct sockaddr *)&a, sizeof a);
    printf("non-blocking connect: %s\n", r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK ? "WSAEWOULDBLOCK (pending)" : "unexpected");
    fd_set wr; FD_ZERO(&wr); FD_SET(c, &wr); struct timeval tv = { 2, 0 };
    printf("select writable: %d, FD_ISSET=%d\n", select(0, NULL, &wr, NULL, &tv), FD_ISSET(c, &wr) ? 1 : 0);
    nb = 0; ioctlsocket(c, FIONBIO, &nb);
    int nd = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (char *)&nd, sizeof nd);
    send(c, "hello winsock", 13, 0);
    fd_set rd; FD_ZERO(&rd); FD_SET(c, &rd); select(0, &rd, NULL, NULL, &tv);
    Sleep(20); u_long avail = 0; ioctlsocket(c, FIONREAD, &avail);
    char buf[64] = { 0 }; int n = recv(c, buf, sizeof buf - 1, 0);
    printf("tcp echo: '%s' (%d bytes, FIONREAD said %lu)\n", buf, n, avail);
    printf("recv after peer close: %d\n", recv(c, buf, sizeof buf, 0));
    WaitForSingleObject(th, 2000); closesocket(c); closesocket(listener);

    SOCKET r1 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in dead = a; dead.sin_port = htons(port);   // listener closed: nothing there now
    printf("connect to closed port: %s\n", connect(r1, (struct sockaddr *)&dead, sizeof dead) == SOCKET_ERROR && WSAGetLastError() == WSAECONNREFUSED ? "WSAECONNREFUSED" : "unexpected");
    closesocket(r1);

    SOCKET u1 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), u2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in ua = { 0 }; ua.sin_family = AF_INET; ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(u2, (struct sockaddr *)&ua, sizeof ua); int ul = sizeof ua; getsockname(u2, (struct sockaddr *)&ua, &ul);
    sendto(u1, "ping", 4, 0, (struct sockaddr *)&ua, sizeof ua);
    struct sockaddr_in from; int fl = sizeof from; char ub[16] = { 0 };
    n = recvfrom(u2, ub, sizeof ub - 1, 0, (struct sockaddr *)&from, &fl);
    printf("udp: '%s' %d bytes from family=%d addrlen=%d\n", ub, n, from.sin_family, fl);
    closesocket(u1); closesocket(u2);

    struct addrinfo hints = { 0 }, *res = NULL; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo("localhost", "80", &hints, &res);
    char ip[64] = ""; if (!g && res) inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof ip);
    printf("getaddrinfo(localhost:80): %d -> %s port %d family=%d addrlen=%d\n", g, ip, res ? ntohs(((struct sockaddr_in *)res->ai_addr)->sin_port) : 0,
           res ? res->ai_family : 0, res ? (int)res->ai_addrlen : 0);
    if (res) freeaddrinfo(res);
    printf("unknown host: %s\n", getaddrinfo("no-such-host.invalid", NULL, &hints, &res) == WSAHOST_NOT_FOUND ? "WSAHOST_NOT_FOUND" : "other error");
    struct in6_addr a6; char s6[64];
    inet_pton(AF_INET6, "2001:db8::1", &a6); inet_ntop(AF_INET6, &a6, s6, sizeof s6);
    printf("inet_pton/ntop v6 round trip: %s, inet_addr: %08lx\n", s6, (unsigned long)ntohl(inet_addr("10.1.2.3")));
    char hn[256]; printf("gethostname: %s\n", gethostname(hn, sizeof hn) == 0 && hn[0] ? "ok" : "failed");
    WSACleanup();
    return 0;
}
