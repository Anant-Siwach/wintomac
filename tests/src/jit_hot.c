// Hot runtime-generated code: the wintomac JIT (runtime/jit*.c) translates it to arm64 once it is hot.
// Exercises hot loops in a VirtualAlloc buffer (integer, memory incl. misaligned accesses, SSE), callbacks into
// recompiled code and back, indirect jumps, recursion, string instructions handled by the interpreter inside
// translated code, self-modifying code after the code is hot (patched from C, and code patching code it is
// running), and 4 threads running the same translations (shared atomics, results checked).
// Machine code: tests/jit/jit_hot.s (bytes in jit_hot_code.h). Reference output: x86_64 macOS build under Rosetta 2.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <pthread.h>
#endif
#include "jit_hot_code.h"

#define MSABI __attribute__((ms_abi))
typedef uint64_t (MSABI *fn_n)(uint64_t);
typedef uint64_t (MSABI *fn_pn)(void *, uint64_t);
typedef uint64_t (MSABI *fn_cb)(uint64_t (MSABI *)(uint64_t), uint64_t);
typedef uint32_t (MSABI *fn_void)(void);

static uint8_t *code;
static void *alloc_exec(size_t n) {
#ifdef _WIN32
    return VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
static void flush_icache(void *p, size_t n) {
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), p, n);
#else
    (void)p; (void)n;
#endif
}

static int callbacks;
static uint64_t MSABI cb(uint64_t x) {   // C called from JIT code; calls JIT code itself every 64th time
    callbacks++;
    uint64_t r = x * 3 + 1;
    if ((x & 63) == 0) r += ((fn_n)(code + OFF_SQUARE))(x);
    return r;
}

#define F(off, type) ((type)(code + (off)))

// ---- 4 threads ----
static uint64_t cells[3];
static uint64_t ref_int, ref_switch, ref_fp;
static int thread_bad;
static float fpv[4] = {1.5f, 2.25f, 3.0f, 0.75f};
static void thread_body(int id) {
    for (int k = 0; k < 3; k++) {
        if (F(OFF_INT_LOOP, fn_n)(100000) != ref_int) thread_bad++;
        if (F(OFF_SWITCH_LOOP, fn_n)(50000) != ref_switch) thread_bad++;
        if (F(OFF_FP_LOOP, fn_pn)(fpv, 20000) != ref_fp) thread_bad++;
        if (F(OFF_ATOMIC_LOOP, fn_pn)(cells, 50000) != 50000) thread_bad++;
        // each thread writes a fresh copy of int_loop next to copies the others are running (same page:
        // translations of the shared code get invalidated while other threads execute them) and runs it
        uint8_t *copy = code + 0x8000 + 0x200 * (id * 3 + k);   // (a page of its own: write faults, then self-checking)
        memcpy(copy, jit_hot_code + OFF_INT_LOOP, OFF_MEM_LOOP - OFF_INT_LOOP);
        flush_icache(copy, OFF_MEM_LOOP - OFF_INT_LOOP);
        for (int r = 0; r < 4; r++) if (((fn_n)copy)(30000 + r) != F(OFF_INT_LOOP, fn_n)(30000 + r)) thread_bad++;
    }
}
#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) { thread_body((int)(intptr_t)p); return 0; }
#else
static void *thread_main(void *p) { thread_body((int)(intptr_t)p); return NULL; }
#endif

int main(void) {
    code = alloc_exec(65536);
    if (!code) { printf("alloc failed\n"); return 1; }
    memcpy(code, jit_hot_code, sizeof jit_hot_code);

    for (uint64_t n = 1; n <= 1000000; n *= 10) printf("int_loop(%llu) = %llx\n", (unsigned long long)n, (unsigned long long)F(OFF_INT_LOOP, fn_n)(n));

    static uint8_t buf[1200];
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (uint8_t)(i * 7 + 3);
    uint64_t m = F(OFF_MEM_LOOP, fn_pn)(buf, 300000);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < (int)sizeof buf; i++) { h ^= buf[i]; h *= 1099511628211ull; }
    printf("mem_loop = %llx buf hash = %llx\n", (unsigned long long)m, (unsigned long long)h);

    for (uint64_t n = 1; n <= 100000; n *= 10) printf("fp_loop(%llu) = %lld\n", (unsigned long long)n, (long long)F(OFF_FP_LOOP, fn_pn)(fpv, n));

    uint64_t cbr = F(OFF_CB_LOOP, fn_cb)(cb, 5000);
    printf("cb_loop = %llx, callbacks = %d\n", (unsigned long long)cbr, callbacks);
    printf("switch_loop = %llx\n", (unsigned long long)F(OFF_SWITCH_LOOP, fn_n)(1000003));
    printf("fib(27) = %llu\n", (unsigned long long)F(OFF_FIB, fn_n)(27));
    uint8_t sbuf[256];
    printf("str_loop = %llu\n", (unsigned long long)F(OFF_STR_LOOP, fn_pn)(sbuf, 3000));

    // self-modifying code after the code is hot: patched from C ...
    fn_void gc = F(OFF_GET_CONST, fn_void);
    uint64_t s = 0;
    for (int i = 0; i < 2000; i++) s += gc();
    uint32_t imm = 0x22334455;
    memcpy(code + OFF_GET_CONST + 1, &imm, 4);
    for (int i = 0; i < 2000; i++) s += gc();
    imm = 0x01020304;
    memcpy(code + OFF_GET_CONST + 1, &imm, 4);
    flush_icache(code + OFF_GET_CONST, 6);
    for (int i = 0; i < 2000; i++) s += gc();
    printf("get_const sum = %llx (last %x)\n", (unsigned long long)s, gc());
    // ... and by the running code itself (each iteration rewrites the immediate get_const returns next)
    printf("smc_loop = %llx\n", (unsigned long long)F(OFF_SMC_LOOP, fn_pn)(code + OFF_GET_CONST + 1, 3000));
    printf("get_const after = %x\n", gc());

    // threads: the same translations from 4 threads at once
    ref_int = F(OFF_INT_LOOP, fn_n)(100000);
    ref_switch = F(OFF_SWITCH_LOOP, fn_n)(50000);
    ref_fp = F(OFF_FP_LOOP, fn_pn)(fpv, 20000);
#ifdef _WIN32
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, thread_main, (LPVOID)(intptr_t)i, 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
#else
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, thread_main, (void *)(intptr_t)i);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
#endif
    printf("threads: mismatches %d, cells = %llu %llu %llu\n", thread_bad, (unsigned long long)cells[0], (unsigned long long)cells[1],
           (unsigned long long)cells[2]);
#ifdef _WIN32
    fflush(stdout);
    ExitProcess(0);   // (the runtime prints JIT statistics here under WINTOMAC_JIT_TRACE=1)
#endif
    return 0;
}
