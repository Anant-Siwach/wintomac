// d3d12.dll on Metal directly (WINTOMAC_D3D12_BACKEND=metal): a feasibility prototype of our own D3DMetal-style
// layer, next to the default libvkd3d -> MoltenVK path. Design, binding model and plan: docs/d3d12_metal.md.
//
// The objects here are plain C COM objects with the vtables libvkd3d's headers declare (ID3D12Device9,
// ID3D12GraphicsCommandList6, ...), so runtime/d3d12.c wraps them for the guest exactly like libvkd3d's objects:
// the generated Win64 thunks (tools/gen_d3d12.py), the hand-written shims and the GS/breadth workarounds all work
// unchanged. Methods the prototype doesn't implement log their name once and return E_NOTIMPL / do nothing.
//
// Implemented: device, command queue/allocator/list, committed buffers and textures (default/upload/readback),
// descriptor heaps (RTV/DSV on the CPU; CBV_SRV_UAV and samplers as Metal Tier 2 argument buffers of 24-byte entries,
// see d3d12mtl.h), root signatures, graphics/compute PSOs (shaders through d3d12mtl_shader.cpp), barriers (no-ops:
// Metal tracks hazards of these resources), fences (MTLSharedEvent), CopyTextureRegion/CopyBufferRegion/CopyResource,
// ClearRenderTargetView/ClearDepthStencilView, DrawInstanced/DrawIndexedInstanced/Dispatch.
//
// Command lists record into a small command stream; ExecuteCommandLists replays the streams into one
// MTLCommandBuffer (render passes are formed lazily at the first draw after a render-target change, clears become
// load actions). Built without ARC: Metal objects are retained/released by hand.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <mach/mach_time.h>
#define BOOL WINBOOL_   // vkd3d_windows.h's BOOL (int) vs Objective-C's BOOL (bool)
#include "d3d12_internal.h"
#undef BOOL
#include "d3d12mtl.h"

#define S_FALSE_ 1
#define E_OUTOFMEMORY_ 0x8007000Eu
#define E_FAIL_ 0x80004005u

extern const GUID IID_IUnknown, IID_ID3D12Object, IID_ID3D12DeviceChild, IID_ID3D12Pageable, IID_ID3D12CommandList;
const char *d3d12_method_name(const char *kind, int slot);   // d3d12.c (generated method tables)

enum { K_DEVICE, K_QUEUE, K_ALLOC, K_LIST, K_FENCE, K_RESOURCE, K_DESCHEAP, K_ROOTSIG, K_PSO, K_NKINDS };
static const char *const kind_names[K_NKINDS] = { "Device", "Queue", "Alloc", "List", "Fence", "Resource", "DescHeap", "RootSig", "PSO" };

typedef struct MDevice MDevice;
typedef struct { const void *vtbl; int kind; atomic_int refs; MDevice *dev; } MHead;

static void mtl_log_once(const char *what) { d3d12_log_once(what); }

// ---- unimplemented methods: every vtable slot nobody filled logs its name once ----------------------------------
#define STUB(n) static u64 stub_##n(MHead *o) { char m[160]; snprintf(m, sizeof m, "metal backend: ID3D12 %s::%s", \
    kind_names[o->kind], d3d12_method_name(kind_names[o->kind], n)); mtl_log_once(m); return E_NOTIMPL_; }
#define STUB10(a) STUB(a##0) STUB(a##1) STUB(a##2) STUB(a##3) STUB(a##4) STUB(a##5) STUB(a##6) STUB(a##7) STUB(a##8) STUB(a##9)
STUB10() STUB10(1) STUB10(2) STUB10(3) STUB10(4) STUB10(5) STUB10(6) STUB10(7) STUB10(8) STUB10(9)
#define SREF(a) stub_##a##0, stub_##a##1, stub_##a##2, stub_##a##3, stub_##a##4, stub_##a##5, stub_##a##6, stub_##a##7, stub_##a##8, stub_##a##9
static void *const stubs[100] = { stub_0, stub_1, stub_2, stub_3, stub_4, stub_5, stub_6, stub_7, stub_8, stub_9,
    SREF(1), SREF(2), SREF(3), SREF(4), SREF(5), SREF(6), SREF(7), SREF(8), SREF(9) };
static void fill_stubs(void *vtbl, size_t size) {
    void **v = vtbl;
    for (size_t i = 0; i < size / sizeof(void *) && i < 100; i++) if (!v[i]) v[i] = stubs[i];
}

// ---- formats ---------------------------------------------------------------------------------------------------
typedef struct { DXGI_FORMAT dxgi; MTLPixelFormat mtl; u8 bytes, block; MTLVertexFormat vtx; } Fmt;
static const Fmt fmts[] = {
    { DXGI_FORMAT_R32G32B32A32_TYPELESS, MTLPixelFormatRGBA32Float, 16, 1, MTLVertexFormatFloat4 },
    { DXGI_FORMAT_R32G32B32A32_FLOAT, MTLPixelFormatRGBA32Float, 16, 1, MTLVertexFormatFloat4 },
    { DXGI_FORMAT_R32G32B32A32_UINT, MTLPixelFormatRGBA32Uint, 16, 1, MTLVertexFormatUInt4 },
    { DXGI_FORMAT_R32G32B32A32_SINT, MTLPixelFormatRGBA32Sint, 16, 1, MTLVertexFormatInt4 },
    { DXGI_FORMAT_R32G32B32_FLOAT, MTLPixelFormatInvalid, 12, 1, MTLVertexFormatFloat3 },
    { DXGI_FORMAT_R32G32B32_UINT, MTLPixelFormatInvalid, 12, 1, MTLVertexFormatUInt3 },
    { DXGI_FORMAT_R16G16B16A16_TYPELESS, MTLPixelFormatRGBA16Float, 8, 1, MTLVertexFormatHalf4 },
    { DXGI_FORMAT_R16G16B16A16_FLOAT, MTLPixelFormatRGBA16Float, 8, 1, MTLVertexFormatHalf4 },
    { DXGI_FORMAT_R16G16B16A16_UNORM, MTLPixelFormatRGBA16Unorm, 8, 1, MTLVertexFormatUShort4Normalized },
    { DXGI_FORMAT_R16G16B16A16_UINT, MTLPixelFormatRGBA16Uint, 8, 1, MTLVertexFormatUShort4 },
    { DXGI_FORMAT_R16G16B16A16_SNORM, MTLPixelFormatRGBA16Snorm, 8, 1, MTLVertexFormatShort4Normalized },
    { DXGI_FORMAT_R16G16B16A16_SINT, MTLPixelFormatRGBA16Sint, 8, 1, MTLVertexFormatShort4 },
    { DXGI_FORMAT_R32G32_TYPELESS, MTLPixelFormatRG32Float, 8, 1, MTLVertexFormatFloat2 },
    { DXGI_FORMAT_R32G32_FLOAT, MTLPixelFormatRG32Float, 8, 1, MTLVertexFormatFloat2 },
    { DXGI_FORMAT_R32G32_UINT, MTLPixelFormatRG32Uint, 8, 1, MTLVertexFormatUInt2 },
    { DXGI_FORMAT_R32G32_SINT, MTLPixelFormatRG32Sint, 8, 1, MTLVertexFormatInt2 },
    { DXGI_FORMAT_R10G10B10A2_TYPELESS, MTLPixelFormatRGB10A2Unorm, 4, 1, MTLVertexFormatUInt1010102Normalized },
    { DXGI_FORMAT_R10G10B10A2_UNORM, MTLPixelFormatRGB10A2Unorm, 4, 1, MTLVertexFormatUInt1010102Normalized },
    { DXGI_FORMAT_R10G10B10A2_UINT, MTLPixelFormatRGB10A2Uint, 4, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_R11G11B10_FLOAT, MTLPixelFormatRG11B10Float, 4, 1, MTLVertexFormatFloatRG11B10 },
    { DXGI_FORMAT_R8G8B8A8_TYPELESS, MTLPixelFormatRGBA8Unorm, 4, 1, MTLVertexFormatUChar4Normalized },
    { DXGI_FORMAT_R8G8B8A8_UNORM, MTLPixelFormatRGBA8Unorm, 4, 1, MTLVertexFormatUChar4Normalized },
    { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, MTLPixelFormatRGBA8Unorm_sRGB, 4, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_R8G8B8A8_UINT, MTLPixelFormatRGBA8Uint, 4, 1, MTLVertexFormatUChar4 },
    { DXGI_FORMAT_R8G8B8A8_SNORM, MTLPixelFormatRGBA8Snorm, 4, 1, MTLVertexFormatChar4Normalized },
    { DXGI_FORMAT_R8G8B8A8_SINT, MTLPixelFormatRGBA8Sint, 4, 1, MTLVertexFormatChar4 },
    { DXGI_FORMAT_R16G16_TYPELESS, MTLPixelFormatRG16Float, 4, 1, MTLVertexFormatHalf2 },
    { DXGI_FORMAT_R16G16_FLOAT, MTLPixelFormatRG16Float, 4, 1, MTLVertexFormatHalf2 },
    { DXGI_FORMAT_R16G16_UNORM, MTLPixelFormatRG16Unorm, 4, 1, MTLVertexFormatUShort2Normalized },
    { DXGI_FORMAT_R16G16_UINT, MTLPixelFormatRG16Uint, 4, 1, MTLVertexFormatUShort2 },
    { DXGI_FORMAT_R16G16_SNORM, MTLPixelFormatRG16Snorm, 4, 1, MTLVertexFormatShort2Normalized },
    { DXGI_FORMAT_R16G16_SINT, MTLPixelFormatRG16Sint, 4, 1, MTLVertexFormatShort2 },
    { DXGI_FORMAT_R32_TYPELESS, MTLPixelFormatR32Float, 4, 1, MTLVertexFormatFloat },
    { DXGI_FORMAT_D32_FLOAT, MTLPixelFormatDepth32Float, 4, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_R32_FLOAT, MTLPixelFormatR32Float, 4, 1, MTLVertexFormatFloat },
    { DXGI_FORMAT_R32_UINT, MTLPixelFormatR32Uint, 4, 1, MTLVertexFormatUInt },
    { DXGI_FORMAT_R32_SINT, MTLPixelFormatR32Sint, 4, 1, MTLVertexFormatInt },
    // no 24-bit depth on Apple GPUs: D24S8 is stored as D32S8 (copies to/from buffers would need repacking)
    { DXGI_FORMAT_R24G8_TYPELESS, MTLPixelFormatDepth32Float_Stencil8, 4, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_D24_UNORM_S8_UINT, MTLPixelFormatDepth32Float_Stencil8, 4, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_R32G8X24_TYPELESS, MTLPixelFormatDepth32Float_Stencil8, 8, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_D32_FLOAT_S8X24_UINT, MTLPixelFormatDepth32Float_Stencil8, 8, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_R8G8_TYPELESS, MTLPixelFormatRG8Unorm, 2, 1, MTLVertexFormatUChar2Normalized },
    { DXGI_FORMAT_R8G8_UNORM, MTLPixelFormatRG8Unorm, 2, 1, MTLVertexFormatUChar2Normalized },
    { DXGI_FORMAT_R8G8_UINT, MTLPixelFormatRG8Uint, 2, 1, MTLVertexFormatUChar2 },
    { DXGI_FORMAT_R16_TYPELESS, MTLPixelFormatR16Float, 2, 1, MTLVertexFormatHalf },
    { DXGI_FORMAT_R16_FLOAT, MTLPixelFormatR16Float, 2, 1, MTLVertexFormatHalf },
    { DXGI_FORMAT_D16_UNORM, MTLPixelFormatDepth16Unorm, 2, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_R16_UNORM, MTLPixelFormatR16Unorm, 2, 1, MTLVertexFormatUShortNormalized },
    { DXGI_FORMAT_R16_UINT, MTLPixelFormatR16Uint, 2, 1, MTLVertexFormatUShort },
    { DXGI_FORMAT_R16_SINT, MTLPixelFormatR16Sint, 2, 1, MTLVertexFormatShort },
    { DXGI_FORMAT_R8_TYPELESS, MTLPixelFormatR8Unorm, 1, 1, MTLVertexFormatUCharNormalized },
    { DXGI_FORMAT_R8_UNORM, MTLPixelFormatR8Unorm, 1, 1, MTLVertexFormatUCharNormalized },
    { DXGI_FORMAT_R8_UINT, MTLPixelFormatR8Uint, 1, 1, MTLVertexFormatUChar },
    { DXGI_FORMAT_A8_UNORM, MTLPixelFormatA8Unorm, 1, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_B8G8R8A8_TYPELESS, MTLPixelFormatBGRA8Unorm, 4, 1, MTLVertexFormatUChar4Normalized_BGRA },
    { DXGI_FORMAT_B8G8R8A8_UNORM, MTLPixelFormatBGRA8Unorm, 4, 1, MTLVertexFormatUChar4Normalized_BGRA },
    { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, MTLPixelFormatBGRA8Unorm_sRGB, 4, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_B8G8R8X8_UNORM, MTLPixelFormatBGRA8Unorm, 4, 1, MTLVertexFormatInvalid },
    { DXGI_FORMAT_BC1_TYPELESS, MTLPixelFormatBC1_RGBA, 8, 4, 0 }, { DXGI_FORMAT_BC1_UNORM, MTLPixelFormatBC1_RGBA, 8, 4, 0 },
    { DXGI_FORMAT_BC1_UNORM_SRGB, MTLPixelFormatBC1_RGBA_sRGB, 8, 4, 0 },
    { DXGI_FORMAT_BC2_UNORM, MTLPixelFormatBC2_RGBA, 16, 4, 0 }, { DXGI_FORMAT_BC3_UNORM, MTLPixelFormatBC3_RGBA, 16, 4, 0 },
    { DXGI_FORMAT_BC3_UNORM_SRGB, MTLPixelFormatBC3_RGBA_sRGB, 16, 4, 0 },
    { DXGI_FORMAT_BC4_UNORM, MTLPixelFormatBC4_RUnorm, 8, 4, 0 }, { DXGI_FORMAT_BC5_UNORM, MTLPixelFormatBC5_RGUnorm, 16, 4, 0 },
    { DXGI_FORMAT_BC6H_UF16, MTLPixelFormatBC6H_RGBUfloat, 16, 4, 0 }, { DXGI_FORMAT_BC7_UNORM, MTLPixelFormatBC7_RGBAUnorm, 16, 4, 0 },
    { DXGI_FORMAT_BC7_UNORM_SRGB, MTLPixelFormatBC7_RGBAUnorm_sRGB, 16, 4, 0 },
};
static const Fmt *fmt(DXGI_FORMAT f) {
    for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++) if (fmts[i].dxgi == f) return &fmts[i];
    return NULL;
}
static MTLPixelFormat pixfmt(DXGI_FORMAT f) { const Fmt *x = fmt(f); return x ? x->mtl : MTLPixelFormatInvalid; }
static int is_depth(MTLPixelFormat f) {
    return f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float_Stencil8;
}
static int has_stencil(MTLPixelFormat f) { return f == MTLPixelFormatDepth32Float_Stencil8; }
// SRV formats of depth resources (R24_UNORM_X8_TYPELESS, R32_FLOAT_X8X24_TYPELESS, ...) read the depth plane.
static MTLPixelFormat view_fmt(DXGI_FORMAT f, MTLPixelFormat res) {
    if (is_depth(res)) return res;
    MTLPixelFormat p = pixfmt(f);
    return p ? p : res;
}

