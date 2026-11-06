// Multithreaded atomics and locks: contended fetch-add, a CAS spinlock guarding shared state, a single-
// producer/single-consumer ring buffer (acquire/release), and per-thread work with plain shared reads
// (a job-system shape: mostly private work, little sharing). This is where x86 TSO emulation costs show.
#include "bench.h"

#define NT 4
static volatile long long counter;
static volatile int spin;
static long long guarded[8];

static void *t_fetch_add(void *arg) {
    for (int i = 0; i < 2000000; i++) __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
    return arg;
}

static void *t_spinlock(void *arg) {
    long id = (long)(intptr_t)arg;
    for (int i = 0; i < 300000; i++) {
        while (__atomic_exchange_n(&spin, 1, __ATOMIC_ACQUIRE)) { while (__atomic_load_n(&spin, __ATOMIC_RELAXED)) {} }
        for (int k = 0; k < 8; k++) guarded[k] += id + k;
        __atomic_store_n(&spin, 0, __ATOMIC_RELEASE);
    }
    return arg;
}

#define RING 1024
static uint64_t ring[RING];
static volatile uint64_t head, tail;   // head: written by producer, tail: by consumer
#define NMSG 4000000
static void *t_producer(void *arg) {
    for (uint64_t i = 1; i <= NMSG; i++) {
        uint64_t h = __atomic_load_n(&head, __ATOMIC_RELAXED);
        while (h - __atomic_load_n(&tail, __ATOMIC_ACQUIRE) >= RING) {}
        ring[h % RING] = i * 3;
        __atomic_store_n(&head, h + 1, __ATOMIC_RELEASE);
    }
    return arg;
}
static uint64_t consumed;
static void *t_consumer(void *arg) {
    uint64_t sum = 0;
    for (uint64_t n = 0; n < NMSG; n++) {
        uint64_t t = __atomic_load_n(&tail, __ATOMIC_RELAXED);
        while (__atomic_load_n(&head, __ATOMIC_ACQUIRE) == t) {}
        sum += ring[t % RING];
        __atomic_store_n(&tail, t + 1, __ATOMIC_RELEASE);
    }
    consumed = sum;
    return arg;
}

// Job-system style: each thread transforms its own slice of a shared array (memory accesses that are not
// stack-relative, so the recompiler must treat them as potentially shared), with one atomic per job.
#define JN (1 << 20)
static uint32_t jobdata[JN];
static volatile long jobs_done;
static void *t_jobs(void *arg) {
    long id = (long)(intptr_t)arg;
    for (int pass = 0; pass < 400; pass++) {
        uint32_t *p = jobdata + id * (JN / NT);
        for (int i = 0; i < JN / NT; i++) p[i] = p[i] * 1664525u + 1013904223u + (uint32_t)(p[i] >> 13);
        __atomic_fetch_add(&jobs_done, 1, __ATOMIC_SEQ_CST);
    }
    return arg;
}

static void run(void *(*fn)(void *)) {
    bench_thread t[NT];
    for (long i = 0; i < NT; i++) t[i] = bench_thread_start(fn, (void *)(intptr_t)i);
    for (int i = 0; i < NT; i++) bench_thread_join(t[i]);
}

int main(void) {
    double t0 = bench_now_ms();
    run(t_fetch_add);
    bench_report("atomic_fetch_add", bench_now_ms() - t0, (uint64_t)counter);

    t0 = bench_now_ms();
    run(t_spinlock);
    uint64_t h = 0; for (int k = 0; k < 8; k++) h = h * 31 + (uint64_t)guarded[k];
    bench_report("spinlock", bench_now_ms() - t0, h);

    t0 = bench_now_ms();
    bench_thread p = bench_thread_start(t_producer, 0), c = bench_thread_start(t_consumer, 0);
    bench_thread_join(p); bench_thread_join(c);
    bench_report("spsc_ring", bench_now_ms() - t0, consumed);

    for (int i = 0; i < JN; i++) jobdata[i] = (uint32_t)i;
    t0 = bench_now_ms();
    run(t_jobs);
    h = 0; for (int i = 0; i < JN; i += 97) h = h * 31 + jobdata[i];
    bench_report("job_slices", bench_now_ms() - t0, h + (uint64_t)jobs_done);
    return 0;
}
