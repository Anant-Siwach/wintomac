/* PS5 homebrew graphics demo for the wintomac PS5 chain (docs/ps5_graphics.md): animated gradients, bouncing boxes
   and a text banner drawn on the CPU into two 1280x720 display buffers, flipped at vsync through AnyPS5's
   libSceVideoOut -> libSceAgcDriver -> Vulkan -> MoltenVK -> Metal. Runs until its window is closed (or Esc / Cmd-Q).
   DEMO_SECONDS=n stops after n seconds; DEMO_DUMP=1 prints a hash of frames 0, 60 and 120 (headless checks).
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
int sceKernelUsleep(unsigned usec);

#define W 1280
#define H 720
#define NBUF 3

/* 5x7 font, rows top to bottom, 5 bits each (bit 4 = leftmost column) */
static const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-:.!";
static const uint8_t font[][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},
    {31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},
    {17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},{17,17,10,4,4,4,4},
    {31,1,2,4,8,16,31},{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},
    {2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,2,12},{0,0,0,31,0,0,0},{0,12,12,0,12,12,0},{0,0,0,0,0,12,12},{4,4,4,4,4,0,4},
};

static void text(uint32_t *px, int x0, int y0, int s, const char *t, uint32_t col) {
    for (; *t; t++, x0 += 6 * s) {
        const char *p = strchr(chars, *t);
        if (!p || *t == 0) continue;
        const uint8_t *g = font[p - chars];
        for (int r = 0; r < 7; r++) for (int cc = 0; cc < 5; cc++) if (g[r] & (16 >> cc))
            for (int y = y0 + r * s; y < y0 + (r + 1) * s; y++) for (int x = x0 + cc * s; x < x0 + (cc + 1) * s; x++)
                if (x >= 0 && x < W && y >= 0 && y < H) px[y * W + x] = col;
    }
}
static void box(uint32_t *px, int x0, int y0, int w, int h, uint32_t col) {
    for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < H; y++) for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < W; x++) px[y * W + x] = col;
}
static int tri(int t, int n) { t %= 2 * n; return t < n ? t : 2 * n - t; }   /* 0..n..0 */

static void draw(uint32_t *px, int f) {
    int sh = (f * 3) & 511;
    for (int y = 0; y < H; y++) {
        uint32_t *row = px + y * W;
        for (int x = 0; x < W; x++) {
            int r = tri(x + sh, 255), g = tri(y * 2 + sh / 2, 255), b = tri(x + y + 2 * sh, 255);
            row[x] = 0xff000000u | (uint32_t)(r / 2) << 16 | (uint32_t)(g / 2) << 8 | (uint32_t)(b / 2 + 40);
        }
    }
    static const uint32_t cols[] = { 0xffff5050u, 0xff50ff70u, 0xff5090ffu, 0xffffd040u, 0xffff60ffu };
    for (int i = 0; i < 5; i++) {
        int sz = 70 + 20 * i;
        box(px, tri(f * (5 + i) + i * 300, W - sz), tri(f * (4 + 2 * i) + i * 170, H - sz), sz, sz, cols[i]);
    }
    const char *l1 = "PS5 HOMEBREW - NATIVE ON APPLE SILICON";
    int s = 5, tw = (int)strlen(l1) * 6 * s;
    box(px, (W - tw) / 2 - 20, 60, tw + 40, 7 * s + 40, 0xc0000000u);
    text(px, (W - tw) / 2 + 3, 83, s, l1, 0xff000000u);
    text(px, (W - tw) / 2, 80, s, l1, 0xffffffffu);
    const char *l2 = "ANYPS5 AND WINTOMAC - VULKAN ON METAL";
    int s2 = 3, tw2 = (int)strlen(l2) * 6 * s2;
    text(px, (W - tw2) / 2, H - 80, s2, l2, 0xffffffffu);
    char buf[32]; snprintf(buf, sizeof buf, "FRAME %d", f);
    text(px, 20, H - 40, 2, buf, 0xffffff80u);
}

static uint32_t fnv(const void *p, size_t n) {
    const uint8_t *b = p; uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

int main(void) {
    const char *secs = getenv("DEMO_SECONDS");
    int limit = secs ? atoi(secs) * 60 : -1;
    int dump = getenv("DEMO_DUMP") != NULL;
    int h = sceVideoOutOpen(255, 0, 0, NULL);
    if (h < 0) { printf("sceVideoOutOpen = %d\n", h); return 1; }
    size_t bytes = (size_t)W * H * 4;
    uint32_t *buf[NBUF];
    VideoOutBuffers bufs[NBUF];
    for (int i = 0; i < NBUF; i++) {
        uintptr_t p = (uintptr_t)malloc(bytes + 65536);
        buf[i] = (uint32_t *)((p + 65535) & ~(uintptr_t)65535);
        bufs[i] = (VideoOutBuffers){ buf[i], NULL, { 0, 0 } };
    }
    VideoOutBufferAttribute2 attr;
    sceVideoOutSetBufferAttribute2(&attr, 0x8000000000000000ull /* B8G8R8A8 sRGB */, 1 /* linear */, W, H, 0, 0, 0);
    if (sceVideoOutRegisterBuffers2(h, 0, 0, bufs, NBUF, &attr, 0, NULL) < 0) { printf("register failed\n"); return 2; }
    printf("PS5 demo running (%dx%d); close the window to quit\n", W, H);
    for (int f = 0; limit < 0 || f < limit; f++) {
        int i = f % NBUF;
        VideoOutFlipStatus st;
        /* at most NBUF-1 flips queued, so the buffer we draw into is not on screen */
        for (int t = 0; t < 2000; t++) { memset(&st, 0, sizeof st); sceVideoOutGetFlipStatus(h, &st); if (st.flipPendingNum < NBUF - 1) break; sceKernelUsleep(500); }
        draw(buf[i], f);
        if (dump && f % 60 == 0 && f <= 120) printf("frame %d hash %08x\n", f, fnv(buf[i], bytes));
        if (sceVideoOutSubmitFlip(h, i, 1, f) < 0) { printf("flip failed\n"); return 3; }
    }
    return 0;
}
