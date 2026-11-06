// MSVC-style structured exception handling: __try/__except with filters, __finally on normal and
// abnormal exit, nested scopes, RaiseException with parameters, and hardware faults (integer divide by zero).
// Built with clang -fms-extensions (MSVC scope tables + __C_specific_handler).
#include <windows.h>
#include <stdio.h>

static int finally_runs, filter_calls;
static volatile int zero = 0;

static int filter(DWORD code, EXCEPTION_POINTERS *ep, DWORD want) {
    filter_calls++;
    if (code == want) {
        printf("  filter: code=0x%08lx params=%lu p0=%llu\n", (unsigned long)code, (unsigned long)ep->ExceptionRecord->NumberParameters,
               (unsigned long long)(ep->ExceptionRecord->NumberParameters ? ep->ExceptionRecord->ExceptionInformation[0] : 0));
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

__declspec(noinline) static void raise_with(DWORD code, ULONG_PTR p) { ULONG_PTR a[1] = { p }; RaiseException(code, 0, 1, a); }

__declspec(noinline) static int inner(int mode) {
    int r = 0;
    __try {
        __try {
            if (mode == 1) raise_with(0xE0001234, 77);
            if (mode == 2) { volatile int q = 100 / zero; r = q; }  // EXCEPTION_INT_DIVIDE_BY_ZERO (result kept live)
            if (mode == 3) raise_with(0xE0005678, 5); // not handled here: passes through two scopes
            r = 1;
        } __finally {
            finally_runs++;
            printf("  inner finally (abnormal=%d)\n", AbnormalTermination());
        }
    } __except (filter(GetExceptionCode(), GetExceptionInformation(), 0xE0001234)) {
        printf("  inner caught 0x%08lx\n", (unsigned long)GetExceptionCode());
        r = -1;
    }
    return r;
}

int main(void) {
    printf("normal: %d\n", inner(0));
    printf("raised: %d\n", inner(1));
    __try { printf("div0: %d\n", inner(2)); }
    __except (filter(GetExceptionCode(), GetExceptionInformation(), EXCEPTION_INT_DIVIDE_BY_ZERO)) { printf("main caught divide by zero\n"); }
    __try { inner(3); }
    __except (GetExceptionCode() == 0xE0005678 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { printf("main caught 0x%08lx\n", (unsigned long)GetExceptionCode()); }
    int n = 0;
    for (int i = 0; i < 1000; i++) { __try { raise_with(0xE0000001, i); } __except (EXCEPTION_EXECUTE_HANDLER) { n++; } }
    printf("1000 raises caught: %d\n", n);
    printf("finally runs=%d filter calls=%d\n", finally_runs, filter_calls);
    return 0;
}
