// Threads and waitable objects (events, semaphores, threads) on top of pthreads.
// One global lock + condvar for all waitables: simple and correct; optimise when a game needs it.
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#include "win32.h"

enum { W_EVENT = 1, W_SEM, W_THREAD, W_MUTEX };
typedef struct Waitable {
    int type;
    int manual_reset;
    i64 count, max;        // event: 0/1 signaled; sem: count; thread: 1 when exited; mutex: recursion
    u32 exit_code;
    pthread_t owner;
} Waitable;

static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wcond = PTHREAD_COND_INITIALIZER;

#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102
#define WAIT_FAILED 0xFFFFFFFF
#define INFINITE 0xFFFFFFFF

static u64 new_waitable(int type, int manual, i64 count, i64 max) {
    Waitable *w = calloc(1, sizeof *w);
    w->type = type; w->manual_reset = manual; w->count = count; w->max = max;
    return rt_handle_new(H_WAIT, (intptr_t)w);
}
static Waitable *get_w(u64 h) { intptr_t p; return rt_handle_get(h, H_WAIT, &p) ? (Waitable *)p : NULL; }

static int signaled(Waitable *w) {
    if (w->type == W_MUTEX) return w->count == 0 || pthread_equal(w->owner, pthread_self());
    return w->count > 0;
}
static void consume(Waitable *w) {
    if (w->type == W_EVENT && !w->manual_reset) w->count = 0;
    else if (w->type == W_SEM) w->count--;
    else if (w->type == W_MUTEX) { w->owner = pthread_self(); w->count++; }
}

