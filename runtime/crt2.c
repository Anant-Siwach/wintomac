// UCRT / msvcrt (part 2): C99 math, the _s string functions, wide-character conversions, stat/find/path helpers,
// stdio extras, environment, time zone, locale objects, and the C helpers of vcruntime140 (type_info,
// std::exception, _CxxThrowException, RTTI casts) used when a game ships without vcruntime140.dll.
// Behaviour follows the Microsoft Learn CRT reference (return values, errno, _TRUNCATE, buffer-size rules).
// The MSVC RTTI / throw data layouts follow the documented x64 ABI as also described by Wine's msvcrt (cxx.h)
// and LLVM's MicrosoftCXXABI; no code copied.
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "winstr.h"

extern __thread int rt_crt_errno;
FILE *rt_host_file(u64 p);
char *rt_win_format(const void *fmt, int wide, u64 va, size_t *n);
int rt_win_sscanf(const char *buf, const char *fmt, u64 va);
#define SETERR(e) (rt_crt_errno = (e))
enum { W_EPERM = 1, W_ENOENT = 2, W_EBADF = 9, W_ENOMEM = 12, W_EACCES = 13, W_EEXIST = 17, W_EINVAL = 22, W_ERANGE = 34, W_EILSEQ = 42, W_STRUNCATE = 80 };
static int map_errno(int e) { return e == EILSEQ ? W_EILSEQ : e < 35 ? e : W_EINVAL; }   // POSIX 1..34 match the CRT's numbering

static ShimFn shim_fn(const char *name) {
    extern const Shim shims_kernel32[], shims_files[], shims_msvcrt[], shims_misc[], shims_seh[], shims_kernel32x[], shims_extra[];
    const Shim *t[] = { shims_seh, shims_kernel32, shims_files, shims_msvcrt, shims_misc, shims_extra, shims_kernel32x };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) for (const Shim *s = t[i]; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}
static u64 call4(Ctx *c, const char *name, u64 a1, u64 a2, u64 a3, u64 a4) {
    ShimFn f = shim_fn(name); if (!f) return 0;
    u64 s1 = c->r[RCX], s2 = c->r[RDX], s3 = c->r[R8], s4 = c->r[R9];
    c->r[RCX] = a1; c->r[RDX] = a2; c->r[R8] = a3; c->r[R9] = a4; f(c);
    u64 r = c->r[RAX]; c->r[RCX] = s1; c->r[RDX] = s2; c->r[R8] = s3; c->r[R9] = s4;
    return r;
}

// ==== math (doubles/floats in xmm0..2, ints by position in rdx/r8) =====================================================
#define D(i) (c->x[i].fd[0])
#define F(i) (c->x[i].f[0])
#define RETD(v) do { double r_ = (v); memset(&c->x[0], 0, 16); c->x[0].fd[0] = r_; RET(0); } while (0)
#define RETF(v) do { float r_ = (v); memset(&c->x[0], 0, 16); c->x[0].f[0] = r_; RET(0); } while (0)
#define M1(n) SHIM(n) { RETD(n(D(0))); }
#define M1F(n) SHIM(n) { RETF(n(F(0))); }
#define M2(n) SHIM(n) { RETD(n(D(0), D(1))); }
#define M2F(n) SHIM(n) { RETF(n(F(0), F(1))); }
M1(cbrt) M1F(cbrtf) M1(expm1) M1F(expm1f) M1(log1p) M1F(log1pf) M1(logb) M1F(logbf) M1(nearbyint) M1F(nearbyintf)
M1(rint) M1F(rintf) M1(erf) M1F(erff) M1(erfc) M1F(erfcf) M1(tgamma) M1F(tgammaf) M1(lgamma) M1F(lgammaf)
M1(acosh) M1F(acoshf) M1(asinh) M1F(asinhf) M1(atanh) M1F(atanhf) M1F(sinhf) M1F(coshf) M1F(tanhf) M1F(fabsf)
M2(fmax) M2F(fmaxf) M2(fmin) M2F(fminf) M2(fdim) M2F(fdimf) M2(remainder) M2F(remainderf) M2(nextafter) M2F(nextafterf)
M2F(hypotf)
#undef M1F
SHIM(_hypot) { RETD(hypot(D(0), D(1))); }
SHIM(_hypotf) { RETF(hypotf(F(0), F(1))); }
SHIM(_nextafter) { RETD(nextafter(D(0), D(1))); }
SHIM(_nextafterf) { RETF(nextafterf(F(0), F(1))); }
SHIM(_copysign) { RETD(copysign(D(0), D(1))); }
SHIM(_chgsign) { RETD(-D(0)); }
SHIM(_logb) { RETD(logb(D(0))); }
SHIM(_scalb) { RETD(scalbn(D(0), (int)(i32)A2)); }
SHIM(fma) { RETD(fma(D(0), D(1), D(2))); }
SHIM(fmaf) { RETF(fmaf(F(0), F(1), F(2))); }
SHIM(scalbn) { RETD(scalbn(D(0), (int)(i32)A2)); }
SHIM(scalbnf) { RETF(scalbnf(F(0), (int)(i32)A2)); }
SHIM(scalbln) { RETD(scalbln(D(0), (long)(i32)A2)); }
SHIM(scalblnf) { RETF(scalblnf(F(0), (long)(i32)A2)); }
SHIM(ldexpf) { RETF(ldexpf(F(0), (int)(i32)A2)); }
SHIM(frexpf) { int e; float r = frexpf(F(0), &e); if (A2) ST32(A2, (u32)e); RETF(r); }
SHIM(modf) { double ip; double r = modf(D(0), &ip); if (A2) memcpy((void *)A2, &ip, 8); RETD(r); }
SHIM(modff) { float ip; float r = modff(F(0), &ip); if (A2) memcpy((void *)A2, &ip, 4); RETF(r); }
SHIM(remquo) { int q; double r = remquo(D(0), D(1), &q); if (A3) ST32(A3, (u32)q); RETD(r); }
SHIM(remquof) { int q; float r = remquof(F(0), F(1), &q); if (A3) ST32(A3, (u32)q); RETF(r); }
SHIM(nan) { RETD(nan("")); }
SHIM(nanf) { RETF(nanf("")); }
// long is 32-bit on Windows
static u64 lclamp(double v) { if (isnan(v) || v >= 2147483648.0 || v < -2147483648.0) return 0x80000000u; return (u32)(i32)v; }
SHIM(lround) { RET(lclamp(round(D(0)))); }
SHIM(lroundf) { RET(lclamp(roundf(F(0)))); }
SHIM(llround) { RET((u64)llround(D(0))); }
SHIM(llroundf) { RET((u64)llroundf(F(0))); }
SHIM(lrint) { RET(lclamp(rint(D(0)))); }
SHIM(lrintf) { RET(lclamp(rintf(F(0)))); }
SHIM(llrint) { RET((u64)llrint(D(0))); }
SHIM(llrintf) { RET((u64)llrintf(F(0))); }
SHIM(ilogb) { RET((u32)ilogb(D(0))); }
SHIM(ilogbf) { RET((u32)ilogbf(F(0))); }
SHIM(_isnan) { RET(isnan(D(0)) ? 1 : 0); }
SHIM(_isnanf) { RET(isnan(F(0)) ? 1 : 0); }
SHIM(_finite) { RET(isfinite(D(0)) ? 1 : 0); }
SHIM(_finitef) { RET(isfinite(F(0)) ? 1 : 0); }
// _dclass: FP_INFINITE 1, FP_NAN 2, FP_NORMAL -1, FP_SUBNORMAL -2, FP_ZERO 0 (Windows values)
static u64 wclass(int k) { return (u64)(u32)(k == FP_INFINITE ? 1 : k == FP_NAN ? 2 : k == FP_NORMAL ? -1 : k == FP_SUBNORMAL ? -2 : 0); }
SHIM(_dclass) { RET(wclass(fpclassify(D(0)))); }
SHIM(_ldclass) { RET(wclass(fpclassify(D(0)))); }   // long double == double on Windows x64
SHIM(_fdclass) { RET(wclass(fpclassify(F(0)))); }
SHIM(_dsign) { RET(signbit(D(0)) ? 0x8000 : 0); }
SHIM(_ldsign) { RET(signbit(D(0)) ? 0x8000 : 0); }
SHIM(_fdsign) { RET(signbit(F(0)) ? 0x8000 : 0); }
SHIM(_dtest) { double v; memcpy(&v, (void *)A1, 8); RET(wclass(fpclassify(v))); }
SHIM(_fdtest) { float v; memcpy(&v, (void *)A1, 4); RET(wclass(fpclassify(v))); }
SHIM(_fpclass) {   // _FPCLASS_SNAN 1, QNAN 2, NINF 4, NN 8, ND 0x10, NZ 0x20, PZ 0x40, PD 0x80, PN 0x100, PINF 0x200
    double v = D(0); int s = signbit(v) != 0;
    switch (fpclassify(v)) { case FP_NAN: RET(2); case FP_INFINITE: RET(s ? 4 : 0x200); case FP_ZERO: RET(s ? 0x20 : 0x40);
                             case FP_SUBNORMAL: RET(s ? 0x10 : 0x80); default: RET(s ? 8 : 0x100); }
}
// x87/SSE control word: we always run round-to-nearest with all exceptions masked; report the Windows default.
static u32 fp_cw = 0x0009001F;   // _CW_DEFAULT: _RC_NEAR | _PC_53 | _EM_* masked
SHIM(_controlfp) { fp_cw = (fp_cw & ~(u32)A2) | ((u32)A1 & (u32)A2); RET(fp_cw); }
SHIM(_control87) { fp_cw = (fp_cw & ~(u32)A2) | ((u32)A1 & (u32)A2); RET(fp_cw); }
SHIM(_controlfp_s) { fp_cw = (fp_cw & ~(u32)A3) | ((u32)A2 & (u32)A3); if (A1) ST32(A1, fp_cw); RET(0); }   // (*current, new, mask)
SHIM(_clearfp) { RET(0); }
SHIM(_statusfp) { RET(0); }
SHIM(_fpreset) { RET(0); }
SHIM(_set_SSE2_enable) { RET(A1 ? 1 : 0); }
SHIM(__fpe_flt_rounds) { RET(1); }
SHIM(fegetround) { RET(0); }   // FE_TONEAREST
SHIM(fesetround) { RET((u32)A1 ? 1 : 0); }
SHIM(feclearexcept) { RET(0); }
SHIM(fetestexcept) { RET(0); }
SHIM(fegetenv) { if (A1) memset((void *)A1, 0, 8); RET(0); }
SHIM(fesetenv) { RET(0); }

// ==== character classes ===============================================================================================
SHIM(isblank) { int ch = (int)(i32)A1; RET(ch == ' ' || ch == '\t'); }
SHIM(iscntrl) { int ch = (int)(i32)A1; RET(ch >= 0 && ch < 256 && (ch < 32 || ch == 127) ? 0x20 : 0); }
SHIM(isgraph) { int ch = (int)(i32)A1; RET(ch > 32 && ch < 127 ? 1 : 0); }
SHIM(__isascii) { RET((u32)A1 < 128); }
SHIM(__toascii) { RET((u32)A1 & 0x7F); }
SHIM(_isctype) { RET(0); }
#define WCL(n, f) SHIM(n) { RET(f((wint_t)(u16)A1) ? 1 : 0); } SHIM(_##n##_l) { RET(f((wint_t)(u16)A1) ? 1 : 0); }
WCL(iswspace, iswspace) WCL(iswalnum, iswalnum) WCL(iswcntrl, iswcntrl) WCL(iswdigit, iswdigit) WCL(iswpunct, iswpunct)
WCL(iswalpha, iswalpha) WCL(iswupper, iswupper) WCL(iswlower, iswlower) WCL(iswprint, iswprint) WCL(iswxdigit, iswxdigit)
WCL(iswgraph, iswgraph) WCL(iswblank, iswblank)
#define CL(n) SHIM(_##n##_l) { int ch = (int)(i32)A1; RET(ch >= 0 && ch < 128 ? (n(ch) ? 1 : 0) : 0); }
CL(isalpha) CL(isdigit) CL(isspace) CL(isupper) CL(islower) CL(isalnum) CL(isxdigit) CL(ispunct) CL(isprint) CL(iscntrl) CL(isgraph)
SHIM(_tolower_l) { int ch = (int)(i32)A1; RET((u32)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch)); }
SHIM(_toupper_l) { int ch = (int)(i32)A1; RET((u32)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch)); }
SHIM(_tolower) { RET((u32)((i32)A1 + 32)); }
SHIM(_toupper) { RET((u32)((i32)A1 - 32)); }
SHIM(_towlower_l) { RET((u16)towlower((wint_t)(u16)A1)); }
SHIM(_towupper_l) { RET((u16)towupper((wint_t)(u16)A1)); }
// ctype table: UCRT's __pctype_func() points at entry 0 of a 257-entry table (entry -1 is EOF).
static u16 pctype_tab[257], pwctype_tab[257];
static void pctype_init(void) {
    if (pctype_tab[1 + ' ']) return;
    for (int ch = 0; ch < 256; ch++) {
        u16 f = 0;
        if (ch < 128) {
            if (isupper(ch)) f |= 0x1; if (islower(ch)) f |= 0x2; if (isdigit(ch)) f |= 0x4; if (isspace(ch)) f |= 0x8;
            if (ispunct(ch)) f |= 0x10; if (iscntrl(ch)) f |= 0x20; if (ch == ' ' || ch == '\t') f |= 0x40; if (isxdigit(ch)) f |= 0x80;
            if (isalpha(ch)) f |= 0x100;
        }
        pctype_tab[1 + ch] = f; pwctype_tab[1 + ch] = f;
    }
}
SHIM(__pctype_func) { pctype_init(); RET(&pctype_tab[1]); }
SHIM(__pwctype_func) { pctype_init(); RET(&pwctype_tab[1]); }
SHIM(_get_current_locale) { static u64 loc[2]; static u8 info[1024]; loc[0] = (u64)info; loc[1] = (u64)info; RET(loc); }
SHIM(_create_locale) { u64 *l = calloc(2, 8); l[0] = (u64)calloc(1, 1024); l[1] = (u64)calloc(1, 1024); RET(l); }
SHIM(_wcreate_locale) { sh__create_locale(c); }
SHIM(_free_locale) { if (A1) { u64 *l = (u64 *)A1; free((void *)l[0]); free((void *)l[1]); free(l); } RET(0); }
SHIM(___lc_locale_name_func) { static u64 names[6]; RET(names); }   // all categories: the "C" locale (NULL names)
SHIM(___lc_collate_cp_func) { RET(0); }
SHIM(_wsetlocale) { static const u16 C[] = { 'C', 0 }; RET(C); }
SHIM(_getmbcp) { RET(0); }
SHIM(_setmbcp) { RET(0); }

