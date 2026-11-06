// UCRT (api-ms-win-crt-*, ucrtbase) + legacy msvcrt shims.
// Matched by function name only (dll == NULL) because the api-set DLL names vary.
#include <ctype.h>
#include <pthread.h>
#include <strings.h>
#include <math.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <time.h>

#include "win32.h"

// ---- Windows printf engine ---------------------------------------------------
// Win64 va_list is a pointer into consecutive 8-byte slots (floats are stored as doubles).
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}

static size_t utf16_to_utf8(const u16 *w, long maxchars, char *out, size_t outcap) {
    size_t o = 0;
    for (long i = 0; (maxchars < 0 || i < maxchars) && w[i]; i++) {
        u32 cp = w[i];
        if (cp >= 0xD800 && cp < 0xDC00 && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; }
        char t[4]; int n;
        if (cp < 0x80) { t[0] = cp; n = 1; }
        else if (cp < 0x800) { t[0] = 0xC0 | cp >> 6; t[1] = 0x80 | (cp & 63); n = 2; }
        else if (cp < 0x10000) { t[0] = 0xE0 | cp >> 12; t[1] = 0x80 | (cp >> 6 & 63); t[2] = 0x80 | (cp & 63); n = 3; }
        else { t[0] = 0xF0 | cp >> 18; t[1] = 0x80 | (cp >> 12 & 63); t[2] = 0x80 | (cp >> 6 & 63); t[3] = 0x80 | (cp & 63); n = 4; }
        if (o + n < outcap) { memcpy(out + o, t, n); }
        o += n;
    }
    if (outcap) out[o < outcap ? o : outcap - 1] = 0;
    return o;
}

// wide: format string is UTF-16 (wprintf family). Output is always UTF-8 here.
static char *win_format(const void *fmtp, int wide, u64 va, size_t *outlen) {
    Buf b = {0};
    bput(&b, "", 0);
    #define FCH(i) (wide ? (int)((const u16 *)fmtp)[i] : (int)(u8)((const char *)fmtp)[i])
    #define NEXT() (va += 8, LD64(va - 8))
    for (size_t i = 0; FCH(i); ) {
        if (FCH(i) != '%') {
            if (wide) { char t[8]; u16 w[2] = { (u16)FCH(i), 0 }; size_t n = utf16_to_utf8(w, 1, t, sizeof t); bput(&b, t, n); }
            else { char ch = (char)FCH(i); bput(&b, &ch, 1); }
            i++; continue;
        }
        i++;
        char spec[64]; int sp = 0; spec[sp++] = '%';
        while (strchr("-+ #0", FCH(i)) && FCH(i)) spec[sp++] = (char)FCH(i++);
        if (FCH(i) == '*') { sp += snprintf(spec + sp, 16, "%d", (int)(i32)NEXT()); i++; }
        else while (isdigit(FCH(i))) spec[sp++] = (char)FCH(i++);
        if (FCH(i) == '.') {
            spec[sp++] = FCH(i++);
            if (FCH(i) == '*') { sp += snprintf(spec + sp, 16, "%d", (int)(i32)NEXT()); i++; }
            else while (isdigit(FCH(i))) spec[sp++] = (char)FCH(i++);
        }
        int size = 0;  // 0 default(int=32), 1 hh, 2 h, 8 64-bit, 'l' (32-bit long / wide str), 'w'
        for (;;) {
            int ch = FCH(i);
            if (ch == 'h') { size = size == 2 ? 1 : 2; i++; }
            else if (ch == 'l') { size = size == 'l' ? 8 : 'l'; i++; }
            else if (ch == 'L' || ch == 'q' || ch == 'j' || ch == 'z' || ch == 't') { size = ch == 'L' ? 'L' : 8; i++; }
            else if (ch == 'w') { size = 'w'; i++; }
            else if (ch == 'I') {
                if (FCH(i + 1) == '6' && FCH(i + 2) == '4') { size = 8; i += 3; }
                else if (FCH(i + 1) == '3' && FCH(i + 2) == '2') { size = 0; i += 3; }
                else { size = 8; i++; }
            } else break;
        }
        int conv = FCH(i); if (!conv) break; i++;
        char tmp[512]; int n = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            u64 v = NEXT();
            if (size == 8) { strcpy(spec + sp, "ll"); sp += 2; }
            else if (size == 1) v = (conv == 'd' || conv == 'i') ? (u64)(i64)(i8)v : (u8)v;
            else if (size == 2) v = (conv == 'd' || conv == 'i') ? (u64)(i64)(i16)v : (u16)v;
            else v = (conv == 'd' || conv == 'i') ? (u64)(i64)(i32)v : (u32)v;
            if (size != 8) { strcpy(spec + sp, "ll"); sp += 2; }
            spec[sp++] = (char)conv; spec[sp] = 0;
            n = snprintf(tmp, sizeof tmp, spec, (long long)v);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            u64 bits = NEXT(); double d; memcpy(&d, &bits, 8);
            spec[sp++] = (char)conv; spec[sp] = 0;
            n = snprintf(tmp, sizeof tmp, spec, d);
            break;
        }
        case 'p': n = snprintf(tmp, sizeof tmp, "%016llX", (unsigned long long)NEXT()); break;
        case 'c': case 'C': {
            u64 v = NEXT();
            int w = (conv == 'C') ^ wide; if (size == 'l' || size == 'w') w = 1; if (size == 2) w = 0;
            if (w) { u16 ws[2] = { (u16)v, 0 }; char t[8]; utf16_to_utf8(ws, 1, t, sizeof t); spec[sp++] = 's'; spec[sp] = 0; n = snprintf(tmp, sizeof tmp, spec, t); }
            else { spec[sp++] = 'c'; spec[sp] = 0; n = snprintf(tmp, sizeof tmp, spec, (int)(u8)v); }
            break;
        }
        case 's': case 'S': case 'Z': {
            u64 p = NEXT();
            int w = (conv == 'S') ^ wide; if (size == 'l' || size == 'w') w = 1; if (size == 2) w = 0;
            spec[sp++] = 's'; spec[sp] = 0;
            if (!p) { n = snprintf(tmp, sizeof tmp, spec, "(null)"); break; }
            if (w) {
                size_t need = utf16_to_utf8((const u16 *)p, -1, NULL, 0) + 1;
                char *u = malloc(need); utf16_to_utf8((const u16 *)p, -1, u, need);
                int m = snprintf(NULL, 0, spec, u); char *o = malloc(m + 1); snprintf(o, m + 1, spec, u);
                bput(&b, o, m); free(o); free(u); continue;
            }
            int m = snprintf(NULL, 0, spec, (const char *)p);
            if (m < (int)sizeof tmp) n = snprintf(tmp, sizeof tmp, spec, (const char *)p);
            else { char *o = malloc(m + 1); snprintf(o, m + 1, spec, (const char *)p); bput(&b, o, m); free(o); continue; }
            break;
        }
        case 'n': { u64 p = NEXT(); if (p) ST32(p, (u32)b.n); break; }
        case '%': tmp[0] = '%'; n = 1; break;
        default: tmp[0] = '%'; tmp[1] = (char)conv; n = 2; break;
        }
        if (n > (int)sizeof tmp - 1) n = sizeof tmp - 1;
        bput(&b, tmp, n);
    }
    #undef FCH
    #undef NEXT
    *outlen = b.n;
    return b.p;
}

