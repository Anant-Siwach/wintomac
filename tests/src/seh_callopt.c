// SEH (__try/__except, __finally) unwinding through recompiled frames that use the selective call-site sync:
// the frames between the raise and the handler keep values in callee-saved registers, and the handler's
// frame reads values that were live across the __try. Raised by RaiseException (an import) and by a hardware
// exception (integer divide by zero, which the recompiled code turns into an SEH exception).
#include <windows.h>
#include <stdio.h>

static volatile int zero;
static int finally_runs;

__declspec(noinline) static long long raise_it(int n) {
    if (n % 3 == 0) RaiseException(0xE0000001u + n % 2, 0, 0, NULL);
    if (n % 3 == 1) return 100 / zero;   // EXCEPTION_INT_DIVIDE_BY_ZERO
    return n * 3;
}

__declspec(noinline) static long long middle(int n, long long seed) {
    long long a = seed, b = seed * 5, c = seed ^ 0x77, d = n, e = n * 9, f = 13, g = seed + 1;
    for (int i = 0; i < n % 4 + 2; i++) { a += b; b ^= c; c += d; d += e; e ^= f; f += g; g += a >> 3; }
    long long r = raise_it(n);
    return r + a + b + c + d + e + f + g;
}

__declspec(noinline) static long long with_finally(int n) {
    long long x = n * 7, y = n * 11, r = 0;
    __try {
        r = middle(n, x ^ y);
    } __finally {
        finally_runs++;
    }
    return r + x + y;
}

static int filter(unsigned code) {
    return code == 0xE0000001u || code == 0xE0000002u || code == EXCEPTION_INT_DIVIDE_BY_ZERO
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

int main(void) {
    long long total = 0, k1 = 101, k2 = 202, k3 = 303, k4 = 404;
    unsigned codes = 0;
    for (int n = 0; n < 30; n++) {
        __try {
            total += with_finally(n) + k1;
        } __except (filter(GetExceptionCode())) {
            codes = codes * 7 + (GetExceptionCode() & 0xff);
            total += k2 * n - k3 + k4;   // values live across the __try
        }
        k1 += k2; k2 ^= k3; k3 += n; k4 -= k1 >> 2;
    }
    printf("total=%lld codes=%u finally=%d k=%lld %lld %lld %lld\n", total, codes, finally_runs, k1, k2, k3, k4);
    return 0;
}
