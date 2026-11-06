/* PS5 homebrew input test for the wintomac PS5 chain (docs/ps5_graphics.md).
   Opens video out (AnyPS5's pad emulation lives in libSceVideoOut's window), flips a grey frame at ~60 Hz for
   PAD_SECONDS (default 5) seconds and prints every change of scePadReadState: buttons, sticks, analog triggers,
   connected. Headless checks: WINTOMAC_VIRTUAL_PAD=1 (xinput.c: a scripted SDL virtual gamepad) and
   WINTOMAC_TEST_DETACHED_KEYS=1 (user32.c: Return, Up, W typed into the window -> Cross, D-pad up, left stick up).
   Built with the open ps5-payload-dev SDK. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t reserved0, tiling_mode, aspect_ratio, width, height, pitch_in_pixel;
    uint64_t option, pixel_format, dcc_cb_register_clear_color;
    uint32_t dcc_control, pad0;
    uint64_t reserved1[3];
} VideoOutBufferAttribute2;
typedef struct { const void *data, *metadata, *reserved[2]; } VideoOutBuffers;
typedef struct { uint64_t count, processTime, reserved0; int64_t flipArg; uint64_t reserved1, processTimeCounter;
                 int32_t gcQueueNum, flipPendingNum, currentBuffer; uint32_t reserved2; uint64_t submitTsc, reserved3[7]; } VideoOutFlipStatus;
int sceVideoOutOpen(int userId, int busType, int index, const void *param);
void sceVideoOutSetBufferAttribute2(VideoOutBufferAttribute2 *a, uint64_t pixelFormat, uint32_t tilingMode, uint32_t width,
                                    uint32_t height, uint64_t option, uint32_t dccControl, uint64_t dccClearColor);
int sceVideoOutRegisterBuffers2(int handle, int setIndex, int bufferIndexStart, const VideoOutBuffers *buffers, int bufferNum,
                                const VideoOutBufferAttribute2 *attribute, int category, void *option);
int sceVideoOutSubmitFlip(int handle, int index, int flipMode, int64_t flipArg);
int sceVideoOutGetFlipStatus(int handle, VideoOutFlipStatus *status);
int sceUserServiceInitialize(const void *p);
int sceUserServiceGetInitialUser(int *u);
int scePadInit(void);
int scePadOpen(int user, int type, int idx, const void *param);
int scePadReadState(int h, void *state);
int sceKernelUsleep(unsigned usec);

#define W 320
#define H 180

int main(void) {
    const char *secs = getenv("PAD_SECONDS");
    int frames = (secs ? atoi(secs) : 5) * 60;
    int h = sceVideoOutOpen(255, 0, 0, NULL);
    if (h < 0) { printf("sceVideoOutOpen = %d\n", h); return 1; }
    uintptr_t raw = (uintptr_t)malloc((size_t)W * H * 4 + 65536);
    uint32_t *fb = (uint32_t *)((raw + 65535) & ~(uintptr_t)65535);
    for (int i = 0; i < W * H; i++) fb[i] = 0xff404040u;
    VideoOutBufferAttribute2 attr;
    sceVideoOutSetBufferAttribute2(&attr, 0x8000000000000000ull, 1, W, H, 0, 0, 0);
    VideoOutBuffers b = { fb, NULL, { 0, 0 } };
    if (sceVideoOutRegisterBuffers2(h, 0, 0, &b, 1, &attr, 0, NULL) < 0) { printf("register failed\n"); return 2; }
    sceUserServiceInitialize(0);
    int user = -1; sceUserServiceGetInitialUser(&user);
    printf("scePadInit = %d\n", scePadInit());
    int pad = scePadOpen(user, 0, 0, 0);
    printf("scePadOpen: handle %s\n", pad >= 0 ? "ok" : "failed");
    uint8_t st[256], last[16];
    memset(last, 0xff, sizeof last);
    for (int f = 0; f < frames; f++) {
        VideoOutFlipStatus fs;
        for (int t = 0; t < 200; t++) { memset(&fs, 0, sizeof fs); sceVideoOutGetFlipStatus(h, &fs); if (fs.flipPendingNum == 0) break; sceKernelUsleep(1000); }
        sceVideoOutSubmitFlip(h, 0, 1, f);
        memset(st, 0, sizeof st);
        int rc = pad >= 0 ? scePadReadState(pad, st) : -1;
        uint8_t now[16] = { st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8], st[9], st[76], (uint8_t)(rc != 0) };
        if (memcmp(now, last, 12) != 0) {
            uint32_t buttons; memcpy(&buttons, st, 4);
            printf("t=%4dms pad buttons=%06x left=%3u,%3u right=%3u,%3u l2=%3u r2=%3u connected=%u rc=%d\n",
                   f * 1000 / 60, buttons, st[4], st[5], st[6], st[7], st[8], st[9], st[76], rc);
            memcpy(last, now, 12);
        }
        sceKernelUsleep(16000);
    }
    printf("pad test done\n");
    return 0;
}
