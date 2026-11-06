// Small shared helpers for the API shims in advapi32.c, crt2.c, kernel32x.c, win32x.c and vcrt.c:
// UTF-16 <-> UTF-8 on guest memory, Windows <-> host paths, and "unsupported feature" logging.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32.h"

// Guest UTF-16 string (pointer) -> UTF-8 in out[n]. Handles surrogate pairs. Returns bytes written.
static inline size_t wx_w2u(u64 w, char *out, size_t n) {
    size_t o = 0;
    if (!n) return 0;
    for (size_t i = 0; w && LD16(w + 2 * i); i++) {
        u32 cp = LD16(w + 2 * i);
        if (cp >= 0xD800 && cp < 0xDC00 && LD16(w + 2 * i + 2) >= 0xDC00 && LD16(w + 2 * i + 2) < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (LD16(w + 2 * i + 2) - 0xDC00); i++;
        }
        char t[4]; int k;
        if (cp < 0x80) { t[0] = (char)cp; k = 1; }
        else if (cp < 0x800) { t[0] = (char)(0xC0 | cp >> 6); t[1] = (char)(0x80 | (cp & 63)); k = 2; }
        else if (cp < 0x10000) { t[0] = (char)(0xE0 | cp >> 12); t[1] = (char)(0x80 | (cp >> 6 & 63)); t[2] = (char)(0x80 | (cp & 63)); k = 3; }
        else { t[0] = (char)(0xF0 | cp >> 18); t[1] = (char)(0x80 | (cp >> 12 & 63)); t[2] = (char)(0x80 | (cp >> 6 & 63)); t[3] = (char)(0x80 | (cp & 63)); k = 4; }
        if (o + k + 1 > n) break;
        memcpy(out + o, t, k); o += k;
    }
    out[o] = 0;
    return o;
}
// UTF-8 -> guest UTF-16 at w (cap units incl. terminator; w may be 0 to measure). Returns units needed incl. NUL.
static inline size_t wx_u2w(const char *s, u64 w, size_t cap) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        u32 cp = *p++;
        if (cp >= 0xF0 && p[0] && p[1] && p[2]) { cp = (cp & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
        else if (cp >= 0xE0 && p[0] && p[1]) { cp = (cp & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (cp >= 0xC0 && p[0]) { cp = (cp & 31) << 6 | (p[0] & 63); p += 1; }
        if (cp >= 0x10000) {
            if (w && o + 2 < cap) { ST16(w + 2 * o, (u16)(0xD800 + ((cp - 0x10000) >> 10))); ST16(w + 2 * o + 2, (u16)(0xDC00 + ((cp - 0x10000) & 0x3FF))); }
            o += 2;
        } else {
            if (w && o + 1 < cap) ST16(w + 2 * o, (u16)cp);
            o++;
        }
    }
    if (w && cap) ST16(w + 2 * (o < cap ? o : cap - 1), 0);
    return o + 1;
}
static inline size_t wx_wlen(u64 w) { size_t n = 0; while (LD16(w + 2 * n)) n++; return n; }

// Copy a UTF-8 result into a caller buffer the Win32 way: buffer too small -> required size incl. NUL,
// else characters written without NUL. (GetCurrentDirectory / GetTempPath / GetFullPathName convention.)
static inline u32 wx_ret_str_w(const char *s, u64 buf, u32 cap) {
    size_t need = wx_u2w(s, 0, 0);
    if (!buf || cap < need) return (u32)need;
    wx_u2w(s, buf, cap);
    return (u32)need - 1;
}
static inline u32 wx_ret_str_a(const char *s, u64 buf, u32 cap) {
    size_t n = strlen(s);
    if (!buf || cap < n + 1) return (u32)(n + 1);
    memcpy((void *)buf, s, n + 1);
    return (u32)n;
}

// Features we deliberately don't provide fail with a documented error code; WINTOMAC_STUBS=1 logs each once.
static inline void wx_stub_log(const char *name) {
    static const char *seen[256]; static int nseen;
    static int on = -1;
    if (on < 0) on = getenv("WINTOMAC_STUBS") && *getenv("WINTOMAC_STUBS") != '0';
    if (!on) return;
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], name)) return;
    if (nseen < 256) seen[nseen++] = name;
    rt_log("unsupported: %s (returning failure)", name);
}
#define WX_FAIL(name, err, ret) do { wx_stub_log(#name); rt_set_last_error(err); RET(ret); } while (0)

// Paths (kernel32x.c): the process' Windows-side current directory and conversions.
const char *rt_win_cwd(void);                                    // e.g. "C:\\" or "C:\\data\\"
int rt_full_path(const char *in, char *out, size_t n);           // GetFullPathName semantics; returns length
void rt_win_path_from_host(const char *host, char *out, size_t n);
