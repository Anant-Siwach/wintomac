// d3d12.dll breadth for real engines (Unreal Engine 4/5, Unity): interfaces newer than libvkd3d's, and the
// helper objects engines query at startup.
//   * QueryInterface for every wrapped object (d3d12_qi): ID3D12Device10..14, ID3D12GraphicsCommandList7..10 and
//     ID3D12Heap1 answer with the same object (their extra methods are below); the device also answers
//     ID3D12InfoQueue(1) and ID3D12DeviceRemovedExtendedData(1,2) with harmless objects. Anything else goes to
//     libvkd3d, and unknown IIDs are logged once.
//   * ID3D12Device10..14: CreateCommittedResource3 / CreatePlacedResource2 / CreateReservedResource2 (barrier
//     layouts -> initial states), CreateSampler2, GetResourceAllocationInfo3; CreateCommandQueue1.
//   * ID3D12GraphicsCommandList7..10: enhanced barriers translated to legacy ones (OPTIONS12 still reports them
//     unsupported, so engines normally don't use them), OMSetFrontAndBackStencilRef, dynamic depth bias / strip cut
//     (logged), work graphs (no-ops).
//   * dstorage.dll: DStorageGetFactory & co. fail cleanly, so titles take their Win32 file I/O path.
// Interface layouts: Microsoft's DirectX-Headers (MIT), see third_party/directx-headers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>
#include <pthread.h>
#include "d3d12_internal.h"

#define DXGI_ERROR_NOT_CURRENTLY_AVAILABLE_ 0x887A0022u
#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }

static int guid_eq(u64 g, const GUID *x) { return g && !memcmp((void *)g, x, 16); }
static void guid_str(u64 g, char *buf, size_t n) {
    const GUID *x = (const GUID *)g;
    snprintf(buf, n, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", x->Data1, x->Data2, x->Data3,
             x->Data4[0], x->Data4[1], x->Data4[2], x->Data4[3], x->Data4[4], x->Data4[5], x->Data4[6], x->Data4[7]);
}

