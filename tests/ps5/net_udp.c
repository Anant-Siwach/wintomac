/* PS5 compat: UDP over loopback in four variants (bound/unbound sender, non-blocking, MSG_DONTWAIT); each prints the same on a native build. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
static void variant(int c) {
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a1; memset(&a1, 0, sizeof a1); a1.sin_family = AF_INET; a1.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifndef __linux__
#ifdef __APPLE__
    a1.sin_len = sizeof a1;
#else
    a1.sin_len = sizeof a1;
#endif
#endif
    int b = bind(u1, (void *)&a1, sizeof a1); socklen_t l = sizeof a1; getsockname(u1, (void *)&a1, &l);
    if (c == 1) { struct sockaddr_in o = a1; o.sin_port = 0; bind(u2, (void *)&o, sizeof o); }
    int n = sendto(u2, "dgram", 5, 0, (void *)&a1, sizeof a1);
    usleep(100000);
    if (c == 2) fcntl(u1, F_SETFL, O_NONBLOCK);
    struct pollfd pf = {u1, POLLIN, 0}; int pr = poll(&pf, 1, 1000);
    char d[16] = {0}; int r = recvfrom(u1, d, sizeof d, c == 3 ? MSG_DONTWAIT : 0, 0, 0);
    printf("variant %d: bind=%d sendto=%d poll=%d pollin=%d recv=%d '%s'\n", c, b, n, pr, (pf.revents & POLLIN) != 0, r, d);
    close(u1); close(u2);
}
int main(void) { for (int c = 0; c < 4; c++) variant(c); return 0; }
