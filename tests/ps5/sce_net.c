/* PS5 compat: sceNet loopback TCP echo (sceNetInit/PoolCreate/Socket/Bind/Listen/Accept/Connect/Send/Recv). Hand-written expectations. */
/* PS5_LDFLAGS: -lSceNet */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
int sceNetInit(void); int sceNetTerm(void);
int sceNetPoolCreate(const char *name, int size, int flags); int sceNetPoolDestroy(int id);
int sceNetSocket(const char *name, int fam, int type, int proto); int sceNetSocketClose(int s);
int sceNetBind(int s, const void *addr, int len); int sceNetListen(int s, int backlog);
int sceNetAccept(int s, void *addr, int *len); int sceNetConnect(int s, const void *addr, int len);
int sceNetSend(int s, const void *buf, size_t len, int flags); int sceNetRecv(int s, void *buf, size_t len, int flags);
uint16_t sceNetHtons(uint16_t); int sceNetInetPton(int af, const char *src, void *dst);
int sceNetGetsockname(int s, void *addr, int *len);
static int ls;
static void *srv(void *a) {
    (void)a; int c = sceNetAccept(ls, 0, 0); char b[64]; int n = sceNetRecv(c, b, sizeof b, 0);
    for (int i = 0; i < n; i++) if (b[i] >= 'a' && b[i] <= 'z') b[i] -= 32; sceNetSend(c, b, n, 0); sceNetSocketClose(c); return 0;
}
int main(void) {
    printf("init=%d\n", sceNetInit());
    int pool = sceNetPoolCreate("wt", 64 * 1024, 0); printf("pool ok=%d\n", pool >= 0);
    ls = sceNetSocket("srv", AF_INET, SOCK_STREAM, 0); printf("socket ok=%d\n", ls >= 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_len = sizeof a; a.sin_family = AF_INET; a.sin_port = sceNetHtons(34567);
    sceNetInetPton(AF_INET, "127.0.0.1", &a.sin_addr);
    printf("bind=%d listen=%d\n", sceNetBind(ls, &a, sizeof a), sceNetListen(ls, 4));
    pthread_t t; pthread_create(&t, 0, srv, 0);
    int c = sceNetSocket("cli", AF_INET, SOCK_STREAM, 0);
    printf("connect=%d\n", sceNetConnect(c, &a, sizeof a));
    printf("send=%d\n", sceNetSend(c, "hello sceNet", 12, 0));
    char b[64] = {0}; int n = sceNetRecv(c, b, sizeof b - 1, 0); printf("recv(%d)=%s\n", n, b);
    pthread_join(t, 0); sceNetSocketClose(c); sceNetSocketClose(ls);
    printf("pool destroy=%d term=%d\n", sceNetPoolDestroy(pool), sceNetTerm());
    return 0;
}