// ---- QueryInterface ---------------------------------------------------------------------------------------
// Newer interfaces implemented by the same wrapper (their extra vtable slots are the shims in this file).
static const struct { const char *kind; GUID iid; } same_object[] = {
    { "Device", G(0x517f8718, 0xaa66, 0x49f9, 0xb0, 0x2b, 0xa7, 0xab, 0x89, 0xc0, 0x60, 0x31) },   // ID3D12Device10
    { "Device", G(0x5405c344, 0xd457, 0x444e, 0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39) },   // ID3D12Device11
    { "Device", G(0x5af5c532, 0x4c91, 0x4cd0, 0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5) },   // ID3D12Device12
    { "Device", G(0x14eecffc, 0x4df8, 0x40f7, 0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e) },   // ID3D12Device13
    { "Device", G(0x5f6e592d, 0xd895, 0x44c2, 0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23) },   // ID3D12Device14
    { "List", G(0xdd171223, 0x8b61, 0x4769, 0x90, 0xe3, 0x16, 0x0c, 0xcd, 0xe4, 0xe2, 0xc1) },     // ID3D12GraphicsCommandList7
    { "List", G(0xee936ef9, 0x599d, 0x4d28, 0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce, 0x51) },     // ID3D12GraphicsCommandList8
    { "List", G(0x34ed2808, 0xffe6, 0x4c2b, 0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59, 0xe1) },     // ID3D12GraphicsCommandList9
    { "List", G(0x7013c015, 0xd161, 0x4b63, 0xa0, 0x8c, 0x23, 0x85, 0x52, 0xdd, 0x8a, 0xcc) },     // ID3D12GraphicsCommandList10
    { "Heap", G(0x572f7389, 0x2168, 0x49e3, 0x96, 0x93, 0xd6, 0xdf, 0x58, 0x71, 0xbf, 0x6d) },     // ID3D12Heap1
};
static const GUID iid_info_queue[] = {
    G(0x0742a90b, 0xc387, 0x483f, 0xb9, 0x46, 0x30, 0xa7, 0xe4, 0xe6, 0x14, 0x58),   // ID3D12InfoQueue
    G(0x2852dd88, 0xb484, 0x4c0c, 0xb6, 0xb1, 0x67, 0x16, 0x85, 0x00, 0xe6, 0x00),   // ID3D12InfoQueue1
};
static const GUID iid_dred[] = {
    G(0x98931d33, 0x5ae8, 0x4791, 0xaa, 0x3c, 0x1a, 0x73, 0xa2, 0x93, 0x4e, 0x71),   // ID3D12DeviceRemovedExtendedData
    G(0x9727a022, 0xcf1d, 0x4dda, 0x9e, 0xba, 0xef, 0xfa, 0x65, 0x3f, 0xc5, 0x06),   // ID3D12DeviceRemovedExtendedData1
    G(0x67fc5816, 0xe4ca, 0x4915, 0xbf, 0x18, 0x42, 0x54, 0x12, 0x72, 0xda, 0x54),   // ID3D12DeviceRemovedExtendedData2
};
static const GUID iid_dred_settings[] = {
    G(0x82bc481c, 0x6b9b, 0x4030, 0xae, 0xdb, 0x7e, 0xe3, 0xd1, 0xdf, 0x1e, 0x63),   // ID3D12DeviceRemovedExtendedDataSettings
    G(0xdbd5ae51, 0x3317, 0x4f0a, 0xad, 0xf9, 0x1d, 0x7c, 0xed, 0xca, 0xae, 0x0b),   // ...Settings1
    G(0x61552388, 0x01ab, 0x4008, 0xa4, 0x36, 0x83, 0xdb, 0x18, 0x95, 0x66, 0xea),   // ...Settings2
};
static const GUID iid_unknown = G(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
#define IN_LIST(g, list) in_list(g, list, sizeof list / sizeof *list)
static int in_list(u64 g, const GUID *l, size_t n) { for (size_t i = 0; i < n; i++) if (guid_eq(g, &l[i])) return 1; return 0; }
int d3d12_is_dred_settings_iid(u64 g) { return IN_LIST(g, iid_dred_settings); }
static u64 d3d12_dred(void);

void d3d12_qi(Ctx *c, const char *kind) {   // (This, REFIID, void **)
    IUnknown *h = d3d12_host(A1);
    if (!A3) RET(E_POINTER_);
    ST64(A3, 0);
    if (!h || !A2) RET(E_INVALIDARG_);
    for (size_t i = 0; i < sizeof same_object / sizeof *same_object; i++)
        if (!strcmp(same_object[i].kind, kind) && guid_eq(A2, &same_object[i].iid)) {
            h->lpVtbl->AddRef(h); ST64(A3, A1); RET(S_OK);
        }
    if (!strcmp(kind, "Device")) {
        if (IN_LIST(A2, iid_info_queue)) { ST64(A3, d3d12_info_queue()); RET(S_OK); }
        if (IN_LIST(A2, iid_dred)) { ST64(A3, d3d12_dred()); RET(S_OK); }
    }
    u32 hr = h->lpVtbl->QueryInterface(h, (REFIID)A2, (void **)A3);
    if (SUCCEEDED(hr)) RET(d3d12_out(A3, hr));
    char g[48], m[160]; guid_str(A2, g, sizeof g);
    snprintf(m, sizeof m, "ID3D12 %s::QueryInterface(%s)", kind, g); d3d12_log_once(m);
    RET(hr);
}

// ---- static helper objects (no reference counting: process-lifetime singletons) ---------------------------
typedef struct { u64 vtbl; } StaticObj;
// vtable: one shim per method name "__wintomac_<prefix>_<name>", falling back to a shim that returns 0
// (S_OK / FALSE / 0 count), which is the right answer for nearly every method of these objects.
static u64 static_vtbl(const char *prefix, const char *const *names) {
    int n = 0; while (names[n]) n++;
    u64 *v = calloc(n + 1, 8), zero = rt_thunk_for(NULL, "__wintomac_D3D12Ext_Zero");
    for (int i = 0; i < n; i++) {
        char nm[160]; snprintf(nm, sizeof nm, "__wintomac_%s_%s", prefix, names[i]);
        v[i] = rt_thunk_for(NULL, nm); if (!v[i]) v[i] = zero;
    }
    return (u64)v;
}
SHIM(D3D12Ext_Zero) { RET(0); }
SHIM(D3D12Ext_One) { RET(1); }   // AddRef / Release of singletons

// ID3D12InfoQueue1: empty message queue; filters, breaks and callbacks are accepted and ignored.
static const char *const iq_methods[] = { "QueryInterface", "AddRef", "Release", "SetMessageCountLimit", "ClearStoredMessages",
    "GetMessage", "GetNumMessagesAllowedByStorageFilter", "GetNumMessagesDeniedByStorageFilter", "GetNumStoredMessages",
    "GetNumStoredMessagesAllowedByRetrievalFilter", "GetNumMessagesDiscardedByMessageCountLimit", "GetMessageCountLimit",
    "AddStorageFilterEntries", "GetStorageFilter", "ClearStorageFilter", "PushEmptyStorageFilter", "PushCopyOfStorageFilter",
    "PushStorageFilter", "PopStorageFilter", "GetStorageFilterStackSize", "AddRetrievalFilterEntries", "GetRetrievalFilter",
    "ClearRetrievalFilter", "PushEmptyRetrievalFilter", "PushCopyOfRetrievalFilter", "PushRetrievalFilter", "PopRetrievalFilter",
    "GetRetrievalFilterStackSize", "AddMessage", "AddApplicationMessage", "SetBreakOnCategory", "SetBreakOnSeverity",
    "SetBreakOnID", "GetBreakOnCategory", "GetBreakOnSeverity", "GetBreakOnID", "SetMuteDebugOutput", "GetMuteDebugOutput",
    "RegisterMessageCallback", "UnregisterMessageCallback", NULL };
static StaticObj info_queue, dred, dred_settings;
static pthread_once_t objs_once = PTHREAD_ONCE_INIT;
static const char *const dred_methods[] = { "QueryInterface", "AddRef", "Release", "GetAutoBreadcrumbsOutput",
    "GetPageFaultAllocationOutput", "GetAutoBreadcrumbsOutput1", "GetPageFaultAllocationOutput1", "GetPageFaultAllocationOutput2",
    "GetDeviceState", NULL };
static const char *const dreds_methods[] = { "QueryInterface", "AddRef", "Release", "SetAutoBreadcrumbsEnablement",
    "SetPageFaultEnablement", "SetWatsonDumpEnablement", "SetBreadcrumbContextEnablement", "UseMarkersOnlyAutoBreadcrumbs", NULL };
static void init_objs(void) {
    info_queue.vtbl = static_vtbl("D3D12InfoQueue", iq_methods);
    dred.vtbl = static_vtbl("D3D12Dred", dred_methods);
    dred_settings.vtbl = static_vtbl("D3D12DredSettings", dreds_methods);
}
u64 d3d12_info_queue(void) { pthread_once(&objs_once, init_objs); d3d12_log_once("ID3D12InfoQueue (no-op object)"); return (u64)&info_queue; }
static u64 d3d12_dred(void) { pthread_once(&objs_once, init_objs); return (u64)&dred; }
u64 d3d12_dred_settings(void) { pthread_once(&objs_once, init_objs); d3d12_log_once("DRED settings (accepted, no-op)"); return (u64)&dred_settings; }

#define STATIC_QI(name, test) SHIM(name) { if (!A3) RET(E_POINTER_); if (guid_eq(A2, &iid_unknown) || (test)) { ST64(A3, A1); RET(S_OK); } ST64(A3, 0); RET(E_NOINTERFACE_); }
STATIC_QI(D3D12InfoQueue_QueryInterface, IN_LIST(A2, iid_info_queue))
STATIC_QI(D3D12Dred_QueryInterface, IN_LIST(A2, iid_dred))
STATIC_QI(D3D12DredSettings_QueryInterface, IN_LIST(A2, iid_dred_settings))
SHIM(D3D12InfoQueue_GetMessage) { if (A4) ST64(A4, 0); RET(E_INVALIDARG_); }               // (index, D3D12_MESSAGE *, SIZE_T *len)
SHIM(D3D12InfoQueue_GetFilter) { if (A3) ST64(A3, 0); RET(S_OK); }                         // (filter *, SIZE_T *len): empty filter
SHIM(D3D12InfoQueue_GetMessageCountLimit) { RET(~0ull); }
SHIM(D3D12InfoQueue_RegisterMessageCallback) { if (AN(5)) ST32(AN(5), 1); RET(S_OK); }      // (cb, flags, ctx, DWORD *cookie)
SHIM(D3D12Dred_NotAvailable) { RET(DXGI_ERROR_NOT_CURRENTLY_AVAILABLE_); }                 // breadcrumbs / page faults: DRED is off

// ---- ID3D12Device10..14 ---------------------------------------------------------------------------------
// D3D12_BARRIER_LAYOUT -> legacy initial state.
static D3D12_RESOURCE_STATES layout_state(u32 layout) {
    switch (layout) {
    case 1: case 19: case 25: case 31: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case 2: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case 3: case 20: case 26: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case 4: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case 5: return D3D12_RESOURCE_STATE_DEPTH_READ;
    case 6: case 21: case 27: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case 7: case 22: case 28: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case 8: case 23: case 29: return D3D12_RESOURCE_STATE_COPY_DEST;
    case 9: return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    case 10: return D3D12_RESOURCE_STATE_RESOLVE_DEST;
    case 11: return D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    default: return D3D12_RESOURCE_STATE_COMMON;   // UNDEFINED, COMMON/PRESENT, *_QUEUE_COMMON, video
    }
}
// (This, heap props, heap flags, const D3D12_RESOURCE_DESC1 *, layout, clear, protected session, ncast, cast formats, riid, out)
SHIM(D3D12Device_CreateCommittedResource3) {
    ID3D12Device9 *h = d3d12_host(A1); int bad = 0;
    void *prot = d3d12_in(AN(7), &bad);
    if (bad) RET(E_INVALIDARG_);
    D3D12_TRACE("Device.CreateCommittedResource3");
    if (AN(8)) d3d12_log_once("CreateCommittedResource3: castable formats ignored");
    u32 hr = h->lpVtbl->CreateCommittedResource2(h, (const D3D12_HEAP_PROPERTIES *)A2, (D3D12_HEAP_FLAGS)A3, (const D3D12_RESOURCE_DESC1 *)A4,
                                                 layout_state((u32)AN(5)), (const D3D12_CLEAR_VALUE *)AN(6), prot, (REFIID)AN(10), (void **)AN(11));
    RET(SUCCEEDED(hr) && AN(11) ? d3d12_out(AN(11), hr) : hr);
}
// (This, heap, offset, desc1, layout, clear, ncast, cast formats, riid, out)
SHIM(D3D12Device_CreatePlacedResource2) {
    ID3D12Device9 *h = d3d12_host(A1); int bad = 0;
    void *heap = d3d12_in(A2, &bad);
    if (bad) RET(E_INVALIDARG_);
    D3D12_TRACE("Device.CreatePlacedResource2");
    u32 hr = h->lpVtbl->CreatePlacedResource1(h, heap, A3, (const D3D12_RESOURCE_DESC1 *)A4, layout_state((u32)AN(5)),
                                              (const D3D12_CLEAR_VALUE *)AN(6), (REFIID)AN(9), (void **)AN(10));
    RET(SUCCEEDED(hr) && AN(10) ? d3d12_out(AN(10), hr) : hr);
}
// (This, desc, layout, clear, protected session, ncast, cast formats, riid, out): reserved = tiled resources,
// which MoltenVK can't back (no sparse binding); libvkd3d refuses, as it does for CreateReservedResource.
SHIM(D3D12Device_CreateReservedResource2) {
    ID3D12Device9 *h = d3d12_host(A1);
    D3D12_TRACE("Device.CreateReservedResource2");
    u32 hr = h->lpVtbl->CreateReservedResource(h, (const D3D12_RESOURCE_DESC *)A2, layout_state((u32)A3),
                                               (const D3D12_CLEAR_VALUE *)A4, (REFIID)AN(8), (void **)AN(9));
    RET(SUCCEEDED(hr) && AN(9) ? d3d12_out(AN(9), hr) : hr);
}
void d3d12_create_sampler(ID3D12Device9 *h, const D3D12_SAMPLER_DESC *d, D3D12_CPU_DESCRIPTOR_HANDLE dst);   // d3d12_pso.c
// (This, const D3D12_SAMPLER_DESC2 *, D3D12_CPU_DESCRIPTOR_HANDLE): SAMPLER_DESC plus Flags (UINT border colour,
// non-normalised coordinates).
SHIM(D3D12Device_CreateSampler2) {
    ID3D12Device9 *h = d3d12_host(A1);
    D3D12_SAMPLER_DESC d; memcpy(&d, (const void *)A2, sizeof d);
    u32 flags = LD32(A2 + sizeof d);
    if (flags & 1)   // D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR: 0/1 integer colours become 0.0/1.0
        for (int i = 0; i < 4; i++) { u32 v; memcpy(&v, &d.BorderColor[i], 4); d.BorderColor[i] = v ? 1.0f : 0.0f; }
    if (flags & 2) d3d12_log_once("CreateSampler2: non-normalized coordinates");
    d3d12_create_sampler(h, &d, (D3D12_CPU_DESCRIPTOR_HANDLE){ A3 });
    RET(0);
}
// (This, D3D12_RESOURCE_ALLOCATION_INFO *ret, visible mask, count, const D3D12_RESOURCE_DESC1 *, ncast[], cast[][], info1[])
SHIM(D3D12Device_GetResourceAllocationInfo3) {
    ID3D12Device9 *h = d3d12_host(A1);
    D3D12_TRACE("Device.GetResourceAllocationInfo3");
    RET((u64)h->lpVtbl->GetResourceAllocationInfo2(h, (D3D12_RESOURCE_ALLOCATION_INFO *)A2, (UINT)A3, (UINT)A4,
                                                    (const D3D12_RESOURCE_DESC1 *)AN(5), (D3D12_RESOURCE_ALLOCATION_INFO1 *)AN(8)));
}
SHIM(D3D12Device_OpenExistingHeapFromAddress1) {   // (This, address, size, riid, out)
    if (AN(5)) ST64(AN(5), 0);
    d3d12_log_once("ID3D12Device13::OpenExistingHeapFromAddress1");
    RET(E_NOTIMPL_);
}
SHIM(D3D12Device_CreateRootSignatureFromSubobjectInLibrary) {   // (This, node mask, blob, size, name, riid, out)
    if (AN(7)) ST64(AN(7), 0);
    d3d12_log_once("ID3D12Device14::CreateRootSignatureFromSubobjectInLibrary (state object libraries)");
    RET(E_NOTIMPL_);
}
// ID3D12Device9::CreateCommandQueue1(This, desc, creator id, riid, out): libvkd3d stubs it; the creator ID only
// groups queues for scheduling, so it is a plain CreateCommandQueue.
SHIM(D3D12Device_CreateCommandQueue1) {
    ID3D12Device9 *h = d3d12_host(A1);
    D3D12_TRACE("Device.CreateCommandQueue1");
    u32 hr = h->lpVtbl->CreateCommandQueue(h, (const D3D12_COMMAND_QUEUE_DESC *)A2, (REFIID)A4, (void **)AN(5));
    RET(SUCCEEDED(hr) && AN(5) ? d3d12_out(AN(5), hr) : hr);
}

// ---- ID3D12GraphicsCommandList7..10 ---------------------------------------------------------------------
// D3D12_BARRIER_ACCESS bits -> legacy states (for buffers and access-only texture barriers).
static D3D12_RESOURCE_STATES access_state(u32 a) {
    if (a == 0x80000000u || !a) return D3D12_RESOURCE_STATE_COMMON;   // NO_ACCESS / COMMON
    D3D12_RESOURCE_STATES s = 0;
    if (a & 0x3) s |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if (a & 0x4) s |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    if (a & 0x8) s |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (a & 0x10) s |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (a & 0x20) s |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (a & 0x40) s |= D3D12_RESOURCE_STATE_DEPTH_READ;
    if (a & 0x80) s |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (a & 0x100) s |= D3D12_RESOURCE_STATE_STREAM_OUT;
    if (a & 0x200) s |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (a & 0x400) s |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (a & 0x800) s |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (a & 0x1000) s |= D3D12_RESOURCE_STATE_RESOLVE_DEST;
    if (a & 0x2000) s |= D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    if (s & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE |
             D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_STREAM_OUT))
        s &= D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE |
             D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_STREAM_OUT;   // write states are exclusive
    return s;
}
typedef struct { u32 sync_before, sync_after, access_before, access_after; } GlobalBarrier;
typedef struct { u32 sync_before, sync_after, access_before, access_after, layout_before, layout_after; u64 res;
                 u32 mip, nmips, slice, nslices, plane, nplanes, flags; } TextureBarrier;