// ==== strings ==========================================================================================================
// _s functions: invalid arguments -> EINVAL (dest cleared when possible); too small -> ERANGE and dest[0] = 0.
SHIM(strcpy_s) {  // (dst, n, src)
    char *d = (char *)A1; size_t n = A2; const char *s = (const char *)A3;
    if (!d || !n) RET(SETERR(W_EINVAL)); if (!s) { d[0] = 0; RET(SETERR(W_EINVAL)); }
    size_t l = strlen(s); if (l >= n) { d[0] = 0; RET(SETERR(W_ERANGE)); }
    memmove(d, s, l + 1); RET(0);
}
SHIM(strcat_s) {
    char *d = (char *)A1; size_t n = A2; const char *s = (const char *)A3;
    if (!d || !n) RET(SETERR(W_EINVAL)); if (!s) { d[0] = 0; RET(SETERR(W_EINVAL)); }
    size_t dl = strnlen(d, n); if (dl == n) { d[0] = 0; RET(SETERR(W_EINVAL)); }
    size_t l = strlen(s); if (dl + l >= n) { d[0] = 0; RET(SETERR(W_ERANGE)); }
    memmove(d + dl, s, l + 1); RET(0);
}
static u64 strncpy_s_(char *d, size_t n, const char *s, size_t cnt) {   // cnt == (size_t)-1 is _TRUNCATE
    if (!d || !n) return (u64)SETERR(W_EINVAL);
    if (!s) { d[0] = 0; return (u64)SETERR(W_EINVAL); }
    size_t l = strnlen(s, cnt);
    if (l >= n) { if (cnt == (size_t)-1) { memmove(d, s, n - 1); d[n - 1] = 0; return W_STRUNCATE; } d[0] = 0; return (u64)SETERR(W_ERANGE); }
    memmove(d, s, l); d[l] = 0; return 0;
}
SHIM(strncpy_s) { RET(strncpy_s_((char *)A1, A2, (const char *)A3, A4)); }
SHIM(strncat_s) {  // (dst, n, src, count)
    char *d = (char *)A1; size_t n = A2; if (!d || !n) RET(SETERR(W_EINVAL));
    size_t dl = strnlen(d, n); if (dl == n) { d[0] = 0; RET(SETERR(W_EINVAL)); }
    u64 r = strncpy_s_(d + dl, n - dl, (const char *)A3, A4);
    if (r && r != W_STRUNCATE) d[0] = 0;
    RET(r);
}
static size_t wl(u64 p) { return wx_wlen(p); }
static size_t wnl(u64 p, size_t m) { size_t n = 0; while (n < m && LD16(p + 2 * n)) n++; return n; }
static u64 wcsncpy_s_(u64 d, size_t n, u64 s, size_t cnt) {
    if (!d || !n) return (u64)SETERR(W_EINVAL);
    if (!s) { ST16(d, 0); return (u64)SETERR(W_EINVAL); }
    size_t l = wnl(s, cnt);
    if (l >= n) { if (cnt == (size_t)-1) { memmove((void *)d, (void *)s, 2 * (n - 1)); ST16(d + 2 * (n - 1), 0); return W_STRUNCATE; } ST16(d, 0); return (u64)SETERR(W_ERANGE); }
    memmove((void *)d, (void *)s, 2 * l); ST16(d + 2 * l, 0); return 0;
}
SHIM(wcsncpy_s) { RET(wcsncpy_s_(A1, A2, A3, A4)); }
SHIM(wcscat_s) {
    u64 d = A1; size_t n = A2; if (!d || !n) RET(SETERR(W_EINVAL)); if (!A3) { ST16(d, 0); RET(SETERR(W_EINVAL)); }
    size_t dl = wnl(d, n); if (dl == n) { ST16(d, 0); RET(SETERR(W_EINVAL)); }
    size_t l = wl(A3); if (dl + l >= n) { ST16(d, 0); RET(SETERR(W_ERANGE)); }
    memmove((void *)(d + 2 * dl), (void *)A3, 2 * (l + 1)); RET(0);
}
SHIM(wcsncat_s) {
    u64 d = A1; size_t n = A2; if (!d || !n) RET(SETERR(W_EINVAL));
    size_t dl = wnl(d, n); if (dl == n) { ST16(d, 0); RET(SETERR(W_EINVAL)); }
    u64 r = wcsncpy_s_(d + 2 * dl, n - dl, A3, A4); if (r && r != W_STRUNCATE) ST16(d, 0); RET(r);
}
SHIM(memcpy_s) {  // (dst, dn, src, n)
    if (!A4) RET(0); if (!A1) RET(SETERR(W_EINVAL));
    if (!A3) { memset((void *)A1, 0, A2); RET(SETERR(W_EINVAL)); }
    if (A2 < A4) { memset((void *)A1, 0, A2); RET(SETERR(W_ERANGE)); }
    memcpy((void *)A1, (void *)A3, A4); RET(0);
}
SHIM(memmove_s) { if (!A4) RET(0); if (!A1 || !A3) RET(SETERR(W_EINVAL)); if (A2 < A4) RET(SETERR(W_ERANGE)); memmove((void *)A1, (void *)A3, A4); RET(0); }
SHIM(wmemcpy_s) { if (!A4) RET(0); if (!A1 || !A3) RET(SETERR(W_EINVAL)); if (A2 < A4) { memset((void *)A1, 0, 2 * A2); RET(SETERR(W_ERANGE)); } memcpy((void *)A1, (void *)A3, 2 * A4); RET(0); }
SHIM(wmemmove_s) { if (!A4) RET(0); if (!A1 || !A3) RET(SETERR(W_EINVAL)); if (A2 < A4) RET(SETERR(W_ERANGE)); memmove((void *)A1, (void *)A3, 2 * A4); RET(0); }
SHIM(_strlwr) { for (char *p = (char *)A1; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32; RET(A1); }
SHIM(_strupr) { for (char *p = (char *)A1; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; RET(A1); }
SHIM(_strlwr_s) { if (!A1) RET(SETERR(W_EINVAL)); if (strnlen((char *)A1, A2) == A2) { if (A2) ST8(A1, 0); RET(SETERR(W_EINVAL)); } sh__strlwr(c); RET(0); }
SHIM(_strupr_s) { if (!A1) RET(SETERR(W_EINVAL)); if (strnlen((char *)A1, A2) == A2) { if (A2) ST8(A1, 0); RET(SETERR(W_EINVAL)); } sh__strupr(c); RET(0); }
SHIM(_strrev) { char *s = (char *)A1; size_t n = strlen(s); for (size_t i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; } RET(A1); }
SHIM(_strset) { for (char *p = (char *)A1; *p; p++) *p = (char)A2; RET(A1); }
SHIM(_strnset) { char *p = (char *)A1; for (size_t i = 0; i < A3 && p[i]; i++) p[i] = (char)A2; RET(A1); }
SHIM(_memicmp) { const u8 *a = (const u8 *)A1, *b = (const u8 *)A2; for (size_t i = 0; i < A3; i++) { int x = tolower(a[i]), y = tolower(b[i]); if (x != y) RET((u32)(x - y)); } RET(0); }
SHIM(_strcmpi) { RET((u32)strcasecmp((char *)A1, (char *)A2)); }
SHIM(_stricoll) { RET((u32)strcasecmp((char *)A1, (char *)A2)); }
SHIM(_strncoll) { RET((u32)strncmp((char *)A1, (char *)A2, A3)); }
SHIM(_strnicoll) { RET((u32)strncasecmp((char *)A1, (char *)A2, A3)); }
SHIM(_strcoll_l) { RET((u32)strcmp((char *)A1, (char *)A2)); }
SHIM(_stricmp_l) { RET((u32)strcasecmp((char *)A1, (char *)A2)); }
SHIM(_strxfrm_l) { size_t n = strlen((char *)A2); if (A1 && A3 > n) memcpy((void *)A1, (void *)A2, n + 1); RET(n); }
static int wcmp_(u64 a, u64 b, size_t n, int ci) {
    for (size_t i = 0; i < n; i++) {
        u32 x = LD16(a + 2 * i), y = LD16(b + 2 * i);
        if (ci) { x = (u32)towlower(x); y = (u32)towlower(y); }
        if (x != y || !x) return (int)x - (int)y;
    }
    return 0;
}
SHIM(_wcscoll_l) { RET((u32)wcmp_(A1, A2, ~(size_t)0, 0)); }
SHIM(_wcsicoll) { RET((u32)wcmp_(A1, A2, ~(size_t)0, 1)); }
SHIM(_wcsxfrm_l) { size_t n = wl(A2); if (A1 && A3 > n) memcpy((void *)A1, (void *)A2, 2 * (n + 1)); RET(n); }
SHIM(_wcsicmp_l) { RET((u32)wcmp_(A1, A2, ~(size_t)0, 1)); }
SHIM(_wcsdup) { if (!A1) RET(0); size_t n = wl(A1); u16 *w = malloc(2 * (n + 1)); memcpy(w, (void *)A1, 2 * (n + 1)); RET(w); }
SHIM(_wcslwr) { for (u64 p = A1; LD16(p); p += 2) ST16(p, (u16)towlower(LD16(p))); RET(A1); }
SHIM(_wcsupr) { for (u64 p = A1; LD16(p); p += 2) ST16(p, (u16)towupper(LD16(p))); RET(A1); }
SHIM(_wcslwr_s) { if (!A1) RET(SETERR(W_EINVAL)); sh__wcslwr(c); RET(0); }
SHIM(_wcsupr_s) { if (!A1) RET(SETERR(W_EINVAL)); sh__wcsupr(c); RET(0); }
SHIM(_wcsrev) { size_t n = wl(A1); for (size_t i = 0; i < n / 2; i++) { u16 t = LD16(A1 + 2 * i); ST16(A1 + 2 * i, LD16(A1 + 2 * (n - 1 - i))); ST16(A1 + 2 * (n - 1 - i), t); } RET(A1); }
SHIM(wcscat) { size_t n = wl(A1); memmove((void *)(A1 + 2 * n), (void *)A2, 2 * (wl(A2) + 1)); RET(A1); }
SHIM(wcsncpy) { size_t i = 0; for (; i < A3 && LD16(A2 + 2 * i); i++) ST16(A1 + 2 * i, LD16(A2 + 2 * i)); for (; i < A3; i++) ST16(A1 + 2 * i, 0); RET(A1); }
SHIM(wcsncat) { size_t n = wl(A1), i = 0; for (; i < A3 && LD16(A2 + 2 * i); i++) ST16(A1 + 2 * (n + i), LD16(A2 + 2 * i)); ST16(A1 + 2 * (n + i), 0); RET(A1); }
SHIM(wcschr) { u16 ch = (u16)A2; for (u64 p = A1; ; p += 2) { u16 v = LD16(p); if (v == ch) RET(p); if (!v) RET(0); } }
SHIM(wcsrchr) { u16 ch = (u16)A2; u64 r = 0; for (u64 p = A1; ; p += 2) { u16 v = LD16(p); if (v == ch) r = p; if (!v) RET(r); } }
SHIM(wcsstr) {
    size_t nl = wl(A2); if (!nl) RET(A1);
    for (u64 p = A1; LD16(p); p += 2) if (!memcmp((void *)p, (void *)A2, 2 * nl)) RET(p);
    RET(0);
}
static int in_set(u64 set, u16 ch) { for (u64 q = set; LD16(q); q += 2) if (LD16(q) == ch) return 1; return 0; }
SHIM(wcspbrk) { for (u64 p = A1; LD16(p); p += 2) if (in_set(A2, LD16(p))) RET(p); RET(0); }
SHIM(wcsspn) { size_t n = 0; while (LD16(A1 + 2 * n) && in_set(A2, LD16(A1 + 2 * n))) n++; RET(n); }
SHIM(wcscspn) { size_t n = 0; while (LD16(A1 + 2 * n) && !in_set(A2, LD16(A1 + 2 * n))) n++; RET(n); }
static u64 wcstok_(u64 s, u64 delim, u64 *ctx) {
    if (!s) s = *ctx; if (!s) return 0;
    while (LD16(s) && in_set(delim, LD16(s))) s += 2;
    if (!LD16(s)) { *ctx = s; return 0; }
    u64 tok = s; while (LD16(s) && !in_set(delim, LD16(s))) s += 2;
    if (LD16(s)) { ST16(s, 0); s += 2; }
    *ctx = s; return tok;
}
SHIM(wcstok) { static __thread u64 save; u64 *ctx = A3 ? (u64 *)A3 : &save; RET(wcstok_(A1, A2, ctx)); }   // UCRT: (str, delim, context)
SHIM(wcstok_s) { RET(wcstok_(A1, A2, (u64 *)A3)); }
SHIM(strtok_s) { RET(strtok_r(A1 ? (char *)A1 : NULL, (const char *)A2, (char **)A3)); }
SHIM(strnlen_s) { RET(A1 ? strnlen((char *)A1, A2) : 0); }
SHIM(wcsnlen_s) { RET(A1 ? wnl(A1, A2) : 0); }
SHIM(wmemset) { for (size_t i = 0; i < A3; i++) ST16(A1 + 2 * i, (u16)A2); RET(A1); }
SHIM(wmemcpy) { memcpy((void *)A1, (void *)A2, 2 * A3); RET(A1); }
SHIM(wmemmove) { memmove((void *)A1, (void *)A2, 2 * A3); RET(A1); }
SHIM(wmemcmp) { for (size_t i = 0; i < A3; i++) { u16 x = LD16(A1 + 2 * i), y = LD16(A2 + 2 * i); if (x != y) RET((u32)(x < y ? -1 : 1)); } RET(0); }
SHIM(wmemchr) { for (size_t i = 0; i < A3; i++) if (LD16(A1 + 2 * i) == (u16)A2) RET(A1 + 2 * i); RET(0); }

// ==== multibyte <-> wide (the "C" locale: bytes map 1:1 to U+0000..U+00FF, like the existing mbrtowc) =================
SHIM(mbtowc) { if (!A2) RET(0); u8 ch = LD8(A2); if (!A3) RET((u64)-1); if (A1) ST16(A1, ch); RET(ch ? 1 : 0); }
SHIM(_mbtowc_l) { sh_mbtowc(c); }
SHIM(mblen) { if (!A1) RET(0); RET(LD8(A1) ? 1 : 0); }
SHIM(mbrlen) { if (!A1) RET(0); if (!A2) RET((u64)-2); RET(LD8(A1) ? 1 : 0); }
SHIM(_mbstrlen) { RET(strlen((char *)A1)); }
SHIM(wctomb) { if (!A1) RET(0); u16 w = (u16)A2; if (w > 255) { SETERR(W_EILSEQ); RET((u64)-1); } ST8(A1, (u8)w); RET(1); }
SHIM(wctomb_s) { u16 w = (u16)AN(4); if (!A2) { if (A1) ST32(A1, 0); RET(0); } if (w > 255) { if (A1) ST32(A1, (u32)-1); RET(SETERR(W_EILSEQ)); } if (!A3) RET(SETERR(W_ERANGE)); ST8(A2, (u8)w); if (A1) ST32(A1, 1); RET(0); }   // (*ret, dst, size, wc)
SHIM(btowc) { int ch = (int)(i32)A1; RET(ch < 0 || ch > 255 ? 0xFFFF : (u32)ch); }
SHIM(wctob) { u16 w = (u16)A1; RET(w < 256 ? w : (u32)-1); }
SHIM(mbstowcs) {  // (dst, src, n) -> chars converted, excluding NUL
    const u8 *s = (const u8 *)A2; size_t n = strlen((const char *)s);
    if (!A1) RET(n);
    size_t i = 0; for (; i < A3 && i < n; i++) ST16(A1 + 2 * i, s[i]);
    if (i < A3) ST16(A1 + 2 * i, 0);
    RET(i);
}
SHIM(mbstowcs_s) {  // (*converted incl NUL, dst, dsize, src, count)
    const u8 *s = (const u8 *)A4; size_t cnt = AN(5), n = strnlen((const char *)s, cnt == (size_t)-1 ? (size_t)-1 : cnt);
    if (!A2) { if (A1) ST64(A1, n + 1); RET(0); }
    size_t room = A3; int trunc = 0;
    if (n >= room) { if (cnt != (size_t)-1) { if (room) ST16(A2, 0); RET(SETERR(W_ERANGE)); } n = room - 1; trunc = 1; }
    for (size_t i = 0; i < n; i++) ST16(A2 + 2 * i, s[i]);
    ST16(A2 + 2 * n, 0); if (A1) ST64(A1, n + 1);
    RET(trunc ? W_STRUNCATE : 0);
}
SHIM(wcstombs) {  // (dst, src, n): characters above U+00FF have no "C"-locale encoding -> -1, EILSEQ
    size_t n = wl(A2);
    for (size_t i = 0; i < n; i++) if (LD16(A2 + 2 * i) > 255) { SETERR(W_EILSEQ); RET((u64)-1); }
    if (!A1) RET(n);
    size_t i = 0; for (; i < A3 && i < n; i++) ST8(A1 + i, (u8)LD16(A2 + 2 * i));
    if (i < A3) ST8(A1 + i, 0);
    RET(i);
}
SHIM(wcstombs_s) {  // (*converted incl NUL, dst, dsize, src, count)
    size_t cnt = AN(5), n = wnl(A4, cnt);
    for (size_t i = 0; i < n; i++) if (LD16(A4 + 2 * i) > 255) { if (A2 && A3) ST8(A2, 0); RET(SETERR(W_EILSEQ)); }
    if (!A2) { if (A1) ST64(A1, n + 1); RET(0); }
    int trunc = 0;
    if (n >= A3) { if (cnt != (size_t)-1) { if (A3) ST8(A2, 0); RET(SETERR(W_ERANGE)); } n = A3 - 1; trunc = 1; }
    for (size_t i = 0; i < n; i++) ST8(A2 + i, (u8)LD16(A4 + 2 * i));
    ST8(A2 + n, 0); if (A1) ST64(A1, n + 1);
    RET(trunc ? W_STRUNCATE : 0);
}
SHIM(wcrtomb_s) {  // (*ret, dst, size, wc, state)
    u16 w = (u16)A4;
    if (w > 255) { if (A1) ST64(A1, (u64)-1); RET(SETERR(W_EILSEQ)); }
    if (A2) { if (!A3) RET(SETERR(W_ERANGE)); ST8(A2, (u8)w); }
    if (A1) ST64(A1, 1); RET(0);
}
SHIM(wcsrtombs) {  // (dst, const wchar_t **src, n, state)
    u64 s = LD64(A2); size_t n = wl(s), i = 0;
    for (size_t k = 0; k < n; k++) if (LD16(s + 2 * k) > 255) { SETERR(W_EILSEQ); RET((u64)-1); }
    if (!A1) RET(n);
    for (; i < A3 && i < n; i++) ST8(A1 + i, (u8)LD16(s + 2 * i));
    if (i < A3) { ST8(A1 + i, 0); ST64(A2, 0); } else ST64(A2, s + 2 * i);
    RET(i);
}

// ==== number <-> string ================================================================================================
static void w2ascii(u64 w, char *out, size_t n) { size_t i = 0; for (; i + 1 < n && LD16(w + 2 * i); i++) { u16 ch = LD16(w + 2 * i); out[i] = ch < 128 ? (char)ch : '\x7f'; } out[i] = 0; }
static i64 wtoll_(u64 s, u64 endp, int base, int is_signed, int bits) {
    char b[512]; w2ascii(s, b, sizeof b); char *e; errno = 0;
    i64 v; u64 uv;
    if (is_signed) {
        v = strtoll(b, &e, base);
        if (bits == 32 && (v > 0x7FFFFFFFll || v < -0x80000000ll)) { v = v > 0 ? 0x7FFFFFFF : -0x80000000ll; SETERR(W_ERANGE); }
    } else {
        uv = strtoull(b, &e, base); v = (i64)uv;
        if (bits == 32 && uv > 0xFFFFFFFFull && !(b[0] == '-' || (b[0] == ' ' && strchr(b, '-')))) { v = 0xFFFFFFFF; SETERR(W_ERANGE); }
    }
    if (errno == ERANGE) SETERR(W_ERANGE);
    if (endp) ST64(endp, s + 2 * (u64)(e - b));
    return v;
}
SHIM(wcstol) { RET((u32)(i32)wtoll_(A1, A2, (int)A3, 1, 32)); }
SHIM(wcstoul) { RET((u32)wtoll_(A1, A2, (int)A3, 0, 32)); }
SHIM(wcstoll) { RET((u64)wtoll_(A1, A2, (int)A3, 1, 64)); }
SHIM(wcstoull) { RET((u64)wtoll_(A1, A2, (int)A3, 0, 64)); }
SHIM(_wcstoi64) { RET((u64)wtoll_(A1, A2, (int)A3, 1, 64)); }
SHIM(_wcstoui64) { RET((u64)wtoll_(A1, A2, (int)A3, 0, 64)); }
SHIM(wcstoimax) { RET((u64)wtoll_(A1, A2, (int)A3, 1, 64)); }
SHIM(wcstoumax) { RET((u64)wtoll_(A1, A2, (int)A3, 0, 64)); }
SHIM(_wtoi) { RET((u32)(i32)wtoll_(A1, 0, 10, 1, 32)); }
SHIM(_wtol) { RET((u32)(i32)wtoll_(A1, 0, 10, 1, 32)); }
SHIM(_wtoll) { RET((u64)wtoll_(A1, 0, 10, 1, 64)); }
SHIM(_wtoi64) { RET((u64)wtoll_(A1, 0, 10, 1, 64)); }
static double wtod_(u64 s, u64 endp) { char b[512]; w2ascii(s, b, sizeof b); char *e; double d = strtod(b, &e); if (endp) ST64(endp, s + 2 * (u64)(e - b)); return d; }
SHIM(wcstod) { RETD(wtod_(A1, A2)); }
SHIM(wcstof) { RETF((float)wtod_(A1, A2)); }
SHIM(_wtof) { RETD(wtod_(A1, 0)); }
SHIM(_wcstod_l) { RETD(wtod_(A1, A2)); }
SHIM(strtof) { char *e; errno = 0; float f = strtof((char *)A1, &e); if (errno == ERANGE) SETERR(W_ERANGE); if (A2) ST64(A2, (u64)e); RETF(f); }
SHIM(_strtod_l) { char *e; double d = strtod((char *)A1, &e); if (A2) ST64(A2, (u64)e); RETD(d); }
SHIM(_strtof_l) { char *e; float f = strtof((char *)A1, &e); if (A2) ST64(A2, (u64)e); RETF(f); }
SHIM(_atof_l) { RETD(atof((char *)A1)); }
SHIM(atol) { RET((u32)(i32)strtol((char *)A1, NULL, 10)); }
SHIM(atoll) { RET((u64)strtoll((char *)A1, NULL, 10)); }
SHIM(_atoi64) { RET((u64)strtoll((char *)A1, NULL, 10)); }
SHIM(strtoimax) { RET((u64)strtoll((char *)A1, (char **)A2, (int)A3)); }
SHIM(strtoumax) { RET((u64)strtoull((char *)A1, (char **)A2, (int)A3)); }
SHIM(_strtol_l) { RET((u32)(i32)strtol((char *)A1, (char **)A2, (int)A3)); }
SHIM(_strtoul_l) { RET((u32)strtoul((char *)A1, (char **)A2, (int)A3)); }
SHIM(_strtoi64_l) { RET((u64)strtoll((char *)A1, (char **)A2, (int)A3)); }
SHIM(_strtoui64_l) { RET((u64)strtoull((char *)A1, (char **)A2, (int)A3)); }
// _itoa family: negative values are signed only for radix 10 (Windows rule); other radixes print the two's complement.
static void utoa_(u64 v, int neg, char *out, int radix) {
    char t[80]; int n = 0;
    if (radix < 2 || radix > 36) { out[0] = 0; return; }
    do { int d = (int)(v % (u64)radix); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (u64)radix; } while (v);
    int o = 0; if (neg) out[o++] = '-';
    while (n) out[o++] = t[--n];
    out[o] = 0;
}
static void itoa_(i64 v, int bits, char *out, int radix) {
    if (radix == 10 && v < 0) utoa_((u64)-v, 1, out, radix);
    else utoa_(bits == 32 ? (u64)(u32)v : (u64)v, 0, out, radix);
}
SHIM(_itoa) { char b[80]; itoa_((i32)A1, 32, b, (int)A3); strcpy((char *)A2, b); RET(A2); }
SHIM(_ltoa) { char b[80]; itoa_((i32)A1, 32, b, (int)A3); strcpy((char *)A2, b); RET(A2); }
SHIM(_i64toa) { char b[80]; itoa_((i64)A1, 64, b, (int)A3); strcpy((char *)A2, b); RET(A2); }
static u64 xtoa_s(char *b, u64 dst, size_t n) {
    if (!dst || !n) return (u64)SETERR(W_EINVAL);
    if (strlen(b) + 1 > n) { ST8(dst, 0); return (u64)SETERR(W_ERANGE); }
    strcpy((char *)dst, b); return 0;
}
SHIM(_itoa_s) { char b[80]; itoa_((i32)A1, 32, b, (int)A4); RET(xtoa_s(b, A2, A3)); }   // (value, buf, size, radix)
SHIM(_ltoa_s) { char b[80]; itoa_((i32)A1, 32, b, (int)A4); RET(xtoa_s(b, A2, A3)); }
SHIM(_ultoa_s) { char b[80]; utoa_((u32)A1, 0, b, (int)A4); RET(xtoa_s(b, A2, A3)); }
SHIM(_i64toa_s) { char b[80]; itoa_((i64)A1, 64, b, (int)A4); RET(xtoa_s(b, A2, A3)); }
SHIM(_ui64toa_s) { char b[80]; utoa_(A1, 0, b, (int)A4); RET(xtoa_s(b, A2, A3)); }
static u64 put_w(const char *b, u64 dst) { size_t i = 0; for (; b[i]; i++) ST16(dst + 2 * i, (u8)b[i]); ST16(dst + 2 * i, 0); return dst; }
SHIM(_itow) { char b[80]; itoa_((i32)A1, 32, b, (int)A3); RET(put_w(b, A2)); }
SHIM(_ltow) { char b[80]; itoa_((i32)A1, 32, b, (int)A3); RET(put_w(b, A2)); }
SHIM(_ultow) { char b[80]; utoa_((u32)A1, 0, b, (int)A3); RET(put_w(b, A2)); }
SHIM(_i64tow) { char b[80]; itoa_((i64)A1, 64, b, (int)A3); RET(put_w(b, A2)); }
SHIM(_ui64tow) { char b[80]; utoa_(A1, 0, b, (int)A3); RET(put_w(b, A2)); }
SHIM(_itow_s) { char b[80]; itoa_((i32)A1, 32, b, (int)A4); if (!A2 || !A3) RET(SETERR(W_EINVAL)); if (strlen(b) + 1 > A3) { ST16(A2, 0); RET(SETERR(W_ERANGE)); } put_w(b, A2); RET(0); }

// ==== printf / scanf variants ===========================================================================================
static u64 put_units(const char *s, size_t n, u64 dst, size_t cap) {   // UTF-8 -> UTF-16 into dst (cap units); returns units needed
    char *z = malloc(n + 1); memcpy(z, s, n); z[n] = 0;
    size_t need = wx_u2w(z, 0, 0) - 1;
    if (dst && cap) { u16 *t = malloc(2 * (need + 1)); wx_u2w(z, (u64)t, need + 1); memcpy((void *)dst, t, 2 * (need < cap ? need : cap)); free(t); }
    free(z); return need;
}
SHIM(__stdio_common_vswprintf) {  // (options, wbuf, count, wfmt, locale, va)
    size_t n; char *s = rt_win_format((void *)A4, 1, AN(6), &n);
    u64 buf = A2; size_t cnt = A3; u64 opts = A1;
    size_t units = put_units(s, n, 0, 0); free(s);
    if (!buf || !cnt) RET(units);
    s = rt_win_format((void *)A4, 1, AN(6), &n); put_units(s, n, buf, cnt); free(s);
    if (units < cnt) { ST16(buf + 2 * units, 0); RET(units); }
    ST16(buf + 2 * (cnt - 1), 0);
    RET((opts & 2) ? units : (u64)-1);
}
SHIM(__stdio_common_vswprintf_s) {  // (options, wbuf, size, wfmt, locale, va)
    size_t n; char *s = rt_win_format((void *)A4, 1, AN(6), &n);
    size_t units = put_units(s, n, 0, 0);
    if (units < A3) { put_units(s, n, A2, A3); ST16(A2 + 2 * units, 0); free(s); RET(units); }
    if (A3) ST16(A2, 0); free(s); SETERR(W_ERANGE); RET((u64)-1);
}
SHIM(__stdio_common_vsnwprintf_s) {  // (options, wbuf, size, count, wfmt, locale, va)
    size_t n; char *s = rt_win_format((void *)AN(5), 1, AN(7), &n);
    size_t units = put_units(s, n, 0, 0), sz = A3, cnt = A4;
    size_t lim = cnt == (size_t)-1 ? sz - 1 : cnt;
    if (units <= lim && units < sz) { put_units(s, n, A2, sz); ST16(A2 + 2 * units, 0); free(s); RET(units); }
    if (cnt == (size_t)-1 && sz) { put_units(s, n, A2, sz - 1); ST16(A2 + 2 * (sz - 1), 0); free(s); RET((u64)-1); }
    if (lim < sz) { put_units(s, n, A2, lim); ST16(A2 + 2 * lim, 0); free(s); RET((u64)-1); }
    if (sz) ST16(A2, 0); free(s); SETERR(W_ERANGE); RET((u64)-1);
}
SHIM(__stdio_common_vfprintf_s) {  // (options, FILE*, fmt, locale, va)
    size_t n; char *s = rt_win_format((void *)A3, 0, AN(5), &n);
    size_t w = fwrite(s, 1, n, rt_host_file(A2)); free(s); RET(w == n ? n : (u64)-1);
}
SHIM(__stdio_common_vfprintf_p) { sh___stdio_common_vfprintf_s(c); }
SHIM(__stdio_common_vfwprintf_s) { size_t n; char *s = rt_win_format((void *)A3, 1, AN(5), &n); fwrite(s, 1, n, rt_host_file(A2)); free(s); RET(n); }
SHIM(__stdio_common_vfwprintf_p) { sh___stdio_common_vfwprintf_s(c); }
SHIM(__stdio_common_vsprintf_p) {  // (options, buf, count, fmt, locale, va)
    size_t n; char *s = rt_win_format((void *)A4, 0, AN(6), &n);
    if (A2 && A3) { size_t k = n < A3 ? n : A3 - 1; memcpy((void *)A2, s, k); ST8(A2 + k, 0); }
    free(s); RET(A2 && n >= A3 ? (u64)-1 : n);
}
SHIM(__stdio_common_vswprintf_p) { sh___stdio_common_vswprintf_s(c); }
SHIM(_vscprintf) { size_t n; char *s = rt_win_format((void *)A1, 0, A2, &n); free(s); RET(n); }
SHIM(_vscwprintf) { size_t n; char *s = rt_win_format((void *)A1, 1, A2, &n); size_t u = put_units(s, n, 0, 0); free(s); RET(u); }
SHIM(_vsnprintf_s) {  // (buf, size, count, fmt, va)
    size_t n; char *s = rt_win_format((void *)A4, 0, AN(5), &n); size_t sz = A2, cnt = A3;
    size_t lim = cnt == (size_t)-1 ? sz - 1 : cnt;
    if (n <= lim && n < sz) { memcpy((void *)A1, s, n + 1); free(s); RET(n); }
    if (cnt == (size_t)-1 && sz) { memcpy((void *)A1, s, sz - 1); ST8(A1 + sz - 1, 0); free(s); RET((u64)-1); }
    if (lim < sz) { memcpy((void *)A1, s, lim); ST8(A1 + lim, 0); free(s); RET((u64)-1); }
    if (sz) ST8(A1, 0); free(s); SETERR(W_ERANGE); RET((u64)-1);
}
SHIM(_vsprintf_s) { size_t n; char *s = rt_win_format((void *)A3, 0, A4, &n); if (n < A2) { memcpy((void *)A1, s, n + 1); free(s); RET(n); } if (A2) ST8(A1, 0); free(s); RET((u64)-1); }   // (buf, size, fmt, va)
static u64 spill(Ctx *c, int fixed) {   // msvcrt varargs: spill register args into the home area
    u64 sp = c->r[RSP];
    ST64(sp + 8, c->r[RCX]); ST64(sp + 16, c->r[RDX]); ST64(sp + 24, c->r[R8]); ST64(sp + 32, c->r[R9]);
    return sp + 8 + 8ull * fixed;
}
SHIM(fwprintf) { u64 f = A1, fmt = A2; size_t n; char *s = rt_win_format((void *)fmt, 1, spill(c, 2), &n); fwrite(s, 1, n, rt_host_file(f)); free(s); RET(n); }
SHIM(wprintf) { u64 fmt = A1; size_t n; char *s = rt_win_format((void *)fmt, 1, spill(c, 1), &n); fwrite(s, 1, n, stdout); free(s); RET(n); }
SHIM(vfwprintf) { size_t n; char *s = rt_win_format((void *)A2, 1, A3, &n); fwrite(s, 1, n, rt_host_file(A1)); free(s); RET(n); }
SHIM(_snwprintf_s) {  // (buf, size, count, fmt, ...)
    u64 buf = A1, sz = A2, cnt = A3, fmt = A4; size_t n; char *s = rt_win_format((void *)fmt, 1, spill(c, 4), &n);
    size_t units = put_units(s, n, 0, 0), lim = cnt == (u64)-1 ? sz - 1 : cnt;
    if (units <= lim && units < sz) { put_units(s, n, buf, sz); ST16(buf + 2 * units, 0); free(s); RET(units); }
    if (sz) { size_t k = lim < sz ? lim : sz - 1; put_units(s, n, buf, k); ST16(buf + 2 * k, 0); }
    free(s); RET((u64)-1);
}
SHIM(_snprintf) { u64 buf = A1, cnt = A2, fmt = A3; size_t n; char *s = rt_win_format((void *)fmt, 0, spill(c, 3), &n); memcpy((void *)buf, s, n < cnt ? n + 1 : cnt); free(s); RET(n <= cnt ? n : (u64)-1); }
SHIM(_scprintf) { u64 fmt = A1; size_t n; char *s = rt_win_format((void *)fmt, 0, spill(c, 1), &n); free(s); RET(n); }
SHIM(sprintf_s) { u64 buf = A1, sz = A2, fmt = A3; size_t n; char *s = rt_win_format((void *)fmt, 0, spill(c, 3), &n); if (n < sz) memcpy((void *)buf, s, n + 1); else if (sz) ST8(buf, 0); free(s); RET(n < sz ? n : (u64)-1); }
SHIM(swprintf_s) { u64 buf = A1, sz = A2, fmt = A3; size_t n; char *s = rt_win_format((void *)fmt, 1, spill(c, 3), &n); size_t u = put_units(s, n, 0, 0); if (u < sz) { put_units(s, n, buf, sz); ST16(buf + 2 * u, 0); } else if (sz) ST16(buf, 0); free(s); RET(u < sz ? u : (u64)-1); }
SHIM(__stdio_common_vswscanf) {  // (options, wbuf, count, wfmt, locale, va): narrow-converted input; %s/%c targets must be narrow
    char b[4096], f[1024]; wx_w2u(A2, b, sizeof b); wx_w2u(A4, f, sizeof f);
    RET((u32)rt_win_sscanf(b, f, AN(6)));
}
SHIM(__stdio_common_vfwscanf) { WX_FAIL(__stdio_common_vfwscanf, 0, (u64)-1); }

// ==== stdio extras ======================================================================================================
static int win_oflags(int f) {   // _O_* -> host open flags
    int fl = f & 3; if (f & 8) fl |= O_APPEND; if (f & 0x100) fl |= O_CREAT; if (f & 0x200) fl |= O_TRUNC; if (f & 0x400) fl |= O_EXCL;
    return fl | O_CLOEXEC;
}
static int open_path(const char *win, int oflag, int pmode) {
    char host[4096]; rt_host_path(win, host, sizeof host);
    int fd = open(host, win_oflags(oflag), (pmode & 0x80) ? 0644 : 0444);
    if (fd < 0) SETERR(map_errno(errno));
    return fd;
}
SHIM(_wopen) { char p[2048]; wx_w2u(A1, p, sizeof p); RET((u32)open_path(p, (int)A2, (int)A3 ? (int)A3 : 0x180)); }   // (path, oflag, pmode)
SHIM(_sopen) { RET((u32)open_path((const char *)A1, (int)A2, (int)A4 ? (int)A4 : 0x180)); }   // (path, oflag, shflag, pmode)
SHIM(_wsopen) { char p[2048]; wx_w2u(A1, p, sizeof p); RET((u32)open_path(p, (int)A2, (int)A4 ? (int)A4 : 0x180)); }
SHIM(_sopen_s) { int fd = open_path((const char *)A2, (int)A3, (int)AN(5)); ST32(A1, (u32)fd); RET(fd < 0 ? (u64)rt_crt_errno : 0); }   // (*fd, path, oflag, shflag, pmode)
SHIM(_wsopen_s) { char p[2048]; wx_w2u(A2, p, sizeof p); int fd = open_path(p, (int)A3, (int)AN(5)); ST32(A1, (u32)fd); RET(fd < 0 ? (u64)rt_crt_errno : 0); }
SHIM(_dup) { int r = dup((int)A1); if (r < 0) SETERR(W_EBADF); RET((u32)r); }
SHIM(_dup2) { int r = dup2((int)A1, (int)A2); if (r < 0) { SETERR(W_EBADF); RET((u32)-1); } RET(0); }
SHIM(_pipe) { int p[2]; if (pipe(p)) { SETERR(map_errno(errno)); RET((u32)-1); } ST32(A1, (u32)p[0]); ST32(A1 + 4, (u32)p[1]); RET(0); }   // (fds[2], size, mode)
SHIM(_popen) { wx_stub_log("_popen"); SETERR(W_EINVAL); RET(0); }   // no Windows command interpreter here
SHIM(_wpopen) { wx_stub_log("_wpopen"); SETERR(W_EINVAL); RET(0); }
SHIM(_pclose) { SETERR(W_EINVAL); RET((u32)-1); }
SHIM(_chsize) { RET(ftruncate((int)A1, (off_t)(i32)A2) ? (SETERR(map_errno(errno)), (u64)(u32)-1) : 0); }
SHIM(_chsize_s) { RET(ftruncate((int)A1, (off_t)A2) ? (u64)SETERR(map_errno(errno)) : 0); }
SHIM(_eof) { int fd = (int)A1; off_t cur = lseek(fd, 0, SEEK_CUR), end = lseek(fd, 0, SEEK_END); lseek(fd, cur, SEEK_SET); RET(cur < 0 ? (u32)-1 : cur >= end); }
SHIM(_tell) { RET((u32)lseek((int)A1, 0, SEEK_CUR)); }
SHIM(_telli64) { RET((u64)lseek((int)A1, 0, SEEK_CUR)); }
SHIM(_commit) { RET(fsync((int)A1) ? (u32)-1 : 0); }
SHIM(_locking) { RET(0); }
SHIM(_umask) { RET((u32)umask(0) & 0x180); }
SHIM(_getpid) { RET((u32)getpid()); }
SHIM(_open_osfhandle) { intptr_t fd; if (rt_handle_get(A1, H_FILE, &fd)) RET((u32)fd); SETERR(W_EBADF); RET((u32)-1); }
static FILE *open_file(const char *win, const char *mode) {
    char host[4096], m[16]; rt_host_path(win, host, sizeof host);
    size_t k = 0; for (const char *p = mode; *p && k + 1 < sizeof m; p++) if (strchr("rwa+bx", *p)) m[k++] = *p;   // drop t, c, n, S, R, T, D, ccs=
    m[k] = 0;
    FILE *f = fopen(host, m);
    if (!f) SETERR(map_errno(errno));
    return f;
}
SHIM(fopen_s) { FILE *f = open_file((const char *)A2, (const char *)A3); ST64(A1, (u64)f); RET(f ? 0 : (u64)rt_crt_errno); }   // (FILE**, path, mode)
SHIM(_wfopen_s) { char p[2048], m[32]; wx_w2u(A2, p, sizeof p); wx_w2u(A3, m, sizeof m); FILE *f = open_file(p, m); ST64(A1, (u64)f); RET(f ? 0 : (u64)rt_crt_errno); }
SHIM(_fsopen) { RET(open_file((const char *)A1, (const char *)A2)); }   // (path, mode, shflag)
SHIM(_wfsopen) { char p[2048], m[32]; wx_w2u(A1, p, sizeof p); wx_w2u(A2, m, sizeof m); RET(open_file(p, m)); }
static u64 reopen(const char *win, const char *mode, u64 stream) {
    FILE *hf = rt_host_file(stream);
    char m[16]; size_t k = 0; for (const char *p = mode; *p && k + 1 < sizeof m; p++) if (strchr("rwa+bx", *p)) m[k++] = *p; m[k] = 0;
    FILE *r;
    if (win) { char host[4096]; rt_host_path(win, host, sizeof host); r = freopen(host, m, hf); }
    else r = freopen(NULL, m, hf);
    if (!r) { SETERR(map_errno(errno)); return 0; }
    return r == hf ? stream : (u64)r;
}
SHIM(freopen) { RET(reopen((const char *)A1, (const char *)A2, A3)); }   // (path, mode, stream)
SHIM(freopen_s) { u64 r = reopen((const char *)A2, (const char *)A3, A4); ST64(A1, r); RET(r ? 0 : (u64)rt_crt_errno); }   // (FILE**, path, mode, stream)
SHIM(_wfreopen_s) { char p[2048], m[32]; wx_w2u(A2, p, sizeof p); wx_w2u(A3, m, sizeof m); u64 r = reopen(p, m, A4); ST64(A1, r); RET(r ? 0 : (u64)rt_crt_errno); }
SHIM(setbuf) { FILE *f = rt_host_file(A1); if (A2) setvbuf(f, (char *)A2, _IOFBF, 512); else setvbuf(f, NULL, _IONBF, 0); RET(0); }   // Windows BUFSIZ is 512
SHIM(rewind) { rewind(rt_host_file(A1)); RET(0); }
static int put_wc(u16 w, FILE *f) { char b[8]; u16 t[2] = { w, 0 }; size_t n = wx_w2u((u64)t, b, sizeof b); return fwrite(b, 1, n, f) == n ? 0 : -1; }
SHIM(fputwc) { RET(put_wc((u16)A1, rt_host_file(A2)) ? 0xFFFF : (u16)A1); }
SHIM(_fputwc_nolock) { sh_fputwc(c); }
SHIM(fputws) { char b[8192]; wx_w2u(A1, b, sizeof b); RET(fputs(b, rt_host_file(A2)) < 0 ? (u32)-1 : 0); }
SHIM(_putws) { char b[8192]; wx_w2u(A1, b, sizeof b); RET(puts(b) < 0 ? (u32)-1 : 0); }
SHIM(putwchar) { RET(put_wc((u16)A1, stdout) ? 0xFFFF : (u16)A1); }
SHIM(fgetws) {  // (buf, n, FILE*): reads a line of UTF-8 and widens it
    FILE *f = rt_host_file(A3); int n = (int)A2; if (n <= 0) RET(0);
    char *tmp = malloc((size_t)n * 4 + 4); int k = 0, ch;
    while (k < n * 4 && (ch = fgetc(f)) != EOF) { tmp[k++] = (char)ch; if (ch == '\n') break; }
    if (!k) { free(tmp); RET(0); }
    tmp[k] = 0; wx_u2w(tmp, A1, (size_t)n); free(tmp); RET(A1);
}
SHIM(getchar) { RET((u32)getchar()); }
SHIM(getwchar) { RET((u32)getchar()); }
SHIM(_getch) { RET((u32)getchar()); }
SHIM(_kbhit) { RET(0); }
SHIM(gets_s) { char *b = (char *)A1; if (!fgets(b, (int)A2, stdin)) RET(0); size_t l = strlen(b); if (l && b[l - 1] == '\n') b[l - 1] = 0; RET(A1); }
SHIM(tmpfile) { RET(tmpfile()); }
SHIM(tmpfile_s) { FILE *f = tmpfile(); ST64(A1, (u64)f); RET(f ? 0 : W_EACCES); }
static u32 tmp_counter;
SHIM(tmpnam) {  // "\sXXXX." names like UCRT, inside C:\tmp so the host path is writable
    static char sbuf[260]; char *b = A1 ? (char *)A1 : sbuf;
    snprintf(b, 260, "C:\\tmp\\s%x.%u", (unsigned)getpid() & 0xffff, __atomic_add_fetch(&tmp_counter, 1, __ATOMIC_SEQ_CST));
    char host[2048]; rt_host_path("C:\\tmp", host, sizeof host); mkdir(host, 0755);
    RET(b);
}
SHIM(tmpnam_s) { char b[260]; snprintf(b, sizeof b, "C:\\tmp\\s%x.%u", (unsigned)getpid() & 0xffff, __atomic_add_fetch(&tmp_counter, 1, __ATOMIC_SEQ_CST)); if (strlen(b) + 1 > A2) RET(W_ERANGE); strcpy((char *)A1, b); RET(0); }
SHIM(_tempnam) {  // (dir, prefix) -> malloc'd name
    const char *dir = A1 && *(const char *)A1 ? (const char *)A1 : "C:\\tmp";
    char *b = malloc(1024);
    snprintf(b, 1024, "%s%s%s%x%u", dir, dir[strlen(dir) - 1] == '\\' ? "" : "\\", A2 ? (const char *)A2 : "", (unsigned)getpid() & 0xffff, __atomic_add_fetch(&tmp_counter, 1, __ATOMIC_SEQ_CST));
    RET(b);
}
static int mktemp_(char *t) {
    size_t l = strlen(t); if (l < 6 || strcmp(t + l - 6, "XXXXXX")) return W_EINVAL;
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    char *x = t + l - 6;
    snprintf(x, 6, "%05u", (unsigned)getpid() % 100000);
    for (int i = 0; i < 26; i++) {
        x[5] = letters[i];
        char host[4096]; rt_host_path(t, host, sizeof host); struct stat st;
        if (stat(host, &st)) return 0;
    }
    return W_EEXIST;
}
SHIM(_mktemp_s) { if (!A1 || strnlen((char *)A1, A2) >= A2) RET(SETERR(W_EINVAL)); int r = mktemp_((char *)A1); if (r) { ST8(A1, 0); RET(SETERR(r)); } RET(0); }
SHIM(_mktemp) { RET(mktemp_((char *)A1) ? 0 : A1); }
SHIM(_setmaxstdio) { RET((u32)A1 <= 8192 ? (u32)A1 : (u32)-1); }
SHIM(_getmaxstdio) { RET(512); }
// UCRT's FILE internals as msvcp140's basic_filebuf sees them: an always-empty buffer, so every read and write
// goes through fgetc/fputc/fread/fwrite on the real stream.
typedef struct { u64 f; u64 base, ptr; i32 cnt; } StreamBuf;
static StreamBuf sbufs[256]; static pthread_mutex_t sb_lock = PTHREAD_MUTEX_INITIALIZER;
SHIM(_get_stream_buffer_pointers) {  // (FILE*, char ***base, char ***ptr, int **count)
    pthread_mutex_lock(&sb_lock);
    StreamBuf *b = NULL;
    for (int i = 0; i < 256 && !b; i++) if (sbufs[i].f == A1) b = &sbufs[i];
    for (int i = 0; i < 256 && !b; i++) if (!sbufs[i].f) { b = &sbufs[i]; b->f = A1; }
    if (!b) b = &sbufs[(A1 >> 4) & 255];
    b->base = b->ptr = 0; b->cnt = 0;
    pthread_mutex_unlock(&sb_lock);
    if (A2) ST64(A2, (u64)&b->base); if (A3) ST64(A3, (u64)&b->ptr); if (A4) ST64(A4, (u64)&b->cnt);
    RET(0);
}
SHIM(_fseek_nolock) { RET(fseek(rt_host_file(A1), (long)(i32)A2, (int)A3)); }
SHIM(_ftell_nolock) { RET((u32)ftell(rt_host_file(A1))); }
SHIM(_fseeki64_nolock) { RET(fseeko(rt_host_file(A1), (off_t)A2, (int)A3)); }
SHIM(_ftelli64_nolock) { RET(ftello(rt_host_file(A1))); }
SHIM(_fread_nolock) { RET(fread((void *)A1, A2, A3, rt_host_file(A4))); }
SHIM(_fwrite_nolock) { RET(fwrite((void *)A1, A2, A3, rt_host_file(A4))); }
SHIM(_fflush_nolock) { RET(fflush(rt_host_file(A1))); }
SHIM(_fclose_nolock) { RET(fclose(rt_host_file(A1))); }
SHIM(_getc_nolock) { RET((u32)getc(rt_host_file(A1))); }
SHIM(_putc_nolock) { RET((u32)putc((int)A1, rt_host_file(A2))); }
SHIM(_ungetc_nolock) { RET((u32)ungetc((int)A1, rt_host_file(A2))); }
SHIM(_fcloseall) { RET(0); }
SHIM(_flushall) { fflush(NULL); RET(0); }
SHIM(_rmtmp) { RET(0); }
SHIM(perror) {
    const char *s = (const char *)A1;
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(rt_crt_errno)); else fprintf(stderr, "%s\n", strerror(rt_crt_errno));
    RET(0);
}

// ==== stat / paths / directories =========================================================================================
// Windows st_mode: type | read | write | exec (directories and .exe/.com/.bat/.cmd), replicated to group/other.
static u16 win_mode(const struct stat *st, const char *path) {
    u16 m = S_ISDIR(st->st_mode) ? 0x4000 : 0x8000;
    u16 p = 0x100; if (st->st_mode & S_IWUSR) p |= 0x80;
    const char *dot = strrchr(path, '.');
    if (S_ISDIR(st->st_mode) || (dot && (!strcasecmp(dot, ".exe") || !strcasecmp(dot, ".com") || !strcasecmp(dot, ".bat") || !strcasecmp(dot, ".cmd")))) p |= 0x40;
    return m | p | p >> 3 | p >> 6;
}
// layout: 0 = _stat32 (36 B), 1 = _stat64i32 (48 B), 2 = _stat64 (56 B), 3 = _stat32i64 (48 B)
static void fill_stat(u64 b, const struct stat *st, const char *path, int layout) {
    size_t sz = layout == 0 ? 36 : layout == 2 ? 56 : 48;
    memset((void *)b, 0, sz);
    ST32(b, 2); ST16(b + 6, win_mode(st, path)); ST16(b + 8, 1); ST32(b + 16, 2);   // dev / rdev = drive C (2)
    u64 size = S_ISDIR(st->st_mode) ? 0 : (u64)st->st_size;
    i64 t[3] = { st->st_atime, st->st_mtime, st->st_ctime };
    if (layout == 0) { ST32(b + 20, (u32)size); for (int i = 0; i < 3; i++) ST32(b + 24 + 4 * i, (u32)t[i]); }
    else if (layout == 1) { ST32(b + 20, (u32)size); for (int i = 0; i < 3; i++) ST64(b + 24 + 8 * i, (u64)t[i]); }
    else if (layout == 2) { ST64(b + 24, size); for (int i = 0; i < 3; i++) ST64(b + 32 + 8 * i, (u64)t[i]); }
    else { ST64(b + 24, size); for (int i = 0; i < 3; i++) ST32(b + 32 + 4 * i, (u32)t[i]); }
}
static u64 stat_path(const char *win, u64 buf, int layout) {
    char host[4096]; rt_host_path(win, host, sizeof host);
    size_t l = strlen(win);
    if (l > 3 && (win[l - 1] == '\\' || win[l - 1] == '/')) { SETERR(W_ENOENT); return (u64)(u32)-1; }   // Windows: trailing slash fails except roots
    struct stat st; if (stat(host, &st)) { SETERR(map_errno(errno)); return (u64)(u32)-1; }
    fill_stat(buf, &st, host, layout); return 0;
}
static u64 stat_w(u64 w, u64 buf, int layout) { char p[2048]; wx_w2u(w, p, sizeof p); return stat_path(p, buf, layout); }
SHIM(_stat32) { RET(stat_path((const char *)A1, A2, 0)); }
SHIM(_stat64i32) { RET(stat_path((const char *)A1, A2, 1)); }
SHIM(_stat64) { RET(stat_path((const char *)A1, A2, 2)); }
SHIM(_stat32i64) { RET(stat_path((const char *)A1, A2, 3)); }
SHIM(_wstat32) { RET(stat_w(A1, A2, 0)); }
SHIM(_wstat64i32) { RET(stat_w(A1, A2, 1)); }
SHIM(_wstat64) { RET(stat_w(A1, A2, 2)); }
SHIM(_wstat32i64) { RET(stat_w(A1, A2, 3)); }
static u64 fstat_(int fd, u64 buf, int layout) {
    struct stat st; if (fstat(fd, &st)) { SETERR(W_EBADF); return (u64)(u32)-1; }
    char p[1024] = ""; fcntl(fd, F_GETPATH, p); fill_stat(buf, &st, p, layout); return 0;
}
SHIM(_fstat32) { RET(fstat_((int)A1, A2, 0)); }
SHIM(_fstat64i32) { RET(fstat_((int)A1, A2, 1)); }
SHIM(_fstat32i64) { RET(fstat_((int)A1, A2, 3)); }
static u64 path_op(const char *win, int op, int arg) {   // 0 unlink, 1 mkdir, 2 rmdir, 3 access, 4 chmod
    char host[4096]; rt_host_path(win, host, sizeof host); int r;
    switch (op) {
    case 0: { struct stat st; if (!stat(host, &st) && S_ISDIR(st.st_mode)) { SETERR(W_EACCES); return (u64)(u32)-1; } r = unlink(host); break; }
    case 1: r = mkdir(host, 0755); break;
    case 2: r = rmdir(host); if (r && errno == ENOTEMPTY) { SETERR(41); return (u64)(u32)-1; } break;   // Windows ENOTEMPTY is 41
    case 3: { struct stat st; r = stat(host, &st); if (!r && (arg & 2) && !(st.st_mode & S_IWUSR)) { SETERR(W_EACCES); return (u64)(u32)-1; } break; }
    default: { struct stat st; r = stat(host, &st); if (!r) r = chmod(host, (arg & 0x80) ? (st.st_mode | S_IWUSR) & 07777 : st.st_mode & ~(mode_t)0222 & 07777); break; }
    }
    if (r) { SETERR(map_errno(errno)); return (u64)(u32)-1; }
    return 0;
}
static u64 path_op_w(u64 w, int op, int arg) { char p[2048]; wx_w2u(w, p, sizeof p); return path_op(p, op, arg); }
SHIM(_unlink) { RET(path_op((const char *)A1, 0, 0)); }
SHIM(_wunlink) { RET(path_op_w(A1, 0, 0)); }
SHIM(_mkdir) { RET(path_op((const char *)A1, 1, 0)); }
SHIM(_wmkdir) { RET(path_op_w(A1, 1, 0)); }
SHIM(_rmdir) { RET(path_op((const char *)A1, 2, 0)); }
SHIM(_wrmdir) { RET(path_op_w(A1, 2, 0)); }
SHIM(_access) { RET(path_op((const char *)A1, 3, (int)A2)); }
SHIM(_waccess) { RET(path_op_w(A1, 3, (int)A2)); }
SHIM(_access_s) { RET(path_op((const char *)A1, 3, (int)A2) ? (u64)rt_crt_errno : 0); }
SHIM(_waccess_s) { RET(path_op_w(A1, 3, (int)A2) ? (u64)rt_crt_errno : 0); }
SHIM(_chmod) { RET(path_op((const char *)A1, 4, (int)A2)); }
SHIM(_wchmod) { RET(path_op_w(A1, 4, (int)A2)); }
SHIM(_wrename) { char a[2048], b[2048], ha[4096], hb[4096]; wx_w2u(A1, a, sizeof a); wx_w2u(A2, b, sizeof b); rt_host_path(a, ha, sizeof ha); rt_host_path(b, hb, sizeof hb);
                 if (rename(ha, hb)) { SETERR(map_errno(errno)); RET((u32)-1); } RET(0); }
SHIM(_chdir) { if (!call4(c, "SetCurrentDirectoryA", A1, 0, 0, 0)) { SETERR(W_ENOENT); RET((u32)-1); } RET(0); }
SHIM(_wchdir) { if (!call4(c, "SetCurrentDirectoryW", A1, 0, 0, 0)) { SETERR(W_ENOENT); RET((u32)-1); } RET(0); }
static u64 getcwd_(u64 buf, int n, int wide) {   // NULL buffer: malloc max(n, needed)
    const char *cwd = rt_win_cwd();
    size_t need = wide ? wx_u2w(cwd, 0, 0) : strlen(cwd) + 1;
    if (!buf) { size_t sz = (size_t)n > need ? (size_t)n : need; buf = (u64)malloc(wide ? 2 * sz : sz); n = (int)sz; }
    else if ((size_t)n < need) { SETERR(W_ERANGE); return 0; }
    if (wide) wx_u2w(cwd, buf, (size_t)n); else memcpy((void *)buf, cwd, need);
    return buf;
}
SHIM(_getcwd) { RET(getcwd_(A1, (int)A2, 0)); }
SHIM(_wgetcwd) { RET(getcwd_(A1, (int)A2, 1)); }
SHIM(_getdcwd) { RET(getcwd_(A2, (int)A3, 0)); }   // (drive, buf, n)
SHIM(_wgetdcwd) { RET(getcwd_(A2, (int)A3, 1)); }
SHIM(_getdrive) { RET((u32)((rt_win_cwd()[0] | 32) - 'a' + 1)); }
SHIM(_chdrive) { RET((u32)A1 == 3 ? 0 : (u32)-1); }
static u64 fullpath_(u64 buf, u64 rel, size_t n, int wide) {
    char in[2048], out[4096];
    if (wide) wx_w2u(rel, in, sizeof in); else snprintf(in, sizeof in, "%s", (const char *)rel);
    rt_full_path(rel ? in : ".", out, sizeof out);
    size_t need = wide ? wx_u2w(out, 0, 0) : strlen(out) + 1;
    if (!buf) { n = need > 260 ? need : 260; buf = (u64)malloc(wide ? 2 * n : n); }
    else if (n < need) { SETERR(W_ERANGE); return 0; }
    if (wide) wx_u2w(out, buf, n); else memcpy((void *)buf, out, need);
    return buf;
}
SHIM(_fullpath) { RET(fullpath_(A1, A2, A3, 0)); }
SHIM(_wfullpath) { RET(fullpath_(A1, A2, A3, 1)); }
// _splitpath: drive "C:", dir up to and including the last separator, fname, ext (from the last dot).
static void split(const char *p, char *dr, char *di, char *fn, char *ex) {
    *dr = *di = *fn = *ex = 0;
    if (p[0] && p[1] == ':') { dr[0] = p[0]; dr[1] = ':'; dr[2] = 0; p += 2; }
    const char *ls = NULL; for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') ls = q;
    if (ls) { memcpy(di, p, (size_t)(ls - p + 1)); di[ls - p + 1] = 0; p = ls + 1; }
    const char *dot = strrchr(p, '.');
    if (dot) { memcpy(fn, p, (size_t)(dot - p)); fn[dot - p] = 0; strcpy(ex, dot); } else strcpy(fn, p);
}
static int put_part(const char *s, u64 dst, size_t n, int wide) {
    if (!dst) return 0;
    size_t need = wide ? wx_u2w(s, 0, 0) : strlen(s) + 1;
    if (n < need) return W_ERANGE;
    if (wide) wx_u2w(s, dst, n); else memcpy((void *)dst, s, need);
    return 0;
}
static u64 splitpath_s(Ctx *c, int wide) {   // (path, drive, dn, dir, dirn, fname, fn, ext, en)
    char in[2048], dr[8], di[2048], fn[1024], ex[1024];
    if (!A1) return (u64)SETERR(W_EINVAL);
    if (wide) wx_w2u(A1, in, sizeof in); else snprintf(in, sizeof in, "%s", (const char *)A1);
    split(in, dr, di, fn, ex);
    u64 dsts[4] = { A2, A4, AN(6), AN(8) }; size_t ns[4] = { A3, AN(5), AN(7), AN(9) }; const char *parts[4] = { dr, di, fn, ex };
    for (int i = 0; i < 4; i++) if (put_part(parts[i], dsts[i], ns[i], wide)) {
        for (int k = 0; k < 4; k++) if (dsts[k] && ns[k]) { if (wide) ST16(dsts[k], 0); else ST8(dsts[k], 0); }
        return (u64)SETERR(W_ERANGE);
    }
    return 0;
}
SHIM(_splitpath_s) { RET(splitpath_s(c, 0)); }
SHIM(_wsplitpath_s) { RET(splitpath_s(c, 1)); }
SHIM(_splitpath) {  // (path, drive, dir, fname, ext): buffers assumed _MAX_* sized
    char dr[8], di[2048], fn[1024], ex[1024]; split((const char *)A1, dr, di, fn, ex);
    if (A2) strcpy((char *)A2, dr); if (A3) strcpy((char *)A3, di); if (A4) strcpy((char *)A4, fn); if (AN(5)) strcpy((char *)AN(5), ex);
    RET(0);
}
static void makepath(char *out, size_t n, const char *dr, const char *di, const char *fn, const char *ex) {
    size_t o = 0; out[0] = 0;
    if (dr && *dr) o += (size_t)snprintf(out + o, n - o, "%c:", dr[0]);
    if (di && *di) { o += (size_t)snprintf(out + o, n - o, "%s", di); char l = di[strlen(di) - 1]; if (l != '\\' && l != '/') o += (size_t)snprintf(out + o, n - o, "\\"); }
    if (fn && *fn) o += (size_t)snprintf(out + o, n - o, "%s", fn);
    if (ex && *ex) o += (size_t)snprintf(out + o, n - o, "%s%s", ex[0] == '.' ? "" : ".", ex);
}
SHIM(_makepath) { char b[4096]; makepath(b, sizeof b, (char *)A2, (char *)A3, (char *)A4, (char *)AN(5)); strcpy((char *)A1, b); RET(0); }
SHIM(_makepath_s) { char b[4096]; makepath(b, sizeof b, (char *)A3, (char *)A4, (char *)AN(5), (char *)AN(6)); if (strlen(b) + 1 > A2) { if (A2) ST8(A1, 0); RET(SETERR(W_ERANGE)); } strcpy((char *)A1, b); RET(0); }
// _findfirst & co. on FindFirstFileA/W. Layouts: _finddata64i32_t { attrib, i64 create/access/write @8, u32 size @32, name[260] @36 },
// _finddata64_t: size i64 @32, name @40; _finddata32_t: times i32 @4/8/12, size @16, name @20. Wide: name is WCHAR[260].
typedef struct { u64 h; int wide; } FindH;
static void fill_fd(u64 out, u64 wfd, int wide, int layout) {   // layout 0: 32, 1: 64i32, 2: 64
    u32 attr = LD32(wfd); u64 t[3] = { LD64(wfd + 4), LD64(wfd + 12), LD64(wfd + 20) };
    u64 size = (u64)LD32(wfd + 28) << 32 | LD32(wfd + 32);
    i64 tt[3]; for (int i = 0; i < 3; i++) tt[i] = t[i] ? (i64)(t[i] / 10000000ull) - 11644473600ll : -1;
    u64 name_off = layout == 0 ? 20 : layout == 1 ? 36 : 40;
    memset((void *)out, 0, name_off + (wide ? 520 : 260));
    ST32(out, attr);
    if (layout == 0) { for (int i = 0; i < 3; i++) ST32(out + 4 + 4 * i, (u32)tt[i]); ST32(out + 16, (u32)size); }
    else { for (int i = 0; i < 3; i++) ST64(out + 8 + 8 * i, (u64)tt[i]); if (layout == 1) ST32(out + 32, (u32)size); else ST64(out + 32, size); }
    memcpy((void *)(out + name_off), (void *)(wfd + 44), wide ? 520 : 260);
}
static u64 findfirst_(Ctx *c, int wide, int layout) {   // (spec, data) -> intptr handle or -1
    u8 wfd[600];
    u64 h = call4(c, wide ? "FindFirstFileW" : "FindFirstFileA", A1, (u64)wfd, 0, 0);
    if (h == ~0ull) { SETERR(W_ENOENT); return ~0ull; }
    fill_fd(A2, (u64)wfd, wide, layout);
    return h;
}
static u64 findnext_(Ctx *c, int wide, int layout) {
    u8 wfd[600];
    if (!call4(c, wide ? "FindNextFileW" : "FindNextFileA", A1, (u64)wfd, 0, 0)) { SETERR(W_ENOENT); return (u64)(u32)-1; }
    fill_fd(A2, (u64)wfd, wide, layout); return 0;
}
SHIM(_findfirst32) { RET(findfirst_(c, 0, 0)); }
SHIM(_findfirst64i32) { RET(findfirst_(c, 0, 1)); }
SHIM(_findfirst64) { RET(findfirst_(c, 0, 2)); }
SHIM(_wfindfirst32) { RET(findfirst_(c, 1, 0)); }
SHIM(_wfindfirst64i32) { RET(findfirst_(c, 1, 1)); }
SHIM(_wfindfirst64) { RET(findfirst_(c, 1, 2)); }
SHIM(_findnext32) { RET(findnext_(c, 0, 0)); }
SHIM(_findnext64i32) { RET(findnext_(c, 0, 1)); }
SHIM(_findnext64) { RET(findnext_(c, 0, 2)); }
SHIM(_wfindnext32) { RET(findnext_(c, 1, 0)); }
SHIM(_wfindnext64i32) { RET(findnext_(c, 1, 1)); }
SHIM(_wfindnext64) { RET(findnext_(c, 1, 2)); }
SHIM(_findclose) { RET(call4(c, "FindClose", A1, 0, 0, 0) ? 0 : (u32)-1); }

// ==== environment / arguments ===========================================================================================
static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 wide_dup(const char *s) { size_t n = wx_u2w(s, 0, 0); u64 w = (u64)malloc(2 * n); wx_u2w(s, w, n); return w; }
SHIM(_wgetenv) {  // the returned string stays valid (one cached copy per variable value)
    static struct { char name[128]; char *val; u64 w; } cache[128]; static int ncache;
    char nm[256]; wx_w2u(A1, nm, sizeof nm);
    const char *v = getenv(nm); if (!v) RET(0);
    pthread_mutex_lock(&env_lock);
    for (int i = 0; i < ncache; i++) if (!strcmp(cache[i].name, nm) && !strcmp(cache[i].val, v)) { u64 r = cache[i].w; pthread_mutex_unlock(&env_lock); RET(r); }
    u64 w = wide_dup(v);
    if (ncache < 128) { snprintf(cache[ncache].name, 128, "%s", nm); cache[ncache].val = strdup(v); cache[ncache].w = w; ncache++; }
    pthread_mutex_unlock(&env_lock);
    RET(w);
}
SHIM(getenv_s) {  // (*required, buf, n, name)
    const char *v = getenv((const char *)A4);
    if (!v) { if (A1) ST64(A1, 0); if (A2 && A3) ST8(A2, 0); RET(0); }
    size_t need = strlen(v) + 1; if (A1) ST64(A1, need);
    if (!A2 || !A3) RET(0);
    if (A3 < need) RET(SETERR(W_ERANGE));
    memcpy((void *)A2, v, need); RET(0);
}
SHIM(_wgetenv_s) {
    char nm[256]; wx_w2u(A4, nm, sizeof nm); const char *v = getenv(nm);
    if (!v) { if (A1) ST64(A1, 0); if (A2 && A3) ST16(A2, 0); RET(0); }
    size_t need = wx_u2w(v, 0, 0); if (A1) ST64(A1, need);
    if (!A2 || !A3) RET(0);
    if (A3 < need) RET(SETERR(W_ERANGE));
    wx_u2w(v, A2, A3); RET(0);
}
SHIM(_dupenv_s) {  // (char **buf, size_t *n, name)
    const char *v = getenv((const char *)A3);
    if (!v) { ST64(A1, 0); if (A2) ST64(A2, 0); RET(0); }
    ST64(A1, (u64)strdup(v)); if (A2) ST64(A2, strlen(v) + 1); RET(0);
}
SHIM(_wdupenv_s) {
    char nm[256]; wx_w2u(A3, nm, sizeof nm); const char *v = getenv(nm);
    if (!v) { ST64(A1, 0); if (A2) ST64(A2, 0); RET(0); }
    ST64(A1, wide_dup(v)); if (A2) ST64(A2, wx_u2w(v, 0, 0)); RET(0);
}
SHIM(_wputenv) { char b[4096]; wx_w2u(A1, b, sizeof b); char *eq = strchr(b, '='); if (!eq) RET((u32)-1); *eq = 0; if (eq[1]) setenv(b, eq + 1, 1); else unsetenv(b); RET(0); }
SHIM(_wputenv_s) { char n[256], v[4096]; wx_w2u(A1, n, sizeof n); wx_w2u(A2, v, sizeof v); if (*v) setenv(n, v, 1); else unsetenv(n); RET(0); }
static u64 g_wargv, g_wenv; static int g_wargc;
static void build_wide(void) {
    pthread_mutex_lock(&env_lock);
    if (!g_wargv) {
        u64 *av = calloc((size_t)rt_argc + 1, 8);
        for (int i = 0; i < rt_argc; i++) av[i] = wide_dup(rt_argv[i]);
        g_wargc = rt_argc; g_wargv = (u64)av;
        extern char **environ; int n = 0; while (environ[n]) n++;
        u64 *ev = calloc((size_t)n + 1, 8); for (int i = 0; i < n; i++) ev[i] = wide_dup(environ[i]);
        g_wenv = (u64)ev;
    }
    pthread_mutex_unlock(&env_lock);
}
SHIM(__p___wargv) { build_wide(); RET(&g_wargv); }
SHIM(__p__wenviron) { build_wide(); RET(&g_wenv); }
SHIM(_configure_wide_argv) { build_wide(); RET(0); }
SHIM(_initialize_wide_environment) { build_wide(); RET(0); }
SHIM(_get_initial_wide_environment) { build_wide(); RET(g_wenv); }
SHIM(_get_initial_narrow_environment) { extern char **environ; RET(environ); }
SHIM(__wgetmainargs) { build_wide(); ST32(A1, (u32)g_wargc); ST64(A2, g_wargv); ST64(A3, g_wenv); RET(0); }   // (*argc, *wargv, *wenv, expand, startinfo)
SHIM(_get_wide_winmain_command_line) {
    static u64 w; if (!w) { size_t n = 1; for (int i = 1; i < rt_argc; i++) n += strlen(rt_argv[i]) + 3;
        char *s = calloc(1, n); for (int i = 1; i < rt_argc; i++) { if (i > 1) strcat(s, " "); strcat(s, rt_argv[i]); } w = wide_dup(s); free(s); }
    RET(w);
}
SHIM(__p__wcmdln) { static u64 p; if (!p) p = call4(c, "GetCommandLineW", 0, 0, 0, 0); RET(&p); }
SHIM(__p___initenv) { extern char **environ; static u64 e; e = (u64)environ; RET(&e); }
SHIM(__p___winitenv) { build_wide(); RET(&g_wenv); }
SHIM(_get_pgmptr) { static char p[1100]; if (!*p) call4(c, "GetModuleFileNameA", 0, (u64)p, sizeof p, 0); ST64(A1, (u64)p); RET(0); }
SHIM(_get_wpgmptr) { static u16 p[1100]; if (!*p) call4(c, "GetModuleFileNameW", 0, (u64)p, 1100, 0); ST64(A1, (u64)p); RET(0); }

// ==== errors / runtime ===================================================================================================
SHIM(_set_errno) { rt_crt_errno = (int)A1; RET(0); }
SHIM(_get_errno) { if (!A1) RET(W_EINVAL); ST32(A1, (u32)rt_crt_errno); RET(0); }
SHIM(_set_doserrno) { RET(0); }
SHIM(_get_doserrno) { if (A1) ST32(A1, 0); RET(0); }
static const char *win_strerror(int e) { return e == 41 ? "Directory not empty" : e == W_STRUNCATE ? "Truncated" : strerror(e); }
SHIM(strerror_s) {  // (buf, n, errnum)
    if (!A1 || !A2) RET(W_EINVAL);
    const char *s = win_strerror((int)A3); size_t l = strlen(s);
    if (l + 1 > A2) { memcpy((void *)A1, s, A2 - 1); ST8(A1 + A2 - 1, 0); RET(W_ERANGE); }
    memcpy((void *)A1, s, l + 1); RET(0);
}
SHIM(_strerror) { static __thread char b[512]; const char *e = win_strerror(rt_crt_errno); if (A1) snprintf(b, sizeof b, "%s: %s\n", (char *)A1, e); else snprintf(b, sizeof b, "%s\n", e); RET(b); }
SHIM(_wcserror) { static __thread u16 w[256]; wx_u2w(win_strerror((int)A1), (u64)w, 256); RET(w); }
SHIM(_wcserror_s) { if (!A1 || !A2) RET(W_EINVAL); wx_u2w(win_strerror((int)A3), A1, A2); RET(0); }
SHIM(__sys_nerr) { static int n = 43; RET(&n); }
SHIM(__sys_errlist) { static u64 list[43]; if (!list[1]) for (int i = 0; i < 43; i++) list[i] = (u64)strdup(win_strerror(i)); RET(list); }
SHIM(_seh_filter_dll) { RET(0); }   // EXCEPTION_CONTINUE_SEARCH
static void die(const char *why) { fflush(NULL); rt_log("%s: terminating", why); _exit(0xC0000409 & 0xFF); }
SHIM(_invoke_watson) { die("_invoke_watson"); }
SHIM(_invalid_parameter_noinfo) { die("invalid parameter passed to a CRT function"); }
SHIM(_invalid_parameter_noinfo_noreturn) { die("invalid parameter passed to a CRT function"); }
SHIM(_invalid_parameter) { die("invalid parameter passed to a CRT function"); }
SHIM(_Exit) { fflush(NULL); _exit((int)A1); }
SHIM(quick_exit) { fflush(NULL); _exit((int)A1); }
SHIM(_crt_at_quick_exit) { RET(0); }
SHIM(at_quick_exit) { RET(0); }
SHIM(system) { if (!A1) RET(0); SETERR(W_ENOENT); RET((u32)-1); }   // no command interpreter
SHIM(_wsystem) { if (!A1) RET(0); SETERR(W_ENOENT); RET((u32)-1); }
SHIM(_callnewh) { RET(0); }   // no new handler installed: operator new reports failure
SHIM(_query_new_handler) { RET(0); }
SHIM(_set_new_handler) { RET(0); }
SHIM(_query_new_mode) { RET(0); }
SHIM(raise) { int s = (int)A1; if (s == 22 || s == 2 || s == 15) { fflush(NULL); rt_log("raise(%d)", s); _exit(3); } RET(0); }   // SIGABRT / SIGINT / SIGTERM default actions terminate
SHIM(_set_error_mode) { RET(0); }
SHIM(_set_abort_behavior) { RET(0); }
SHIM(__threadid) { RET(LD32(c->gs_base + 0x48)); }
SHIM(__threadhandle) { RET((u64)-2); }
SHIM(_beginthread) {  // (start(void*), stack, arg): via CreateThread; the handle is returned like _beginthread does
    u64 h = 0; ShimFn f = shim_fn("_beginthreadex");
    if (!f) { extern const Shim shims_threads[]; for (const Shim *s = shims_threads; s->name; s++) if (!strcmp(s->name, "_beginthreadex")) f = s->fn; }
    u64 sp = c->r[RSP]; u64 save[3] = { LD64(sp + 40), LD64(sp + 48), LD64(sp + 56) };
    u64 s1 = c->r[RCX], s2 = c->r[RDX], s3 = c->r[R8], s4 = c->r[R9];
    c->r[RCX] = 0; c->r[RDX] = s2; c->r[R8] = s1; c->r[R9] = s3; ST64(sp + 40, 0); ST64(sp + 48, 0);
    f(c); h = c->r[RAX];
    c->r[RCX] = s1; c->r[RDX] = s2; c->r[R8] = s3; c->r[R9] = s4; ST64(sp + 40, save[0]); ST64(sp + 48, save[1]); ST64(sp + 56, save[2]);
    RET(h ? h : (u64)-1);
}
SHIM(_endthread) { extern const Shim shims_threads[]; for (const Shim *s = shims_threads; s->name; s++) if (!strcmp(s->name, "ExitThread")) { c->r[RCX] = 0; s->fn(c); } RET(0); }

// ==== heap ================================================================================================================
extern size_t malloc_size(const void *);
SHIM(_aligned_realloc) {  // (p, size, align)
    if (!A1) { void *p = NULL; if (posix_memalign(&p, A3 < 8 ? 8 : A3, A2 ? A2 : 1)) p = NULL; RET(p); }
    if (!A2) { free((void *)A1); RET(0); }
    void *p = NULL; if (posix_memalign(&p, A3 < 8 ? 8 : A3, A2)) RET(0);
    size_t old = malloc_size((void *)A1); memcpy(p, (void *)A1, old < A2 ? old : A2); free((void *)A1); RET(p);
}
SHIM(_aligned_recalloc) { size_t n = A2 * A3; u64 old = A1 ? malloc_size((void *)A1) : 0; u64 sv = c->r[RDX], s3 = c->r[R8]; c->r[RDX] = n; c->r[R8] = A4; sh__aligned_realloc(c); c->r[RDX] = sv; c->r[R8] = s3;
                        u64 p = c->r[RAX]; if (p && n > old) memset((void *)(p + old), 0, n - old); RET(p); }
SHIM(_aligned_msize) { RET(A1 ? malloc_size((void *)A1) : (u64)-1); }
SHIM(_aligned_offset_malloc) {  // (size, align, offset): pointer + offset is aligned
    size_t al = A2 < 8 ? 8 : A2; u8 *raw = malloc(A1 + al + A3 + 16); if (!raw) RET(0);
    u64 p = ((u64)raw + 16 + A3 + al - 1) & ~(u64)(al - 1); p -= A3;
    ST64(p - 8, (u64)raw); RET(p);
}
SHIM(_aligned_offset_free) { if (A1) free((void *)LD64(A1 - 8)); RET(0); }
SHIM(_recalloc) { size_t n = A2 * A3, old = A1 ? malloc_size((void *)A1) : 0; u8 *p = realloc((void *)A1, n ? n : 1); if (p && n > old) memset(p + old, 0, n - old); RET(p); }
SHIM(_expand) { RET(A1 && A2 <= malloc_size((void *)A1) ? A1 : 0); }
SHIM(_heapchk) { RET((u32)-2); }   // _HEAPOK
SHIM(_heapmin) { RET(0); }
SHIM(_get_heap_handle) { RET(0x7E000000); }
SHIM(_malloc_base) { RET(malloc(A1 ? A1 : 1)); }
SHIM(_calloc_base) { RET(calloc(A1, A2)); }
SHIM(_realloc_base) { RET(realloc((void *)A1, A2)); }
SHIM(_free_base) { free((void *)A1); RET(0); }
// qsort_s / bsearch_s: comparator(context, a, b)
typedef struct { Ctx *c; u64 fn, ctx; } SCmp;
static int scmp(void *p, const void *a, const void *b) { SCmp *g = p; u64 args[3] = { g->ctx, (u64)a, (u64)b }; return (int)(i32)rt_guest_call(g->c, g->fn, 3, args); }
SHIM(qsort_s) { SCmp g = { c, A4, AN(5) }; qsort_r((void *)A1, A2, A3, &g, scmp); RET(0); }   // (base, n, size, cmp, ctx)
SHIM(bsearch_s) {  // (key, base, n, size, cmp, ctx)
    SCmp g = { c, AN(5), AN(6) }; size_t lo = 0, hi = A3;
    while (lo < hi) { size_t mid = (lo + hi) / 2; u64 e = A2 + mid * A4; int r = scmp(&g, (void *)A1, (void *)e); if (!r) RET(e); if (r < 0) hi = mid; else lo = mid + 1; }
    RET(0);
}
SHIM(_lfind) {  // (key, base, *num, width, cmp(a, b))
    u32 n = LD32(A3);
    for (u32 i = 0; i < n; i++) { u64 e = A2 + (u64)i * A4, args[2] = { A1, e }; if (!(i32)rt_guest_call(c, AN(5), 2, args)) RET(e); }
    RET(0);
}
SHIM(_rotl) { u32 v = (u32)A1, s = (u32)A2 & 31; RET(s ? (v << s | v >> (32 - s)) : v); }
SHIM(_rotr) { u32 v = (u32)A1, s = (u32)A2 & 31; RET(s ? (v >> s | v << (32 - s)) : v); }
SHIM(_rotl64) { u64 v = A1; u32 s = (u32)A2 & 63; RET(s ? (v << s | v >> (64 - s)) : v); }
SHIM(_rotr64) { u64 v = A1; u32 s = (u32)A2 & 63; RET(s ? (v >> s | v << (64 - s)) : v); }
SHIM(_byteswap_ushort) { RET(__builtin_bswap16((u16)A1)); }
SHIM(_byteswap_ulong) { RET(__builtin_bswap32((u32)A1)); }
SHIM(_byteswap_uint64) { RET(__builtin_bswap64(A1)); }
SHIM(div) { i32 a = (i32)A1, b = (i32)A2; RET(((u64)(u32)(a % b) << 32) | (u32)(a / b)); }   // div_t returned in rax
SHIM(ldiv) { sh_div(c); }
SHIM(lldiv) { i64 a = (i64)A2, b = (i64)A3; ST64(A1, (u64)(a / b)); ST64(A1 + 8, (u64)(a % b)); RET(A1); }   // 16-byte struct: hidden pointer

// ==== time =================================================================================================================
static void to_wtm(u64 out, const struct tm *t) {
    int v[9] = { t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon, t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst };
    memcpy((void *)out, v, sizeof v);
}
static void from_wtm(u64 in, struct tm *t) {
    int v[9]; memcpy(v, (void *)in, sizeof v); memset(t, 0, sizeof *t);
    t->tm_sec = v[0]; t->tm_min = v[1]; t->tm_hour = v[2]; t->tm_mday = v[3]; t->tm_mon = v[4]; t->tm_year = v[5]; t->tm_isdst = v[8];
}
static u64 time_s(u64 out, i64 t, int local) {   // Windows rejects negative times
    if (!out) return (u64)SETERR(W_EINVAL);
    if (t < 0) { memset((void *)out, 0xFF, 36); return (u64)SETERR(W_EINVAL); }
    time_t tt = (time_t)t; struct tm r; if (local) localtime_r(&tt, &r); else gmtime_r(&tt, &r); to_wtm(out, &r); return 0;
}
SHIM(_localtime64_s) { RET(time_s(A1, A2 ? (i64)LD64(A2) : -1, 1)); }   // (tm*, const __time64_t*)
SHIM(_gmtime64_s) { RET(time_s(A1, A2 ? (i64)LD64(A2) : -1, 0)); }
SHIM(_localtime32_s) { RET(time_s(A1, A2 ? (i64)(i32)LD32(A2) : -1, 1)); }
SHIM(_gmtime32_s) { RET(time_s(A1, A2 ? (i64)(i32)LD32(A2) : -1, 0)); }
static __thread u32 tm_buf[9];
SHIM(_localtime32) { i64 t = (i32)LD32(A1); if (t < 0) RET(0); time_s((u64)tm_buf, t, 1); RET(tm_buf); }
SHIM(_gmtime32) { i64 t = (i32)LD32(A1); if (t < 0) RET(0); time_s((u64)tm_buf, t, 0); RET(tm_buf); }
SHIM(_mktime32) { struct tm t; from_wtm(A1, &t); time_t r = mktime(&t); to_wtm(A1, &t); RET((u32)r); }
SHIM(_mkgmtime64) { struct tm t; from_wtm(A1, &t); time_t r = timegm(&t); to_wtm(A1, &t); RET((u64)r); }
SHIM(_mkgmtime32) { struct tm t; from_wtm(A1, &t); time_t r = timegm(&t); to_wtm(A1, &t); RET((u32)r); }
SHIM(_time32) { time_t t = time(NULL); if (A1) ST32(A1, (u32)t); RET((u32)t); }
SHIM(_difftime64) { RETD((double)((i64)A1 - (i64)A2)); }
SHIM(_difftime32) { RETD((double)((i32)A1 - (i32)A2)); }
static struct timespec start_ts;
__attribute__((constructor)) static void clock_start(void) { clock_gettime(CLOCK_MONOTONIC, &start_ts); }
SHIM(clock) {  // Windows clock(): wall-clock ms since process start (CLOCKS_PER_SEC 1000)
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    RET((u32)((t.tv_sec - start_ts.tv_sec) * 1000 + (t.tv_nsec - start_ts.tv_nsec) / 1000000));
}
static long tz_sec; static int tz_dst; static char tz_names[2][64]; static u64 tz_ptrs[2];
static void tz_init(void) {
    tzset();
    time_t now = time(NULL); struct tm l; localtime_r(&now, &l);
    struct tm jan = l; jan.tm_mon = 0; jan.tm_mday = 1; jan.tm_isdst = -1; time_t tj = mktime(&jan); struct tm lj; localtime_r(&tj, &lj);
    tz_sec = -lj.tm_gmtoff + (lj.tm_isdst ? 3600 : 0);   // seconds west of UTC, standard time
    tz_dst = daylight != 0;
    snprintf(tz_names[0], 64, "%s", tzname[0]); snprintf(tz_names[1], 64, "%s", tzname[1]);
    tz_ptrs[0] = (u64)tz_names[0]; tz_ptrs[1] = (u64)tz_names[1];
}
SHIM(_tzset) { tz_init(); RET(0); }
SHIM(__timezone) { if (!tz_ptrs[0]) tz_init(); static i32 v; v = (i32)tz_sec; RET(&v); }
SHIM(__daylight) { if (!tz_ptrs[0]) tz_init(); static i32 v; v = tz_dst; RET(&v); }
SHIM(__dstbias) { static i32 v = -3600; RET(&v); }
SHIM(__tzname) { if (!tz_ptrs[0]) tz_init(); RET(tz_ptrs); }
SHIM(_get_timezone) { if (!tz_ptrs[0]) tz_init(); if (!A1) RET(W_EINVAL); ST32(A1, (u32)(i32)tz_sec); RET(0); }
SHIM(_get_daylight) { if (!tz_ptrs[0]) tz_init(); if (!A1) RET(W_EINVAL); ST32(A1, (u32)tz_dst); RET(0); }
SHIM(_get_dstbias) { if (!A1) RET(W_EINVAL); ST32(A1, (u32)-3600); RET(0); }
SHIM(_get_tzname) {  // (*ret incl NUL, buf, n, index)
    if (!tz_ptrs[0]) tz_init(); if ((u32)A4 > 1) RET(W_EINVAL);
    const char *s = tz_names[(u32)A4]; size_t need = strlen(s) + 1; if (A1) ST64(A1, need);
    if (!A2) RET(0); if (A3 < need) RET(W_ERANGE);
    memcpy((void *)A2, s, need); RET(0);
}
SHIM(_strftime_l) { struct tm t; from_wtm(A4, &t); int v[9]; memcpy(v, (void *)A4, sizeof v); t.tm_wday = v[6]; t.tm_yday = v[7];   // (buf, max, fmt, tm, locale)
                    RET(strftime((char *)A1, A2, (const char *)A3, &t)); }
static char *asctime_(u64 tm, char *b) {
    static const char *const dn[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }, *const mn[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int v[9]; memcpy(v, (void *)tm, sizeof v);
    snprintf(b, 26, "%s %s %02d %02d:%02d:%02d %04d\n", dn[(v[6] % 7 + 7) % 7], mn[(v[4] % 12 + 12) % 12], v[3], v[2], v[1], v[0], v[5] + 1900);
    return b;
}
SHIM(asctime) { static __thread char b[32]; RET(asctime_(A1, b)); }
SHIM(asctime_s) { if (!A1 || A2 < 26) RET(W_EINVAL); asctime_(A3, (char *)A1); RET(0); }   // (buf, n, tm)
SHIM(_ctime64) { static __thread char b[32]; i64 t = (i64)LD64(A1); if (t < 0) RET(0); u32 tm[9]; time_s((u64)tm, t, 1); RET(asctime_((u64)tm, b)); }
SHIM(_ctime64_s) { if (!A1 || A2 < 26) RET(W_EINVAL); u32 tm[9]; if (time_s((u64)tm, (i64)LD64(A3), 1)) RET(W_EINVAL); asctime_((u64)tm, (char *)A1); RET(0); }
SHIM(_ctime32) { static __thread char b[32]; i64 t = (i32)LD32(A1); if (t < 0) RET(0); u32 tm[9]; time_s((u64)tm, t, 1); RET(asctime_((u64)tm, b)); }
SHIM(_timespec64_get) { if ((u32)A2 != 1) RET(0); struct timespec t; clock_gettime(CLOCK_REALTIME, &t); ST64(A1, (u64)t.tv_sec); ST32(A1 + 8, (u32)t.tv_nsec); RET(1); }   // TIME_UTC 1
SHIM(_timespec32_get) { if ((u32)A2 != 1) RET(0); struct timespec t; clock_gettime(CLOCK_REALTIME, &t); ST32(A1, (u32)t.tv_sec); ST32(A1 + 4, (u32)t.tv_nsec); RET(1); }
SHIM(_strdate) { time_t n = time(NULL); struct tm l; localtime_r(&n, &l); strftime((char *)A1, 9, "%m/%d/%y", &l); RET(A1); }
SHIM(_strtime) { time_t n = time(NULL); struct tm l; localtime_r(&n, &l); strftime((char *)A1, 9, "%H:%M:%S", &l); RET(A1); }
SHIM(_ftime64_s) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); if (!tz_ptrs[0]) tz_init(); ST64(A1, (u64)t.tv_sec); ST16(A1 + 8, (u16)(t.tv_nsec / 1000000)); ST16(A1 + 10, (u16)(tz_sec / 60)); ST16(A1 + 12, (u16)tz_dst); RET(0); }
SHIM(_ftime64) { sh__ftime64_s(c); }

