/* PS5 compat: libScePad with no controller attached: Init/Open/ReadState must fail or report disconnected, never crash. */
/* PS5_LDFLAGS: -lScePad -lSceUserService */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int sceUserServiceInitialize(const void *p); int sceUserServiceGetInitialUser(int *u);
int scePadInit(void); int scePadOpen(int user, int type, int idx, const void *param);
int scePadReadState(int h, void *state); int scePadClose(int h);
int scePadGetHandle(int user, int type, int idx);
int main(void) {
    sceUserServiceInitialize(0); int u = -1; sceUserServiceGetInitialUser(&u);
    printf("pad init=%d\n", scePadInit());
    int h = scePadOpen(u, 0, 0, 0);
    printf("open handle>=0: %d (raw %d)\n", h >= 0, h < 0 ? h : 0);
    unsigned char st[256]; memset(st, 0, sizeof st);
    int rc = scePadReadState(h, st);
    printf("readstate rc=%d connected=%d buttons=%02x%02x%02x%02x\n", rc, st[0x1f], st[3], st[2], st[1], st[0]);
    if (h >= 0) printf("close=%d\n", scePadClose(h));
    return 0;
}
