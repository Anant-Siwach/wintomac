// Win32 / UCRT imports of AnyPS5's Windows libraries (third_party/anyps5-win, docs/ps5.md) that no other shim table
// covers: write watching for guest memory, thread stack limits, security-descriptor no-ops, Bessel functions,
// iphlpapi change notifications, ntdll loader/env helpers and window subclassing. Found with
//   python3 -I tools/api_coverage.py --list --with-redist third_party/anyps5-win/prx/libs/*
// Semantics follow the Microsoft Learn documentation of each API. No Wine or Microsoft code copied.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <wctype.h>

#include "winstr.h"

FILE *rt_host_file(u64 p);

static ShimFn ps5_shim_fn(const char *name) {
    extern const Shim shims_kernel32[], shims_threads[], shims_files[], shims_user32[], shims_misc[], shims_extra[],
        shims_extra2[], shims_kernel32x[], shims_win32x[];
    const Shim *t[] = { shims_kernel32, shims_threads, shims_files, shims_user32, shims_misc, shims_extra, shims_extra2,
                        shims_kernel32x, shims_win32x };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) for (const Shim *s = t[i]; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}
// Call another shim with up to 4 register arguments (stack arguments stay the caller's).
static u64 call4(Ctx *c, const char *name, u64 a1, u64 a2, u64 a3, u64 a4) {
    ShimFn f = ps5_shim_fn(name); if (!f) return 0;
    u64 s[4] = { c->r[RCX], c->r[RDX], c->r[R8], c->r[R9] };
    c->r[RCX] = a1; c->r[RDX] = a2; c->r[R8] = a3; c->r[R9] = a4;
    f(c);
    u64 r = c->r[RAX];
    c->r[RCX] = s[0]; c->r[RDX] = s[1]; c->r[R8] = s[2]; c->r[R9] = s[3];
    return r;
}
#define RETD(v) do { double r_ = (v); memset(&c->x[0], 0, 16); c->x[0].fd[0] = r_; RET(0); } while (0)
#define D(i) (c->x[i].fd[0])
#define CUR_PROCESS(h) ((h) == (u64)-1 || (h) == 0)

// ---- UCRT ---------------------------------------------------------------------------------------------------------
SHIM(_set_thread_local_invalid_parameter_handler) { RET(0); }
SHIM(_j0) { RETD(j0(D(0))); }
SHIM(_j1) { RETD(j1(D(0))); }
SHIM(_jn) { RETD(jn((int)A1, D(1))); }
SHIM(_y0) { RETD(y0(D(0))); }
SHIM(_y1) { RETD(y1(D(0))); }
SHIM(_yn) { RETD(yn((int)A1, D(1))); }
SHIM(_fgetc_nolock) { RET((u32)fgetc(rt_host_file(A1))); }
SHIM(_fputc_nolock) { RET((u32)fputc((int)A1, rt_host_file(A2))); }
SHIM(_wfdopen) { char m[16]; wx_w2u(A2, m, sizeof m); RET(fdopen((int)A1, m)); }
// wctype(name) -> UCRT ctype mask (_UPPER 1 _LOWER 2 _DIGIT 4 _SPACE 8 _PUNCT 16 _CONTROL 32 _BLANK 64 _HEX 128
// _ALPHA 0x103), the value iswctype (win_misc.c) takes.
SHIM(wctype) {
    static const struct { const char *n; u16 m; } t[] = {
        { "alnum", 0x107 }, { "alpha", 0x103 }, { "blank", 0x40 }, { "cntrl", 0x20 }, { "digit", 0x4 }, { "graph", 0x117 },
        { "lower", 0x2 }, { "print", 0x157 }, { "punct", 0x10 }, { "space", 0x8 }, { "upper", 0x1 }, { "xdigit", 0x80 } };
    const char *n = (const char *)A1;
    if (n) for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(n, t[i].n)) RET(t[i].m);
    RET(0);
}
SHIM(_wutime64) {  // (path, struct __utimbuf64 { i64 actime, modtime; } * or NULL)
    char w[2048], h[2048]; wx_w2u(A1, w, sizeof w); rt_host_path(w, h, sizeof h);
    struct timeval tv[2]; const struct timeval *p = NULL;
    if (A2) { tv[0].tv_sec = (time_t)LD64(A2); tv[1].tv_sec = (time_t)LD64(A2 + 8); tv[0].tv_usec = tv[1].tv_usec = 0; p = tv; }
    RET(utimes(h, p) == 0 ? 0 : (u64)-1);
}

