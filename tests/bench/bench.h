// Shared helpers for the wintomac benchmarks (tools/bench.sh).
// Every benchmark is plain C/C++ that builds three ways from the same source:
//   Windows x64 (recompiled by wintomac), x86_64 macOS (run by Rosetta 2) and arm64 macOS (native ceiling).
// Each kernel prints one line:  BENCH <name> <milliseconds> <checksum>
// The checksum must be identical in all three builds (tools/bench.sh checks it).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static inline double bench_now_ms(void) {
    LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
typedef HANDLE bench_thread;
typedef DWORD (WINAPI *bench_thread_fn_)(LPVOID);
static inline bench_thread bench_thread_start(void *(*fn)(void *), void *arg) {
    return CreateThread(NULL, 0, (bench_thread_fn_)(void *)fn, arg, 0, NULL);
}
static inline void bench_thread_join(bench_thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#define BENCH_NOINLINE __attribute__((noinline))
#else
#include <time.h>
#include <pthread.h>
static inline double bench_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
typedef pthread_t bench_thread;
static inline bench_thread bench_thread_start(void *(*fn)(void *), void *arg) {
    pthread_t t; pthread_create(&t, NULL, fn, arg); return t;
}
static inline void bench_thread_join(bench_thread t) { pthread_join(t, NULL); }
#define BENCH_NOINLINE __attribute__((noinline))
#endif

static inline void bench_report(const char *name, double ms, uint64_t checksum) {
    printf("BENCH %s %.2f %016llx\n", name, ms, (unsigned long long)checksum);
    fflush(stdout);
}

// Deterministic PRNG (xorshift64*), identical on every build.
static inline uint64_t bench_rng(uint64_t *s) {
    uint64_t x = *s; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *s = x; return x * 0x2545F4914F6CDD1Dull;
}

// Keeps the optimizer from deleting a computation without adding work.
static volatile uint64_t bench_sink;