typedef struct { u32 sync_before, sync_after, access_before, access_after; u64 res, offset, size; } BufferBarrier;
_Static_assert(sizeof(TextureBarrier) == 64 && sizeof(BufferBarrier) == 40, "barrier layouts");
// Barrier(This, UINT32 groups, const D3D12_BARRIER_GROUP *): { type, count, pointer } groups.
SHIM(D3D12List_Barrier) {
    ID3D12GraphicsCommandList6 *h = d3d12_host(A1); u32 ng = (u32)A2;
    D3D12_TRACE("List.Barrier");
    d3d12_log_once("ID3D12GraphicsCommandList7::Barrier (enhanced barriers, translated to legacy barriers)");
    for (u32 g = 0; g < ng; g++) {
        u64 grp = A3 + 16ull * g; u32 type = LD32(grp), n = LD32(grp + 4); u64 p = LD64(grp + 8);
        D3D12_RESOURCE_BARRIER *b = alloca(sizeof *b * (n + 1)); u32 nb = 0; int bad = 0;
        for (u32 i = 0; i < n; i++) {
            D3D12_RESOURCE_BARRIER *o = &b[nb]; memset(o, 0, sizeof *o);
            if (type == 0) {                                  // global: a UAV barrier on everything
                o->Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; nb++;
            } else if (type == 1) {                           // texture: layout transition
                const TextureBarrier *t = (const TextureBarrier *)p + i;
                void *r = d3d12_in(t->res, &bad);
                D3D12_RESOURCE_STATES before = layout_state(t->layout_before), after = layout_state(t->layout_after);
                if (t->layout_before == t->layout_after) { before = access_state(t->access_before); after = access_state(t->access_after); }
                if (before == after) {
                    if ((t->access_before | t->access_after) & 0x10) { o->Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; o->UAV.pResource = r; nb++; }
                    continue;
                }
                o->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; o->Transition.pResource = r;
                // NumMipLevels == 0: IndexOrFirstMipLevel is a subresource index (0xffffffff = all); ranges become "all"
                o->Transition.Subresource = t->nmips == 0 ? t->mip : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                o->Transition.StateBefore = before; o->Transition.StateAfter = after; nb++;
            } else if (type == 2) {                           // buffer: access transition
                const BufferBarrier *bb = (const BufferBarrier *)p + i;
                void *r = d3d12_in(bb->res, &bad);
                D3D12_RESOURCE_STATES before = access_state(bb->access_before), after = access_state(bb->access_after);
                if (before == after) { o->Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; o->UAV.pResource = r; nb++; continue; }
                o->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; o->Transition.pResource = r;
                o->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                o->Transition.StateBefore = before; o->Transition.StateAfter = after; nb++;
            }
        }
        if (!bad && nb) h->lpVtbl->ResourceBarrier(h, nb, b);
    }
    RET(0);
}
SHIM(D3D12List_OMSetFrontAndBackStencilRef) {   // (This, front, back): one reference for both faces
    ID3D12GraphicsCommandList6 *h = d3d12_host(A1);
    if ((u32)A2 != (u32)A3) d3d12_log_once("OMSetFrontAndBackStencilRef: separate back-face reference (front used)");
    h->lpVtbl->OMSetStencilRef(h, (UINT)A2);
    RET(0);
}
SHIM(D3D12List_RSSetDepthBias) { d3d12_log_once("ID3D12GraphicsCommandList9::RSSetDepthBias (OPTIONS16 reports it unsupported)"); RET(0); }
SHIM(D3D12List_IASetIndexBufferStripCutValue) { d3d12_log_once("ID3D12GraphicsCommandList9::IASetIndexBufferStripCutValue"); RET(0); }
SHIM(D3D12List_SetProgram) { d3d12_log_once("ID3D12GraphicsCommandList10::SetProgram (work graphs)"); RET(0); }
SHIM(D3D12List_DispatchGraph) { d3d12_log_once("ID3D12GraphicsCommandList10::DispatchGraph (work graphs)"); RET(0); }
SHIM(D3D12Heap_GetProtectedResourceSession) { if (A3) ST64(A3, 0); RET(E_NOINTERFACE_); }   // (This, riid, out)