// ---- kernel32 -----------------------------------------------------------------------------------------------------
SHIM(ReadProcessMemory) {   // (process, base, buffer, size, *read): only the own process exists
    if (!CUR_PROCESS(A1) && A1 != 0x7e50) { rt_set_last_error(6); RET(0); }
    memmove((void *)A3, (const void *)A2, (size_t)A4);
    if (AN(5)) ST64(AN(5), A4);
    RET(1);
}
SHIM(WriteProcessMemory) {
    if (!CUR_PROCESS(A1) && A1 != 0x7e50) { rt_set_last_error(6); RET(0); }
    memmove((void *)A2, (const void *)A3, (size_t)A4);
    if (AN(5)) ST64(AN(5), A4);
    RET(1);
}
SHIM(GetCurrentThreadStackLimits) {   // (*low, *high): the TEB's StackLimit/StackBase, else the host thread stack
    u64 hi = c->gs_base ? LD64(c->gs_base + 8) : 0, lo = c->gs_base ? LD64(c->gs_base + 16) : 0;
    if (!hi || hi <= lo) {
        pthread_t t = pthread_self();
        hi = (u64)pthread_get_stackaddr_np(t); lo = hi - (u64)pthread_get_stacksize_np(t);
    }
    if (A1) ST64(A1, lo);
    if (A2) ST64(A2, hi);
}
// Write watching (MEM_WRITE_WATCH): macOS has no per-page dirty bits we can read, so every page of the range is
// reported as written. Over-reporting is what Windows' contract allows callers to tolerate (a page can always have
// been written again); it costs extra uploads, never stale data.
SHIM(GetWriteWatch) {   // (flags, base, size, addresses[], *count, *granularity) -> 0 ok
    u64 base = A2 & ~0xfffull, end = (A2 + A3 + 0xfff) & ~0xfffull, arr = A4, pc = AN(5), pg = AN(6);
    if (!pc) RET(87);
    u64 cap = LD64(pc), n = 0;
    for (u64 p = base; p < end && n < cap; p += 0x1000, n++) if (arr) ST64(arr + 8 * n, p);
    ST64(pc, n);
    if (pg) ST32(pg, 0x1000);
    RET(0);
}
SHIM(ResetWriteWatch) { RET(0); }
SHIM(K32GetMappedFileNameA) { WX_FAIL(K32GetMappedFileNameA, 50, 0); }
SHIM(ReOpenFile) {   // (handle, access, share, flags): a second handle on the same open file
    intptr_t fd;
    if (!rt_handle_get(A1, H_FILE, &fd)) { rt_set_last_error(6); RET((u64)-1); }
    int nfd = dup((int)fd);
    if (nfd < 0) { rt_set_last_error(5); RET((u64)-1); }
    RET(rt_handle_new(H_FILE, nfd));
}
SHIM(SetProcessWorkingSetSizeEx) { RET(1); }
SHIM(GetProcessWorkingSetSizeEx) {   // (process, *min, *max, *flags)
    if (A2) ST64(A2, 200ull << 12);
    if (A3) ST64(A3, 1380ull << 12);
    if (A4) ST32(A4, 0);
    RET(1);
}
static __thread u32 thread_error_mode;
SHIM(SetThreadErrorMode) { if (A2) ST32(A2, thread_error_mode); thread_error_mode = (u32)A1; RET(1); }
SHIM(GetThreadErrorMode) { RET(thread_error_mode); }
SHIM(GetNamedPipeHandleStateW) { WX_FAIL(GetNamedPipeHandleStateW, 1, 0); }
SHIM(QueryInformationJobObject) { WX_FAIL(QueryInformationJobObject, 50, 0); }
SHIM(SetProcessInformation) { RET(1); }
SHIM(VirtualLock) { RET(1); }
SHIM(VirtualUnlock) { RET(1); }