// ---- stdio ------------------------------------------------------------------
// Guest FILE* values are host FILE* pointers (UCRT FILE is opaque, so the guest never looks inside).
SHIM(__acrt_iob_func) { u32 i = (u32)A1; RET(i == 0 ? stdin : i == 1 ? stdout : stderr); }
// Legacy msvcrt exposes stdin/stdout/stderr as &_iob[0..2], an array of 48-byte FILE structs the guest
// indexes into itself. Hand out a real array and map its entries back to host streams in every FILE shim.
static u8 guest_iob[3 * 48];
FILE *rt_host_file(u64 p);
FILE *rt_host_file(u64 p) {
    if (p >= (u64)guest_iob && p < (u64)guest_iob + sizeof guest_iob) {
        int i = (int)((p - (u64)guest_iob) / 48);
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    return (FILE *)p;
}
SHIM(__iob_func) { RET(guest_iob); }

SHIM(__stdio_common_vfprintf) {  // (options, FILE*, fmt, locale, va_list)
    size_t n; char *s = win_format((void *)A3, 0, AN(5), &n);
    size_t w = fwrite(s, 1, n, rt_host_file(A2)); free(s); RET(w == n ? n : (u64)-1);
}
SHIM(__stdio_common_vfwprintf) {
    size_t n; char *s = win_format((void *)A3, 1, AN(5), &n);
    fwrite(s, 1, n, rt_host_file(A2)); free(s); RET(n);
}
SHIM(__stdio_common_vsprintf) {  // (options, buf, count, fmt, locale, va_list)
    size_t n; char *s = win_format((void *)A4, 0, AN(6), &n);
    char *buf = (char *)A2; size_t cnt = A3; u64 opts = A1;
    if (!buf || !cnt) { free(s); RET(n); }
    int standard = (opts & 2) != 0;  // _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR
    if (n < cnt) { memcpy(buf, s, n + 1); free(s); RET(n); }
    memcpy(buf, s, cnt - 1); buf[cnt - 1] = 0; free(s);
    RET(standard ? n : (u64)-1);
}
SHIM(__stdio_common_vsnprintf_s) {  // (options, buf, size, count, fmt, locale, va_list)
    size_t n; char *s = win_format((void *)AN(5), 0, AN(7), &n);
    char *buf = (char *)A2; size_t sz = A3;
    if (n < sz) { memcpy(buf, s, n + 1); free(s); RET(n); }
    if (sz) { memcpy(buf, s, sz - 1); buf[sz - 1] = 0; }
    free(s); RET((u64)-1);
}
SHIM(__stdio_common_vsprintf_s) {  // (options, buf, size, fmt, locale, va_list)
    size_t n; char *s = win_format((void *)A4, 0, AN(6), &n);
    char *buf = (char *)A2; size_t sz = A3;
    if (n < sz) { memcpy(buf, s, n + 1); free(s); RET(n); }
    if (sz) buf[0] = 0;
    free(s); RET((u64)-1);
}
// Legacy msvcrt.dll varargs entry points: varargs start in the register/home slots.
static u64 spill_varargs(Ctx *c, int fixed) {
    // Home space at [rsp+8..]: the caller allocated it; spill the register args there so that
    // all args are contiguous 8-byte slots starting at [rsp+8].
    u64 sp = c->r[RSP];
    ST64(sp + 8, c->r[RCX]); ST64(sp + 16, c->r[RDX]); ST64(sp + 24, c->r[R8]); ST64(sp + 32, c->r[R9]);
    return sp + 8 + 8ull * fixed;
}
SHIM(printf) { size_t n; char *s = win_format((void *)A1, 0, spill_varargs(c, 1), &n); fwrite(s, 1, n, stdout); free(s); RET(n); }
SHIM(fprintf) { u64 f = A1, fmt = A2; size_t n; char *s = win_format((void *)fmt, 0, spill_varargs(c, 2), &n); fwrite(s, 1, n, rt_host_file(f)); free(s); RET(n); }
SHIM(sprintf) { u64 buf = A1, fmt = A2; size_t n; char *s = win_format((void *)fmt, 0, spill_varargs(c, 2), &n); memcpy((void *)buf, s, n + 1); free(s); RET(n); }
SHIM(vfprintf) { size_t n; char *s = win_format((void *)A2, 0, A3, &n); fwrite(s, 1, n, rt_host_file(A1)); free(s); RET(n); }
SHIM(vprintf) { size_t n; char *s = win_format((void *)A1, 0, A2, &n); fwrite(s, 1, n, stdout); free(s); RET(n); }

// msvcrt _vsnprintf: no terminator when the output exactly fills the buffer; -1 when it doesn't fit.
SHIM(_vsnprintf) {  // (buf, count, fmt, va)
    size_t n; char *s = win_format((void *)A3, 0, A4, &n); size_t cnt = A2;
    memcpy((void *)A1, s, n < cnt ? n + 1 : cnt); free(s);
    RET(n <= cnt ? n : (u64)-1);
}
static size_t utf8_to_utf16(const char *s, size_t n, u64 out, size_t cap) {  // returns units needed
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        u8 b = (u8)s[i]; u32 cp; int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : 4;
        cp = len == 1 ? b : len == 2 ? b & 31 : len == 3 ? b & 15 : b & 7;
        for (int k = 1; k < len && i + k < n; k++) cp = (cp << 6) | ((u8)s[i + k] & 63);
        i += len;
        if (cp >= 0x10000) { if (o + 1 < cap) { cp -= 0x10000; ST16(out + 2 * o, (u16)(0xD800 | cp >> 10)); ST16(out + 2 * o + 2, (u16)(0xDC00 | (cp & 0x3FF))); } o += 2; }
        else { if (o < cap) ST16(out + 2 * o, (u16)cp); o++; }
    }
    return o;
}
SHIM(_vsnwprintf) {  // (wbuf, count in wchars, wfmt, va)
    size_t n; char *s = win_format((void *)A3, 1, A4, &n); size_t cnt = A2;
    size_t units = utf8_to_utf16(s, n, A1, cnt); free(s);
    if (units < cnt) ST16(A1 + 2 * units, 0);
    RET(units <= cnt ? units : (u64)-1);
}

