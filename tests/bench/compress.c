// Compression: miniz (deflate/inflate, MIT, vendored in tests/bench/miniz) on game-asset-like data.
#include "bench.h"
#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_TIME
#include "miniz/miniz.h"
#include "miniz/miniz.c"   /* single translation unit: same code in all three builds */

static const char *words[] = { "texture", "mesh", "vertex", "shader", "entity", "player", "monster", "sound",
    "level", "trigger", "light", "origin", "angle", "model", "health", "ammo", "weapon", "door", "func_",
    "info_", "target", "spawnflags", "classname", "{", "}", "\"", " ", "\n", "0", "128", "-64", "255" };

int main(void) {
    enum { N = 4 << 20 };
    uint8_t *src = malloc(N);
    uint64_t s = 5;
    size_t n = 0;
    while (n < N) {
        if (bench_rng(&s) % 8 == 0) { src[n++] = (uint8_t)bench_rng(&s); continue; }   // some noise
        const char *w = words[bench_rng(&s) % (sizeof words / sizeof *words)];
        size_t l = strlen(w);
        if (n + l > N) l = N - n;
        memcpy(src + n, w, l); n += l;
    }
    mz_ulong cap = mz_compressBound(N);
    uint8_t *cmp = malloc(cap), *out = malloc(N);

    double t0 = bench_now_ms();
    mz_ulong clen = 0;
    uint64_t h = 0;
    for (int r = 0; r < 2; r++) {
        src[r] ^= 0x55;
        clen = cap;
        if (mz_compress2(cmp, &clen, src, N, 6) != MZ_OK) { printf("compress failed\n"); return 1; }
        h = h * 31 + clen;
    }
    bench_report("deflate", bench_now_ms() - t0, h);

    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 15; r++) {
        mz_ulong olen = N;
        if (mz_uncompress(out, &olen, cmp, clen) != MZ_OK || olen != N) { printf("uncompress failed\n"); return 1; }
        h = h * 31 + out[(r * 7919) % N];
    }
    bench_report("inflate", bench_now_ms() - t0, h + (memcmp(out, src, N) == 0));

    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 20; r++) h += (uint32_t)mz_crc32((uint32_t)r, src, N) + (uint32_t)mz_adler32(1, src, N);
    bench_report("crc32_adler", bench_now_ms() - t0, h);
    return 0;
}
