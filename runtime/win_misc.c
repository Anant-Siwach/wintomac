// Assorted Win32 / msvcrt surface: registry, GDI objects, codepages, display enumeration, locale bits.
#include <SDL3/SDL.h>
#include <pthread.h>
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wctype.h>
#include <sched.h>

#include "win32.h"

extern void rt_set_last_error(u32 e);
FILE *rt_host_file(u64 p);

// ---- exceptions (registration only; dispatch needs SEH support) ------------------------
static u64 veh_next = 0x5EED0001;
SHIM(AddVectoredExceptionHandler) { RET(veh_next++); }
SHIM(RemoveVectoredExceptionHandler) { RET(1); }
SHIM(AddVectoredContinueHandler) { RET(veh_next++); }
SHIM(RemoveVectoredContinueHandler) { RET(1); }

// ---- advapi32: the registry lives in advapi32.c ------------------------------------------
#define ERROR_FILE_NOT_FOUND 2
static u64 luid_next = 0x1000;
SHIM(AllocateLocallyUniqueId) { ST32(A1, (u32)__atomic_fetch_add(&luid_next, 1, __ATOMIC_SEQ_CST)); ST32(A1 + 4, 0); RET(1); }

// ---- gdi32: opaque objects --------------------------------------------------------------
static u64 gdi_next = 0x40000;
SHIM(CreateBitmap) { RET(gdi_next++); }
SHIM(CreateCompatibleDC) { RET(gdi_next++); }
SHIM(CreateCompatibleBitmap) { RET(gdi_next++); }
// DIB sections own pixel memory the caller writes through *ppvBits (SDL builds window icons this way).
static struct { u64 h; void *bits; } dibs[64];
SHIM(CreateDIBSection) {  // (hdc, const BITMAPINFO *, usage, void **ppvBits, hSection, offset)
    i32 w = (i32)LD32(A2 + 4), h = (i32)LD32(A2 + 8); u16 bpp = LD16(A2 + 14);
    if (w <= 0 || !h || !bpp) { if (A4) ST64(A4, 0); RET(0); }
    size_t stride = ((size_t)w * bpp + 31) / 32 * 4, size = stride * (size_t)(h < 0 ? -h : h);
    void *bits = calloc(1, size);
    u64 hb = gdi_next++;
    for (int i = 0; i < 64; i++) if (!dibs[i].h) { dibs[i].h = hb; dibs[i].bits = bits; break; }
    if (A4) ST64(A4, (u64)bits);
    RET(hb);
}
SHIM(SelectObject) { RET(1); }
SHIM(DeleteDC) { RET(1); }
SHIM(DeleteObject) { for (int i = 0; i < 64; i++) if (dibs[i].h == A1) { free(dibs[i].bits); dibs[i].h = 0; dibs[i].bits = NULL; } RET(1); }
extern void rt_display_mode(int *w, int *h, int *hz);
extern void rt_desktop_mode(int *w, int *h, int *hz);
extern int rt_display_change(int w, int h, int hz, int test);
SHIM(GetDeviceCaps) {  // HORZRES/DESKTOPHORZRES, VERTRES/DESKTOPVERTRES, BITSPIXEL, LOGPIXELSX/Y, VREFRESH
    int w, h, hz; rt_display_mode(&w, &h, &hz);
    switch ((i32)A2) { case 8: case 118: RET(w); case 10: case 117: RET(h); case 12: RET(32); case 88: case 90: RET(96); case 116: RET(hz); default: RET(0); }
}
SHIM(CreateIconIndirect) { RET(gdi_next++); }
SHIM(DestroyCursor) { RET(1); }
SHIM(DestroyIcon) { RET(1); }
SHIM(IsWindowUnicode) { RET(1); }