// For crt2.c: the Windows printf engine (fmt narrow or wide; va = Win64 va_list) and the scanf engine on a string.
char *rt_win_format(const void *fmt, int wide, u64 va, size_t *n) { return win_format(fmt, wide, va, n); }
SHIM(puts) { RET(puts((const char *)A1)); }
SHIM(putchar) { RET(putchar((int)A1)); }
SHIM(fputs) { RET(fputs((const char *)A1, rt_host_file(A2))); }
SHIM(fputc) { RET(fputc((int)A1, rt_host_file(A2))); }
SHIM(fwrite) { RET(fwrite((void *)A1, A2, A3, rt_host_file(A4))); }
SHIM(fread) { RET(fread((void *)A1, A2, A3, rt_host_file(A4))); }
SHIM(fflush) { RET(fflush(rt_host_file(A1))); }
SHIM(setvbuf) { RET(setvbuf(rt_host_file(A1), (char *)A2, (int)A3 == 0 ? _IOFBF : (int)A3 == 4 ? _IONBF : _IOLBF, A4)); }
SHIM(fclose) { RET(fclose(rt_host_file(A1))); }
SHIM(fseek) { RET(fseek(rt_host_file(A1), (long)(i32)A2, (int)A3)); }
SHIM(_fseeki64) { RET(fseeko(rt_host_file(A1), (off_t)A2, (int)A3)); }
SHIM(ftell) { RET((u32)ftell(rt_host_file(A1))); }
SHIM(_ftelli64) { RET(ftello(rt_host_file(A1))); }
SHIM(fgets) { RET(fgets((char *)A1, (int)A2, rt_host_file(A3))); }
SHIM(fgetc) { RET((u32)fgetc(rt_host_file(A1))); }
SHIM(feof) { RET(feof(rt_host_file(A1))); }
SHIM(ferror) { RET(ferror(rt_host_file(A1))); }
SHIM(_fileno) { RET(fileno(rt_host_file(A1))); }

// ---- heap / memory / strings: identity-mapped, so host libc is ABI compatible ------
SHIM(malloc) { RET(malloc(A1)); }
SHIM(calloc) { RET(calloc(A1, A2)); }
SHIM(realloc) { RET(realloc((void *)A1, A2)); }
SHIM(free) {   // WINTOMAC_HEAP_TRACE: see runtime/kernel32.c
    extern FILE *rt_heap_trace_f;
    if (__builtin_expect(rt_heap_trace_f != NULL, 0)) fprintf(rt_heap_trace_f, "f %p %llx\n", (void *)A1, (unsigned long long)LD64(c->r[RSP]));
    free((void *)A1); RET(0);
}
SHIM(_aligned_malloc) { void *p = 0; if (posix_memalign(&p, A2 < 8 ? 8 : A2, A1)) p = 0; RET(p); }
SHIM(_aligned_free) { free((void *)A1); RET(0); }
SHIM(_msize) { extern size_t malloc_size(const void *); RET(malloc_size((void *)A1)); }
SHIM(memcpy) { RET(memcpy((void *)A1, (void *)A2, A3)); }
SHIM(memmove) { RET(memmove((void *)A1, (void *)A2, A3)); }
SHIM(memset) { RET(memset((void *)A1, (int)A2, A3)); }
SHIM(memcmp) { RET((u32)memcmp((void *)A1, (void *)A2, A3)); }
SHIM(memchr) { RET(memchr((void *)A1, (int)A2, A3)); }
SHIM(strlen) { RET(strlen((char *)A1)); }
SHIM(strnlen) { RET(strnlen((char *)A1, A2)); }
SHIM(strcmp) { RET((u32)strcmp((char *)A1, (char *)A2)); }
SHIM(strncmp) { RET((u32)strncmp((char *)A1, (char *)A2, A3)); }
SHIM(_stricmp) { RET((u32)strcasecmp((char *)A1, (char *)A2)); }
SHIM(_strnicmp) { RET((u32)strncasecmp((char *)A1, (char *)A2, A3)); }
SHIM(strcpy) { RET(strcpy((char *)A1, (char *)A2)); }
SHIM(strncpy) { RET(strncpy((char *)A1, (char *)A2, A3)); }
SHIM(strcat) { RET(strcat((char *)A1, (char *)A2)); }
SHIM(strchr) { RET(strchr((char *)A1, (int)A2)); }
SHIM(strrchr) { RET(strrchr((char *)A1, (int)A2)); }
SHIM(strstr) { RET(strstr((char *)A1, (char *)A2)); }
SHIM(_strdup) { RET(strdup((char *)A1)); }
SHIM(strtol) { RET((u32)(i32)strtol((char *)A1, (char **)A2, (int)A3)); }
SHIM(strtoul) { RET((u32)strtoul((char *)A1, (char **)A2, (int)A3)); }
SHIM(_strtoi64) { RET(strtoll((char *)A1, (char **)A2, (int)A3)); }
SHIM(_strtoui64) { RET(strtoull((char *)A1, (char **)A2, (int)A3)); }
SHIM(atoi) { RET((u32)atoi((char *)A1)); }
SHIM(strtod) { double d = strtod((char *)A1, (char **)A2); memcpy(&c->x[0], &d, 8); RET(0); }
SHIM(atof) { double d = atof((char *)A1); memcpy(&c->x[0], &d, 8); RET(0); }
SHIM(wcslen) { const u16 *w = (const u16 *)A1; u64 n = 0; while (w[n]) n++; RET(n); }
SHIM(tolower) { RET((u32)tolower((int)A1)); }
SHIM(toupper) { RET((u32)toupper((int)A1)); }

// ---- math: doubles arrive in xmm0/xmm1, result in xmm0 --------------------------
#define MATH1(n) SHIM(n) { double r = n(c->x[0].fd[0]); c->x[0].fd[0] = r; c->x[0].fd[1] = 0; RET(0); }
#define MATH2(n) SHIM(n) { double r = n(c->x[0].fd[0], c->x[1].fd[0]); c->x[0].fd[0] = r; RET(0); }
#define MATH1F(n) SHIM(n) { float r = n(c->x[0].f[0]); c->x[0].d[0] = 0; c->x[0].f[0] = r; RET(0); }
#define MATH2F(n) SHIM(n) { float r = n(c->x[0].f[0], c->x[1].f[0]); c->x[0].f[0] = r; RET(0); }
MATH1(sin) MATH1(cos) MATH1(tan) MATH1(asin) MATH1(acos) MATH1(atan) MATH1(exp) MATH1(log) MATH1(log10)
MATH1(sqrt) MATH1(floor) MATH1(ceil) MATH1(round) MATH1(trunc) MATH1(log2) MATH1(exp2) MATH1(sinh) MATH1(cosh) MATH1(tanh)
MATH2(atan2) MATH2(pow) MATH2(fmod) MATH2(hypot)
MATH1F(sinf) MATH1F(cosf) MATH1F(tanf) MATH1F(asinf) MATH1F(acosf) MATH1F(atanf) MATH1F(expf) MATH1F(logf)
MATH1F(sqrtf) MATH1F(floorf) MATH1F(ceilf) MATH1F(roundf) MATH1F(truncf) MATH1F(log10f) MATH1F(log2f) MATH1F(exp2f)
MATH2F(atan2f) MATH2F(powf) MATH2F(fmodf)
SHIM(abs) { RET((u32)abs((int)A1)); }
SHIM(labs) { RET((u32)abs((int)A1)); }
SHIM(llabs) { RET(llabs((long long)A1)); }
SHIM(rand) { RET((u32)(rand() & 0x7fff)); }   // Windows RAND_MAX is 0x7fff
SHIM(srand) { srand((unsigned)A1); RET(0); }