// ---- objects ----------------------------------------------------------------------------------------------------
typedef struct MResource MResource;
typedef struct MHeap MHeap;
struct MDevice {
    const ID3D12Device9Vtbl *vtbl; int kind; atomic_int refs; MDevice *dev;
    id<MTLDevice> mtl;
    id<MTLResidencySet> residency; int residency_dirty;
    MTLSharedEventListener *listener;
    pthread_mutex_t lock;
    MResource **res; size_t nres, capres;   // buffers sorted by GPU address (root descriptors, vertex/index buffers)
    MHeap **heaps; size_t nheaps, capheaps; // descriptor heaps (CPU/GPU handle -> heap)
    D3D_FEATURE_LEVEL level;
};
struct MResource {
    const ID3D12Resource2Vtbl *vtbl; int kind; atomic_int refs; MDevice *dev;
    D3D12_RESOURCE_DESC desc; D3D12_HEAP_PROPERTIES hp; D3D12_HEAP_FLAGS hflags;
    id<MTLBuffer> buf; id<MTLTexture> tex; u64 va;
    MTLPixelFormat pf;
};
typedef struct {   // RTV / DSV heap entry (CPU only), copied into command streams at record time
    MResource *res; id<MTLTexture> tex; u32 level, slice; MTLPixelFormat pf; u32 valid;
} MRtv;
struct MHeap {
    const ID3D12DescriptorHeapVtbl *vtbl; int kind; atomic_int refs; MDevice *dev;
    D3D12_DESCRIPTOR_HEAP_DESC desc; u32 stride;
    u8 *cpu; id<MTLBuffer> buf; u64 gpu;
    id *objs;   // per descriptor: retained view texture / sampler state (keeps MTLResourceIDs valid)
};
typedef struct {
    const ID3D12RootSignatureVtbl *vtbl; int kind; atomic_int refs; MDevice *dev;
    struct vkd3d_shader_versioned_root_signature_desc desc;   // version 1.1
    D3D12MtlRootLayout layout;
} MRootSig;
typedef struct {
    const ID3D12PipelineStateVtbl *vtbl; int kind; atomic_int refs; MDevice *dev;
    MRootSig *rs; int compute;
    id<MTLRenderPipelineState> rps; id<MTLDepthStencilState> dss; id<MTLComputePipelineState> cps;
    MTLCullMode cull; MTLWinding winding; MTLTriangleFillMode fill; MTLDepthClipMode clip;
    float bias, slope, clamp; MTLSize tg;
    u32 vs_stages;   // which stages read root parameters (bit 0 vertex, bit 1 fragment)
} MPso;
typedef struct {
    const ID3D12FenceVtbl *vtbl; int kind; atomic_int refs; MDevice *dev;
    id<MTLSharedEvent> ev; D3D12_FENCE_FLAGS flags;
} MFence;
typedef struct {
    const ID3D12CommandAllocatorVtbl *vtbl; int kind; atomic_int refs; MDevice *dev;
    D3D12_COMMAND_LIST_TYPE type;
} MAlloc;
typedef struct {
    const ID3D12CommandQueueVtbl *vtbl; int kind; atomic_int refs; MDevice *dev;
    D3D12_COMMAND_QUEUE_DESC desc; id<MTLCommandQueue> q;
    pthread_mutex_t lock;
} MQueue;
typedef struct {
    const ID3D12GraphicsCommandList6Vtbl *vtbl; int kind; atomic_int refs; MDevice *dev;
    D3D12_COMMAND_LIST_TYPE type; int open;
    u8 *cmd; size_t ncmd, capcmd;   // recorded command stream
    MRootSig *grs, *crs;            // record-time root signatures (constants need parameter offsets)
} MList;

static void dev_addref(MDevice *d) { atomic_fetch_add(&d->refs, 1); }
static ULONG dev_release(MDevice *d);
static void head_init(void *o, const void *vtbl, int kind, MDevice *d) {
    MHead *h = o; h->vtbl = vtbl; h->kind = kind; atomic_store(&h->refs, 1); h->dev = d;
    if (d && (void *)d != o) dev_addref(d);
}

// QueryInterface: the generic interfaces plus the kind's own family.
static int iid_eq(REFIID a, const GUID *b) { return !memcmp(a, b, sizeof *b); }
static const GUID *const *kind_iids(int kind);
static HRESULT STDMETHODCALLTYPE obj_qi(void *self, REFIID riid, void **out) {
    MHead *o = self;
    if (!out) return E_POINTER_;
    const GUID *const *l = kind_iids(o->kind);
    int ok = iid_eq(riid, &IID_IUnknown) || iid_eq(riid, &IID_ID3D12Object) || (o->kind != K_DEVICE && iid_eq(riid, &IID_ID3D12DeviceChild));
    for (int i = 0; !ok && l[i]; i++) ok = iid_eq(riid, l[i]);
    if (!ok) { *out = NULL; return E_NOINTERFACE_; }
    atomic_fetch_add(&o->refs, 1); *out = o; return S_OK;
}
static ULONG STDMETHODCALLTYPE obj_addref(void *self) { return atomic_fetch_add(&((MHead *)self)->refs, 1) + 1; }
static void destroy(MHead *o);
static ULONG STDMETHODCALLTYPE obj_release(void *self) {
    MHead *o = self;
    ULONG r = atomic_fetch_sub(&o->refs, 1) - 1;
    if (!r) { MDevice *d = o->dev; destroy(o); if (d && (void *)d != self) dev_release(d); }
    return r;
}
static HRESULT STDMETHODCALLTYPE obj_get_private(void *self, REFGUID g, UINT *size, void *data) { if (size) *size = 0; return 0x887A0002u; /* DXGI_ERROR_NOT_FOUND */ }
static HRESULT STDMETHODCALLTYPE obj_set_private(void *self, REFGUID g, UINT size, const void *data) { return S_OK; }
static HRESULT STDMETHODCALLTYPE obj_set_private_if(void *self, REFGUID g, const IUnknown *data) { return S_OK; }
static HRESULT STDMETHODCALLTYPE obj_set_name(void *self, const WCHAR *name) { return S_OK; }
static HRESULT STDMETHODCALLTYPE obj_get_device(void *self, REFIID riid, void **out) {
    MDevice *d = ((MHead *)self)->dev;
    return d->vtbl->QueryInterface((ID3D12Device9 *)d, riid, out);
}
#define OBJ_METHODS .QueryInterface = (void *)obj_qi, .AddRef = (void *)obj_addref, .Release = (void *)obj_release, \
    .GetPrivateData = (void *)obj_get_private, .SetPrivateData = (void *)obj_set_private, \
    .SetPrivateDataInterface = (void *)obj_set_private_if, .SetName = (void *)obj_set_name
#define CHILD_METHODS OBJ_METHODS, .GetDevice = (void *)obj_get_device

// ---- device: resource and heap registries --------------------------------------------------------------------
static void reg_resource(MDevice *d, MResource *r) {
    pthread_mutex_lock(&d->lock);
    if (r->buf) {
        if (d->nres == d->capres) { d->capres = d->capres ? 2 * d->capres : 64; d->res = realloc(d->res, d->capres * sizeof *d->res); }
        size_t i = d->nres;
        while (i && d->res[i - 1]->va > r->va) { d->res[i] = d->res[i - 1]; i--; }
        d->res[i] = r; d->nres++;
    }
    id<MTLAllocation> a = r->buf ? (id<MTLAllocation>)r->buf : (id<MTLAllocation>)r->tex;
    if (a) { [d->residency addAllocation:a]; d->residency_dirty = 1; }
    pthread_mutex_unlock(&d->lock);
}
static void unreg_resource(MDevice *d, MResource *r) {
    pthread_mutex_lock(&d->lock);
    for (size_t i = 0; i < d->nres; i++)
        if (d->res[i] == r) { memmove(&d->res[i], &d->res[i + 1], (d->nres - i - 1) * sizeof *d->res); d->nres--; break; }
    id<MTLAllocation> a = r->buf ? (id<MTLAllocation>)r->buf : (id<MTLAllocation>)r->tex;
    if (a) { [d->residency removeAllocation:a]; d->residency_dirty = 1; }
    pthread_mutex_unlock(&d->lock);
}
static MResource *find_va(MDevice *d, u64 va) {
    MResource *found = NULL;
    pthread_mutex_lock(&d->lock);
    size_t lo = 0, hi = d->nres;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (d->res[m]->va <= va) lo = m + 1; else hi = m; }
    if (lo && va < d->res[lo - 1]->va + d->res[lo - 1]->desc.Width) found = d->res[lo - 1];
    pthread_mutex_unlock(&d->lock);
    return found;
}
static void reg_heap(MDevice *d, MHeap *h) {
    pthread_mutex_lock(&d->lock);
    if (d->nheaps == d->capheaps) { d->capheaps = d->capheaps ? 2 * d->capheaps : 16; d->heaps = realloc(d->heaps, d->capheaps * sizeof *d->heaps); }
    d->heaps[d->nheaps++] = h;
    if (h->buf) { [d->residency addAllocation:h->buf]; d->residency_dirty = 1; }
    pthread_mutex_unlock(&d->lock);
}
static void unreg_heap(MDevice *d, MHeap *h) {
    pthread_mutex_lock(&d->lock);
    for (size_t i = 0; i < d->nheaps; i++)
        if (d->heaps[i] == h) { d->heaps[i] = d->heaps[--d->nheaps]; break; }
    if (h->buf) { [d->residency removeAllocation:h->buf]; d->residency_dirty = 1; }
    pthread_mutex_unlock(&d->lock);
}
// heap + descriptor index for a CPU handle (gpu = 0) or a GPU handle (gpu = 1)
static MHeap *find_heap(MDevice *d, u64 p, int gpu, u32 *index) {
    MHeap *found = NULL;
    pthread_mutex_lock(&d->lock);
    for (size_t i = 0; i < d->nheaps; i++) {
        MHeap *h = d->heaps[i];
        u64 base = gpu ? h->gpu : (u64)h->cpu, size = (u64)h->desc.NumDescriptors * h->stride;
        if (base && p >= base && p < base + size) { found = h; if (index) *index = (u32)((p - base) / h->stride); break; }
    }
    pthread_mutex_unlock(&d->lock);
    return found;
}

// ---- resources --------------------------------------------------------------------------------------------------
static u32 mip_dim(u64 d, u32 level) { u64 v = d >> level; return v ? (u32)v : 1; }
static u32 sub_count(const D3D12_RESOURCE_DESC *d) {
    u32 mips = d->MipLevels ? d->MipLevels : 1;
    return d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? 1 : mips * (d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d->DepthOrArraySize);
}
static u32 max_mips(const D3D12_RESOURCE_DESC *d) {
    u64 m = d->Width > d->Height ? d->Width : d->Height; u32 n = 1;
    while (m > 1) { m >>= 1; n++; }
    return n;
}
static HRESULT STDMETHODCALLTYPE res_map(ID3D12Resource2 *self, UINT sub, const D3D12_RANGE *range, void **data) {
    MResource *r = (MResource *)self;
    if (!r->buf || r->buf.storageMode == MTLStorageModePrivate) { if (data) *data = NULL; return E_INVALIDARG_; }
    if (data) *data = r->buf.contents;
    return S_OK;
}
static void STDMETHODCALLTYPE res_unmap(ID3D12Resource2 *self, UINT sub, const D3D12_RANGE *range) {}
static D3D12_RESOURCE_DESC *STDMETHODCALLTYPE res_get_desc(ID3D12Resource2 *self, D3D12_RESOURCE_DESC *ret) { *ret = ((MResource *)self)->desc; return ret; }
static D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE res_va(ID3D12Resource2 *self) { return ((MResource *)self)->va; }
static HRESULT STDMETHODCALLTYPE res_heap_props(ID3D12Resource2 *self, D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS *f) {
    MResource *r = (MResource *)self;
    if (hp) *hp = r->hp;
    if (f) *f = r->hflags;
    return S_OK;
}
static ID3D12Resource2Vtbl res_vtbl = { CHILD_METHODS, .Map = res_map, .Unmap = res_unmap, .GetDesc = res_get_desc,
    .GetGPUVirtualAddress = res_va, .GetHeapProperties = res_heap_props };

static HRESULT create_resource(MDevice *d, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC *desc,
                               REFIID riid, void **out) {
    if (!hp || !desc) return E_INVALIDARG_;
    MResource *r = calloc(1, sizeof *r);
    head_init(r, &res_vtbl, K_RESOURCE, d);
    r->desc = *desc; r->hp = *hp; r->hflags = hf;
    D3D12_HEAP_TYPE type = hp->Type;
    if (type == D3D12_HEAP_TYPE_CUSTOM) type = hp->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE ? D3D12_HEAP_TYPE_DEFAULT : D3D12_HEAP_TYPE_UPLOAD;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        // Unified memory: every buffer is shared, so upload/readback heaps are plain mappable memory and default-heap
        // buffers can still be the target of the CPU-side copies a full implementation would add (WriteToSubresource).
        MTLResourceOptions o = MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked;
        if (type == D3D12_HEAP_TYPE_UPLOAD) o |= MTLResourceCPUCacheModeWriteCombined;
        r->buf = [d->mtl newBufferWithLength:desc->Width ? desc->Width : 1 options:o];
        if (!r->buf) { free(r); dev_release(d); return E_OUTOFMEMORY_; }
        r->va = r->buf.gpuAddress;
    } else {
        MTLPixelFormat pf = pixfmt(desc->Format);
        if (!pf) {
            char m[96]; snprintf(m, sizeof m, "metal backend: texture format %u", desc->Format); mtl_log_once(m);
            free(r); dev_release(d); return E_INVALIDARG_;
        }
        r->pf = pf;
        if (!r->desc.MipLevels) r->desc.MipLevels = max_mips(desc);
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.pixelFormat = pf; td.width = desc->Width; td.height = desc->Height; td.mipmapLevelCount = r->desc.MipLevels;
        td.sampleCount = desc->SampleDesc.Count ? desc->SampleDesc.Count : 1;
        switch (desc->Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            td.textureType = desc->DepthOrArraySize > 1 ? MTLTextureType1DArray : MTLTextureType1D; td.arrayLength = desc->DepthOrArraySize; break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            td.textureType = MTLTextureType3D; td.depth = desc->DepthOrArraySize; break;
        default:
            td.textureType = td.sampleCount > 1 ? (desc->DepthOrArraySize > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample)
                           : desc->DepthOrArraySize > 1 ? MTLTextureType2DArray : MTLTextureType2D;
            td.arrayLength = desc->DepthOrArraySize; break;
        }
        MTLTextureUsage u = MTLTextureUsagePixelFormatView;
        if (!(desc->Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) u |= MTLTextureUsageShaderRead;
        if (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) u |= MTLTextureUsageRenderTarget;
        if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) u |= MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
        td.usage = u;
        td.storageMode = MTLStorageModePrivate;
        td.hazardTrackingMode = MTLHazardTrackingModeTracked;
        r->tex = [d->mtl newTextureWithDescriptor:td];
        [td release];
        if (!r->tex) { free(r); dev_release(d); return E_OUTOFMEMORY_; }
    }
    reg_resource(d, r);
    HRESULT hr = obj_qi(r, riid, out);
    obj_release(r);
    return hr;
}

