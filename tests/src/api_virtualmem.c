// VirtualAlloc / VirtualFree / MapViewOfFileEx address-space rules, and that breaking them can't hurt the heap.
// Regression test for the GZDoom crash class: the old shims returned some other address for a busy requested
// address, munmap()ed 64 KB at whatever address VirtualFree(MEM_RELEASE) got (a double release, a heap pointer),
// and mapped views with MAP_FIXED, so pages macOS malloc owned could be unmapped or replaced under live blocks.
// Expected output (tests/expected/api_virtualmem.out) is what Windows prints.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int heap_check(void) {   // allocate/fill/verify/free a spread of sizes: dies or returns 0 on a damaged heap
    enum { N = 4000 };
    static unsigned char *b[N];
    for (int i = 0; i < N; i++) { size_t n = 16 + (i * 37) % 4000; b[i] = malloc(n); if (!b[i]) return 0; memset(b[i], i & 0xFF, n); }
    for (int i = 0; i < N; i++) { size_t n = 16 + (i * 37) % 4000; for (size_t k = 0; k < n; k++) if (b[i][k] != (i & 0xFF)) return 0; }
    for (int i = 0; i < N; i += 2) free(b[i]);
    for (int i = 1; i < N; i += 2) free(b[i]);
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // 1. a big executable region (GZDoom's ZScript JIT allocates 1 MB blocks like this) is released completely
    unsigned char *x = VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    printf("alloc rwx: %d\n", x != NULL);
    x[0] = 0xB8; x[1] = 42; x[2] = x[3] = x[4] = 0; x[5] = 0xC3;   // mov eax, 42; ret
    x[(1 << 20) - 1] = 1;
    printf("call jit code: %d\n", ((int (*)(void))x)());
    printf("release: %d\n", VirtualFree(x, 0, MEM_RELEASE));
    void *again = VirtualAlloc(x, 1 << 20, MEM_RESERVE, PAGE_READWRITE);   // the whole MB must be free again
    printf("re-reserve same address: %d\n", again == x);
    if (again) VirtualFree(again, 0, MEM_RELEASE);

    // 2. releasing twice must fail, not unmap whatever the heap put there meanwhile
    void *r = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    printf("release once: %d\n", VirtualFree(r, 0, MEM_RELEASE));
    void *blocks[256]; for (int i = 0; i < 256; i++) { blocks[i] = malloc(144); memset(blocks[i], 0x5A, 144); }
    SetLastError(0); BOOL ok = VirtualFree(r, 0, MEM_RELEASE);
    printf("release twice: %d err=%lu\n", ok, GetLastError());
    int intact = 1; for (int i = 0; i < 256; i++) { for (int k = 0; k < 144; k++) intact &= ((unsigned char *)blocks[i])[k] == 0x5A; free(blocks[i]); }
    printf("heap blocks intact: %d\n", intact);

    // 3. heap memory is not VirtualAlloc's to give out or take back
    char *h = malloc(144); memset(h, 0x33, 144);
    char *page = (char *)((ULONG_PTR)h & ~(ULONG_PTR)0xFFFF);
    SetLastError(0);
    void *over = VirtualAlloc(page, 65536, MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    printf("reserve over heap: %d err=%lu\n", over != NULL, GetLastError());
    SetLastError(0); ok = VirtualFree(h, 0, MEM_RELEASE);
    printf("release heap pointer: %d err=%lu\n", ok, GetLastError());
    SetLastError(0); ok = VirtualFree(page, 4096, MEM_RELEASE);
    printf("release with size: %d err=%lu\n", ok, GetLastError());
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, NULL);
    char path[MAX_PATH], tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp); GetTempFileNameA(tmp, "wtm", 0, path);
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    HANDLE fm = CreateFileMappingA(f, NULL, PAGE_READWRITE, 0, 65536, NULL);
    SetLastError(0);
    void *v = MapViewOfFileEx(fm, FILE_MAP_WRITE, 0, 0, 65536, page);
    printf("map view over heap: %d err=%lu\n", v != NULL, GetLastError());
    if (v) UnmapViewOfFile(v);
    CloseHandle(fm); CloseHandle(f); CloseHandle(m);
    int same = 1; for (int k = 0; k < 144; k++) same &= h[k] == 0x33;
    printf("heap block intact: %d\n", same);
    free(h);

    // 4. decommit gives zero pages back on recommit
    unsigned char *d = VirtualAlloc(NULL, 65536, MEM_RESERVE, PAGE_NOACCESS);
    VirtualAlloc(d, 8192, MEM_COMMIT, PAGE_READWRITE); memset(d, 0xEE, 8192);
    printf("decommit: %d\n", VirtualFree(d, 4096, MEM_DECOMMIT));
    VirtualAlloc(d, 4096, MEM_COMMIT, PAGE_READWRITE);
    printf("recommitted page: %02x, untouched page: %02x\n", d[100], d[4096 + 100]);
    printf("release reservation: %d\n", VirtualFree(d, 0, MEM_RELEASE));

    printf("heap ok: %d\n", heap_check());
    return 0;
}