// ---- advapi32: security descriptors (single-user host; access is always granted) ---------------------------------
SHIM(AccessCheck) {   // (sd, token, desired, mapping, privset, *privlen, *granted, *status)
    if (AN(7)) ST32(AN(7), (u32)A3);
    if (AN(8)) ST32(AN(8), 1);
    RET(1);
}
SHIM(ConvertStringSecurityDescriptorToSecurityDescriptorW) {   // (str, rev, **sd, *size): an empty absolute SD
    u8 *sd = calloc(1, 40);
    sd[0] = 1; sd[2] = 0; sd[3] = 0x80;   // revision 1, SE_SELF_RELATIVE
    if (A3) ST64(A3, (u64)sd);
    if (A4) ST32(A4, 20);
    RET(1);
}
SHIM(GetFileSecurityW) { WX_FAIL(GetFileSecurityW, 50, 0); }
SHIM(GetSecurityDescriptorDacl) {   // (sd, *present, **dacl, *defaulted): no DACL = full access
    if (A2) ST32(A2, 0);
    if (A3) ST64(A3, 0);
    if (A4) ST32(A4, 0);
    RET(1);
}
SHIM(OpenThreadToken) { rt_set_last_error(1008); RET(0); }   // ERROR_NO_TOKEN: not impersonating
SHIM(SetNamedSecurityInfoW) { RET(0); }
SHIM(SetSecurityInfo) { RET(0); }
SHIM(CertGetEnhancedKeyUsage) { WX_FAIL(CertGetEnhancedKeyUsage, 0x80092004u, 0); }   // CRYPT_E_NOT_FOUND
SHIM(CoSetProxyBlanket) { RET(0); }

// ---- iphlpapi: no change notifications, no interface tables --------------------------------------------------------
SHIM(CancelMibChangeNotify2) { RET(0); }
SHIM(ConvertInterfaceLuidToNameA) { RET(50); }
SHIM(FreeMibTable) { RET(0); }
SHIM(GetExtendedTcpTable) { RET(50); }
SHIM(GetIfTable2) { if (A1) ST64(A1, 0); RET(50); }
static u64 notify_handle(u64 out) { static u64 next = 0x6e000000; u64 h = __atomic_add_fetch(&next, 8, __ATOMIC_SEQ_CST); if (out) ST64(out, h); return 0; }
SHIM(NotifyIpInterfaceChange) { RET(notify_handle(AN(5))); }      // (family, cb, ctx, initial, *handle)
SHIM(NotifyRouteChange2) { RET(notify_handle(AN(5))); }
SHIM(NotifyUnicastIpAddressChange) { RET(notify_handle(AN(5))); }

// ---- setupapi / cfgmgr32: no device tree ---------------------------------------------------------------------------
SHIM(CM_Get_DevNode_Registry_PropertyW) { RET(0x25); }   // CR_NO_SUCH_VALUE
SHIM(CM_Get_Device_IDW) { RET(0x13); }                   // CR_FAILURE
SHIM(CM_Get_Device_ID_Size) { if (A1) ST32(A1, 0); RET(0x13); }

// ---- dxgi (used only to read the HDR monitor's luminance; without it AnyPS5 stays SDR) -----------------------------
SHIM(CreateDXGIFactory1) { if (A2) ST64(A2, 0); RET(0x887A0004u); }   // DXGI_ERROR_UNSUPPORTED

