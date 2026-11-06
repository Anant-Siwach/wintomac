// kernel32 (part 2): paths and the current directory, file mappings, extended file information, locks,
// date/number formatting and locales, processor topology, psapi module queries, one-time init, I/O completion
// ports, thread-pool timers/waits and the remaining process/thread odds and ends games import.
// Semantics follow the Microsoft Learn documentation of each API; Wine's kernelbase was consulted as a
// behavioural reference for corner cases (GetFullPathName normalisation, date picture strings). No code copied.
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "winstr.h"

enum { ERR_FILE_NOT_FOUND = 2, ERR_PATH_NOT_FOUND = 3, ERR_ACCESS = 5, ERR_HANDLE = 6, ERR_NOT_SUPPORTED = 50,
       ERR_FILE_EXISTS = 80, ERR_PARAM = 87, ERR_INSUFF = 122, ERR_ALREADY_EXISTS = 183, ERR_MORE_DATA = 234,
       ERR_NO_MORE_ITEMS = 259, ERR_LOCK_VIOLATION = 33, ERR_NO_MORE_FILES = 18 };
#define H_FILE_ 1          /* == H_FILE */
#define H_MAPPING 24
#define H_IOCP 25

// Call another shim by name with up to four register arguments (stack arguments stay the caller's).
static ShimFn shim_fn(const char *name) {
    extern const Shim shims_kernel32[], shims_threads[], shims_files[], shims_extra[], shims_extra2[], shims_misc[];
    const Shim *t[] = { shims_kernel32, shims_threads, shims_files, shims_extra, shims_extra2, shims_misc };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) for (const Shim *s = t[i]; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}
static u64 call4(Ctx *c, const char *name, u64 a1, u64 a2, u64 a3, u64 a4) {
    ShimFn f = shim_fn(name); if (!f) return 0;
    u64 s1 = c->r[RCX], s2 = c->r[RDX], s3 = c->r[R8], s4 = c->r[R9];
    c->r[RCX] = a1; c->r[RDX] = a2; c->r[R8] = a3; c->r[R9] = a4;
    f(c);
    u64 r = c->r[RAX];
    c->r[RCX] = s1; c->r[RDX] = s2; c->r[R8] = s3; c->r[R9] = s4;
    return r;
}
static int fd_of(u64 h) { intptr_t fd; return rt_handle_get(h, H_FILE_, &fd) ? (int)fd : -1; }
static u64 ft_from_ts(struct timespec t) { return ((u64)t.tv_sec + 11644473600ull) * 10000000ull + (u64)t.tv_nsec / 100; }
static struct timespec ts_from_ft(u64 ft) { struct timespec t; t.tv_sec = (time_t)(ft / 10000000ull) - 11644473600ll; t.tv_nsec = (long)(ft % 10000000ull) * 100; return t; }
static u32 errno_win(int e) {
    switch (e) { case ENOENT: return ERR_FILE_NOT_FOUND; case ENOTDIR: return ERR_PATH_NOT_FOUND; case EACCES: case EPERM: return ERR_ACCESS;
                 case EEXIST: return ERR_ALREADY_EXISTS; case ENOSPC: return 112; case ENOTEMPTY: return 145; default: return 31; }
}
static void w2a_in(u64 p, int wide, char *out, size_t n) {   // API string argument -> UTF-8 (A strings taken as Latin-1/ASCII)
    if (!p) { out[0] = 0; return; }
    if (wide) { wx_w2u(p, out, n); return; }
    size_t o = 0; for (const u8 *s = (const u8 *)p; *s && o + 3 < n; s++) { if (*s < 0x80) out[o++] = (char)*s; else { out[o++] = (char)(0xC0 | *s >> 6); out[o++] = (char)(0x80 | (*s & 63)); } }
    out[o] = 0;
}
static u32 ret_str(const char *s, u64 buf, u32 cap, int wide) { return wide ? wx_ret_str_w(s, buf, cap) : wx_ret_str_a(s, buf, cap); }

// ==== paths ========================================================================================================
static char win_cwd[2048];
static pthread_mutex_t cwd_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *root_dir(void) { const char *r = getenv("WINTOMAC_ROOT"); return r && *r ? r : rt_exe_dir(); }
static const char *profile_base(void) {
    static char b[1100];
    if (!*b) snprintf(b, sizeof b, "%s", rt_user_data_dir());   // per game when packaged (appdata.c)
    return b;
}
// Host path -> Windows path: the program root is C:\, the per-user store holds C:\users\..., the rest is Z:\ (Wine's convention).
void rt_win_path_from_host(const char *host, char *out, size_t n) {
    char abs[2048];
    if (host[0] != '/') { char cwd[1024]; if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/"); snprintf(abs, sizeof abs, "%s/%s", cwd, host); }
    else snprintf(abs, sizeof abs, "%s", host);
    char real[PATH_MAX]; if (realpath(abs, real)) snprintf(abs, sizeof abs, "%s", real);
    char rr[PATH_MAX], pr[PATH_MAX];
    const char *root = realpath(root_dir(), rr) ? rr : root_dir();
    const char *prof = realpath(profile_base(), pr) ? pr : profile_base();
    const char *rest; const char *drive;
    size_t lr = strlen(root), lp = strlen(prof);
    if (!strncmp(abs, prof, lp) && (abs[lp] == '/' || !abs[lp])) { rest = abs + lp; drive = "C:"; }
    else if (!strncmp(abs, root, lr) && (abs[lr] == '/' || !abs[lr])) { rest = abs + lr; drive = "C:"; }
    else { rest = abs; drive = "Z:"; }
    snprintf(out, n, "%s%s", drive, *rest ? rest : "\\");
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
}
static void cwd_init(void) {
    if (*win_cwd) return;
    char cwd[1024];
    if (getcwd(cwd, sizeof cwd)) rt_win_path_from_host(cwd, win_cwd, sizeof win_cwd); else strcpy(win_cwd, "C:\\");
    if (win_cwd[0] == 'Z') strcpy(win_cwd, "C:\\");   // started outside the game tree: present the program root
}
const char *rt_win_cwd(void) { pthread_mutex_lock(&cwd_lock); cwd_init(); pthread_mutex_unlock(&cwd_lock); return win_cwd; }

// GetFullPathName: resolve against the current directory, '/' -> '\', collapse separators, "." and "..",
// strip trailing dots/spaces of the last component; a trailing separator is kept.
int rt_full_path(const char *in, char *out, size_t n) {
    char buf[4096], tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", in);
    for (char *p = tmp; *p; p++) if (*p == '/') *p = '\\';
    if (!strncmp(tmp, "\\\\?\\", 4)) { snprintf(out, n, "%s", tmp); return (int)strlen(out); }
    const char *cwd = rt_win_cwd();
    char prefix[16]; const char *rest;
    if (tmp[0] == '\\' && tmp[1] == '\\') {   // UNC: \\server\share\...
        const char *s = tmp + 2; const char *e = strchr(s, '\\'); if (e) e = strchr(e + 1, '\\');
        size_t pl = e ? (size_t)(e - tmp) : strlen(tmp);
        if (pl >= sizeof buf) pl = sizeof buf - 1;
        memcpy(buf, tmp, pl); buf[pl] = 0; rest = tmp + pl;
        snprintf(prefix, sizeof prefix, "%s", "");
        char norm[4096]; snprintf(norm, sizeof norm, "%s", buf);
        // fall through with the UNC root as prefix
        size_t o = strlen(norm);
        char *save = NULL, rcopy[4096]; snprintf(rcopy, sizeof rcopy, "%s", rest);
        int trailing = *rest && rest[strlen(rest) - 1] == '\\';
        for (char *comp = strtok_r(rcopy, "\\", &save); comp; comp = strtok_r(NULL, "\\", &save)) {
            if (!strcmp(comp, ".")) continue;
            if (!strcmp(comp, "..")) { char *s2 = strrchr(norm, '\\'); if (s2 && (size_t)(s2 - norm) > pl - 1) { *s2 = 0; o = strlen(norm); } continue; }
            o += (size_t)snprintf(norm + o, sizeof norm - o, "\\%s", comp);
        }
        if (trailing) snprintf(norm + strlen(norm), sizeof norm - strlen(norm), "\\");
        snprintf(out, n, "%s", norm); return (int)strlen(out);
    }
    if (((tmp[0] | 32) >= 'a' && (tmp[0] | 32) <= 'z') && tmp[1] == ':') {
        snprintf(prefix, sizeof prefix, "%c:", tmp[0]);
        if (tmp[2] == '\\') rest = tmp + 2;
        else if ((tmp[0] | 32) == (cwd[0] | 32)) { snprintf(buf, sizeof buf, "%s\\%s", cwd + 2, tmp + 2); rest = buf; }
        else { snprintf(buf, sizeof buf, "\\%s", tmp + 2); rest = buf; }
    } else {
        snprintf(prefix, sizeof prefix, "%c:", cwd[0]);
        if (tmp[0] == '\\') rest = tmp;
        else { snprintf(buf, sizeof buf, "%s\\%s", cwd + 2, tmp); rest = buf; }
    }
    char rcopy[4096]; snprintf(rcopy, sizeof rcopy, "%s", rest);
    int trailing = *tmp && (tmp[strlen(tmp) - 1] == '\\') && strcmp(tmp + (strlen(tmp) >= 2 ? strlen(tmp) - 2 : 0), ":\\");
    char *comps[512]; int nc = 0; char *save = NULL;
    for (char *comp = strtok_r(rcopy, "\\", &save); comp && nc < 512; comp = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { if (nc) nc--; continue; }
        comps[nc++] = comp;
    }
    if (nc && !trailing) {   // Windows drops trailing dots and spaces of the final component ("file." -> "file")
        char *l = comps[nc - 1]; size_t ll = strlen(l);
        while (ll && (l[ll - 1] == '.' || l[ll - 1] == ' ')) l[--ll] = 0;
        if (!ll) nc--;
    }
    size_t o = (size_t)snprintf(out, n, "%s", prefix);
    for (int i = 0; i < nc && o < n; i++) o += (size_t)snprintf(out + o, n - o, "\\%s", comps[i]);
    if ((!nc || trailing) && o < n) o += (size_t)snprintf(out + o, n - o, "\\");
    return (int)strlen(out);
}
static u32 full_path_api(Ctx *c, int wide) {   // (name, cch, buf, *filepart)
    char in[2048], full[4096]; w2a_in(A1, wide, in, sizeof in);
    if (!*in) { rt_set_last_error(ERR_PARAM); return 0; }
    rt_full_path(in, full, sizeof full);
    u32 r = ret_str(full, A3, (u32)A2, wide);
    if (A4) {
        u64 fp = 0;
        if (r < (u32)A2) {
            const char *last = strrchr(full, '\\');
            if (last && last[1]) {
                size_t idx = wide ? wx_u2w(full, 0, 0) - 1 - (wx_u2w(last + 1, 0, 0) - 1) : (size_t)(last + 1 - full);
                fp = A3 + (wide ? 2 * idx : idx);
            }
        }
        ST64(A4, fp);
    }
    return r;
}
SHIM(GetFullPathNameW) { RET(full_path_api(c, 1)); }
SHIM(GetFullPathNameA) { RET(full_path_api(c, 0)); }
static int set_cwd(u64 p, int wide) {
    char in[2048], full[4096], host[4096]; w2a_in(p, wide, in, sizeof in);
    rt_full_path(in, full, sizeof full);
    rt_host_path(full, host, sizeof host);
    struct stat st;
    if (stat(host, &st)) { rt_set_last_error(ERR_FILE_NOT_FOUND); return 0; }
    if (!S_ISDIR(st.st_mode)) { rt_set_last_error(267); return 0; }   // ERROR_DIRECTORY
    if (chdir(host)) { rt_set_last_error(errno_win(errno)); return 0; }
    size_t l = strlen(full); if (l > 3 && full[l - 1] == '\\') full[l - 1] = 0;
    pthread_mutex_lock(&cwd_lock); snprintf(win_cwd, sizeof win_cwd, "%s", full); pthread_mutex_unlock(&cwd_lock);
    return 1;
}
SHIM(SetCurrentDirectoryW) { RET(set_cwd(A1, 1)); }
SHIM(SetCurrentDirectoryA) { RET(set_cwd(A1, 0)); }
static u32 path_exists_copy(Ctx *c, int wide) {   // GetLongPathName / GetShortPathName: (in, out, cch)
    char in[2048], host[4096]; w2a_in(A1, wide, in, sizeof in);
    rt_host_path(in, host, sizeof host);
    struct stat st; if (stat(host, &st)) { rt_set_last_error(ERR_FILE_NOT_FOUND); return 0; }
    return ret_str(in, A2, (u32)A3, wide);
}
SHIM(GetLongPathNameW) { RET(path_exists_copy(c, 1)); }
SHIM(GetLongPathNameA) { RET(path_exists_copy(c, 0)); }
SHIM(GetShortPathNameW) { RET(path_exists_copy(c, 1)); }
SHIM(GetShortPathNameA) { RET(path_exists_copy(c, 0)); }
SHIM(GetTempPathA) { char host[2048]; rt_host_path("C:\\tmp", host, sizeof host); mkdir(host, 0755); RET(wx_ret_str_a("C:\\tmp\\", A2, (u32)A1)); }
SHIM(GetTempPath2W) { char host[2048]; rt_host_path("C:\\tmp", host, sizeof host); mkdir(host, 0755); RET(wx_ret_str_w("C:\\tmp\\", A2, (u32)A1)); }
SHIM(GetSystemDirectoryW) { RET(wx_ret_str_w("C:\\windows\\system32", A1, (u32)A2)); }
SHIM(GetSystemDirectoryA) { RET(wx_ret_str_a("C:\\windows\\system32", A1, (u32)A2)); }
SHIM(GetWindowsDirectoryW) { RET(wx_ret_str_w("C:\\windows", A1, (u32)A2)); }
SHIM(GetWindowsDirectoryA) { RET(wx_ret_str_a("C:\\windows", A1, (u32)A2)); }
SHIM(GetSystemWindowsDirectoryW) { RET(wx_ret_str_w("C:\\windows", A1, (u32)A2)); }
SHIM(GetSystemWow64DirectoryW) { WX_FAIL(GetSystemWow64DirectoryW, 120, 0); }   // ERROR_CALL_NOT_IMPLEMENTED: no WoW64 here
static u32 temp_file_name(Ctx *c, int wide) {   // (dir, prefix, unique, out[MAX_PATH])
    static u32 counter;
    char dir[2048], pre[16]; w2a_in(A1, wide, dir, sizeof dir); w2a_in(A2, wide, pre, sizeof pre); pre[3] = 0;
    u32 u = (u32)A3 & 0xFFFF;
    char name[2300];
    size_t dl = strlen(dir); const char *sep = dl && dir[dl - 1] != '\\' && dir[dl - 1] != '/' ? "\\" : "";
    if (u) snprintf(name, sizeof name, "%s%s%s%X.tmp", dir, sep, pre, u);
    else {
        for (int tries = 0; tries < 65535; tries++) {
            u = (u32)((getpid() + __atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST)) & 0xFFFF); if (!u) continue;
            snprintf(name, sizeof name, "%s%s%s%X.tmp", dir, sep, pre, u);
            char host[4096]; rt_host_path(name, host, sizeof host);
            int fd = open(host, O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd >= 0) { close(fd); break; }
            if (errno != EEXIST) { rt_set_last_error(errno_win(errno)); return 0; }
        }
    }
    if (wide) wx_u2w(name, A4, 260); else snprintf((char *)A4, 260, "%s", name);
    return u;
}
SHIM(GetTempFileNameW) { RET(temp_file_name(c, 1)); }
SHIM(GetTempFileNameA) { RET(temp_file_name(c, 0)); }
static u32 drive_type(u64 p, int wide) {
    if (!p) return 3;   // DRIVE_FIXED (current drive)
    char r[64]; w2a_in(p, wide, r, sizeof r);
    int l = (int)strlen(r);
    if (l >= 2 && r[1] == ':' && (l == 2 || (l == 3 && (r[2] == '\\' || r[2] == '/')))) {
        int d = r[0] | 32; return d == 'c' || d == 'z' ? 3 : 1;   // DRIVE_NO_ROOT_DIR
    }
    return 1;
}
SHIM(GetDriveTypeW) { RET(drive_type(A1, 1)); }
SHIM(GetDriveTypeA) { RET(drive_type(A1, 0)); }
static int host_statfs(u64 p, int wide, struct statfs *sf) {
    char in[2048] = "C:\\", host[4096]; if (p) w2a_in(p, wide, in, sizeof in);
    rt_host_path(in, host, sizeof host);
    if (statfs(host, sf)) { rt_set_last_error(errno_win(errno)); return 0; }
    return 1;
}
static int disk_free_ex(Ctx *c, int wide) {   // (dir, *availToCaller, *total, *free)
    struct statfs sf; if (!host_statfs(A1, wide, &sf)) return 0;
    if (A2) ST64(A2, (u64)sf.f_bavail * sf.f_bsize); if (A3) ST64(A3, (u64)sf.f_blocks * sf.f_bsize); if (A4) ST64(A4, (u64)sf.f_bfree * sf.f_bsize);
    return 1;
}
SHIM(GetDiskFreeSpaceExW) { RET(disk_free_ex(c, 1)); }
SHIM(GetDiskFreeSpaceExA) { RET(disk_free_ex(c, 0)); }
static int disk_free(Ctx *c, int wide) {   // (root, *secPerClus, *bytesPerSec, *freeClus, *totalClus): 4 KB clusters, capped like Windows
    struct statfs sf; if (!host_statfs(A1, wide, &sf)) return 0;
    u64 fr = (u64)sf.f_bavail * sf.f_bsize / 4096, tot = (u64)sf.f_blocks * sf.f_bsize / 4096;
    if (A2) ST32(A2, 8); if (A3) ST32(A3, 512);
    if (A4) ST32(A4, fr > 0xFFFFFFFF ? 0xFFFFFFFF : (u32)fr); if (AN(5)) ST32(AN(5), tot > 0xFFFFFFFF ? 0xFFFFFFFF : (u32)tot);
    return 1;
}
SHIM(GetDiskFreeSpaceW) { RET(disk_free(c, 1)); }
SHIM(GetDiskFreeSpaceA) { RET(disk_free(c, 0)); }
static int volume_info(Ctx *c, int wide) {   // (root, name, nameSize, *serial, *maxComp, *flags, fsName, fsNameSize)
    if (A2 && A3) { if (wide) wx_u2w("Macintosh HD", A2, (u32)A3); else snprintf((char *)A2, (u32)A3, "Macintosh HD"); }
    if (A4) ST32(A4, 0x57544D43);
    if (AN(5)) ST32(AN(5), 255);
    if (AN(6)) ST32(AN(6), 0x00000002 | 0x00000004 | 0x00000008 | 0x00000010 | 0x00040000);   // case-preserved, unicode, ACLs, compression, named streams
    if (AN(7) && AN(8)) { if (wide) wx_u2w("NTFS", AN(7), (u32)AN(8)); else snprintf((char *)AN(7), (u32)AN(8), "NTFS"); }
    return 1;
}
SHIM(GetVolumeInformationW) { RET(volume_info(c, 1)); }
SHIM(GetVolumeInformationA) { RET(volume_info(c, 0)); }
SHIM(GetVolumePathNameW) { if ((u32)A3 < 4) { rt_set_last_error(206); RET(0); } wx_u2w("C:\\", A2, (u32)A3); RET(1); }   // (file, out, cch)
SHIM(GetVolumeNameForVolumeMountPointW) { WX_FAIL(GetVolumeNameForVolumeMountPointW, ERR_NOT_SUPPORTED, 0); }
SHIM(GetVolumePathNamesForVolumeNameW) { WX_FAIL(GetVolumePathNamesForVolumeNameW, ERR_NOT_SUPPORTED, 0); }

