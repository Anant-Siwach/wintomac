// Win32 sync primitives used by SDL3 and modern CRTs: condition variables over CRITICAL_SECTION and SRW locks
// (producer/consumer with 4 consumers), WaitOnAddress/WakeByAddress handoff, waitable timers, CV timeouts.
#include <windows.h>
#include <stdio.h>

#define ITEMS 20000
static CRITICAL_SECTION cs; static SRWLOCK srw = SRWLOCK_INIT;
static CONDITION_VARIABLE cv_cs = CONDITION_VARIABLE_INIT, cv_srw = CONDITION_VARIABLE_INIT;
static int q_cs[64], n_cs, q_srw[64], n_srw, done_cs, done_srw;
static volatile LONG sum_cs, sum_srw;

static DWORD WINAPI consumer_cs(LPVOID a) {
    (void)a;
    for (;;) {
        EnterCriticalSection(&cs);
        while (!n_cs && !done_cs) SleepConditionVariableCS(&cv_cs, &cs, INFINITE);
        if (!n_cs && done_cs) { LeaveCriticalSection(&cs); return 0; }
        int v = q_cs[--n_cs];
        WakeAllConditionVariable(&cv_cs);
        LeaveCriticalSection(&cs);
        InterlockedAdd(&sum_cs, v);
    }
}
static DWORD WINAPI consumer_srw(LPVOID a) {
    (void)a;
    for (;;) {
        AcquireSRWLockExclusive(&srw);
        while (!n_srw && !done_srw) SleepConditionVariableSRW(&cv_srw, &srw, INFINITE, 0);
        if (!n_srw && done_srw) { ReleaseSRWLockExclusive(&srw); return 0; }
        int v = q_srw[--n_srw];
        WakeAllConditionVariable(&cv_srw);
        ReleaseSRWLockExclusive(&srw);
        InterlockedAdd(&sum_srw, v);
    }
}
static volatile LONG flag, seen;
static DWORD WINAPI waiter(LPVOID a) {
    (void)a; LONG zero = 0;
    for (int i = 0; i < 1000; i++) {
        while (flag == 0) WaitOnAddress(&flag, &zero, sizeof flag, INFINITE);
        seen++; InterlockedExchange(&flag, 0); WakeByAddressSingle((PVOID)&flag);
    }
    return 0;
}

int main(void) {
    InitializeCriticalSection(&cs);
    HANDLE t[8];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 0, consumer_cs, NULL, 0, NULL);
    for (int i = 0; i < 4; i++) t[4 + i] = CreateThread(NULL, 0, consumer_srw, NULL, 0, NULL);
    for (int i = 1; i <= ITEMS; i++) {
        EnterCriticalSection(&cs); while (n_cs == 64) SleepConditionVariableCS(&cv_cs, &cs, INFINITE);
        q_cs[n_cs++] = i; WakeConditionVariable(&cv_cs); LeaveCriticalSection(&cs);
        AcquireSRWLockExclusive(&srw); while (n_srw == 64) SleepConditionVariableSRW(&cv_srw, &srw, INFINITE, 0);
        q_srw[n_srw++] = i; WakeConditionVariable(&cv_srw); ReleaseSRWLockExclusive(&srw);
    }
    EnterCriticalSection(&cs); done_cs = 1; WakeAllConditionVariable(&cv_cs); LeaveCriticalSection(&cs);
    AcquireSRWLockExclusive(&srw); done_srw = 1; WakeAllConditionVariable(&cv_srw); ReleaseSRWLockExclusive(&srw);
    WaitForMultipleObjects(8, t, TRUE, INFINITE);
    long long want = (long long)ITEMS * (ITEMS + 1) / 2;
    printf("CV over CRITICAL_SECTION: sum %s\n", sum_cs == want ? "correct" : "WRONG");
    printf("CV over SRW lock:         sum %s\n", sum_srw == want ? "correct" : "WRONG");

    HANDLE w = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    LONG one = 1;
    for (int i = 0; i < 1000; i++) {
        InterlockedExchange(&flag, 1); WakeByAddressSingle((PVOID)&flag);
        while (flag == 1) WaitOnAddress(&flag, &one, sizeof flag, INFINITE);
    }
    WaitForSingleObject(w, 5000);
    printf("WaitOnAddress handoffs: %ld\n", seen);

    EnterCriticalSection(&cs);
    DWORD t0 = GetTickCount(); BOOL r = SleepConditionVariableCS(&cv_cs, &cs, 100); DWORD dt = GetTickCount() - t0;
    LeaveCriticalSection(&cs);
    printf("CV timeout: returned %s, error=%s, waited ~100ms: %s\n", r ? "TRUE" : "FALSE", GetLastError() == ERROR_TIMEOUT ? "ERROR_TIMEOUT" : "?", dt >= 90 && dt < 400 ? "yes" : "no");

    HANDLE tm = CreateWaitableTimerW(NULL, TRUE, NULL);
    LARGE_INTEGER due; due.QuadPart = -1500000;   // 150 ms, relative
    SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE);
    t0 = GetTickCount();
    printf("timer not yet signalled: %s\n", WaitForSingleObject(tm, 0) == WAIT_TIMEOUT ? "yes" : "no");
    DWORD wr = WaitForSingleObject(tm, 2000); dt = GetTickCount() - t0;
    printf("timer fired: %s after ~150ms: %s\n", wr == WAIT_OBJECT_0 ? "yes" : "no", dt >= 130 && dt < 600 ? "yes" : "no");
    CancelWaitableTimer(tm); CloseHandle(tm);
    OSVERSIONINFOW v = { sizeof v }; GetVersionExW(&v);
    printf("reports Windows %lu.%lu build %lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return 0;
}