// ---- descriptor heaps -------------------------------------------------------------------------------------------
static u32 heap_stride(D3D12_DESCRIPTOR_HEAP_TYPE t) {
    return t == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || t == D3D12_DESCRIPTOR_HEAP_TYPE_DSV ? (u32)sizeof(MRtv) : D3D12MTL_ENTRY_SIZE;
}
static D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE heap_get_desc(ID3D12DescriptorHeap *self, D3D12_DESCRIPTOR_HEAP_DESC *ret) { *ret = ((MHeap *)self)->desc; return ret; }
static D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE heap_cpu_start(ID3D12DescriptorHeap *self, D3D12_CPU_DESCRIPTOR_HANDLE *ret) { ret->ptr = (SIZE_T)((MHeap *)self)->cpu; return ret; }
static D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE heap_gpu_start(ID3D12DescriptorHeap *self, D3D12_GPU_DESCRIPTOR_HANDLE *ret) { ret->ptr = ((MHeap *)self)->gpu; return ret; }
static ID3D12DescriptorHeapVtbl heap_vtbl = { CHILD_METHODS, .GetDesc = heap_get_desc,
    .GetCPUDescriptorHandleForHeapStart = heap_cpu_start, .GetGPUDescriptorHandleForHeapStart = heap_gpu_start };

// Writes one CBV_SRV_UAV / sampler entry (24 bytes: addr, MTLResourceID, metadata) and keeps its view object alive.
static void put_entry(MDevice *d, D3D12_CPU_DESCRIPTOR_HANDLE h, u64 addr, id tex_or_sampler, int tex_slot, u64 meta) {
    u32 idx; MHeap *heap = find_heap(d, h.ptr, 0, &idx);
    u64 *e = (u64 *)h.ptr;
    e[0] = addr; e[1] = 0; e[2] = meta;
    if (tex_or_sampler) {
        MTLResourceID rid = [tex_or_sampler gpuResourceID];
        e[tex_slot ? D3D12MTL_SLOT_TEXTURE : D3D12MTL_SLOT_ADDR] = rid._impl;
    }
    if (heap) {
        [tex_or_sampler retain];
        [heap->objs[idx] release];
        heap->objs[idx] = tex_or_sampler;
    }
}

// ---- root signatures --------------------------------------------------------------------------------------------
static ID3D12RootSignatureVtbl rs_vtbl = { CHILD_METHODS };