// ==== files =========================================================================================================
static int host_of(u64 p, int wide, char *host, size_t n) { char in[2048]; w2a_in(p, wide, in, sizeof in); rt_host_path(in, host, n); return 1; }
static int set_attrs(u64 p, int wide, u32 attrs) {
    char host[4096]; host_of(p, wide, host, sizeof host);
    struct stat st; if (stat(host, &st)) { rt_set_last_error(errno_win(errno)); return 0; }
    mode_t m = (attrs & 1) ? (st.st_mode & ~(mode_t)0222) : (st.st_mode | S_IWUSR);   // FILE_ATTRIBUTE_READONLY
    chmod(host, m & 07777); return 1;
}
SHIM(SetFileAttributesW) { RET(set_attrs(A1, 1, (u32)A2)); }
SHIM(SetFileAttributesA) { RET(set_attrs(A1, 0, (u32)A2)); }
static u32 attr_of(const struct stat *st, const char *name) {
    u32 a = S_ISDIR(st->st_mode) ? 0x10 : 0;
    if (!(st->st_mode & S_IWUSR)) a |= 1;
    const char *b = strrchr(name, '/'); b = b ? b + 1 : name;
    if (b[0] == '.' && strcmp(b, ".") && strcmp(b, "..")) a |= 2;
    return a ? a : 0x80;
}
SHIM(GetFileAttributesExA) {  // (path, level, WIN32_FILE_ATTRIBUTE_DATA*)
    char host[4096]; host_of(A1, 0, host, sizeof host);
    struct stat st; if (stat(host, &st)) { rt_set_last_error(errno_win(errno)); RET(0); }
    u64 d = A3; memset((void *)d, 0, 36);
    ST32(d, attr_of(&st, host));
    ST64(d + 4, ft_from_ts(st.st_birthtimespec)); ST64(d + 12, ft_from_ts(st.st_atimespec)); ST64(d + 20, ft_from_ts(st.st_mtimespec));
    if (!S_ISDIR(st.st_mode)) { ST32(d + 28, (u32)((u64)st.st_size >> 32)); ST32(d + 32, (u32)st.st_size); }
    RET(1);
}
static int move_file(u64 a, u64 b, int wide, u32 flags) {   // MOVEFILE_REPLACE_EXISTING 1
    char pa[4096], pb[4096]; host_of(a, wide, pa, sizeof pa);
    if (!b) { if (unlink(pa) && rmdir(pa)) { rt_set_last_error(errno_win(errno)); return 0; } return 1; }   // delete (on reboot: now)
    host_of(b, wide, pb, sizeof pb);
    struct stat st;
    if (!(flags & 1) && !stat(pb, &st) && strcasecmp(pa, pb)) { rt_set_last_error(ERR_ALREADY_EXISTS); return 0; }
    if (rename(pa, pb)) { rt_set_last_error(errno_win(errno)); return 0; }
    return 1;
}
SHIM(MoveFileExA) { RET(move_file(A1, A2, 0, (u32)A3)); }
SHIM(MoveFileW) { RET(move_file(A1, A2, 1, 0)); }
SHIM(MoveFileA) { RET(move_file(A1, A2, 0, 0)); }
SHIM(MoveFileWithProgressW) { RET(move_file(A1, A2, 1, (u32)AN(5))); }
static int replace_file(Ctx *c, int wide) {   // (replaced, replacement, backup, flags, excl, reserved)
    char pr[4096], pn[4096], pbk[4096]; host_of(A1, wide, pr, sizeof pr); host_of(A2, wide, pn, sizeof pn);
    struct stat st; if (stat(pr, &st)) { rt_set_last_error(1176); return 0; }   // ERROR_UNABLE_TO_MOVE_REPLACEMENT... target missing
    if (A3) { host_of(A3, wide, pbk, sizeof pbk); if (rename(pr, pbk)) { rt_set_last_error(1177); return 0; } }
    if (rename(pn, pr)) { rt_set_last_error(1176); return 0; }
    return 1;
}
SHIM(ReplaceFileW) { RET(replace_file(c, 1)); }
SHIM(ReplaceFileA) { RET(replace_file(c, 0)); }
SHIM(CopyFileExA) { ShimFn f = shim_fn("CopyFileA"); u64 s = c->r[R8]; c->r[R8] = (u64)((u32)AN(6) & 1); f(c); c->r[R8] = s; }
static int mkdir_ex(u64 p, int wide) {
    char host[4096]; host_of(p, wide, host, sizeof host);
    if (mkdir(host, 0755)) { rt_set_last_error(errno_win(errno)); return 0; }
    return 1;
}
SHIM(CreateDirectoryExW) { RET(mkdir_ex(A2, 1)); }   // (template, new, sa)
SHIM(CreateDirectoryExA) { RET(mkdir_ex(A2, 0)); }
SHIM(CreateSymbolicLinkW) {  // (link, target, flags) -> BOOLEAN
    char pl[4096], tgt[2048], pt[4096]; host_of(A1, 1, pl, sizeof pl); wx_w2u(A2, tgt, sizeof tgt);
    if (((tgt[0] | 32) >= 'a' && (tgt[0] | 32) <= 'z' && tgt[1] == ':') || tgt[0] == '\\') rt_host_path(tgt, pt, sizeof pt);
    else { snprintf(pt, sizeof pt, "%s", tgt); for (char *q = pt; *q; q++) if (*q == '\\') *q = '/'; }
    if (symlink(pt, pl)) { rt_set_last_error(errno_win(errno)); RET(0); }
    RET(1);
}
SHIM(CreateHardLinkW) { char a[4096], b[4096]; host_of(A1, 1, a, sizeof a); host_of(A2, 1, b, sizeof b); if (link(b, a)) { rt_set_last_error(errno_win(errno)); RET(0); } RET(1); }
SHIM(AreFileApisANSI) { RET(1); }
SHIM(SetFileApisToANSI) { RET(0); }
SHIM(SetFileApisToOEM) { RET(0); }
SHIM(CompareFileTime) { u64 a = LD64(A1), b = LD64(A2); RET((u32)(a < b ? -1 : a > b ? 1 : 0)); }
SHIM(SetFileTime) {  // (h, *creation, *access, *write)
    int fd = fd_of(A1); struct stat st; if (fd < 0 || fstat(fd, &st)) { rt_set_last_error(ERR_HANDLE); RET(0); }
    struct timespec t[2] = { st.st_atimespec, st.st_mtimespec };
    if (A3 && LD64(A3) && LD64(A3) != ~0ull) t[0] = ts_from_ft(LD64(A3));
    if (A4 && LD64(A4) && LD64(A4) != ~0ull) t[1] = ts_from_ft(LD64(A4));
    RET(futimens(fd, t) == 0);
}
static int lock_range(int fd, u64 off, u64 len, int lock, int excl, int wait) {
    struct flock fl = { 0 };
    fl.l_start = (off_t)off; fl.l_len = (off_t)(len > 0x7FFFFFFFFFFFFFFFull ? 0 : len); fl.l_whence = SEEK_SET;
    fl.l_type = !lock ? F_UNLCK : excl ? F_WRLCK : F_RDLCK;
    if (fcntl(fd, wait ? F_SETLKW : F_SETLK, &fl)) {
        if (lock && errno == EBADF && !excl) return 1;   // shared lock on a write-only fd: nothing to protect against in-process
        rt_set_last_error(lock ? ERR_LOCK_VIOLATION : 158); return 0;   // ERROR_NOT_LOCKED
    }
    return 1;
}
SHIM(LockFile) { int fd = fd_of(A1); if (fd < 0) { rt_set_last_error(ERR_HANDLE); RET(0); }   // (h, offLo, offHi, lenLo, lenHi)
                 RET(lock_range(fd, (u64)(u32)A3 << 32 | (u32)A2, (u64)(u32)AN(5) << 32 | (u32)A4, 1, 1, 0)); }
SHIM(UnlockFile) { int fd = fd_of(A1); if (fd < 0) { rt_set_last_error(ERR_HANDLE); RET(0); }
                   RET(lock_range(fd, (u64)(u32)A3 << 32 | (u32)A2, (u64)(u32)AN(5) << 32 | (u32)A4, 0, 0, 0)); }
SHIM(LockFileEx) {  // (h, flags, reserved, lenLo, lenHi, ov): LOCKFILE_FAIL_IMMEDIATELY 1, LOCKFILE_EXCLUSIVE_LOCK 2
    int fd = fd_of(A1); if (fd < 0) { rt_set_last_error(ERR_HANDLE); RET(0); }
    u64 ov = AN(6), off = ov ? ((u64)LD32(ov + 20) << 32 | LD32(ov + 16)) : 0;
    RET(lock_range(fd, off, (u64)(u32)AN(5) << 32 | (u32)A4, 1, ((u32)A2 & 2) != 0, !((u32)A2 & 1)));
}
SHIM(UnlockFileEx) {  // (h, reserved, lenLo, lenHi, ov)
    int fd = fd_of(A1); if (fd < 0) { rt_set_last_error(ERR_HANDLE); RET(0); }
    u64 ov = AN(5), off = ov ? ((u64)LD32(ov + 20) << 32 | LD32(ov + 16)) : 0;
    RET(lock_range(fd, off, (u64)(u32)A4 << 32 | (u32)A3, 0, 0, 0));
}
SHIM(PeekNamedPipe) {  // (h, buf, size, *read, *avail, *leftThisMsg): bytes available only; nothing is consumed
    int fd = fd_of(A1); if (fd < 0) { rt_set_last_error(ERR_HANDLE); RET(0); }
    int n = 0; if (ioctl(fd, FIONREAD, &n)) n = 0;
    if (A4) ST32(A4, 0); if (AN(5)) ST32(AN(5), (u32)n); if (AN(6)) ST32(AN(6), 0);
    RET(1);
}
SHIM(CreateNamedPipeW) { WX_FAIL(CreateNamedPipeW, 120, (u64)-1); }   // ERROR_CALL_NOT_IMPLEMENTED
SHIM(CreateNamedPipeA) { WX_FAIL(CreateNamedPipeA, 120, (u64)-1); }
SHIM(ConnectNamedPipe) { WX_FAIL(ConnectNamedPipe, 120, 0); }
SHIM(DisconnectNamedPipe) { WX_FAIL(DisconnectNamedPipe, 120, 0); }
SHIM(WaitNamedPipeW) { WX_FAIL(WaitNamedPipeW, ERR_FILE_NOT_FOUND, 0); }
SHIM(WaitNamedPipeA) { WX_FAIL(WaitNamedPipeA, ERR_FILE_NOT_FOUND, 0); }