// ---- codepages: the ANSI codepage is UTF-8 (GetACP() == 65001) ------------------------------
static int utf8_decode(const u8 *s, long n, long *i, u32 *cp) {
    u8 b = s[*i];
    int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
    if (*i + len > n) len = 1;
    u32 v = len == 1 ? b : len == 2 ? b & 31 : len == 3 ? b & 15 : b & 7;
    for (int k = 1; k < len; k++) v = (v << 6) | (s[*i + k] & 63);
    *i += len; *cp = len == 1 && b >= 0x80 ? 0xFFFD : v;
    return len;
}
SHIM(MultiByteToWideChar) {  // (cp, flags, src, srclen, dst, dstlen)
    const u8 *s = (const u8 *)A3; long n = (long)(i32)A4; u64 dst = AN(5); long cap = (long)(i32)AN(6);
    if (n < 0) n = (long)strlen((const char *)s) + 1;
    long i = 0, o = 0;
    while (i < n) {
        u32 cp; utf8_decode(s, n, &i, &cp);
        int units = cp >= 0x10000 ? 2 : 1;
        if (cap) {
            if (o + units > cap) { rt_set_last_error(122); RET(0); }  // ERROR_INSUFFICIENT_BUFFER
            if (units == 2) { cp -= 0x10000; ST16(dst + 2 * o, (u16)(0xD800 | cp >> 10)); ST16(dst + 2 * o + 2, (u16)(0xDC00 | (cp & 0x3FF))); }
            else ST16(dst + 2 * o, (u16)cp);
        }
        o += units;
    }
    RET(o);
}
SHIM(WideCharToMultiByte) {  // (cp, flags, src, srclen, dst, dstlen, defchar, useddef)
    u64 s = A3; long n = (long)(i32)A4; u8 *dst = (u8 *)AN(5); long cap = (long)(i32)AN(6);
    if (n < 0) { n = 0; while (LD16(s + 2 * n)) n++; n++; }
    long o = 0;
    for (long i = 0; i < n; i++) {
        u32 cp = LD16(s + 2 * i);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) { cp = 0x10000 + ((cp - 0xD800) << 10) + (LD16(s + 2 * i + 2) - 0xDC00); i++; }
        u8 t[4]; int k = cp < 0x80 ? (t[0] = (u8)cp, 1) : cp < 0x800 ? (t[0] = 0xC0 | cp >> 6, t[1] = 0x80 | (cp & 63), 2)
                 : cp < 0x10000 ? (t[0] = 0xE0 | cp >> 12, t[1] = 0x80 | (cp >> 6 & 63), t[2] = 0x80 | (cp & 63), 3)
                 : (t[0] = 0xF0 | cp >> 18, t[1] = 0x80 | (cp >> 12 & 63), t[2] = 0x80 | (cp >> 6 & 63), t[3] = 0x80 | (cp & 63), 4);
        if (cap) { if (o + k > cap) { rt_set_last_error(122); RET(0); } memcpy(dst + o, t, k); }
        o += k;
    }
    if (AN(8)) ST32(AN(8), 0);
    RET(o);
}
SHIM(IsDBCSLeadByteEx) { RET(0); }
SHIM(IsDBCSLeadByte) { RET(0); }
SHIM(GetCPInfo) { u64 p = A2; memset((void *)p, 0, 20); ST32(p, 4); ST8(p + 4, '?'); RET(1); }

// ---- environment ------------------------------------------------------------------------
SHIM(GetEnvironmentVariableW) {  // (name, buf, size)
    char name[256]; size_t i = 0; for (; i + 1 < sizeof name && LD16(A1 + 2 * i); i++) name[i] = (char)LD16(A1 + 2 * i); name[i] = 0;
    const char *v = getenv(name);
    if (!v) { rt_set_last_error(203); RET(0); }  // ERROR_ENVVAR_NOT_FOUND
    size_t n = strlen(v);
    if (n + 1 > (u32)A3) RET(n + 1);
    for (size_t k = 0; k < n; k++) ST16(A2 + 2 * k, (u8)v[k]);
    ST16(A2 + 2 * n, 0); RET(n);
}
SHIM(SetEnvironmentVariableW) {
    char name[256], val[1024]; size_t i = 0;
    for (; i + 1 < sizeof name && LD16(A1 + 2 * i); i++) name[i] = (char)LD16(A1 + 2 * i); name[i] = 0;
    if (!A2) { unsetenv(name); RET(1); }
    for (i = 0; i + 1 < sizeof val && LD16(A2 + 2 * i); i++) val[i] = (char)LD16(A2 + 2 * i); val[i] = 0;
    setenv(name, val, 1); RET(1);
}
SHIM(SetEnvironmentVariableA) { if (A2) setenv((char *)A1, (char *)A2, 1); else unsetenv((char *)A1); RET(1); }
SHIM(GetHandleInformation) { if (A2) ST32(A2, 0); RET(1); }
SHIM(DeviceIoControl) { rt_set_last_error(1); RET(0); }  // ERROR_INVALID_FUNCTION

