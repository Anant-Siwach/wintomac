/* PS5 compat: libkernel sce* API: scePthread*, sceKernel semaphores/event flags/equeue, time, usleep. Hand-written expectations. */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
typedef void *ScePthread; typedef void *ScePthreadMutex; typedef void *ScePthreadCond;
int scePthreadCreate(ScePthread *t, const void *attr, void *(*fn)(void *), void *arg, const char *name);
int scePthreadJoin(ScePthread t, void **ret);
int scePthreadMutexInit(ScePthreadMutex *m, const void *attr, const char *name);
int scePthreadMutexLock(ScePthreadMutex *m); int scePthreadMutexUnlock(ScePthreadMutex *m);
int scePthreadMutexDestroy(ScePthreadMutex *m);
int scePthreadCondInit(ScePthreadCond *c, const void *attr, const char *name);
int scePthreadCondWait(ScePthreadCond *c, ScePthreadMutex *m); int scePthreadCondSignal(ScePthreadCond *c);
int sceKernelCreateSema(void **h, const char *name, unsigned attr, int init, int max, void *opt);
int sceKernelSignalSema(void *h, int n); int sceKernelWaitSema(void *h, int n, unsigned *timeout);
int sceKernelPollSema(void *h, int n); int sceKernelDeleteSema(void *h);
int sceKernelCreateEventFlag(void **h, const char *name, unsigned attr, uint64_t init, void *opt);
int sceKernelSetEventFlag(void *h, uint64_t bits); int sceKernelWaitEventFlag(void *h, uint64_t bits, unsigned mode, uint64_t *res, unsigned *timeout);
int sceKernelDeleteEventFlag(void *h);
uint64_t sceKernelGetProcessTime(void); uint64_t sceKernelGetProcessTimeCounter(void); uint64_t sceKernelGetProcessTimeCounterFrequency(void);
int sceKernelUsleep(unsigned us); uint64_t sceKernelGetTscFrequency(void);
int sceKernelIsNeoMode(void);

static ScePthreadMutex mu; static ScePthreadCond cv; static int flag; static void *sema, *evf; /* handles are pointers */
static void *th(void *a) {
    sceKernelUsleep(5000); scePthreadMutexLock(&mu); flag = (int)(intptr_t)a; scePthreadCondSignal(&cv); scePthreadMutexUnlock(&mu);
    sceKernelWaitSema(sema, 1, 0); sceKernelSetEventFlag(evf, 0x5); return (void *)77;
}
int main(void) {
    scePthreadMutexInit(&mu, 0, "mu"); scePthreadCondInit(&cv, 0, "cv");
    printf("sema create=%d\n", sceKernelCreateSema(&sema, "s", 0, 0, 10, 0));
    printf("evf create=%d\n", sceKernelCreateEventFlag(&evf, "e", 0x100, 0, 0));
    ScePthread t; printf("pthread create=%d\n", scePthreadCreate(&t, 0, th, (void *)5, "worker"));
    scePthreadMutexLock(&mu); while (!flag) scePthreadCondWait(&cv, &mu); scePthreadMutexUnlock(&mu);
    printf("cond flag=%d\n", flag);
    printf("pollsema(empty)=%s\n", sceKernelPollSema(sema, 1) < 0 ? "EBUSY" : "ok");
    sceKernelSignalSema(sema, 1);
    uint64_t res = 0; printf("waitevf=%d res=%llx\n", sceKernelWaitEventFlag(evf, 0x5, 0x21, &res, 0), (unsigned long long)res);
    void *r; printf("join=%d ret=%ld\n", scePthreadJoin(t, &r), (long)r);
    uint64_t a = sceKernelGetProcessTime(); sceKernelUsleep(30000); uint64_t b = sceKernelGetProcessTime();
    printf("usleep 30ms: %d\n", b - a >= 29000 && b - a < 500000);
    uint64_t c = sceKernelGetProcessTimeCounter(); printf("counter freq nonzero=%d counter nonzero=%d\n", sceKernelGetProcessTimeCounterFrequency() != 0, c != 0);
    printf("tsc freq plausible=%d\n", sceKernelGetTscFrequency() > 100000000ULL);
    sceKernelDeleteSema(sema); sceKernelDeleteEventFlag(evf); scePthreadMutexDestroy(&mu);
    return 0;
}
