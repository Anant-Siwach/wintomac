// Legacy (non-VEX) SSE4.1 forms with the implicit xmm0 mask (blendvps/blendvpd/pblendvb) and phminposuw.
// Godot 4.7 (x86-64-v2 baseline) uses all of them. Reference: the same source as x86_64 under Rosetta 2.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef struct { uint8_t b[16]; } V;

static void show(const char *n, V v) {
    printf("%-10s", n);
    for (int i = 15; i >= 0; i--) printf("%02x", v.b[i]);
    printf("\n");
}

#define BLENDV(op, name)                                                                 \
    static V name(V a, V b, V m) {                                                       \
        __asm__ volatile("movdqu %1, %%xmm0\n\t"                                         \
                         "movdqu %2, %%xmm1\n\t"                                         \
                         "movdqu %3, %%xmm2\n\t"                                         \
                         "movdqa %%xmm2, %%xmm3\n\t"                                     \
                         "movdqa %%xmm0, %%xmm4\n\t"                                     \
                         "movdqa %%xmm4, %%xmm0\n\t"                                     \
                         "movdqa %%xmm1, %%xmm5\n\t"                                     \
                         op " %%xmm3, %%xmm5\n\t"                                        \
                         "movdqu %%xmm5, %0\n\t"                                         \
                         : "=m"(a) : "m"(m), "m"(a), "m"(b)                              \
                         : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5");              \
        return a;                                                                        \
    }
BLENDV("blendvps", blendvps)
BLENDV("blendvpd", blendvpd)
BLENDV("pblendvb", pblendvb)

static V blendvps_mem(V a, V b, V m) {
    __asm__ volatile("movdqu %1, %%xmm0\n\t"
                     "movdqu %2, %%xmm6\n\t"
                     "blendvps %3, %%xmm6\n\t"
                     "movdqu %%xmm6, %0\n\t"
                     : "=m"(a) : "m"(m), "m"(a), "m"(b) : "xmm0", "xmm6");
    return a;
}

static V phminposuw(V a) {
    V r;
    __asm__ volatile("movdqu %1, %%xmm7\n\t"
                     "phminposuw %%xmm7, %%xmm1\n\t"
                     "movdqu %%xmm1, %0\n\t"
                     : "=m"(r) : "m"(a) : "xmm1", "xmm7");
    return r;
}

int main(void) {
    uint32_t seed = 12345;
    for (int t = 0; t < 6; t++) {
        V a, b, m;
        for (int i = 0; i < 16; i++) {
            seed = seed * 1103515245u + 12345u; a.b[i] = (uint8_t)(seed >> 16);
            seed = seed * 1103515245u + 12345u; b.b[i] = (uint8_t)(seed >> 16);
            seed = seed * 1103515245u + 12345u; m.b[i] = (uint8_t)(seed >> 16);
        }
        printf("case %d\n", t);
        show("blendvps", blendvps(a, b, m));
        show("blendvpd", blendvpd(a, b, m));
        show("pblendvb", pblendvb(a, b, m));
        show("bvps_mem", blendvps_mem(a, b, m));
        show("phminpos", phminposuw(a));
    }
    V z; memset(&z, 0xff, sizeof z); z.b[6] = 0x10; z.b[7] = 0x00; z.b[12] = 0x10; z.b[13] = 0x00;
    show("phmin_tie", phminposuw(z));
    return 0;
}
