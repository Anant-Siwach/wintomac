// File enumeration and overlapped I/O: FindFirstFile/FindNextFile with wildcards (Windows rules: "*.*" matches
// everything, case-insensitive), file times + info, overlapped ReadFile at offsets with event, EOF, copy, cleanup.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int cmp(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }
static void list(const char *pattern) {
    WIN32_FIND_DATAA fd; char *names[64]; int n = 0;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("%-22s -> none (error %lu)\n", pattern, GetLastError()); return; }
    do { char b[300]; snprintf(b, sizeof b, "%s%s", fd.cFileName, fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? "/" : ""); names[n++] = _strdup(b); }
    while (FindNextFileA(h, &fd) && n < 64);
    DWORD last = GetLastError(); FindClose(h);
    qsort(names, n, sizeof *names, cmp);
    printf("%-22s ->", pattern); for (int i = 0; i < n; i++) { printf(" %s", names[i]); free(names[i]); } printf("  (end: %s)\n", last == ERROR_NO_MORE_FILES ? "NO_MORE_FILES" : "?");
}
static void make(const char *path, const char *text) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w; WriteFile(h, text, (DWORD)strlen(text), &w, NULL); CloseHandle(h);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    CreateDirectoryA("fstest", NULL); CreateDirectoryA("fstest\\Sub", NULL);
    make("fstest\\alpha.txt", "0123456789abcdefghij"); make("fstest\\Beta.TXT", "beta"); make("fstest\\gamma.dat", "g"); make("fstest\\noext", "n");
    make("fstest\\Sub\\deep.txt", "deep");
    list("fstest\\*"); list("fstest\\*.*"); list("fstest\\*.txt"); list("FSTEST\\?????.txt"); list("fstest\\sub\\*"); list("fstest\\*.nothing");

    HANDLE h = CreateFileA("fstest\\alpha.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    BY_HANDLE_FILE_INFORMATION bi; GetFileInformationByHandle(h, &bi);
    printf("info: size=%lu dir=%d links=%lu\n", bi.nFileSizeLow, !!(bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), bi.nNumberOfLinks);
    FILETIME wt; GetFileTime(h, NULL, NULL, &wt); SYSTEMTIME st; FileTimeToSystemTime(&wt, &st);
    SYSTEMTIME now; GetSystemTime(&now);
    printf("write time is this year: %s\n", st.wYear == now.wYear ? "yes" : "no");
    OVERLAPPED ov = { 0 }; ov.Offset = 10; ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    char buf[16] = { 0 }; DWORD got = 0;
    BOOL ok = ReadFile(h, buf, 5, NULL, &ov);
    BOOL pending = !ok && GetLastError() == ERROR_IO_PENDING;
    DWORD wr = WaitForSingleObject(ov.hEvent, 1000);
    GetOverlappedResult(h, &ov, &got, TRUE);
    printf("overlapped read @10: '%s' (%lu bytes, %s, event %s)\n", buf, got, ok || pending ? "accepted" : "FAILED", wr == WAIT_OBJECT_0 ? "signalled" : "not signalled");
    OVERLAPPED ov2 = { 0 }; ov2.Offset = 100;
    ok = ReadFile(h, buf, 5, &got, &ov2);
    printf("read past end: %s\n", !ok && GetLastError() == ERROR_HANDLE_EOF ? "ERROR_HANDLE_EOF" : "unexpected");
    CloseHandle(ov.hEvent); CloseHandle(h);

    printf("copy (no overwrite) to existing: %s\n", !CopyFileA("fstest\\gamma.dat", "fstest\\noext", TRUE) && GetLastError() == ERROR_FILE_EXISTS ? "refused" : "unexpected");
    printf("copy: %s\n", CopyFileA("fstest\\alpha.txt", "fstest\\copy.txt", FALSE) ? "ok" : "FAILED");
    printf("rmdir non-empty: %s\n", !RemoveDirectoryA("fstest\\Sub") && GetLastError() == ERROR_DIR_NOT_EMPTY ? "ERROR_DIR_NOT_EMPTY" : "unexpected");
    const char *files[] = { "fstest\\alpha.txt", "fstest\\Beta.TXT", "fstest\\gamma.dat", "fstest\\noext", "fstest\\copy.txt", "fstest\\Sub\\deep.txt" };
    for (int i = 0; i < 6; i++) DeleteFileA(files[i]);
    printf("cleanup: %s\n", RemoveDirectoryA("fstest\\Sub") && RemoveDirectoryA("fstest") ? "ok" : "FAILED");
    return 0;
}
