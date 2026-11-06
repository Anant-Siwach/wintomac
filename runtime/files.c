// File APIs with Windows -> macOS path translation.
//
// Path model:
//   C:\wintomac\<x>     -> directory of the program (what GetModuleFileName reports)
//   <drive>:\<path>     -> $WINTOMAC_ROOT/<path>   (default: the program's directory)
//   relative paths      -> relative to the host cwd
// Backslashes become slashes; each component is matched case-insensitively against what exists on disk,
// because Windows filesystems are case-insensitive and games are inconsistent about case.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "win32.h"

static char root_dir[1024];

void rt_files_init(const char *exe_dir) {
    const char *r = getenv("WINTOMAC_ROOT");
    snprintf(root_dir, sizeof root_dir, "%s", r && *r ? r : exe_dir);
}

static void wide_to_utf8(u64 w, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; LD16(w + 2 * i) && o + 4 < n; i++) {
        u32 cp = LD16(w + 2 * i);
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = 0xC0 | cp >> 6; out[o++] = 0x80 | (cp & 63); }
        else { out[o++] = 0xE0 | cp >> 12; out[o++] = 0x80 | (cp >> 6 & 63); out[o++] = 0x80 | (cp & 63); }
    }
    out[o] = 0;
}

// Resolve each component case-insensitively; components that don't exist are kept as given.
static void ci_resolve(char *path) {
    char out[2048] = "", *save = NULL;
    char tmp[2048]; snprintf(tmp, sizeof tmp, "%s", path);
    int abs = tmp[0] == '/';
    if (abs) strcpy(out, "/");
    for (char *comp = strtok_r(tmp, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
        char cand[2048];
        snprintf(cand, sizeof cand, "%s%s%s", out, (*out && out[strlen(out) - 1] != '/') ? "/" : "", comp);
        struct stat st;
        if (stat(cand, &st) != 0) {
            DIR *d = opendir(*out ? out : ".");
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) if (!strcasecmp(e->d_name, comp)) {
                    snprintf(cand, sizeof cand, "%s%s%s", out, (*out && out[strlen(out) - 1] != '/') ? "/" : "", e->d_name);
                    break;
                }
                closedir(d);
            }
        }
        snprintf(out, sizeof out, "%s", cand);
    }
    if (*out) strcpy(path, out);
}