static int fd_path(int fd, char *out) { return fcntl(fd, F_GETPATH, out) == 0; }
SHIM(GetFileInformationByHandleEx) {  // (h, class, buf, size)
    int fd = fd_of(A1); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { rt_set_last_error(ERR_HANDLE); RET(0); }
    u32 cls = (u32)A2, sz = (u32)A4; u64 b = A3;
    char hp[PATH_MAX] = "";
    switch (cls) {
    case 0:   // FileBasicInfo
        if (sz < 40) { rt_set_last_error(24); RET(0); }   // ERROR_BAD_LENGTH
        ST64(b, ft_from_ts(st.st_birthtimespec)); ST64(b + 8, ft_from_ts(st.st_atimespec)); ST64(b + 16, ft_from_ts(st.st_mtimespec));
        ST64(b + 24, ft_from_ts(st.st_ctimespec)); fd_path(fd, hp); ST32(b + 32, attr_of(&st, hp)); ST32(b + 36, 0); RET(1);
    case 1:   // FileStandardInfo
        if (sz < 24) { rt_set_last_error(24); RET(0); }
        ST64(b, (u64)st.st_blocks * 512); ST64(b + 8, S_ISDIR(st.st_mode) ? 0 : (u64)st.st_size); ST32(b + 16, (u32)st.st_nlink);
        ST8(b + 20, 0); ST8(b + 21, S_ISDIR(st.st_mode) ? 1 : 0); ST16(b + 22, 0); RET(1);
    case 2: {   // FileNameInfo: path without the drive ("\dir\file")
        if (sz < 4) { rt_set_last_error(24); RET(0); }
        char win[4096]; if (!fd_path(fd, hp)) strcpy(hp, "/"); rt_win_path_from_host(hp, win, sizeof win);
        u32 units = (u32)wx_u2w(win + 2, 0, 0) - 1; ST32(b, 2 * units);
        u32 room = (sz - 4) / 2;
        u16 *tmp = malloc(2 * (units + 1)); wx_u2w(win + 2, (u64)tmp, units + 1);
        memcpy((void *)(b + 4), tmp, 2 * (units < room ? units : room)); free(tmp);
        if (units > room) { rt_set_last_error(ERR_MORE_DATA); RET(0); }
        RET(1);
    }
    case 9:   // FileAttributeTagInfo
        if (sz < 8) { rt_set_last_error(24); RET(0); }
        fd_path(fd, hp); ST32(b, attr_of(&st, hp)); ST32(b + 4, 0); RET(1);
    case 18:  // FileIdInfo
        if (sz < 24) { rt_set_last_error(24); RET(0); }
        ST64(b, (u64)st.st_dev); ST64(b + 8, (u64)st.st_ino); ST64(b + 16, 0); RET(1);
    default: WX_FAIL(GetFileInformationByHandleEx(class), ERR_PARAM, 0);
    }
}
SHIM(SetFileInformationByHandle) {  // (h, class, buf, size)
    int fd = fd_of(A1); if (fd < 0) { rt_set_last_error(ERR_HANDLE); RET(0); }
    u32 cls = (u32)A2; u64 b = A3; char hp[PATH_MAX];
    switch (cls) {
    case 0: {   // FileBasicInfo: times (0 = keep, -1 = keep) and the read-only attribute
        struct stat st; fstat(fd, &st);
        struct timespec t[2] = { st.st_atimespec, st.st_mtimespec };
        if (LD64(b + 8) && LD64(b + 8) != ~0ull) t[0] = ts_from_ft(LD64(b + 8));
        if (LD64(b + 16) && LD64(b + 16) != ~0ull) t[1] = ts_from_ft(LD64(b + 16));
        futimens(fd, t);
        u32 a = LD32(b + 32); if (a) fchmod(fd, (a & 1) ? (st.st_mode & ~(mode_t)0222 & 07777) : ((st.st_mode | S_IWUSR) & 07777));
        RET(1);
    }
    case 3: {   // FileRenameInfo { BOOLEAN replace (padded to 8); HANDLE root; DWORD len; WCHAR name[] }
        if (!fd_path(fd, hp)) { rt_set_last_error(ERR_HANDLE); RET(0); }
        u32 len = LD32(b + 16) / 2; u16 *w = calloc(len + 1, 2); memcpy(w, (void *)(b + 20), 2 * len);
        char nm[2048], dst[4096]; wx_w2u((u64)w, nm, sizeof nm); free(w); rt_host_path(nm, dst, sizeof dst);
        struct stat st; if (!LD8(b) && !stat(dst, &st)) { rt_set_last_error(ERR_ALREADY_EXISTS); RET(0); }
        if (rename(hp, dst)) { rt_set_last_error(errno_win(errno)); RET(0); }
        RET(1);
    }
    case 4: case 21:   // FileDispositionInfo { BOOLEAN delete } / FileDispositionInfoEx { DWORD flags (1 = DELETE) }
        if ((cls == 4 ? LD8(b) : (LD32(b) & 1)) && fd_path(fd, hp)) { if (unlink(hp) && rmdir(hp)) { rt_set_last_error(errno_win(errno)); RET(0); } }
        RET(1);
    case 5: RET(1);   // FileAllocationInfo: allocation is the filesystem's business
    case 6: RET(ftruncate(fd, (off_t)LD64(b)) == 0);   // FileEndOfFileInfo
    default: WX_FAIL(SetFileInformationByHandle(class), ERR_PARAM, 0);
    }
}
static u32 final_path(Ctx *c, int wide) {   // (h, buf, cch, flags): VOLUME_NAME_DOS -> "\\?\C:\...", VOLUME_NAME_NONE (4) -> "\..."
    int fd = fd_of(A1); char hp[PATH_MAX], win[4096], out[4200];
    if (fd < 0 || !fd_path(fd, hp)) { rt_set_last_error(ERR_HANDLE); return 0; }
    rt_win_path_from_host(hp, win, sizeof win);
    if ((u32)A4 & 4) snprintf(out, sizeof out, "%s", win + 2); else snprintf(out, sizeof out, "\\\\?\\%s", win);
    return ret_str(out, A2, (u32)A3, wide);
}
SHIM(GetFinalPathNameByHandleW) { RET(final_path(c, 1)); }
SHIM(GetFinalPathNameByHandleA) { RET(final_path(c, 0)); }

// ---- file mappings ---------------------------------------------------------------------------------------------------
typedef struct { int fd; u64 size; u32 prot; u8 *anon; char name[128]; } Mapping;
static Mapping *named_maps[64];
static struct { u64 addr, len; int anon; } views[1024];
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 create_mapping(Ctx *c, int wide) {   // (file, sa, protect, maxHi, maxLo, name)
    u64 hf = A1; u32 prot = (u32)A3; u64 size = (u64)(u32)A4 << 32 | (u32)AN(5);
    char name[128] = ""; w2a_in(AN(6), wide, name, sizeof name);
    pthread_mutex_lock(&map_lock);
    if (*name) for (int i = 0; i < 64; i++) if (named_maps[i] && !strcmp(named_maps[i]->name, name)) {   // existing named section
        pthread_mutex_unlock(&map_lock); rt_set_last_error(ERR_ALREADY_EXISTS); return rt_handle_new(H_MAPPING, (intptr_t)named_maps[i]);
    }
    Mapping *m = calloc(1, sizeof *m); m->prot = prot; m->fd = -1; snprintf(m->name, sizeof m->name, "%s", name);
    if (hf == ~0ull || !hf) {   // pagefile-backed: one shared anonymous block every view aliases
        if (!size) { pthread_mutex_unlock(&map_lock); free(m); rt_set_last_error(ERR_PARAM); return 0; }
        m->anon = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
        if (m->anon == MAP_FAILED) { pthread_mutex_unlock(&map_lock); free(m); rt_set_last_error(8); return 0; }
    } else {
        int fd = fd_of(hf); struct stat st;
        if (fd < 0 || fstat(fd, &st)) { pthread_mutex_unlock(&map_lock); free(m); rt_set_last_error(ERR_HANDLE); return 0; }
        if (!size) size = (u64)st.st_size;
        if (!size) { pthread_mutex_unlock(&map_lock); free(m); rt_set_last_error(1006); return 0; }   // ERROR_FILE_INVALID (empty file)
        if (size > (u64)st.st_size && (prot & 0x44)) ftruncate(fd, (off_t)size);   // PAGE_READWRITE / EXECUTE_READWRITE grow the file
        m->fd = dup(fd);
    }
    m->size = size;
    if (*name) for (int i = 0; i < 64; i++) if (!named_maps[i]) { named_maps[i] = m; break; }
    pthread_mutex_unlock(&map_lock);
    rt_set_last_error(0);
    return rt_handle_new(H_MAPPING, (intptr_t)m);
}
// For ps5_vmem.c (MapViewOfFile3): the file descriptor (or the shared anonymous block) and size of a section handle.
int rt_mapping_info(u64 h, int *fd, u8 **anon, u64 *size) {
    intptr_t p; if (!rt_handle_get(h, H_MAPPING, &p)) return 0;
    Mapping *m = (Mapping *)p; *fd = m->fd; *anon = m->anon; *size = m->size; return 1;
}
SHIM(CreateFileMappingW) { RET(create_mapping(c, 1)); }
SHIM(CreateFileMappingA) { RET(create_mapping(c, 0)); }
static u64 open_mapping(u64 name, int wide) {
    char nm[128]; w2a_in(name, wide, nm, sizeof nm);
    pthread_mutex_lock(&map_lock);
    for (int i = 0; i < 64; i++) if (named_maps[i] && !strcmp(named_maps[i]->name, nm)) { pthread_mutex_unlock(&map_lock); return rt_handle_new(H_MAPPING, (intptr_t)named_maps[i]); }
    pthread_mutex_unlock(&map_lock);
    rt_set_last_error(ERR_FILE_NOT_FOUND); return 0;
}
SHIM(OpenFileMappingW) { RET(open_mapping(A3, 1)); }   // (access, inherit, name)
SHIM(OpenFileMappingA) { RET(open_mapping(A3, 0)); }
static u64 map_view(u64 h, u32 access, u64 off, u64 len, u64 want) {
    intptr_t p; if (!rt_handle_get(h, H_MAPPING, &p)) { rt_set_last_error(ERR_HANDLE); return 0; }
    Mapping *m = (Mapping *)p;
    if (off > m->size) { rt_set_last_error(ERR_ACCESS); return 0; }
    if (!len) len = m->size - off;
    u64 r;
    if (m->anon) r = (u64)(m->anon + off);
    else {
        int prot = PROT_READ | ((access & 2) || (access & 1) ? PROT_WRITE : 0);   // FILE_MAP_WRITE 2 / FILE_MAP_COPY 1
        int flags = (access & 1) && !(access & 2) ? MAP_PRIVATE : MAP_SHARED;
        u64 aoff = off & ~(u64)(getpagesize() - 1), delta = off - aoff;
        // A requested address is only a hint (never MAP_FIXED: that would silently replace whatever host memory,
        // e.g. malloc's, is there); like Windows, the view fails if it can't go exactly there.
        void *v = mmap(want ? (void *)(want - delta) : NULL, len + delta, prot, flags, m->fd, (off_t)aoff);
        if (v == MAP_FAILED) { rt_set_last_error(8); return 0; }
        if (want && (u64)v != want - delta) { munmap(v, len + delta); rt_set_last_error(487); return 0; }   // ERROR_INVALID_ADDRESS
        r = (u64)v + delta;
    }
    pthread_mutex_lock(&map_lock);
    for (int i = 0; i < 1024; i++) if (!views[i].addr) { views[i].addr = r; views[i].len = len; views[i].anon = m->anon != NULL; break; }
    pthread_mutex_unlock(&map_lock);
    return r;
}
SHIM(MapViewOfFile) { RET(map_view(A1, (u32)A2, (u64)(u32)A3 << 32 | (u32)A4, AN(5), 0)); }   // (h, access, offHi, offLo, size)
SHIM(MapViewOfFileEx) { RET(map_view(A1, (u32)A2, (u64)(u32)A3 << 32 | (u32)A4, AN(5), AN(6))); }
SHIM(UnmapViewOfFile) {
    pthread_mutex_lock(&map_lock);
    for (int i = 0; i < 1024; i++) if (views[i].addr == A1) {
        if (!views[i].anon) { u64 a = A1 & ~(u64)(getpagesize() - 1); munmap((void *)a, views[i].len + (A1 - a)); }
        views[i].addr = 0; pthread_mutex_unlock(&map_lock); RET(1);
    }
    pthread_mutex_unlock(&map_lock);
    rt_set_last_error(487); RET(0);   // ERROR_INVALID_ADDRESS
}
SHIM(FlushViewOfFile) { u64 a = A1 & ~(u64)(getpagesize() - 1); msync((void *)a, (A2 ? A2 : 4096) + (A1 - a), MS_SYNC); RET(1); }

