// Test DLL: exports functions and data, has DllMain, relocated pointers, and calls back into the exe.
#include <windows.h>
#include <stdio.h>

static int attached;
static const char *greeting = "hello from mathlib.dll";      // pointer in .data: needs a base relocation
static int (*ops[3])(int, int);                                // filled at runtime: relocated function pointers
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int sub(int a, int b) { return a - b; }

__declspec(dllexport) int mathlib_counter = 100;
__declspec(dllexport) int mathlib_apply(int op, int a, int b) { return ops[op % 3](a, b); }
__declspec(dllexport) const char *mathlib_greeting(void) { return greeting; }
__declspec(dllexport) int mathlib_attached(void) { return attached; }
__declspec(dllexport) int mathlib_fold(int (*cb)(int, int), const int *v, int n) { int r = v[0]; for (int i = 1; i < n; i++) r = cb(r, v[i]); return r; }
__declspec(dllexport) void *mathlib_where(void) { HMODULE m = 0; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&mathlib_where, &m); return m; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) { ops[0] = add; ops[1] = mul; ops[2] = sub; attached++; mathlib_counter += 1; (void)h; }
    return TRUE;
}
