// CRT part 2 + kernel32 odds and ends: _s string functions, wide conversions, number formatting, math classes,
// time, qsort_s, wide printf; date/number formatting, ordinal compares, env expansion, one-time init,
// processor topology; CNG hashing and base64. Expected output: tests/expected/api_crt2.out (Windows behaviour).
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <errno.h>
#include <time.h>

short __cdecl _dclass(double); int __cdecl _dsign(double); short __cdecl _fdclass(float);   // UCRT exports (not in mingw headers)
static int cmp_ctx(void *ctx, const void *a, const void *b) { (*(int *)ctx)++; return *(const int *)a - *(const int *)b; }
static int once_calls;
static BOOL CALLBACK once_fn(PINIT_ONCE o, PVOID p, PVOID *ctx) { once_calls++; *ctx = (PVOID)0x1230; return TRUE; }
static int wfmt(wchar_t *buf, size_t n, const wchar_t *f, ...) { va_list ap; va_start(ap, f); int r = _vsnwprintf_s(buf, n, _TRUNCATE, f, ap); va_end(ap); return r; }
static int cfmt(const char *f, ...) { va_list ap; va_start(ap, f); int r = _vscprintf(f, ap); va_end(ap); return r; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char b[16]; int r;
    r = strcpy_s(b, sizeof b, "hello"); printf("strcpy_s: %d '%s'\n", r, b);
    r = strcpy_s(b, 4, "hello"); printf("strcpy_s too long: %d '%s'\n", r, b);
    strcpy(b, "ab"); r = strcat_s(b, sizeof b, "cd"); printf("strcat_s: %d '%s'\n", r, b);
    r = strncpy_s(b, 4, "abcdef", _TRUNCATE); printf("strncpy_s truncate: %d '%s'\n", r, b);
    r = strncpy_s(b, sizeof b, "abcdef", 3); printf("strncpy_s count: %d '%s'\n", r, b);
    char big[8] = "xxxxxxx"; r = memcpy_s(big, 4, "123456", 6); printf("memcpy_s overflow: %d first=%d\n", r, big[0]);
    wchar_t w[32]; r = wcscpy_s(w, 32, L"wide"); r = wcscat_s(w, 32, L"str"); printf("wcscat_s: %d %ls\n", r, w);
    r = wcsncpy_s(w, 4, L"abcdef", _TRUNCATE); printf("wcsncpy_s truncate: %d %ls\n", r, w);
    strcpy(b, "abc"); printf("_strrev: %s  _strupr: %s\n", _strrev(b), _strupr(b));
    wchar_t *d = _wcsdup(L"Dup"); printf("_wcsdup: %ls  _wcslwr: %ls\n", d, _wcslwr(d)); free(d);
    wchar_t tk[] = L"a, b,,c", *ctx = NULL; int nt = 0;
    for (wchar_t *t = wcstok_s(tk, L", ", &ctx); t; t = wcstok_s(NULL, L", ", &ctx)) printf("%stok=%ls", nt++ ? " " : "", t);
    printf("\n");

    size_t conv = 0; r = (int)mbstowcs_s(&conv, w, 32, "Gr\xfc\xdf", _TRUNCATE); printf("mbstowcs_s: %d conv=%zu w[2]=%04x\n", r, conv, w[2]);
    errno = 0; size_t n = wcstombs(b, L"\x4e2d", sizeof b); printf("wcstombs unmappable: %d errno=%d\n", (int)n, errno);
    n = wcstombs(b, L"plain", sizeof b); printf("wcstombs: %d '%s'\n", (int)n, b);
    wchar_t *end; long lv = wcstol(L"  -123xyz", &end, 10); printf("wcstol: %ld rest=%ls\n", lv, end);
    unsigned long uv = wcstoul(L"ff", NULL, 16); printf("wcstoul hex: %lu\n", uv);
    errno = 0; lv = wcstol(L"99999999999", NULL, 10); printf("wcstol overflow: %ld errno=%d\n", lv, errno);
    double dv = wcstod(L"2.5e3!", &end); printf("wcstod: %.1f rest=%ls\n", dv, end);
    printf("_wtoi: %d  _wtof: %.2f\n", _wtoi(L"42"), _wtof(L"1.25"));
    printf("_itoa: %s ", _itoa(-255, b, 16)); printf("%s ", _itoa(-255, b, 10)); printf("%s\n", _itoa(5, b, 2));
    r = _itoa_s(12345, b, 4, 10); printf("_itoa_s small: %d\n", r);
    _ultow(3054, w, 16); printf("_ultow: %ls\n", w);

    printf("_dclass: %d %d %d %d %d\n", _dclass(1.0), _dclass(0.0), _dclass(INFINITY), _dclass(NAN), _dclass(1e-310));
    printf("_dsign: %d %d  _fdclass: %d\n", _dsign(-2.0), _dsign(2.0), _fdclass(1.0f));
    printf("lround: %ld %ld  llround: %lld  cbrt: %.3f  hypotf: %.2f  fmaf: %.1f\n", lround(2.5), lround(-2.5), llround(1e12 + 0.5), cbrt(27.0), hypotf(3.0f, 4.0f), fmaf(2.0f, 3.0f, 1.0f));
    double ip; double fr = modf(-3.75, &ip); printf("modf: %.2f %.2f  remainder: %.1f  scalbn: %.1f\n", fr, ip, remainder(7.0, 2.0), scalbn(1.5, 3));
    printf("nextafter > 1: %d  erf(0): %.1f  logb(8): %.1f\n", nextafter(1.0, 2.0) > 1.0, erf(0.0), logb(8.0));

    int arr[] = { 5, 3, 9, 1 }, calls = 0; qsort_s(arr, 4, sizeof *arr, cmp_ctx, &calls);
    printf("qsort_s: %d %d %d %d (comparator called: %s)\n", arr[0], arr[1], arr[2], arr[3], calls > 0 ? "yes" : "no");
    r = swprintf_s(w, 32, L"%d-%ls-%hs", 7, L"w", "n"); printf("swprintf_s: %d %ls\n", r, w);
    r = wfmt(w, 6, L"%ls", L"truncated"); printf("_vsnwprintf_s truncate: %d %ls\n", r, w);
    printf("_vscprintf: %d\n", cfmt("%d:%s", 1234, "abc"));
    char eb[64]; strerror_s(eb, sizeof eb, ENOENT); printf("strerror_s: %s\n", eb);
    struct tm tmv; __time64_t t0 = 86400 * 365; r = _gmtime64_s(&tmv, &t0);
    printf("_gmtime64_s: %d %04d-%02d-%02d wday=%d yday=%d\n", r, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_wday, tmv.tm_yday);
    __time64_t neg = -1; r = _gmtime64_s(&tmv, &neg); printf("_gmtime64_s negative: %d\n", r);
    printf("_mkgmtime64 roundtrip: %s\n", _mkgmtime64(&(struct tm){ .tm_year = 71, .tm_mon = 0, .tm_mday = 1 }) == t0 ? "yes" : "no");

    SYSTEMTIME st = { 2024, 3, 0, 9, 14, 5, 7, 0 }; char out[64];
    GetDateFormatA(LOCALE_USER_DEFAULT, 0, &st, "dddd, MMMM d, yyyy", out, sizeof out); printf("GetDateFormat: %s\n", out);
    GetDateFormatA(LOCALE_USER_DEFAULT, 0, &st, "ddd dd'/'MMM'/'yy", out, sizeof out); printf("GetDateFormat: %s\n", out);
    GetTimeFormatA(LOCALE_USER_DEFAULT, 0, &st, "hh:mm:ss tt", out, sizeof out); printf("GetTimeFormat: %s\n", out);
    GetTimeFormatA(LOCALE_USER_DEFAULT, 0, &st, "H'h'm", out, sizeof out); printf("GetTimeFormat: %s\n", out);
    int need = GetDateFormatA(LOCALE_USER_DEFAULT, 0, &st, "yyyy", NULL, 0); printf("GetDateFormat size query: %d\n", need);
    GetNumberFormatA(LOCALE_USER_DEFAULT, 0, "-1234567.891", NULL, out, sizeof out); printf("GetNumberFormat: %s\n", out);
    NUMBERFMTA nf = { 1, 1, 3, ".", " ", 1 }; GetNumberFormatA(LOCALE_USER_DEFAULT, 0, "9999.96", &nf, out, sizeof out); printf("GetNumberFormat custom: %s\n", out);
    printf("CompareStringOrdinal: %d %d %d\n", CompareStringOrdinal(L"abc", -1, L"ABC", -1, TRUE), CompareStringOrdinal(L"abc", -1, L"ABC", -1, FALSE), CompareStringOrdinal(L"ab", -1, L"abc", -1, FALSE));
    printf("LocaleNameToLCID: %lx  lstrlenW: %d  lstrcmpiW: %d\n", LocaleNameToLCID(L"en-US", 0), lstrlenW(L"four"), lstrcmpiW(L"Same", L"sAME"));
    SetEnvironmentVariableA("WTM_X", "val");
    ExpandEnvironmentStringsA("%WTM_X%-%WTM_NOPE%-%", out, sizeof out); printf("ExpandEnvironmentStrings: %s\n", out);

    INIT_ONCE once = INIT_ONCE_STATIC_INIT; PVOID oc = NULL;
    InitOnceExecuteOnce(&once, once_fn, NULL, &oc); InitOnceExecuteOnce(&once, once_fn, NULL, &oc);
    printf("InitOnceExecuteOnce: calls=%d ctx=%p\n", once_calls, oc);
    DWORD len = 0; GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    BYTE *lp = malloc(len); BOOL ok = GetLogicalProcessorInformationEx(RelationProcessorCore, (void *)lp, &len);
    int cores = 0; for (DWORD o = 0; o < len; o += ((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(lp + o))->Size) cores++;
    SYSTEM_INFO si; GetSystemInfo(&si); free(lp);
    printf("processor cores: ok=%d match=%s\n", ok, cores == (int)si.dwNumberOfProcessors ? "yes" : "no");
    HANDLE fm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 64, NULL); printf("anon mapping handle: %s\n", fm ? "ok" : "fail");

    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hh; UCHAR dig[32];
    NTSTATUS s1 = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0); BCryptHashData(hh, (PUCHAR)"abc", 3, 0); BCryptFinishHash(hh, dig, 32, 0);
    printf("SHA256(abc): %lx ", (unsigned long)s1); for (int i = 0; i < 32; i++) printf("%02x", dig[i]); printf("\n");
    BCryptDestroyHash(hh); BCryptCloseAlgorithmProvider(alg, 0);
    printf("BCryptGenRandom: %lx\n", (unsigned long)BCryptGenRandom(NULL, dig, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    DWORD bl = sizeof out; CryptBinaryToStringA((const BYTE *)"hello", 5, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &bl);
    printf("base64: %s (%lu)\n", out, bl);
    BYTE raw[16]; DWORD rl = sizeof raw; CryptStringToBinaryA("aGVsbG8=", 0, CRYPT_STRING_BASE64, raw, &rl, NULL, NULL);
    printf("unbase64: %.*s (%lu)\n", (int)rl, raw, rl);
    return 0;
}
