// A program linked the way old MinGW toolchains did it: fixed image base 0x400000 and no .reloc section
// (see the Makefile rule). macOS on arm64 can't map anything below 4 GB, so the recompiler must move it and
// recover the relocations itself: pointers stored in data (tables, vtables, strings), absolute addresses in
// code (`mov ecx, offset sym`, `[table + rax*8]`), and a switch jump table with absolute entries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*op_fn)(int, int);
static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }
static int mod(int a, int b) { return b ? a % b : 0; }

struct op { const char *name; op_fn fn; const struct op *next; };
static const struct op ops[] = {
    { "add", add, &ops[1] }, { "sub", sub, &ops[2] }, { "mul", mul, &ops[3] }, { "mod", mod, NULL },
};
static const char *const words[] = { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot" };
static int counters[64] = { 1 };   // initialized: lives in .data (code immediates into .bss count as numbers)
static int *const counter_ptr = &counters[7];        // pointer to data, stored in data
static const char *greeting = "hello from 0x400000";

__attribute__((noinline)) static const char *classify(int v) {
    switch (v) {   // dense switch: a jump table
    case 0: return "zero"; case 1: return "one"; case 2: return "two"; case 3: return "three";
    case 4: return "four"; case 5: return "five"; case 6: return "six"; case 7: return "seven";
    case 8: return "eight"; case 9: return "nine"; default: return "many";
    }
}
// Absolute addresses in code: `mov $sym, %reg` (32-bit immediate) and `sym(,%idx,4)` (absolute displacement).
// MinGW's own codegen is RIP-relative, so these are written by hand; the native reference uses plain C.
__attribute__((noinline)) static int *abs_ptr(void) {
#if defined(__x86_64__) && defined(_WIN32)
    int *p; __asm__("movl $counters+28, %k0" : "=r"(p)); return p;
#else
    return &counters[7];
#endif
}
__attribute__((noinline)) static int abs_load(long i) {
#if defined(__x86_64__) && defined(_WIN32)
    int v; __asm__("movl counters(,%1,4), %0" : "=r"(v) : "r"(i)); return v;
#else
    return counters[i];
#endif
}

static int cmp_words(const void *a, const void *b) { return strcmp(*(const char *const *)b, *(const char *const *)a); }

int main(void) {
    for (const struct op *o = &ops[0]; o; o = o->next) printf("%s(17, 5) = %d\n", o->name, o->fn(17, 5));
    for (int i = 0; i < 11; i++) printf("%s%s", classify(i), i < 10 ? " " : "\n");
    const char *sorted[6]; memcpy(sorted, words, sizeof sorted);
    qsort(sorted, 6, sizeof *sorted, cmp_words);              // callback address passed to a shim
    for (int i = 0; i < 6; i++) printf("%s%s", sorted[i], i < 5 ? " " : "\n");
    for (int *p = counters; p < counters + 64; p++) *p = (int)(p - counters) * 3;   // pointer vs array end
    printf("counter_ptr -> %d, inside: %s\n", *counter_ptr, counter_ptr >= counters && counter_ptr < counters + 64 ? "yes" : "no");
    printf("%s (%zu chars)\n", greeting, strlen(greeting));
    printf("abs_ptr == &counters[7]: %s, *abs_ptr = %d, abs_load(9) = %d\n", abs_ptr() == &counters[7] ? "yes" : "no", *abs_ptr(), abs_load(9));
    return 0;
}
