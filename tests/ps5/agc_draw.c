/* PS5 homebrew GPU test for the wintomac PS5 chain (docs/ps5_graphics.md).
   Loads /app0/agc_draw.bin (made by tests/ps5/gen_agc_draw.py): a raw AGC command buffer (draws + a compute
   dispatch), a data area (RDNA2 shader code, textures, vertex buffers, zero-filled buffers for depth and compute
   output) and relocations that patch addresses into both. Submits it with sceAgcDriverSubmitDcb, waits for the label
   its last packet writes, flips the 640x360 display buffer and prints a hash of each region the file asks to check.
   AnyPS5's libSceAgcDriver recompiles the shaders to SPIR-V and runs them on Vulkan (our bridge -> MoltenVK -> Metal).
   Everything GPU-side is data, so new cases need no rebuild of this program. Built with the open ps5-payload-dev SDK.

   File: "AGC2", u32 dcb_words, data_file_words, data_total_bytes, nrelocs, nchecks; dcb[]; data[];
         relocs[{u32 where, base, offset, shape}]; checks[{u32 base, offset, bytes}]
   where: word index into dcb (< dcb_words) or data (minus dcb_words). base: 0 data area, 1 display buffer, 2 label.
   shape: shift | bitpos << 8 | nbits << 16; word |= ((base + offset) >> shift & mask(nbits)) << bitpos. */
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
typedef struct { uint32_t *addr; uint32_t dw_num; uint8_t flags, reserved[3]; } AgcPacket;
int sceVideoOutOpen(int userId, int busType, int index, const void *param);
void sceVideoOutSetBufferAttribute2(VideoOutBufferAttribute2 *a, uint64_t pixelFormat, uint32_t tilingMode, uint32_t width,
                                    uint32_t height, uint64_t option, uint32_t dccControl, uint64_t dccClearColor);
int sceVideoOutRegisterBuffers2(int handle, int setIndex, int bufferIndexStart, const VideoOutBuffers *buffers, int bufferNum,
                                const VideoOutBufferAttribute2 *attribute, int category, void *option);
int sceVideoOutSubmitFlip(int handle, int index, int flipMode, int64_t flipArg);
int sceVideoOutGetFlipStatus(int handle, VideoOutFlipStatus *status);
int sceAgcDriverSubmitDcb(const AgcPacket *packet);
int sceKernelUsleep(unsigned usec);

#define W 640
#define H 360

static void *aligned(size_t n, size_t a) { uintptr_t p = (uintptr_t)calloc(1, n + a); return (void *)((p + a - 1) & ~(uintptr_t)(a - 1)); }
static uint32_t fnv(const void *p, size_t n) { const uint8_t *b = p; uint32_t h = 2166136261u; while (n--) { h ^= *b++; h *= 16777619u; } return h; }

int main(void) {
    FILE *f = fopen("/app0/agc_draw.bin", "rb");
    if (!f) { printf("no /app0/agc_draw.bin\n"); return 1; }
    uint32_t hdr[6];
    if (fread(hdr, 4, 6, f) != 6 || hdr[0] != 0x32434741u) { printf("bad agc_draw.bin (want AGC2)\n"); return 1; }
    uint32_t ndcb = hdr[1], nfile = hdr[2], total = hdr[3], nrel = hdr[4], nchk = hdr[5];
    uint32_t *dcb = aligned(ndcb * 4, 256), *data = aligned(total, 65536), *rel = malloc(nrel * 16 + 16), *chk = malloc(nchk * 12 + 12);
    volatile uint64_t *label = aligned(64, 64);
    if (fread(dcb, 4, ndcb, f) != ndcb || fread(data, 4, nfile, f) != nfile || fread(rel, 16, nrel, f) != nrel || fread(chk, 12, nchk, f) != nchk) {
        printf("short agc_draw.bin\n"); return 1;
    }
    fclose(f);

    int h = sceVideoOutOpen(255, 0, 0, NULL);
    printf("sceVideoOutOpen = %d\n", h);
    if (h < 0) return 1;
    size_t bytes = (size_t)W * H * 4;
    uint32_t *fb = aligned(bytes, 65536);
    for (size_t i = 0; i < (size_t)W * H; i++) fb[i] = 0xff202020u;   /* grey: anything not drawn stays grey */
    VideoOutBufferAttribute2 attr;
    sceVideoOutSetBufferAttribute2(&attr, 0x8000000022000000ull /* R8G8B8A8 sRGB */, 1 /* linear */, W, H, 0, 0, 0);
    VideoOutBuffers b = { fb, NULL, { 0, 0 } };
    int r = sceVideoOutRegisterBuffers2(h, 0, 0, &b, 1, &attr, 0, NULL);
    printf("sceVideoOutRegisterBuffers2 = %d\n", r);
    if (r < 0) return 2;

    uint64_t bases[3] = { (uint64_t)(uintptr_t)data, (uint64_t)(uintptr_t)fb, (uint64_t)(uintptr_t)label };
    for (uint32_t i = 0; i < nrel; i++) {
        uint32_t where = rel[4 * i], base = rel[4 * i + 1], off = rel[4 * i + 2], shape = rel[4 * i + 3];
        uint32_t shift = shape & 0xff, pos = (shape >> 8) & 0xff, nbits = (shape >> 16) & 0xff;
        uint64_t v = (bases[base < 3 ? base : 0] + off) >> shift;
        if (nbits < 32) v &= (1ull << nbits) - 1;
        uint32_t *w = where < ndcb ? &dcb[where] : &data[where - ndcb];
        *w |= (uint32_t)(v << pos);
    }
    AgcPacket p = { dcb, ndcb, 0, { 0, 0, 0 } };
    r = sceAgcDriverSubmitDcb(&p);
    printf("sceAgcDriverSubmitDcb(%u dwords) = %d\n", ndcb, r);
    int t = 0;
    for (; t < 60000 && *label != 1; t++) sceKernelUsleep(1000);
    printf("label %s\n", *label == 1 ? "written" : "NOT written (timeout)");
    r = sceVideoOutSubmitFlip(h, 0, 1, 0);
    VideoOutFlipStatus st;
    for (t = 0; t < 10000; t++) { memset(&st, 0, sizeof st); sceVideoOutGetFlipStatus(h, &st); if (st.count >= 1 && st.flipPendingNum == 0) break; sceKernelUsleep(1000); }
    printf("flip %s\n", st.count >= 1 ? "done" : "NOT done");
    for (uint32_t i = 0; i < nchk; i++) {
        const uint32_t *q = (const uint32_t *)(uintptr_t)(bases[chk[3 * i] < 3 ? chk[3 * i] : 0] + chk[3 * i + 1]);
        printf("check %u: %u bytes hash %08x words %08x %08x %08x %08x\n", i, chk[3 * i + 2], fnv(q, chk[3 * i + 2]), q[0], q[1], q[2], q[3]);
    }
    return 0;
}