static u32 wait_for(int n, Waitable **ws, int all, u32 ms) {
    struct timespec dl;
    if (ms != INFINITE) {
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    }
    pthread_mutex_lock(&wlock);
    for (;;) {
        if (all) {
            int ok = 1;
            for (int i = 0; i < n; i++) ok &= signaled(ws[i]);
            if (ok) { for (int i = 0; i < n; i++) consume(ws[i]); pthread_mutex_unlock(&wlock); return WAIT_OBJECT_0; }
        } else {
            for (int i = 0; i < n; i++)
                if (signaled(ws[i])) { consume(ws[i]); pthread_mutex_unlock(&wlock); return WAIT_OBJECT_0 + i; }
        }
        if (ms == 0) break;
        int r = ms == INFINITE ? pthread_cond_wait(&wcond, &wlock) : pthread_cond_timedwait(&wcond, &wlock, &dl);
        if (r == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&wlock);
    return WAIT_TIMEOUT;
}

SHIM(CreateEventA) { RET(new_waitable(W_EVENT, (int)(u32)A2, (u32)A3 ? 1 : 0, 1)); }   // (sa, manual, initial, name)
SHIM(CreateEventW) { sh_CreateEventA(c); }
SHIM(CreateEventExW) { u32 fl = (u32)A3; RET(new_waitable(W_EVENT, fl & 1, (fl & 2) ? 1 : 0, 1)); }
int rt_signal_event(u64 h) {  // SetEvent from host code (audio mixer threads etc.)
    Waitable *w = get_w(h); if (!w) return 0;
    pthread_mutex_lock(&wlock); w->count = 1; pthread_cond_broadcast(&wcond); pthread_mutex_unlock(&wlock); return 1;
}
SHIM(SetEvent) { Waitable *w = get_w(A1); if (!w) RET(0); pthread_mutex_lock(&wlock); w->count = 1; pthread_cond_broadcast(&wcond); pthread_mutex_unlock(&wlock); RET(1); }
SHIM(ResetEvent) { Waitable *w = get_w(A1); if (!w) RET(0); pthread_mutex_lock(&wlock); w->count = 0; pthread_mutex_unlock(&wlock); RET(1); }
SHIM(CreateSemaphoreA) { RET(new_waitable(W_SEM, 0, (i32)A2, (i32)A3)); }             // (sa, initial, max, name)
SHIM(CreateSemaphoreW) { sh_CreateSemaphoreA(c); }
SHIM(CreateSemaphoreExW) { sh_CreateSemaphoreA(c); }
SHIM(ReleaseSemaphore) {
    Waitable *w = get_w(A1); if (!w) RET(0);
    pthread_mutex_lock(&wlock);
    if (A3) ST32(A3, (u32)w->count);
    int ok = w->count + (i32)A2 <= w->max;
    if (ok) w->count += (i32)A2;
    pthread_cond_broadcast(&wcond); pthread_mutex_unlock(&wlock);
    RET(ok);
}
SHIM(CreateMutexA) { u64 h = new_waitable(W_MUTEX, 0, 0, 0); if ((u32)A2) { Waitable *w = get_w(h); w->owner = pthread_self(); w->count = 1; } RET(h); }
SHIM(CreateMutexW) { sh_CreateMutexA(c); }
SHIM(ReleaseMutex) {
    Waitable *w = get_w(A1); if (!w) RET(0);
    pthread_mutex_lock(&wlock); if (w->count > 0) w->count--; pthread_cond_broadcast(&wcond); pthread_mutex_unlock(&wlock);
    RET(1);
}
void gfx_main_service(void);   // ps5_gfx.m: main-thread window work queued by other threads
SHIM(WaitForSingleObject) {
    gfx_main_service();
    if (A1 == (u64)-1 || A1 == (u64)-2) RET(WAIT_TIMEOUT);  // pseudo handles never signal
    Waitable *w = get_w(A1); if (!w) RET(WAIT_FAILED);
    RET(wait_for(1, &w, 0, (u32)A2));
}
SHIM(WaitForSingleObjectEx) { sh_WaitForSingleObject(c); }
u32 rt_wait_handles(u32 n, u64 handles, int all, u32 ms) {   // for MsgWaitForMultipleObjects
    Waitable *ws[64]; if (n > 64) return WAIT_FAILED;
    for (u32 i = 0; i < n; i++) if (!(ws[i] = get_w(LD64(handles + 8 * i)))) return WAIT_FAILED;
    return wait_for((int)n, ws, all, ms);
}
SHIM(WaitForMultipleObjects) {  // (n, handles*, all, ms)
    u32 n = (u32)A1; Waitable *ws[64];
    if (n > 64) RET(WAIT_FAILED);
    for (u32 i = 0; i < n; i++) if (!(ws[i] = get_w(LD64(A2 + 8 * i)))) RET(WAIT_FAILED);
    RET(wait_for((int)n, ws, (int)(u32)A3, (u32)A4));
}

// ---- threads ------------------------------------------------------------------
typedef struct { u64 start, param; Waitable *w; Ctx *ctx; u32 tid; } ThreadStart;
static u32 next_tid = 2;
static __thread ThreadStart *cur_thread;

#define GUEST_STACK (4ull << 20)

// Guest context for a host thread we didn't create (audio mixer, driver callbacks): own stack + TEB + TLS,
// created on first use and kept for the thread's lifetime.
Ctx *rt_foreign_ctx(void) {
    if (rt_cur_ctx) return rt_cur_ctx;
    static __thread Ctx *mine;
    if (mine) { rt_cur_ctx = mine; return mine; }
    Ctx *c = calloc(1, sizeof *c);
    u8 *stack = mmap(NULL, GUEST_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    u8 *teb = calloc(1, 0x2000);
    u64 top = (u64)stack + GUEST_STACK;
    pthread_mutex_lock(&wlock); u32 tid = next_tid++; pthread_mutex_unlock(&wlock);
    ST64((u64)teb + 0x08, top); ST64((u64)teb + 0x10, (u64)stack); ST64((u64)teb + 0x30, (u64)teb);
    ST64((u64)teb + 0x60, rt_peb); ST32((u64)teb + 0x48, tid);
    c->gs_base = (u64)teb; c->mxcsr = 0x1f80; c->fcw = 0x27f; c->r[RSP] = top - 0x100;
    extern void rt_setup_thread_tls(u64 teb); rt_setup_thread_tls((u64)teb);
    mine = c; rt_cur_ctx = c;
    return c;
}
static void *thread_main(void *arg) {
    ThreadStart *ts = arg; cur_thread = ts;
    Ctx *c = ts->ctx;
    u8 *stack = mmap(NULL, GUEST_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    u8 *teb = calloc(1, 0x2000);
    u64 top = (u64)stack + GUEST_STACK;
    ST64((u64)teb + 0x08, top); ST64((u64)teb + 0x10, (u64)stack); ST64((u64)teb + 0x30, (u64)teb);
    ST64((u64)teb + 0x60, rt_peb); ST32((u64)teb + 0x48, ts->tid);
    c->gs_base = (u64)teb; c->mxcsr = 0x1f80; c->r[RSP] = top - 0x100;
    extern void rt_setup_thread_tls(u64 teb); rt_setup_thread_tls((u64)teb);
    u64 args[1] = { ts->param };
    u32 code = (u32)rt_guest_call(c, ts->start, 1, args);
    pthread_mutex_lock(&wlock); ts->w->count = 1; ts->w->exit_code = code; pthread_cond_broadcast(&wcond); pthread_mutex_unlock(&wlock);
    return NULL;
}

static u64 spawn(u64 start, u64 param, u64 tid_out) {
    ThreadStart *ts = calloc(1, sizeof *ts);
    ts->start = start; ts->param = param; ts->ctx = calloc(1, sizeof(Ctx));
    pthread_mutex_lock(&wlock); ts->tid = next_tid++; pthread_mutex_unlock(&wlock);
    u64 h = new_waitable(W_THREAD, 1, 0, 1);
    ts->w = get_w(h);
    if (tid_out) ST32(tid_out, ts->tid);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8u << 20);  // host stack: deep recompiled frames
    pthread_t t; pthread_create(&t, &at, thread_main, ts); pthread_detach(t);
    return h;
}
SHIM(CreateThread) { RET(spawn(A3, A4, AN(6))); }            // (sa, stack, start, param, flags, *tid)
SHIM(_beginthreadex) { RET(spawn(A3, A4, AN(6))); }          // (sa, stack, start, arg, flags, *tid)
void rt_exit_thread(u32 code) {
    if (cur_thread) { pthread_mutex_lock(&wlock); cur_thread->w->count = 1; cur_thread->w->exit_code = code; pthread_cond_broadcast(&wcond); pthread_mutex_unlock(&wlock); }
    pthread_exit(NULL);
}
SHIM(ExitThread) {
    if (cur_thread) { pthread_mutex_lock(&wlock); cur_thread->w->count = 1; cur_thread->w->exit_code = (u32)A1; pthread_cond_broadcast(&wcond); pthread_mutex_unlock(&wlock); }
    pthread_exit(NULL);
}
SHIM(_endthreadex) { sh_ExitThread(c); }
SHIM(GetExitCodeThread) { Waitable *w = get_w(A1); if (!w) RET(0); ST32(A2, w->count ? w->exit_code : 259 /* STILL_ACTIVE */); RET(1); }
SHIM(ResumeThread) { RET(0); }
SHIM(SuspendThread) { RET((u32)-1); }
SHIM(GetThreadPriority) { RET(0); }
SHIM(SetThreadPriority) { RET(1); }
SHIM(GetThreadContext) { RET(0); }
SHIM(SetThreadContext) { RET(0); }
SHIM(SwitchToThread) { sched_yield(); RET(1); }
SHIM(DuplicateHandle) { if (A4) ST64(A4, A2); RET(1); }  // (srcProc, src, dstProc, *dst, ...): no refcounting yet
SHIM(GetProcessAffinityMask) { ST64(A2, 0xFF); ST64(A3, 0xFF); RET(1); }
SHIM(SetProcessAffinityMask) { RET(1); }
SHIM(SetThreadAffinityMask) { RET(0xFF); }

// SRW locks / condition variables / InitOnce (Vista+ primitives used by modern MSVC CRT and games).
// SRWLOCK and CONDITION_VARIABLE are pointer-sized; we lazily store a host object pointer in them.
static pthread_rwlock_t *srw(u64 p) {
    pthread_rwlock_t *l = (pthread_rwlock_t *)LD64(p);
    if (l) return l;
    pthread_rwlock_t *n = malloc(sizeof *n); pthread_rwlock_init(n, NULL);
    if (__atomic_compare_exchange_n((u64 *)p, (u64 *)&l, (u64)n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return n;
    pthread_rwlock_destroy(n); free(n); return l;
}
SHIM(InitializeSRWLock) { ST64(A1, 0); RET(0); }
SHIM(AcquireSRWLockExclusive) { pthread_rwlock_wrlock(srw(A1)); RET(0); }
SHIM(ReleaseSRWLockExclusive) { pthread_rwlock_unlock(srw(A1)); RET(0); }
SHIM(AcquireSRWLockShared) { pthread_rwlock_rdlock(srw(A1)); RET(0); }
SHIM(ReleaseSRWLockShared) { pthread_rwlock_unlock(srw(A1)); RET(0); }
SHIM(TryAcquireSRWLockExclusive) { RET(pthread_rwlock_trywrlock(srw(A1)) == 0); }
SHIM(TryAcquireSRWLockShared) { RET(pthread_rwlock_tryrdlock(srw(A1)) == 0); }


// ---- condition variables (CONDITION_VARIABLE: pointer-sized, lazily -> host CondVar) ----------------------------
// Windows CVs can wait on a CRITICAL_SECTION *or* an SRW lock (a pthread rwlock here, which pthread condvars can't
// wait on). So each CV has its own mutex + generation counter: a waiter samples the generation before releasing
// the user's lock, and waits until it changes; wakes bump it. No lost wakeups, works with any lock type.
typedef struct { pthread_mutex_t m; pthread_cond_t c; u64 gen; } CondVar;
static CondVar *cv_get(u64 p) {
    CondVar *v = (CondVar *)LD64(p);
    if (v) return v;
    CondVar *n = calloc(1, sizeof *n); pthread_mutex_init(&n->m, NULL); pthread_cond_init(&n->c, NULL);
    if (__atomic_compare_exchange_n((u64 *)p, (u64 *)&v, (u64)n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return n;
    pthread_cond_destroy(&n->c); pthread_mutex_destroy(&n->m); free(n); return v;
}
static void deadline(struct timespec *dl, u32 ms) {
    clock_gettime(CLOCK_REALTIME, dl);
    dl->tv_sec += ms / 1000; dl->tv_nsec += (long)(ms % 1000) * 1000000;
    if (dl->tv_nsec >= 1000000000) { dl->tv_sec++; dl->tv_nsec -= 1000000000; }
}
// Wait for a wake on `v` given the generation sampled while the caller's lock was still held. 1 = woken, 0 = timeout.
static int cv_wait_gen(CondVar *v, u64 gen, u32 ms) {
    struct timespec dl; if (ms != INFINITE) deadline(&dl, ms);
    int woke = 1;
    while (v->gen == gen) {
        int r = ms == INFINITE ? pthread_cond_wait(&v->c, &v->m) : pthread_cond_timedwait(&v->c, &v->m, &dl);
        if (r == ETIMEDOUT) { woke = v->gen != gen; break; }
    }
    pthread_mutex_unlock(&v->m);
    return woke;
}
extern pthread_mutex_t *rt_cs_mutex(u64 cs);
extern void rt_set_last_error(u32 e);
SHIM(InitializeConditionVariable) { ST64(A1, 0); RET(0); }
SHIM(WakeConditionVariable) { CondVar *v = cv_get(A1); pthread_mutex_lock(&v->m); v->gen++; pthread_cond_signal(&v->c); pthread_mutex_unlock(&v->m); RET(0); }
SHIM(WakeAllConditionVariable) { CondVar *v = cv_get(A1); pthread_mutex_lock(&v->m); v->gen++; pthread_cond_broadcast(&v->c); pthread_mutex_unlock(&v->m); RET(0); }
SHIM(SleepConditionVariableCS) {  // (cv, cs, ms)
    CondVar *v = cv_get(A1); pthread_mutex_t *cs = rt_cs_mutex(A2);
    pthread_mutex_lock(&v->m); u64 gen = v->gen;
    pthread_mutex_unlock(cs);
    int ok = cv_wait_gen(v, gen, (u32)A3);
    pthread_mutex_lock(cs);
    if (!ok) { rt_set_last_error(1460); RET(0); }   // ERROR_TIMEOUT
    RET(1);
}
SHIM(SleepConditionVariableSRW) {  // (cv, srw, ms, flags: CONDITION_VARIABLE_LOCKMODE_SHARED = 1)
    CondVar *v = cv_get(A1); pthread_rwlock_t *l = srw(A2); int shared = (u32)A4 & 1;
    pthread_mutex_lock(&v->m); u64 gen = v->gen;
    pthread_rwlock_unlock(l);
    int ok = cv_wait_gen(v, gen, (u32)A3);
    if (shared) pthread_rwlock_rdlock(l); else pthread_rwlock_wrlock(l);
    if (!ok) { rt_set_last_error(1460); RET(0); }
    RET(1);
}

// ---- WaitOnAddress / WakeByAddress (futex-like): hashed CondVars keyed by address ------------------------------
static CondVar addr_cv[64]; static pthread_once_t addr_once = PTHREAD_ONCE_INIT;
static void addr_init(void) { for (int i = 0; i < 64; i++) { pthread_mutex_init(&addr_cv[i].m, NULL); pthread_cond_init(&addr_cv[i].c, NULL); } }
static CondVar *addr_bucket(u64 a) { pthread_once(&addr_once, addr_init); return &addr_cv[(a >> 3) % 64]; }
SHIM(WaitOnAddress) {  // (addr, compare*, size, ms): returns while *addr != *compare, or on wake/timeout
    CondVar *v = addr_bucket(A1); u32 sz = (u32)A3; u32 ms = (u32)A4;
    pthread_mutex_lock(&v->m);
    if (memcmp((void *)A1, (void *)A2, sz)) { pthread_mutex_unlock(&v->m); RET(1); }
    u64 gen = v->gen;
    int ok = cv_wait_gen(v, gen, ms);
    if (!ok) { rt_set_last_error(1460); RET(0); }
    RET(1);
}
static void wake_addr(u64 a) { CondVar *v = addr_bucket(a); pthread_mutex_lock(&v->m); v->gen++; pthread_cond_broadcast(&v->c); pthread_mutex_unlock(&v->m); }
SHIM(WakeByAddressSingle) { wake_addr(A1); RET(0); }   // waking all waiters in the bucket is allowed (they re-check)
SHIM(WakeByAddressAll) { wake_addr(A1); RET(0); }

// ---- waitable timers: a waitable that a helper thread signals at the due time ----------------------------------
typedef struct { Waitable *w; u64 gen; i64 due100ns; u32 period; } TimerArm;
static void *timer_thread(void *arg) {
    TimerArm *t = arg;
    for (;;) {
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        i64 now100 = (i64)now.tv_sec * 10000000 + now.tv_nsec / 100;
        if (t->due100ns > now100) { i64 d = t->due100ns - now100; struct timespec ts = { (time_t)(d / 10000000), (long)(d % 10000000) * 100 }; nanosleep(&ts, NULL); }
        pthread_mutex_lock(&wlock);
        int live = t->w->exit_code == (u32)t->gen;   // re-armed or cancelled since? (exit_code holds the timer generation)
        if (live) { t->w->count = 1; pthread_cond_broadcast(&wcond); }
        pthread_mutex_unlock(&wlock);
        if (!live || !t->period) break;
        t->due100ns += (i64)t->period * 10000;
    }
    free(t); return NULL;
}
static u64 new_timer(int manual) { return new_waitable(W_EVENT, manual, 0, 1); }
SHIM(CreateWaitableTimerW) { RET(new_timer((u32)A2 != 0)); }          // (sa, manualReset, name)
SHIM(CreateWaitableTimerA) { RET(new_timer((u32)A2 != 0)); }
SHIM(CreateWaitableTimerExW) { RET(new_timer(((u32)A3 & 1) != 0)); }  // (sa, name, flags: CREATE_WAITABLE_TIMER_MANUAL_RESET = 1, access)
SHIM(SetWaitableTimer) {  // (h, LARGE_INTEGER *due (<0 relative, 100 ns), period ms, completion, arg, resume)
    Waitable *w = get_w(A1); if (!w) RET(0);
    i64 due = (i64)LD64(A2);
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    i64 now100 = (i64)now.tv_sec * 10000000 + now.tv_nsec / 100;
    TimerArm *t = calloc(1, sizeof *t); t->w = w; t->period = (u32)A3;
    t->due100ns = due < 0 ? now100 - due : due - 116444736000000000ll;   // absolute FILETIME -> unix 100 ns
    pthread_mutex_lock(&wlock); w->count = 0; t->gen = ++w->exit_code; pthread_mutex_unlock(&wlock);
    pthread_t th; pthread_create(&th, NULL, timer_thread, t); pthread_detach(th);
    RET(1);
}
SHIM(SetWaitableTimerEx) { sh_SetWaitableTimer(c); }
SHIM(CancelWaitableTimer) { Waitable *w = get_w(A1); if (!w) RET(0); pthread_mutex_lock(&wlock); w->exit_code++; pthread_mutex_unlock(&wlock); RET(1); }

// ---- thread names: visible in macOS crash reports, Instruments and lldb --------------------------------------
SHIM(SetThreadDescription) {  // (thread, PCWSTR name)
    char n[64]; size_t i = 0; for (; i + 1 < sizeof n && LD16(A2 + 2 * i); i++) n[i] = (char)LD16(A2 + 2 * i); n[i] = 0;
    if (A1 == (u64)-2) pthread_setname_np(n);   // only the calling thread can be renamed on macOS
    RET(0);
}
SHIM(GetThreadDescription) { u16 *w = calloc(1, 2); ST64(A2, (u64)w); RET(0); }

const Shim shims_threads[] = {
#define E(n) { NULL, #n, sh_##n }
    E(CreateEventA), E(CreateEventW), E(CreateEventExW), E(SetEvent), E(ResetEvent),
    E(CreateSemaphoreA), E(CreateSemaphoreW), E(CreateSemaphoreExW), E(ReleaseSemaphore),
    E(CreateMutexA), E(CreateMutexW), E(ReleaseMutex),
    E(WaitForSingleObject), E(WaitForSingleObjectEx), E(WaitForMultipleObjects),
    E(CreateThread), E(_beginthreadex), E(ExitThread), E(_endthreadex), E(GetExitCodeThread),
    E(ResumeThread), E(SuspendThread), E(GetThreadPriority), E(SetThreadPriority), E(GetThreadContext),
    E(SetThreadContext), E(SwitchToThread), E(DuplicateHandle), E(GetProcessAffinityMask),
    E(SetProcessAffinityMask), E(SetThreadAffinityMask),
    E(InitializeConditionVariable), E(WakeConditionVariable), E(WakeAllConditionVariable), E(SleepConditionVariableCS),
    E(SleepConditionVariableSRW), E(WaitOnAddress), E(WakeByAddressSingle), E(WakeByAddressAll), E(CreateWaitableTimerW),
    E(CreateWaitableTimerA), E(CreateWaitableTimerExW), E(SetWaitableTimer), E(SetWaitableTimerEx), E(CancelWaitableTimer),
    E(SetThreadDescription), E(GetThreadDescription),
    E(InitializeSRWLock), E(AcquireSRWLockExclusive), E(ReleaseSRWLockExclusive), E(AcquireSRWLockShared),
    E(ReleaseSRWLockShared), E(TryAcquireSRWLockExclusive), E(TryAcquireSRWLockShared),
#undef E
    { 0, 0, 0 }
};
