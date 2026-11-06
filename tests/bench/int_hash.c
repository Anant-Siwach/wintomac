// Integer workloads: string/buffer hashing, an open-addressing hash table, sorting, and a sieve.
#include "bench.h"

#define BUF_SIZE (1 << 20)
static uint8_t buf[BUF_SIZE];

BENCH_NOINLINE static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

BENCH_NOINLINE static uint32_t murmur3_32(const uint8_t *key, size_t len, uint32_t seed) {
    uint32_t h = seed;
    size_t nb = len / 4;
    for (size_t i = 0; i < nb; i++) {
        uint32_t k; memcpy(&k, key + i * 4, 4);
        k *= 0xcc9e2d51; k = (k << 15) | (k >> 17); k *= 0x1b873593;
        h ^= k; h = (h << 13) | (h >> 19); h = h * 5 + 0xe6546b64;
    }
    h ^= (uint32_t)len; h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13; h *= 0xc2b2ae35; h ^= h >> 16;
    return h;
}

// open addressing, linear probing
#define TBL_BITS 16
typedef struct { uint64_t key, val; } Slot;
static Slot table[1 << TBL_BITS];

static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return x;
}

BENCH_NOINLINE static void tbl_put(uint64_t k, uint64_t v) {
    uint64_t i = mix64(k) & ((1 << TBL_BITS) - 1);
    while (table[i].key && table[i].key != k) i = (i + 1) & ((1 << TBL_BITS) - 1);
    table[i].key = k; table[i].val = v;
}

BENCH_NOINLINE static uint64_t tbl_get(uint64_t k) {
    uint64_t i = mix64(k) & ((1 << TBL_BITS) - 1);
    while (table[i].key) { if (table[i].key == k) return table[i].val; i = (i + 1) & ((1 << TBL_BITS) - 1); }
    return 0;
}

static void quicksort(uint32_t *a, long lo, long hi) {
    while (lo < hi) {
        uint32_t p = a[(lo + hi) / 2];
        long i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { uint32_t t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { quicksort(a, lo, j); lo = i; } else { quicksort(a, i, hi); hi = j; }
    }
}

int main(void) {
    uint64_t s = 42;
    for (int i = 0; i < BUF_SIZE; i++) buf[i] = (uint8_t)bench_rng(&s);

    double t0 = bench_now_ms();
    uint64_t h = 0;
    for (int r = 0; r < 100; r++) { buf[r] ^= 1; h ^= fnv1a(buf, BUF_SIZE); }
    bench_report("hash_fnv1a", bench_now_ms() - t0, h);

    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 250; r++) { buf[r] ^= 1; h += murmur3_32(buf, BUF_SIZE, (uint32_t)r); }
    bench_report("hash_murmur3", bench_now_ms() - t0, h);

    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 60; r++) {
        memset(table, 0, sizeof table);
        for (uint64_t k = 1; k <= 40000; k++) tbl_put(k * 2654435761ull + (uint64_t)r, k);
        for (uint64_t k = 1; k <= 80000; k++) h += tbl_get(k * 2654435761ull + (uint64_t)r);
    }
    bench_report("hashtable", bench_now_ms() - t0, h);

    enum { N = 1 << 20 };
    uint32_t *a = malloc(N * sizeof *a);
    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < N; i++) a[i] = (uint32_t)bench_rng(&s);
        quicksort(a, 0, N - 1);
        for (int i = 0; i < N; i += 1024) h = h * 31 + a[i];
    }
    bench_report("quicksort", bench_now_ms() - t0, h);
    free(a);

    enum { SN = 20000000 };
    uint8_t *comp = malloc(SN + 1);
    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 2; r++) {
        memset(comp, 0, SN + 1);
        for (long i = 2; i * i <= SN; i++) if (!comp[i]) for (long j = i * i; j <= SN; j += i) comp[j] = 1;
        for (long i = 2; i <= SN; i++) h += !comp[i];
    }
    bench_report("sieve", bench_now_ms() - t0, h);
    free(comp);
    return 0;
}