// ==== time and locale ==================================================================================================
SHIM(GetDynamicTimeZoneInformation) {  // TIME_ZONE_INFORMATION (172) + TimeZoneKeyName[128] + DynamicDaylightTimeDisabled
    memset((void *)A1, 0, 432);
    u64 r = call4(c, "GetTimeZoneInformation", A1, 0, 0, 0);
    u16 *std = (u16 *)(A1 + 4);
    if (std[0]) memcpy((void *)(A1 + 172), std, 64); else wx_u2w("UTC", A1 + 172, 128);
    RET(r);
}
SHIM(GetSystemTimeAdjustment) { ST32(A1, 156250); ST32(A2, 156250); ST32(A3, 1); RET(1); }
SHIM(SetSystemTime) { WX_FAIL(SetSystemTime, 1314, 0); }   // ERROR_PRIVILEGE_NOT_HELD
SHIM(SetLocalTime) { WX_FAIL(SetLocalTime, 1314, 0); }
SHIM(TzSpecificLocalTimeToSystemTime) {  // (tz, local SYSTEMTIME*, utc SYSTEMTIME*): host time zone
    u16 v[8]; memcpy(v, (void *)A2, 16);
    struct tm l = { 0 }; l.tm_year = v[0] - 1900; l.tm_mon = v[1] - 1; l.tm_mday = v[3]; l.tm_hour = v[4]; l.tm_min = v[5]; l.tm_sec = v[6]; l.tm_isdst = -1;
    time_t t = mktime(&l); struct tm g; gmtime_r(&t, &g);
    u16 o[8] = { (u16)(g.tm_year + 1900), (u16)(g.tm_mon + 1), (u16)g.tm_wday, (u16)g.tm_mday, (u16)g.tm_hour, (u16)g.tm_min, (u16)g.tm_sec, v[7] };
    memcpy((void *)A3, o, 16); RET(1);
}

// Locales: a small table; everything else resolves to en-US like a fresh Windows install.
static const struct { u32 lcid; const char *name, *lang, *country; } locales[] = {
    { 0x0409, "en-US", "English", "United States" }, { 0x0809, "en-GB", "English", "United Kingdom" }, { 0x0407, "de-DE", "German", "Germany" },
    { 0x040c, "fr-FR", "French", "France" }, { 0x0c0a, "es-ES", "Spanish", "Spain" }, { 0x0410, "it-IT", "Italian", "Italy" },
    { 0x0411, "ja-JP", "Japanese", "Japan" }, { 0x0412, "ko-KR", "Korean", "Korea" }, { 0x0804, "zh-CN", "Chinese", "China" },
    { 0x0404, "zh-TW", "Chinese", "Taiwan" }, { 0x0419, "ru-RU", "Russian", "Russia" }, { 0x0415, "pl-PL", "Polish", "Poland" },
    { 0x0416, "pt-BR", "Portuguese", "Brazil" }, { 0x0816, "pt-PT", "Portuguese", "Portugal" }, { 0x0413, "nl-NL", "Dutch", "Netherlands" },
    { 0x041d, "sv-SE", "Swedish", "Sweden" }, { 0x041f, "tr-TR", "Turkish", "Turkey" }, { 0x0422, "uk-UA", "Ukrainian", "Ukraine" },
};
#define NLOC (sizeof locales / sizeof *locales)
static int loc_by_lcid(u32 l) { if (l == 0 || l == 0x400 || l == 0x800 || l == 0x7f) l = 0x409; for (unsigned i = 0; i < NLOC; i++) if (locales[i].lcid == l) return (int)i; return -1; }
static int loc_by_name(const char *n) {
    if (!n || !*n || !strcmp(n, "!x-sys-default-locale")) return 0;
    for (unsigned i = 0; i < NLOC; i++) if (!strcasecmp(locales[i].name, n)) return (int)i;
    return -1;
}
SHIM(LocaleNameToLCID) { char n[86]; w2a_in(A1, 1, n, sizeof n); int i = loc_by_name(A1 ? n : NULL); if (i < 0) { rt_set_last_error(ERR_PARAM); RET(0); } RET(locales[i].lcid); }
SHIM(LCIDToLocaleName) {  // (lcid, out, cch, flags) -> chars incl NUL
    int i = loc_by_lcid((u32)A1); if (i < 0) { rt_set_last_error(ERR_PARAM); RET(0); }
    size_t need = strlen(locales[i].name) + 1;
    if (!A2 || !(u32)A3) RET(need);
    if ((u32)A3 < need) { rt_set_last_error(ERR_INSUFF); RET(0); }
    wx_u2w(locales[i].name, A2, (u32)A3); RET(need);
}
SHIM(IsValidLocale) { RET(loc_by_lcid((u32)A1) >= 0); }
SHIM(IsValidLocaleName) { char n[86]; w2a_in(A1, 1, n, sizeof n); RET(loc_by_name(n) >= 0); }
SHIM(ResolveLocaleName) {  // (name, out, cch): "en" -> "en-US"
    char n[86]; w2a_in(A1, 1, n, sizeof n); int idx = 0;
    if (A1 && *n) { idx = -1; for (unsigned i = 0; i < NLOC; i++) if (!strncasecmp(locales[i].name, n, strlen(n))) { idx = (int)i; break; } }
    const char *r = idx < 0 ? "" : locales[idx].name; size_t need = strlen(r) + 1;
    if (!A2) RET(need);
    if ((u32)A3 < need) { rt_set_last_error(ERR_INSUFF); RET(0); }
    wx_u2w(r, A2, (u32)A3); RET(need);
}
static int enum_locales(Ctx *c, int wide) {
    for (unsigned i = 0; i < NLOC; i++) {
        char s[16]; snprintf(s, sizeof s, "%08x", locales[i].lcid);
        u16 w[16]; wx_u2w(s, (u64)w, 16);
        u64 arg = wide ? (u64)w : (u64)s;
        if (!(u32)rt_guest_call(c, A1, 1, &arg)) break;
    }
    return 1;
}
SHIM(EnumSystemLocalesW) { RET(enum_locales(c, 1)); }
SHIM(EnumSystemLocalesA) { RET(enum_locales(c, 0)); }
SHIM(GetUserGeoID) { RET(244); }   // United States
SHIM(GetGeoInfoW) {  // (geoid, type, out, cch, lang): GEO_NATION 1, GEO_ISO2 4, GEO_ISO3 5, GEO_FRIENDLYNAME 8, GEO_OFFICIALNAME 9
    const char *s; u32 t = (u32)A2;
    s = t == 1 ? "244" : t == 4 ? "US" : t == 5 ? "USA" : t == 8 ? "United States" : t == 9 ? "United States of America" : t == 2 ? "39.45" : t == 3 ? "-98.908" : NULL;
    if (!s) { rt_set_last_error(1004); RET(0); }   // ERROR_INVALID_FLAGS
    size_t need = strlen(s) + 1;
    if (!(u32)A4) RET(need);
    if ((u32)A4 < need) { rt_set_last_error(ERR_INSUFF); RET(0); }
    wx_u2w(s, A3, (u32)A4); RET(need);
}
SHIM(GetUserDefaultGeoName) { size_t need = 3; if (!(u32)A2) RET(need); if ((u32)A2 < need) { rt_set_last_error(ERR_INSUFF); RET(0); } wx_u2w("US", A1, (u32)A2); RET(need); }

// Picture-string formatting for GetDateFormat / GetTimeFormat (en-US names).
static const char *const mon_l[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static const char *const day_l[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static void now_systemtime(u16 v[8]) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm l; localtime_r(&ts.tv_sec, &l);
    v[0] = (u16)(l.tm_year + 1900); v[1] = (u16)(l.tm_mon + 1); v[2] = (u16)l.tm_wday; v[3] = (u16)l.tm_mday;
    v[4] = (u16)l.tm_hour; v[5] = (u16)l.tm_min; v[6] = (u16)l.tm_sec; v[7] = (u16)(ts.tv_nsec / 1000000);
}
static int dow(int y, int m, int d) { static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 }; if (m < 3) y--; return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7; }
static void fmt_picture(const char *pic, const u16 *v, int date, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = pic; *p && o + 32 < n; ) {
        char ch = *p; int k = 1;
        if (ch == '\'') {   // quoted literal; '' is a quote
            p++;
            while (*p && o + 2 < n) { if (*p == '\'') { if (p[1] == '\'') { out[o++] = '\''; p += 2; continue; } p++; break; } out[o++] = *p++; }
            continue;
        }
        while (p[k] == ch) k++;
        if (date && ch == 'd') {
            if (k == 1) o += (size_t)sprintf(out + o, "%d", v[3]); else if (k == 2) o += (size_t)sprintf(out + o, "%02d", v[3]);
            else { int w = dow(v[0], v[1], v[3]); o += (size_t)sprintf(out + o, "%.*s", k == 3 ? 3 : 32, day_l[w]); }
        } else if (date && ch == 'M') {
            if (k == 1) o += (size_t)sprintf(out + o, "%d", v[1]); else if (k == 2) o += (size_t)sprintf(out + o, "%02d", v[1]);
            else o += (size_t)sprintf(out + o, "%.*s", k == 3 ? 3 : 32, mon_l[(v[1] + 11) % 12]);
        } else if (date && ch == 'y') {
            if (k == 1) o += (size_t)sprintf(out + o, "%d", v[0] % 100); else if (k == 2) o += (size_t)sprintf(out + o, "%02d", v[0] % 100);
            else o += (size_t)sprintf(out + o, "%d", v[0]);
        } else if (date && ch == 'g') o += (size_t)sprintf(out + o, "A.D.");
        else if (!date && (ch == 'h' || ch == 'H')) {
            int h = ch == 'h' ? (v[4] % 12 ? v[4] % 12 : 12) : v[4];
            o += (size_t)sprintf(out + o, k >= 2 ? "%02d" : "%d", h);
        } else if (!date && ch == 'm') o += (size_t)sprintf(out + o, k >= 2 ? "%02d" : "%d", v[5]);
        else if (!date && ch == 's') o += (size_t)sprintf(out + o, k >= 2 ? "%02d" : "%d", v[6]);
        else if (!date && ch == 't') o += (size_t)sprintf(out + o, "%.*s", k >= 2 ? 2 : 1, v[4] < 12 ? "AM" : "PM");
        else { for (int i = 0; i < k; i++) out[o++] = ch; }
        p += k;
    }
    out[o] = 0;
}
static u32 put_result(const char *s, u64 out, u32 cch, int wide) {   // returns chars incl NUL; 0 + ERROR_INSUFFICIENT_BUFFER when too small
    size_t need = wide ? wx_u2w(s, 0, 0) : strlen(s) + 1;
    if (!cch) return (u32)need;
    if (cch < need) { rt_set_last_error(ERR_INSUFF); return 0; }
    if (wide) wx_u2w(s, out, cch); else memcpy((void *)out, s, need);
    return (u32)need;
}
static u32 date_fmt(u32 flags, u64 st, u64 fmt, u64 out, u32 cch, int wide) {
    u16 v[8]; if (st) memcpy(v, (void *)st, 16); else now_systemtime(v);
    if (v[1] < 1 || v[1] > 12 || v[3] < 1 || v[3] > 31) { rt_set_last_error(ERR_PARAM); return 0; }
    char pic[256];
    if (fmt) w2a_in(fmt, wide, pic, sizeof pic);
    else snprintf(pic, sizeof pic, "%s", (flags & 2) ? "dddd, MMMM d, yyyy" : (flags & 8) ? "MMMM yyyy" : "M/d/yyyy");   // DATE_LONGDATE 2, DATE_YEARMONTH 8
    char s[512]; fmt_picture(pic, v, 1, s, sizeof s);
    return put_result(s, out, cch, wide);
}
SHIM(GetDateFormatW) { RET(date_fmt((u32)A2, A3, A4, AN(5), (u32)AN(6), 1)); }   // (lcid, flags, st, fmt, out, cch)
SHIM(GetDateFormatA) { RET(date_fmt((u32)A2, A3, A4, AN(5), (u32)AN(6), 0)); }
SHIM(GetDateFormatEx) { RET(date_fmt((u32)A2, A3, A4, AN(5), (u32)AN(6), 1)); }   // (locale, flags, st, fmt, out, cch, calendar)
static u32 time_fmt(u32 flags, u64 st, u64 fmt, u64 out, u32 cch, int wide) {
    u16 v[8]; if (st) memcpy(v, (void *)st, 16); else now_systemtime(v);
    if (v[4] > 23 || v[5] > 59 || v[6] > 59) { rt_set_last_error(ERR_PARAM); return 0; }
    char pic[256];
    if (fmt) w2a_in(fmt, wide, pic, sizeof pic);
    else {   // TIME_NOMINUTESORSECONDS 1, TIME_NOSECONDS 2, TIME_NOTIMEMARKER 4, TIME_FORCE24HOURFORMAT 8
        const char *h = (flags & 8) ? "H" : "h";
        snprintf(pic, sizeof pic, "%s%s%s%s", h, (flags & 1) ? "" : ":mm", (flags & 3) ? "" : ":ss", (flags & 12) ? "" : " tt");
    }
    char s[512]; fmt_picture(pic, v, 0, s, sizeof s);
    return put_result(s, out, cch, wide);
}
SHIM(GetTimeFormatW) { RET(time_fmt((u32)A2, A3, A4, AN(5), (u32)AN(6), 1)); }
SHIM(GetTimeFormatA) { RET(time_fmt((u32)A2, A3, A4, AN(5), (u32)AN(6), 0)); }
SHIM(GetTimeFormatEx) { RET(time_fmt((u32)A2, A3, A4, AN(5), (u32)AN(6), 1)); }   // (locale, flags, st, fmt, out, cch)
// NUMBERFMT { UINT digits, leadingZero, grouping; LPWSTR decSep, thousandSep; UINT negativeOrder }
static u32 number_fmt(const char *val, u64 nf, int wide, int currency, u64 out, u32 cch) {
    u32 digits = 2, lead = 1, group = 3, neg = currency ? 0 : 1; char dec[8] = ".", thou[8] = ",";
    if (nf) {
        digits = LD32(nf); lead = LD32(nf + 4); group = LD32(nf + 8);
        w2a_in(LD64(nf + 16), wide, dec, sizeof dec); w2a_in(LD64(nf + 24), wide, thou, sizeof thou); neg = LD32(nf + 32);
    }
    const char *p = val; int negative = 0;
    if (*p == '-') { negative = 1; p++; }
    char ip[128] = "", fp[128] = ""; size_t il = 0, fl = 0;
    while (*p >= '0' && *p <= '9' && il < 120) ip[il++] = *p++;
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9' && fl < 120) fp[fl++] = *p++; }
    if (*p) { rt_set_last_error(ERR_PARAM); return 0; }
    // round to `digits`
    char num[256]; snprintf(num, sizeof num, "%s%s", ip, fp);
    size_t keep = il + digits; while (strlen(num) < keep) strcat(num, "0");
    int carry = strlen(num) > keep && num[keep] >= '5'; num[keep] = 0;
    for (long i = (long)keep - 1; carry && i >= 0; i--) { if (num[i] == '9') num[i] = '0'; else { num[i]++; carry = 0; } }
    if (carry) { memmove(num + 1, num, strlen(num) + 1); num[0] = '1'; il++; }
    char ipart[256]; memcpy(ipart, num, il); ipart[il] = 0;
    char *fpart = num + il;
    // strip leading zeros of the integer part
    char *ib = ipart; while (*ib == '0' && ib[1]) ib++;
    if (!*ib || (!strcmp(ib, "0") && !lead)) ib = lead ? "0" : "";
    char g[512] = ""; size_t L = strlen(ib), o = 0;
    for (size_t i = 0; i < L; i++) { if (group && i && (L - i) % group == 0) o += (size_t)sprintf(g + o, "%s", thou); g[o++] = ib[i]; g[o] = 0; }
    char body[600]; snprintf(body, sizeof body, "%s%s%s%s", currency ? "$" : "", g, digits ? dec : "", digits ? fpart : "");
    int allzero = 1; for (const char *q = num; *q; q++) if (*q != '0') allzero = 0;
    char s[700];
    if (negative && !allzero) {
        if (currency) snprintf(s, sizeof s, neg == 0 ? "(%s)" : "-%s", body);
        else snprintf(s, sizeof s, neg == 0 ? "(%s)" : neg == 2 ? "- %s" : neg == 3 ? "%s-" : neg == 4 ? "%s -" : "-%s", body);
    } else snprintf(s, sizeof s, "%s", body);
    return put_result(s, out, cch, wide);
}
SHIM(GetNumberFormatW) { char v[256]; w2a_in(A3, 1, v, sizeof v); RET(number_fmt(v, A4, 1, 0, AN(5), (u32)AN(6))); }   // (lcid, flags, value, fmt, out, cch)
SHIM(GetNumberFormatA) { char v[256]; w2a_in(A3, 0, v, sizeof v); RET(number_fmt(v, A4, 0, 0, AN(5), (u32)AN(6))); }
SHIM(GetNumberFormatEx) { char v[256]; w2a_in(A3, 1, v, sizeof v); RET(number_fmt(v, A4, 1, 0, AN(5), (u32)AN(6))); }
SHIM(GetCurrencyFormatW) { char v[256]; w2a_in(A3, 1, v, sizeof v); RET(number_fmt(v, 0, 1, 1, AN(5), (u32)AN(6))); }
SHIM(GetCurrencyFormatEx) { char v[256]; w2a_in(A3, 1, v, sizeof v); RET(number_fmt(v, 0, 1, 1, AN(5), (u32)AN(6))); }
static int ci_cmp_w(u64 a, int na, u64 b, int nb, int ci) {
    if (na < 0) na = (int)wx_wlen(a); if (nb < 0) nb = (int)wx_wlen(b);
    for (int i = 0; ; i++) {
        if (i == na || i == nb) return na == nb ? 0 : i == na ? -1 : 1;
        u32 x = LD16(a + 2 * i), y = LD16(b + 2 * i);
        if (ci) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; if (x >= 0xE0 && x <= 0xFE && x != 0xF7) x -= 32; if (y >= 0xE0 && y <= 0xFE && y != 0xF7) y -= 32; }
        if (x != y) return x < y ? -1 : 1;
    }
}
SHIM(CompareStringOrdinal) { RET((u32)(ci_cmp_w(A1, (int)(i32)A2, A3, (int)(i32)A4, (int)(u32)AN(5)) + 2)); }   // CSTR_LESS_THAN 1 / EQUAL 2 / GREATER 3
SHIM(CompareStringEx) { ShimFn f = shim_fn("CompareStringW"); u64 s = c->r[RCX]; c->r[RCX] = 0x409; f(c); c->r[RCX] = s; }
SHIM(LCMapStringEx) { ShimFn f = shim_fn("LCMapStringW"); u64 s = c->r[RCX]; c->r[RCX] = 0x409; f(c); c->r[RCX] = s; }
SHIM(LCMapStringA) {  // (lcid, flags, src, n, dst, dn): lower/upper case only
    u32 fl = (u32)A2; int n = (int)(i32)A4; const u8 *s = (const u8 *)A3;
    if (n < 0) n = (int)strlen((const char *)s) + 1;
    if (!(u32)AN(6)) RET((u32)n);
    if ((u32)AN(6) < (u32)n) { rt_set_last_error(ERR_INSUFF); RET(0); }
    u8 *d = (u8 *)AN(5);
    for (int i = 0; i < n; i++) { u8 ch = s[i]; if ((fl & 0x100) && ch >= 'A' && ch <= 'Z') ch += 32; if ((fl & 0x200) && ch >= 'a' && ch <= 'z') ch -= 32; d[i] = ch; }
    RET((u32)n);
}
static u16 ctype1(u32 ch) {   // C1_UPPER 1, LOWER 2, DIGIT 4, SPACE 8, PUNCT 0x10, CNTRL 0x20, BLANK 0x40, XDIGIT 0x80, ALPHA 0x100, DEFINED 0x200
    u16 f = 0x200;
    if (ch >= 'A' && ch <= 'Z') f |= 0x101; else if (ch >= 'a' && ch <= 'z') f |= 0x102;
    else if (ch >= '0' && ch <= '9') f |= 4;
    if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) f |= 0x80;
    if (ch == ' ' || (ch >= 9 && ch <= 13)) f |= 8;
    if (ch == ' ' || ch == '\t') f |= 0x40;
    if (ch < 32 || ch == 127) f |= 0x20;
    if (ch > 32 && ch < 127 && !(f & 0x104)) f |= 0x10;
    if (ch >= 0xC0 && ch <= 0xFF && ch != 0xD7 && ch != 0xF7) f |= ch < 0xDF ? 0x101 : 0x102;
    return f;
}
SHIM(GetStringTypeExA) {  // (lcid, type, src, n, out)
    if ((u32)A2 != 1) WX_FAIL(GetStringTypeExA(non-CT_CTYPE1), 1004, 0);
    int n = (int)(i32)A4; if (n < 0) n = (int)strlen((const char *)A3) + 1;
    for (int i = 0; i < n; i++) ST16(AN(5) + 2 * i, ctype1(LD8(A3 + i)));
    RET(1);
}
SHIM(GetStringTypeA) { u64 s = c->r[RDX]; c->r[RDX] = c->r[RDX]; (void)s; sh_GetStringTypeExA(c); }   // same layout (lcid, type, src, n, out)
// lstr* (ANSI and wide).
SHIM(lstrlenW) { RET(A1 ? wx_wlen(A1) : 0); }
SHIM(lstrlenA) { RET(A1 ? strlen((const char *)A1) : 0); }
SHIM(lstrcmpW) { RET((u32)ci_cmp_w(A1, -1, A2, -1, 0)); }
SHIM(lstrcmpiW) { RET((u32)ci_cmp_w(A1, -1, A2, -1, 1)); }
SHIM(lstrcmpA) { int r = strcmp((const char *)A1, (const char *)A2); RET((u32)(r < 0 ? -1 : r > 0)); }
SHIM(lstrcmpiA) { int r = strcasecmp((const char *)A1, (const char *)A2); RET((u32)(r < 0 ? -1 : r > 0)); }
SHIM(lstrcpyW) { memmove((void *)A1, (void *)A2, 2 * (wx_wlen(A2) + 1)); RET(A1); }
SHIM(lstrcpyA) { memmove((void *)A1, (void *)A2, strlen((const char *)A2) + 1); RET(A1); }
SHIM(lstrcpynW) { int n = (int)(i32)A3; if (n <= 0) RET(A1); size_t l = wx_wlen(A2); if (l > (size_t)n - 1) l = (size_t)n - 1; memmove((void *)A1, (void *)A2, 2 * l); ST16(A1 + 2 * l, 0); RET(A1); }
SHIM(lstrcpynA) { int n = (int)(i32)A3; if (n <= 0) RET(A1); size_t l = strlen((const char *)A2); if (l > (size_t)n - 1) l = (size_t)n - 1; memmove((void *)A1, (void *)A2, l); ST8(A1 + l, 0); RET(A1); }
SHIM(lstrcatW) { size_t l = wx_wlen(A1); memmove((void *)(A1 + 2 * l), (void *)A2, 2 * (wx_wlen(A2) + 1)); RET(A1); }
SHIM(lstrcatA) { strcat((char *)A1, (const char *)A2); RET(A1); }