// ---- qsort / bsearch with guest comparators ----------------------------------------
typedef struct { Ctx *c; u64 fn; } GuestCmp;
static int cmp_thunk(void *ctx, const void *a, const void *b) {
    GuestCmp *g = ctx; u64 args[2] = { (u64)a, (u64)b };
    return (int)(i32)rt_guest_call(g->c, g->fn, 2, args);
}
SHIM(qsort) { GuestCmp g = { c, A4 }; qsort_r((void *)A1, A2, A3, &g, cmp_thunk); RET(0); }
SHIM(bsearch) {
    GuestCmp g = { c, AN(5) }; u8 *base = (u8 *)A2; size_t lo = 0, hi = A3, sz = A4;
    while (lo < hi) { size_t mid = (lo + hi) / 2; int r = cmp_thunk(&g, (void *)A1, base + mid * sz);
        if (!r) RET(base + mid * sz); if (r < 0) hi = mid; else lo = mid + 1; }
    RET(0);
}

// ---- process / startup -----------------------------------------------------------
static int g_argc; static u64 g_argv, g_environ; static i32 g_fmode, g_commode;
static u64 atexit_fns[256]; static int natexit;

static void build_args(void) {
    if (g_argv) return;
    g_argc = rt_argc;
    char **av = calloc(rt_argc + 1, sizeof(char *));
    for (int i = 0; i < rt_argc; i++) av[i] = rt_argv[i];
    g_argv = (u64)av;
    extern char **environ; g_environ = (u64)environ;
}
static void bind_caller_data(Ctx *c);
SHIM(__p___argc) { build_args(); RET(&g_argc); }
SHIM(__p___argv) { build_args(); RET(&g_argv); }
SHIM(__p__acmdln) { extern char *rt_cmdline_a(void); static char *p; if (!p) p = rt_cmdline_a(); RET(&p); }
SHIM(strtok) { static _Thread_local char *save; RET(strtok_r(A1 ? (char *)A1 : NULL, (const char *)A2, &save)); }
SHIM(__p__environ) { build_args(); RET(&g_environ); }
SHIM(__p__fmode) { RET(&g_fmode); }
SHIM(__p__commode) { RET(&g_commode); }
SHIM(__getmainargs) { bind_caller_data(c); build_args(); ST32(A1, g_argc); ST64(A2, g_argv); ST64(A3, g_environ); RET(0); }
SHIM(_configure_narrow_argv) { RET(0); }
// Data exports of msvcrt (_acmdln, _fmode, _commode, __initenv, _iob, ...): the guest reads the variable through
// its IAT slot, so the slot must hold the variable's address, not a code thunk. The loader binds every import to
// a thunk; rt_bind_data_imports() rewrites the slots of these names in a module's IAT. It runs on the CRT
// startup calls that precede any use (__set_app_type, _initterm[_e], __getmainargs), once per module.
static char *g_acmdln; static u64 g_initenv; static i32 g_app_type;
static u64 data_import(const char *n) {
    build_args();
    if (!g_acmdln) { extern char *rt_cmdline_a(void); g_acmdln = rt_cmdline_a(); }
    if (!g_initenv) g_initenv = g_environ;
    if (!strcmp(n, "_acmdln")) return (u64)&g_acmdln;
    if (!strcmp(n, "_fmode")) return (u64)&g_fmode;
    if (!strcmp(n, "_commode")) return (u64)&g_commode;
    if (!strcmp(n, "__initenv")) return (u64)&g_initenv;
    if (!strcmp(n, "_environ")) return (u64)&g_environ;
    if (!strcmp(n, "__argc")) return (u64)&g_argc;
    if (!strcmp(n, "__argv")) return (u64)&g_argv;
    if (!strcmp(n, "_iob")) return (u64)guest_iob;
    if (!strcmp(n, "_timezone") || !strcmp(n, "_daylight") || !strcmp(n, "_tzname")) {   // msvcrt time-zone globals
        static i32 tzv, dlv; static char tzn[2][64]; static u64 tzp[2];
        tzset(); tzv = (i32)timezone; dlv = daylight;
        snprintf(tzn[0], 64, "%s", tzname[0]); snprintf(tzn[1], 64, "%s", tzname[1]); tzp[0] = (u64)tzn[0]; tzp[1] = (u64)tzn[1];
        return !strcmp(n, "_timezone") ? (u64)&tzv : !strcmp(n, "_daylight") ? (u64)&dlv : (u64)tzp;
    }
    return 0;
}
void rt_bind_data_imports(u64 base) {
    static u64 done[64]; static int ndone; static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    if (!base || LD16(base) != 0x5A4D) return;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < ndone; i++) if (done[i] == base) { pthread_mutex_unlock(&lk); return; }
    if (ndone < 64) done[ndone++] = base;
    u32 lf = LD32(base + 0x3C);
    if (LD32(base + lf) != 0x4550 || LD16(base + lf + 24) != 0x20B) { pthread_mutex_unlock(&lk); return; }
    u32 rva = LD32(base + lf + 24 + 120);   // import directory (data directory 1)
    for (u64 d = base + rva; rva && LD32(d + 12); d += 20) {
        const char *dll = (const char *)(base + LD32(d + 12));
        if (strncasecmp(dll, "msvcr", 5) && strncasecmp(dll, "ucrtbase", 8) && strncasecmp(dll, "api-ms-win-crt", 14)) continue;
        u32 ilt = LD32(d), iat = LD32(d + 16);
        if (!ilt) continue;   // no hint/name table left to read names from
        for (u64 k = 0; LD64(base + ilt + 8 * k); k++) {
            u64 e = LD64(base + ilt + 8 * k);
            if (e >> 63) continue;
            u64 v = data_import((const char *)(base + (e & 0x7FFFFFFF) + 2));
            if (v) ST64(base + iat + 8 * k, v);
        }
    }
    pthread_mutex_unlock(&lk);
}
static void bind_caller_data(Ctx *c) { u64 sz; rt_bind_data_imports(rt_module_base_for(LD64(c->r[RSP]), &sz)); rt_bind_data_imports(rt_module_handle(NULL)); }
SHIM(__set_app_type) { g_app_type = (i32)A1; bind_caller_data(c); RET(0); }
SHIM(__lconv_init) { RET(0); }
SHIM(msvcrt_data_import) { rt_log("msvcrt data import called as a function"); _exit(3); }
SHIM(_initialize_narrow_environment) { RET(0); }
SHIM(_set_app_type) { RET(0); }
SHIM(_set_new_mode) { RET(0); }
SHIM(_configthreadlocale) { RET(0); }
SHIM(__setusermatherr) { RET(0); }
SHIM(_set_invalid_parameter_handler) { RET(0); }
SHIM(_set_fmode) { g_fmode = (i32)A1; RET(0); }
SHIM(signal) { RET(0); }
__thread int rt_crt_errno;   // the guest's errno (crt2.c sets it too)
SHIM(_errno) { RET(&rt_crt_errno); }

