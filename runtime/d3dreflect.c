// D3DReflect: guest-callable ID3D11ShaderReflection / ID3D12ShaderReflection over vkd3d-utils' native
// ID3D12ShaderReflection. The D3D11 and D3D12 interfaces have the same method order and, except for
// D3D11_SHADER_INPUT_BIND_DESC (a prefix of the D3D12 struct), identical descriptor layouts.
//
// Each guest object is { guest_vtbl*, host_obj* }. Guest vtables hold thunk addresses for the shims below.
#define INITGUID
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <vkd3d/vkd3d_utils.h>
#include <vkd3d/vkd3d_d3d12shader.h>

#include "win32.h"

DEFINE_GUID(WT_IID_ID3D11ShaderReflection, 0x8d536ca1, 0x0cca, 0x4956, 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84);

typedef struct { u64 vtbl; void *host; u32 refs; int d3d11; } GObj;   // reflection (refcounted)
typedef struct { u64 vtbl; void *host; } GSub;                        // owned sub-objects (cb / var / type)

enum { K_REFL, K_CB, K_VAR, K_TYPE };
static u64 vtbls[4][24];
static const char *const methods[4][24] = {
    { "QueryInterface", "AddRef", "Release", "GetDesc", "GetConstantBufferByIndex", "GetConstantBufferByName",
      "GetResourceBindingDesc", "GetInputParameterDesc", "GetOutputParameterDesc", "GetPatchConstantParameterDesc",
      "GetVariableByName", "GetResourceBindingDescByName", "GetMovInstructionCount", "GetMovcInstructionCount",
      "GetConversionInstructionCount", "GetBitwiseInstructionCount", "GetGSInputPrimitive", "IsSampleFrequencyShader",
      "GetNumInterfaceSlots", "GetMinFeatureLevel", "GetThreadGroupSize", "GetRequiresFlags", NULL },
    { "GetDesc", "GetVariableByIndex", "GetVariableByName", NULL },
    { "GetDesc", "GetType", "GetBuffer", "GetInterfaceSlot", NULL },
    { "GetDesc", "GetMemberTypeByIndex", "GetMemberTypeByName", "GetMemberTypeName", "IsEqual", "GetSubType",
      "GetBaseClass", "GetNumInterfaces", "GetInterfaceByIndex", "IsOfType", "ImplementsInterface", NULL },
};
static const char *const prefixes[4] = { "Refl", "ReflCB", "ReflVar", "ReflType" };

static void init_vtbls(void) {
    static int done; if (done) return; done = 1;
    for (int k = 0; k < 4; k++)
        for (int i = 0; methods[k][i]; i++) {
            char n[96]; snprintf(n, sizeof n, "__wintomac_%s_%s", prefixes[k], methods[k][i]);
            vtbls[k][i] = rt_thunk_for(NULL, n);
            if (!vtbls[k][i]) rt_log("d3dreflect: missing shim %s", n);
        }
}

// Stable wrapper per host sub-object (callers compare interface pointers).
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct Ent { void *host; GSub *w; struct Ent *next; } Ent;
static Ent *buckets[1024];
static GSub null_obj[4];
static u64 wrap(int kind, void *host) {
    if (!host) { null_obj[kind].vtbl = (u64)vtbls[kind]; null_obj[kind].host = NULL; return (u64)&null_obj[kind]; }
    size_t h = ((uintptr_t)host >> 4) & 1023;
    pthread_mutex_lock(&map_lock);
    for (Ent *e = buckets[h]; e; e = e->next) if (e->host == host) { pthread_mutex_unlock(&map_lock); return (u64)e->w; }
    GSub *w = malloc(sizeof *w); w->vtbl = (u64)vtbls[kind]; w->host = host;
    Ent *e = malloc(sizeof *e); e->host = host; e->w = w; e->next = buckets[h]; buckets[h] = e;
    pthread_mutex_unlock(&map_lock);
    return (u64)w;
}
#define HOST(T) ((T *)((GSub *)A1)->host)
#define NULLCHK(fail) do { if (!((GSub *)A1)->host) RET(fail); } while (0)   /* D3D null object */
#define E_FAIL_ 0x80004005u
#define R ID3D12ShaderReflection
#define CB ID3D12ShaderReflectionConstantBuffer
#define VAR ID3D12ShaderReflectionVariable
#define TY ID3D12ShaderReflectionType