// ---- displays: one monitor, modes from SDL --------------------------------------------------
static const SDL_DisplayMode **modes; static int nmodes;
static void load_modes(void) {
    if (modes) return;
    SDL_InitSubSystem(SDL_INIT_VIDEO);
    modes = (const SDL_DisplayMode **)SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &nmodes);
    if (!modes) { static const SDL_DisplayMode *none[1]; modes = none; nmodes = 0; }
}
static void put_w(u64 dst, const char *s, size_t cap) { size_t i = 0; for (; s[i] && i + 1 < cap; i++) ST16(dst + 2 * i, (u8)s[i]); ST16(dst + 2 * i, 0); }

// DEVMODEW / DEVMODEA differ only in the two 32-char name fields (wchar vs char): A layout = W offset - 32 / - 64.
#define DM_W 0
#define DM_A 1
static u32 dm_off(int a, u32 woff) { return !a ? woff : woff >= 102 ? woff - 64 : woff >= 64 ? woff - 32 : woff; }
static int fill_devmode(u64 dm, i32 idx, int a) {
    load_modes();
    int w, h, hz;
    if (idx == -1) rt_display_mode(&w, &h, &hz);          // ENUM_CURRENT_SETTINGS: what the game set
    else if (idx == -2) rt_desktop_mode(&w, &h, &hz);     // ENUM_REGISTRY_SETTINGS
    else {
        if (idx < 0 || idx >= nmodes) return 0;           // Windows lists modes ascending; SDL lists descending
        const SDL_DisplayMode *m = modes[nmodes - 1 - idx];
        w = m->w; h = m->h; hz = m->refresh_rate > 0 ? (int)(m->refresh_rate + 0.5f) : 60;
    }
    u32 size = a ? 156 : 220;
    u16 extra = LD16(dm + dm_off(a, 70));
    memset((void *)dm, 0, size); ST16(dm + dm_off(a, 70), extra);
    if (a) strcpy((char *)dm, "\\\\.\\DISPLAY1"); else put_w(dm, "\\\\.\\DISPLAY1", 32);
    ST16(dm + dm_off(a, 64), 0x401); ST16(dm + dm_off(a, 68), (u16)size);
    ST32(dm + dm_off(a, 72), 0x00040000 | 0x00080000 | 0x00100000 | 0x00400000 | 0x00000020);  // BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY|POSITION
    ST32(dm + dm_off(a, 168), 32); ST32(dm + dm_off(a, 172), (u32)w); ST32(dm + dm_off(a, 176), (u32)h); ST32(dm + dm_off(a, 184), (u32)hz);
    return 1;
}
SHIM(EnumDisplaySettingsW) { RET(fill_devmode(A3, (i32)(u32)A2, DM_W)); }   // (device, modenum, DEVMODE*)
SHIM(EnumDisplaySettingsExW) { RET(fill_devmode(A3, (i32)(u32)A2, DM_W)); }
SHIM(EnumDisplaySettingsA) { RET(fill_devmode(A3, (i32)(u32)A2, DM_A)); }
SHIM(EnumDisplaySettingsExA) { RET(fill_devmode(A3, (i32)(u32)A2, DM_A)); }
// ChangeDisplaySettings: virtual mode switch (see user32.c). NULL devmode = back to the desktop mode.
static u32 change_mode(u64 dm, u32 flags, int a) {
    if (!dm) return (u32)rt_display_change(0, 0, 0, 0);
    u32 fields = LD32(dm + dm_off(a, 72));
    int dw, dh, dhz; rt_display_mode(&dw, &dh, &dhz);
    int w = (fields & 0x00080000) ? (int)LD32(dm + dm_off(a, 172)) : dw;
    int h = (fields & 0x00100000) ? (int)LD32(dm + dm_off(a, 176)) : dh;
    int hz = (fields & 0x00400000) ? (int)LD32(dm + dm_off(a, 184)) : 0;
    if (fields & 0x00040000 && LD32(dm + dm_off(a, 168)) && LD32(dm + dm_off(a, 168)) < 16) return (u32)-2;  // DISP_CHANGE_BADMODE
    int dsw, dsh, dshz; rt_desktop_mode(&dsw, &dsh, &dshz);
    if (w == dsw && h == dsh && !(flags & 2)) return (u32)rt_display_change(0, 0, 0, 0);
    return (u32)rt_display_change(w, h, hz, flags & 2 /* CDS_TEST */);
}
SHIM(ChangeDisplaySettingsExW) { RET(change_mode(A2, (u32)A4, DM_W)); }  // (device, devmode, hwnd, flags, lparam)
SHIM(ChangeDisplaySettingsW) { RET(change_mode(A1, (u32)A2, DM_W)); }      // (devmode, flags)
SHIM(ChangeDisplaySettingsExA) { RET(change_mode(A2, (u32)A4, DM_A)); }
SHIM(ChangeDisplaySettingsA) { RET(change_mode(A1, (u32)A2, DM_A)); }
SHIM(EnumDisplayDevicesA) {  // (device, index, DISPLAY_DEVICEA*, flags)
    if ((u32)A2 > 0) RET(0);
    u64 d = A3; u32 cb = LD32(d); memset((void *)d, 0, cb > 424 ? 424 : cb); ST32(d, cb);
    strcpy((char *)d + 4, A1 ? "Monitor" : "\\\\.\\DISPLAY1"); strcpy((char *)d + 36, "wintomac display");
    ST32(d + 164, 1 | 4); RET(1);
}
SHIM(EnumDisplayDevicesW) {
    if ((u32)A2 > 0) RET(0);
    u64 d = A3; u32 cb = LD32(d); memset((void *)d, 0, cb > 840 ? 840 : cb); ST32(d, cb);
    put_w(d + 4, A1 ? "Monitor" : "\\\\.\\DISPLAY1", 32); put_w(d + 68, "wintomac display", 128);
    ST32(d + 324, 1 | 4); RET(1);
}
SHIM(EnumDisplayMonitors) {  // (hdc, clip, MONITORENUMPROC, lparam): one monitor
    load_modes();
    int w, h, hz; rt_display_mode(&w, &h, &hz);
    static u32 rect[4]; rect[0] = rect[1] = 0; rect[2] = (u32)w; rect[3] = (u32)h;
    u64 args[4] = { 0x10020, 0, (u64)rect, A4 };
    rt_guest_call(c, A3, 4, args);
    RET(1);
}