SHIM(_initterm) {
    bind_caller_data(c);
    for (u64 p = A1; p < A2; p += 8) { u64 fn = LD64(p); if (fn) rt_guest_call(c, fn, 0, NULL); }
    RET(0);
}
SHIM(_initterm_e) {
    bind_caller_data(c);
    for (u64 p = A1; p < A2; p += 8) { u64 fn = LD64(p); if (fn) { u64 r = (u32)rt_guest_call(c, fn, 0, NULL); if (r) RET(r); } }
    RET(0);
}
SHIM(_crt_atexit) { if (natexit < 256) atexit_fns[natexit++] = A1; RET(0); }
SHIM(atexit) { if (natexit < 256) atexit_fns[natexit++] = A1; RET(0); }
SHIM(_onexit) { if (natexit < 256) atexit_fns[natexit++] = A1; RET(A1); }
SHIM(_register_onexit_function) { if (natexit < 256) atexit_fns[natexit++] = A2; RET(0); }
SHIM(_initialize_onexit_table) { RET(0); }
SHIM(_execute_onexit_table) { RET(0); }

static void run_atexit(Ctx *c) {
    while (natexit > 0) { u64 fn = atexit_fns[--natexit]; rt_guest_call(c, fn, 0, NULL); }
    fflush(NULL);
}
SHIM(exit) { int code = (int)A1; run_atexit(c); _exit(code); }
SHIM(_exit) { fflush(NULL); _exit((int)A1); }
SHIM(_cexit) { run_atexit(c); RET(0); }
SHIM(_c_exit) { RET(0); }
SHIM(abort) { fflush(NULL); rt_log("guest called abort()"); if (getenv("WINTOMAC_ABORT_STACK")) rt_guest_stack(c); _exit(3); }
SHIM(_amsg_exit) { rt_log("guest _amsg_exit(%d)", (int)A1); _exit(255); }