// ---- ID3D1xShaderReflection ------------------------------------------------------------------
SHIM(Refl_QueryInterface) {
    GObj *o = (GObj *)A1; const GUID *iid = (const GUID *)A2;
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_ID3D12ShaderReflection) || IsEqualGUID(iid, &WT_IID_ID3D11ShaderReflection)) {
        __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST); ST64(A3, A1); RET(S_OK);
    }
    ST64(A3, 0); RET(E_NOINTERFACE);
}
SHIM(Refl_AddRef) { RET(__atomic_add_fetch(&((GObj *)A1)->refs, 1, __ATOMIC_SEQ_CST)); }
SHIM(Refl_Release) {
    GObj *o = (GObj *)A1; u32 r = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
    if (!r) { R *h = o->host; h->lpVtbl->Release(h); free(o); }  // sub-object wrappers stay cached (tiny)
    RET(r);
}
SHIM(Refl_GetDesc) { RET(HOST(R)->lpVtbl->GetDesc(HOST(R), (D3D12_SHADER_DESC *)A2)); }
SHIM(Refl_GetConstantBufferByIndex) { RET(wrap(K_CB, HOST(R)->lpVtbl->GetConstantBufferByIndex(HOST(R), (UINT)A2))); }
static CB *cb_by_name(R *r, const char *name) {
    D3D12_SHADER_DESC d; if (FAILED(r->lpVtbl->GetDesc(r, &d)) || !name) return NULL;
    for (UINT i = 0; i < d.ConstantBuffers; i++) {
        CB *cb = r->lpVtbl->GetConstantBufferByIndex(r, i); D3D12_SHADER_BUFFER_DESC bd;
        if (cb && SUCCEEDED(cb->lpVtbl->GetDesc(cb, &bd)) && bd.Name && !strcmp(bd.Name, name)) return cb;
    }
    return NULL;
}
static VAR *var_in_cb(CB *cb, const char *name) {
    D3D12_SHADER_BUFFER_DESC bd; if (!cb || FAILED(cb->lpVtbl->GetDesc(cb, &bd)) || !name) return NULL;
    for (UINT i = 0; i < bd.Variables; i++) {
        VAR *v = cb->lpVtbl->GetVariableByIndex(cb, i); D3D12_SHADER_VARIABLE_DESC vd;
        if (v && SUCCEEDED(v->lpVtbl->GetDesc(v, &vd)) && vd.Name && !strcmp(vd.Name, name)) return v;
    }
    return NULL;
}
SHIM(Refl_GetConstantBufferByName) { RET(wrap(K_CB, cb_by_name(HOST(R), (const char *)A2))); }
static void copy_bind(GObj *o, u64 out, const D3D12_SHADER_INPUT_BIND_DESC *d) {
    // D3D11_SHADER_INPUT_BIND_DESC == first 40 bytes of the D3D12 struct (no Space / uID)
    memcpy((void *)out, d, o->d3d11 ? 40 : sizeof *d);
}
SHIM(Refl_GetResourceBindingDesc) {
    D3D12_SHADER_INPUT_BIND_DESC d; HRESULT hr = HOST(R)->lpVtbl->GetResourceBindingDesc(HOST(R), (UINT)A2, &d);
    if (SUCCEEDED(hr)) copy_bind((GObj *)A1, A3, &d);
    RET((u32)hr);
}
SHIM(Refl_GetResourceBindingDescByName) {
    R *r = HOST(R); D3D12_SHADER_DESC sd; const char *name = (const char *)A2;
    if (!name || FAILED(r->lpVtbl->GetDesc(r, &sd))) RET(E_INVALIDARG);
    for (UINT i = 0; i < sd.BoundResources; i++) {
        D3D12_SHADER_INPUT_BIND_DESC d;
        if (SUCCEEDED(r->lpVtbl->GetResourceBindingDesc(r, i, &d)) && d.Name && !strcmp(d.Name, name)) { copy_bind((GObj *)A1, A3, &d); RET(S_OK); }
    }
    RET(E_INVALIDARG);
}
SHIM(Refl_GetInputParameterDesc) { RET((u32)HOST(R)->lpVtbl->GetInputParameterDesc(HOST(R), (UINT)A2, (D3D12_SIGNATURE_PARAMETER_DESC *)A3)); }
SHIM(Refl_GetOutputParameterDesc) { RET((u32)HOST(R)->lpVtbl->GetOutputParameterDesc(HOST(R), (UINT)A2, (D3D12_SIGNATURE_PARAMETER_DESC *)A3)); }
SHIM(Refl_GetPatchConstantParameterDesc) { RET((u32)HOST(R)->lpVtbl->GetPatchConstantParameterDesc(HOST(R), (UINT)A2, (D3D12_SIGNATURE_PARAMETER_DESC *)A3)); }
SHIM(Refl_GetVariableByName) {  // searches every constant buffer, like D3D
    R *r = HOST(R); D3D12_SHADER_DESC d;
    if (SUCCEEDED(r->lpVtbl->GetDesc(r, &d)))
        for (UINT i = 0; i < d.ConstantBuffers; i++) { VAR *v = var_in_cb(r->lpVtbl->GetConstantBufferByIndex(r, i), (const char *)A2); if (v) RET(wrap(K_VAR, v)); }
    RET(wrap(K_VAR, NULL));
}
SHIM(Refl_GetMovInstructionCount) { RET(HOST(R)->lpVtbl->GetMovInstructionCount(HOST(R))); }
SHIM(Refl_GetMovcInstructionCount) { RET(HOST(R)->lpVtbl->GetMovcInstructionCount(HOST(R))); }
SHIM(Refl_GetConversionInstructionCount) { RET(HOST(R)->lpVtbl->GetConversionInstructionCount(HOST(R))); }
SHIM(Refl_GetBitwiseInstructionCount) { RET(HOST(R)->lpVtbl->GetBitwiseInstructionCount(HOST(R))); }
SHIM(Refl_GetGSInputPrimitive) { RET(HOST(R)->lpVtbl->GetGSInputPrimitive(HOST(R))); }
SHIM(Refl_IsSampleFrequencyShader) { RET(HOST(R)->lpVtbl->IsSampleFrequencyShader(HOST(R))); }
SHIM(Refl_GetNumInterfaceSlots) { RET(HOST(R)->lpVtbl->GetNumInterfaceSlots(HOST(R))); }
SHIM(Refl_GetMinFeatureLevel) {  // from the shader model (vkd3d-utils stubs this)
    D3D12_SHADER_DESC d; if (!A2 || FAILED(HOST(R)->lpVtbl->GetDesc(HOST(R), &d))) RET(E_INVALIDARG);
    u32 major = (d.Version >> 4) & 0xF, minor = d.Version & 0xF;
    ST32(A2, major >= 5 ? 0xb000 : major == 4 ? (minor ? 0xa100 : 0xa000) : 0x9100);  // 11_0 / 10_1 / 10_0 / 9_1
    RET(S_OK);
}
SHIM(Refl_GetThreadGroupSize) { RET(HOST(R)->lpVtbl->GetThreadGroupSize(HOST(R), (UINT *)A2, (UINT *)A3, (UINT *)A4)); }
SHIM(Refl_GetRequiresFlags) { RET(HOST(R)->lpVtbl->GetRequiresFlags(HOST(R))); }