// Environment-string expansion with the variables a Windows profile has.
static const char *win_env(const char *n, char *buf, size_t sz) {
    static const char *const fixed[][2] = {
        { "USERPROFILE", "C:\\users\\wintomac" }, { "HOMEDRIVE", "C:" }, { "HOMEPATH", "\\users\\wintomac" },
        { "APPDATA", "C:\\users\\wintomac\\AppData\\Roaming" }, { "LOCALAPPDATA", "C:\\users\\wintomac\\AppData\\Local" },
        { "TEMP", "C:\\tmp" }, { "TMP", "C:\\tmp" }, { "SystemRoot", "C:\\windows" }, { "windir", "C:\\windows" }, { "SystemDrive", "C:" },
        { "ProgramFiles", "C:\\Program Files" }, { "ProgramFiles(x86)", "C:\\Program Files (x86)" }, { "ProgramData", "C:\\ProgramData" },
        { "PUBLIC", "C:\\users\\Public" }, { "USERNAME", "wintomac" }, { "COMPUTERNAME", "WINTOMAC" }, { "OS", "Windows_NT" },
        { "PROCESSOR_ARCHITECTURE", "AMD64" }, { "ComSpec", "C:\\windows\\system32\\cmd.exe" },
    };
    const char *v = getenv(n);
    if (v) { snprintf(buf, sz, "%s", v); return buf; }
    for (unsigned i = 0; i < sizeof fixed / sizeof *fixed; i++) if (!strcasecmp(fixed[i][0], n)) return fixed[i][1];
    if (!strcasecmp(n, "NUMBER_OF_PROCESSORS")) { int k = 1; size_t s = sizeof k; sysctlbyname("hw.logicalcpu", &k, &s, NULL, 0); snprintf(buf, sz, "%d", k); return buf; }
    return NULL;
}
static void expand_env(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; ) {
        const char *e = *p == '%' ? strchr(p + 1, '%') : NULL;
        if (e && e > p + 1) {
            char name[256]; snprintf(name, sizeof name, "%.*s", (int)(e - p - 1), p + 1);
            char buf[1024]; const char *v = win_env(name, buf, sizeof buf);
            if (v) { o += (size_t)snprintf(out + o, n - o, "%s", v); if (o >= n) o = n - 1; p = e + 1; continue; }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}
SHIM(ExpandEnvironmentStringsW) {  // (src, dst, n) -> chars incl NUL
    char in[4096], out[8192]; wx_w2u(A1, in, sizeof in); expand_env(in, out, sizeof out);
    size_t need = wx_u2w(out, 0, 0);
    if (A2 && (u32)A3 >= need) wx_u2w(out, A2, (u32)A3);
    RET(need);
}
SHIM(ExpandEnvironmentStringsA) {
    char out[8192]; expand_env((const char *)A1, out, sizeof out);
    size_t need = strlen(out) + 1;
    if (A2 && (u32)A3 >= need) memcpy((void *)A2, out, need);
    RET(need);
}
static u32 computer_name(u64 buf, u64 pn, int wide) {   // *pn: in = capacity, out = length (or needed incl NUL)
    const char *nm = "WINTOMAC"; u32 need = (u32)strlen(nm) + 1, cap = LD32(pn);
    if (!buf || cap < need) { ST32(pn, need); rt_set_last_error(ERR_MORE_DATA); return 0; }
    if (wide) wx_u2w(nm, buf, cap); else memcpy((void *)buf, nm, need);
    ST32(pn, need - 1); return 1;
}
SHIM(GetComputerNameW) { RET(computer_name(A1, A2, 1)); }
SHIM(GetComputerNameA) { RET(computer_name(A1, A2, 0)); }
SHIM(GetComputerNameExW) { RET(computer_name(A2, A3, 1)); }   // (format, buf, *n)
SHIM(GetComputerNameExA) { RET(computer_name(A2, A3, 0)); }

// ==== processes, threads, modules ========================================================================================
SHIM(GetProcessId) { if (A1 == ~0ull) RET(getpid()); rt_set_last_error(ERR_HANDLE); RET(0); }
SHIM(GetThreadId) { if (A1 == ~1ull) RET(LD32(c->gs_base + 0x48)); WX_FAIL(GetThreadId(other thread), ERR_HANDLE, 0); }
SHIM(GetProcessIdOfThread) { RET(getpid()); }
SHIM(GetThreadTimes) {  // (h, *creation, *exit, *kernel, *user): CPU times of the calling thread
    u64 kt = 0, ut = 0;
    if (A1 == ~1ull) {
        thread_basic_info_data_t info; mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
        mach_port_t self = mach_thread_self();
        if (thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &cnt) == KERN_SUCCESS) {
            ut = (u64)info.user_time.seconds * 10000000ull + (u64)info.user_time.microseconds * 10;
            kt = (u64)info.system_time.seconds * 10000000ull + (u64)info.system_time.microseconds * 10;
        }
        mach_port_deallocate(mach_task_self(), self);
    }
    if (A2) ST64(A2, 0); if (A3) ST64(A3, 0); if (A4) ST64(A4, kt); if (AN(5)) ST64(AN(5), ut);
    RET(1);
}
SHIM(GetPriorityClass) { RET(0x20); }   // NORMAL_PRIORITY_CLASS
SHIM(SetPriorityClass) { RET(1); }
SHIM(SetThreadIdealProcessor) { RET(0); }
SHIM(SetThreadIdealProcessorEx) { if (A3) { ST16(A3, 0); ST8(A3 + 2, 0); } RET(1); }
static int ncpu(void) { int k = 1; size_t s = sizeof k; sysctlbyname("hw.logicalcpu", &k, &s, NULL, 0); return k < 1 ? 1 : k > 64 ? 64 : k; }
static u64 all_mask(void) { int n = ncpu(); return n >= 64 ? ~0ull : (1ull << n) - 1; }
SHIM(GetThreadGroupAffinity) { memset((void *)A2, 0, 16); ST64(A2, all_mask()); RET(1); }   // GROUP_AFFINITY { mask, group, reserved[3] }
SHIM(SetThreadGroupAffinity) { if (A3) { memset((void *)A3, 0, 16); ST64(A3, all_mask()); } RET(1); }
SHIM(TerminateThread) { WX_FAIL(TerminateThread, ERR_ACCESS, 0); }   // a pthread cannot be killed safely
void gfx_main_service(void);   // ps5_gfx.m
SHIM(SleepEx) { gfx_main_service(); u32 ms = (u32)A1; if (ms == 0) sched_yield(); else if (ms != 0xFFFFFFFF) usleep(ms * 1000u); else for (;;) pause(); RET(0); }
SHIM(WaitForMultipleObjectsEx) { ShimFn f = shim_fn("WaitForMultipleObjects"); f(c); }   // same first four arguments
SHIM(SignalObjectAndWait) {  // (signal, wait, ms, alertable)
    if (!call4(c, "SetEvent", A1, 0, 0, 0) && !call4(c, "ReleaseMutex", A1, 0, 0, 0)) call4(c, "ReleaseSemaphore", A1, 1, 0, 0);
    RET(call4(c, "WaitForSingleObject", A2, A3, 0, 0));
}
SHIM(OpenEventW) { WX_FAIL(OpenEventW, ERR_FILE_NOT_FOUND, 0); }   // named objects aren't shared: none exists to open
SHIM(OpenEventA) { WX_FAIL(OpenEventA, ERR_FILE_NOT_FOUND, 0); }
SHIM(OpenMutexW) { WX_FAIL(OpenMutexW, ERR_FILE_NOT_FOUND, 0); }
SHIM(OpenMutexA) { WX_FAIL(OpenMutexA, ERR_FILE_NOT_FOUND, 0); }
SHIM(OpenSemaphoreW) { WX_FAIL(OpenSemaphoreW, ERR_FILE_NOT_FOUND, 0); }
SHIM(OpenSemaphoreA) { WX_FAIL(OpenSemaphoreA, ERR_FILE_NOT_FOUND, 0); }
SHIM(QueueUserAPC) { WX_FAIL(QueueUserAPC, ERR_NOT_SUPPORTED, 0); }
SHIM(FlushProcessWriteBuffers) { __atomic_thread_fence(__ATOMIC_SEQ_CST); RET(0); }