// ---- msvcrt odds and ends -----------------------------------------------------------------
static pthread_mutex_t crt_locks[64];
static void crt_locks_init(void) {
    static int done; if (done) return; done = 1;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < 64; i++) pthread_mutex_init(&crt_locks[i], &a);
}
SHIM(_lock) { crt_locks_init(); pthread_mutex_lock(&crt_locks[(u32)A1 & 63]); RET(0); }
SHIM(_unlock) { crt_locks_init(); pthread_mutex_unlock(&crt_locks[(u32)A1 & 63]); RET(0); }
SHIM(___lc_codepage_func) { RET(0); }   // "C" locale
SHIM(___mb_cur_max_func) { RET(1); }
SHIM(__mb_cur_max) { static int one = 1; RET(&one); }
SHIM(setlocale) { static char cl[] = "C"; RET(cl); }
SHIM(_ultoa) { char *b = (char *)A2; unsigned long v = (u32)A1; int base = (int)A3; char t[40]; int n = 0;
               do { int d = (int)(v % base); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
               for (int i = 0; i < n; i++) b[i] = t[n - 1 - i]; b[n] = 0; RET(b); }
SHIM(_ui64toa) { char *b = (char *)A2; u64 v = A1; int base = (int)A3; char t[72]; int n = 0;
                 do { int d = (int)(v % base); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
                 for (int i = 0; i < n; i++) b[i] = t[n - 1 - i]; b[n] = 0; RET(b); }
SHIM(getc) { RET((u32)getc(rt_host_file(A1))); }
SHIM(putc) { RET((u32)putc((int)A1, rt_host_file(A2))); }
SHIM(ungetc) { RET((u32)ungetc((int)A1, rt_host_file(A2))); }
SHIM(fgetpos) { fpos_t p; int r = fgetpos(rt_host_file(A1), &p); ST64(A2, (u64)ftello(rt_host_file(A1))); RET((u32)r); }
SHIM(fsetpos) { RET((u32)fseeko(rt_host_file(A1), (off_t)LD64(A2), SEEK_SET)); }
// wide-char stdio: UTF-16 units, done byte-wise through the host FILE (rare in practice)
SHIM(fgetwc) { int ch = fgetc(rt_host_file(A1)); RET(ch == EOF ? 0xFFFF : (u32)ch); }
SHIM(getwc) { sh_fgetwc(c); }
SHIM(putwc) { u32 ch = (u16)A1; fputc(ch < 0x80 ? (int)ch : '?', rt_host_file(A2)); RET(ch); }
SHIM(ungetwc) { RET((u32)ungetc((int)(u16)A1, rt_host_file(A2))); }
SHIM(strcoll) { RET((u32)strcmp((char *)A1, (char *)A2)); }
SHIM(strxfrm) { size_t n = strlen((char *)A2); if (A3 > n) memcpy((void *)A1, (void *)A2, n + 1); RET(n); }
SHIM(wcscoll) { u64 a = A1, b = A2; for (;; a += 2, b += 2) { u16 x = LD16(a), y = LD16(b); if (x != y || !x) RET((u32)((int)x - (int)y)); } }
SHIM(wcsxfrm) { u64 n = 0; while (LD16(A2 + 2 * n)) n++; if (A3 > n) memcpy((void *)A1, (void *)A2, 2 * (n + 1)); RET(n); }
SHIM(towlower) { u32 ch = (u16)A1; RET(ch < 128 ? (u32)tolower((int)ch) : ch); }
SHIM(towupper) { u32 ch = (u16)A1; RET(ch < 128 ? (u32)toupper((int)ch) : ch); }
SHIM(iswctype) {  // Windows ctype masks: _UPPER 1 _LOWER 2 _DIGIT 4 _SPACE 8 _PUNCT 16 _CONTROL 32 _BLANK 64 _HEX 128 _ALPHA 0x103
    u32 ch = (u16)A1, m = (u16)A2, r = 0;
    if (ch < 128) { r |= isupper((int)ch) ? 1 : 0; r |= islower((int)ch) ? 2 : 0; r |= isdigit((int)ch) ? 4 : 0; r |= isspace((int)ch) ? 8 : 0;
                    r |= ispunct((int)ch) ? 16 : 0; r |= iscntrl((int)ch) ? 32 : 0; r |= ch == ' ' || ch == '\t' ? 64 : 0;
                    r |= isxdigit((int)ch) ? 128 : 0; r |= isalpha((int)ch) ? 0x100 : 0; }
    RET(r & m);
}
SHIM(strftime) {  // (buf, max, fmt, struct tm*): Windows tm layout == 9 ints, same as the first 9 host fields
    struct tm t = {0}; u64 p = AN(4) ? A4 : A4;
    t.tm_sec = (int)LD32(p); t.tm_min = (int)LD32(p + 4); t.tm_hour = (int)LD32(p + 8); t.tm_mday = (int)LD32(p + 12);
    t.tm_mon = (int)LD32(p + 16); t.tm_year = (int)LD32(p + 20); t.tm_wday = (int)LD32(p + 24); t.tm_yday = (int)LD32(p + 28); t.tm_isdst = (int)LD32(p + 32);
    RET(strftime((char *)A1, A2, (const char *)A3, &t));
}
SHIM(wcsftime) { if (A2) ST16(A1, 0); RET(0); }

// ---- ntdll bits that user-mode libraries look up dynamically ----------------------------------
SHIM(RtlDllShutdownInProgress) { RET(0); }
void gfx_main_service(void);   // ps5_gfx.m
SHIM(NtDelayExecution) {  // (alertable, LARGE_INTEGER *interval): negative = relative, in 100 ns units
    gfx_main_service();
    i64 t = A2 ? (i64)LD64(A2) : 0;
    if (t < 0) { u64 us = (u64)(-t) / 10; struct timespec ts = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 }; nanosleep(&ts, NULL); }
    else sched_yield();
    RET(0);
}
static u32 timer_res = 10000;  // 1 ms, in 100 ns units
SHIM(NtQueryTimerResolution) { ST32(A1, 156250); ST32(A2, 5000); ST32(A3, timer_res); RET(0); }   // (*max, *min, *current)
SHIM(NtSetTimerResolution) { if ((u32)A2) timer_res = (u32)A1; if (A3) ST32(A3, timer_res); RET(0); }

const Shim shims_misc[] = {
    { NULL, "RtlDllShutdownInProgress", sh_RtlDllShutdownInProgress }, { NULL, "NtDelayExecution", sh_NtDelayExecution },
    { NULL, "NtQueryTimerResolution", sh_NtQueryTimerResolution }, { NULL, "NtSetTimerResolution", sh_NtSetTimerResolution },
#define E(n) { NULL, #n, sh_##n }
    E(AddVectoredExceptionHandler), E(RemoveVectoredExceptionHandler), E(AddVectoredContinueHandler), E(RemoveVectoredContinueHandler),
    E(AllocateLocallyUniqueId),
    E(CreateBitmap), E(CreateCompatibleDC), E(CreateCompatibleBitmap), E(CreateDIBSection), E(SelectObject), E(DeleteDC),
    E(DeleteObject), E(GetDeviceCaps), E(CreateIconIndirect), E(DestroyCursor), E(DestroyIcon), E(IsWindowUnicode),
    E(MultiByteToWideChar), E(WideCharToMultiByte), E(IsDBCSLeadByteEx), E(IsDBCSLeadByte), E(GetCPInfo),
    E(GetEnvironmentVariableW), E(SetEnvironmentVariableW), E(SetEnvironmentVariableA), E(GetHandleInformation), E(DeviceIoControl),
    E(EnumDisplaySettingsW), E(EnumDisplaySettingsExW), E(EnumDisplaySettingsA), E(EnumDisplaySettingsExA),
    E(ChangeDisplaySettingsExW), E(ChangeDisplaySettingsW), E(ChangeDisplaySettingsExA), E(ChangeDisplaySettingsA),
    E(EnumDisplayDevicesA), E(EnumDisplayDevicesW), E(EnumDisplayMonitors),
    E(_lock), E(_unlock), E(___lc_codepage_func), E(___mb_cur_max_func), E(__mb_cur_max), E(setlocale), E(_ultoa), E(_ui64toa),
    E(getc), E(putc), E(ungetc), E(fgetpos), E(fsetpos), E(fgetwc), E(getwc), E(putwc), E(ungetwc), E(strcoll), E(strxfrm),
    E(wcscoll), E(wcsxfrm), E(towlower), E(towupper), E(iswctype), E(strftime), E(wcsftime),
#undef E
    { 0, 0, 0 }
};
