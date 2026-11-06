// Multi-image test: static import of mathlib.dll, dynamic LoadLibrary of strlib.dll, callbacks across images.
#include <windows.h>
#include <stdio.h>
__declspec(dllimport) int mathlib_counter;
__declspec(dllimport) int mathlib_apply(int, int, int);
__declspec(dllimport) const char *mathlib_greeting(void);
__declspec(dllimport) int mathlib_attached(void);
__declspec(dllimport) int mathlib_fold(int (*)(int, int), const int *, int);
__declspec(dllimport) void *mathlib_where(void);
static int maxf(int a, int b) { return a > b ? a : b; }

int main(void) {
    printf("%s\n", mathlib_greeting());
    printf("attached=%d counter=%d\n", mathlib_attached(), mathlib_counter);
    printf("apply: %d %d %d\n", mathlib_apply(0, 20, 22), mathlib_apply(1, 6, 7), mathlib_apply(2, 50, 8));
    int v[] = { 3, 41, 7, 19, 2 };
    printf("fold(max via exe callback) = %d\n", mathlib_fold(maxf, v, 5));
    HMODULE m = GetModuleHandleA("mathlib.dll");
    printf("GetModuleHandle matches FROM_ADDRESS: %s\n", m && m == (HMODULE)mathlib_where() ? "yes" : "no");
    HMODULE s = LoadLibraryA("strlib.dll");
    int (*len)(const char *) = (int (*)(const char *))GetProcAddress(s, "strlib_len");
    int (*score)(const char *) = (int (*)(const char *))GetProcAddress(s, "strlib_score");
    printf("strlib: loaded=%s len=%d score=%d\n", s ? "yes" : "no", len ? len("wintomac") : -1, score ? score("abc") : -1);
    printf("missing export -> %p\n", (void *)GetProcAddress(s, "no_such_function"));
    int *ctr = (int *)GetProcAddress(m, "mathlib_counter");
    printf("data export via GetProcAddress = %d\n", ctr ? *ctr : -1);
    return 0;
}
