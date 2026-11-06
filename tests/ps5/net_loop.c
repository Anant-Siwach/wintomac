/* PS5 compat: networking basics over loopback only: BSD sockets (libkernel) TCP + UDP, getsockname, poll. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

static int lfd; static struct sockaddr_in addr;
static void *server(void *a) {
    (void)a; int c = accept(lfd, 0, 0); char b[64]; int n = read(c, b, sizeof b);
    for (int i = 0; i < n; i++) if (b[i] >= 'a' && b[i] <= 'z') b[i] -= 32;
    write(c, b, n); close(c); return 0;
}
int main(void) {
    lfd = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr); addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
    printf("bind=%d\n", bind(lfd, (void *)&addr, sizeof addr));
    socklen_t l = sizeof addr; getsockname(lfd, (void *)&addr, &l);
    printf("port assigned=%d\n", ntohs(addr.sin_port) != 0);
    printf("listen=%d\n", listen(lfd, 4));
    pthread_t t; pthread_create(&t, 0, server, 0);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    printf("connect=%d\n", connect(c, (void *)&addr, sizeof addr));
    write(c, "hello loopback", 14); char b[64] = {0}; int n = read(c, b, sizeof b - 1);
    printf("echo(%d)=%s\n", n, b); close(c); pthread_join(t, 0); close(lfd);
    /* UDP */
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a1 = addr; a1.sin_port = 0; bind(u1, (void *)&a1, sizeof a1); l = sizeof a1; getsockname(u1, (void *)&a1, &l);
    sendto(u2, "dgram", 5, 0, (void *)&a1, sizeof a1);
    struct pollfd pf = {u1, POLLIN, 0}; printf("poll=%d\n", poll(&pf, 1, 2000));
    char d[16] = {0}; n = recvfrom(u1, d, sizeof d, 0, 0, 0); printf("udp(%d)=%s\n", n, d);
    char s[32]; inet_ntop(AF_INET, &a1.sin_addr, s, sizeof s); printf("addr=%s\n", s);
    return 0;
}
