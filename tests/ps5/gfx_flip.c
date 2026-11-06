/* PS5 homebrew drawing test for the wintomac PS5 chain (docs/ps5_graphics.md).
   Opens video out, registers two linear 8-bit BGRA display buffers, draws a deterministic test pattern on the CPU
   into them and flips. AnyPS5's libSceVideoOut hands each flip to libSceAgcDriver, which uploads the buffer and
   presents it through Vulkan (here: our Vulkan bridge -> MoltenVK -> Metal). APS5_DUMP_FRAMES=n APS5_DUMP_SCALE=1
   makes AnyPS5 read the presented frames back into frame_<i>.bmp. Built with the open ps5-payload-dev SDK. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libSceVideoOut (prototypes as implemented by AnyPS5, core/libs/prx/libSceVideoOut) */
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
int sceVideoOutClose(int handle);
void sceVideoOutSetBufferAttribute2(VideoOutBufferAttribute2 *a, uint64_t pixelFormat, uint32_t tilingMode, uint32_t width,
                                    uint32_t height, uint64_t option, uint32_t dccControl, uint64_t dccClearColor);
int sceVideoOutRegisterBuffers2(int handle, int setIndex, int bufferIndexStart, const VideoOutBuffers *buffers, int bufferNum,
                                const VideoOutBufferAttribute2 *attribute, int category, void *option);
int sceVideoOutSubmitFlip(int handle, int index, int flipMode, int64_t flipArg);
int sceVideoOutGetFlipStatus(int handle, VideoOutFlipStatus *status);
int sceKernelUsleep(unsigned usec);

#define W 640
#define H 360
#define FRAMES 3
#define PIXEL_FORMAT_B8G8R8A8_SRGB 0x8000000000000000ull  /* "A8R8G8B8_SRGB": 32-bit words 0xAARRGGBB */
#define TILING_LINEAR 1

static void draw(uint32_t *px, int frame) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t r = x * 255 / (W - 1), g = y * 255 / (H - 1), b = (frame * 80) & 255;
            px[y * W + x] = 0xff000000u | r << 16 | g << 8 | b;
        }
    /* a white square that moves 40 px per frame, and a red bar */
    for (int y = 100; y < 180; y++) for (int x = 40 + frame * 40; x < 120 + frame * 40; x++) px[y * W + x] = 0xffffffffu;
    for (int y = 300; y < 330; y++) for (int x = 200; x < 440; x++) px[y * W + x] = 0xffff0000u;
}

static uint32_t fnv(const void *p, size_t n) {
    const uint8_t *b = p; uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

int main(void) {
    /* GFX_FLIP_SECONDS=n: keep animating (60 flips a second) for n seconds, to watch it in a window */
    const char *secs = getenv("GFX_FLIP_SECONDS");
    int frames = secs ? atoi(secs) * 60 : FRAMES;
    if (frames < FRAMES) frames = FRAMES;
    int h = sceVideoOutOpen(255, 0, 0, NULL);
    printf("sceVideoOutOpen = %d\n", h);
    if (h < 0) return 1;
    size_t bytes = (size_t)W * H * 4;
    uint32_t *buf[2];
    for (int i = 0; i < 2; i++) {
        uintptr_t p = (uintptr_t)malloc(bytes + 65536);
        buf[i] = (uint32_t *)((p + 65535) & ~(uintptr_t)65535);  /* display buffers must be 64 KiB aligned */
        memset(buf[i], 0, bytes);
    }
    VideoOutBufferAttribute2 attr;
    sceVideoOutSetBufferAttribute2(&attr, PIXEL_FORMAT_B8G8R8A8_SRGB, TILING_LINEAR, W, H, 0, 0, 0);
    VideoOutBuffers bufs[2] = { { buf[0], NULL, { 0, 0 } }, { buf[1], NULL, { 0, 0 } } };
    int r = sceVideoOutRegisterBuffers2(h, 0, 0, bufs, 2, &attr, 0, NULL);
    printf("sceVideoOutRegisterBuffers2 = %d\n", r);
    if (r < 0) return 2;
    for (int f = 0; f < frames; f++) {
        int i = f & 1;
        /* wait until the buffer is no longer on screen / queued */
        VideoOutFlipStatus st;
        for (int t = 0; t < 5000; t++) { memset(&st, 0, sizeof st); sceVideoOutGetFlipStatus(h, &st); if (st.flipPendingNum == 0) break; sceKernelUsleep(1000); }
        draw(buf[i], f < FRAMES ? f : f % 13);
        if (f < FRAMES) printf("frame %d: buffer %d hash %08x\n", f, i, fnv(buf[i], bytes));
        r = sceVideoOutSubmitFlip(h, i, 1, f);
        if (r < 0) { printf("sceVideoOutSubmitFlip = %d\n", r); return 3; }
    }
    VideoOutFlipStatus st;
    for (int t = 0; t < 10000; t++) { memset(&st, 0, sizeof st); sceVideoOutGetFlipStatus(h, &st); if (st.count >= (uint64_t)frames && st.flipPendingNum == 0) break; sceKernelUsleep(1000); }
    printf("flips done: %s\n", st.count >= (uint64_t)frames ? "all" : "NOT all");
    sceVideoOutClose(h);
    return 0;
}