// Windows wchar_t is 16-bit: wide conversions are done by hand ("C" locale: bytes <-> code units).
SHIM(mbrtowc) { if (!A2) RET(0); u8 ch = LD8(A2); if (A1) ST16(A1, ch); RET(ch ? 1 : 0); }
SHIM(wcrtomb) { if (!A1) RET(1); u16 w = (u16)A2; ST8(A1, w < 256 ? (u8)w : '?'); RET(1); }
SHIM(mbsrtowcs) {  // (dst, const char **src, n, state)
    u64 src = LD64(A2); size_t i = 0;
    for (; !A1 || i < A3; i++) { u8 ch = LD8(src + i); if (A1) ST16(A1 + 2 * i, ch); if (!ch) { if (A1) ST64(A2, 0); RET(i); } }
    if (A1) ST64(A2, src + i); RET(i);
}
SHIM(wcsnlen) { const u16 *w = (const u16 *)A1; u64 n = 0; while (n < A2 && w[n]) n++; RET(n); }
SHIM(getenv) { RET(getenv((const char *)A1)); }
// UCRT stream state for code that reads FILE internals: AnyPS5's libc (docs/ps5.md) checks
// ((__crt_stdio_stream_data *)f)->_flags (offset 0x14: _IOREAD 1, _IOWRITE 2) under _lock_file before reading or
// writing. In Apple's struct __sFILE bytes 0x14-0x17 are alignment padding before _bf, so the equivalent flags are
// written there on every lock (update streams report read|write, so the guest never resets _ptr/_cnt itself).
static void ucrt_stamp_flags(u64 p, FILE *f) {
    if ((u64)f != p || !f) return;   // legacy msvcrt _iob entries are not host FILEs
    short fl = f->_flags;
    u32 u = (fl & __SRW) ? 3 : (fl & __SRD) ? 1 : (fl & __SWR) ? 2 : 0;
    memcpy((char *)f + 0x14, &u, 4);
}
SHIM(_lock_file) { FILE *f = rt_host_file(A1); flockfile(f); ucrt_stamp_flags(A1, f); RET(0); }
SHIM(_unlock_file) { funlockfile(rt_host_file(A1)); RET(0); }
SHIM(localeconv) {  // Windows struct lconv: 10 char* then chars, then wchar_t* fields
    static u64 lc[24]; static u8 init;
    if (!init) { static char dp[] = ".", empty[] = ""; lc[0] = (u64)dp; for (int i = 1; i < 10; i++) lc[i] = (u64)empty;
                 memset(&lc[10], 127, 8); static u16 wdp[] = { '.', 0 }, wempty[] = { 0 }; lc[11] = (u64)wdp; for (int i = 12; i < 21; i++) lc[i] = (u64)wempty; init = 1; }
    RET(lc);
}
SHIM(_assert) { fprintf(stderr, "Assertion failed: %s, file %s, line %u\n", (char *)A1, (char *)A2, (u32)A3); _exit(3); }
SHIM(_wassert) { fprintf(stderr, "Assertion failed (wide) line %u\n", (u32)A3); _exit(3); }
SHIM(strerror) { RET(strerror((int)A1)); }
SHIM(_read) { RET((u32)read((int)A1, (void *)A2, (u32)A3)); }
SHIM(_write) { RET((u32)write((int)A1, (void *)A2, (u32)A3)); }
SHIM(_setmode) { RET(0x4000); }
SHIM(rand_s) { ST32(A1, arc4random()); RET(0); }
SHIM(__intrinsic_setjmp) { RET(0); }   // first return; longjmp support needs lifter cooperation (see DESIGN.md)
SHIM(_setjmp) { RET(0); }
SHIM(longjmp) { rt_log("longjmp: not supported yet"); _exit(76); }
SHIM(_isatty) { RET(isatty((int)A1)); }
// _get_osfhandle: a file HANDLE for an open CRT descriptor (one per descriptor, cached); -1 for a closed one.
SHIM(_get_osfhandle) {
    static u64 osf[1024]; int fd = (int)A1;
    if (fd < 0 || fd >= 1024 || fcntl(fd, F_GETFD) < 0) RET((u64)-1);
    if (!osf[fd]) osf[fd] = rt_handle_new(H_FILE, fd);
    RET(osf[fd]);
}
// ---- scanf engine (Windows varargs = 8-byte slots of destination pointers) ----------------------------
typedef struct { const char *s; FILE *f; long n; int eof; } ScanIn;
static int sc_get(ScanIn *in) {
    int ch;
    if (in->f) ch = fgetc(in->f); else ch = (in->n == 0 || !*in->s) ? EOF : (u8)*in->s++;
    if (ch == EOF) in->eof = 1; else if (in->n > 0) in->n--;
    return ch;
}
static void sc_unget(ScanIn *in, int ch) {
    if (ch == EOF) return;
    if (in->f) ungetc(ch, in->f); else in->s--;
    if (in->n >= 0) in->n++;
    in->eof = 0;
}
static int win_scan(ScanIn *in, const char *fmt, u64 ap) {
    int assigned = 0, any_input = 0;
    #define NEXT_PTR() (ap += 8, LD64(ap - 8))
    while (*fmt) {
        if (isspace((u8)*fmt)) { while (isspace((u8)*fmt)) fmt++; int ch; while (isspace(ch = sc_get(in))) {} sc_unget(in, ch); continue; }
        if (*fmt != '%') { int ch = sc_get(in); if (ch != (u8)*fmt) { sc_unget(in, ch); break; } fmt++; any_input = 1; continue; }
        fmt++;
        if (*fmt == '%') { int ch; while (isspace(ch = sc_get(in))) {} if (ch != '%') { sc_unget(in, ch); break; } fmt++; continue; }
        int skip = 0, width = 0, size = 0;   // size: 1 hh, 2 h, 4 int/l, 8 ll/I64/z/j/t, 'L' long double, 'w' wide
        if (*fmt == '*') { skip = 1; fmt++; }
        while (isdigit((u8)*fmt)) width = width * 10 + (*fmt++ - '0');
        for (;;) {
            if (*fmt == 'h') { size = size == 2 ? 1 : 2; fmt++; }
            else if (*fmt == 'l') { size = size == 'l' ? 8 : 'l'; fmt++; }
            else if (*fmt == 'L') { size = 'L'; fmt++; }
            else if (*fmt == 'j' || *fmt == 'z' || *fmt == 't') { size = 8; fmt++; }
            else if (*fmt == 'w') { size = 'w'; fmt++; }
            else if (*fmt == 'I' && fmt[1] == '6' && fmt[2] == '4') { size = 8; fmt += 3; }
            else if (*fmt == 'I' && fmt[1] == '3' && fmt[2] == '2') { size = 4; fmt += 3; }
            else if (*fmt == 'I') { size = 8; fmt++; }
            else break;
        }
        char conv = *fmt++;
        if (!conv) break;
        if (conv == 'n') { if (!skip) { u64 p = NEXT_PTR(); (void)p; } continue; }   // count tracking omitted: rarely used
        if (conv != 'c' && conv != '[') { int ch; while (isspace(ch = sc_get(in))) {} sc_unget(in, ch); }
        char buf[512]; int k = 0, lim = width ? width : (int)sizeof buf - 1;
        if (lim > (int)sizeof buf - 1) lim = (int)sizeof buf - 1;
        if (conv == 'c') {
            int cnt = width ? width : 1; u64 dst = skip ? 0 : NEXT_PTR(); int got = 0;
            for (; got < cnt; got++) { int ch = sc_get(in); if (ch == EOF) break; if (dst) { if (size == 'l' || size == 'w') ST16(dst + 2 * got, (u16)ch); else ST8(dst + got, (u8)ch); } }
            if (!got) break;
            any_input = 1; if (!skip) assigned++;
            continue;
        }
        if (conv == 's' || conv == '[') {
            u8 set[256] = { 0 }; int neg = 0;
            if (conv == '[') {
                if (*fmt == '^') { neg = 1; fmt++; }
                if (*fmt == ']') { set[']'] = 1; fmt++; }
                while (*fmt && *fmt != ']') {
                    if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') { for (int c2 = (u8)fmt[0]; c2 <= (u8)fmt[2]; c2++) set[c2] = 1; fmt += 3; }
                    else set[(u8)*fmt++] = 1;
                }
                if (*fmt == ']') fmt++;
            }
            int ch;
            while (k < lim && (ch = sc_get(in)) != EOF) {
                int ok = conv == 's' ? !isspace(ch) : (set[ch] ^ neg);
                if (!ok) { sc_unget(in, ch); break; }
                buf[k++] = (char)ch;
            }
            if (!k) break;
            buf[k] = 0; any_input = 1;
            if (!skip) {
                u64 dst = NEXT_PTR();
                if (size == 'l' || size == 'w') { for (int i = 0; i <= k; i++) ST16(dst + 2 * i, (u8)buf[i]); } else memcpy((void *)dst, buf, (size_t)k + 1);
                assigned++;
            }
            continue;
        }
        // numbers: collect a plausible token, then convert with the host's strtoX
        int ch, isfloat = strchr("eEfgGaA", conv) != NULL;
        while (k < lim && (ch = sc_get(in)) != EOF) {
            int ok = isalnum(ch) || ((ch == '+' || ch == '-') && (k == 0 || (isfloat && (buf[k - 1] == 'e' || buf[k - 1] == 'E' || buf[k - 1] == 'p' || buf[k - 1] == 'P')))) || (isfloat && ch == '.');
            if (!isfloat && !strchr("xX", conv) && conv != 'i' && conv != 'p' && isalpha(ch)) ok = 0;
            if ((conv == 'x' || conv == 'X' || conv == 'p' || conv == 'i') && isalpha(ch) && !isxdigit(ch) && !(k > 0 && buf[k - 1] == '0' && (ch == 'x' || ch == 'X'))) ok = 0;
            if (!ok) { sc_unget(in, ch); break; }
            buf[k++] = (char)ch;
        }
        buf[k] = 0;
        char *end = buf;
        if (isfloat) {
            double d = strtod(buf, &end);
            if (end == buf) { for (char *q = buf + k; q > end; ) sc_unget(in, (u8)*--q); break; }
            for (char *q = buf + k; q > end; ) sc_unget(in, (u8)*--q);
            any_input = 1;
            if (!skip) { u64 dst = NEXT_PTR(); if (size == 'l' || size == 'L') memcpy((void *)dst, &d, 8); else { float f = (float)d; memcpy((void *)dst, &f, 4); } assigned++; }
            continue;
        }
        int base = conv == 'd' || conv == 'u' ? 10 : conv == 'o' ? 8 : conv == 'i' ? 0 : 16;
        u64 v = (conv == 'd' || conv == 'i') ? (u64)strtoll(buf, &end, base) : strtoull(buf, &end, base);
        if (end == buf) { for (char *q = buf + k; q > end; ) sc_unget(in, (u8)*--q); break; }
        for (char *q = buf + k; q > end; ) sc_unget(in, (u8)*--q);
        any_input = 1;
        if (!skip) {
            u64 dst = NEXT_PTR();
            if (conv == 'p' || size == 8) ST64(dst, v); else if (size == 1) ST8(dst, (u8)v); else if (size == 2) ST16(dst, (u16)v); else ST32(dst, (u32)v);
            assigned++;
        }
    }
    #undef NEXT_PTR
    return (!assigned && !any_input && in->eof) ? -1 : assigned;
}
int rt_win_sscanf(const char *buf, const char *fmt, u64 va) { ScanIn in = { buf, NULL, -1, 0 }; return win_scan(&in, fmt, va); }
SHIM(__stdio_common_vsscanf) {  // (options, buffer, count, format, locale, va_list)
    ScanIn in = { (const char *)A2, NULL, (long)(i64)A3 == -1 ? -1 : (long)A3, 0 };
    RET((u32)win_scan(&in, (const char *)A4, AN(6)));
}
SHIM(__stdio_common_vfscanf) {  // (options, FILE*, format, locale, va_list)
    ScanIn in = { NULL, rt_host_file(A2), -1, 0 };
    RET((u32)win_scan(&in, (const char *)A3, AN(5)));
}
SHIM(sscanf) { u64 buf = A1, fmt = A2; ScanIn in = { (const char *)buf, NULL, -1, 0 }; RET((u32)win_scan(&in, (const char *)fmt, spill_varargs(c, 2))); }
SHIM(fscanf) { u64 f = A1, fmt = A2; ScanIn in = { NULL, rt_host_file(f), -1, 0 }; RET((u32)win_scan(&in, (const char *)fmt, spill_varargs(c, 2))); }
SHIM(vsscanf) { ScanIn in = { (const char *)A1, NULL, -1, 0 }; RET((u32)win_scan(&in, (const char *)A2, A3)); }

