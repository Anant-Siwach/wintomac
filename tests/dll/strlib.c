// Second test DLL, loaded only via LoadLibrary/GetProcAddress; imports from mathlib.dll itself.
#include <windows.h>
__declspec(dllimport) int mathlib_apply(int op, int a, int b);
static int inited;
__declspec(dllexport) int strlib_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
__declspec(dllexport) int strlib_score(const char *s) { int h = 0; for (; *s; s++) h = mathlib_apply(1, h, 31) + *s; return h + inited * 1000000; }
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { (void)h; (void)r; if (reason == DLL_PROCESS_ATTACH) inited = 1; return TRUE; }