void rt_host_path(const char *win, char *out, size_t n) {
    char p[2048]; size_t k = 0;
    for (const char *s = win; *s && k + 1 < sizeof p; s++) p[k++] = *s == '\\' ? '/' : *s;
    p[k] = 0;
    const char *rest = p;
    if (!strncasecmp(p, "//?/", 4) || !strncasecmp(p, "//./", 4)) rest = p + 4;  // \\?\C:\... prefixes
    if (!strcasecmp(rest, "NUL") || !strcasecmp(rest, "NUL:")) { snprintf(out, n, "/dev/null"); return; }   // the null device
    if (((rest[0] | 32) >= 'a' && (rest[0] | 32) <= 'z') && rest[1] == ':') {
        const char *tail = rest + 2; while (*tail == '/') tail++;
        if ((rest[0] | 32) == 'z') snprintf(out, n, "/%s", tail);   // Z:\ is the host root (Wine's convention)
        else if (!strncasecmp(tail, "wintomac/", 9)) snprintf(out, n, "%s/%s", rt_exe_dir(), tail + 9);
        else if (!strcasecmp(tail, "wintomac")) snprintf(out, n, "%s", rt_exe_dir());   // the bare program directory (std::filesystem::canonical(current_path()))
        else if (!strncasecmp(tail, "users/", 6)) {   // profile folders (AppData, Saved Games, Documents) -> per-user Mac storage
            char base[1100]; snprintf(base, sizeof base, "%s", rt_user_data_dir());   // per game when packaged (appdata.c)
            mkdir(base, 0755);
            snprintf(out, n, "%s/%s", base, tail);
            // create intermediate directories so games can write saves without creating every parent themselves
            char tmp[2048]; snprintf(tmp, sizeof tmp, "%s", out);
            for (char *q = tmp + strlen(base) + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
        }
        else snprintf(out, n, "%s/%s", root_dir, tail);
    } else {
        snprintf(out, n, "%s", rest);
    }
    ci_resolve(out);
}

// ---- CreateFile & friends -----------------------------------------------------------
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
enum { CREATE_NEW = 1, CREATE_ALWAYS, OPEN_EXISTING, OPEN_ALWAYS, TRUNCATE_EXISTING };
extern void rt_set_last_error(u32 e);

static u32 errno_to_win(int e) {
    switch (e) { case ENOENT: return 2; case ENOTDIR: return 3; case EACCES: case EPERM: return 5; case EEXIST: return 80;
                 case ENOSPC: return 112; default: return 31; }
}

static u64 create_file(Ctx *c, const char *wpath) {
    char path[2048]; rt_host_path(wpath, path, sizeof path);
    u32 access_ = (u32)A2, disp = (u32)AN(5);
    int fl = (access_ & GENERIC_WRITE) ? ((access_ & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (access_ & 0x10000000u) fl = O_RDWR;  // GENERIC_ALL
    switch (disp) {
    case CREATE_NEW: fl |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: fl |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: fl |= O_CREAT; break;
    case TRUNCATE_EXISTING: fl |= O_TRUNC; break;
    default: break;
    }
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {  // directory handles (FILE_FLAG_BACKUP_SEMANTICS)
        int fd = open(path, O_RDONLY);
        if (fd >= 0) return rt_handle_new(H_FILE, fd);
    }
    int fd = open(path, fl | O_CLOEXEC, 0644);
    if (rt_trace_enabled()) rt_log("CreateFile(%s -> %s) = %d", wpath, path, fd);
    if (fd < 0) { rt_set_last_error(errno_to_win(errno)); return (u64)-1; }
    rt_set_last_error((disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) && (fl & O_CREAT) ? 0 : 0);
    return rt_handle_new(H_FILE, fd);
}
SHIM(CreateFileA) { RET(create_file(c, (const char *)A1)); }
SHIM(CreateFileW) { char p[2048]; wide_to_utf8(A1, p, sizeof p); RET(create_file(c, p)); }

static int fd_of(u64 h) { intptr_t fd; return rt_handle_get(h, H_FILE, &fd) ? (int)fd : -1; }
SHIM(SetFilePointerEx) {  // (h, i64 dist, *newpos, method)
    int fd = fd_of(A1); if (fd < 0) RET(0);
    off_t r = lseek(fd, (off_t)(i64)A2, (int)(u32)A4);
    if (r < 0) RET(0);
    if (A3) ST64(A3, (u64)r);
    RET(1);
}
SHIM(SetFilePointer) {  // (h, i32 lo, *i32 hi, method)
    int fd = fd_of(A1); if (fd < 0) RET(0xFFFFFFFF);
    i64 d = (i64)(i32)A2; if (A3) d = (i64)(((u64)LD32(A3) << 32) | (u32)A2);
    off_t r = lseek(fd, (off_t)d, (int)(u32)A4);
    if (r < 0) RET(0xFFFFFFFF);
    if (A3) ST32(A3, (u32)((u64)r >> 32));
    RET((u32)r);
}
SHIM(GetFileSizeEx) { int fd = fd_of(A1); struct stat st; if (fd < 0 || fstat(fd, &st)) RET(0); ST64(A2, (u64)st.st_size); RET(1); }
SHIM(GetFileSize) { int fd = fd_of(A1); struct stat st; if (fd < 0 || fstat(fd, &st)) RET(0xFFFFFFFF); if (A2) ST32(A2, (u32)((u64)st.st_size >> 32)); RET((u32)st.st_size); }
SHIM(FlushFileBuffers) { int fd = fd_of(A1); RET(fd >= 0 && fsync(fd) == 0); }
SHIM(SetEndOfFile) { int fd = fd_of(A1); if (fd < 0) RET(0); off_t p = lseek(fd, 0, SEEK_CUR); RET(ftruncate(fd, p) == 0); }

static u64 attrs_for(const char *wpath) {
    char path[2048]; rt_host_path(wpath, path, sizeof path);
    struct stat st;
    if (stat(path, &st)) { rt_set_last_error(2); return 0xFFFFFFFF; }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;  // FILE_ATTRIBUTE_DIRECTORY / NORMAL
}
SHIM(GetFileAttributesA) { RET(attrs_for((const char *)A1)); }
SHIM(GetFileAttributesW) { char p[2048]; wide_to_utf8(A1, p, sizeof p); RET(attrs_for(p)); }
SHIM(GetFileAttributesExW) {  // (path, level, WIN32_FILE_ATTRIBUTE_DATA*)
    char p[2048], path[2048]; wide_to_utf8(A1, p, sizeof p); rt_host_path(p, path, sizeof path);
    struct stat st; if (stat(path, &st)) { rt_set_last_error(2); RET(0); }
    u64 d = A3; memset((void *)d, 0, 36);
    ST32(d, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    u64 ft = ((u64)st.st_mtime + 11644473600ull) * 10000000ull;
    for (int i = 0; i < 3; i++) { ST32(d + 4 + 8 * i, (u32)ft); ST32(d + 8 + 8 * i, (u32)(ft >> 32)); }
    ST32(d + 28, (u32)((u64)st.st_size >> 32)); ST32(d + 32, (u32)st.st_size);
    RET(1);
}
static int mkdir_win(const char *wpath) { char path[2048]; rt_host_path(wpath, path, sizeof path); if (mkdir(path, 0755) && errno != EEXIST) { rt_set_last_error(errno_to_win(errno)); return 0; } if (errno == EEXIST) rt_set_last_error(183); return 1; }
SHIM(CreateDirectoryA) { RET(mkdir_win((const char *)A1)); }
SHIM(CreateDirectoryW) { char p[2048]; wide_to_utf8(A1, p, sizeof p); RET(mkdir_win(p)); }
SHIM(DeleteFileW) { char p[2048], path[2048]; wide_to_utf8(A1, p, sizeof p); rt_host_path(p, path, sizeof path); RET(unlink(path) == 0); }
SHIM(DeleteFileA) { char path[2048]; rt_host_path((const char *)A1, path, sizeof path); RET(unlink(path) == 0); }
SHIM(MoveFileExW) {
    char a[2048], b[2048], pa[2048], pb[2048]; wide_to_utf8(A1, a, sizeof a); wide_to_utf8(A2, b, sizeof b);
    rt_host_path(a, pa, sizeof pa); rt_host_path(b, pb, sizeof pb); RET(rename(pa, pb) == 0);
}
const char *rt_win_cwd(void);   // kernel32x.c: tracks SetCurrentDirectory
SHIM(GetCurrentDirectoryW) {  // the Windows-side current directory (C:\ = program root until SetCurrentDirectory)
    const char *p = rt_win_cwd(); char w[2048]; size_t n = strlen(p);
    if ((u32)A1 <= n) RET(n + 1);
    snprintf(w, sizeof w, "%s", p); for (size_t i = 0; i < n; i++) ST16(A2 + 2 * i, (u8)w[i]); ST16(A2 + 2 * n, 0); RET(n);
}
SHIM(GetTempPathW) {
    const char *p = "C:\\tmp\\"; size_t n = strlen(p);
    char host[2048]; rt_host_path("C:\\tmp", host, sizeof host); mkdir(host, 0755);
    if ((u32)A1 <= n) RET(n + 1);
    for (size_t i = 0; i < n; i++) ST16(A2 + 2 * i, (u8)p[i]); ST16(A2 + 2 * n, 0); RET(n);
}

// ---- msvcrt FILE* on translated paths -------------------------------------------------
SHIM(fopen_win) { char path[2048]; rt_host_path((const char *)A1, path, sizeof path); RET(fopen(path, (const char *)A2)); }
SHIM(_wfopen) {
    char p[2048], m[16], path[2048]; wide_to_utf8(A1, p, sizeof p); wide_to_utf8(A2, m, sizeof m);
    rt_host_path(p, path, sizeof path); RET(fopen(path, m));
}
SHIM(_fdopen) { RET(fdopen((int)A1, (const char *)A2)); }
SHIM(_filelengthi64) { struct stat st; RET(fstat((int)A1, &st) ? (u64)-1 : (u64)st.st_size); }
SHIM(_fstat64) {  // struct _stat64 (56 bytes): dev u32, ino u16, mode u16, nlink i16, uid i16, gid i16, rdev u32, size i64 @24, times @32
    struct stat st; if (fstat((int)A1, &st)) RET((u64)-1);
    u64 p = A2; memset((void *)p, 0, 56);
    u16 mode = S_ISDIR(st.st_mode) ? 0x4000 : 0x8000; mode |= 0x180;
    ST16(p + 6, mode); ST16(p + 8, 1); ST64(p + 24, (u64)st.st_size);
    ST64(p + 32, (u64)st.st_atime); ST64(p + 40, (u64)st.st_mtime); ST64(p + 48, (u64)st.st_ctime);
    RET(0);
}
SHIM(_lseeki64) { RET((u64)lseek((int)A1, (off_t)A2, (int)A3)); }
SHIM(_open) { char path[2048]; rt_host_path((const char *)A1, path, sizeof path); int f = (int)A2, fl = f & 3;
              if (f & 0x100) fl |= O_CREAT; if (f & 0x200) fl |= O_TRUNC; if (f & 0x400) fl |= O_EXCL; if (f & 8) fl |= O_APPEND;
              RET((u32)open(path, fl, 0644)); }
SHIM(_close) { RET((u32)close((int)A1)); }

// ---- directory enumeration (FindFirstFile*) -------------------------------------------------------
#include <fnmatch.h>
#define H_FIND 7
typedef struct { char **names; int n, i; char dir[2048]; } Find;
static int wild_match(const char *pat, const char *name) {
    if (!strcmp(pat, "*") || !strcmp(pat, "*.*")) return 1;                 // Windows: *.* matches names without a dot too
    char p[512]; size_t k = 0;
    for (const char *s = pat; *s && k + 2 < sizeof p; s++) { if (*s == '[' || *s == ']') p[k++] = '\\'; p[k++] = *s; }
    p[k] = 0;
    return fnmatch(p, name, FNM_CASEFOLD) == 0;
}
static u64 ft_from(time_t t) { return ((u64)t + 11644473600ull) * 10000000ull; }
// WIN32_FIND_DATA{W,A}: attrs, 3 FILETIMEs, size hi/lo, 2 reserved, cFileName[260], cAlternateFileName[14]
static void fill_find(u64 d, const char *dir, const char *name, int wide) {
    char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, name);
    struct stat st; if (stat(path, &st)) memset(&st, 0, sizeof st);
    memset((void *)d, 0, wide ? 592 : 320);
    u32 attr = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    if (name[0] == '.' && strcmp(name, ".") && strcmp(name, "..")) attr |= 0x2;   // FILE_ATTRIBUTE_HIDDEN for dotfiles
    if (!(st.st_mode & S_IWUSR)) attr = (attr & ~0x80u) | 0x1;                    // READONLY
    ST32(d, attr);
    u64 t[3] = { ft_from(st.st_birthtimespec.tv_sec), ft_from(st.st_atime), ft_from(st.st_mtime) };
    for (int i = 0; i < 3; i++) ST64(d + 4 + 8 * i, t[i]);
    if (!S_ISDIR(st.st_mode)) { ST32(d + 28, (u32)((u64)st.st_size >> 32)); ST32(d + 32, (u32)st.st_size); }
    if (wide) { size_t i = 0; for (; name[i] && i < 259; i++) ST16(d + 44 + 2 * i, (u8)name[i]); ST16(d + 44 + 2 * i, 0); }
    else { strncpy((char *)d + 44, name, 259); }
}
static u64 find_first(const char *wpattern, u64 data, int wide) {
    char pat[2048]; snprintf(pat, sizeof pat, "%s", wpattern);
    for (char *p = pat; *p; p++) if (*p == '/') *p = '\\';
    char *slash = strrchr(pat, '\\');
    const char *mask = slash ? slash + 1 : pat;
    char windir[2048];
    if (slash) { size_t n = (size_t)(slash - pat); memcpy(windir, pat, n); windir[n] = 0; if (n == 2 && windir[1] == ':') strcat(windir, "\\"); }
    else strcpy(windir, ".");
    Find *f = calloc(1, sizeof *f);
    rt_host_path(windir, f->dir, sizeof f->dir);
    DIR *d = opendir(f->dir);
    if (!d) { free(f); rt_set_last_error(3); return (u64)-1; }   // ERROR_PATH_NOT_FOUND
    int is_root = !strcmp(f->dir, "/");
    if (!is_root) {   // Windows lists "." and ".." in non-root directories
        for (int k = 0; k < 2; k++) if (wild_match(mask, k ? ".." : ".")) { f->names = realloc(f->names, sizeof(char *) * (f->n + 1)); f->names[f->n++] = strdup(k ? ".." : "."); }
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!wild_match(mask, e->d_name)) continue;
        f->names = realloc(f->names, sizeof(char *) * (f->n + 1)); f->names[f->n++] = strdup(e->d_name);
    }
    closedir(d);
    if (!f->n) { free(f->names); free(f); rt_set_last_error(2); return (u64)-1; }   // ERROR_FILE_NOT_FOUND
    fill_find(data, f->dir, f->names[0], wide); f->i = 1;
    return rt_handle_new(H_FIND, (intptr_t)f);
}
static int find_next(u64 h, u64 data, int wide) {
    intptr_t p; if (!rt_handle_get(h, H_FIND, &p)) { rt_set_last_error(6); return 0; }
    Find *f = (Find *)p;
    if (f->i >= f->n) { rt_set_last_error(18); return 0; }   // ERROR_NO_MORE_FILES
    fill_find(data, f->dir, f->names[f->i++], wide);
    return 1;
}
SHIM(FindFirstFileW) { char p[2048]; wide_to_utf8(A1, p, sizeof p); RET(find_first(p, A2, 1)); }
SHIM(FindFirstFileA) { RET(find_first((const char *)A1, A2, 0)); }
SHIM(FindFirstFileExW) { char p[2048]; wide_to_utf8(A1, p, sizeof p); RET(find_first(p, A3, 1)); }   // (name, level, data, op, filter, flags)
SHIM(FindFirstFileExA) { RET(find_first((const char *)A1, A3, 0)); }
SHIM(FindNextFileW) { RET(find_next(A1, A2, 1)); }
SHIM(FindNextFileA) { RET(find_next(A1, A2, 0)); }
SHIM(FindClose) {
    intptr_t p; if (!rt_handle_get(A1, H_FIND, &p)) RET(0);
    Find *f = (Find *)p; for (int i = 0; i < f->n; i++) free(f->names[i]); free(f->names); free(f);
    rt_handle_close(A1); RET(1);
}

// ---- file information / times ---------------------------------------------------------------
SHIM(GetFileTime) {  // (h, *creation, *access, *write)
    int fd = fd_of(A1); struct stat st; if (fd < 0 || fstat(fd, &st)) RET(0);
    if (A2) ST64(A2, ft_from(st.st_birthtimespec.tv_sec)); if (A3) ST64(A3, ft_from(st.st_atime)); if (A4) ST64(A4, ft_from(st.st_mtime));
    RET(1);
}
SHIM(GetFileInformationByHandle) {  // BY_HANDLE_FILE_INFORMATION (52 bytes)
    int fd = fd_of(A1); struct stat st; if (fd < 0 || fstat(fd, &st)) RET(0);
    u64 d = A2; memset((void *)d, 0, 52);
    ST32(d, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    ST64(d + 4, ft_from(st.st_birthtimespec.tv_sec)); ST64(d + 12, ft_from(st.st_atime)); ST64(d + 20, ft_from(st.st_mtime));
    ST32(d + 28, (u32)st.st_dev); ST32(d + 32, (u32)((u64)st.st_size >> 32)); ST32(d + 36, (u32)st.st_size);
    ST32(d + 40, (u32)st.st_nlink); ST32(d + 44, (u32)((u64)st.st_ino >> 32)); ST32(d + 48, (u32)st.st_ino);
    RET(1);
}
// SYSTEMTIME { year, month, dayofweek, day, hour, minute, second, ms } (u16 each)
SHIM(FileTimeToSystemTime) {
    u64 ft = LD64(A1); time_t t = (time_t)(ft / 10000000ull - 11644473600ull); struct tm g; gmtime_r(&t, &g);
    u16 v[8] = { (u16)(g.tm_year + 1900), (u16)(g.tm_mon + 1), (u16)g.tm_wday, (u16)g.tm_mday, (u16)g.tm_hour, (u16)g.tm_min, (u16)g.tm_sec, (u16)(ft / 10000 % 1000) };
    memcpy((void *)A2, v, 16); RET(1);
}
SHIM(SystemTimeToFileTime) {
    u16 v[8]; memcpy(v, (void *)A1, 16);
    struct tm g = { 0 }; g.tm_year = v[0] - 1900; g.tm_mon = v[1] - 1; g.tm_mday = v[3]; g.tm_hour = v[4]; g.tm_min = v[5]; g.tm_sec = v[6];
    time_t t = timegm(&g); ST64(A2, ft_from(t) + (u64)v[7] * 10000ull); RET(1);
}
SHIM(FileTimeToLocalFileTime) {
    time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
    ST64(A2, LD64(A1) + (u64)((i64)lt.tm_gmtoff * 10000000ll)); RET(1);
}
SHIM(SystemTimeToTzSpecificLocalTime) {  // (tz, utc SYSTEMTIME*, local SYSTEMTIME*)
    u16 v[8]; memcpy(v, (void *)A2, 16);
    struct tm g = { 0 }; g.tm_year = v[0] - 1900; g.tm_mon = v[1] - 1; g.tm_mday = v[3]; g.tm_hour = v[4]; g.tm_min = v[5]; g.tm_sec = v[6];
    time_t t = timegm(&g); struct tm l; localtime_r(&t, &l);
    u16 o[8] = { (u16)(l.tm_year + 1900), (u16)(l.tm_mon + 1), (u16)l.tm_wday, (u16)l.tm_mday, (u16)l.tm_hour, (u16)l.tm_min, (u16)l.tm_sec, v[7] };
    memcpy((void *)A3, o, 16); RET(1);
}
SHIM(GetSystemTime) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm g; gmtime_r(&ts.tv_sec, &g);
    u16 v[8] = { (u16)(g.tm_year + 1900), (u16)(g.tm_mon + 1), (u16)g.tm_wday, (u16)g.tm_mday, (u16)g.tm_hour, (u16)g.tm_min, (u16)g.tm_sec, (u16)(ts.tv_nsec / 1000000) };
    memcpy((void *)A1, v, 16); RET(0);
}
SHIM(GetLocalTime) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm g; localtime_r(&ts.tv_sec, &g);
    u16 v[8] = { (u16)(g.tm_year + 1900), (u16)(g.tm_mon + 1), (u16)g.tm_wday, (u16)g.tm_mday, (u16)g.tm_hour, (u16)g.tm_min, (u16)g.tm_sec, (u16)(ts.tv_nsec / 1000000) };
    memcpy((void *)A1, v, 16); RET(0);
}
static int rmdir_win(const char *w) { char p[2048]; rt_host_path(w, p, sizeof p); if (rmdir(p)) { rt_set_last_error(errno == ENOTEMPTY ? 145 : errno_to_win(errno)); return 0; } return 1; }
SHIM(RemoveDirectoryW) { char p[2048]; wide_to_utf8(A1, p, sizeof p); RET(rmdir_win(p)); }
SHIM(RemoveDirectoryA) { RET(rmdir_win((const char *)A1)); }
static int copy_file(const char *a, const char *b, int fail_if_exists) {
    char pa[2048], pb[2048]; rt_host_path(a, pa, sizeof pa); rt_host_path(b, pb, sizeof pb);
    int in = open(pa, O_RDONLY); if (in < 0) { rt_set_last_error(errno_to_win(errno)); return 0; }
    int out = open(pb, O_WRONLY | O_CREAT | O_TRUNC | (fail_if_exists ? O_EXCL : 0), 0644);
    if (out < 0) { close(in); rt_set_last_error(errno == EEXIST ? 80 : errno_to_win(errno)); return 0; }
    char buf[65536]; ssize_t n; int ok = 1;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) { ok = 0; break; }
    close(in); close(out); return ok && n >= 0;
}
SHIM(CopyFileW) { char a[2048], b[2048]; wide_to_utf8(A1, a, sizeof a); wide_to_utf8(A2, b, sizeof b); RET(copy_file(a, b, (int)(u32)A3)); }
SHIM(CopyFileA) { RET(copy_file((const char *)A1, (const char *)A2, (int)(u32)A3)); }
SHIM(CopyFileExW) { char a[2048], b[2048]; wide_to_utf8(A1, a, sizeof a); wide_to_utf8(A2, b, sizeof b); RET(copy_file(a, b, (int)((u32)AN(6) & 1))); }
SHIM(GetLogicalDrives) { RET(1u << 2); }   // just C:
SHIM(GetCurrentDirectoryA) { const char *p = rt_win_cwd(); size_t n = strlen(p); if ((u32)A1 <= n) RET(n + 1); memcpy((void *)A2, p, n + 1); RET(n); }

