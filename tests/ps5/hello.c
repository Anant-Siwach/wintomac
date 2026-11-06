/* PS5 homebrew hello world for the wintomac PS5 chain (tools/ps5_build.sh, docs/ps5.md).
   Built with the open ps5-payload-dev SDK; relinked by AnyPS5 --windows; recompiled by wintomac. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static uint64_t fib(int n) { uint64_t a = 0, b = 1; while (n--) { uint64_t t = a + b; a = b; b = t; } return a; }

int main(int argc, char **argv) {
    printf("Hello from PS5 homebrew!\n");
    printf("argc=%d\n", argc);
    char *buf = malloc(64);
    snprintf(buf, 64, "fib(50)=%llu", (unsigned long long)fib(50));
    puts(buf);
    free(buf);
    double s = 0;
    for (int i = 1; i <= 1000; i++) s += sqrt((double)i) * sin(i * 0.01);
    printf("sum=%.6f\n", s);
    float v[8]; for (int i = 0; i < 8; i++) v[i] = i * 1.5f;
    float dot = 0; for (int i = 0; i < 8; i++) dot += v[i] * v[7 - i];
    printf("dot=%.3f strlen=%zu\n", dot, strlen("wintomac"));
    return 7;
}
