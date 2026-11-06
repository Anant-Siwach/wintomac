// Remaining Win32 surface games' runtimes (SDL3, CRTs, engines) import: console, process/system info, locale,
// interlocked SLists, memory status, shell known folders, and the small DLLs (gdi32, imm32, setupapi, dbghelp,
// version, winmm, advapi32 security, ole32 drag & drop) where "not available" is the right answer on macOS.
#include <poll.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <malloc/malloc.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "com.h"

extern char **environ;

// ---- console ---------------------------------------------------------------------------------
SHIM(AllocConsole) { RET(1); }
SHIM(AttachConsole) { rt_set_last_error(6); RET(0); }
SHIM(FreeConsole) { RET(1); }
SHIM(GetConsoleOutputCP) { RET(65001); }
SHIM(GetConsoleCP) { RET(65001); }
SHIM(SetConsoleOutputCP) { RET(1); }
SHIM(GetConsoleScreenBufferInfo) { rt_set_last_error(6); RET(0); }   // not a console window
// Console input from the terminal's stdin: each byte becomes a KEY_EVENT (key down) record, '\n' as '\r' like
// a Windows console. Used by dedicated servers and console tools.
static u32 stdin_avail(void) {
    int n = 0;
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return 0;
    if (ioctl(0, FIONREAD, &n) < 0) n = 0;
    return n > 0 ? (u32)n : 0;
}
// Each typed character becomes a key-down and a key-up record, like a real console (some programs, e.g.
// QuakeSpasm's dedicated server, act on key-up only). Records are queued so callers can read one at a time.
static u8 conq[512]; static u32 conq_head, conq_len;   // queued characters, each = 2 records (down, up)
static u32 con_half;                                   // 1 = the head character's key-down was already delivered
static void con_fill(int block) {
    u32 n = stdin_avail(); if (!n && !block) return; if (!n) n = 1;
    if (n > sizeof conq - conq_len) n = sizeof conq - conq_len;
    u8 tmp[512]; ssize_t got = read(0, tmp, n);
    for (ssize_t i = 0; i < got; i++) conq[(conq_head + conq_len++) % sizeof conq] = tmp[i] == '\n' ? '\r' : tmp[i];
}
static u32 con_pending(void) { con_fill(0); return conq_len * 2 - con_half; }
SHIM(GetNumberOfConsoleInputEvents) { if (A2) ST32(A2, con_pending()); RET(1); }
static u32 read_console_input(u64 buf, u32 len) {  // INPUT_RECORD: 20 bytes, KEY_EVENT_RECORD at +4
    if (!con_pending()) con_fill(1);                // nothing pending: block for one key, as Windows would
    u32 n = 0;
    while (n < len && conq_len) {
        u8 ch = conq[conq_head]; u64 r = buf + 20ull * n++;
        memset((void *)r, 0, 20);
        ST16(r, 1); ST32(r + 4, con_half ? 0 : 1); ST16(r + 8, 1);   // KEY_EVENT, bKeyDown, wRepeatCount
        ST16(r + 10, ch == '\r' ? 0x0D : ch);         // wVirtualKeyCode (approximate)
        ST16(r + 14, ch);                               // uChar
        if (con_half) { con_half = 0; conq_head = (conq_head + 1) % sizeof conq; conq_len--; } else con_half = 1;
    }
    return n;
}
SHIM(ReadConsoleInputA) { u32 n = A3 ? read_console_input(A2, (u32)A3) : 0; if (A4) ST32(A4, n); RET(1); }
SHIM(ReadConsoleInputW) { sh_ReadConsoleInputA(c); }
SHIM(ReadConsoleW) { RET(0); }
SHIM(GenerateConsoleCtrlEvent) { RET(1); }
SHIM(SetConsoleTitleW) { RET(1); }
SHIM(SetConsoleTitleA) { RET(1); }
static u64 write_console(Ctx *c, int wide) {  // (h, buf, n, *written, reserved)
    int fd = 1; u32 n = (u32)A3;
    if (wide) { for (u32 i = 0; i < n; i++) { u16 ch = LD16(A2 + 2 * i); char b = ch < 128 ? (char)ch : '?'; if (write(fd, &b, 1) < 0) break; } }
    else if (write(fd, (void *)A2, n) < 0) n = 0;
    if (A4) ST32(A4, n);
    return 1;
}
SHIM(WriteConsoleW) { RET(write_console(c, 1)); }
SHIM(WriteConsoleA) { RET(write_console(c, 0)); }
SHIM(SetStdHandle) { RET(1); }