// One-time initialisation. INIT_ONCE holds 0 (fresh), 1 (running) or context|2 (done).
static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t once_cond = PTHREAD_COND_INITIALIZER;
SHIM(InitOnceInitialize) { ST64(A1, 0); RET(0); }
static int once_begin(u64 once, int check_only, int *pending, u64 *ctx) {   // returns 0 when check_only and not done
    pthread_mutex_lock(&once_lock);
    for (;;) {
        u64 v = LD64(once);
        if ((v & 3) == 2) { *pending = 0; *ctx = v & ~3ull; pthread_mutex_unlock(&once_lock); return 1; }
        if (check_only) { pthread_mutex_unlock(&once_lock); return 0; }
        if (v == 0) { ST64(once, 1); *pending = 1; *ctx = 0; pthread_mutex_unlock(&once_lock); return 1; }
        pthread_cond_wait(&once_cond, &once_lock);
    }
}
static void once_end(u64 once, int ok, u64 ctx) {
    pthread_mutex_lock(&once_lock); ST64(once, ok ? ((ctx & ~3ull) | 2) : 0); pthread_cond_broadcast(&once_cond); pthread_mutex_unlock(&once_lock);
}
SHIM(InitOnceExecuteOnce) {  // (once, fn, param, *context): fn(once, param, &context) -> BOOL
    int pending; u64 ctx; u64 once = A1, fn = A2, param = A3, pctx = A4;
    once_begin(once, 0, &pending, &ctx);
    if (pending) {
        u64 local = 0, args[3] = { once, param, (u64)&local };
        int ok = (int)(u32)rt_guest_call(c, fn, 3, args);
        once_end(once, ok, local); ctx = local;
        if (!ok) RET(0);
    }
    if (pctx) ST64(pctx, ctx & ~3ull);
    RET(1);
}
SHIM(InitOnceBeginInitialize) {  // (once, flags, *pending, *context): INIT_ONCE_CHECK_ONLY 1
    int pending; u64 ctx;
    if (!once_begin(A1, (u32)A2 & 1, &pending, &ctx)) { rt_set_last_error(524); RET(0); }   // ERROR_GEN_FAILURE-ish: not done yet
    if (A3) ST32(A3, (u32)pending); if (A4) ST64(A4, ctx);
    RET(1);
}
SHIM(InitOnceComplete) { once_end(A1, !((u32)A2 & 4), A3); RET(1); }   // (once, flags (INIT_ONCE_INIT_FAILED 4), context)

// Heaps: every heap is the process heap (malloc); HeapAlloc & co. ignore the handle.
static u64 heap_next = 0x7E000000;
SHIM(HeapCreate) { RET(__atomic_add_fetch(&heap_next, 0x10, __ATOMIC_SEQ_CST)); }
SHIM(HeapDestroy) { RET(1); }
SHIM(HeapCompact) { RET(0x100000); }
SHIM(HeapValidate) { RET(1); }
SHIM(HeapSetInformation) { RET(1); }
SHIM(HeapQueryInformation) { if (A4 >= 4 && A3) ST32(A3, 2); if (AN(5)) ST64(AN(5), 4); RET(1); }   // HeapCompatibilityInformation: LFH
SHIM(HeapLock) { RET(1); }
SHIM(HeapUnlock) { RET(1); }
SHIM(VirtualAllocEx) { RET(call4(c, "VirtualAlloc", A2, A3, A4, AN(5))); }   // (proc, addr, size, type, prot)
SHIM(VirtualFreeEx) { RET(call4(c, "VirtualFree", A2, A3, A4, 0)); }
SHIM(VirtualQueryEx) { RET(call4(c, "VirtualQuery", A2, A3, A4, 0)); }
SHIM(VirtualProtectEx) { RET(call4(c, "VirtualProtect", A2, A3, A4, AN(5))); }

// Processor topology: one package, one NUMA node, one core per logical CPU (no SMT on Apple silicon), private L1s, shared L2.
static u64 cache_size(const char *n, u64 def) { u64 v = 0; size_t s = sizeof v; return sysctlbyname(n, &v, &s, NULL, 0) || !v ? def : v; }
SHIM(GetLogicalProcessorInformation) {  // (buf, *len): SYSTEM_LOGICAL_PROCESSOR_INFORMATION is 32 bytes
    int n = ncpu(); u32 cnt = (u32)n * 3 + 3, need = cnt * 32;
    if (LD32(A2) < need || !A1) { ST32(A2, need); rt_set_last_error(ERR_INSUFF); RET(0); }
    u64 p = A1; memset((void *)p, 0, need);
    u64 l1d = cache_size("hw.l1dcachesize", 65536), l1i = cache_size("hw.l1icachesize", 131072), l2 = cache_size("hw.l2cachesize", 4194304);
    u16 line = (u16)cache_size("hw.cachelinesize", 64);
    for (int i = 0; i < n; i++) {
        ST64(p, 1ull << i); ST32(p + 8, 0); ST8(p + 16, 0); p += 32;                                                             // core
        ST64(p, 1ull << i); ST32(p + 8, 2); ST8(p + 16, 1); ST8(p + 17, 8); ST16(p + 18, line); ST32(p + 20, (u32)l1d); ST32(p + 24, 2); p += 32;   // L1 data
        ST64(p, 1ull << i); ST32(p + 8, 2); ST8(p + 16, 1); ST8(p + 17, 8); ST16(p + 18, line); ST32(p + 20, (u32)l1i); ST32(p + 24, 1); p += 32;   // L1 instr
    }
    ST64(p, all_mask()); ST32(p + 8, 2); ST8(p + 16, 2); ST8(p + 17, 16); ST16(p + 18, line); ST32(p + 20, (u32)l2); ST32(p + 24, 0); p += 32;   // L2 unified
    ST64(p, all_mask()); ST32(p + 8, 3); p += 32;   // package
    ST64(p, all_mask()); ST32(p + 8, 1); ST32(p + 16, 0); p += 32;   // NUMA node 0
    ST32(A2, need); RET(1);
}
SHIM(GetLogicalProcessorInformationEx) {  // (relationship, buf, *len): variable-size SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX
    u32 rel = (u32)A1; int n = ncpu();
    int want_core = rel == 0 || rel == 0xFFFF, want_numa = rel == 1 || rel == 0xFFFF, want_cache = rel == 2 || rel == 0xFFFF,
        want_pkg = rel == 3 || rel == 0xFFFF, want_group = rel == 4 || rel == 0xFFFF;
    u32 need = (want_core ? 48u * (u32)n : 0) + (want_numa ? 48 : 0) + (want_cache ? 56u * (2u * (u32)n + 1) : 0) + (want_pkg ? 48 : 0) + (want_group ? 80 : 0);
    if (LD32(A3) < need || !A2) { ST32(A3, need); rt_set_last_error(ERR_INSUFF); RET(0); }
    u64 p = A2; memset((void *)p, 0, need);
    u64 l1d = cache_size("hw.l1dcachesize", 65536), l1i = cache_size("hw.l1icachesize", 131072), l2 = cache_size("hw.l2cachesize", 4194304);
    u16 line = (u16)cache_size("hw.cachelinesize", 64);
    if (want_core) for (int i = 0; i < n; i++) { ST32(p, 0); ST32(p + 4, 48); ST16(p + 30, 1); ST64(p + 32, 1ull << i); p += 48; }
    if (want_pkg) { ST32(p, 3); ST32(p + 4, 48); ST16(p + 30, 1); ST64(p + 32, all_mask()); p += 48; }
    if (want_numa) { ST32(p, 1); ST32(p + 4, 48); ST32(p + 8, 0); ST16(p + 30, 1); ST64(p + 32, all_mask()); p += 48; }
    if (want_cache) {
        for (int i = 0; i < n; i++) for (int k = 0; k < 2; k++) {
            ST32(p, 2); ST32(p + 4, 56); ST8(p + 8, 1); ST8(p + 9, 8); ST16(p + 10, line); ST32(p + 12, (u32)(k ? l1i : l1d)); ST32(p + 16, k ? 1 : 2);
            ST16(p + 38, 1); ST64(p + 40, 1ull << i); p += 56;
        }
        ST32(p, 2); ST32(p + 4, 56); ST8(p + 8, 2); ST8(p + 9, 16); ST16(p + 10, line); ST32(p + 12, (u32)l2); ST32(p + 16, 0);
        ST16(p + 38, 1); ST64(p + 40, all_mask()); p += 56;
    }
    if (want_group) { ST32(p, 4); ST32(p + 4, 80); ST16(p + 8, 1); ST16(p + 10, 1); ST8(p + 32, (u8)n); ST8(p + 33, (u8)n); ST64(p + 72, all_mask()); p += 80; }
    ST32(A3, need); RET(1);
}