// ---- fences -----------------------------------------------------------------------------------------------------
static UINT64 STDMETHODCALLTYPE fence_completed(ID3D12Fence1 *self) { return ((MFence *)self)->ev.signaledValue; }
static HRESULT STDMETHODCALLTYPE fence_set_event(ID3D12Fence1 *self, UINT64 value, HANDLE event) {
    MFence *f = (MFence *)self;
    if (f->ev.signaledValue >= value) { if (event) rt_signal_event((u64)event); return S_OK; }
    if (!event) {   // D3D12: a NULL event means "block until the value is reached"
        while (f->ev.signaledValue < value) usleep(50);
        return S_OK;
    }
    u64 h = (u64)event;
    [f->ev notifyListener:f->dev->listener atValue:value block:^(id<MTLSharedEvent> e, uint64_t v) { rt_signal_event(h); }];
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fence_signal(ID3D12Fence1 *self, UINT64 value) { ((MFence *)self)->ev.signaledValue = value; return S_OK; }
static D3D12_FENCE_FLAGS STDMETHODCALLTYPE fence_flags(ID3D12Fence1 *self) { return ((MFence *)self)->flags; }
static ID3D12Fence1Vtbl fence_vtbl = { CHILD_METHODS, .GetCompletedValue = fence_completed, .SetEventOnCompletion = fence_set_event,
    .Signal = fence_signal, .GetCreationFlags = fence_flags };

// ---- command allocator ------------------------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE alloc_reset(ID3D12CommandAllocator *self) { return S_OK; }   // streams belong to the lists
static ID3D12CommandAllocatorVtbl alloc_vtbl = { CHILD_METHODS, .Reset = alloc_reset };

// ---- pipeline state ---------------------------------------------------------------------------------------------
static ID3D12PipelineStateVtbl pso_vtbl = { CHILD_METHODS };

static MTLBlendFactor blend_factor(D3D12_BLEND b) {
    switch (b) {
    case D3D12_BLEND_ZERO: return MTLBlendFactorZero;
    case D3D12_BLEND_ONE: return MTLBlendFactorOne;
    case D3D12_BLEND_SRC_COLOR: return MTLBlendFactorSourceColor;
    case D3D12_BLEND_INV_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case D3D12_BLEND_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case D3D12_BLEND_INV_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case D3D12_BLEND_DEST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case D3D12_BLEND_INV_DEST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case D3D12_BLEND_DEST_COLOR: return MTLBlendFactorDestinationColor;
    case D3D12_BLEND_INV_DEST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case D3D12_BLEND_SRC_ALPHA_SAT: return MTLBlendFactorSourceAlphaSaturated;
    case D3D12_BLEND_BLEND_FACTOR: return MTLBlendFactorBlendColor;
    case D3D12_BLEND_INV_BLEND_FACTOR: return MTLBlendFactorOneMinusBlendColor;
    case D3D12_BLEND_SRC1_COLOR: return MTLBlendFactorSource1Color;
    case D3D12_BLEND_INV_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
    case D3D12_BLEND_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
    case D3D12_BLEND_INV_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation blend_op(D3D12_BLEND_OP o) {
    switch (o) {
    case D3D12_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
    case D3D12_BLEND_OP_REV_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case D3D12_BLEND_OP_MIN: return MTLBlendOperationMin;
    case D3D12_BLEND_OP_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}
static MTLCompareFunction compare(D3D12_COMPARISON_FUNC f) {
    switch (f) {
    case D3D12_COMPARISON_FUNC_NEVER: return MTLCompareFunctionNever;
    case D3D12_COMPARISON_FUNC_LESS: return MTLCompareFunctionLess;
    case D3D12_COMPARISON_FUNC_EQUAL: return MTLCompareFunctionEqual;
    case D3D12_COMPARISON_FUNC_LESS_EQUAL: return MTLCompareFunctionLessEqual;
    case D3D12_COMPARISON_FUNC_GREATER: return MTLCompareFunctionGreater;
    case D3D12_COMPARISON_FUNC_NOT_EQUAL: return MTLCompareFunctionNotEqual;
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}
static MTLStencilOperation stencil_op(D3D12_STENCIL_OP o) {
    switch (o) {
    case D3D12_STENCIL_OP_ZERO: return MTLStencilOperationZero;
    case D3D12_STENCIL_OP_REPLACE: return MTLStencilOperationReplace;
    case D3D12_STENCIL_OP_INCR_SAT: return MTLStencilOperationIncrementClamp;
    case D3D12_STENCIL_OP_DECR_SAT: return MTLStencilOperationDecrementClamp;
    case D3D12_STENCIL_OP_INVERT: return MTLStencilOperationInvert;
    case D3D12_STENCIL_OP_INCR: return MTLStencilOperationIncrementWrap;
    case D3D12_STENCIL_OP_DECR: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}
static MTLStencilDescriptor *stencil_desc(const D3D12_DEPTH_STENCILOP_DESC *s, u8 rmask, u8 wmask) {
    MTLStencilDescriptor *d = [[MTLStencilDescriptor new] autorelease];
    d.stencilCompareFunction = compare(s->StencilFunc); d.stencilFailureOperation = stencil_op(s->StencilFailOp);
    d.depthFailureOperation = stencil_op(s->StencilDepthFailOp); d.depthStencilPassOperation = stencil_op(s->StencilPassOp);
    d.readMask = rmask; d.writeMask = wmask;
    return d;
}

// Shader -> MTLFunction. The MSL is compiled by the Metal framework at runtime (WINTOMAC_D3D12_MSL_DUMP=dir keeps it).
static id<MTLFunction> make_function(MDevice *d, MRootSig *rs, enum vkd3d_shader_visibility stage, const D3D12_SHADER_BYTECODE *bc,
                                     const struct vkd3d_shader_signature *next_input, int points, char *err, size_t errn) {
    D3D12MtlShaderIn in = { stage, bc->pShaderBytecode, bc->BytecodeLength, &rs->desc.u.v_1_1, &rs->layout, next_input, points };
    char *msl = d3d12mtl_translate(&in, err, errn);
    if (!msl) return nil;
    const char *dump = getenv("WINTOMAC_D3D12_MSL_DUMP");
    if (dump) {
        static atomic_int n; char p[512]; snprintf(p, sizeof p, "%s/shader_%03d_%s.metal", dump, atomic_fetch_add(&n, 1),
                                                   stage == VKD3D_SHADER_VISIBILITY_VERTEX ? "vs" : stage == VKD3D_SHADER_VISIBILITY_PIXEL ? "ps" : "cs");
        FILE *f = fopen(p, "w"); if (f) { fputs(msl, f); fclose(f); }
    }
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.languageVersion = MTLLanguageVersion3_1;
    o.mathMode = MTLMathModeSafe;   // D3D float semantics (no fast-math reassociation)
    NSError *e = nil;
    id<MTLLibrary> lib = [d->mtl newLibraryWithSource:[NSString stringWithUTF8String:msl] options:o error:&e];
    [o release];
    free(msl);
    if (!lib) { snprintf(err, errn, "MSL compile: %s", e ? e.localizedDescription.UTF8String : "?"); return nil; }
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
    [lib release];
    if (!fn) snprintf(err, errn, "no main0 in MSL");
    return fn;
}
static int scan_sigs(const D3D12_SHADER_BYTECODE *bc, struct vkd3d_shader_scan_signature_info *s) {
    memset(s, 0, sizeof *s);
    s->type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
    struct vkd3d_shader_compile_info ci = { VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO, s };
    ci.source.code = bc->pShaderBytecode; ci.source.size = bc->BytecodeLength;
    ci.source_type = d3d12mtl_is_dxil(bc->pShaderBytecode, bc->BytecodeLength) ? VKD3D_SHADER_SOURCE_DXBC_DXIL : VKD3D_SHADER_SOURCE_DXBC_TPF;
    ci.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY; ci.log_level = VKD3D_SHADER_LOG_NONE;
    return vkd3d_shader_scan(&ci, NULL) >= 0;
}
static MTLPrimitiveTopologyClass topo_class(D3D12_PRIMITIVE_TOPOLOGY_TYPE t) {
    return t == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT ? MTLPrimitiveTopologyClassPoint
         : t == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ? MTLPrimitiveTopologyClassLine : MTLPrimitiveTopologyClassTriangle;
}
static double now_ms(void) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static HRESULT create_graphics_pso(MDevice *d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *g, REFIID riid, void **out) {
    MRootSig *rs = (MRootSig *)g->pRootSignature;
    char err[1024] = "";
    if (!rs) { mtl_log_once("metal backend: PSO without root signature (root signature in DXIL) unsupported"); return E_INVALIDARG_; }
    if ((g->GS.BytecodeLength || g->HS.BytecodeLength || g->DS.BytecodeLength)) {
        mtl_log_once("metal backend: geometry/tessellation shaders not implemented (docs/d3d12_metal.md)");
        return E_NOTIMPL_;
    }
    double t0 = now_ms();
    struct vkd3d_shader_scan_signature_info vss, pss; int have_ps = g->PS.BytecodeLength > 0;
    if (!scan_sigs(&g->VS, &vss)) return E_INVALIDARG_;
    if (have_ps && !scan_sigs(&g->PS, &pss)) { vkd3d_shader_free_scan_signature_info(&vss); return E_INVALIDARG_; }
    id<MTLFunction> vs = make_function(d, rs, VKD3D_SHADER_VISIBILITY_VERTEX, &g->VS, have_ps ? &pss.input : NULL,
                                     g->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, err, sizeof err);
    id<MTLFunction> ps = vs && have_ps ? make_function(d, rs, VKD3D_SHADER_VISIBILITY_PIXEL, &g->PS, NULL, 0, err, sizeof err) : nil;
    if (!vs || (have_ps && !ps)) {
        rt_log("d3d12 (metal): shader translation failed: %s", err);
        [vs release]; vkd3d_shader_free_scan_signature_info(&vss); if (have_ps) vkd3d_shader_free_scan_signature_info(&pss);
        return E_INVALIDARG_;
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = ps;
    // input layout -> vertex descriptor; attribute index = the VS input register of the element's semantic
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    u32 next_off[D3D12MTL_MAX_VB] = { 0 };
    for (u32 i = 0; i < g->InputLayout.NumElements; i++) {
        const D3D12_INPUT_ELEMENT_DESC *e = &g->InputLayout.pInputElementDescs[i];
        int reg = -1;
        for (unsigned k = 0; k < vss.input.element_count; k++)
            if (!strcasecmp(vss.input.elements[k].semantic_name, e->SemanticName) && vss.input.elements[k].semantic_index == e->SemanticIndex)
                reg = vss.input.elements[k].register_index;
        const Fmt *f = fmt(e->Format);
        if (e->InputSlot >= D3D12MTL_MAX_VB || !f || !f->vtx) { mtl_log_once("metal backend: vertex format / slot unsupported"); continue; }
        u32 off = e->AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? next_off[e->InputSlot] : e->AlignedByteOffset;
        next_off[e->InputSlot] = off + f->bytes;
        if (reg < 0) continue;   // not read by the shader
        vd.attributes[reg].format = f->vtx; vd.attributes[reg].offset = off; vd.attributes[reg].bufferIndex = D3D12MTL_BUF_VB0 + e->InputSlot;
        MTLVertexBufferLayoutDescriptor *l = vd.layouts[D3D12MTL_BUF_VB0 + e->InputSlot];
        l.stride = MTLBufferLayoutStrideDynamic;   // D3D12 sets strides with IASetVertexBuffers
        if (e->InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) {
            l.stepFunction = e->InstanceDataStepRate ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionConstant;
            l.stepRate = e->InstanceDataStepRate ? e->InstanceDataStepRate : 0;
        }
    }
    pd.vertexDescriptor = vd;
    for (u32 i = 0; i < g->NumRenderTargets && i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *c = pd.colorAttachments[i];
        c.pixelFormat = pixfmt(g->RTVFormats[i]);
        const D3D12_RENDER_TARGET_BLEND_DESC *b = &g->BlendState.RenderTarget[g->BlendState.IndependentBlendEnable ? i : 0];
        c.blendingEnabled = b->BlendEnable;
        c.sourceRGBBlendFactor = blend_factor(b->SrcBlend); c.destinationRGBBlendFactor = blend_factor(b->DestBlend);
        c.rgbBlendOperation = blend_op(b->BlendOp);
        c.sourceAlphaBlendFactor = blend_factor(b->SrcBlendAlpha); c.destinationAlphaBlendFactor = blend_factor(b->DestBlendAlpha);
        c.alphaBlendOperation = blend_op(b->BlendOpAlpha);
        c.writeMask = (b->RenderTargetWriteMask & 1 ? MTLColorWriteMaskRed : 0) | (b->RenderTargetWriteMask & 2 ? MTLColorWriteMaskGreen : 0)
                    | (b->RenderTargetWriteMask & 4 ? MTLColorWriteMaskBlue : 0) | (b->RenderTargetWriteMask & 8 ? MTLColorWriteMaskAlpha : 0);
        if (b->LogicOpEnable) mtl_log_once("metal backend: logic ops (no Metal equivalent)");
    }
    MTLPixelFormat dsf = pixfmt(g->DSVFormat);
    if (is_depth(dsf)) { pd.depthAttachmentPixelFormat = dsf; if (has_stencil(dsf)) pd.stencilAttachmentPixelFormat = dsf; }
    pd.rasterSampleCount = g->SampleDesc.Count ? g->SampleDesc.Count : 1;
    pd.alphaToCoverageEnabled = g->BlendState.AlphaToCoverageEnable;
    pd.inputPrimitiveTopology = topo_class(g->PrimitiveTopologyType);
    NSError *e = nil;
    id<MTLRenderPipelineState> rps = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&e];
    [pd release]; [vs release]; [ps release];
    vkd3d_shader_free_scan_signature_info(&vss); if (have_ps) vkd3d_shader_free_scan_signature_info(&pss);
    if (!rps) { rt_log("d3d12 (metal): render pipeline: %s", e ? e.localizedDescription.UTF8String : "?"); return E_INVALIDARG_; }

    MPso *p = calloc(1, sizeof *p);
    head_init(p, &pso_vtbl, K_PSO, d);
    p->rs = rs; obj_addref(rs); p->rps = rps;
    const D3D12_DEPTH_STENCIL_DESC *ds = &g->DepthStencilState;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    if (is_depth(dsf) && ds->DepthEnable) { dd.depthCompareFunction = compare(ds->DepthFunc); dd.depthWriteEnabled = ds->DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL; }
    if (has_stencil(dsf) && ds->StencilEnable) {
        dd.frontFaceStencil = stencil_desc(&ds->FrontFace, ds->StencilReadMask, ds->StencilWriteMask);
        dd.backFaceStencil = stencil_desc(&ds->BackFace, ds->StencilReadMask, ds->StencilWriteMask);
    }
    p->dss = [d->mtl newDepthStencilStateWithDescriptor:dd];
    [dd release];
    const D3D12_RASTERIZER_DESC *r = &g->RasterizerState;
    p->cull = r->CullMode == D3D12_CULL_MODE_FRONT ? MTLCullModeFront : r->CullMode == D3D12_CULL_MODE_BACK ? MTLCullModeBack : MTLCullModeNone;
    // D3D: clockwise is front unless FrontCounterClockwise; Metal's y-down viewport keeps the same winding sense.
    p->winding = r->FrontCounterClockwise ? MTLWindingCounterClockwise : MTLWindingClockwise;
    p->fill = r->FillMode == D3D12_FILL_MODE_WIREFRAME ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
    p->clip = r->DepthClipEnable ? MTLDepthClipModeClip : MTLDepthClipModeClamp;
    p->bias = (float)r->DepthBias; p->slope = r->SlopeScaledDepthBias; p->clamp = r->DepthBiasClamp;
    if (getenv("WINTOMAC_D3D12_TRACE")) rt_log("d3d12 (metal): graphics PSO compiled in %.1f ms", now_ms() - t0);
    HRESULT hr = obj_qi(p, riid, out);
    obj_release(p);
    return hr;
}

// ---- command lists: recording ------------------------------------------------------------------------------------
enum {
    C_PSO = 1, C_RS, C_ROOT_CONST, C_ROOT_TABLE, C_ROOT_VA, C_VIEWPORTS, C_SCISSORS, C_TOPOLOGY, C_VB, C_IB, C_RTS,
    C_CLEAR_RTV, C_CLEAR_DSV, C_DRAW, C_DRAW_INDEXED, C_DISPATCH, C_COPY_BUF, C_COPY_TEX, C_COPY_RES, C_BLEND, C_STENCIL_REF,
};
typedef struct { u32 op, size; } CmdHdr;
static void *rec(MList *l, u32 op, size_t size) {
    size_t total = (sizeof(CmdHdr) + size + 7) & ~(size_t)7;
    if (l->ncmd + total > l->capcmd) { l->capcmd = (l->capcmd + total) * 2; l->cmd = realloc(l->cmd, l->capcmd); }
    CmdHdr *h = (CmdHdr *)(l->cmd + l->ncmd);
    h->op = op; h->size = (u32)total; l->ncmd += total;
    memset(h + 1, 0, total - sizeof *h);
    return h + 1;
}
typedef struct { u32 compute, index, count, offset; u32 data[]; } CRootConst;
typedef struct { u32 compute, index; u64 handle; } CRootVal;   // table GPU handle or root descriptor address
typedef struct { u32 compute; MRootSig *rs; } CRs;
typedef struct { u32 n; D3D12_VIEWPORT v[16]; } CViewports;
typedef struct { u32 n; D3D12_RECT r[16]; } CScissors;
typedef struct { u32 start, n; D3D12_VERTEX_BUFFER_VIEW v[]; } CVb;
typedef struct { u32 n; MRtv rt[8]; MRtv ds; } CRts;
typedef struct { MRtv rt; float color[4]; u32 flags; float depth; u8 stencil; } CClear;
typedef struct { u32 a, b, c, d, e; } CDraw;
typedef struct { MResource *dst, *src; u64 dst_off, src_off, size; } CCopyBuf;
typedef struct { D3D12_TEXTURE_COPY_LOCATION dst, src; u32 x, y, z, has_box; D3D12_BOX box; } CCopyTex;

static HRESULT STDMETHODCALLTYPE list_close(ID3D12GraphicsCommandList6 *self) { ((MList *)self)->open = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE list_reset(ID3D12GraphicsCommandList6 *self, ID3D12CommandAllocator *a, ID3D12PipelineState *pso) {
    MList *l = (MList *)self;
    l->ncmd = 0; l->open = 1; l->grs = l->crs = NULL;
    if (pso) *(MPso **)rec(l, C_PSO, sizeof(MPso *)) = (MPso *)pso;
    return S_OK;
}
static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE list_type(ID3D12GraphicsCommandList6 *self) { return ((MList *)self)->type; }
static void STDMETHODCALLTYPE list_clear_state(ID3D12GraphicsCommandList6 *self, ID3D12PipelineState *pso) {
    MList *l = (MList *)self;
    *(MPso **)rec(l, C_PSO, sizeof(MPso *)) = (MPso *)pso;
}
static void STDMETHODCALLTYPE list_draw(ID3D12GraphicsCommandList6 *self, UINT n, UINT inst, UINT first, UINT first_inst) {
    *(CDraw *)rec((MList *)self, C_DRAW, sizeof(CDraw)) = (CDraw){ n, inst, first, first_inst, 0 };
}
static void STDMETHODCALLTYPE list_draw_indexed(ID3D12GraphicsCommandList6 *self, UINT n, UINT inst, UINT first, INT base, UINT first_inst) {
    *(CDraw *)rec((MList *)self, C_DRAW_INDEXED, sizeof(CDraw)) = (CDraw){ n, inst, first, (u32)base, first_inst };
}
static void STDMETHODCALLTYPE list_dispatch(ID3D12GraphicsCommandList6 *self, UINT x, UINT y, UINT z) {
    *(CDraw *)rec((MList *)self, C_DISPATCH, sizeof(CDraw)) = (CDraw){ x, y, z, 0, 0 };
}
static void STDMETHODCALLTYPE list_copy_buffer(ID3D12GraphicsCommandList6 *self, ID3D12Resource *dst, UINT64 doff, ID3D12Resource *src, UINT64 soff, UINT64 n) {
    *(CCopyBuf *)rec((MList *)self, C_COPY_BUF, sizeof(CCopyBuf)) = (CCopyBuf){ (MResource *)dst, (MResource *)src, doff, soff, n };
}
static void STDMETHODCALLTYPE list_copy_texture(ID3D12GraphicsCommandList6 *self, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
                                                const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box) {
    CCopyTex *c = rec((MList *)self, C_COPY_TEX, sizeof(CCopyTex));
    c->dst = *dst; c->src = *src; c->x = x; c->y = y; c->z = z;
    if (box) { c->has_box = 1; c->box = *box; }
}
static void STDMETHODCALLTYPE list_copy_resource(ID3D12GraphicsCommandList6 *self, ID3D12Resource *dst, ID3D12Resource *src) {
    *(CCopyBuf *)rec((MList *)self, C_COPY_RES, sizeof(CCopyBuf)) = (CCopyBuf){ (MResource *)dst, (MResource *)src };
}
static void STDMETHODCALLTYPE list_topology(ID3D12GraphicsCommandList6 *self, D3D12_PRIMITIVE_TOPOLOGY t) {
    *(u32 *)rec((MList *)self, C_TOPOLOGY, 4) = t;
}
static void STDMETHODCALLTYPE list_viewports(ID3D12GraphicsCommandList6 *self, UINT n, const D3D12_VIEWPORT *v) {
    CViewports *c = rec((MList *)self, C_VIEWPORTS, sizeof(CViewports));
    c->n = n > 16 ? 16 : n; memcpy(c->v, v, c->n * sizeof *v);
}
static void STDMETHODCALLTYPE list_scissors(ID3D12GraphicsCommandList6 *self, UINT n, const D3D12_RECT *r) {
    CScissors *c = rec((MList *)self, C_SCISSORS, sizeof(CScissors));
    c->n = n > 16 ? 16 : n; memcpy(c->r, r, c->n * sizeof *r);
}
static void STDMETHODCALLTYPE list_blend_factor(ID3D12GraphicsCommandList6 *self, const FLOAT f[4]) {
    static const float one[4] = { 1, 1, 1, 1 };
    memcpy(rec((MList *)self, C_BLEND, 16), f ? f : one, 16);
}
static void STDMETHODCALLTYPE list_stencil_ref(ID3D12GraphicsCommandList6 *self, UINT ref) { *(u32 *)rec((MList *)self, C_STENCIL_REF, 4) = ref; }
static void STDMETHODCALLTYPE list_set_pso(ID3D12GraphicsCommandList6 *self, ID3D12PipelineState *p) { *(MPso **)rec((MList *)self, C_PSO, sizeof(MPso *)) = (MPso *)p; }
static void STDMETHODCALLTYPE list_barrier(ID3D12GraphicsCommandList6 *self, UINT n, const D3D12_RESOURCE_BARRIER *b) {
    // Transitions and aliasing barriers: nothing to do, every resource here is hazard-tracked by Metal. UAV barriers
    // between dispatches: Metal orders dispatches in a serial compute encoder that touch the same tracked resource.
}
static void STDMETHODCALLTYPE list_set_heaps(ID3D12GraphicsCommandList6 *self, UINT n, ID3D12DescriptorHeap *const *h) {}   // residency set covers them
static void set_rs(MList *l, int compute, ID3D12RootSignature *rs) {
    if (compute) l->crs = (MRootSig *)rs; else l->grs = (MRootSig *)rs;
    *(CRs *)rec(l, C_RS, sizeof(CRs)) = (CRs){ (u32)compute, (MRootSig *)rs };
}
static void STDMETHODCALLTYPE list_set_crs(ID3D12GraphicsCommandList6 *self, ID3D12RootSignature *rs) { set_rs((MList *)self, 1, rs); }
static void STDMETHODCALLTYPE list_set_grs(ID3D12GraphicsCommandList6 *self, ID3D12RootSignature *rs) { set_rs((MList *)self, 0, rs); }
static void root_val(MList *l, int compute, u32 op, UINT idx, u64 v) { *(CRootVal *)rec(l, op, sizeof(CRootVal)) = (CRootVal){ (u32)compute, idx, v }; }
static void STDMETHODCALLTYPE list_c_table(ID3D12GraphicsCommandList6 *self, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { root_val((MList *)self, 1, C_ROOT_TABLE, i, h.ptr); }
static void STDMETHODCALLTYPE list_g_table(ID3D12GraphicsCommandList6 *self, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { root_val((MList *)self, 0, C_ROOT_TABLE, i, h.ptr); }
static void STDMETHODCALLTYPE list_c_va(ID3D12GraphicsCommandList6 *self, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { root_val((MList *)self, 1, C_ROOT_VA, i, va); }
static void STDMETHODCALLTYPE list_g_va(ID3D12GraphicsCommandList6 *self, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { root_val((MList *)self, 0, C_ROOT_VA, i, va); }
static void root_consts(MList *l, int compute, UINT idx, UINT n, const void *data, UINT off) {
    CRootConst *c = rec(l, C_ROOT_CONST, sizeof(CRootConst) + 4 * n);
    c->compute = compute; c->index = idx; c->count = n; c->offset = off; memcpy(c->data, data, 4 * n);
}
static void STDMETHODCALLTYPE list_c_const(ID3D12GraphicsCommandList6 *self, UINT i, UINT v, UINT off) { root_consts((MList *)self, 1, i, 1, &v, off); }
static void STDMETHODCALLTYPE list_g_const(ID3D12GraphicsCommandList6 *self, UINT i, UINT v, UINT off) { root_consts((MList *)self, 0, i, 1, &v, off); }
static void STDMETHODCALLTYPE list_c_consts(ID3D12GraphicsCommandList6 *self, UINT i, UINT n, const void *d, UINT off) { root_consts((MList *)self, 1, i, n, d, off); }
static void STDMETHODCALLTYPE list_g_consts(ID3D12GraphicsCommandList6 *self, UINT i, UINT n, const void *d, UINT off) { root_consts((MList *)self, 0, i, n, d, off); }
static void STDMETHODCALLTYPE list_ib(ID3D12GraphicsCommandList6 *self, const D3D12_INDEX_BUFFER_VIEW *v) {
    D3D12_INDEX_BUFFER_VIEW *c = rec((MList *)self, C_IB, sizeof *c);
    if (v) *c = *v;
}
static void STDMETHODCALLTYPE list_vb(ID3D12GraphicsCommandList6 *self, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW *v) {
    CVb *c = rec((MList *)self, C_VB, sizeof(CVb) + n * sizeof *v);
    c->start = start; c->n = n;
    if (v) memcpy(c->v, v, n * sizeof *v); else memset(c->v, 0, n * sizeof *v);
}
static void STDMETHODCALLTYPE list_set_rts(ID3D12GraphicsCommandList6 *self, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rts, WINBOOL_ single,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE *ds) {
    CRts *c = rec((MList *)self, C_RTS, sizeof(CRts));
    c->n = n > 8 ? 8 : n;
    for (u32 i = 0; i < c->n; i++) {
        const MRtv *r = single ? (const MRtv *)(rts[0].ptr + i * sizeof(MRtv)) : (const MRtv *)rts[i].ptr;
        c->rt[i] = *r;
    }
    if (ds) c->ds = *(const MRtv *)ds->ptr;
}
static void STDMETHODCALLTYPE list_clear_rtv(ID3D12GraphicsCommandList6 *self, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT nrects, const D3D12_RECT *rects) {
    CClear *c = rec((MList *)self, C_CLEAR_RTV, sizeof(CClear));
    c->rt = *(const MRtv *)rtv.ptr; memcpy(c->color, color, 16);
    if (nrects) mtl_log_once("metal backend: ClearRenderTargetView with rects clears the whole view");
}
static void STDMETHODCALLTYPE list_clear_dsv(ID3D12GraphicsCommandList6 *self, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                             UINT8 stencil, UINT nrects, const D3D12_RECT *rects) {
    CClear *c = rec((MList *)self, C_CLEAR_DSV, sizeof(CClear));
    c->rt = *(const MRtv *)dsv.ptr; c->flags = flags; c->depth = depth; c->stencil = stencil;
    if (nrects) mtl_log_once("metal backend: ClearDepthStencilView with rects clears the whole view");
}
static void STDMETHODCALLTYPE list_nop_marker(ID3D12GraphicsCommandList6 *self, UINT m, const void *d, UINT s) {}
static void STDMETHODCALLTYPE list_nop_end(ID3D12GraphicsCommandList6 *self) {}
static void STDMETHODCALLTYPE list_predication(ID3D12GraphicsCommandList6 *self, ID3D12Resource *b, UINT64 off, D3D12_PREDICATION_OP op) {
    if (b) mtl_log_once("metal backend: predication ignored (always draws)");
}
static ID3D12GraphicsCommandList6Vtbl list_vtbl = { CHILD_METHODS,
    .GetType = list_type, .Close = list_close, .Reset = list_reset, .ClearState = list_clear_state,
    .DrawInstanced = list_draw, .DrawIndexedInstanced = list_draw_indexed, .Dispatch = list_dispatch,
    .CopyBufferRegion = list_copy_buffer, .CopyTextureRegion = list_copy_texture, .CopyResource = list_copy_resource,
    .IASetPrimitiveTopology = list_topology, .RSSetViewports = list_viewports, .RSSetScissorRects = list_scissors,
    .OMSetBlendFactor = list_blend_factor, .OMSetStencilRef = list_stencil_ref, .SetPipelineState = list_set_pso,
    .ResourceBarrier = list_barrier, .SetDescriptorHeaps = list_set_heaps,
    .SetComputeRootSignature = list_set_crs, .SetGraphicsRootSignature = list_set_grs,
    .SetComputeRootDescriptorTable = list_c_table, .SetGraphicsRootDescriptorTable = list_g_table,
    .SetComputeRoot32BitConstant = list_c_const, .SetGraphicsRoot32BitConstant = list_g_const,
    .SetComputeRoot32BitConstants = list_c_consts, .SetGraphicsRoot32BitConstants = list_g_consts,
    .SetComputeRootConstantBufferView = list_c_va, .SetGraphicsRootConstantBufferView = list_g_va,
    .SetComputeRootShaderResourceView = list_c_va, .SetGraphicsRootShaderResourceView = list_g_va,
    .SetComputeRootUnorderedAccessView = list_c_va, .SetGraphicsRootUnorderedAccessView = list_g_va,
    .IASetIndexBuffer = list_ib, .IASetVertexBuffers = list_vb, .OMSetRenderTargets = list_set_rts,
    .ClearDepthStencilView = list_clear_dsv, .ClearRenderTargetView = list_clear_rtv,
    .SetPredication = list_predication, .SetMarker = list_nop_marker, .BeginEvent = list_nop_marker, .EndEvent = list_nop_end };

// ---- command lists: replay into a Metal command buffer ----------------------------------------------------------
typedef struct { id<MTLBuffer> buf; u64 off; } BufRef;
typedef struct {
    MDevice *dev; id<MTLCommandBuffer> cb;
    id<MTLRenderCommandEncoder> re; id<MTLBlitCommandEncoder> be; id<MTLComputeCommandEncoder> ce;
    CRts rts; int pass_dirty;
    CClear clears[16]; int nclears;         // pending colour clears (become load actions)
    CClear dclear; int has_dclear;          // pending depth/stencil clear
    MPso *pso; MRootSig *rs[2];
    u8 root[2][1024];                       // root argument blocks (graphics, compute)
    BufRef params[2][D3D12MTL_MAX_ROOT_PARAMS];
    D3D12_VERTEX_BUFFER_VIEW vb[D3D12MTL_MAX_VB]; D3D12_INDEX_BUFFER_VIEW ib;
    D3D12_PRIMITIVE_TOPOLOGY topo;
    CViewports vp; CScissors sc; float blend[4]; u32 stencil_ref;
    u32 draws;
    // what the current render encoder already has (state is only re-sent when it changes)
    MPso *bound_pso; int dirty_vp, dirty_vb, dirty_root[2], dirty_misc;
} Replay;
static BufRef va_ref(MDevice *d, u64 va) {
    MResource *r = find_va(d, va);
    return r ? (BufRef){ r->buf, va - r->va } : (BufRef){ nil, 0 };
}
static BufRef table_ref(MDevice *d, u64 gpu_handle) {
    u32 idx; MHeap *h = find_heap(d, gpu_handle, 1, &idx);
    return h ? (BufRef){ h->buf, gpu_handle - h->gpu } : (BufRef){ nil, 0 };
}
static void end_encoders(Replay *r) {
    if (r->re) { [r->re endEncoding]; r->re = nil; }
    if (r->be) { [r->be endEncoding]; r->be = nil; }
    if (r->ce) { [r->ce endEncoding]; r->ce = nil; }
}
static int same_view(const MRtv *a, const MRtv *b) { return a->tex == b->tex && a->level == b->level && a->slice == b->slice; }
static void set_attachment(MTLRenderPassAttachmentDescriptor *a, const MRtv *v) {
    a.texture = v->tex; a.level = v->level;
    if (v->tex.textureType == MTLTextureType3D) a.depthPlane = v->slice; else a.slice = v->slice;
    a.loadAction = MTLLoadActionLoad; a.storeAction = MTLStoreActionStore;
}
// Begin a render pass for the current render targets; pending clears of these views become load actions,
// other pending clears are flushed first (they happened earlier in the stream).
static void flush_clears(Replay *r, const CRts *keep);
static void begin_pass(Replay *r) {
    end_encoders(r);
    flush_clears(r, &r->rts);
    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
    for (u32 i = 0; i < r->rts.n; i++) {
        if (!r->rts.rt[i].tex) continue;
        MTLRenderPassColorAttachmentDescriptor *a = pd.colorAttachments[i];
        set_attachment(a, &r->rts.rt[i]);
        for (int k = 0; k < r->nclears; k++)
            if (same_view(&r->clears[k].rt, &r->rts.rt[i])) {
                float *c = r->clears[k].color;
                a.loadAction = MTLLoadActionClear; a.clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
                r->clears[k] = r->clears[--r->nclears]; k--;
            }
    }
    if (r->rts.ds.tex) {
        set_attachment(pd.depthAttachment, &r->rts.ds);
        if (has_stencil(r->rts.ds.pf)) set_attachment(pd.stencilAttachment, &r->rts.ds);
        if (r->has_dclear && same_view(&r->dclear.rt, &r->rts.ds)) {
            if (r->dclear.flags & D3D12_CLEAR_FLAG_DEPTH) { pd.depthAttachment.loadAction = MTLLoadActionClear; pd.depthAttachment.clearDepth = r->dclear.depth; }
            if ((r->dclear.flags & D3D12_CLEAR_FLAG_STENCIL) && has_stencil(r->rts.ds.pf)) {
                pd.stencilAttachment.loadAction = MTLLoadActionClear; pd.stencilAttachment.clearStencil = r->dclear.stencil;
            }
            r->has_dclear = 0;
        }
    }
    r->re = [r->cb renderCommandEncoderWithDescriptor:pd];
    r->pass_dirty = 0;
    r->bound_pso = NULL; r->dirty_vp = r->dirty_vb = r->dirty_root[0] = r->dirty_misc = 1;
}
static void flush_clears(Replay *r, const CRts *keep) {
    for (int k = 0; k < r->nclears; k++) {
        int kept = 0;
        for (u32 i = 0; keep && i < keep->n; i++) if (same_view(&r->clears[k].rt, &keep->rt[i])) kept = 1;
        if (kept) continue;
        MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
        set_attachment(pd.colorAttachments[0], &r->clears[k].rt);
        float *c = r->clears[k].color;
        pd.colorAttachments[0].loadAction = MTLLoadActionClear; pd.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        if (r->re || r->be || r->ce) end_encoders(r);
        [[r->cb renderCommandEncoderWithDescriptor:pd] endEncoding];
        r->clears[k] = r->clears[--r->nclears]; k--;
    }
    if (r->has_dclear && !(keep && same_view(&r->dclear.rt, &keep->ds))) {
        MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
        set_attachment(pd.depthAttachment, &r->dclear.rt);
        if (r->dclear.flags & D3D12_CLEAR_FLAG_DEPTH) { pd.depthAttachment.loadAction = MTLLoadActionClear; pd.depthAttachment.clearDepth = r->dclear.depth; }
        if (has_stencil(r->dclear.rt.pf)) {
            set_attachment(pd.stencilAttachment, &r->dclear.rt);
            if (r->dclear.flags & D3D12_CLEAR_FLAG_STENCIL) { pd.stencilAttachment.loadAction = MTLLoadActionClear; pd.stencilAttachment.clearStencil = r->dclear.stencil; }
        }
        if (r->re || r->be || r->ce) end_encoders(r);
        [[r->cb renderCommandEncoderWithDescriptor:pd] endEncoding];
        r->has_dclear = 0;
    }
}
static id<MTLBlitCommandEncoder> blit(Replay *r) {
    if (r->be) return r->be;
    end_encoders(r);
    flush_clears(r, NULL);
    r->pass_dirty = 1;
    return r->be = [r->cb blitCommandEncoder];
}
static MTLPrimitiveType prim(D3D12_PRIMITIVE_TOPOLOGY t) {
    switch (t) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return MTLPrimitiveTypePoint;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return MTLPrimitiveTypeLine;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return MTLPrimitiveTypeLineStrip;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}
// Root arguments for one stage: the root block (constants) and the per-parameter buffers.
static void bind_root(Replay *r, int compute, int stage /* 0 vertex 1 fragment 2 compute */) {
    MRootSig *rs = r->rs[compute];
    if (!rs) return;
    const D3D12MtlRootLayout *L = &rs->layout;
    for (u32 i = 0; i < L->count; i++) {
        int slot = L->param[i].slot;
        if (slot < 0) continue;
        BufRef b = r->params[compute][i];
        if (!b.buf) continue;
        if (stage == 0) [r->re setVertexBuffer:b.buf offset:b.off atIndex:slot];
        else if (stage == 1) [r->re setFragmentBuffer:b.buf offset:b.off atIndex:slot];
        else [r->ce setBuffer:b.buf offset:b.off atIndex:slot];
    }
    if (stage == 0) [r->re setVertexBytes:r->root[compute] length:L->size atIndex:D3D12MTL_BUF_ROOT];
    else if (stage == 1) [r->re setFragmentBytes:r->root[compute] length:L->size atIndex:D3D12MTL_BUF_ROOT];
    else [r->ce setBytes:r->root[compute] length:L->size atIndex:D3D12MTL_BUF_ROOT];
}
static void prepare_draw(Replay *r) {
    if (!r->re || r->pass_dirty) begin_pass(r);
    MPso *p = r->pso;
    id<MTLRenderCommandEncoder> e = r->re;
    if (p != r->bound_pso) {
        [e setRenderPipelineState:p->rps];
        [e setDepthStencilState:p->dss];
        [e setCullMode:p->cull]; [e setFrontFacingWinding:p->winding]; [e setTriangleFillMode:p->fill];
        [e setDepthClipMode:p->clip]; [e setDepthBias:p->bias slopeScale:p->slope clamp:p->clamp];
        r->bound_pso = p;
    }
    if (r->dirty_vp) {
        // viewports / scissors, the latter clamped to the attachment (Metal validates that)
        id<MTLTexture> t = r->rts.n && r->rts.rt[0].tex ? r->rts.rt[0].tex : r->rts.ds.tex;
        u32 tw = t ? mip_dim(t.width, r->rts.n && r->rts.rt[0].tex ? r->rts.rt[0].level : r->rts.ds.level) : 1;
        u32 th = t ? mip_dim(t.height, r->rts.n && r->rts.rt[0].tex ? r->rts.rt[0].level : r->rts.ds.level) : 1;
        MTLViewport vps[16]; MTLScissorRect scs[16];
        for (u32 i = 0; i < r->vp.n; i++) {
            const D3D12_VIEWPORT *v = &r->vp.v[i];
            vps[i] = (MTLViewport){ v->TopLeftX, v->TopLeftY, v->Width, v->Height, v->MinDepth, v->MaxDepth };
        }
        if (r->vp.n) [e setViewports:vps count:r->vp.n];
        u32 ns = r->sc.n;
        for (u32 i = 0; i < ns; i++) {
            const D3D12_RECT *s = &r->sc.r[i];
            LONG l = s->left < 0 ? 0 : s->left, tp = s->top < 0 ? 0 : s->top;
            LONG rr = s->right > (LONG)tw ? (LONG)tw : s->right, b = s->bottom > (LONG)th ? (LONG)th : s->bottom;
            scs[i] = rr > l && b > tp ? (MTLScissorRect){ (NSUInteger)l, (NSUInteger)tp, (NSUInteger)(rr - l), (NSUInteger)(b - tp) } : (MTLScissorRect){ 0, 0, 0, 0 };
        }
        if (!ns) { scs[0] = (MTLScissorRect){ 0, 0, 0, 0 }; ns = 1; }   // D3D12: no scissor set = nothing drawn
        [e setScissorRects:scs count:ns];
        r->dirty_vp = 0;
    }
    if (r->dirty_misc) {
        [e setBlendColorRed:r->blend[0] green:r->blend[1] blue:r->blend[2] alpha:r->blend[3]];
        [e setStencilReferenceValue:r->stencil_ref];
        r->dirty_misc = 0;
    }
    if (r->dirty_vb) {
        for (u32 i = 0; i < D3D12MTL_MAX_VB; i++) {
            if (!r->vb[i].BufferLocation) continue;
            BufRef b = va_ref(r->dev, r->vb[i].BufferLocation);
            if (b.buf) [e setVertexBuffer:b.buf offset:b.off attributeStride:r->vb[i].StrideInBytes atIndex:D3D12MTL_BUF_VB0 + i];
        }
        r->dirty_vb = 0;
    }
    if (r->dirty_root[0]) {
        bind_root(r, 0, 0);
        bind_root(r, 0, 1);
        r->dirty_root[0] = 0;
    }
}
static void copy_texture(Replay *r, const CCopyTex *c) {
    MResource *dst = (MResource *)c->dst.pResource, *src = (MResource *)c->src.pResource;
    id<MTLBlitCommandEncoder> b = blit(r);
    if (c->dst.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && c->src.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        // buffer -> texture
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *f = &c->src.PlacedFootprint;
        u32 mips = dst->desc.MipLevels, sub = c->dst.SubresourceIndex, level = sub % mips, slice = sub / mips;
        const Fmt *ff = fmt(f->Footprint.Format); u32 blk = ff ? ff->block : 1;
        u32 x0 = 0, y0 = 0, z0 = 0, w = f->Footprint.Width, h = f->Footprint.Height, dd = f->Footprint.Depth;
        if (c->has_box) { x0 = c->box.left; y0 = c->box.top; z0 = c->box.front; w = c->box.right - x0; h = c->box.bottom - y0; dd = c->box.back - z0; }
        u64 rowp = f->Footprint.RowPitch, rows = (f->Footprint.Height + blk - 1) / blk;
        u64 off = f->Offset + (u64)z0 * rowp * rows + (u64)(y0 / blk) * rowp + (u64)(x0 / blk) * (ff ? ff->bytes : 4);
        [b copyFromBuffer:src->buf sourceOffset:off sourceBytesPerRow:rowp sourceBytesPerImage:rowp * rows sourceSize:MTLSizeMake(w, h, dd)
                toTexture:dst->tex destinationSlice:dst->tex.textureType == MTLTextureType3D ? 0 : slice destinationLevel:level
                destinationOrigin:MTLOriginMake(c->x, c->y, dst->tex.textureType == MTLTextureType3D ? c->z : 0)];
    } else if (c->dst.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && c->src.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        // texture -> buffer
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *f = &c->dst.PlacedFootprint;
        u32 mips = src->desc.MipLevels, sub = c->src.SubresourceIndex, level = sub % mips, slice = sub / mips;
        const Fmt *ff = fmt(f->Footprint.Format); u32 blk = ff ? ff->block : 1;
        u32 x0 = 0, y0 = 0, z0 = 0, w = mip_dim(src->desc.Width, level), h = mip_dim(src->desc.Height, level), dd = 1;
        if (c->has_box) { x0 = c->box.left; y0 = c->box.top; z0 = c->box.front; w = c->box.right - x0; h = c->box.bottom - y0; dd = c->box.back - z0; }
        u64 rowp = f->Footprint.RowPitch, rows = (f->Footprint.Height + blk - 1) / blk;
        u64 off = f->Offset + (u64)c->z * rowp * rows + (u64)(c->y / blk) * rowp + (u64)(c->x / blk) * (ff ? ff->bytes : 4);
        [b copyFromTexture:src->tex sourceSlice:src->tex.textureType == MTLTextureType3D ? 0 : slice sourceLevel:level
              sourceOrigin:MTLOriginMake(x0, y0, z0) sourceSize:MTLSizeMake(w, h, dd)
                  toBuffer:dst->buf destinationOffset:off destinationBytesPerRow:rowp destinationBytesPerImage:rowp * rows];
    } else if (c->dst.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && c->src.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        u32 smips = src->desc.MipLevels, dmips = dst->desc.MipLevels;
        u32 sl = c->src.SubresourceIndex % smips, ss = c->src.SubresourceIndex / smips;
        u32 dl = c->dst.SubresourceIndex % dmips, ds = c->dst.SubresourceIndex / dmips;
        u32 x0 = 0, y0 = 0, z0 = 0, w = mip_dim(src->desc.Width, sl), h = mip_dim(src->desc.Height, sl), dd = 1;
        if (c->has_box) { x0 = c->box.left; y0 = c->box.top; z0 = c->box.front; w = c->box.right - x0; h = c->box.bottom - y0; dd = c->box.back - z0; }
        [b copyFromTexture:src->tex sourceSlice:ss sourceLevel:sl sourceOrigin:MTLOriginMake(x0, y0, z0) sourceSize:MTLSizeMake(w, h, dd)
                 toTexture:dst->tex destinationSlice:ds destinationLevel:dl destinationOrigin:MTLOriginMake(c->x, c->y, c->z)];
    } else mtl_log_once("metal backend: CopyTextureRegion between two footprints");
}
static void replay(Replay *r, MList *l) {
    for (size_t pos = 0; pos < l->ncmd; ) {
        CmdHdr *h = (CmdHdr *)(l->cmd + pos);
        void *a = h + 1;
        pos += h->size;
        switch (h->op) {
        case C_PSO: r->pso = *(MPso **)a; break;
        case C_RS: { CRs *c = a; if (r->rs[c->compute] != c->rs) { r->rs[c->compute] = c->rs; memset(r->params[c->compute], 0, sizeof r->params[0]); r->dirty_root[c->compute] = 1; } break; }
        case C_ROOT_CONST: {
            CRootConst *c = a; MRootSig *rs = r->rs[c->compute];
            if (rs && c->index < rs->layout.count) memcpy(r->root[c->compute] + rs->layout.param[c->index].offset + 4 * c->offset, c->data, 4 * c->count);
            r->dirty_root[c->compute] = 1;
            break;
        }
        case C_ROOT_TABLE: case C_ROOT_VA: {
            CRootVal *c = a; MRootSig *rs = r->rs[c->compute];
            if (!rs || c->index >= rs->layout.count) break;
            memcpy(r->root[c->compute] + rs->layout.param[c->index].offset, &c->handle, 8);   // MSC-style top-level layout
            r->params[c->compute][c->index] = h->op == C_ROOT_TABLE ? table_ref(r->dev, c->handle) : va_ref(r->dev, c->handle);
            r->dirty_root[c->compute] = 1;
            break;
        }
        case C_VIEWPORTS: r->vp = *(CViewports *)a; r->dirty_vp = 1; break;
        case C_SCISSORS: r->sc = *(CScissors *)a; r->dirty_vp = 1; break;
        case C_TOPOLOGY: r->topo = *(u32 *)a; break;
        case C_BLEND: memcpy(r->blend, a, 16); r->dirty_misc = 1; break;
        case C_STENCIL_REF: r->stencil_ref = *(u32 *)a; r->dirty_misc = 1; break;
        case C_VB: { CVb *c = a; for (u32 i = 0; i < c->n && c->start + i < D3D12MTL_MAX_VB; i++) r->vb[c->start + i] = c->v[i]; r->dirty_vb = 1; break; }
        case C_IB: r->ib = *(D3D12_INDEX_BUFFER_VIEW *)a; break;
        case C_RTS: r->rts = *(CRts *)a; r->pass_dirty = 1; break;
        case C_CLEAR_RTV: {
            CClear *c = a;
            if (r->re) { end_encoders(r); r->pass_dirty = 1; }   // clears inside a pass: start a new one
            for (int k = 0; k < r->nclears; k++) if (same_view(&r->clears[k].rt, &c->rt)) { r->clears[k] = r->clears[--r->nclears]; k--; }
            if (r->nclears == 16) flush_clears(r, NULL);
            r->clears[r->nclears++] = *c;
            break;
        }
        case C_CLEAR_DSV: {
            CClear *c = a;
            if (r->re) { end_encoders(r); r->pass_dirty = 1; }
            if (r->has_dclear && !same_view(&r->dclear.rt, &c->rt)) flush_clears(r, NULL);
            r->dclear = *c; r->has_dclear = 1;
            break;
        }
        case C_DRAW: case C_DRAW_INDEXED: {
            CDraw *c = a;
            if (!r->pso || !r->pso->rps) break;
            prepare_draw(r);
            r->draws++;
            if (h->op == C_DRAW)
                [r->re drawPrimitives:prim(r->topo) vertexStart:c->c vertexCount:c->a instanceCount:c->b baseInstance:c->e];
            else {
                BufRef ib = va_ref(r->dev, r->ib.BufferLocation);
                if (!ib.buf) break;
                int wide = r->ib.Format == DXGI_FORMAT_R32_UINT;
                [r->re drawIndexedPrimitives:prim(r->topo) indexCount:c->a indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                 indexBuffer:ib.buf indexBufferOffset:ib.off + (u64)c->c * (wide ? 4 : 2) instanceCount:c->b
                                  baseVertex:(int)c->d baseInstance:c->e];
            }
            break;
        }
        case C_DISPATCH: {
            CDraw *c = a;
            if (!r->pso || !r->pso->cps) break;
            if (!r->ce) { end_encoders(r); flush_clears(r, NULL); r->pass_dirty = 1; r->ce = [r->cb computeCommandEncoder]; }
            [r->ce setComputePipelineState:r->pso->cps];
            bind_root(r, 1, 2);
            [r->ce dispatchThreadgroups:MTLSizeMake(c->a, c->b, c->c) threadsPerThreadgroup:r->pso->tg];
            break;
        }
        case C_COPY_BUF: {
            CCopyBuf *c = a;
            [blit(r) copyFromBuffer:c->src->buf sourceOffset:c->src_off toBuffer:c->dst->buf destinationOffset:c->dst_off size:c->size];
            break;
        }
        case C_COPY_RES: {
            CCopyBuf *c = a;
            if (c->dst->buf && c->src->buf) [blit(r) copyFromBuffer:c->src->buf sourceOffset:0 toBuffer:c->dst->buf destinationOffset:0 size:c->src->desc.Width];
            else if (c->dst->tex && c->src->tex) [blit(r) copyFromTexture:c->src->tex toTexture:c->dst->tex];
            break;
        }
        case C_COPY_TEX: copy_texture(r, a); break;
        }
    }
}

// ---- command queue ----------------------------------------------------------------------------------------------
static void commit_residency(MDevice *d) {
    pthread_mutex_lock(&d->lock);
    if (d->residency_dirty) { [d->residency commit]; d->residency_dirty = 0; }
    pthread_mutex_unlock(&d->lock);
}
static double stat_ms; static u64 stat_lists, stat_draws;
static void STDMETHODCALLTYPE queue_execute(ID3D12CommandQueue *self, UINT n, ID3D12CommandList *const *lists) {
    MQueue *q = (MQueue *)self;
    double t0 = now_ms();
    commit_residency(q->dev);
    @autoreleasepool {
        Replay *r = calloc(1, sizeof *r);
        r->dev = q->dev;
        r->cb = [q->q commandBuffer];
        for (UINT i = 0; i < n; i++) {
            MList *l = (MList *)lists[i];
            // D3D12 state does not carry over between command lists
            id<MTLCommandBuffer> cb = r->cb;
            memset(r, 0, sizeof *r); r->dev = q->dev; r->cb = cb; r->blend[0] = r->blend[1] = r->blend[2] = r->blend[3] = 1;
            if (l->kind == K_LIST) replay(r, l);
            end_encoders(r);
            flush_clears(r, NULL);
            end_encoders(r);
            stat_draws += r->draws;
        }
        [r->cb commit];
        free(r);
    }
    stat_lists += n; stat_ms += now_ms() - t0;
}
static HRESULT STDMETHODCALLTYPE queue_signal(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 v) {
    MQueue *q = (MQueue *)self; MFence *f = (MFence *)fence;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [q->q commandBufferWithUnretainedReferences];
        [cb encodeSignalEvent:f->ev value:v];
        [cb commit];
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE queue_wait(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 v) {
    MQueue *q = (MQueue *)self; MFence *f = (MFence *)fence;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [q->q commandBufferWithUnretainedReferences];
        [cb encodeWaitForEvent:f->ev value:v];
        [cb commit];
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE queue_ts_freq(ID3D12CommandQueue *self, UINT64 *f) { if (f) *f = 1000000000ull; return S_OK; }
static D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE queue_desc(ID3D12CommandQueue *self, D3D12_COMMAND_QUEUE_DESC *ret) { *ret = ((MQueue *)self)->desc; return ret; }
static void STDMETHODCALLTYPE queue_marker(ID3D12CommandQueue *self, UINT m, const void *d, UINT s) {}
static void STDMETHODCALLTYPE queue_end(ID3D12CommandQueue *self) {}
static ID3D12CommandQueueVtbl queue_vtbl = { CHILD_METHODS, .ExecuteCommandLists = queue_execute, .Signal = queue_signal,
    .Wait = queue_wait, .GetTimestampFrequency = queue_ts_freq, .GetDesc = queue_desc, .SetMarker = queue_marker,
    .BeginEvent = queue_marker, .EndEvent = queue_end };

// ---- device methods ---------------------------------------------------------------------------------------------
static UINT STDMETHODCALLTYPE dev_node_count(ID3D12Device9 *self) { return 1; }
static HRESULT STDMETHODCALLTYPE dev_create_queue(ID3D12Device9 *self, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **out) {
    MDevice *d = (MDevice *)self;
    MQueue *q = calloc(1, sizeof *q);
    head_init(q, &queue_vtbl, K_QUEUE, d);
    q->desc = *desc; q->q = [d->mtl newCommandQueueWithMaxCommandBufferCount:256];
    [q->q addResidencySet:d->residency];
    HRESULT hr = obj_qi(q, riid, out); obj_release(q);
    return hr;
}
static HRESULT STDMETHODCALLTYPE dev_create_queue1(ID3D12Device9 *self, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID creator, REFIID riid, void **out) {
    return dev_create_queue(self, desc, riid, out);
}
static HRESULT STDMETHODCALLTYPE dev_create_alloc(ID3D12Device9 *self, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out) {
    MAlloc *a = calloc(1, sizeof *a);
    head_init(a, &alloc_vtbl, K_ALLOC, (MDevice *)self); a->type = type;
    HRESULT hr = obj_qi(a, riid, out); obj_release(a);
    return hr;
}
static HRESULT STDMETHODCALLTYPE dev_create_list(ID3D12Device9 *self, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *alloc,
                                                 ID3D12PipelineState *pso, REFIID riid, void **out) {
    MList *l = calloc(1, sizeof *l);
    head_init(l, &list_vtbl, K_LIST, (MDevice *)self); l->type = type;
    list_reset((ID3D12GraphicsCommandList6 *)l, alloc, pso);
    HRESULT hr = obj_qi(l, riid, out); obj_release(l);
    return hr;
}
static HRESULT STDMETHODCALLTYPE dev_create_list1(ID3D12Device9 *self, UINT mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
                                                  REFIID riid, void **out) {
    HRESULT hr = dev_create_list(self, mask, type, NULL, NULL, riid, out);
    if (SUCCEEDED(hr)) ((MList *)*out)->open = 0;   // created closed
    return hr;
}
static HRESULT STDMETHODCALLTYPE dev_create_gpso(ID3D12Device9 *self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *g, REFIID riid, void **out) {
    if (out) *out = NULL;
    @autoreleasepool { return create_graphics_pso((MDevice *)self, g, riid, out); }
}
static HRESULT STDMETHODCALLTYPE dev_create_cpso(ID3D12Device9 *self, const D3D12_COMPUTE_PIPELINE_STATE_DESC *c, REFIID riid, void **out) {
    MDevice *d = (MDevice *)self; MRootSig *rs = (MRootSig *)c->pRootSignature;
    if (out) *out = NULL;
    if (!rs) return E_INVALIDARG_;
    char err[1024];
    @autoreleasepool {
        id<MTLFunction> fn = make_function(d, rs, VKD3D_SHADER_VISIBILITY_COMPUTE, &c->CS, NULL, 0, err, sizeof err);
        if (!fn) { rt_log("d3d12 (metal): compute shader: %s", err); return E_INVALIDARG_; }
        NSError *e = nil;
        id<MTLComputePipelineState> cps = [d->mtl newComputePipelineStateWithFunction:fn error:&e];
        [fn release];
        if (!cps) { rt_log("d3d12 (metal): compute pipeline: %s", e.localizedDescription.UTF8String); return E_INVALIDARG_; }
        MPso *p = calloc(1, sizeof *p);
        head_init(p, &pso_vtbl, K_PSO, d);
        p->rs = rs; obj_addref(rs); p->compute = 1; p->cps = cps;
        // threadgroup size: [numthreads] from the shader (vkd3d-shader's D3D asm has dcl_thread_group / DXIL metadata)
        p->tg = MTLSizeMake(1, 1, 1);
        struct vkd3d_shader_compile_info ci = { VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO };
        ci.source.code = c->CS.pShaderBytecode; ci.source.size = c->CS.BytecodeLength;
        ci.source_type = d3d12mtl_is_dxil(ci.source.code, ci.source.size) ? VKD3D_SHADER_SOURCE_DXBC_DXIL : VKD3D_SHADER_SOURCE_DXBC_TPF;
        ci.target_type = VKD3D_SHADER_TARGET_D3D_ASM; ci.log_level = VKD3D_SHADER_LOG_NONE;
        struct vkd3d_shader_code asm_ = { 0 };
        if (vkd3d_shader_compile(&ci, &asm_, NULL) >= 0) {
            const char *s = strstr(asm_.code, "dcl_thread_group ");
            unsigned x, y, z;
            if (s && sscanf(s + 17, "%u, %u, %u", &x, &y, &z) == 3) p->tg = MTLSizeMake(x, y, z);
            vkd3d_shader_free_shader_code(&asm_);
        }
        HRESULT hr = obj_qi(p, riid, out); obj_release(p);
        return hr;
    }
}
static HRESULT STDMETHODCALLTYPE dev_check_feature(ID3D12Device9 *self, D3D12_FEATURE f, void *data, UINT size) {
    MDevice *d = (MDevice *)self;
    switch (f) {
    case D3D12_FEATURE_D3D12_OPTIONS: {
        D3D12_FEATURE_DATA_D3D12_OPTIONS *o = data; if (size != sizeof *o) return E_INVALIDARG_;
        memset(o, 0, sizeof *o);
        o->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_2; o->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
        o->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2; o->TypedUAVLoadAdditionalFormats = TRUE;
        o->MaxGPUVirtualAddressBitsPerResource = 40;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 *o = data; if (size != sizeof *o) return E_INVALIDARG_;
        memset(o, 0, sizeof *o); o->WaveOps = TRUE; o->WaveLaneCountMin = o->WaveLaneCountMax = 32; o->TotalLaneCount = 32 * 1024;
        o->Int64ShaderOps = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE: {
        D3D12_FEATURE_DATA_ARCHITECTURE *a = data; if (size != sizeof *a) return E_INVALIDARG_;
        a->TileBasedRenderer = TRUE; a->UMA = TRUE; a->CacheCoherentUMA = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_FEATURE_LEVELS: {
        D3D12_FEATURE_DATA_FEATURE_LEVELS *l = data; if (size != sizeof *l) return E_INVALIDARG_;
        l->MaxSupportedFeatureLevel = 0;
        for (UINT i = 0; i < l->NumFeatureLevels; i++)
            if (l->pFeatureLevelsRequested[i] <= D3D_FEATURE_LEVEL_12_0 && l->pFeatureLevelsRequested[i] > l->MaxSupportedFeatureLevel)
                l->MaxSupportedFeatureLevel = l->pFeatureLevelsRequested[i];
        return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
        D3D12_FEATURE_DATA_SHADER_MODEL *m = data; if (size != sizeof *m) return E_INVALIDARG_;
        if (m->HighestShaderModel > D3D_SHADER_MODEL_6_0) m->HighestShaderModel = D3D_SHADER_MODEL_6_0;   // vkd3d-shader's DXIL coverage
        return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE *r = data; if (size != sizeof *r) return E_INVALIDARG_;
        if (r->HighestVersion > D3D_ROOT_SIGNATURE_VERSION_1_1) r->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT *s = data; if (size != sizeof *s) return E_INVALIDARG_;
        const Fmt *x = fmt(s->Format);
        if (!x) { s->Support1 = 0; s->Support2 = 0; return E_FAIL_; }
        s->Support1 = D3D12_FORMAT_SUPPORT1_BUFFER | D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
        if (x->vtx) s->Support1 |= D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER;
        if (x->mtl && x->block == 1) s->Support1 |= is_depth(x->mtl) ? D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL : D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE;
        s->Support2 = 0;
        return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *m = data; if (size != sizeof *m) return E_INVALIDARG_;
        m->NumQualityLevels = [d->mtl supportsTextureSampleCount:m->SampleCount] ? 1 : 0;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS2: case D3D12_FEATURE_D3D12_OPTIONS3: case D3D12_FEATURE_D3D12_OPTIONS4:
    case D3D12_FEATURE_D3D12_OPTIONS5: case D3D12_FEATURE_D3D12_OPTIONS6: case D3D12_FEATURE_D3D12_OPTIONS7:
        memset(data, 0, size);   // conservative: no new optional features
        return S_OK;
    default: {
        char m[96]; snprintf(m, sizeof m, "metal backend: CheckFeatureSupport(%u)", f); mtl_log_once(m);
        return E_INVALIDARG_;
    }
    }
}
static HRESULT STDMETHODCALLTYPE dev_create_heap(ID3D12Device9 *self, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **out) {
    MDevice *d = (MDevice *)self;
    MHeap *h = calloc(1, sizeof *h);
    head_init(h, &heap_vtbl, K_DESCHEAP, d);
    h->desc = *desc; h->stride = heap_stride(desc->Type);
    size_t bytes = (size_t)desc->NumDescriptors * h->stride;
    int gpu = (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
           && (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    if (gpu) {
        h->buf = [d->mtl newBufferWithLength:bytes ? bytes : 16 options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        h->cpu = h->buf.contents; h->gpu = h->buf.gpuAddress;
        memset(h->cpu, 0, bytes);
    } else h->cpu = calloc(1, bytes ? bytes : 16);
    h->objs = calloc(desc->NumDescriptors ? desc->NumDescriptors : 1, sizeof(id));
    reg_heap(d, h);
    HRESULT hr = obj_qi(h, riid, out); obj_release(h);
    return hr;
}
static UINT STDMETHODCALLTYPE dev_increment(ID3D12Device9 *self, D3D12_DESCRIPTOR_HEAP_TYPE t) { return heap_stride(t); }
static HRESULT STDMETHODCALLTYPE dev_create_rs(ID3D12Device9 *self, UINT mask, const void *code, SIZE_T len, REFIID riid, void **out) {
    MDevice *d = (MDevice *)self;
    struct vkd3d_shader_versioned_root_signature_desc v = { 0 };
    char *msg = NULL;
    struct vkd3d_shader_code dxbc = { code, len };
    if (vkd3d_shader_parse_root_signature(&dxbc, &v, &msg) < 0) { vkd3d_shader_free_messages(msg); return E_INVALIDARG_; }
    vkd3d_shader_free_messages(msg);
    MRootSig *rs = calloc(1, sizeof *rs);
    head_init(rs, &rs_vtbl, K_ROOTSIG, d);
    if (v.version == VKD3D_SHADER_ROOT_SIGNATURE_VERSION_1_1) rs->desc = v;
    else { vkd3d_shader_convert_root_signature(&rs->desc, VKD3D_SHADER_ROOT_SIGNATURE_VERSION_1_1, &v); vkd3d_shader_free_root_signature(&v); }
    char err[160];
    if (!d3d12mtl_root_layout(&rs->desc.u.v_1_1, &rs->layout, err, sizeof err)) {
        rt_log("d3d12 (metal): root signature: %s", err);
        obj_release(rs); return E_INVALIDARG_;
    }
    HRESULT hr = obj_qi(rs, riid, out); obj_release(rs);
    return hr;
}
static void STDMETHODCALLTYPE dev_cbv(ID3D12Device9 *self, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    put_entry((MDevice *)self, h, desc ? desc->BufferLocation : 0, nil, 0, desc ? desc->SizeInBytes : 0);
}
static void STDMETHODCALLTYPE dev_srv(ID3D12Device9 *self, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    MDevice *d = (MDevice *)self; MResource *r = (MResource *)res;
    if (!r) { put_entry(d, h, 0, nil, 0, 0); return; }   // null descriptor
    @autoreleasepool {
        if (r->buf) {
            // buffer SRV: address in slot 0; a texture-buffer view in slot 1 (typed: its format; raw/structured: R32Uint,
            // which is how vkd3d-shader reads them)
            u64 first = desc ? desc->Buffer.FirstElement : 0, n = desc ? desc->Buffer.NumElements : r->desc.Width / 4;
            u32 stride = desc && desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride : 4;
            MTLPixelFormat pf = desc && desc->Format ? pixfmt(desc->Format) : MTLPixelFormatR32Uint;
            const Fmt *ff = desc && desc->Format ? fmt(desc->Format) : NULL;
            u32 esize = ff ? ff->bytes : stride;
            u64 off = first * esize, words = (n * esize) / (pf == MTLPixelFormatR32Uint ? 4 : esize);
            id<MTLTexture> tv = nil;
            if (pf && words) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pf width:words
                                                    resourceOptions:r->buf.resourceOptions usage:MTLTextureUsageShaderRead];
                tv = [[r->buf newTextureWithDescriptor:td offset:off bytesPerRow:words * (pf == MTLPixelFormatR32Uint ? 4 : esize)] autorelease];
            }
            put_entry(d, h, r->va + off, tv, 1, n * esize);
            return;
        }
        MTLPixelFormat pf = view_fmt(desc ? desc->Format : r->desc.Format, r->pf);
        MTLTextureType tt = r->tex.textureType; NSRange levels = NSMakeRange(0, r->desc.MipLevels), slices = NSMakeRange(0, r->tex.arrayLength);
        if (desc) switch (desc->ViewDimension) {
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            tt = MTLTextureType2D; levels = NSMakeRange(desc->Texture2D.MostDetailedMip, desc->Texture2D.MipLevels == ~0u ? r->desc.MipLevels - desc->Texture2D.MostDetailedMip : desc->Texture2D.MipLevels);
            slices = NSMakeRange(0, 1); break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            tt = MTLTextureType2DArray; levels = NSMakeRange(desc->Texture2DArray.MostDetailedMip, desc->Texture2DArray.MipLevels == ~0u ? r->desc.MipLevels - desc->Texture2DArray.MostDetailedMip : desc->Texture2DArray.MipLevels);
            slices = NSMakeRange(desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize); break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE: tt = MTLTextureTypeCube; slices = NSMakeRange(0, 6); break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: tt = MTLTextureTypeCubeArray; break;
        case D3D12_SRV_DIMENSION_TEXTURE3D: tt = MTLTextureType3D; slices = NSMakeRange(0, 1); break;
        default: break;
        }
        id<MTLTexture> v = [[r->tex newTextureViewWithPixelFormat:pf textureType:tt levels:levels slices:slices] autorelease];
        put_entry(d, h, 0, v, 1, 0);
    }
}
static void STDMETHODCALLTYPE dev_uav(ID3D12Device9 *self, ID3D12Resource *res, ID3D12Resource *counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
                                      D3D12_CPU_DESCRIPTOR_HANDLE h) {
    MDevice *d = (MDevice *)self; MResource *r = (MResource *)res;
    if (counter) mtl_log_once("metal backend: UAV counters");
    if (!r) { put_entry(d, h, 0, nil, 0, 0); return; }
    @autoreleasepool {
        if (r->buf) {
            u64 first = desc ? desc->Buffer.FirstElement : 0, n = desc ? desc->Buffer.NumElements : r->desc.Width / 4;
            u32 stride = desc && desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride : 4;
            const Fmt *ff = desc && desc->Format ? fmt(desc->Format) : NULL;
            u32 esize = ff ? ff->bytes : stride;
            MTLPixelFormat pf = ff ? ff->mtl : MTLPixelFormatR32Uint;
            u64 off = first * esize, words = (n * esize) / (pf == MTLPixelFormatR32Uint ? 4 : esize);
            id<MTLTexture> tv = nil;
            if (pf && words) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pf width:words
                                                    resourceOptions:r->buf.resourceOptions usage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
                tv = [[r->buf newTextureWithDescriptor:td offset:off bytesPerRow:words * (pf == MTLPixelFormatR32Uint ? 4 : esize)] autorelease];
            }
            put_entry(d, h, r->va + off, tv, 1, n * esize);
            return;
        }
        u32 level = desc && desc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D ? desc->Texture2D.MipSlice : 0;
        id<MTLTexture> v = [[r->tex newTextureViewWithPixelFormat:view_fmt(desc ? desc->Format : r->desc.Format, r->pf) textureType:r->tex.textureType
                                                           levels:NSMakeRange(level, 1) slices:NSMakeRange(0, r->tex.arrayLength)] autorelease];
        put_entry(d, h, 0, v, 1, 0);
    }
}
static void rtv_common(MResource *r, u32 level, u32 slice, DXGI_FORMAT f, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    MRtv *v = (MRtv *)h.ptr;
    memset(v, 0, sizeof *v);
    if (!r || !r->tex) return;
    v->res = r; v->tex = r->tex; v->level = level; v->slice = slice; v->pf = r->pf; v->valid = 1;
    MTLPixelFormat pf = f ? pixfmt(f) : r->pf;
    if (pf && pf != r->pf) mtl_log_once("metal backend: RTV/DSV format differs from the resource (view not created)");
}
static void STDMETHODCALLTYPE dev_rtv(ID3D12Device9 *self, ID3D12Resource *res, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    u32 level = 0, slice = 0;
    if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) level = desc->Texture2D.MipSlice;
    if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) { level = desc->Texture2DArray.MipSlice; slice = desc->Texture2DArray.FirstArraySlice; }
    rtv_common((MResource *)res, level, slice, desc ? desc->Format : 0, h);
}
static void STDMETHODCALLTYPE dev_dsv(ID3D12Device9 *self, ID3D12Resource *res, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    u32 level = desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D ? desc->Texture2D.MipSlice : 0;
    rtv_common((MResource *)res, level, 0, desc ? desc->Format : 0, h);
}
static MTLSamplerAddressMode sam_addr(D3D12_TEXTURE_ADDRESS_MODE m) {
    switch (m) {
    case D3D12_TEXTURE_ADDRESS_MODE_WRAP: return MTLSamplerAddressModeRepeat;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return MTLSamplerAddressModeMirrorRepeat;
    case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return MTLSamplerAddressModeClampToBorderColor;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeClampToEdge;
    }
}
static void STDMETHODCALLTYPE dev_sampler(ID3D12Device9 *self, const D3D12_SAMPLER_DESC *s, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    MDevice *d = (MDevice *)self;
    @autoreleasepool {
        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor new] autorelease];
        unsigned f = s->Filter;
        sd.mipFilter = (f & 0x1) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        sd.magFilter = (f & 0x4) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.minFilter = (f & 0x10) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        if (f & 0x40) { sd.maxAnisotropy = s->MaxAnisotropy ? s->MaxAnisotropy : 1; sd.mipFilter = MTLSamplerMipFilterLinear; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; }
        if (f & 0x80) sd.compareFunction = compare(s->ComparisonFunc);
        sd.sAddressMode = sam_addr(s->AddressU); sd.tAddressMode = sam_addr(s->AddressV); sd.rAddressMode = sam_addr(s->AddressW);
        sd.lodMinClamp = s->MinLOD; sd.lodMaxClamp = s->MaxLOD;
        const float *b = s->BorderColor;
        sd.borderColor = b[3] == 0 ? MTLSamplerBorderColorTransparentBlack : b[0] >= 0.5f ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
        sd.supportArgumentBuffers = YES;
        if (s->MipLODBias != 0) mtl_log_once("metal backend: sampler MipLODBias (no per-sampler bias in Metal; MSC passes it in the entry metadata)");
        id<MTLSamplerState> ss = [[d->mtl newSamplerStateWithDescriptor:sd] autorelease];
        u32 bias; memcpy(&bias, &s->MipLODBias, 4);
        put_entry(d, h, 0, ss, 0, bias);
    }
}
static void copy_descs(MDevice *d, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, u32 n, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    u32 stride = heap_stride(t);
    memmove((void *)dst.ptr, (const void *)src.ptr, (size_t)n * stride);
    if (stride != D3D12MTL_ENTRY_SIZE) return;
    u32 di, si; MHeap *dh = find_heap(d, dst.ptr, 0, &di), *sh = find_heap(d, src.ptr, 0, &si);
    if (!dh || !sh) return;
    for (u32 i = 0; i < n; i++) { id o = [sh->objs[si + i] retain]; [dh->objs[di + i] release]; dh->objs[di + i] = o; }
}
static void STDMETHODCALLTYPE dev_copy_descs_simple(ID3D12Device9 *self, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE dst, const D3D12_CPU_DESCRIPTOR_HANDLE src,
                                                    D3D12_DESCRIPTOR_HEAP_TYPE t) {
    copy_descs((MDevice *)self, dst, src, n, t);
}
static void STDMETHODCALLTYPE dev_copy_descs(ID3D12Device9 *self, UINT ndst, const D3D12_CPU_DESCRIPTOR_HANDLE *dst, const UINT *dsize, UINT nsrc,
                                             const D3D12_CPU_DESCRIPTOR_HANDLE *src, const UINT *ssize, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    u32 stride = heap_stride(t), di = 0, doff = 0, si = 0, soff = 0;
    while (di < ndst && si < nsrc) {
        u32 dn = dsize ? dsize[di] : 1, sn = ssize ? ssize[si] : 1;
        u32 n = dn - doff < sn - soff ? dn - doff : sn - soff;
        copy_descs((MDevice *)self, (D3D12_CPU_DESCRIPTOR_HANDLE){ dst[di].ptr + doff * stride }, (D3D12_CPU_DESCRIPTOR_HANDLE){ src[si].ptr + soff * stride }, n, t);
        doff += n; soff += n;
        if (doff == dn) { di++; doff = 0; }
        if (soff == sn) { si++; soff = 0; }
    }
}
static void footprints(const D3D12_RESOURCE_DESC *desc, UINT first, UINT n, UINT64 base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
                       UINT *rows, UINT64 *row_size, UINT64 *total) {
    u64 off = base, end = 0;
    const Fmt *ff = fmt(desc->Format);
    u32 mips = desc->MipLevels ? desc->MipLevels : max_mips(desc);
    for (UINT i = 0; i < n; i++) {
        UINT sub = first + i;
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            if (layouts) { layouts[i].Offset = off; layouts[i].Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_UNKNOWN, (UINT)desc->Width, 1, 1, (UINT)((desc->Width + 255) & ~255ull) }; }
            if (rows) rows[i] = 1;
            if (row_size) row_size[i] = desc->Width;
            end = off + desc->Width; continue;
        }
        u32 level = sub % mips, blk = ff ? ff->block : 1, bytes = ff ? ff->bytes : 4;
        u32 w = mip_dim(desc->Width, level), h = mip_dim(desc->Height, level);
        u32 dd = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? mip_dim(desc->DepthOrArraySize, level) : 1;
        u32 bw = (w + blk - 1) / blk, bh = (h + blk - 1) / blk;
        u64 rs = (u64)bw * bytes, pitch = (rs + 255) & ~255ull;
        off = (off + 511) & ~511ull;
        if (layouts) { layouts[i].Offset = off; layouts[i].Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ desc->Format, w, h, dd, (UINT)pitch }; }
        if (rows) rows[i] = bh;
        if (row_size) row_size[i] = rs;
        end = off + pitch * (bh - 1) * 1 + rs + pitch * bh * (dd - 1);
        off += pitch * bh * dd;
    }
    if (total) *total = end - base;
}
static void STDMETHODCALLTYPE dev_footprints(ID3D12Device9 *self, const D3D12_RESOURCE_DESC *desc, UINT first, UINT n, UINT64 base,
                                             D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows, UINT64 *row_size, UINT64 *total) {
    footprints(desc, first, n, base, layouts, rows, row_size, total);
}
static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE dev_alloc_info(ID3D12Device9 *self, D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT mask, UINT n,
                                                                        const D3D12_RESOURCE_DESC *descs) {
    u64 size = 0;
    for (UINT i = 0; i < n; i++) {
        u64 t = 0;
        footprints(&descs[i], 0, sub_count(&descs[i]), 0, NULL, NULL, NULL, &t);
        size += (t + 65535) & ~65535ull;
    }
    ret->SizeInBytes = size; ret->Alignment = 65536;
    return ret;
}
static HRESULT STDMETHODCALLTYPE dev_create_committed(ID3D12Device9 *self, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC *desc,
                                                      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) {
    if (out) *out = NULL;
    @autoreleasepool { return create_resource((MDevice *)self, hp, hf, desc, riid, out); }
}
static HRESULT STDMETHODCALLTYPE dev_create_committed1(ID3D12Device9 *self, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC *desc,
                                                       D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *s, REFIID riid, void **out) {
    return dev_create_committed(self, hp, hf, desc, state, clear, riid, out);
}
static HRESULT STDMETHODCALLTYPE dev_create_fence(ID3D12Device9 *self, UINT64 init, D3D12_FENCE_FLAGS flags, REFIID riid, void **out) {
    MDevice *d = (MDevice *)self;
    MFence *f = calloc(1, sizeof *f);
    head_init(f, &fence_vtbl, K_FENCE, d);
    f->ev = [d->mtl newSharedEvent]; f->ev.signaledValue = init; f->flags = flags;
    HRESULT hr = obj_qi(f, riid, out); obj_release(f);
    return hr;
}
static HRESULT STDMETHODCALLTYPE dev_removed_reason(ID3D12Device9 *self) { return S_OK; }
static D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE dev_custom_heap_props(ID3D12Device9 *self, D3D12_HEAP_PROPERTIES *ret, UINT mask, D3D12_HEAP_TYPE t) {
    memset(ret, 0, sizeof *ret); ret->Type = D3D12_HEAP_TYPE_CUSTOM; ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    ret->CPUPageProperty = t == D3D12_HEAP_TYPE_DEFAULT ? D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE : t == D3D12_HEAP_TYPE_UPLOAD ? D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE : D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    return ret;
}
static LUID *STDMETHODCALLTYPE dev_luid(ID3D12Device9 *self, LUID *ret) {
    u64 id = ((MDevice *)self)->mtl.registryID;
    ret->LowPart = (u32)id; ret->HighPart = (LONG)(id >> 32);
    return ret;
}
static HRESULT STDMETHODCALLTYPE dev_set_stable_power(ID3D12Device9 *self, WINBOOL_ e) { return S_OK; }
static HRESULT STDMETHODCALLTYPE dev_make_resident(ID3D12Device9 *self, UINT n, ID3D12Pageable *const *o) { return S_OK; }
static ID3D12Device9Vtbl dev_vtbl = { OBJ_METHODS,
    .GetNodeCount = dev_node_count, .CreateCommandQueue = dev_create_queue, .CreateCommandAllocator = dev_create_alloc,
    .CreateGraphicsPipelineState = dev_create_gpso, .CreateComputePipelineState = dev_create_cpso, .CreateCommandList = dev_create_list,
    .CheckFeatureSupport = dev_check_feature, .CreateDescriptorHeap = dev_create_heap, .GetDescriptorHandleIncrementSize = dev_increment,
    .CreateRootSignature = dev_create_rs, .CreateConstantBufferView = dev_cbv, .CreateShaderResourceView = dev_srv,
    .CreateUnorderedAccessView = dev_uav, .CreateRenderTargetView = dev_rtv, .CreateDepthStencilView = dev_dsv, .CreateSampler = dev_sampler,
    .CopyDescriptors = dev_copy_descs, .CopyDescriptorsSimple = dev_copy_descs_simple, .GetResourceAllocationInfo = dev_alloc_info,
    .GetCustomHeapProperties = dev_custom_heap_props, .CreateCommittedResource = dev_create_committed, .CreateFence = dev_create_fence,
    .GetDeviceRemovedReason = dev_removed_reason, .GetCopyableFootprints = dev_footprints, .SetStablePowerState = dev_set_stable_power,
    .GetAdapterLuid = dev_luid, .MakeResident = dev_make_resident, .Evict = dev_make_resident,
    .CreateCommandList1 = dev_create_list1, .CreateCommittedResource1 = dev_create_committed1, .CreateCommandQueue1 = dev_create_queue1 };

// ---- destruction ------------------------------------------------------------------------------------------------
static void destroy(MHead *o) {
    d3d12_forget(o);   // drop the guest wrapper cached for this object
    switch (o->kind) {
    case K_RESOURCE: { MResource *r = (MResource *)o; unreg_resource(r->dev, r); [r->buf release]; [r->tex release]; break; }
    case K_DESCHEAP: {
        MHeap *h = (MHeap *)o; unreg_heap(h->dev, h);
        for (u32 i = 0; i < h->desc.NumDescriptors; i++) [h->objs[i] release];
        free(h->objs);
        if (h->buf) [h->buf release]; else free(h->cpu);
        break;
    }
    case K_ROOTSIG: vkd3d_shader_free_root_signature(&((MRootSig *)o)->desc); break;
    case K_PSO: { MPso *p = (MPso *)o; [p->rps release]; [p->dss release]; [p->cps release]; obj_release(p->rs); break; }
    case K_FENCE: [((MFence *)o)->ev release]; break;
    case K_QUEUE: [((MQueue *)o)->q release]; break;
    case K_LIST: free(((MList *)o)->cmd); break;
    case K_DEVICE: {
        MDevice *d = (MDevice *)o;
        if (getenv("WINTOMAC_D3D12_TRACE") && stat_lists)
            rt_log("d3d12 (metal): %llu command lists, %llu draws, %.3f ms CPU in ExecuteCommandLists",
                   (unsigned long long)stat_lists, (unsigned long long)stat_draws, stat_ms);
        [d->residency release]; [d->listener release]; [d->mtl release];
        free(d->res); free(d->heaps);
        break;
    }
    }
    free(o);
}
static ULONG dev_release(MDevice *d) { return obj_release(d); }

static const GUID *const iids_device[] = { &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4,
    &IID_ID3D12Device5, &IID_ID3D12Device6, &IID_ID3D12Device7, &IID_ID3D12Device8, &IID_ID3D12Device9, NULL };
static const GUID *const iids_queue[] = { &IID_ID3D12CommandQueue, &IID_ID3D12Pageable, NULL };
static const GUID *const iids_alloc[] = { &IID_ID3D12CommandAllocator, &IID_ID3D12Pageable, NULL };
static const GUID *const iids_list[] = { &IID_ID3D12CommandList, &IID_ID3D12GraphicsCommandList, &IID_ID3D12GraphicsCommandList1,
    &IID_ID3D12GraphicsCommandList2, &IID_ID3D12GraphicsCommandList3, &IID_ID3D12GraphicsCommandList4, &IID_ID3D12GraphicsCommandList5,
    &IID_ID3D12GraphicsCommandList6, NULL };
static const GUID *const iids_fence[] = { &IID_ID3D12Fence, &IID_ID3D12Fence1, &IID_ID3D12Pageable, NULL };
static const GUID *const iids_resource[] = { &IID_ID3D12Resource, &IID_ID3D12Resource1, &IID_ID3D12Resource2, &IID_ID3D12Pageable, NULL };
static const GUID *const iids_heap[] = { &IID_ID3D12DescriptorHeap, &IID_ID3D12Pageable, NULL };
static const GUID *const iids_rs[] = { &IID_ID3D12RootSignature, NULL };
static const GUID *const iids_pso[] = { &IID_ID3D12PipelineState, &IID_ID3D12Pageable, NULL };
static const GUID *const *kind_iids(int kind) {
    static const GUID *const *const t[K_NKINDS] = { iids_device, iids_queue, iids_alloc, iids_list, iids_fence, iids_resource, iids_heap, iids_rs, iids_pso };
    return t[kind];
}

// ---- presentation (runtime/d3d12_swapchain.c calls this from its presenter thread) ------------------------------
// The back buffer is drawn into the window's CAMetalLayer drawable with a full-screen triangle (CAMetalLayer has no
// RGBA8 format, and the layer may be a different size), on the game's queue, so it is ordered after its frame.
static const char present_msl[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V vs(uint i [[vertex_id]]) { V v; float2 p = float2((i << 1) & 2, i & 2); v.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); v.uv = p; return v; }\n"
    "fragment float4 ps(V v [[stage_in]], texture2d<float> t [[texture(0)]]) { constexpr sampler s(filter::linear); return t.sample(s, v.uv); }\n";
int d3d12mtl_present(void *queue, void *backbuffer, void *layer_, int sync) {
    MQueue *q = queue; MResource *bb = backbuffer; MDevice *d = q->dev;
    CAMetalLayer *layer = (CAMetalLayer *)layer_;
    static id<MTLRenderPipelineState> pipes[2]; static pthread_mutex_t plock = PTHREAD_MUTEX_INITIALIZER;
    if (!layer || !bb || !bb->tex) return 0;
    @autoreleasepool {
        int srgb = bb->pf == MTLPixelFormatRGBA8Unorm_sRGB || bb->pf == MTLPixelFormatBGRA8Unorm_sRGB;
        MTLPixelFormat lf = srgb ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
        if (layer.device != d->mtl) layer.device = d->mtl;
        if (layer.pixelFormat != lf) layer.pixelFormat = lf;
        CGSize want = CGSizeMake(bb->desc.Width, bb->desc.Height);   // the game's size; the layer scales
        if (!CGSizeEqualToSize(layer.drawableSize, want)) layer.drawableSize = want;
        layer.displaySyncEnabled = sync != 0;
        pthread_mutex_lock(&plock);
        if (!pipes[srgb]) {
            NSError *e = nil;
            id<MTLLibrary> lib = [d->mtl newLibraryWithSource:[NSString stringWithUTF8String:present_msl] options:nil error:&e];
            MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
            pd.vertexFunction = [[lib newFunctionWithName:@"vs"] autorelease]; pd.fragmentFunction = [[lib newFunctionWithName:@"ps"] autorelease];
            pd.colorAttachments[0].pixelFormat = lf;
            pipes[srgb] = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&e];
            [lib release];
        }
        pthread_mutex_unlock(&plock);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable || !pipes[srgb]) return 0;
        id<MTLCommandBuffer> cb = [q->q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:pipes[srgb]];
        [e setFragmentTexture:bb->tex atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];
    }
    return 1;
}

// ---- entry point (runtime/d3d12.c D3D12CreateDevice) ------------------------------------------------------------
int d3d12mtl_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("WINTOMAC_D3D12_BACKEND"); on = e && !strcasecmp(e, "metal"); }
    return on;
}
static void init_vtables(void) {
    fill_stubs(&dev_vtbl, sizeof dev_vtbl); fill_stubs(&queue_vtbl, sizeof queue_vtbl); fill_stubs(&alloc_vtbl, sizeof alloc_vtbl);
    fill_stubs(&list_vtbl, sizeof list_vtbl); fill_stubs(&fence_vtbl, sizeof fence_vtbl); fill_stubs(&res_vtbl, sizeof res_vtbl);
    fill_stubs(&heap_vtbl, sizeof heap_vtbl); fill_stubs(&rs_vtbl, sizeof rs_vtbl); fill_stubs(&pso_vtbl, sizeof pso_vtbl);
}
HRESULT d3d12mtl_create_device(D3D_FEATURE_LEVEL level, REFIID riid, void **out) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_vtables);
    if (level > D3D_FEATURE_LEVEL_12_0) return E_INVALIDARG_;
    id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
    if (!mtl || ![mtl supportsFamily:MTLGPUFamilyApple7] || mtl.argumentBuffersSupport < MTLArgumentBuffersTier2) {
        rt_log("d3d12 (metal): needs an Apple GPU family 7+ device with Tier 2 argument buffers");
        [mtl release]; return E_FAIL_;
    }
    if (!out) { [mtl release]; return S_FALSE_; }
    MDevice *d = calloc(1, sizeof *d);
    head_init(d, &dev_vtbl, K_DEVICE, d);
    d->mtl = mtl; d->level = level;
    pthread_mutex_init(&d->lock, NULL);
    @autoreleasepool {
        MTLResidencySetDescriptor *rd = [[MTLResidencySetDescriptor new] autorelease];
        rd.label = @"wintomac d3d12";
        NSError *e = nil;
        d->residency = [mtl newResidencySetWithDescriptor:rd error:&e];
        d->listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:dispatch_queue_create("wintomac.d3d12.fences", DISPATCH_QUEUE_SERIAL)];
    }
    if (getenv("WINTOMAC_D3D12_TRACE")) rt_log("d3d12: Metal backend (experimental) on %s", mtl.name.UTF8String);
    HRESULT hr = obj_qi(d, riid, out);
    obj_release(d);
    return hr;
}
