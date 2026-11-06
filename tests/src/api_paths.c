// Paths and files: GetFullPathName normalisation, the current directory, temp names, drive types, moves,
// file mappings, extended file information, locks, shlwapi Path* helpers and the CRT path/stat/find functions.
// Expected output (tests/expected/api_paths.out) follows Windows behaviour; absolute paths are compared, not printed.
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>

static void full(const char *in) {
    char out[MAX_PATH]; char *fp = NULL;
    DWORD n = GetFullPathNameA(in, sizeof out, out, &fp);
    printf("full '%s' -> '%s' (%lu) file=%s\n", in, out, n, fp ? fp : "(null)");
}
static void make(const char *p, const char *text) {
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); DWORD w;
    WriteFile(h, text, (DWORD)strlen(text), &w, NULL); CloseHandle(h);
}
static int ends_with(const char *s, const char *t) { size_t a = strlen(s), b = strlen(t); return a >= b && !_stricmp(s + a - b, t); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    full("C:\\x\\y\\..\\z."); full("C:/a//b/"); full("\\\\server\\share\\x\\..\\y"); full("C:\\a\\.\\b\\..\\..\\..\\c");
    char cwd[MAX_PATH], out[MAX_PATH], exp[MAX_PATH];
    GetCurrentDirectoryA(sizeof cwd, cwd);
    GetFullPathNameA("a\\..\\b\\.\\c.txt", sizeof out, out, NULL);
    snprintf(exp, sizeof exp, "%s%sb\\c.txt", cwd, cwd[strlen(cwd) - 1] == '\\' ? "" : "\\");
    printf("relative resolves against cwd: %s\n", !strcmp(out, exp) ? "yes" : out);
    printf("too small buffer -> %lu\n", GetFullPathNameA("C:\\abc", 3, out, NULL));

    RemoveDirectoryA("ptest\\d"); CreateDirectoryA("ptest", NULL);
    printf("chdir ptest: %d\n", SetCurrentDirectoryA("ptest"));
    GetCurrentDirectoryA(sizeof out, out); printf("cwd ends with \\ptest: %s\n", ends_with(out, "\\ptest") ? "yes" : out);
    make("f.txt", "0123456789");
    printf("chdir ..: %d\n", SetCurrentDirectoryA(".."));
    GetCurrentDirectoryA(sizeof out, out); printf("cwd restored: %s\n", !strcmp(out, cwd) ? "yes" : out);
    printf("file seen from parent: %s\n", GetFileAttributesA("ptest\\f.txt") != INVALID_FILE_ATTRIBUTES ? "yes" : "no");
    { SetLastError(0); BOOL ok = SetCurrentDirectoryA("no_such_dir"); DWORD e = GetLastError(); printf("chdir missing: %d err=%lu\n", ok, e); }

    { UINT u = GetTempFileNameA("ptest", "abc", 0x1234, out); printf("temp name: %u '%s'\n", u, out); }
    { UINT d1 = GetDriveTypeA("C:\\"), d2 = GetDriveTypeA("Q:\\"); printf("drive C: %u, Q: %u\n", d1, d2); }
    printf("long path of existing: %s\n", GetLongPathNameA("ptest\\f.txt", out, sizeof out) ? out : "fail");
    { SetLastError(0); DWORD n = GetLongPathNameA("ptest\\nope.txt", out, sizeof out); DWORD e = GetLastError(); printf("long path of missing: %lu err=%lu\n", n, e); }
    ULARGE_INTEGER avail, total, freeb; printf("disk free: %d\n", GetDiskFreeSpaceExA("C:\\", &avail, &total, &freeb) && total.QuadPart > 0);

    make("ptest\\g.txt", "g");
    { SetLastError(0); BOOL ok = MoveFileA("ptest\\g.txt", "ptest\\f.txt"); DWORD e = GetLastError(); printf("MoveFile onto existing: %d err=%lu\n", ok, e); }
    make("ptest\\h.txt", "hhhh");
    printf("MoveFileEx replace: %d\n", MoveFileExA("ptest\\h.txt", "ptest\\g.txt", MOVEFILE_REPLACE_EXISTING));

    HANDLE h = CreateFileA("ptest\\f.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE m = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    const char *v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    printf("file mapping: %c%c%c\n", v[0], v[3], v[9]);
    { BOOL ok = UnmapViewOfFile(v); printf("unmap: %d\n", ok); } CloseHandle(m);
    HANDLE a = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "wtm_shared");
    char *p1 = MapViewOfFile(a, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    HANDLE a2 = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "wtm_shared");
    char *p2 = MapViewOfFile(a2, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    strcpy(p1, "shared"); printf("anonymous named mapping: '%s'\n", p2);
    UnmapViewOfFile(p1); UnmapViewOfFile(p2); CloseHandle(a); CloseHandle(a2);
    { SetLastError(0); HANDLE x = OpenFileMappingA(FILE_MAP_READ, FALSE, "wtm_nope"); DWORD e = GetLastError(); printf("open missing mapping: %p err=%lu\n", (void *)x, e); }

    FILE_STANDARD_INFO si; GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof si);
    printf("standard info: size=%lld dir=%d links=%lu\n", si.EndOfFile.QuadPart, si.Directory, si.NumberOfLinks);
    struct { FILE_NAME_INFO i; WCHAR more[MAX_PATH]; } ni;
    GetFileInformationByHandleEx(h, FileNameInfo, &ni, sizeof ni);
    ni.i.FileName[ni.i.FileNameLength / 2] = 0;
    printf("name info ends with \\ptest\\f.txt: %s\n", ni.i.FileNameLength >= 24 && !_wcsicmp(ni.i.FileName + ni.i.FileNameLength / 2 - 12, L"\\ptest\\f.txt") ? "yes" : "no");
    FILE_END_OF_FILE_INFO eof = { .EndOfFile.QuadPart = 4 };
    { BOOL ok = SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof eof); printf("set end of file: %d", ok); }
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz); printf(" -> %lld\n", sz.QuadPart);
    OVERLAPPED ov = { 0 };
    { BOOL l = LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 4, 0, &ov); BOOL u = UnlockFileEx(h, 0, 4, 0, &ov); printf("lock: %d unlock: %d\n", l, u); }
    CloseHandle(h);

    strcpy(out, "C:\\a\\b"); PathAppendA(out, "..\\c"); printf("PathAppend: %s\n", out);
    PathCombineA(out, "C:\\dir", "sub\\file.txt"); printf("PathCombine: %s\n", out);
    PathCombineA(out, "C:\\dir", "\\root.txt"); printf("PathCombine rooted: %s\n", out);
    printf("PathFindExtension: %s  PathFindFileName: %s\n", PathFindExtensionA("x\\file.tar.gz"), PathFindFileNameA("x\\file.tar.gz"));
    strcpy(out, "C:\\a\\b.txt"); PathRemoveFileSpecA(out); printf("PathRemoveFileSpec: %s", out);
    strcpy(out, "C:\\b.txt"); PathRemoveFileSpecA(out); printf(" | %s\n", out);
    { BOOL a1 = PathIsRelativeA("a\\b"), a2 = PathIsRelativeA("C:\\a"), a3 = PathIsRelativeA("\\a"); printf("PathIsRelative: %d %d %d\n", a1, a2, a3); }
    strcpy(out, "C:\\a\\name.ext"); PathRemoveExtensionA(out); printf("PathRemoveExtension: %s\n", out);
    strcpy(out, "C:\\a"); PathAddBackslashA(out); printf("PathAddBackslash: %s", out); PathRemoveBackslashA(out); printf(" | %s\n", out);
    PathCanonicalizeA(out, "C:\\a\\.\\b\\..\\c"); printf("PathCanonicalize: %s\n", out);
    { BOOL a1 = PathFileExistsA("ptest\\f.txt"), a2 = PathFileExistsA("ptest\\zz"); printf("PathFileExists: %d %d\n", a1, a2); }

    char *fp = _fullpath(NULL, "C:\\q\\..\\r", 0); printf("_fullpath: %s\n", fp); free(fp);
    char dr[8], di[64], fn[64], ex[16];
    _splitpath_s("C:\\dir\\sub\\name.ext", dr, sizeof dr, di, sizeof di, fn, sizeof fn, ex, sizeof ex);
    printf("_splitpath_s: '%s' '%s' '%s' '%s'\n", dr, di, fn, ex);
    _makepath_s(out, sizeof out, "D", "\\x\\y", "file", "txt"); printf("_makepath_s: %s\n", out);
    struct _stat64 st;
    { int r = _stat64("ptest\\f.txt", &st); printf("_stat64 file: %d size=%lld mode=%x\n", r, st.st_size, st.st_mode); }
    { int r = _stat64("ptest", &st); printf("_stat64 dir: %d mode=%x\n", r, st.st_mode); }
    printf("_stat64 trailing slash: %d\n", _stat64("ptest\\f.txt\\", &st));
    { int a1 = _access("ptest\\f.txt", 0), a2 = _access("ptest\\zz", 0); printf("_access: %d %d\n", a1, a2); }
    char *cw = _getcwd(NULL, 0); printf("_getcwd matches: %s\n", !strcmp(cw, cwd) ? "yes" : cw); free(cw);
    struct _finddata64i32_t fd; intptr_t fh = _findfirst64i32("ptest\\*.txt", &fd); int nf = 0, sum = 0;
    if (fh != -1) { do { nf++; sum += (int)fd.size; } while (_findnext64i32(fh, &fd) == 0); _findclose(fh); }
    printf("_findfirst *.txt: %d files, %d bytes\n", nf, sum);
    { int a1 = _mkdir("ptest\\d"); int a2 = _rmdir("ptest\\d"); printf("_mkdir/_rmdir: %d %d\n", a1, a2); }
    { int r = _unlink("ptest\\zz"); printf("_unlink missing: %d errno=%d\n", r, errno); }

    DeleteFileA("ptest\\f.txt"); DeleteFileA("ptest\\g.txt"); DeleteFileA("ptest\\abc1234.tmp");
    printf("cleanup: %d\n", RemoveDirectoryA("ptest"));
    return 0;
}
