// Multithreaded correctness: lock-prefixed RMW, xchg spinlocks, cmpxchg16b, and a lock-free
// message-passing pattern that relies on x86 store ordering (TSO).
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

#define N 4
#define ITERS 200000
static volatile long counter_lock, counter_xadd; static volatile long long counter64;
static volatile long spin; static long protected_counter;
static volatile unsigned long bits[4];
static __attribute__((aligned(16))) volatile unsigned long long pair[2];
// message passing: writer fills data then publishes seq; reader must never see seq without the data.
static volatile int data[64]; static volatile int seq; static volatile int mp_errors;

static DWORD WINAPI worker(LPVOID arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < ITERS; i++) {
        __asm__ volatile("lock incl %0" : "+m"(counter_lock));                      // lock inc
        _InterlockedExchangeAdd(&counter_xadd, 2);                                 // lock xadd
        __asm__ volatile("lock addq %1, %0" : "+m"(counter64) : "r"(3LL));          // lock add
        while (_InterlockedExchange(&spin, 1)) {}                                  // xchg spinlock
        protected_counter++;
        _InterlockedExchange(&spin, 0);
        if ((i & 1023) == 0) _interlockedbittestandset((volatile long *)&bits[id], (i >> 10) & 31);  // lock bts
        unsigned long long lo, hi; unsigned char ok;
        do {
            lo = pair[0]; hi = pair[1];
            unsigned long long nlo = lo + 1, nhi = hi + 1;
            __asm__ volatile("lock cmpxchg16b %1; sete %0" : "=q"(ok), "+m"(*(volatile unsigned __int128 *)pair), "+a"(lo), "+d"(hi)
                             : "b"(nlo), "c"(nhi) : "cc", "memory");
        } while (!ok);
    }
    return 0;
}

static DWORD WINAPI mp_writer(LPVOID a) {
    (void)a;
    for (int s = 1; s <= 20000; s++) {
        for (int k = 0; k < 64; k++) data[k] = s;
        __asm__ volatile("" ::: "memory");   // compiler barrier only: x86 hardware keeps stores ordered
        seq = s;
    }
    return 0;
}
static DWORD WINAPI mp_reader(LPVOID a) {
    (void)a; int last = 0;
    while (last < 20000) {
        int s = seq;
        __asm__ volatile("" ::: "memory");
        if (s != last) {
            for (int k = 0; k < 64; k++) if (data[k] < s) { mp_errors++; break; }
            last = s;
        }
    }
    return 0;
}

int main(void) {
    HANDLE t[N + 2];
    for (int i = 0; i < N; i++) t[i] = CreateThread(NULL, 0, worker, (LPVOID)(intptr_t)i, 0, NULL);
    WaitForMultipleObjects(N, t, TRUE, INFINITE);
    printf("lock inc   %ld (want %d)\n", counter_lock, N * ITERS);
    printf("lock xadd  %ld (want %d)\n", counter_xadd, 2 * N * ITERS);
    printf("lock add64 %lld (want %lld)\n", counter64, 3LL * N * ITERS);
    printf("spinlock   %ld (want %d)\n", protected_counter, N * ITERS);
    printf("cmpxchg16b %llu %llu (want %d)\n", pair[0], pair[1], N * ITERS);
    printf("lock bts   %08lx %08lx %08lx %08lx\n", bits[0], bits[1], bits[2], bits[3]);
    t[N] = CreateThread(NULL, 0, mp_reader, NULL, 0, NULL);
    t[N + 1] = CreateThread(NULL, 0, mp_writer, NULL, 0, NULL);
    WaitForMultipleObjects(2, t + N, TRUE, INFINITE);
    printf("message passing errors: %d\n", mp_errors);
    return 0;
}