// ---- assorted CRT --------------------------------------------------------------------------------
SHIM(clearerr) { clearerr(rt_host_file(A1)); RET(0); }
static int wcmp(u64 a, u64 b, u64 n, int ci) {
    for (u64 i = 0; i < n; i++) {
        u32 x = LD16(a + 2 * i), y = LD16(b + 2 * i);
        if (ci) { if (x < 128) x = (u32)tolower((int)x); if (y < 128) y = (u32)tolower((int)y); }
        if (x != y || !x) return (int)x - (int)y;
    }
    return 0;
}
SHIM(_wcsicmp) { RET((u32)wcmp(A1, A2, ~0ull, 1)); }
SHIM(_wcsnicmp) { RET((u32)wcmp(A1, A2, A3, 1)); }
SHIM(wcscmp) { RET((u32)wcmp(A1, A2, ~0ull, 0)); }
SHIM(wcsncmp) { RET((u32)wcmp(A1, A2, A3, 0)); }
SHIM(wcscpy_s) {  // (dst, n, src) -> errno_t
    u64 n = 0; while (LD16(A3 + 2 * n)) n++;
    if (!A1 || !A2) RET(22);
    if (n >= A2) { ST16(A1, 0); RET(34); }   // ERANGE
    memmove((void *)A1, (void *)A3, 2 * (n + 1)); RET(0);
}
SHIM(wcscpy) { u64 n = 0; while (LD16(A2 + 2 * n)) n++; memmove((void *)A1, (void *)A2, 2 * (n + 1)); RET(A1); }
SHIM(strcspn) { RET(strcspn((char *)A1, (char *)A2)); }
SHIM(strspn) { RET(strspn((char *)A1, (char *)A2)); }
SHIM(strpbrk) { RET(strpbrk((char *)A1, (char *)A2)); }
SHIM(strncat) { RET(strncat((char *)A1, (char *)A2, A3)); }
SHIM(strtoll) { RET(strtoll((char *)A1, (char **)A2, (int)A3)); }
SHIM(strtoull) { RET(strtoull((char *)A1, (char **)A2, (int)A3)); }
SHIM(isprint) { RET((u32)isprint((int)(i32)A1)); }
SHIM(isspace) { RET((u32)isspace((int)(i32)A1)); }
SHIM(isdigit) { RET((u32)isdigit((int)(i32)A1)); }
SHIM(isalpha) { RET((u32)isalpha((int)(i32)A1)); }
SHIM(isalnum) { RET((u32)isalnum((int)(i32)A1)); }
SHIM(isxdigit) { RET((u32)isxdigit((int)(i32)A1)); }
SHIM(isupper) { RET((u32)isupper((int)(i32)A1)); }
SHIM(islower) { RET((u32)islower((int)(i32)A1)); }
SHIM(ispunct) { RET((u32)ispunct((int)(i32)A1)); }
SHIM(frexp) { int e; double r = frexp(c->x[0].fd[0], &e); if (A2) ST32(A2, (u32)e); c->x[0].fd[0] = r; RET(0); }   // (double, int*)
SHIM(ldexp) { double r = ldexp(c->x[0].fd[0], (int)(i32)A2); c->x[0].fd[0] = r; RET(0); }                  // (double, int)
SHIM(copysign) { double r = copysign(c->x[0].fd[0], c->x[1].fd[0]); c->x[0].fd[0] = r; RET(0); }
SHIM(copysignf) { float r = copysignf(c->x[0].f[0], c->x[1].f[0]); c->x[0].f[0] = r; RET(0); }
SHIM(fabs) { double r = fabs(c->x[0].fd[0]); c->x[0].fd[0] = r; RET(0); }
// Windows struct tm == 9 ints, same order as the host's first 9 fields
static __thread u32 win_tm[9];
static u64 put_tm(struct tm *t) {
    if (!t) return 0;
    int v[9] = { t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon, t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst };
    memcpy(win_tm, v, sizeof v); return (u64)win_tm;
}
SHIM(_gmtime64) { time_t t = (time_t)(i64)LD64(A1); struct tm r; RET(put_tm(gmtime_r(&t, &r))); }
SHIM(_localtime64) { time_t t = (time_t)(i64)LD64(A1); struct tm r; RET(put_tm(localtime_r(&t, &r))); }
SHIM(_time64) { time_t t = time(NULL); if (A1) ST64(A1, (u64)t); RET((u64)t); }
SHIM(time) { sh__time64(c); }
SHIM(_mktime64) {
    struct tm t = { 0 }; int v[9]; memcpy(v, (void *)A1, sizeof v);
    t.tm_sec = v[0]; t.tm_min = v[1]; t.tm_hour = v[2]; t.tm_mday = v[3]; t.tm_mon = v[4]; t.tm_year = v[5]; t.tm_isdst = v[8];
    RET((u64)mktime(&t));
}
SHIM(_putenv) { char *e = strdup((const char *)A1); char *eq = strchr(e, '='); if (!eq) { free(e); RET((u64)-1); } *eq = 0;
                if (eq[1]) setenv(e, eq + 1, 1); else unsetenv(e); free(e); RET(0); }
SHIM(_putenv_s) { if (*(const char *)A2) setenv((const char *)A1, (const char *)A2, 1); else unsetenv((const char *)A1); RET(0); }
SHIM(_wremove) { char w[2048]; size_t i = 0; for (; i + 1 < sizeof w && LD16(A1 + 2 * i); i++) w[i] = (char)LD16(A1 + 2 * i); w[i] = 0;
                 char p[2048]; rt_host_path(w, p, sizeof p); RET(remove(p) ? (u64)-1 : 0); }