// ---- constant buffers / variables / types ----------------------------------------------------
SHIM(ReflCB_GetDesc) { NULLCHK(E_FAIL_); RET((u32)HOST(CB)->lpVtbl->GetDesc(HOST(CB), (D3D12_SHADER_BUFFER_DESC *)A2)); }
SHIM(ReflCB_GetVariableByIndex) { NULLCHK(wrap(K_VAR, NULL)); RET(wrap(K_VAR, HOST(CB)->lpVtbl->GetVariableByIndex(HOST(CB), (UINT)A2))); }
SHIM(ReflCB_GetVariableByName) { NULLCHK(wrap(K_VAR, NULL)); RET(wrap(K_VAR, var_in_cb(HOST(CB), (const char *)A2))); }
SHIM(ReflVar_GetDesc) { NULLCHK(E_FAIL_); RET((u32)HOST(VAR)->lpVtbl->GetDesc(HOST(VAR), (D3D12_SHADER_VARIABLE_DESC *)A2)); }
SHIM(ReflVar_GetType) { NULLCHK(wrap(K_TYPE, NULL)); RET(wrap(K_TYPE, HOST(VAR)->lpVtbl->GetType(HOST(VAR)))); }
SHIM(ReflVar_GetBuffer) { NULLCHK(wrap(K_CB, NULL)); RET(wrap(K_CB, HOST(VAR)->lpVtbl->GetBuffer(HOST(VAR)))); }
SHIM(ReflVar_GetInterfaceSlot) { NULLCHK(0xFFFFFFFF); RET(HOST(VAR)->lpVtbl->GetInterfaceSlot(HOST(VAR), (UINT)A2)); }
static TY *unwrap_type(u64 g) { return g ? (TY *)((GSub *)g)->host : NULL; }
SHIM(ReflType_GetDesc) { NULLCHK(E_FAIL_); RET((u32)HOST(TY)->lpVtbl->GetDesc(HOST(TY), (D3D12_SHADER_TYPE_DESC *)A2)); }
SHIM(ReflType_GetMemberTypeByIndex) { NULLCHK(wrap(K_TYPE, NULL)); RET(wrap(K_TYPE, HOST(TY)->lpVtbl->GetMemberTypeByIndex(HOST(TY), (UINT)A2))); }
SHIM(ReflType_GetMemberTypeByName) { NULLCHK(wrap(K_TYPE, NULL)); RET(wrap(K_TYPE, HOST(TY)->lpVtbl->GetMemberTypeByName(HOST(TY), (const char *)A2))); }
SHIM(ReflType_GetMemberTypeName) { NULLCHK(0); RET(HOST(TY)->lpVtbl->GetMemberTypeName(HOST(TY), (UINT)A2)); }
SHIM(ReflType_IsEqual) { NULLCHK(E_FAIL_); RET((u32)HOST(TY)->lpVtbl->IsEqual(HOST(TY), unwrap_type(A2))); }
SHIM(ReflType_GetSubType) { NULLCHK(wrap(K_TYPE, NULL)); RET(wrap(K_TYPE, HOST(TY)->lpVtbl->GetSubType(HOST(TY)))); }
SHIM(ReflType_GetBaseClass) { NULLCHK(wrap(K_TYPE, NULL)); RET(wrap(K_TYPE, HOST(TY)->lpVtbl->GetBaseClass(HOST(TY)))); }
SHIM(ReflType_GetNumInterfaces) { NULLCHK(0); RET(HOST(TY)->lpVtbl->GetNumInterfaces(HOST(TY))); }
SHIM(ReflType_GetInterfaceByIndex) { NULLCHK(wrap(K_TYPE, NULL)); RET(wrap(K_TYPE, HOST(TY)->lpVtbl->GetInterfaceByIndex(HOST(TY), (UINT)A2))); }
SHIM(ReflType_IsOfType) { NULLCHK(E_FAIL_); RET((u32)HOST(TY)->lpVtbl->IsOfType(HOST(TY), unwrap_type(A2))); }
SHIM(ReflType_ImplementsInterface) { NULLCHK(E_FAIL_); RET((u32)HOST(TY)->lpVtbl->ImplementsInterface(HOST(TY), unwrap_type(A2))); }