// psapi / module queries: the program and the DLLs recompiled with it.
static u64 mod_base(u64 h) { return h ? h : rt_module_handle(NULL); }
static u32 module_size(u64 base) { u64 sz = 0; rt_module_base_for(base, &sz); if (!sz) { u32 lf = LD32(base + 0x3C); sz = LD32(base + lf + 24 + 56); } return (u32)sz; }
SHIM(K32EnumProcessModules) {  // (proc, HMODULE*, cb, *needed): the program image
    u64 b = rt_module_handle(NULL);
    if (AN(4)) {}
    ST32(A4, 8);
    if ((u32)A3 >= 8 && A2) ST64(A2, b);
    RET(1);
}
SHIM(K32EnumProcessModulesEx) { sh_K32EnumProcessModules(c); }
static u32 module_base_name(u64 h, u64 buf, u32 n, int wide) {
    const char *p = rt_module_path(mod_base(h)); if (!p) { rt_set_last_error(ERR_HANDLE); return 0; }
    const char *b = strrchr(p, '/'); b = b ? b + 1 : p; const char *b2 = strrchr(b, '\\'); if (b2) b = b2 + 1;
    if (!n) return 0;
    if (wide) { size_t need = wx_u2w(b, 0, 0); wx_u2w(b, buf, n); return (u32)(need - 1 < n ? need - 1 : n - 1); }
    snprintf((char *)buf, n, "%s", b); return (u32)strlen((char *)buf);
}
SHIM(K32GetModuleBaseNameW) { RET(module_base_name(A2, A3, (u32)A4, 1)); }   // (proc, hmod, buf, n)
SHIM(K32GetModuleBaseNameA) { RET(module_base_name(A2, A3, (u32)A4, 0)); }
SHIM(K32GetModuleFileNameExW) { RET(call4(c, "GetModuleFileNameW", A2, A3, A4, 0)); }
SHIM(K32GetModuleFileNameExA) { RET(call4(c, "GetModuleFileNameA", A2, A3, A4, 0)); }
SHIM(K32GetModuleInformation) {  // (proc, hmod, MODULEINFO*, cb): { base, u32 size, entry }
    u64 b = mod_base(A2); if ((u32)A4 < 24) { rt_set_last_error(ERR_INSUFF); RET(0); }
    u32 lf = LD32(b + 0x3C);
    ST64(A3, b); ST32(A3 + 8, module_size(b)); ST32(A3 + 12, 0); ST64(A3 + 16, b + LD32(b + lf + 24 + 16));
    RET(1);
}
SHIM(K32GetPerformanceInfo) {  // PERFORMANCE_INFORMATION (104 bytes), sizes in pages
    u64 mem = 0; size_t s = sizeof mem; sysctlbyname("hw.memsize", &mem, &s, NULL, 0);
    u64 pg = 4096, tot = mem / pg, avail = tot / 2, p = A1;
    memset((void *)p, 0, 104); ST32(p, 104);
    ST64(p + 8, tot / 2); ST64(p + 16, tot * 2); ST64(p + 24, tot / 2); ST64(p + 32, tot); ST64(p + 40, avail); ST64(p + 48, tot / 8);
    ST64(p + 56, tot / 64); ST64(p + 64, tot / 128); ST64(p + 72, tot / 128); ST64(p + 80, pg); ST32(p + 88, 50000); ST32(p + 92, 300); ST32(p + 96, 3000);
    RET(1);
}
static int full_image_name(Ctx *c, int wide) {   // (proc, flags, buf, *size)
    u32 cap = LD32(A4);
    u32 n = (u32)call4(c, wide ? "GetModuleFileNameW" : "GetModuleFileNameA", 0, A3, cap, 0);
    if (!n || n >= cap) { rt_set_last_error(ERR_INSUFF); return 0; }
    ST32(A4, n); return 1;
}
SHIM(QueryFullProcessImageNameW) { RET(full_image_name(c, 1)); }
SHIM(QueryFullProcessImageNameA) { RET(full_image_name(c, 0)); }
SHIM(Module32FirstW) { rt_set_last_error(ERR_NO_MORE_FILES); RET(0); }
SHIM(Module32NextW) { rt_set_last_error(ERR_NO_MORE_FILES); RET(0); }
SHIM(Module32First) { rt_set_last_error(ERR_NO_MORE_FILES); RET(0); }
SHIM(Module32Next) { rt_set_last_error(ERR_NO_MORE_FILES); RET(0); }
SHIM(Thread32First) { rt_set_last_error(ERR_NO_MORE_FILES); RET(0); }
SHIM(Thread32Next) { rt_set_last_error(ERR_NO_MORE_FILES); RET(0); }
static u64 dll_cookie = 0xD11C0000;
SHIM(AddDllDirectory) { RET(__atomic_add_fetch(&dll_cookie, 1, __ATOMIC_SEQ_CST)); }
SHIM(RemoveDllDirectory) { RET(1); }
SHIM(SetDllDirectoryW) { RET(1); }
SHIM(SetDllDirectoryA) { RET(1); }
SHIM(SetDefaultDllDirectories) { RET(1); }
SHIM(SetSearchPathMode) { RET(1); }
SHIM(SetProcessDEPPolicy) { RET(1); }
SHIM(GetProcessDEPPolicy) { if (A2) ST32(A2, 1); if (A3) ST32(A3, 1); RET(1); }
static u32 search_path(Ctx *c, int wide) {   // (path, file, ext, cch, buf, *filepart)
    char file[1024], ext[32] = "", path[4096] = "", cand[4200], full[4200], host[8192];
    w2a_in(A2, wide, file, sizeof file); if (A3) w2a_in(A3, wide, ext, sizeof ext);
    if (A3 && !strchr(file, '.')) strncat(file, ext, sizeof file - strlen(file) - 1);
    if (A1) w2a_in(A1, wide, path, sizeof path);
    else {   // program directory, then the current directory
        char exe[2048]; rt_win_path_from_host(rt_exe_dir(), exe, sizeof exe);
        snprintf(path, sizeof path, "%s;%s", exe, rt_win_cwd());
    }
    int has_dir = strchr(file, '\\') || strchr(file, '/') || strchr(file, ':');
    char *save = NULL;
    for (char *d = has_dir ? (char *)"" : strtok_r(path, ";", &save); d; d = has_dir ? NULL : strtok_r(NULL, ";", &save)) {
        if (has_dir) snprintf(cand, sizeof cand, "%s", file); else snprintf(cand, sizeof cand, "%s\\%s", d, file);
        rt_full_path(cand, full, sizeof full); rt_host_path(full, host, sizeof host);
        struct stat st;
        if (!stat(host, &st)) {
            u32 r = ret_str(full, AN(5), (u32)A4, wide);
            if (AN(6)) { const char *b = strrchr(full, '\\'); size_t idx = b ? (size_t)(b + 1 - full) : 0;
                         ST64(AN(6), r < (u32)A4 ? AN(5) + (wide ? 2 * (wx_u2w(full, 0, 0) - wx_u2w(full + idx, 0, 0)) : idx) : 0); }
            return r;
        }
        if (has_dir) break;
    }
    rt_set_last_error(ERR_FILE_NOT_FOUND); return 0;
}
SHIM(SearchPathW) { RET(search_path(c, 1)); }
SHIM(SearchPathA) { RET(search_path(c, 0)); }
SHIM(GetBinaryTypeA) {  // (app, *type): SCS_64BIT_BINARY 6 for PE32+, SCS_32BIT_BINARY 0
    char host[4096]; host_of(A1, 0, host, sizeof host);
    FILE *f = fopen(host, "rb"); if (!f) { rt_set_last_error(ERR_FILE_NOT_FOUND); RET(0); }
    u8 h[4096] = { 0 }; size_t n = fread(h, 1, sizeof h, f); fclose(f);
    if (n < 64 || h[0] != 'M' || h[1] != 'Z') { rt_set_last_error(193); RET(0); }   // ERROR_BAD_EXE_FORMAT
    u32 lf; memcpy(&lf, h + 0x3C, 4);
    u16 mach = lf + 6 < n ? (u16)(h[lf + 4] | h[lf + 5] << 8) : 0;
    ST32(A2, mach == 0x8664 ? 6 : 0); RET(1);
}
SHIM(GetBinaryTypeW) { char p[2048]; wx_w2u(A1, p, sizeof p); u64 s = c->r[RCX]; c->r[RCX] = (u64)p; sh_GetBinaryTypeA(c); c->r[RCX] = s; }
SHIM(WTSGetActiveConsoleSessionId) { RET(1); }
SHIM(ProcessIdToSessionId) { ST32(A2, 1); RET(1); }
static u64 obj_next = 0x7D000000;
static u64 new_obj(void) { return __atomic_add_fetch(&obj_next, 4, __ATOMIC_SEQ_CST); }
SHIM(PowerCreateRequest) { RET(new_obj()); }
SHIM(PowerSetRequest) { RET(1); }
SHIM(PowerClearRequest) { RET(1); }
SHIM(CreateJobObjectW) { RET(new_obj()); }
SHIM(CreateJobObjectA) { RET(new_obj()); }
SHIM(AssignProcessToJobObject) { RET(1); }
SHIM(SetInformationJobObject) { RET(1); }
SHIM(IsProcessInJob) { ST32(A3, 0); RET(1); }
SHIM(InitializeProcThreadAttributeList) {  // (list, count, flags, *size)
    u64 need = 48 + 24 * (u64)(u32)A2;
    if (!A1 || LD64(A4) < need) { ST64(A4, need); rt_set_last_error(ERR_INSUFF); RET(0); }
    memset((void *)A1, 0, need); RET(1);
}
SHIM(UpdateProcThreadAttribute) { RET(1); }
SHIM(DeleteProcThreadAttributeList) { RET(0); }
SHIM(GetCurrentPackageFullName) { RET(15700); }   // APPMODEL_ERROR_NO_PACKAGE: not a packaged (Store) app
SHIM(GetCurrentPackageId) { RET(15700); }
SHIM(GetCurrentPackageFamilyName) { RET(15700); }
SHIM(GetCurrentPackagePath) { RET(15700); }
SHIM(GetCurrentApplicationUserModelId) { RET(15703); }   // APPMODEL_ERROR_NO_APPLICATION
SHIM(FindFirstChangeNotificationW) { RET(call4(c, "CreateEventW", 0, 1, 0, 0)); }   // a handle that is never signalled
SHIM(FindFirstChangeNotificationA) { RET(call4(c, "CreateEventW", 0, 1, 0, 0)); }
SHIM(FindNextChangeNotification) { RET(1); }
SHIM(FindCloseChangeNotification) { rt_handle_close(A1); RET(1); }
SHIM(ReadDirectoryChangesW) { WX_FAIL(ReadDirectoryChangesW, ERR_NOT_SUPPORTED, 0); }
SHIM(EnumResourceLanguagesA) { WX_FAIL(EnumResourceLanguagesA, 1813, 0); }   // ERROR_RESOURCE_TYPE_NOT_FOUND
SHIM(EnumResourceLanguagesW) { WX_FAIL(EnumResourceLanguagesW, 1813, 0); }
SHIM(EnumResourceLanguagesExW) { WX_FAIL(EnumResourceLanguagesExW, 1813, 0); }
SHIM(SetConsoleTextAttribute) { RET(1); }
SHIM(SetConsoleMode) { RET(1); }
SHIM(SetConsoleCP) { RET(1); }
SHIM(GetConsoleWindow) { RET(0); }
SHIM(GetConsoleProcessList) { if ((u32)A2 >= 1) ST32(A1, (u32)getpid()); RET(1); }
SHIM(FlushConsoleInputBuffer) { RET(1); }
SHIM(SetConsoleCursorPosition) { RET(1); }
SHIM(GetConsoleCursorInfo) { if (A2) { ST32(A2, 25); ST32(A2 + 4, 1); } RET(1); }
SHIM(SetConsoleCursorInfo) { RET(1); }
SHIM(FillConsoleOutputCharacterW) { if (AN(5)) ST32(AN(5), (u32)A3); RET(1); }
SHIM(FillConsoleOutputAttribute) { if (AN(5)) ST32(AN(5), (u32)A3); RET(1); }
SHIM(RtlNtStatusToDosError) {
    u32 s = (u32)A1;
    switch (s) {
    case 0: RET(0); case 0x103: RET(997); case 0x80000005u: RET(234); case 0xC0000008u: RET(6); case 0xC000000Du: RET(87);
    case 0xC0000011u: RET(38); case 0xC0000017u: RET(8); case 0xC0000022u: RET(5); case 0xC0000023u: RET(122); case 0xC0000034u: RET(2);
    case 0xC0000035u: RET(183); case 0xC000003Au: RET(3); case 0xC0000043u: RET(32); case 0xC00000BBu: RET(50); case 0xC0000225u: RET(1168);
    default: RET(317);   // ERROR_MR_MID_NOT_FOUND
    }
}
SHIM(RtlGetLastWin32Error) { RET(call4(c, "GetLastError", 0, 0, 0, 0)); }
SHIM(RtlSetLastWin32Error) { rt_set_last_error((u32)A1); RET(0); }

// ==== I/O completion ports (as thread-safe packet queues; file I/O completes synchronously and posts no packets) ======
typedef struct Pkt { u32 bytes; u64 key, ov; struct Pkt *next; } Pkt;
typedef struct { pthread_mutex_t m; pthread_cond_t cv; Pkt *head, *tail; } Port;
SHIM(CreateIoCompletionPort) {  // (file, existing, key, threads)
    if (A1 != ~0ull && A1) WX_FAIL(CreateIoCompletionPort(associate file), ERR_NOT_SUPPORTED, 0);
    Port *p = calloc(1, sizeof *p); pthread_mutex_init(&p->m, NULL); pthread_cond_init(&p->cv, NULL);
    RET(rt_handle_new(H_IOCP, (intptr_t)p));
}
SHIM(PostQueuedCompletionStatus) {  // (port, bytes, key, ov)
    intptr_t h; if (!rt_handle_get(A1, H_IOCP, &h)) { rt_set_last_error(ERR_HANDLE); RET(0); }
    Port *p = (Port *)h; Pkt *k = calloc(1, sizeof *k); k->bytes = (u32)A2; k->key = A3; k->ov = A4;
    pthread_mutex_lock(&p->m); if (p->tail) p->tail->next = k; else p->head = k; p->tail = k; pthread_cond_signal(&p->cv); pthread_mutex_unlock(&p->m);
    RET(1);
}
static Pkt *port_get(Port *p, u32 ms) {
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&p->m);
    while (!p->head) {
        if (!ms) break;
        if (ms == 0xFFFFFFFF) pthread_cond_wait(&p->cv, &p->m);
        else if (pthread_cond_timedwait(&p->cv, &p->m, &dl)) break;
    }
    Pkt *k = p->head; if (k) { p->head = k->next; if (!p->head) p->tail = NULL; }
    pthread_mutex_unlock(&p->m);
    return k;
}
SHIM(GetQueuedCompletionStatus) {  // (port, *bytes, *key, **ov, ms)
    intptr_t h; if (!rt_handle_get(A1, H_IOCP, &h)) { rt_set_last_error(ERR_HANDLE); RET(0); }
    Pkt *k = port_get((Port *)h, (u32)AN(5));
    if (!k) { ST64(A4, 0); rt_set_last_error(258); RET(0); }   // WAIT_TIMEOUT
    ST32(A2, k->bytes); ST64(A3, k->key); ST64(A4, k->ov); free(k); RET(1);
}
SHIM(GetQueuedCompletionStatusEx) {  // (port, OVERLAPPED_ENTRY*, count, *removed, ms, alertable): entry { key, ov, internal, bytes } 32 bytes
    intptr_t h; if (!rt_handle_get(A1, H_IOCP, &h)) { rt_set_last_error(ERR_HANDLE); RET(0); }
    u32 n = 0, max = (u32)A3; Pkt *k = port_get((Port *)h, (u32)AN(5));
    while (k && n < max) {
        u64 e = A2 + 32ull * n++; ST64(e, k->key); ST64(e + 8, k->ov); ST64(e + 16, 0); ST32(e + 24, k->bytes); free(k);
        if (n < max) k = port_get((Port *)h, 0); else k = NULL;
    }
    ST32(A4, n);
    if (!n) { rt_set_last_error(258); RET(0); }
    RET(1);
}

// ==== thread-pool timers and waits (host threads calling back into the guest) ======================================
typedef struct { u64 cb, ctx; pthread_mutex_t m; pthread_cond_t cv; i64 due; u32 period; int armed, running, closed, gen; pthread_t th; int started; } TpTimer;
static i64 now100(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (i64)ft_from_ts(t); }
static void *timer_thread(void *arg) {
    TpTimer *t = arg; Ctx *c = rt_foreign_ctx();
    pthread_mutex_lock(&t->m);
    while (!t->closed) {
        if (!t->armed) { pthread_cond_wait(&t->cv, &t->m); continue; }
        i64 wait = t->due - now100();
        if (wait > 0) {
            struct timespec dl = ts_from_ft((u64)t->due);
            pthread_cond_timedwait(&t->cv, &t->m, &dl); continue;
        }
        int gen = t->gen;
        t->running = 1; pthread_mutex_unlock(&t->m);
        u64 args[3] = { 0, t->ctx, (u64)t };
        rt_guest_call(c, t->cb, 3, args);
        pthread_mutex_lock(&t->m); t->running = 0; pthread_cond_broadcast(&t->cv);
        if (gen == t->gen) { if (t->period) t->due += (i64)t->period * 10000; else t->armed = 0; }
    }
    pthread_mutex_unlock(&t->m);
    return NULL;
}
SHIM(CreateThreadpoolTimer) {  // (cb, ctx, env)
    TpTimer *t = calloc(1, sizeof *t); t->cb = A1; t->ctx = A2; pthread_mutex_init(&t->m, NULL); pthread_cond_init(&t->cv, NULL);
    RET(t);
}
SHIM(SetThreadpoolTimer) {  // (timer, FILETIME *due (NULL = cancel; negative = relative), period ms, window)
    TpTimer *t = (TpTimer *)A1;
    pthread_mutex_lock(&t->m);
    t->gen++;
    if (!A2) t->armed = 0;
    else {
        i64 d = (i64)LD64(A2);
        t->due = d < 0 ? now100() - d : d == 0 ? now100() : d;
        t->period = (u32)A3; t->armed = 1;
        if (!t->started) { t->started = 1; pthread_create(&t->th, NULL, timer_thread, t); pthread_detach(t->th); }
    }
    pthread_cond_broadcast(&t->cv); pthread_mutex_unlock(&t->m);
    RET(0);
}
SHIM(SetThreadpoolTimerEx) { TpTimer *t = (TpTimer *)A1; pthread_mutex_lock(&t->m); int was = t->armed; pthread_mutex_unlock(&t->m); sh_SetThreadpoolTimer(c); RET(was); }
SHIM(IsThreadpoolTimerSet) { TpTimer *t = (TpTimer *)A1; RET(t->armed); }
SHIM(WaitForThreadpoolTimerCallbacks) {  // (timer, cancelPending)
    TpTimer *t = (TpTimer *)A1; pthread_mutex_lock(&t->m);
    if ((u32)A2) { t->armed = 0; t->gen++; }
    while (t->running) pthread_cond_wait(&t->cv, &t->m);
    pthread_mutex_unlock(&t->m); RET(0);
}
SHIM(CloseThreadpoolTimer) { TpTimer *t = (TpTimer *)A1; pthread_mutex_lock(&t->m); t->closed = 1; t->armed = 0; pthread_cond_broadcast(&t->cv); pthread_mutex_unlock(&t->m); RET(0); }