// ---- process / system -------------------------------------------------------------------------
SHIM(CheckRemoteDebuggerPresent) { if (A2) ST32(A2, 0); RET(1); }
SHIM(DebugBreak) { rt_log("guest DebugBreak() ignored"); RET(0); }
SHIM(IsWow64Process) { if (A2) ST32(A2, 0); RET(1); }
SHIM(IsWow64Process2) { if (A2) ST16(A2, 0); if (A3) ST16(A3, 0x8664); RET(1); }
SHIM(GetCurrentProcessorNumber) { RET(0); }
SHIM(OpenThread) { RET(0x7001); }   // pseudo handle; thread APIs that take it are no-ops
SHIM(CreateProcessW) { rt_log("CreateProcess: launching Windows programs is not supported"); rt_set_last_error(50); RET(0); }
SHIM(CreateProcessA) { sh_CreateProcessW(c); }
SHIM(GetExitCodeProcess) { if (A2) ST32(A2, 0); RET(0); }
SHIM(TerminateProcess) { if (A1 == (u64)-1) { fflush(NULL); _exit((int)(u32)A2); } RET(0); }
SHIM(CreateToolhelp32Snapshot) { rt_set_last_error(50); RET((u64)-1); }
SHIM(Process32FirstW) { RET(0); }
SHIM(Process32NextW) { RET(0); }
SHIM(EnumResourceNamesW) { RET(0); }
SHIM(SetThreadExecutionState) { RET(0x80000000); }   // ES_CONTINUOUS
SHIM(SetHandleInformation) { RET(1); }
SHIM(SetNamedPipeHandleState) { RET(1); }
SHIM(FreeLibraryAndExitThread) { extern void rt_exit_thread(u32); rt_exit_thread((u32)A2); RET(0); }
SHIM(GetStartupInfoW) { memset((void *)A1, 0, 104); ST32(A1, 104); RET(0); }
SHIM(GetStartupInfoA) { memset((void *)A1, 0, 104); ST32(A1, 104); RET(0); }
SHIM(GetProcessTimes) {  // (h, *creation, *exit, *kernel, *user): 100 ns units
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    if (A2) ST64(A2, 0); if (A3) ST64(A3, 0);
    if (A4) ST64(A4, (u64)ru.ru_stime.tv_sec * 10000000ull + (u64)ru.ru_stime.tv_usec * 10);
    if (AN(5)) ST64(AN(5), (u64)ru.ru_utime.tv_sec * 10000000ull + (u64)ru.ru_utime.tv_usec * 10);
    RET(1);
}
SHIM(GetSystemPowerStatus) {  // SYSTEM_POWER_STATUS { AC, BatteryFlag, Percent, SystemStatusFlag, LifeTime, FullLifeTime }
    ST8(A1, 1); ST8(A1 + 1, 128); ST8(A1 + 2, 255); ST8(A1 + 3, 0); ST32(A1 + 4, 0xFFFFFFFF); ST32(A1 + 8, 0xFFFFFFFF); RET(1);
}
SHIM(GlobalMemoryStatusEx) {  // MEMORYSTATUSEX
    u64 total = 0; size_t sz = sizeof total; sysctlbyname("hw.memsize", &total, &sz, NULL, 0);
    u64 p = A1; ST32(p + 4, 30);
    ST64(p + 8, total); ST64(p + 16, total / 2); ST64(p + 24, total * 2); ST64(p + 32, total);
    ST64(p + 40, 0x7FFFFFFE0000ull); ST64(p + 48, 0x7FFF00000000ull); ST64(p + 56, 0);
    RET(1);
}
SHIM(HeapSize) { RET(A3 ? malloc_size((void *)A3) : (u64)-1); }   // (heap, flags, ptr)
SHIM(GlobalLock) { RET(A1); }      // our GlobalAlloc returns plain pointers
SHIM(GlobalUnlock) { RET(1); }
SHIM(GlobalSize) { RET(A1 ? malloc_size((void *)A1) : 0); }
SHIM(MulDiv) {
    i64 a = (i32)(u32)A1, b = (i32)(u32)A2, d = (i32)(u32)A3;
    if (!d) RET((u32)-1);
    i64 p = a * b; i64 r = (p + (((p < 0) ^ (d < 0)) ? -d / 2 : d / 2)) / d;   // rounded like Windows
    RET((u32)(i32)r);
}
SHIM(VerSetConditionMask) { u64 m = A1; u32 type = (u32)A2; u8 cond = (u8)A3;
    for (int i = 0; i < 8; i++) if (type & (1u << i)) m |= (u64)(cond & 7) << (i * 3);
    RET(m); }
SHIM(VerifyVersionInfoW) { RET(1); }   // we present as a current Windows 10/11
SHIM(VerifyVersionInfoA) { RET(1); }
SHIM(GetEnvironmentStringsW) {
    size_t n = 1; for (char **e = environ; *e; e++) n += strlen(*e) + 1;
    u16 *w = calloc(n, 2), *p = w;
    for (char **e = environ; *e; e++) { for (const char *s = *e; *s; s++) *p++ = (u8)*s; *p++ = 0; }
    *p = 0; RET(w);
}
SHIM(FreeEnvironmentStringsW) { free((void *)A1); RET(1); }

