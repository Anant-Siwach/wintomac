// COM basics (ole32 / combase): apartments, CoCreateInstance routing to host-implemented classes,
// task-memory allocator, GUID helpers, PROPVARIANT.
//
// Host COM objects follow one pattern: guest-visible { vtbl*, ...host fields }, where vtbl is an array of
// thunk addresses for shims named "__wintomac_<Class>_<Method>". rt_com_vtable() builds such a table.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "win32.h"
#include "com.h"

u64 *rt_com_vtable(const char *cls, const char *const *methods) {
    int n = 0; while (methods[n]) n++;
    u64 *v = calloc(n + 1, sizeof(u64));
    for (int i = 0; i < n; i++) {
        char name[128]; snprintf(name, sizeof name, "__wintomac_%s_%s", cls, methods[i]);
        v[i] = rt_thunk_for(NULL, name);
        if (!v[i]) rt_log("com: missing method shim %s", name);
    }
    return v;
}

int rt_guid_eq(u64 g, const Guid *want) { return g && !memcmp((void *)g, want, 16); }
const Guid IID_IUnknown_ = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

void rt_guid_str(u64 g, char *out, size_t n) {
    const Guid *x = (const Guid *)g;
    snprintf(out, n, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", x->d1, x->d2, x->d3,
             x->d4[0], x->d4[1], x->d4[2], x->d4[3], x->d4[4], x->d4[5], x->d4[6], x->d4[7]);
}

// ---- class registry: CLSID -> factory (filled by audio and other subsystems) ---------------------
typedef struct { const Guid *clsid; u32 (*create)(Ctx *c, u64 iid, u64 out); const char *name; } ComClass;
static ComClass classes[64]; static int nclasses;
void rt_com_register(const Guid *clsid, u32 (*create)(Ctx *, u64, u64), const char *name) {
    if (nclasses < 64) classes[nclasses++] = (ComClass){ clsid, create, name };
}

static __thread int apartment_depth;
SHIM(CoInitialize) { RET(apartment_depth++ ? 1 /* S_FALSE */ : 0); }
SHIM(CoInitializeEx) { RET(apartment_depth++ ? 1 : 0); }
SHIM(CoUninitialize) { if (apartment_depth > 0) apartment_depth--; RET(0); }
SHIM(OleInitialize) { RET(apartment_depth++ ? 1 : 0); }
SHIM(OleUninitialize) { if (apartment_depth > 0) apartment_depth--; RET(0); }
SHIM(CoInitializeSecurity) { RET(0); }

// CoCreateInstance(rclsid, outer, clsctx, riid, ppv)
SHIM(CoCreateInstance) {
    if (AN(5)) ST64(AN(5), 0);
    for (int i = 0; i < nclasses; i++)
        if (rt_guid_eq(A1, classes[i].clsid)) {
            u32 hr = classes[i].create(c, A4, AN(5));
            if (rt_trace_enabled()) rt_log("CoCreateInstance(%s) = 0x%08x", classes[i].name, hr);
            RET(hr);
        }
    char s[64]; rt_guid_str(A1, s, sizeof s);
    rt_log("CoCreateInstance: class %s not implemented", s);
    RET(0x80040154u);  // REGDB_E_CLASSNOTREG
}
SHIM(CoCreateInstanceEx) {  // (clsid, outer, ctx, server, count, MULTI_QI*)
    u32 n = (u32)AN(5); u64 mqi = AN(6), ok = 0;
    for (u32 k = 0; k < n; k++) {
        u64 e = mqi + 24ull * k;  // { const IID *pIID; IUnknown *pItf; HRESULT hr; }
        u32 hr = 0x80040154u;
        for (int i = 0; i < nclasses; i++) if (rt_guid_eq(A1, classes[i].clsid)) hr = classes[i].create(c, LD64(e), e + 8);
        ST32(e + 16, hr); if (!(hr & 0x80000000u)) ok++;
    }
    RET(ok == n ? 0 : ok ? 0x00040012u /* CO_S_NOTALLINTERFACES */ : 0x80004002u);
}

// Task memory
SHIM(CoTaskMemAlloc) { RET(malloc(A1 ? A1 : 1)); }
SHIM(CoTaskMemRealloc) { RET(realloc((void *)A1, A2)); }
SHIM(CoTaskMemFree) { free((void *)A1); RET(0); }

SHIM(CoCreateGuid) { arc4random_buf((void *)A1, 16); u8 *b = (u8 *)A1; b[7] = (b[7] & 0x0F) | 0x40; b[8] = (b[8] & 0x3F) | 0x80; RET(0); }
SHIM(StringFromGUID2) {  // (guid, LPOLESTR, cch)
    char s[64]; rt_guid_str(A1, s, sizeof s); size_t n = strlen(s);
    if ((u32)A3 <= n) RET(0);
    for (size_t i = 0; i <= n; i++) ST16(A2 + 2 * i, (u8)s[i]);
    RET(n + 1);
}
SHIM(IsEqualGUID) { RET(!memcmp((void *)A1, (void *)A2, 16)); }

// PROPVARIANT: free what PropVariantClear owns (we only ever hand out VT_LPWSTR = 31)
SHIM(PropVariantClear) {
    u16 vt = LD16(A1);
    if (vt == 31 || vt == 30) free((void *)LD64(A1 + 8));
    else if (vt == 65) free((void *)LD64(A1 + 16));   // VT_BLOB (wasapi device formats)
    memset((void *)A1, 0, 24);
    RET(0);
}

const Shim shims_com[] = {
#define E(n) { NULL, #n, sh_##n }
    E(CoInitialize), E(CoInitializeEx), E(CoUninitialize), E(OleInitialize), E(OleUninitialize), E(CoInitializeSecurity),
    E(CoCreateInstance), E(CoCreateInstanceEx), E(CoTaskMemAlloc), E(CoTaskMemRealloc), E(CoTaskMemFree),
    E(CoCreateGuid), E(StringFromGUID2), E(IsEqualGUID), E(PropVariantClear),
#undef E
    { 0, 0, 0 }
};