typedef struct { u64 cb, ctx, handle; i64 timeout; int armed, closed, running, gen, started; pthread_mutex_t m; pthread_cond_t cv; } TpWait;
static void *wait_thread(void *arg) {
    TpWait *w = arg; Ctx *c = rt_foreign_ctx();
    ShimFn wfso = shim_fn("WaitForSingleObject");
    pthread_mutex_lock(&w->m);
    while (!w->closed) {
        if (!w->armed) { pthread_cond_wait(&w->cv, &w->m); continue; }
        int gen = w->gen; u64 h = w->handle; i64 to = w->timeout;
        pthread_mutex_unlock(&w->m);
        u32 res = 0x102;   // poll in slices so re-arming and closing are noticed
        for (;;) {
            u32 slice = 50;
            if (to >= 0) { i64 left = (to - now100()) / 10000; if (left <= 0) break; if (left < slice) slice = (u32)left; }
            c->r[RCX] = h; c->r[RDX] = slice; wfso(c); res = (u32)c->r[RAX];
            if (res != 0x102) break;
            pthread_mutex_lock(&w->m); int stale = gen != w->gen || w->closed; pthread_mutex_unlock(&w->m);
            if (stale) break;
        }
        pthread_mutex_lock(&w->m);
        if (gen != w->gen || w->closed) continue;
        w->armed = 0; w->running = 1; pthread_mutex_unlock(&w->m);
        u64 args[4] = { 0, w->ctx, (u64)w, res };
        rt_guest_call(c, w->cb, 4, args);
        pthread_mutex_lock(&w->m); w->running = 0; pthread_cond_broadcast(&w->cv);
    }
    pthread_mutex_unlock(&w->m);
    return NULL;
}
SHIM(CreateThreadpoolWait) { TpWait *w = calloc(1, sizeof *w); w->cb = A1; w->ctx = A2; pthread_mutex_init(&w->m, NULL); pthread_cond_init(&w->cv, NULL); RET(w); }
SHIM(SetThreadpoolWait) {  // (wait, handle (NULL = stop), FILETIME *timeout (NULL = infinite))
    TpWait *w = (TpWait *)A1; pthread_mutex_lock(&w->m); w->gen++;
    if (!A2) w->armed = 0;
    else {
        w->handle = A2; i64 d = A3 ? (i64)LD64(A3) : -1;
        w->timeout = !A3 ? -1 : d < 0 ? now100() - d : d; w->armed = 1;
        if (!w->started) { pthread_t th; w->started = 1; pthread_create(&th, NULL, wait_thread, w); pthread_detach(th); }
    }
    pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->m); RET(0);
}
SHIM(WaitForThreadpoolWaitCallbacks) { TpWait *w = (TpWait *)A1; pthread_mutex_lock(&w->m); if ((u32)A2) { w->armed = 0; w->gen++; } while (w->running) pthread_cond_wait(&w->cv, &w->m); pthread_mutex_unlock(&w->m); RET(0); }
SHIM(CloseThreadpoolWait) { TpWait *w = (TpWait *)A1; pthread_mutex_lock(&w->m); w->closed = 1; w->armed = 0; pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->m); RET(0); }
SHIM(FreeLibraryWhenCallbackReturns) { RET(0); }

// Thread-pool work items: each submission runs the callback on a fresh host thread.
typedef struct { u64 cb, ctx; int pending; pthread_mutex_t m; pthread_cond_t cv; } TpWork;
typedef struct { TpWork *w; u64 cb, ctx; int simple; } TpRun;
static void *work_thread(void *arg) {
    TpRun *r = arg; Ctx *c = rt_foreign_ctx();
    if (r->simple) { u64 a[2] = { 0, r->ctx }; rt_guest_call(c, r->cb, 2, a); }
    else {
        u64 a[3] = { 0, r->w->ctx, (u64)r->w }; rt_guest_call(c, r->w->cb, 3, a);
        pthread_mutex_lock(&r->w->m); r->w->pending--; pthread_cond_broadcast(&r->w->cv); pthread_mutex_unlock(&r->w->m);
    }
    free(r); return NULL;
}
SHIM(CreateThreadpoolWork) { TpWork *w = calloc(1, sizeof *w); w->cb = A1; w->ctx = A2; pthread_mutex_init(&w->m, NULL); pthread_cond_init(&w->cv, NULL); RET(w); }
SHIM(SubmitThreadpoolWork) {
    TpWork *w = (TpWork *)A1; TpRun *r = calloc(1, sizeof *r); r->w = w;
    pthread_mutex_lock(&w->m); w->pending++; pthread_mutex_unlock(&w->m);
    pthread_t th; pthread_create(&th, NULL, work_thread, r); pthread_detach(th); RET(0);
}
SHIM(WaitForThreadpoolWorkCallbacks) { TpWork *w = (TpWork *)A1; pthread_mutex_lock(&w->m); while (w->pending) pthread_cond_wait(&w->cv, &w->m); pthread_mutex_unlock(&w->m); RET(0); }
SHIM(CloseThreadpoolWork) { RET(0); }   // freed lazily: a still-running callback may hold it
SHIM(TrySubmitThreadpoolCallback) { TpRun *r = calloc(1, sizeof *r); r->cb = A1; r->ctx = A2; r->simple = 1; pthread_t th; pthread_create(&th, NULL, work_thread, r); pthread_detach(th); RET(1); }
SHIM(CallbackMayRunLong) { RET(1); }
SHIM(SetEventWhenCallbackReturns) { call4(c, "SetEvent", A2, 0, 0, 0); RET(0); }
SHIM(ReleaseMutexWhenCallbackReturns) { call4(c, "ReleaseMutex", A2, 0, 0, 0); RET(0); }

const Shim shims_kernel32x[] = {
#define E(n) { NULL, #n, sh_##n },
    E(GetFullPathNameW) E(GetFullPathNameA) E(SetCurrentDirectoryW) E(SetCurrentDirectoryA) E(GetLongPathNameW) E(GetLongPathNameA)
    E(GetShortPathNameW) E(GetShortPathNameA) E(GetTempPathA) E(GetTempPath2W) E(GetSystemDirectoryW) E(GetSystemDirectoryA)
    E(GetWindowsDirectoryW) E(GetWindowsDirectoryA) E(GetSystemWindowsDirectoryW) E(GetSystemWow64DirectoryW) E(GetTempFileNameW)
    E(GetTempFileNameA) E(GetDriveTypeW) E(GetDriveTypeA) E(GetDiskFreeSpaceExW) E(GetDiskFreeSpaceExA) E(GetDiskFreeSpaceW)
    E(GetDiskFreeSpaceA) E(GetVolumeInformationW) E(GetVolumeInformationA) E(GetVolumePathNameW) E(GetVolumeNameForVolumeMountPointW)
    E(GetVolumePathNamesForVolumeNameW) E(SetFileAttributesW) E(SetFileAttributesA) E(GetFileAttributesExA) E(MoveFileExA) E(MoveFileW)
    E(MoveFileA) E(MoveFileWithProgressW) E(ReplaceFileW) E(ReplaceFileA) E(CopyFileExA) E(CreateDirectoryExW) E(CreateDirectoryExA)
    E(CreateSymbolicLinkW) E(CreateHardLinkW) E(AreFileApisANSI) E(SetFileApisToANSI) E(SetFileApisToOEM) E(CompareFileTime)
    E(SetFileTime) E(LockFile) E(UnlockFile) E(LockFileEx) E(UnlockFileEx) E(PeekNamedPipe) E(CreateNamedPipeW) E(CreateNamedPipeA)
    E(ConnectNamedPipe) E(DisconnectNamedPipe) E(WaitNamedPipeW) E(WaitNamedPipeA) E(GetFileInformationByHandleEx)
    E(SetFileInformationByHandle) E(GetFinalPathNameByHandleW) E(GetFinalPathNameByHandleA) E(CreateFileMappingW)
    E(CreateFileMappingA) E(OpenFileMappingW) E(OpenFileMappingA) E(MapViewOfFile) E(MapViewOfFileEx) E(UnmapViewOfFile)
    E(FlushViewOfFile) E(GetDynamicTimeZoneInformation) E(GetSystemTimeAdjustment) E(SetSystemTime) E(SetLocalTime)
    E(TzSpecificLocalTimeToSystemTime) E(LocaleNameToLCID) E(LCIDToLocaleName) E(IsValidLocale) E(IsValidLocaleName)
    E(ResolveLocaleName) E(EnumSystemLocalesW) E(EnumSystemLocalesA) E(GetUserGeoID) E(GetGeoInfoW) E(GetUserDefaultGeoName)
    E(GetDateFormatW) E(GetDateFormatA) E(GetDateFormatEx) E(GetTimeFormatW) E(GetTimeFormatA) E(GetTimeFormatEx) E(GetNumberFormatW)
    E(GetNumberFormatA) E(GetNumberFormatEx) E(GetCurrencyFormatW) E(GetCurrencyFormatEx) E(CompareStringOrdinal) E(CompareStringEx)
    E(LCMapStringEx) E(LCMapStringA) E(GetStringTypeExA) E(GetStringTypeA) E(lstrlenW) E(lstrlenA) E(lstrcmpW) E(lstrcmpiW) E(lstrcmpA)
    E(lstrcmpiA) E(lstrcpyW) E(lstrcpyA) E(lstrcpynW) E(lstrcpynA) E(lstrcatW) E(lstrcatA) E(ExpandEnvironmentStringsW)
    E(ExpandEnvironmentStringsA) E(GetComputerNameW) E(GetComputerNameA) E(GetComputerNameExW) E(GetComputerNameExA)
    E(GetProcessId) E(GetThreadId) E(GetProcessIdOfThread) E(GetThreadTimes) E(GetPriorityClass) E(SetPriorityClass)
    E(SetThreadIdealProcessor) E(SetThreadIdealProcessorEx) E(GetThreadGroupAffinity) E(SetThreadGroupAffinity) E(TerminateThread)
    E(SleepEx) E(WaitForMultipleObjectsEx) E(SignalObjectAndWait) E(OpenEventW) E(OpenEventA) E(OpenMutexW) E(OpenMutexA)
    E(OpenSemaphoreW) E(OpenSemaphoreA) E(QueueUserAPC) E(FlushProcessWriteBuffers) E(InitOnceInitialize) E(InitOnceExecuteOnce)
    E(InitOnceBeginInitialize) E(InitOnceComplete) E(HeapCreate) E(HeapDestroy) E(HeapCompact) E(HeapValidate) E(HeapSetInformation)
    E(HeapQueryInformation) E(HeapLock) E(HeapUnlock) E(VirtualAllocEx) E(VirtualFreeEx) E(VirtualQueryEx) E(VirtualProtectEx)
    E(GetLogicalProcessorInformation) E(GetLogicalProcessorInformationEx) E(K32EnumProcessModules) E(K32EnumProcessModulesEx)
    E(K32GetModuleBaseNameW) E(K32GetModuleBaseNameA) E(K32GetModuleFileNameExW) E(K32GetModuleFileNameExA) E(K32GetModuleInformation)
    E(K32GetPerformanceInfo) E(QueryFullProcessImageNameW) E(QueryFullProcessImageNameA) E(Module32FirstW) E(Module32NextW)
    E(Module32First) E(Module32Next) E(Thread32First) E(Thread32Next) E(AddDllDirectory) E(RemoveDllDirectory) E(SetDllDirectoryW)
    E(SetDllDirectoryA) E(SetDefaultDllDirectories) E(SetSearchPathMode) E(SetProcessDEPPolicy) E(GetProcessDEPPolicy) E(SearchPathW)
    E(SearchPathA) E(GetBinaryTypeA) E(GetBinaryTypeW) E(WTSGetActiveConsoleSessionId) E(ProcessIdToSessionId) E(PowerCreateRequest)
    E(PowerSetRequest) E(PowerClearRequest) E(CreateJobObjectW) E(CreateJobObjectA) E(AssignProcessToJobObject) E(SetInformationJobObject)
    E(IsProcessInJob) E(InitializeProcThreadAttributeList) E(UpdateProcThreadAttribute) E(DeleteProcThreadAttributeList)
    E(GetCurrentPackageFullName) E(GetCurrentPackageId) E(GetCurrentPackageFamilyName) E(GetCurrentPackagePath)
    E(GetCurrentApplicationUserModelId) E(FindFirstChangeNotificationW) E(FindFirstChangeNotificationA) E(FindNextChangeNotification)
    E(FindCloseChangeNotification) E(ReadDirectoryChangesW) E(EnumResourceLanguagesA) E(EnumResourceLanguagesW) E(EnumResourceLanguagesExW)
    E(SetConsoleTextAttribute) E(SetConsoleMode) E(SetConsoleCP) E(GetConsoleWindow) E(GetConsoleProcessList) E(FlushConsoleInputBuffer)
    E(SetConsoleCursorPosition) E(GetConsoleCursorInfo) E(SetConsoleCursorInfo) E(FillConsoleOutputCharacterW) E(FillConsoleOutputAttribute)
    E(RtlNtStatusToDosError) E(RtlGetLastWin32Error) E(RtlSetLastWin32Error)
    E(CreateIoCompletionPort) E(PostQueuedCompletionStatus) E(GetQueuedCompletionStatus) E(GetQueuedCompletionStatusEx)
    E(CreateThreadpoolTimer) E(SetThreadpoolTimer) E(SetThreadpoolTimerEx) E(IsThreadpoolTimerSet) E(WaitForThreadpoolTimerCallbacks)
    E(CloseThreadpoolTimer) E(CreateThreadpoolWait) E(SetThreadpoolWait) E(WaitForThreadpoolWaitCallbacks) E(CloseThreadpoolWait)
    E(FreeLibraryWhenCallbackReturns) E(CreateThreadpoolWork) E(SubmitThreadpoolWork) E(WaitForThreadpoolWorkCallbacks) E(CloseThreadpoolWork)
    E(TrySubmitThreadpoolCallback) E(CallbackMayRunLong) E(SetEventWhenCallbackReturns) E(ReleaseMutexWhenCallbackReturns)
    { NULL, "GetModuleInformation", sh_K32GetModuleInformation }, { NULL, "EnumProcessModules", sh_K32EnumProcessModules },
    { NULL, "GetModuleBaseNameW", sh_K32GetModuleBaseNameW }, { NULL, "GetModuleBaseNameA", sh_K32GetModuleBaseNameA },
    { NULL, "GetModuleFileNameExW", sh_K32GetModuleFileNameExW }, { NULL, "GetModuleFileNameExA", sh_K32GetModuleFileNameExA },
    { NULL, "GetPerformanceInfo", sh_K32GetPerformanceInfo },
#undef E
    { 0, 0, 0 }
};