// ---- interlocked singly linked lists (SLIST_HEADER: 16 bytes; x64 "header16" encoding) ------------------
// Low qword: Depth(16) | Sequence(48); high qword: NextEntry >> 4 in bits 4..63. Updated with 128-bit CAS.
static u64 sl_next(u128 h) { return (u64)(h >> 64) & ~0xFull; }
static u128 sl_make(u64 next, u64 depth, u64 seq) { return ((u128)next << 64) | (u128)(((seq & 0xFFFFFFFFFFFFull) << 16) | (depth & 0xFFFF)); }
SHIM(InterlockedPushEntrySList) {
    u128 *h = (u128 *)A1, old = __atomic_load_n(h, __ATOMIC_SEQ_CST), nw;
    do { ST64(A2, sl_next(old)); u64 lo = (u64)old; nw = sl_make(A2, (lo & 0xFFFF) + 1, (lo >> 16) + 1); }
    while (!__atomic_compare_exchange_n(h, &old, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    RET(sl_next(old));
}
SHIM(InterlockedPopEntrySList) {
    u128 *h = (u128 *)A1, old = __atomic_load_n(h, __ATOMIC_SEQ_CST), nw; u64 first;
    do { first = sl_next(old); if (!first) RET(0); u64 lo = (u64)old; nw = sl_make(LD64(first), (lo & 0xFFFF) - 1, (lo >> 16) + 1); }
    while (!__atomic_compare_exchange_n(h, &old, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    RET(first);
}
SHIM(InterlockedFlushSList) {
    u128 *h = (u128 *)A1, old = __atomic_load_n(h, __ATOMIC_SEQ_CST), nw;
    do { nw = sl_make(0, 0, ((u64)old >> 16) + 1); } while (!__atomic_compare_exchange_n(h, &old, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    RET(sl_next(old));
}
SHIM(QueryDepthSList) { RET(LD64(A1) & 0xFFFF); }

// ---- locale (en-US, UTF-8) ---------------------------------------------------------------------------
static u32 put_str(u64 dst, u32 cap, const char *v, int wide) {   // returns chars incl. NUL (or needed size when cap == 0)
    u32 n = (u32)strlen(v) + 1;
    if (!cap) return n;
    if (cap < n) { rt_set_last_error(122); return 0; }
    for (u32 i = 0; i < n; i++) { if (wide) ST16(dst + 2 * i, (u8)v[i]); else ST8(dst + i, (u8)v[i]); }
    return n;
}
static const char *locale_value(u32 type) {
    switch (type & 0xFFFF) {
    case 0x0E: return ".";        case 0x0F: return ",";        case 0x10: return "3;0";   // decimal, thousand, grouping
    case 0x14: return "$";        case 0x59: return "en";       case 0x5A: return "US";    // currency, ISO639, ISO3166
    case 0x1004: return "65001";  case 0x0B: return "437";      case 0x1F: return "M/d/yyyy";  // ANSI/OEM codepage, short date
    case 0x02: return "English (United States)"; case 0x03: return "ENU"; case 0x5C: return "en-US";
    case 0x01: return "0409";     case 0x0C: return "1";        case 0x0D: return "0";     // LCID, list sep count...
    default: return "";
    }
}
SHIM(GetLocaleInfoW) { RET(put_str(A3, (u32)A4, locale_value((u32)A2), 1)); }   // (lcid, type, buf, n)
SHIM(GetLocaleInfoA) { RET(put_str(A3, (u32)A4, locale_value((u32)A2), 0)); }
SHIM(GetLocaleInfoEx) { RET(put_str(A3, (u32)A4, locale_value((u32)A2), 1)); }   // (name, type, buf, n)
SHIM(GetUserDefaultLCID) { RET(0x0409); }
SHIM(GetSystemDefaultLCID) { RET(0x0409); }
SHIM(GetUserDefaultLangID) { RET(0x0409); }
SHIM(GetUserDefaultUILanguage) { RET(0x0409); }
SHIM(GetSystemDefaultLangID) { RET(0x0409); }
SHIM(GetThreadLocale) { RET(0x0409); }
SHIM(GetUserDefaultLocaleName) { RET(put_str(A1, (u32)A2, "en-US", 1)); }
SHIM(IsValidCodePage) { u32 cp = (u32)A1; RET(cp == 65001 || cp == 1252 || cp == 437 || cp == 20127 || cp == 28591); }
SHIM(CompareStringA) {  // (lcid, flags, s1, n1, s2, n2): CSTR_LESS_THAN 1, EQUAL 2, GREATER 3
    const char *a = (const char *)A3, *b = (const char *)AN(5);
    size_t na = (i32)A4 < 0 ? strlen(a) : (u32)A4, nb = (i32)AN(6) < 0 ? strlen(b) : (u32)AN(6);
    int ci = (u32)A2 & 1;
    for (size_t i = 0; i < na && i < nb; i++) {
        int x = (u8)a[i], y = (u8)b[i]; if (ci) { x = tolower(x); y = tolower(y); }
        if (x != y) RET(x < y ? 1 : 3);
    }
    RET(na == nb ? 2 : na < nb ? 1 : 3);
}
SHIM(CompareStringW) {
    u64 a = A3, b = AN(5); i64 na = (i32)A4, nb = (i32)AN(6);
    if (na < 0) { na = 0; while (LD16(a + 2 * na)) na++; } if (nb < 0) { nb = 0; while (LD16(b + 2 * nb)) nb++; }
    int ci = (u32)A2 & 1;
    for (i64 i = 0; i < na && i < nb; i++) {
        u32 x = LD16(a + 2 * i), y = LD16(b + 2 * i); if (ci) { if (x < 128) x = (u32)tolower((int)x); if (y < 128) y = (u32)tolower((int)y); }
        if (x != y) RET(x < y ? 1 : 3);
    }
    RET(na == nb ? 2 : na < nb ? 1 : 3);
}
SHIM(LCMapStringW) {  // (lcid, flags, src, n, dst, dn): LCMAP_LOWERCASE 0x100 / UPPERCASE 0x200
    u64 src = A3; i64 n = (i32)A4; u32 fl = (u32)A2;
    if (n < 0) { n = 0; while (LD16(src + 2 * n)) n++; n++; }
    if (!(i32)AN(6)) RET((u32)n);
    if ((i32)AN(6) < n) { rt_set_last_error(122); RET(0); }
    for (i64 i = 0; i < n; i++) {
        u32 ch = LD16(src + 2 * i);
        if (ch < 128) { if (fl & 0x100) ch = (u32)tolower((int)ch); if (fl & 0x200) ch = (u32)toupper((int)ch); }
        ST16(AN(5) + 2 * i, (u16)ch);
    }
    RET((u32)n);
}
SHIM(GetStringTypeW) {  // (CT_CTYPE1, src, n, WORD *out): C1_UPPER 1 LOWER 2 DIGIT 4 SPACE 8 PUNCT 16 CNTRL 32 BLANK 64 XDIGIT 128 ALPHA 256 DEFINED 512
    if ((u32)A1 != 1) RET(0);
    i64 n = (i32)A3; if (n < 0) { n = 0; while (LD16(A2 + 2 * n)) n++; n++; }
    for (i64 i = 0; i < n; i++) {
        u32 ch = LD16(A2 + 2 * i); u16 t = 0x200;
        if (ch < 128) {
            int x = (int)ch;
            t |= (isupper(x) ? 1 : 0) | (islower(x) ? 2 : 0) | (isdigit(x) ? 4 : 0) | (isspace(x) ? 8 : 0) | (ispunct(x) ? 16 : 0) |
                 (iscntrl(x) ? 32 : 0) | (x == ' ' || x == '\t' ? 64 : 0) | (isxdigit(x) ? 128 : 0) | (isalpha(x) ? 256 : 0);
        } else t |= 256;   // treat non-ASCII as letters
        ST16(A4 + 2 * i, t);
    }
    RET(1);
}
SHIM(FormatMessageW) {  // (flags, source, id, lang, buf, size, args): plain "error N" text
    char msg[64]; snprintf(msg, sizeof msg, "Windows error %u\r\n", (u32)A3);
    if ((u32)A1 & 0x100) {   // FORMAT_MESSAGE_ALLOCATE_BUFFER: *buf receives a LocalAlloc'd string
        size_t n = strlen(msg); u16 *w = calloc(n + 1, 2); for (size_t i = 0; i < n; i++) w[i] = (u8)msg[i];
        ST64(AN(5), (u64)w); RET(n);
    }
    u32 cap = (u32)AN(6); size_t n = strlen(msg); if (n >= cap) n = cap ? cap - 1 : 0;
    for (size_t i = 0; i < n; i++) ST16(AN(5) + 2 * i, (u8)msg[i]); if (cap) ST16(AN(5) + 2 * n, 0);
    RET(n);
}

// ---- pipes ---------------------------------------------------------------------------------------
SHIM(CreatePipe) {  // (*read, *write, sa, size)
    int fds[2]; if (pipe(fds)) { rt_set_last_error(8); RET(0); }
    ST64(A1, rt_handle_new(H_FILE, fds[0])); ST64(A2, rt_handle_new(H_FILE, fds[1])); RET(1);
}

// ---- shell32 ----------------------------------------------------------------------------------------
SHIM(CommandLineToArgvW) {  // (cmdline, *argc): splits on our real argv, which is what the command line came from
    size_t total = 8ull * (rt_argc + 1);
    for (int i = 0; i < rt_argc; i++) total += 2 * (strlen(rt_argv[i]) + 1);
    u8 *blk = calloc(1, total); u64 *ptrs = (u64 *)blk; u16 *chars = (u16 *)(blk + 8ull * (rt_argc + 1));
    for (int i = 0; i < rt_argc; i++) {
        ptrs[i] = (u64)chars;
        const char *s = i == 0 ? "game.exe" : rt_argv[i];
        while (*s) *chars++ = (u8)*s++;
        *chars++ = 0;
    }
    if (A2) ST32(A2, (u32)rt_argc);
    RET(blk);   // caller frees with LocalFree (one block, like Windows)
}
// Known folders live under C:\users\wintomac\... which the file layer maps to ~/Library/Application Support/wintomac
static const char *csidl_path(u32 csidl) {
    switch (csidl & 0xFF) {
    case 0x05: return "C:\\users\\wintomac\\Documents";
    case 0x1A: return "C:\\users\\wintomac\\AppData\\Roaming";
    case 0x1C: return "C:\\users\\wintomac\\AppData\\Local";
    case 0x23: return "C:\\users\\Public\\AppData";
    case 0x28: return "C:\\users\\wintomac";
    default: return "C:\\users\\wintomac\\Other";
    }
}
static void ensure_dir(const char *win) {   // create the folder (and parents) on the host side
    char host[2048]; rt_host_path(win, host, sizeof host);
    for (char *q = host + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(host, 0755); *q = '/'; }
    mkdir(host, 0755);
}
SHIM(SHGetFolderPathW) {  // (hwnd, csidl, token, flags, path[MAX_PATH])
    const char *p = csidl_path((u32)A2); ensure_dir(p); put_str(AN(5), 260, p, 1); RET(0);
}
SHIM(SHGetFolderPathA) { const char *p = csidl_path((u32)A2); ensure_dir(p); put_str(AN(5), 260, p, 0); RET(0); }
SHIM(SHGetKnownFolderPath) {  // (rfid, flags, token, PWSTR *out) -> CoTaskMem string
    static const struct { Guid g; const char *path; } kf[] = {
        { { 0x3EB685DB, 0x65F9, 0x4CF6, { 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D } }, "C:\\users\\wintomac\\AppData\\Roaming" },
        { { 0xF1B32785, 0x6FBA, 0x4FCF, { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 } }, "C:\\users\\wintomac\\AppData\\Local" },
        { { 0x4C5C32FF, 0xBB9D, 0x43B0, { 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4 } }, "C:\\users\\wintomac\\Saved Games" },
        { { 0xFDD39AD0, 0x238F, 0x46AF, { 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7 } }, "C:\\users\\wintomac\\Documents" },
        { { 0x5E6C858F, 0x0E22, 0x4760, { 0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73 } }, "C:\\users\\wintomac" },   // Profile
        { { 0xA520A1A4, 0x1780, 0x4FF6, { 0xBD, 0x18, 0x16, 0x73, 0x43, 0xC5, 0xAF, 0x16 } }, "C:\\users\\wintomac\\AppData\\LocalLow" },
    };
    const char *p = NULL;
    for (unsigned i = 0; i < sizeof kf / sizeof *kf; i++) if (rt_guid_eq(A1, &kf[i].g)) p = kf[i].path;
    if (!p) { ST64(A4, 0); RET(0x80070002u); }
    ensure_dir(p);
    size_t n = strlen(p); u16 *w = malloc(2 * (n + 1)); for (size_t i = 0; i <= n; i++) w[i] = (u8)p[i];
    ST64(A4, (u64)w); RET(0);
}
SHIM(SHGetPathFromIDListW) { RET(0); }
SHIM(SHBrowseForFolderW) { RET(0); }
SHIM(SHParseDisplayName) { RET(E_NOTIMPL_); }
SHIM(SHOpenFolderAndSelectItems) { RET(E_NOTIMPL_); }
SHIM(SHCreateDirectoryExW) { char w[2048]; size_t i = 0; for (; i + 1 < sizeof w && LD16(A2 + 2 * i); i++) w[i] = (char)LD16(A2 + 2 * i); w[i] = 0; ensure_dir(w); RET(0); }
SHIM(ShellExecuteW) {  // only http(s) URLs: opened in the Mac's default browser
    char u[2048]; size_t i = 0; for (; i + 1 < sizeof u && A3 && LD16(A3 + 2 * i); i++) u[i] = (char)LD16(A3 + 2 * i); u[i] = 0;
    if (!strncasecmp(u, "http://", 7) || !strncasecmp(u, "https://", 8)) {
        if (!fork()) { execlp("open", "open", u, (char *)NULL); _exit(1); }
        RET(42);
    }
    RET(2);   // SE_ERR_FNF
}
SHIM(Shell_NotifyIconW) { RET(1); }
SHIM(DragAcceptFiles) { RET(0); }
SHIM(DragFinish) { RET(0); }
SHIM(DragQueryFileW) { RET(0); }

// ---- gdi32 (window-surface drawing / pixel formats: unused by Vulkan/D3D games) ---------------------------------
static u64 gdi_obj = 0x48000;
SHIM(BitBlt) { RET(1); }
SHIM(StretchBlt) { RET(1); }
SHIM(CombineRgn) { RET(1); }
SHIM(CreateRectRgn) { RET(gdi_obj++); }
SHIM(CreateSolidBrush) { RET(gdi_obj++); }
SHIM(CreateFontIndirectW) { RET(gdi_obj++); }
SHIM(CreateFontW) { RET(gdi_obj++); }
SHIM(CreatePen) { RET(gdi_obj++); }
SHIM(CopyImage) { RET(0); }                       // icons/cursors for message boxes: callers fall back
SHIM(ExtTextOutW) { RET(1); }
SHIM(Rectangle) { RET(1); }
SHIM(SetBkMode) { RET(2); }                       // previous mode: OPAQUE
SHIM(SetTextColor) { RET(0); }
// Hardware gamma: report an identity ramp and accept changes without applying them (games then look as
// they do with gamma 1.0; QuakeSpasm prefers its GLSL gamma path anyway).
SHIM(GetDeviceGammaRamp) { u16 *r = (u16 *)A2; if (!r) RET(0); for (int k = 0; k < 3; k++) for (int i = 0; i < 256; i++) r[k * 256 + i] = (u16)(i * 257); RET(1); }
SHIM(SetDeviceGammaRamp) { RET(A2 != 0); }
// winmm wave audio: no devices (SDL and games then use DirectSound/XAudio2, which we provide).
SHIM(waveOutGetNumDevs) { RET(0); }
SHIM(waveInGetNumDevs) { RET(0); }
SHIM(waveOutGetErrorTextW) { if (A2 && (u32)A3) ST16(A2, 0); RET(0); }
SHIM(waveNoDriver) { RET(6); }                    // MMSYSERR_NODRIVER
SHIM(CreateDCW) { RET(gdi_obj++); }
SHIM(GetDIBits) { RET(0); }
SHIM(SetPixel) { RET(0); }
SHIM(GetICMProfileW) { RET(0); }
SHIM(GetTextMetricsW) { memset((void *)A2, 0, 60); ST32(A2, 16); ST32(A2 + 4, 13); ST32(A2 + 20, 7); RET(1); }   // height, ascent, avg width
SHIM(GetTextExtentPoint32A) { ST32(A4, (u32)A3 * 7); ST32(A4 + 4, 16); RET(1); }   // (hdc, str, n, SIZE*)
SHIM(GetTextExtentPoint32W) { ST32(A4, (u32)A3 * 7); ST32(A4 + 4, 16); RET(1); }

// ---- imm32: no IME on this path (SDL uses macOS text input via our WM_CHAR) ------------------------------
SHIM(ImmGetContext) { RET(0); }
SHIM(ImmReleaseContext) { RET(1); }
SHIM(ImmAssociateContext) { RET(0); }
SHIM(ImmAssociateContextEx) { RET(1); }
SHIM(ImmGetCompositionStringW) { RET(0); }
SHIM(ImmSetCompositionStringW) { RET(0); }
SHIM(ImmGetCandidateListW) { RET(0); }
SHIM(ImmGetCompositionFontW) { RET(0); }
SHIM(ImmSetCompositionWindow) { RET(1); }
SHIM(ImmSetCandidateWindow) { RET(1); }
SHIM(ImmNotifyIME) { RET(1); }
SHIM(ImmGetIMEFileNameA) { RET(0); }
SHIM(ImmGetOpenStatus) { RET(0); }
SHIM(ImmSetOpenStatus) { RET(1); }

// ---- setupapi / cfgmgr32: no PnP device tree (HID controllers come through XInput/SDL instead) ---------------
SHIM(SetupDiGetClassDevsA) { rt_set_last_error(13); RET((u64)-1); }
SHIM(SetupDiGetClassDevsW) { rt_set_last_error(13); RET((u64)-1); }
SHIM(SetupDiEnumDeviceInfo) { rt_set_last_error(259); RET(0); }   // ERROR_NO_MORE_ITEMS
SHIM(SetupDiDestroyDeviceInfoList) { RET(1); }
SHIM(SetupDiEnumDeviceInterfaces) { rt_set_last_error(259); RET(0); }        // ERROR_NO_MORE_ITEMS
SHIM(SetupDiGetDeviceInterfaceDetailA) { rt_set_last_error(259); RET(0); }
SHIM(SetupDiGetDeviceRegistryPropertyA) { rt_set_last_error(13); RET(0); }
SHIM(SetupDiGetDeviceInstanceIdA) { RET(0); }
SHIM(SetupDiGetDeviceRegistryPropertyW) { RET(0); }
SHIM(CM_Locate_DevNodeA) { RET(0x0D); }   // CR_NO_SUCH_DEVNODE
SHIM(CM_Get_Parent) { RET(0x0D); }
SHIM(CM_Get_Device_IDA) { RET(0x0D); }

// ---- dbghelp: no symbols ------------------------------------------------------------------------
SHIM(SymInitialize) { RET(1); }
SHIM(SymSetOptions) { RET(A1); }
SHIM(SymFromAddr) { RET(0); }
SHIM(SymGetLineFromAddr64) { RET(0); }
SHIM(SymCleanup) { RET(1); }

// ---- advapi32 security, version, winmm, ole32, oleaut32 ------------------------------------------------
SHIM(OpenProcessToken) { rt_set_last_error(5); RET(0); }
SHIM(LookupPrivilegeValueA) { rt_set_last_error(1313); RET(0); }
SHIM(AdjustTokenPrivileges) { rt_set_last_error(1300); RET(0); }
SHIM(GetFileVersionInfoSizeA) { if (A2) ST32(A2, 0); rt_set_last_error(1813); RET(0); }   // ERROR_RESOURCE_TYPE_NOT_FOUND
SHIM(GetFileVersionInfoSizeW) { sh_GetFileVersionInfoSizeA(c); }
SHIM(GetFileVersionInfoA) { RET(0); }
SHIM(GetFileVersionInfoW) { RET(0); }
SHIM(VerQueryValueA) { RET(0); }
SHIM(VerQueryValueW) { RET(0); }
SHIM(timeBeginPeriod) { RET(0); }
SHIM(timeEndPeriod) { RET(0); }
SHIM(timeGetTime) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); RET((u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000)); }
SHIM(RegisterDragDrop) { RET(0); }
SHIM(RevokeDragDrop) { RET(0); }
SHIM(ReleaseStgMedium) { RET(0); }
SHIM(CLSIDFromString) {  // "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
    char s[64]; size_t i = 0; for (; i + 1 < sizeof s && LD16(A1 + 2 * i); i++) s[i] = (char)LD16(A1 + 2 * i); s[i] = 0;
    unsigned a, b, cc, d[8];
    if (sscanf(s, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &a, &b, &cc, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) RET(0x800401F3u);
    ST32(A2, a); ST16(A2 + 4, (u16)b); ST16(A2 + 6, (u16)cc); for (int k = 0; k < 8; k++) ST8(A2 + 8 + k, (u8)d[k]);
    RET(0);
}
// BSTR: u32 byte length, then the string; the pointer points at the string
SHIM(SysAllocString) { if (!A1) RET(0); u64 n = 0; while (LD16(A1 + 2 * n)) n++; u8 *b = malloc(4 + 2 * n + 2); ST32((u64)b, (u32)(2 * n)); memcpy(b + 4, (void *)A1, 2 * n + 2); RET(b + 4); }
SHIM(SysFreeString) { if (A1) free((void *)(A1 - 4)); RET(0); }
SHIM(SysStringLen) { RET(A1 ? LD32(A1 - 4) / 2 : 0); }

// ws2_32 #51
SHIM(gethostbyaddr) { RET(0); }

static const char *const ord_oleaut32[] = { [2] = "SysAllocString", [4] = "SysAllocStringLen", [6] = "SysFreeString", [7] = "SysStringLen",
    [8] = "VariantInit", [9] = "VariantClear", [200] = "GetErrorInfo", [201] = "SetErrorInfo" };   // numbers from Wine's oleaut32.spec
const char *rt_extra_ordinal(const char *dll, u16 ord) {
    if (!strncasecmp(dll, "oleaut32", 8) && ord < sizeof ord_oleaut32 / sizeof *ord_oleaut32) return ord_oleaut32[ord];
    if ((!strncasecmp(dll, "ws2_32", 6) || !strncasecmp(dll, "wsock32", 7)) && ord == 51) return "gethostbyaddr";
    if (!strncasecmp(dll, "comctl32", 8) && ord == 17) return "InitCommonControls";
    if ((!strncasecmp(dll, "ws2_32", 6) || !strncasecmp(dll, "wsock32", 7)) && ord == 101) return "WSAAsyncSelect";
    return NULL;
}

const Shim shims_extra[] = {
#define E(n) { NULL, #n, sh_##n },
    E(AllocConsole) E(AttachConsole) E(FreeConsole) E(GetConsoleOutputCP) E(GetConsoleCP) E(SetConsoleOutputCP) E(GetConsoleScreenBufferInfo)
    E(GetNumberOfConsoleInputEvents) E(ReadConsoleInputA) E(ReadConsoleInputW) E(ReadConsoleW) E(GenerateConsoleCtrlEvent) E(SetConsoleTitleW) E(SetConsoleTitleA)
    E(WriteConsoleW) E(WriteConsoleA) E(SetStdHandle) E(CheckRemoteDebuggerPresent) E(DebugBreak) E(IsWow64Process) E(IsWow64Process2)
    E(GetCurrentProcessorNumber) E(OpenThread) E(CreateProcessW) E(CreateProcessA) E(GetExitCodeProcess) E(TerminateProcess)
    E(CreateToolhelp32Snapshot) E(Process32FirstW) E(Process32NextW) E(EnumResourceNamesW) E(SetThreadExecutionState) E(SetHandleInformation)
    E(SetNamedPipeHandleState) E(FreeLibraryAndExitThread) E(GetStartupInfoW) E(GetStartupInfoA) E(GetProcessTimes) E(GetSystemPowerStatus)
    E(GlobalMemoryStatusEx) E(HeapSize) E(GlobalLock) E(GlobalUnlock) E(GlobalSize) E(MulDiv) E(VerSetConditionMask) E(VerifyVersionInfoW)
    E(VerifyVersionInfoA) E(GetEnvironmentStringsW) E(FreeEnvironmentStringsW) E(InterlockedPushEntrySList) E(InterlockedPopEntrySList)
    E(InterlockedFlushSList) E(QueryDepthSList) E(GetLocaleInfoW) E(GetLocaleInfoA) E(GetLocaleInfoEx) E(GetUserDefaultLCID)
    E(GetSystemDefaultLCID) E(GetUserDefaultLangID) E(GetUserDefaultUILanguage) E(GetSystemDefaultLangID) E(GetThreadLocale)
    E(GetUserDefaultLocaleName) E(IsValidCodePage) E(CompareStringA) E(CompareStringW) E(LCMapStringW) E(GetStringTypeW) E(FormatMessageW)
    E(CreatePipe) E(CommandLineToArgvW) E(SHGetFolderPathW) E(SHGetFolderPathA) E(SHGetKnownFolderPath) E(SHGetPathFromIDListW)
    E(SHBrowseForFolderW) E(SHParseDisplayName) E(SHOpenFolderAndSelectItems) E(SHCreateDirectoryExW) E(ShellExecuteW) E(Shell_NotifyIconW)
    E(DragAcceptFiles) E(DragFinish) E(DragQueryFileW) E(BitBlt) E(StretchBlt)
    E(CombineRgn) E(CreateRectRgn) E(CreateSolidBrush) E(CreateFontIndirectW) E(CreateDCW)
    E(CreateFontW) E(CreatePen) E(CopyImage) E(ExtTextOutW) E(Rectangle) E(SetBkMode) E(SetTextColor)
    E(GetDeviceGammaRamp) E(SetDeviceGammaRamp) E(waveOutGetNumDevs) E(waveInGetNumDevs) E(waveOutGetErrorTextW)
    { NULL, "waveOutOpen", sh_waveNoDriver }, { NULL, "waveOutClose", sh_waveNoDriver }, { NULL, "waveOutWrite", sh_waveNoDriver },
    { NULL, "waveOutPrepareHeader", sh_waveNoDriver }, { NULL, "waveOutUnprepareHeader", sh_waveNoDriver },
    { NULL, "waveOutReset", sh_waveNoDriver }, { NULL, "waveOutGetDevCapsW", sh_waveNoDriver },
    { NULL, "waveInOpen", sh_waveNoDriver }, { NULL, "waveInClose", sh_waveNoDriver }, { NULL, "waveInStart", sh_waveNoDriver },
    { NULL, "waveInReset", sh_waveNoDriver }, { NULL, "waveInAddBuffer", sh_waveNoDriver }, { NULL, "waveInPrepareHeader", sh_waveNoDriver },
    { NULL, "waveInUnprepareHeader", sh_waveNoDriver }, { NULL, "waveInGetDevCapsW", sh_waveNoDriver },
    E(GetDIBits) E(SetPixel) E(GetICMProfileW) E(GetTextMetricsW) E(GetTextExtentPoint32A) E(GetTextExtentPoint32W)
    E(ImmGetContext) E(ImmReleaseContext) E(ImmAssociateContext) E(ImmAssociateContextEx) E(ImmGetCompositionStringW)
    E(ImmSetCompositionStringW) E(ImmGetCandidateListW) E(ImmGetCompositionFontW) E(ImmSetCompositionWindow) E(ImmSetCandidateWindow)
    E(ImmNotifyIME) E(ImmGetIMEFileNameA) E(ImmGetOpenStatus) E(ImmSetOpenStatus) E(SetupDiGetClassDevsA) E(SetupDiGetClassDevsW)
    E(SetupDiEnumDeviceInfo) E(SetupDiDestroyDeviceInfoList) E(SetupDiEnumDeviceInterfaces) E(SetupDiGetDeviceInterfaceDetailA) E(SetupDiGetDeviceRegistryPropertyA) E(SetupDiGetDeviceInstanceIdA) E(SetupDiGetDeviceRegistryPropertyW)
    E(CM_Locate_DevNodeA) E(CM_Get_Parent) E(CM_Get_Device_IDA) E(SymInitialize) E(SymSetOptions) E(SymFromAddr) E(SymGetLineFromAddr64)
    E(SymCleanup) E(OpenProcessToken) E(LookupPrivilegeValueA) E(AdjustTokenPrivileges) E(GetFileVersionInfoSizeA) E(GetFileVersionInfoSizeW)
    E(GetFileVersionInfoA) E(GetFileVersionInfoW) E(VerQueryValueA) E(VerQueryValueW) E(timeBeginPeriod) E(timeEndPeriod) E(timeGetTime)
    E(RegisterDragDrop) E(RevokeDragDrop) E(ReleaseStgMedium) E(CLSIDFromString) E(SysAllocString) E(SysFreeString) E(SysStringLen)
    E(gethostbyaddr)
#undef E
    { 0, 0, 0 }
};

// ---- version: present as Windows 10 22H2 (build 19045) so libraries pick their modern, well-tested paths --------
static void fill_version(u64 v, int wide) {   // OSVERSIONINFO[EX]{W,A}
    u32 size = LD32(v);
    u32 csd = wide ? 256 : 128;
    memset((void *)(v + 4), 0, size > 4 ? size - 4 : 0);
    ST32(v + 4, 10); ST32(v + 8, 0); ST32(v + 12, 19045); ST32(v + 16, 2 /* VER_PLATFORM_WIN32_NT */);
    if (size >= 20 + csd + 8) { ST8(v + 20 + csd + 6, 1 /* VER_NT_WORKSTATION */); }
}
SHIM(RtlGetVersion) { fill_version(A1, 1); RET(0); }
SHIM(GetVersionExW) { fill_version(A1, 1); RET(1); }
SHIM(GetVersionExA) { fill_version(A1, 0); RET(1); }
SHIM(GetSystemTimePreciseAsFileTime) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ST64(A1, ((u64)ts.tv_sec + 11644473600ull) * 10000000ull + (u64)ts.tv_nsec / 100); RET(0);
}

// ---- DPI: per-monitor aware at 96 DPI (macOS handles Retina scaling of the Metal layer) ---------------------------
SHIM(GetDpiForMonitor) { if (A3) ST32(A3, 96); if (A4) ST32(A4, 96); RET(0); }   // (hmon, type, *x, *y)
SHIM(GetDpiForSystem) { RET(96); }
SHIM(SetProcessDpiAwareness) { RET(0); }
SHIM(GetProcessDpiAwareness) { if (A2) ST32(A2, 2); RET(0); }
SHIM(GetThreadDpiAwarenessContext) { RET((u64)-4); }                 // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
SHIM(SetThreadDpiAwarenessContext) { RET((u64)-4); }                 // returns the previous context
SHIM(GetAwarenessFromDpiAwarenessContext) { RET(2); }                // DPI_AWARENESS_PER_MONITOR_AWARE
SHIM(IsValidDpiAwarenessContext) { RET(1); }
SHIM(AreDpiAwarenessContextsEqual) { RET(A1 == A2); }
SHIM(EnableNonClientDpiScaling) { RET(1); }
SHIM(GetSystemMetricsForDpi) { extern void rt_display_mode(int *, int *, int *); int w, h, hz; rt_display_mode(&w, &h, &hz);
    i32 i = (i32)A1; RET(i == 0 ? (u32)w : i == 1 ? (u32)h : 0); }

// ---- CPU topology: one NUMA node / processor group holding every core -------------------------------------------
static u64 cpu_mask(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n >= 64 ? ~0ull : (1ull << n) - 1; }
SHIM(GetNumaHighestNodeNumber) { ST32(A1, 0); RET(1); }
SHIM(GetNumaNodeProcessorMask) { if ((u8)A1) RET(0); ST64(A2, cpu_mask()); RET(1); }
SHIM(GetNumaNodeProcessorMaskEx) { if ((u16)A1) RET(0); memset((void *)A2, 0, 16); ST64(A2, cpu_mask()); RET(1); }   // GROUP_AFFINITY
SHIM(GetNumaProcessorNode) { ST8(A2, 0); RET(1); }
SHIM(GetNumaProcessorNodeEx) { ST16(A2, 0); RET(1); }
SHIM(GetCurrentProcessorNumberEx) { ST32(A1, 0); RET(0); }   // PROCESSOR_NUMBER { group, number, reserved }
SHIM(GetActiveProcessorCount) { RET((u32)sysconf(_SC_NPROCESSORS_ONLN)); }
SHIM(GetActiveProcessorGroupCount) { RET(1); }
SHIM(GetMaximumProcessorCount) { RET((u32)sysconf(_SC_NPROCESSORS_ONLN)); }
SHIM(GetLargePageMinimum) { RET(0); }   // no large pages
SHIM(GetPhysicallyInstalledSystemMemory) { u64 t = 0; size_t sz = sizeof t; sysctlbyname("hw.memsize", &t, &sz, NULL, 0); ST64(A1, t / 1024); RET(1); }
SHIM(K32GetProcessMemoryInfo) {  // PROCESS_MEMORY_COUNTERS: cb, PageFaultCount, Peak/WorkingSetSize, ... PagefileUsage, PeakPagefileUsage
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    u32 cb = (u32)A3; if (cb < 72) RET(0);
    memset((void *)(A2 + 4), 0, cb - 4);
    ST32(A2 + 4, (u32)ru.ru_minflt); ST64(A2 + 8, (u64)ru.ru_maxrss); ST64(A2 + 16, (u64)ru.ru_maxrss);
    ST64(A2 + 56, (u64)ru.ru_maxrss); ST64(A2 + 64, (u64)ru.ru_maxrss);
    RET(1);
}
SHIM(GetProcessMemoryInfo) { sh_K32GetProcessMemoryInfo(c); }

// ---- locale / COM / DWM / display config / touch --------------------------------------------------------------
SHIM(GetUserPreferredUILanguages) {  // (flags, *num, PZZWSTR buf, *size in chars)
    static const char langs[] = "en-US\0";
    if (A2) ST32(A2, 1);
    u32 need = 7;
    if (!A3) { ST32(A4, need); RET(1); }
    if (LD32(A4) < need) { ST32(A4, need); rt_set_last_error(122); RET(0); }
    for (u32 i = 0; i < need; i++) ST16(A3 + 2 * i, i < 6 ? (u8)langs[i] : 0);
    ST32(A4, need); RET(1);
}
SHIM(GetSystemPreferredUILanguages) { sh_GetUserPreferredUILanguages(c); }
SHIM(CoIncrementMTAUsage) { if (A1) ST64(A1, 0x7700); RET(0); }
SHIM(CoDecrementMTAUsage) { RET(0); }
SHIM(DwmSetWindowAttribute) { RET(0); }
SHIM(DwmGetWindowAttribute) { RET(E_INVALIDARG_); }
SHIM(DwmFlush) { RET(0); }
SHIM(DwmEnableBlurBehindWindow) { RET(0); }
SHIM(DwmIsCompositionEnabled) { if (A1) ST32(A1, 1); RET(0); }
SHIM(DwmExtendFrameIntoClientArea) { RET(0); }
SHIM(GetDisplayConfigBufferSizes) { RET(50); }   // ERROR_NOT_SUPPORTED: callers fall back to EnumDisplay*
SHIM(QueryDisplayConfig) { RET(50); }
SHIM(DisplayConfigGetDeviceInfo) { RET(50); }
SHIM(RegisterTouchWindow) { RET(1); }
SHIM(UnregisterTouchWindow) { RET(1); }
SHIM(GetTouchInputInfo) { RET(0); }
SHIM(CloseTouchInputHandle) { RET(1); }
SHIM(GetPointerType) { RET(0); }
SHIM(GetPointerPenInfo) { RET(0); }
SHIM(GetPointerDeviceRects) { RET(0); }
SHIM(EnableMouseInPointer) { RET(0); }
SHIM(SetWindowCompositionAttribute) { RET(0); }

const Shim shims_extra2[] = {
#define E(n) { NULL, #n, sh_##n },
    E(RtlGetVersion) E(GetVersionExW) E(GetVersionExA) E(GetSystemTimePreciseAsFileTime) E(GetDpiForMonitor) E(GetDpiForSystem)
    E(SetProcessDpiAwareness) E(GetProcessDpiAwareness) E(GetThreadDpiAwarenessContext) E(SetThreadDpiAwarenessContext)
    E(GetAwarenessFromDpiAwarenessContext) E(IsValidDpiAwarenessContext) E(AreDpiAwarenessContextsEqual) E(EnableNonClientDpiScaling)
    E(GetSystemMetricsForDpi) E(GetNumaHighestNodeNumber) E(GetNumaNodeProcessorMask) E(GetNumaNodeProcessorMaskEx) E(GetNumaProcessorNode)
    E(GetNumaProcessorNodeEx) E(GetCurrentProcessorNumberEx) E(GetActiveProcessorCount) E(GetActiveProcessorGroupCount)
    E(GetMaximumProcessorCount) E(GetLargePageMinimum) E(GetPhysicallyInstalledSystemMemory) E(K32GetProcessMemoryInfo)
    E(GetProcessMemoryInfo) E(GetUserPreferredUILanguages) E(GetSystemPreferredUILanguages) E(CoIncrementMTAUsage) E(CoDecrementMTAUsage)
    E(DwmSetWindowAttribute) E(DwmGetWindowAttribute) E(DwmFlush) E(DwmEnableBlurBehindWindow) E(DwmIsCompositionEnabled)
    E(DwmExtendFrameIntoClientArea) E(GetDisplayConfigBufferSizes) E(QueryDisplayConfig) E(DisplayConfigGetDeviceInfo)
    E(RegisterTouchWindow) E(UnregisterTouchWindow) E(GetTouchInputInfo) E(CloseTouchInputHandle) E(GetPointerType)
    E(GetPointerPenInfo) E(GetPointerDeviceRects) E(EnableMouseInPointer) E(SetWindowCompositionAttribute)
#undef E
    { 0, 0, 0 }
};