SHIM(remove) { char p[2048]; rt_host_path((const char *)A1, p, sizeof p); RET(remove(p) ? (u64)-1 : 0); }
SHIM(rename) { char a[2048], b[2048]; rt_host_path((const char *)A1, a, sizeof a); rt_host_path((const char *)A2, b, sizeof b); RET(rename(a, b) ? (u64)-1 : 0); }
SHIM(_lseek) { RET((u32)lseek((int)A1, (off_t)(i32)A2, (int)A3)); }
SHIM(_wfreopen) { RET(0); }
SHIM(_seh_filter_exe) { RET(0); }   // EXCEPTION_CONTINUE_SEARCH: let our unhandled-exception path report it
SHIM(_register_thread_local_exe_atexit_callback) { RET(0); }
SHIM(_get_narrow_winmain_command_line) {   // command line after the program name
    static char *cl; if (!cl) { size_t n = 1; for (int i = 1; i < rt_argc; i++) n += strlen(rt_argv[i]) + 3;
        cl = calloc(1, n); for (int i = 1; i < rt_argc; i++) { if (i > 1) strcat(cl, " "); strcat(cl, rt_argv[i]); } }
    RET(cl);
}
SHIM(__dllonexit) { if (natexit < 256) atexit_fns[natexit++] = A1; RET(A1); }   // (fn, begin**, end**)
SHIM(__doserrno) { static __thread u32 e; RET(&e); }
SHIM(__pioinfo) { static u8 info[64 * 64]; RET(info); }   // msvcrt's internal fd table: zeros = "not a text-mode handle"
static __thread u64 cur_exc, cur_exc_ctx;
SHIM(__current_exception) { RET(&cur_exc); }
SHIM(__current_exception_context) { RET(&cur_exc_ctx); }
SHIM(terminate) { rt_log("std::terminate"); _exit(3); }

FILE *rt_host_file(u64 p);
const Shim shims_msvcrt[] = {
#define E3(n) { NULL, #n, sh_##n },
    E3(__stdio_common_vfscanf) E3(sscanf) E3(fscanf) E3(vsscanf) E3(clearerr) E3(_wcsicmp) E3(_wcsnicmp) E3(wcscmp) E3(wcsncmp)
    E3(wcscpy_s) E3(wcscpy) E3(strcspn) E3(strspn) E3(strpbrk) E3(strncat) E3(strtoll) E3(strtoull) E3(isprint) E3(isspace)
    E3(isdigit) E3(isalpha) E3(isalnum) E3(isxdigit) E3(isupper) E3(islower) E3(ispunct) E3(frexp) E3(ldexp) E3(copysign)
    E3(copysignf) E3(fabs) E3(_gmtime64) E3(_localtime64) E3(_time64) E3(time) E3(_mktime64) E3(_putenv) E3(_putenv_s)
    E3(_wremove) E3(remove) E3(rename) E3(_lseek) E3(_wfreopen) E3(_seh_filter_exe) E3(_register_thread_local_exe_atexit_callback)
    E3(_get_narrow_winmain_command_line) E3(__dllonexit) E3(__doserrno) E3(__pioinfo) E3(__current_exception) E3(__current_exception_context)
#undef E3
#define E(n) { NULL, #n, sh_##n }
    E(__acrt_iob_func), E(__iob_func), E(__stdio_common_vfprintf), E(__stdio_common_vfwprintf),
    E(__stdio_common_vsprintf), E(__stdio_common_vsnprintf_s), E(__stdio_common_vsprintf_s),
    E(printf), E(fprintf), E(sprintf), E(vfprintf), E(vprintf), E(_vsnprintf), E(_vsnwprintf),
    E(puts), E(putchar), E(fputs), E(fputc), E(fwrite), E(fread), E(fflush), E(setvbuf), E(fclose),
    E(fseek), E(_fseeki64), E(ftell), E(_ftelli64), E(fgets), E(fgetc), E(feof), E(ferror), E(_fileno),
    E(malloc), E(calloc), E(realloc), E(free), E(_aligned_malloc), E(_aligned_free), E(_msize),
    E(memcpy), E(memmove), E(memset), E(memcmp), E(memchr), E(strlen), E(strnlen), E(strcmp), E(strncmp),
    E(_stricmp), E(_strnicmp), E(strcpy), E(strncpy), E(strcat), E(strchr), E(strrchr), E(strstr), E(_strdup),
    E(strtol), E(strtoul), E(_strtoi64), E(_strtoui64), E(atoi), E(strtod), E(atof), E(wcslen), E(tolower), E(toupper),
    E(sin), E(cos), E(tan), E(asin), E(acos), E(atan), E(exp), E(log), E(log10), E(sqrt), E(floor), E(ceil),
    E(round), E(trunc), E(log2), E(exp2), E(sinh), E(cosh), E(tanh), E(atan2), E(pow), E(fmod), E(hypot),
    E(sinf), E(cosf), E(tanf), E(asinf), E(acosf), E(atanf), E(expf), E(logf), E(sqrtf), E(floorf), E(ceilf),
    E(roundf), E(truncf), E(log10f), E(log2f), E(exp2f), E(atan2f), E(powf), E(fmodf),
    E(abs), E(labs), E(llabs), E(rand), E(srand), E(qsort), E(bsearch),
    E(__p___argc), E(__p___argv), E(__p__acmdln), E(strtok), E(__p__environ), E(__p__fmode), E(__p__commode), E(__getmainargs), E(__set_app_type), E(__lconv_init),
    { NULL, "_acmdln", sh_msvcrt_data_import }, { NULL, "_fmode", sh_msvcrt_data_import }, { NULL, "_commode", sh_msvcrt_data_import },
    { NULL, "__initenv", sh_msvcrt_data_import }, { NULL, "_environ", sh_msvcrt_data_import }, { NULL, "__argc", sh_msvcrt_data_import },
    { NULL, "__argv", sh_msvcrt_data_import }, { NULL, "_iob", sh_msvcrt_data_import },
    { NULL, "_timezone", sh_msvcrt_data_import }, { NULL, "_daylight", sh_msvcrt_data_import }, { NULL, "_tzname", sh_msvcrt_data_import },
    E(_configure_narrow_argv), E(_initialize_narrow_environment), E(_set_app_type), E(_set_new_mode),
    E(_configthreadlocale), E(__setusermatherr), E(_set_invalid_parameter_handler), E(_set_fmode), E(signal),
    E(_errno), E(_initterm), E(_initterm_e), E(_crt_atexit), E(atexit), E(_onexit),
    E(_register_onexit_function), E(_initialize_onexit_table), E(_execute_onexit_table),
    E(exit), E(_exit), E(_cexit), E(_c_exit), E(abort), E(_amsg_exit),
    E(mbrtowc), E(wcrtomb), E(mbsrtowcs), E(wcsnlen), E(getenv), E(_lock_file), E(_unlock_file), E(localeconv),
    E(_assert), E(_wassert), E(strerror), E(_read), E(_write), E(_setmode), E(rand_s), E(__intrinsic_setjmp), E(_setjmp),
    E(longjmp), E(_isatty), E(_get_osfhandle), E(__stdio_common_vsscanf), E(terminate),
#undef E
    { 0, 0, 0 }
};