// ==== locale-time data for msvcp140's time_get/time_put facets (when msvcp140.dll is recompiled) =========================
static pthread_mutex_t locales_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
SHIM(_lock_locales) { pthread_mutex_lock(&locales_lock); RET(0); }
SHIM(_unlock_locales) { pthread_mutex_unlock(&locales_lock); RET(0); }
SHIM(__strncnt) { RET(strnlen((const char *)A1, A2)); }
static const char *const dabbr[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }, *const dfull[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mabbr[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
                  *const mfull[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static char *join_names(const char *const *ab, const char *const *full, int n) {   // ":Sun:Sunday:Mon:Monday..."
    size_t len = 1; for (int i = 0; i < n; i++) len += strlen(ab[i]) + strlen(full[i]) + 2;
    char *s = malloc(len); s[0] = 0;
    for (int i = 0; i < n; i++) { strcat(s, ":"); strcat(s, ab[i]); strcat(s, ":"); strcat(s, full[i]); }
    return s;
}
SHIM(_Getdays) { RET(join_names(dabbr, dfull, 7)); }
SHIM(_Getmonths) { RET(join_names(mabbr, mfull, 12)); }
SHIM(_W_Getdays) { char *s = join_names(dabbr, dfull, 7); u64 w = wide_dup(s); free(s); RET(w); }
SHIM(_W_Getmonths) { char *s = join_names(mabbr, mfull, 12); u64 w = wide_dup(s); free(s); RET(w); }
// __lc_time_data: narrow names (7+7+12+12+2 pointers), 3 format pointers, int caltype, long refcount, then the wide copies.
SHIM(_Gettnames) {
    static u64 *t;
    if (!t) {
        t = calloc(2 * 46 + 8, 8); u64 *p = t; int k = 0;
        const char *ampm[] = { "AM", "PM" }, *fmts[] = { "M/d/yyyy", "dddd, MMMM d, yyyy", "h:mm:ss tt" };
        const char *const *lists[] = { dabbr, dfull, mabbr, mfull, ampm, fmts }; int counts[] = { 7, 7, 12, 12, 2, 3 };
        for (int l = 0; l < 6; l++) for (int i = 0; i < counts[l]; i++) p[k++] = (u64)lists[l][i];
        u8 *ints = (u8 *)&p[k]; ST32((u64)ints, 1); ST32((u64)ints + 4, 1); k++;   // ww_caltype, refcount
        for (int l = 0; l < 6; l++) for (int i = 0; i < counts[l]; i++) p[k++] = wide_dup(lists[l][i]);
        p[k++] = wide_dup("en-US");
    }
    RET(t);
}
SHIM(_W_Gettnames) { sh__Gettnames(c); }
SHIM(_Strftime) { struct tm t; from_wtm(A4, &t); int v[9]; memcpy(v, (void *)A4, sizeof v); t.tm_wday = v[6]; t.tm_yday = v[7];   // (buf, max, fmt, tm, lc_time)
                  RET(strftime((char *)A1, A2, (const char *)A3, &t)); }
SHIM(_Wcsftime) {  // (wbuf, max, wfmt, tm, lc_time)
    char f[512], o[1024]; wx_w2u(A3, f, sizeof f);
    struct tm t; from_wtm(A4, &t); int v[9]; memcpy(v, (void *)A4, sizeof v); t.tm_wday = v[6]; t.tm_yday = v[7];
    size_t n = strftime(o, sizeof o, f, &t); if (!n && *f) RET(0);
    size_t need = wx_u2w(o, 0, 0); if (need > A2) RET(0);
    wx_u2w(o, A1, A2); RET(need - 1);
}

// ==== vcruntime140 C helpers (used only when vcruntime140.dll isn't recompiled alongside the game) =========================
// std::exception data { const char *what; bool do_free }
SHIM(__std_exception_copy) {  // (const data *src, data *dst)
    u64 what = LD64(A1); int dofree = LD8(A1 + 8);
    if (dofree && what) { ST64(A2, (u64)strdup((char *)what)); ST8(A2 + 8, 1); }
    else { ST64(A2, what); ST8(A2 + 8, 0); }
    RET(0);
}
SHIM(__std_exception_destroy) { if (LD8(A1 + 8)) free((void *)LD64(A1)); ST64(A1, 0); ST8(A1 + 8, 0); RET(0); }
SHIM(__std_terminate) { die("std::terminate"); }
SHIM(_purecall) { die("pure virtual function call"); }
SHIM(__std_type_info_compare) { if (A1 == A2) RET(0); RET((u32)strcmp((char *)(A1 + 17), (char *)(A2 + 17))); }   // type_info { vptr, undecorated*, char decorated[] }
SHIM(__std_type_info_hash) { u64 h = 0xcbf29ce484222325ull; for (const u8 *p = (const u8 *)(A1 + 17); *p; p++) { h ^= *p; h *= 0x100000001b3ull; } RET(h); }
SHIM(__std_type_info_destroy_list) { RET(0); }
// Undecorate ".?AVFoo@ns@@" -> "class ns::Foo" (enough for type names in messages; templates keep their raw form).
SHIM(__std_type_info_name) {  // (type_info*, __type_info_node*)
    u64 cached = LD64(A1 + 8); if (cached) RET(cached);
    const char *d = (const char *)(A1 + 16); char out[1024] = "";
    if (d[0] == '.' && d[1] == '?' && d[2] == 'A' && (d[3] == 'V' || d[3] == 'U' || d[3] == 'W')) {
        const char *kind = d[3] == 'V' ? "class " : d[3] == 'U' ? "struct " : "enum ";
        const char *p = d + 4; if (d[3] == 'W') p++;
        char parts[16][256]; int np = 0;
        while (*p && *p != '@' && np < 16) { const char *e = strchr(p, '@'); if (!e) e = p + strlen(p); snprintf(parts[np++], 256, "%.*s", (int)(e - p), p); p = *e ? e + 1 : e; }
        size_t o = (size_t)snprintf(out, sizeof out, "%s", kind);
        for (int i = np - 1; i >= 0; i--) o += (size_t)snprintf(out + o, sizeof out - o, "%s%s", parts[i], i ? "::" : "");
    } else snprintf(out, sizeof out, "%s", d);
    char *s = strdup(out);
    u64 expect = 0;
    if (!__atomic_compare_exchange_n((u64 *)(A1 + 8), &expect, (u64)s, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { free(s); RET(expect); }
    RET(s);
}
SHIM(_CxxThrowException) {  // (object, ThrowInfo*): EH_EXCEPTION_NUMBER 0xE06D7363 with the x64 4-parameter record
    u64 sz; u64 base = A2 ? rt_module_base_for(A2, &sz) : 0;
    u64 params[4] = { 0x19930520, A1, A2, base };
    call4(c, "RaiseException", 0xE06D7363, 1, 4, (u64)params);
    die("_CxxThrowException returned");
}
SHIM(__DestructExceptionObject) {  // (EXCEPTION_RECORD*): run the thrown object's destructor (ThrowInfo.pmfnUnwind, image-relative)
    u64 rec = A1; if (!rec || LD32(rec) != 0xE06D7363 || LD32(rec + 24) < 4) RET(0);
    u64 obj = LD64(rec + 32 + 8), ti = LD64(rec + 32 + 16), base = LD64(rec + 32 + 24);
    if (!obj || !ti) RET(0);
    i32 dtor = (i32)LD32(ti + 4);
    if (dtor) { u64 a = obj; rt_guest_call(c, base + (u64)(i64)dtor, 1, &a); }
    RET(0);
}
// RTTI (x64, image-relative): vptr[-1] = CompleteObjectLocator { sig=1, offset, cdOffset, TypeDescriptor rva, ClassHierarchy rva, self rva }
static int col_of(u64 obj, u64 *col, u64 *base) {
    if (!obj) return 0;
    u64 vt = LD64(obj); if (!vt) return 0;
    u64 l = LD64(vt - 8); if (!l || LD32(l) != 1) return 0;
    *col = l; *base = l - LD32(l + 20); return 1;
}
SHIM(__RTtypeid) {
    u64 col, base; if (!col_of(A1, &col, &base)) { die("__RTtypeid on a null or non-polymorphic object (std::bad_typeid)"); }
    RET(base + LD32(col + 12));
}
static int td_eq(u64 a, u64 b) { return a == b || !strcmp((char *)(a + 16), (char *)(b + 16)); }
SHIM(__RTDynamicCast) {  // (obj, vfDelta, srcType, targetType, isReference)
    u64 obj = A1, target = A4; int isref = (int)(u32)AN(5);
    if (!obj) RET(0);
    u64 col, base;
    if (!col_of(obj, &col, &base)) { if (isref) die("dynamic_cast failed (std::bad_cast)"); RET(0); }
    i64 complete = (i64)obj - (i32)LD32(col + 4);
    if (LD32(col + 8)) complete -= (i32)LD32(obj - LD32(col + 8));
    u64 chd = base + LD32(col + 16); u32 nb = LD32(chd + 8); u64 bca = base + LD32(chd + 12);
    for (u32 i = 0; i < nb; i++) {
        u64 bcd = base + LD32(bca + 4 * i);
        if (!td_eq(base + LD32(bcd), target)) continue;
        if (LD32(bcd + 20) & 3) continue;   // BCD_NOTVISIBLE / BCD_AMBIGUOUS
        i32 mdisp = (i32)LD32(bcd + 8), pdisp = (i32)LD32(bcd + 12), vdisp = (i32)LD32(bcd + 16);
        i64 r = complete + mdisp;
        if (pdisp >= 0) { u64 vbt = LD64((u64)(complete + pdisp)); r = complete + pdisp + (i32)LD32(vbt + (u64)vdisp) + mdisp; }
        RET((u64)r);
    }
    if (isref) die("dynamic_cast failed (std::bad_cast)");
    RET(0);
}
SHIM(__RTCastToVoid) { u64 col, base; if (!A1) RET(0); if (!col_of(A1, &col, &base)) RET(A1); i64 r = (i64)A1 - (i32)LD32(col + 4); if (LD32(col + 8)) r -= (i32)LD32(A1 - LD32(col + 8)); RET((u64)r); }

const Shim shims_crt2[] = {
#define E(n) { NULL, #n, sh_##n },
    E(cbrt) E(cbrtf) E(expm1) E(expm1f) E(log1p) E(log1pf) E(logb) E(logbf) E(nearbyint) E(nearbyintf) E(rint) E(rintf) E(erf) E(erff)
    E(erfc) E(erfcf) E(tgamma) E(tgammaf) E(lgamma) E(lgammaf) E(acosh) E(acoshf) E(asinh) E(asinhf) E(atanh) E(atanhf) E(sinhf)
    E(coshf) E(tanhf) E(fabsf) E(fmax) E(fmaxf) E(fmin) E(fminf) E(fdim) E(fdimf) E(remainder) E(remainderf) E(nextafter)
    E(nextafterf) E(hypotf) E(_hypot) E(_hypotf) E(_nextafter) E(_nextafterf) E(_copysign) E(_chgsign) E(_logb) E(_scalb) E(fma)
    E(fmaf) E(scalbn) E(scalbnf) E(scalbln) E(scalblnf) E(ldexpf) E(frexpf) E(modf) E(modff) E(remquo) E(remquof) E(nan) E(nanf)
    E(lround) E(lroundf) E(llround) E(llroundf) E(lrint) E(lrintf) E(llrint) E(llrintf) E(ilogb) E(ilogbf) E(_isnan) E(_isnanf)
    E(_finite) E(_finitef) E(_dclass) E(_ldclass) E(_fdclass) E(_dsign) E(_ldsign) E(_fdsign) E(_dtest) E(_fdtest) E(_fpclass)
    E(_controlfp) E(_control87) E(_controlfp_s) E(_clearfp) E(_statusfp) E(_fpreset) E(_set_SSE2_enable) E(__fpe_flt_rounds)
    E(fegetround) E(fesetround) E(feclearexcept) E(fetestexcept) E(fegetenv) E(fesetenv)
    E(isblank) E(iscntrl) E(isgraph) E(__isascii) E(__toascii) E(_isctype)
    E(iswspace) E(_iswspace_l) E(iswalnum) E(_iswalnum_l) E(iswcntrl) E(_iswcntrl_l) E(iswdigit) E(_iswdigit_l) E(iswpunct) E(_iswpunct_l)
    E(iswalpha) E(_iswalpha_l) E(iswupper) E(_iswupper_l) E(iswlower) E(_iswlower_l) E(iswprint) E(_iswprint_l) E(iswxdigit)
    E(_iswxdigit_l) E(iswgraph) E(_iswgraph_l) E(iswblank) E(_iswblank_l)
    E(_isalpha_l) E(_isdigit_l) E(_isspace_l) E(_isupper_l) E(_islower_l) E(_isalnum_l) E(_isxdigit_l) E(_ispunct_l) E(_isprint_l)
    E(_iscntrl_l) E(_isgraph_l) E(_tolower_l) E(_toupper_l) E(_tolower) E(_toupper) E(_towlower_l) E(_towupper_l) E(__pctype_func)
    E(__pwctype_func) E(_get_current_locale) E(_create_locale) E(_wcreate_locale) E(_free_locale) E(___lc_locale_name_func)
    E(___lc_collate_cp_func) E(_wsetlocale) E(_getmbcp) E(_setmbcp)
    E(strcpy_s) E(strcat_s) E(strncpy_s) E(strncat_s) E(wcsncpy_s) E(wcscat_s) E(wcsncat_s) E(memcpy_s) E(memmove_s) E(wmemcpy_s)
    E(wmemmove_s) E(_strlwr) E(_strupr) E(_strlwr_s) E(_strupr_s) E(_strrev) E(_strset) E(_strnset) E(_memicmp) E(_strcmpi) E(_stricoll)
    E(_strncoll) E(_strnicoll) E(_strcoll_l) E(_stricmp_l) E(_strxfrm_l) E(_wcscoll_l) E(_wcsicoll) E(_wcsxfrm_l) E(_wcsicmp_l)
    E(_wcsdup) E(_wcslwr) E(_wcsupr) E(_wcslwr_s) E(_wcsupr_s) E(_wcsrev) E(wcscat) E(wcsncpy) E(wcsncat) E(wcschr) E(wcsrchr)
    E(wcsstr) E(wcspbrk) E(wcsspn) E(wcscspn) E(wcstok) E(wcstok_s) E(strtok_s) E(strnlen_s) E(wcsnlen_s) E(wmemset) E(wmemcpy)
    E(wmemmove) E(wmemcmp) E(wmemchr)
    E(mbtowc) E(_mbtowc_l) E(mblen) E(mbrlen) E(_mbstrlen) E(wctomb) E(wctomb_s) E(btowc) E(wctob) E(mbstowcs) E(mbstowcs_s)
    E(wcstombs) E(wcstombs_s) E(wcrtomb_s) E(wcsrtombs)
    E(wcstol) E(wcstoul) E(wcstoll) E(wcstoull) E(_wcstoi64) E(_wcstoui64) E(wcstoimax) E(wcstoumax) E(_wtoi) E(_wtol) E(_wtoll)
    E(_wtoi64) E(wcstod) E(wcstof) E(_wtof) E(_wcstod_l) E(strtof) E(_strtod_l) E(_strtof_l) E(_atof_l) E(atol) E(atoll) E(_atoi64)
    E(strtoimax) E(strtoumax) E(_strtol_l) E(_strtoul_l) E(_strtoi64_l) E(_strtoui64_l) E(_itoa) E(_ltoa) E(_i64toa) E(_itoa_s)
    E(_ltoa_s) E(_ultoa_s) E(_i64toa_s) E(_ui64toa_s) E(_itow) E(_ltow) E(_ultow) E(_i64tow) E(_ui64tow) E(_itow_s)
    E(__stdio_common_vswprintf) E(__stdio_common_vswprintf_s) E(__stdio_common_vsnwprintf_s) E(__stdio_common_vfprintf_s)
    E(__stdio_common_vfprintf_p) E(__stdio_common_vfwprintf_s) E(__stdio_common_vfwprintf_p) E(__stdio_common_vsprintf_p)
    E(__stdio_common_vswprintf_p) E(_vscprintf) E(_vscwprintf) E(_vsnprintf_s) E(_vsprintf_s) E(fwprintf) E(wprintf) E(vfwprintf)
    E(_snwprintf_s) E(_snprintf) E(_scprintf) E(sprintf_s) E(swprintf_s) E(__stdio_common_vswscanf) E(__stdio_common_vfwscanf)
    E(_wopen) E(_sopen) E(_wsopen) E(_sopen_s) E(_wsopen_s) E(_dup) E(_dup2) E(_pipe) E(_popen) E(_wpopen) E(_pclose) E(_chsize)
    E(_chsize_s) E(_eof) E(_tell) E(_telli64) E(_commit) E(_locking) E(_umask) E(_getpid) E(_open_osfhandle) E(fopen_s) E(_wfopen_s)
    E(_fsopen) E(_wfsopen) E(freopen) E(freopen_s) E(_wfreopen_s) E(setbuf) E(rewind) E(fputwc) E(_fputwc_nolock) E(fputws) E(_putws)
    E(putwchar) E(fgetws) E(getchar) E(getwchar) E(_getch) E(_kbhit) E(gets_s) E(tmpfile) E(tmpfile_s) E(tmpnam) E(tmpnam_s) E(_tempnam)
    E(_mktemp_s) E(_mktemp) E(_setmaxstdio) E(_getmaxstdio) E(_get_stream_buffer_pointers) E(_fseek_nolock) E(_ftell_nolock)
    E(_fseeki64_nolock) E(_ftelli64_nolock) E(_fread_nolock) E(_fwrite_nolock) E(_fflush_nolock) E(_fclose_nolock) E(_getc_nolock)
    E(_putc_nolock) E(_ungetc_nolock) E(_fcloseall) E(_flushall) E(_rmtmp) E(perror)
    E(_stat32) E(_stat64i32) E(_stat64) E(_stat32i64) E(_wstat32) E(_wstat64i32) E(_wstat64) E(_wstat32i64) E(_fstat32) E(_fstat64i32)
    E(_fstat32i64) E(_unlink) E(_wunlink) E(_mkdir) E(_wmkdir) E(_rmdir) E(_wrmdir) E(_access) E(_waccess) E(_access_s) E(_waccess_s)
    E(_chmod) E(_wchmod) E(_wrename) E(_chdir) E(_wchdir) E(_getcwd) E(_wgetcwd) E(_getdcwd) E(_wgetdcwd) E(_getdrive) E(_chdrive)
    E(_fullpath) E(_wfullpath) E(_splitpath_s) E(_wsplitpath_s) E(_splitpath) E(_makepath) E(_makepath_s) E(_findfirst32)
    E(_findfirst64i32) E(_findfirst64) E(_wfindfirst32) E(_wfindfirst64i32) E(_wfindfirst64) E(_findnext32) E(_findnext64i32)
    E(_findnext64) E(_wfindnext32) E(_wfindnext64i32) E(_wfindnext64) E(_findclose)
    { NULL, "_stat", sh__stat64i32 }, { NULL, "_wstat", sh__wstat64i32 }, { NULL, "_fstat", sh__fstat64i32 },
    { NULL, "_findfirst", sh__findfirst64i32 }, { NULL, "_findnext", sh__findnext64i32 },
    { NULL, "_wfindfirst", sh__wfindfirst64i32 }, { NULL, "_wfindnext", sh__wfindnext64i32 },
    E(_wgetenv) E(getenv_s) E(_wgetenv_s) E(_dupenv_s) E(_wdupenv_s) E(_wputenv) E(_wputenv_s) E(__p___wargv) E(__p__wenviron)
    E(_configure_wide_argv) E(_initialize_wide_environment) E(_get_initial_wide_environment) E(_get_initial_narrow_environment)
    E(__wgetmainargs) E(_get_wide_winmain_command_line) E(__p__wcmdln) E(__p___initenv) E(__p___winitenv) E(_get_pgmptr) E(_get_wpgmptr)
    E(_set_errno) E(_get_errno) E(_set_doserrno) E(_get_doserrno) E(strerror_s) E(_strerror) E(_wcserror) E(_wcserror_s) E(__sys_nerr)
    E(__sys_errlist) E(_seh_filter_dll) E(_invoke_watson) E(_invalid_parameter_noinfo) E(_invalid_parameter_noinfo_noreturn)
    E(_invalid_parameter) E(_Exit) E(quick_exit) E(_crt_at_quick_exit) E(at_quick_exit) E(system) E(_wsystem) E(_callnewh)
    E(_query_new_handler) E(_set_new_handler) E(_query_new_mode) E(raise) E(_set_error_mode) E(_set_abort_behavior) E(__threadid)
    E(__threadhandle) E(_beginthread) E(_endthread)
    E(_aligned_realloc) E(_aligned_recalloc) E(_aligned_msize) E(_aligned_offset_malloc) E(_aligned_offset_free) E(_recalloc) E(_expand)
    E(_heapchk) E(_heapmin) E(_get_heap_handle) E(_malloc_base) E(_calloc_base) E(_realloc_base) E(_free_base) E(qsort_s) E(bsearch_s)
    E(_lfind) E(_rotl) E(_rotr) E(_rotl64) E(_rotr64) E(_byteswap_ushort) E(_byteswap_ulong) E(_byteswap_uint64) E(div) E(ldiv) E(lldiv)
    E(_localtime64_s) E(_gmtime64_s) E(_localtime32_s) E(_gmtime32_s) E(_localtime32) E(_gmtime32) E(_mktime32) E(_mkgmtime64)
    E(_mkgmtime32) E(_time32) E(_difftime64) E(_difftime32) E(clock) E(_tzset) E(__timezone) E(__daylight) E(__dstbias) E(__tzname)
    E(_get_timezone) E(_get_daylight) E(_get_dstbias) E(_get_tzname) E(_strftime_l) E(asctime) E(asctime_s) E(_ctime64) E(_ctime64_s)
    E(_ctime32) E(_timespec64_get) E(_timespec32_get) E(_strdate) E(_strtime) E(_ftime64_s) E(_ftime64)
    { NULL, "_mkgmtime", sh__mkgmtime64 }, { NULL, "_difftime", sh__difftime64 }, { NULL, "timespec_get", sh__timespec64_get },
    E(_lock_locales) E(_unlock_locales) E(__strncnt) E(_Getdays) E(_Getmonths) E(_W_Getdays) E(_W_Getmonths) E(_Gettnames) E(_W_Gettnames)
    E(_Strftime) E(_Wcsftime)
    E(__std_exception_copy) E(__std_exception_destroy) E(__std_terminate) E(_purecall) E(__std_type_info_compare) E(__std_type_info_hash)
    E(__std_type_info_destroy_list) E(__std_type_info_name) E(_CxxThrowException) E(__DestructExceptionObject) E(__RTtypeid)
    E(__RTDynamicCast) E(__RTCastToVoid)
#undef E
    { 0, 0, 0 }
};
