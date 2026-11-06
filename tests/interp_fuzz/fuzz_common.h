// Shared by x86_runner.c (native x86-64 under Rosetta 2) and interp_runner.c (runtime/interp*.c on arm64):
// identical pseudo-random machine states, identical snippet layout, identical output format.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define SCRATCH 0x30000000000ull   // fixed address of the scratch page in both processes (same pointer values)
#define SCRATCH_SIZE 0x2000
#define NSTATES 12

typedef struct { uint64_t r[16]; uint8_t x[16][16]; uint8_t xh[16][16]; uint64_t flags; } FState;

static uint64_t rng_s;
static uint64_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s; }

static uint64_t interesting(void) {
    static const uint64_t v[] = {0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff,
                                 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000ull, 0x7fffffffffffffffull,
                                 0x8000000000000000ull, 0xffffffffffffffffull, 0xfffffffffffffffeull};
    uint64_t k = rnd();
    switch (k % 4) {
    case 0: return v[(k >> 8) % (sizeof v / sizeof v[0])];
    case 1: return rnd() & 0xff;
    case 2: return (uint64_t)-(int64_t)(rnd() & 0xffff);
    default: return rnd();
    }
}

static void fill_lane(uint8_t *x) {
    uint64_t k = rnd();
    float f[4]; double d[2];
    switch (k % 5) {
    case 0: for (int i = 0; i < 4; i++) f[i] = (float)((int64_t)(rnd() % 20001) - 10000) / 64.0f; memcpy(x, f, 16); break;
    case 1: for (int i = 0; i < 2; i++) d[i] = (double)((int64_t)(rnd() % 2000001) - 1000000) / 1024.0; memcpy(x, d, 16); break;
    case 2: for (int i = 0; i < 16; i++) x[i] = (uint8_t)interesting(); break;
    case 3: for (int i = 0; i < 4; i++) { uint32_t u = (uint32_t)interesting(); memcpy(x + 4 * i, &u, 4); } break;
    default: for (int i = 0; i < 2; i++) { uint64_t u = rnd(); memcpy(x + 8 * i, &u, 8); } break;
    }
}

static void make_state(FState *s, int ci, int si, uint8_t *scratch) {
    rng_s = 0x9E3779B97F4A7C15ull ^ ((uint64_t)ci * 1000003u + (uint64_t)si * 7919u + 1);
    for (int i = 0; i < 20; i++) rnd();
    for (int i = 0; i < 16; i++) s->r[i] = interesting();
    s->r[4] = 0;                              // rsp: not loaded (each runner uses its own stack)
    s->r[3] = SCRATCH + 0x100;               // rbx, rsi, rdi: pointers into the scratch page
    s->r[6] = SCRATCH + 0x800;
    s->r[7] = SCRATCH + 0x1000;
    for (int i = 0; i < 16; i++) { fill_lane(s->x[i]); fill_lane(s->xh[i]); }
    uint64_t f = rnd();
    s->flags = 0x202 | (f & 0x8D5);            // random CF PF AF ZF SF OF, DF clear
    for (int i = 0; i < SCRATCH_SIZE; i += 8) { uint64_t v = (i >= 0x800 + 0x400 && i < 0x800 + 0x440) ? 0 : interesting(); memcpy(scratch + i, &v, 8); }
    double dv[2] = {(double)((int64_t)(rnd() % 20001) - 10000) / 16.0, (double)((int64_t)(rnd() % 2001) - 1000) / 8.0};
    memcpy(scratch + 0x800 + 0x400, dv, 16);   // x87 cases read doubles / ints here
    int32_t iv = (int32_t)(rnd() % 100000) - 50000; int64_t lv = (int64_t)(rnd() % 1000000) - 500000;
    memcpy(scratch + 0x800 + 0x418, &iv, 4); memcpy(scratch + 0x800 + 0x420, &lv, 8);
}

static uint64_t fnv(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }

static void print_state(int ci, int si, const char *name, const FState *s, uint64_t flags, uint64_t mask, const uint8_t *scratch) {
    printf("%d.%d %s:", ci, si, name);
    for (int i = 0; i < 16; i++) if (i != 4) printf(" %llx", (unsigned long long)s->r[i]);
    printf(" fl=%llx", (unsigned long long)(flags & (mask | 0x400)));
    for (int i = 0; i < 16; i++) {
        printf(" x%d=", i);
        for (int b = 15; b >= 0; b--) printf("%02x", s->x[i][b]);
        printf(":");
        for (int b = 15; b >= 0; b--) printf("%02x", s->xh[i][b]);
    }
    printf(" mem=%llx\n", (unsigned long long)fnv(scratch, SCRATCH_SIZE));
}

// Snippet layout in an executable (or, for the interpreter, any readable) page:
//   <case bytes> ; pushfq ; pop qword [rip + flagslot] ; ret          flagslot = page + 0xF00
static int build_snippet(uint8_t *page, const uint8_t *code, int n) {
    memcpy(page, code, n);
    page[n] = 0x9C;
    page[n + 1] = 0x8F; page[n + 2] = 0x05;
    int32_t disp = (int32_t)(0xF00 - (n + 7));
    memcpy(page + n + 3, &disp, 4);
    page[n + 7] = 0xC3;
    return n + 8;
}

#define CODEPAGE (SCRATCH + 0x100000)   // snippets run at the same address in both processes (rip-relative cases)
static uint8_t *map_code(int prot) {
    void *p = mmap((void *)CODEPAGE, 4096, prot, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p != (void *)CODEPAGE) { perror("mmap code"); exit(1); }
    return p;
}
static void *map_scratch(void) {
    void *p = mmap((void *)SCRATCH, SCRATCH_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p != (void *)SCRATCH) { perror("mmap scratch"); exit(1); }
    return p;
}

typedef struct { uint8_t code[64]; int n; uint64_t mask; char name[160]; } Case;
static int load_cases(const char *path, Case **out) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    int cap = 4096, n = 0;
    Case *v = calloc(cap, sizeof *v);
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char hex[256], *p = line; unsigned long long m;
        if (sscanf(p, "%255s %llx", hex, &m) != 2) continue;
        Case *c = &v[n++];
        c->n = (int)strlen(hex) / 2;
        for (int i = 0; i < c->n; i++) sscanf(hex + 2 * i, "%2hhx", &c->code[i]);
        c->mask = m;
        p = strchr(p, ' '); p = strchr(p + 1, ' ') + 1;
        p[strcspn(p, "\n")] = 0;
        snprintf(c->name, sizeof c->name, "%s", p);
    }
    fclose(f);
    *out = v;
    return n;
}