// ---- ntdll ---------------------------------------------------------------------------------------------------------
SHIM(LdrFindEntryForAddress) { RET(0xC0000135u); }   // STATUS_DLL_NOT_FOUND: no LDR_DATA_TABLE_ENTRY list
SHIM(LdrGetDllHandle) {   // (path, flags, UNICODE_STRING *name, *handle)
    char b[520] = { 0 };
    if (A3) { u16 len = LD16(A3); u64 buf = LD64(A3 + 8); size_t n = 0;
              for (u16 i = 0; i < len / 2 && n + 1 < sizeof b; i++) { u16 ch = LD16(buf + 2 * i); b[n++] = ch < 128 ? (char)ch : '?'; } }
    u64 h = rt_module_handle(b);
    if (!h) { if (A4) ST64(A4, 0); RET(0xC0000135u); }
    if (A4) ST64(A4, h);
    RET(0);
}
SHIM(NtProtectVirtualMemory) {   // (process, *base, *size, prot, *old)
    u32 old = 0;
    u64 ok = call4(c, "VirtualProtect", A2 ? LD64(A2) : 0, A3 ? LD64(A3) : 0, A4, (u64)&old);
    if (AN(5)) ST32(AN(5), old);
    RET(ok ? 0 : 0xC0000045u);   // STATUS_INVALID_PAGE_PROTECTION
}
SHIM(RtlGetCurrentPeb) { RET(rt_peb); }
SHIM(RtlGetProcessHeaps) { if ((u32)A1 && A2) ST64(A2, call4(c, "GetProcessHeap", 0, 0, 0, 0)); RET(1); }
SHIM(RtlQueryEnvironmentVariable) {   // (env, name, namelen chars, value, valuelen chars, *retlen chars)
    char n[512]; size_t k = 0;
    for (u64 i = 0; i < A3 && k + 1 < sizeof n; i++) { u16 ch = LD16(A2 + 2 * i); n[k++] = ch < 128 ? (char)ch : '?'; }
    n[k] = 0;
    u16 wn[512]; for (size_t i = 0; i <= k; i++) wn[i] = (u8)n[i];
    u32 got = (u32)call4(c, "GetEnvironmentVariableW", (u64)wn, A4, AN(5), 0);
    if (AN(6)) ST64(AN(6), got);
    if (!got) RET(0xC0000100u);                 // STATUS_VARIABLE_NOT_FOUND
    RET(got >= AN(5) ? 0x80000005u : 0);        // STATUS_BUFFER_OVERFLOW
}

// ---- comctl32 window subclassing -----------------------------------------------------------------------------------
// The subclass procedure is recorded but not inserted into message dispatch: AnyPS5's video output uses it only to
// observe size/close messages of the SDL window, which our user32 also reports through SDL events.
SHIM(SetWindowSubclass) { wx_stub_log("SetWindowSubclass"); RET(1); }
SHIM(RemoveWindowSubclass) { RET(1); }
SHIM(DefSubclassProc) { RET(call4(c, "DefWindowProcW", A1, A2, A3, A4)); }

const Shim shims_ps5[] = {
#define E(n) { NULL, #n, sh_##n }
    E(_set_thread_local_invalid_parameter_handler), E(_j0), E(_j1), E(_jn), E(_y0), E(_y1), E(_yn),
    E(_fgetc_nolock), E(_fputc_nolock), E(_wfdopen), E(wctype), E(_wutime64),
    E(ReadProcessMemory), E(WriteProcessMemory), E(GetCurrentThreadStackLimits), E(GetWriteWatch), E(ResetWriteWatch),
    E(K32GetMappedFileNameA), E(ReOpenFile), E(SetProcessWorkingSetSizeEx), E(GetProcessWorkingSetSizeEx),
    E(SetThreadErrorMode), E(GetThreadErrorMode), E(GetNamedPipeHandleStateW), E(QueryInformationJobObject),
    E(SetProcessInformation), E(VirtualLock), E(VirtualUnlock),
    E(AccessCheck), E(ConvertStringSecurityDescriptorToSecurityDescriptorW), E(GetFileSecurityW), E(GetSecurityDescriptorDacl),
    E(OpenThreadToken), E(SetNamedSecurityInfoW), E(SetSecurityInfo), E(CertGetEnhancedKeyUsage), E(CoSetProxyBlanket),
    E(CancelMibChangeNotify2), E(ConvertInterfaceLuidToNameA), E(FreeMibTable), E(GetExtendedTcpTable), E(GetIfTable2),
    E(NotifyIpInterfaceChange), E(NotifyRouteChange2), E(NotifyUnicastIpAddressChange),
    E(CM_Get_DevNode_Registry_PropertyW), E(CM_Get_Device_IDW), E(CM_Get_Device_ID_Size),
    { "dxgi.dll", "CreateDXGIFactory1", sh_CreateDXGIFactory1 },
    E(LdrFindEntryForAddress), E(LdrGetDllHandle), E(NtProtectVirtualMemory), E(RtlGetCurrentPeb), E(RtlGetProcessHeaps),
    E(RtlQueryEnvironmentVariable),
    E(SetWindowSubclass), E(RemoveWindowSubclass), E(DefSubclassProc),
    { NULL, NULL, NULL } };