// ---- dstorage.dll: absent. Titles that ship it check the HRESULT and fall back to plain file I/O. -------------
SHIM(DStorageGetFactory) { if (A2) ST64(A2, 0); d3d12_log_once("DirectStorage (DStorageGetFactory): not available"); RET(E_NOTIMPL_); }
SHIM(DStorageSetConfiguration) { RET(S_OK); }
SHIM(DStorageSetConfiguration1) { RET(S_OK); }
SHIM(DStorageCreateCompressionCodec) { if (A4) ST64(A4, 0); RET(E_NOTIMPL_); }

const Shim shims_d3d12_ext[] = {
#define M(n) { NULL, "__wintomac_" #n, sh_##n }
#define A(n, f) { NULL, "__wintomac_" #n, sh_##f }
    M(D3D12Ext_Zero), M(D3D12Ext_One),
    M(D3D12InfoQueue_QueryInterface), A(D3D12InfoQueue_AddRef, D3D12Ext_One), A(D3D12InfoQueue_Release, D3D12Ext_One),
    M(D3D12InfoQueue_GetMessage), A(D3D12InfoQueue_GetStorageFilter, D3D12InfoQueue_GetFilter),
    A(D3D12InfoQueue_GetRetrievalFilter, D3D12InfoQueue_GetFilter), M(D3D12InfoQueue_GetMessageCountLimit),
    M(D3D12InfoQueue_RegisterMessageCallback),
    M(D3D12Dred_QueryInterface), A(D3D12Dred_AddRef, D3D12Ext_One), A(D3D12Dred_Release, D3D12Ext_One),
    A(D3D12Dred_GetAutoBreadcrumbsOutput, D3D12Dred_NotAvailable), A(D3D12Dred_GetPageFaultAllocationOutput, D3D12Dred_NotAvailable),
    A(D3D12Dred_GetAutoBreadcrumbsOutput1, D3D12Dred_NotAvailable), A(D3D12Dred_GetPageFaultAllocationOutput1, D3D12Dred_NotAvailable),
    A(D3D12Dred_GetPageFaultAllocationOutput2, D3D12Dred_NotAvailable),
    M(D3D12DredSettings_QueryInterface), A(D3D12DredSettings_AddRef, D3D12Ext_One), A(D3D12DredSettings_Release, D3D12Ext_One),
    M(D3D12Device_CreateCommittedResource3), M(D3D12Device_CreatePlacedResource2), M(D3D12Device_CreateReservedResource2),
    M(D3D12Device_CreateSampler2), M(D3D12Device_GetResourceAllocationInfo3), M(D3D12Device_OpenExistingHeapFromAddress1),
    M(D3D12Device_CreateRootSignatureFromSubobjectInLibrary), M(D3D12Device_CreateCommandQueue1),
    M(D3D12List_Barrier), M(D3D12List_OMSetFrontAndBackStencilRef), M(D3D12List_RSSetDepthBias),
    M(D3D12List_IASetIndexBufferStripCutValue), M(D3D12List_SetProgram), M(D3D12List_DispatchGraph),
    M(D3D12Heap_GetProtectedResourceSession),
    { NULL, "DStorageGetFactory", sh_DStorageGetFactory }, { NULL, "DStorageSetConfiguration", sh_DStorageSetConfiguration },
    { NULL, "DStorageSetConfiguration1", sh_DStorageSetConfiguration1 }, { NULL, "DStorageCreateCompressionCodec", sh_DStorageCreateCompressionCodec },
#undef M
#undef A
    { 0, 0, 0 }
};