const Shim shims_files[] = {
    { NULL, "FindFirstFileW", sh_FindFirstFileW }, { NULL, "FindFirstFileA", sh_FindFirstFileA },
    { NULL, "FindFirstFileExW", sh_FindFirstFileExW }, { NULL, "FindFirstFileExA", sh_FindFirstFileExA },
    { NULL, "FindNextFileW", sh_FindNextFileW }, { NULL, "FindNextFileA", sh_FindNextFileA }, { NULL, "FindClose", sh_FindClose },
    { NULL, "GetFileTime", sh_GetFileTime }, { NULL, "GetFileInformationByHandle", sh_GetFileInformationByHandle },
    { NULL, "FileTimeToSystemTime", sh_FileTimeToSystemTime }, { NULL, "SystemTimeToFileTime", sh_SystemTimeToFileTime },
    { NULL, "FileTimeToLocalFileTime", sh_FileTimeToLocalFileTime }, { NULL, "SystemTimeToTzSpecificLocalTime", sh_SystemTimeToTzSpecificLocalTime },
    { NULL, "GetSystemTime", sh_GetSystemTime }, { NULL, "GetLocalTime", sh_GetLocalTime },
    { NULL, "RemoveDirectoryW", sh_RemoveDirectoryW }, { NULL, "RemoveDirectoryA", sh_RemoveDirectoryA },
    { NULL, "CopyFileW", sh_CopyFileW }, { NULL, "CopyFileA", sh_CopyFileA }, { NULL, "CopyFileExW", sh_CopyFileExW },
    { NULL, "GetLogicalDrives", sh_GetLogicalDrives }, { NULL, "GetCurrentDirectoryA", sh_GetCurrentDirectoryA },
#define E(n) { NULL, #n, sh_##n }
    E(CreateFileA), E(CreateFileW), E(SetFilePointerEx), E(SetFilePointer), E(GetFileSizeEx), E(GetFileSize),
    E(FlushFileBuffers), E(SetEndOfFile), E(GetFileAttributesA), E(GetFileAttributesW), E(GetFileAttributesExW),
    E(CreateDirectoryA), E(CreateDirectoryW), E(DeleteFileW), E(DeleteFileA), E(MoveFileExW), E(GetCurrentDirectoryW),
    E(GetTempPathW), { NULL, "fopen", sh_fopen_win }, E(_wfopen), E(_fdopen), E(_filelengthi64), E(_fstat64), E(_lseeki64),
    E(_open), E(_close),
#undef E
    { 0, 0, 0 }
};
