// CRT-linked test: startup code, printf varargs, heap, callbacks, floating point, strings.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { const char *name; int score; double ratio; } Entry;

static int by_score(const void *a, const void *b) { return ((const Entry *)b)->score - ((const Entry *)a)->score; }

__attribute__((noinline)) double integrate(double (*f)(double), double a, double b, int n) {
    double h = (b - a) / n, s = 0.5 * (f(a) + f(b));
    for (int i = 1; i < n; i++) s += f(a + i * h);
    return s * h;
}
static double sq(double x) { return x * x; }

int main(int argc, char **argv) {
    printf("argc=%d argv[0]=%s\n", argc, argv[0] ? "set" : "null");
    Entry *e = malloc(5 * sizeof *e);
    const char *names[] = { "alpha", "bravo", "charlie", "delta", "echo" };
    for (int i = 0; i < 5; i++) { e[i].name = names[i]; e[i].score = (i * 37) % 11; e[i].ratio = e[i].score / 3.0; }
    qsort(e, 5, sizeof *e, by_score);
    for (int i = 0; i < 5; i++) printf("%-8s %3d %8.4f\n", e[i].name, e[i].score, e[i].ratio);
    free(e);
    char buf[64]; snprintf(buf, sizeof buf, "%s-%05d-%x", "id", 42, 0xbeef);
    printf("buf=%s len=%zu\n", buf, strlen(buf));
    printf("integral x^2 0..3 = %.6f\n", integrate(sq, 0.0, 3.0, 1000));
    float f = 1.5f; for (int i = 0; i < 10; i++) f = f * 1.1f + 0.25f;
    printf("float f=%.5f (int)f=%d sqrt=%.6f\n", f, (int)f, sqrt(2.0));
    unsigned long long ubig = 1; for (int i = 0; i < 40; i++) ubig *= 3; long long big = (long long)(ubig / 7) - 5000000000000000000LL;
    printf("big=%lld mod=%lld\n", big, big % 1000003);
    int *arr = calloc(100, sizeof(int)); for (int i = 0; i < 100; i++) arr[i] = i * i; long sum = 0; for (int i = 0; i < 100; i++) sum += arr[i];
    printf("sum=%ld\n", sum); free(arr);
    return 7;
}
