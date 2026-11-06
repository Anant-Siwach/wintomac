/* PS5 compat: libSceAudioOut: open a port, write 10 blocks of a sine wave, close. Run with SDL_AUDIO_DRIVER=dummy (silent). */
/* PS5_LDFLAGS: -lSceAudioOut -lSceUserService */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
int sceUserServiceInitialize(const void *p); int sceUserServiceGetInitialUser(int *u);
int sceAudioOutInit(void); int sceAudioOutOpen(int user, int type, int idx, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int h, const void *p); int sceAudioOutClose(int h);
int sceAudioOutSetVolume(int h, int flag, int *vol);
int main(void) {
    printf("user init=%d\n", sceUserServiceInitialize(0));
    int u = -1; printf("initial user rc=%d\n", sceUserServiceGetInitialUser(&u));
    printf("audio init=%d\n", sceAudioOutInit());
    int h = sceAudioOutOpen(u, 0, 0, 256, 48000, 1 /* S16 stereo */);
    printf("open ok=%d\n", h > 0);
    static int16_t buf[256 * 2];
    int total = 0;
    for (int blk = 0; blk < 10; blk++) {
        for (int i = 0; i < 256; i++) { int16_t s = (int16_t)(sin((blk * 256 + i) * 2 * M_PI * 440 / 48000) * 8000); buf[2 * i] = buf[2 * i + 1] = s; }
        int r = sceAudioOutOutput(h, buf); if (r >= 0) total++;
    }
    printf("blocks written ok=%d\n", total);
    printf("close=%d\n", sceAudioOutClose(h));
    return 0;
}
