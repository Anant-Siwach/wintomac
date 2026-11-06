// JIT benchmark: the guest code of tests/src/jit_hot.c (tests/jit/jit_hot.s) in a VirtualAlloc buffer, timed per
// kernel. Built for Windows (run by wintomac: interpreter or JIT) and as an x86_64 macOS binary (Rosetta 2).
// Prints "BENCH <kernel> <ms> <checksum>" lines; tools/bench_jit.sh runs the variants and tabulates them.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static double now_ms(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return 1000.0 * (double)c.QuadPart / (double)f.QuadPart; }
static void *alloc_exec(size_t n) { return VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }
#else
#include <sys/mman.h>
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static void *alloc_exec(size_t n) { void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0); return p == MAP_FAILED ? NULL : p; }
#endif
#include "../src/jit_hot_code.h"

#define MSABI __attribute__((ms_abi))
typedef uint64_t (MSABI *fn_n)(uint64_t);
typedef uint64_t (MSABI *fn_pn)(void *, uint64_t);
typedef uint64_t (MSABI *fn_cb)(uint64_t (MSABI *)(uint64_t), uint64_t);
static uint8_t *code;
#define F(off, type) ((type)(code + (off)))
static uint64_t MSABI cb(uint64_t x) { return x * 3 + 1; }

static double scale = 1.0;
#define RUN(name, expr) do { double t0 = now_ms(); uint64_t r = (expr); printf("BENCH %s %.1f %llx\n", name, now_ms() - t0, (unsigned long long)r); fflush(stdout); } while (0)

int main(int argc, char **argv) {
    if (argc > 1) scale = atof(argv[1]);
    code = alloc_exec(65536);
    memcpy(code, jit_hot_code, sizeof jit_hot_code);
    static uint8_t buf[1200];
    static float fpv[4] = {1.5f, 2.25f, 3.0f, 0.75f};
    static uint64_t cells[3];
    uint64_t n = (uint64_t)(20000000 * scale);
    RUN("int_loop", F(OFF_INT_LOOP, fn_n)(n));
    RUN("mem_loop", F(OFF_MEM_LOOP, fn_pn)(buf, n));
    RUN("fp_loop", F(OFF_FP_LOOP, fn_pn)(fpv, n / 2));
    RUN("switch_loop", F(OFF_SWITCH_LOOP, fn_n)(n));
    RUN("fib", F(OFF_FIB, fn_n)(30 + (scale >= 2)));
    RUN("callback", F(OFF_CB_LOOP, fn_cb)(cb, n / 10));
    RUN("atomics", F(OFF_ATOMIC_LOOP, fn_pn)(cells, n / 4));
    return 0;
}
