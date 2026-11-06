// Threads hammering the same indirect call sites (racing to fill and re-fill the per-site inline caches in
// recompiled code) and recursing through direct calls with values live in registers across them. Every thread
// computes a deterministic result.
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef unsigned (*fn_t)(unsigned, unsigned);
__attribute__((noinline)) static unsigned f0(unsigned a, unsigned b) { return a + b; }
__attribute__((noinline)) static unsigned f1(unsigned a, unsigned b) { return a * 3 + b; }
__attribute__((noinline)) static unsigned f2(unsigned a, unsigned b) { return a ^ (b * 7); }
__attribute__((noinline)) static unsigned f3(unsigned a, unsigned b) { return (a << 3) - b; }
__attribute__((noinline)) static unsigned f4(unsigned a, unsigned b) { return a + (b >> 1); }
static fn_t volatile table[5] = { f0, f1, f2, f3, f4 };

__attribute__((noinline)) static unsigned walk(unsigned depth, unsigned seed) {
    if (!depth) return seed;
    unsigned a = seed * 2654435761u, b = seed ^ depth, c = depth * 77;
    unsigned r = walk(depth - 1, a ^ b);
    return r + a + b + c;
}

__attribute__((noinline)) static unsigned dispatch(unsigned id, unsigned rounds) {
    unsigned acc = id, k = id * 31;
    for (unsigned i = 0; i < rounds; i++) {
        acc = table[(i * (id + 1)) % 5](acc, i);   // the same call site sees different targets per thread
        k += acc >> 5;
        if ((i & 255) == 0) acc += walk(20, acc);
    }
    return acc + k;
}

static unsigned results[8];
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID p) { unsigned id = (unsigned)(UINT_PTR)p; results[id] = dispatch(id, 200000); return 0; }
#else
static void *worker(void *p) { unsigned id = (unsigned)(unsigned long)p; results[id] = dispatch(id, 200000); return 0; }
#endif

int main(void) {
#ifdef _WIN32
    HANDLE h[8];
    for (int i = 0; i < 8; i++) h[i] = CreateThread(NULL, 0, worker, (LPVOID)(UINT_PTR)i, 0, NULL);
    WaitForMultipleObjects(8, h, TRUE, INFINITE);
    for (int i = 0; i < 8; i++) CloseHandle(h[i]);
#else
    pthread_t t[8];
    for (long i = 0; i < 8; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < 8; i++) pthread_join(t[i], NULL);
#endif
    for (int i = 0; i < 8; i++) printf("thread %d: %u\n", i, results[i]);
    printf("serial check: %u\n", dispatch(3, 1000));
    return 0;
}
