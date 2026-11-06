// memcpy / memset heavy code: small variable-size copies (entity/state snapshots, string building),
// large block copies (texture/streaming buffers), struct assignment, and clears.
#include "bench.h"

typedef struct { float pos[3], rot[4], vel[3]; int id, flags; char name[24]; } Entity;   // 80 bytes

BENCH_NOINLINE static void copy_entities(Entity *dst, const Entity *src, int n) { for (int i = 0; i < n; i++) dst[i] = src[i]; }

int main(void) {
    enum { BIG = 8 << 20 };
    uint8_t *a = malloc(BIG), *b = malloc(BIG);
    uint64_t s = 3;
    for (int i = 0; i < BIG; i++) a[i] = (uint8_t)bench_rng(&s);

    double t0 = bench_now_ms();
    uint64_t h = 0;
    for (int r = 0; r < 200; r++) { a[r] = (uint8_t)r; memcpy(b, a, BIG); h += b[(r * 7919) % BIG]; }
    bench_report("memcpy_large", bench_now_ms() - t0, h);

    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 400; r++) { memset(b, r, BIG); h += b[(r * 7919) % BIG]; }
    bench_report("memset_large", bench_now_ms() - t0, h);

    // small, variable sizes (1..256 bytes), the common case in game code
    static uint16_t sizes[4096], offs[4096];
    for (int i = 0; i < 4096; i++) { sizes[i] = (uint16_t)(1 + bench_rng(&s) % 256); offs[i] = (uint16_t)(bench_rng(&s) % 60000); }
    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 1500; r++)
        for (int i = 0; i < 4096; i++) { memcpy(b + offs[i], a + offs[(i + r) & 4095], sizes[i]); h += b[offs[i]]; }
    bench_report("memcpy_small", bench_now_ms() - t0, h);

    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 1500; r++)
        for (int i = 0; i < 4096; i++) { memset(b + offs[i], (int)i, sizes[i]); h += b[offs[(i + 1) & 4095]]; }
    bench_report("memset_small", bench_now_ms() - t0, h);

    enum { NE = 16384 };
    Entity *e1 = malloc(NE * sizeof(Entity)), *e2 = malloc(NE * sizeof(Entity));
    memcpy(e1, a, NE * sizeof(Entity));
    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 3000; r++) { e1[r % NE].id = r; copy_entities(e2, e1, NE); h += (uint64_t)e2[(r * 31) % NE].id; }
    bench_report("struct_copy", bench_now_ms() - t0, h);

    // byte loop the compiler won't turn into memcpy (strlen/strcpy style)
    char *str = (char *)a; for (int i = 0; i < 65536; i++) str[i] = (char)('a' + i % 26); str[65535] = 0;
    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 4000; r++) {
        char *d = (char *)b; const char *p = str + (r & 63);
        while ((*d++ = *p++)) {}
        h += (uint64_t)(d - (char *)b);
    }
    bench_report("strcpy_loop", bench_now_ms() - t0, h);
    free(a); free(b); free(e1); free(e2);
    return 0;
}
