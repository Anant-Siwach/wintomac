// Freestanding Win64 test: no CRT, kernel32 only.
#include <windows.h>

static HANDLE out;
static void puts_(const char *s) { DWORD n = 0; const char *p = s; while (*p) p++; WriteFile(out, s, (DWORD)(p - s), &n, 0); }
static void putu(unsigned long long v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); puts_(b + i); }

__attribute__((noinline)) static unsigned long long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
__attribute__((noinline)) static int sq(int x) { return x * x; }
__attribute__((noinline)) static int neg(int x) { return -x; }
__attribute__((noinline)) static int dbl(int x) { return x + x; }
static int (*volatile ops[])(int) = { sq, neg, dbl };

__attribute__((noinline)) static const char *name(int k) {
    switch (k) { case 0: return "zero"; case 1: return "one"; case 2: return "two"; case 3: return "three";
                 case 4: return "four"; case 5: return "five"; case 6: return "six"; default: return "many"; }
}

__attribute__((noinline)) static int isort(int *a, int n) {
    for (int i = 1; i < n; i++) { int v = a[i], j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
    return a[0];
}

void entry(void) {
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    puts_("hello from a windows exe\n");
    puts_("fib(25) = "); putu(fib(25)); puts_("\n");
    for (int i = 0; i < 3; i++) { puts_("op"); putu(i); puts_("(7) = "); int r = ops[i](7); if (r < 0) { puts_("-"); r = -r; } putu(r); puts_("\n"); }
    for (int k = 0; k < 9; k++) { puts_(name(k)); puts_(k < 8 ? " " : "\n"); }
    int arr[] = { 9, 3, 7, 1, 8, 2 }; isort(arr, 6);
    for (int i = 0; i < 6; i++) { putu(arr[i]); puts_(i < 5 ? "," : "\n"); }
    unsigned long long h = 1469598103934665603ULL; for (int i = 0; i < 1000; i++) { h ^= i; h *= 1099511628211ULL; }
    puts_("fnv = "); putu(h); puts_("\n");
    ExitProcess(42);
}