// D3DReflect(const void *data, SIZE_T size, REFIID iid, void **out)
SHIM(D3DReflect) {
    const GUID *iid = (const GUID *)A3;
    if (A4) ST64(A4, 0);
    int d3d11 = IsEqualGUID(iid, &WT_IID_ID3D11ShaderReflection);
    if (!d3d11 && !IsEqualGUID(iid, &IID_ID3D12ShaderReflection)) { rt_log("D3DReflect: unsupported interface"); RET(E_NOINTERFACE); }
    init_vtbls();
    void *host = NULL;
    HRESULT hr = D3DReflect((const void *)A1, (SIZE_T)A2, &IID_ID3D12ShaderReflection, &host);
    if (FAILED(hr)) RET((u32)hr);
    GObj *o = calloc(1, sizeof *o);
    o->vtbl = (u64)vtbls[K_REFL]; o->host = host; o->refs = 1; o->d3d11 = d3d11;
    ST64(A4, (u64)o);
    RET(S_OK);
}

const Shim shims_d3dreflect[] = {
    { NULL, "D3DReflect", sh_D3DReflect },
#define E(n) { NULL, "__wintomac_" #n, sh_##n }
    E(Refl_QueryInterface), E(Refl_AddRef), E(Refl_Release), E(Refl_GetDesc), E(Refl_GetConstantBufferByIndex),
    E(Refl_GetConstantBufferByName), E(Refl_GetResourceBindingDesc), E(Refl_GetInputParameterDesc),
    E(Refl_GetOutputParameterDesc), E(Refl_GetPatchConstantParameterDesc), E(Refl_GetVariableByName),
    E(Refl_GetResourceBindingDescByName), E(Refl_GetMovInstructionCount), E(Refl_GetMovcInstructionCount),
    E(Refl_GetConversionInstructionCount), E(Refl_GetBitwiseInstructionCount), E(Refl_GetGSInputPrimitive),
    E(Refl_IsSampleFrequencyShader), E(Refl_GetNumInterfaceSlots), E(Refl_GetMinFeatureLevel),
    E(Refl_GetThreadGroupSize), E(Refl_GetRequiresFlags),
    E(ReflCB_GetDesc), E(ReflCB_GetVariableByIndex), E(ReflCB_GetVariableByName),
    E(ReflVar_GetDesc), E(ReflVar_GetType), E(ReflVar_GetBuffer), E(ReflVar_GetInterfaceSlot),
    E(ReflType_GetDesc), E(ReflType_GetMemberTypeByIndex), E(ReflType_GetMemberTypeByName), E(ReflType_GetMemberTypeName),
    E(ReflType_IsEqual), E(ReflType_GetSubType), E(ReflType_GetBaseClass), E(ReflType_GetNumInterfaces),
    E(ReflType_GetInterfaceByIndex), E(ReflType_IsOfType), E(ReflType_ImplementsInterface),
#undef E
    { 0, 0, 0 }
};
