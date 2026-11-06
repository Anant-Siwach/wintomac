// What Unreal Engine / Unity class D3D12 renderers query at startup, headless:
//   ID3D12Device1..14, ID3D12GraphicsCommandList1..10, ID3D12Fence1, ID3D12Resource1/2, ID3D12Heap1,
//   ID3D12InfoQueue(1) and DRED objects, CreateCommittedResource1/2/3, CreatePlacedResource1/2,
//   GetResourceAllocationInfo1/2/3, CreateHeap1, CreateCommandQueue1, CreateSampler2, enhanced barriers,
//   CheckFeatureSupport answers (feature level, shader model probing, DXR / VRS / mesh shaders / tiled resources
//   reported unsupported), D3D12GetInterface / experimental features / DirectStorage absent,
//   DXGI 1.6 (IDXGIFactory6::EnumAdapterByGpuPreference, IDXGIAdapter4, IDXGIOutput6::GetDesc1 = SDR, tearing).
// dxgi.dll is DXVK's (recompiled with the test). Expected output: tests/expected/dx12_breadth.out.
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08x\n", #x, (unsigned)hr_); return 1; } } while (0)
#define SLOT(obj, i) (((void **)*(void **)(obj))[i])

// Interfaces newer than mingw's headers (Microsoft's DirectX-Headers)
DEFINE_GUID(IID_Device11, 0x5405c344, 0xd457, 0x444e, 0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39);
DEFINE_GUID(IID_Device12, 0x5af5c532, 0x4c91, 0x4cd0, 0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5);
DEFINE_GUID(IID_Device13, 0x14eecffc, 0x4df8, 0x40f7, 0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e);
DEFINE_GUID(IID_Device14, 0x5f6e592d, 0xd895, 0x44c2, 0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23);
DEFINE_GUID(IID_List8, 0xee936ef9, 0x599d, 0x4d28, 0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce, 0x51);
DEFINE_GUID(IID_List9, 0x34ed2808, 0xffe6, 0x4c2b, 0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59, 0xe1);
DEFINE_GUID(IID_List10, 0x7013c015, 0xd161, 0x4b63, 0xa0, 0x8c, 0x23, 0x85, 0x52, 0xdd, 0x8a, 0xcc);
DEFINE_GUID(IID_Heap1_, 0x572f7389, 0x2168, 0x49e3, 0x96, 0x93, 0xd6, 0xdf, 0x58, 0x71, 0xbf, 0x6d);
DEFINE_GUID(IID_DREDSettings2, 0x61552388, 0x01ab, 0x4008, 0xa4, 0x36, 0x83, 0xdb, 0x18, 0x95, 0x66, 0xea);
DEFINE_GUID(IID_DRED2, 0x67fc5816, 0xe4ca, 0x4915, 0xbf, 0x18, 0x42, 0x54, 0x12, 0x72, 0xda, 0x54);
DEFINE_GUID(CLSID_SDKConfiguration, 0x7cda6aca, 0xa03e, 0x49c8, 0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce);
DEFINE_GUID(IID_SDKConfiguration, 0xe9eb5314, 0x33aa, 0x42b2, 0xa7, 0x18, 0xd7, 0x7f, 0x58, 0xb1, 0xf1, 0xc7);
DEFINE_GUID(CLSID_D3D12DREDClass, 0x4a75bbc4, 0x9ff4, 0x4ad8, 0x9f, 0x18, 0xab, 0xae, 0x84, 0xdc, 0x5f, 0xf2);
DEFINE_GUID(IID_Unknown_IID, 0x12345678, 0x1234, 0x1234, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34);

static ID3D12Device *dev;

static void qi(IUnknown *o, const char *name, REFIID iid) {
    IUnknown *r = NULL; HRESULT hr = IUnknown_QueryInterface(o, iid, (void **)&r);
    printf("  QI %-34s %s%s\n", name, SUCCEEDED(hr) ? "ok" : "E_NOINTERFACE", SUCCEEDED(hr) && r == o ? " (same object)" : "");
    if (r) IUnknown_Release(r);
}

int main(void) {
    IDXGIFactory6 *fac6 = NULL; IDXGIFactory4 *fac;
    CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&fac));
    // ---- DXGI 1.6 -----------------------------------------------------------------------------------
    printf("DXGI:\n");
    if (SUCCEEDED(IDXGIFactory4_QueryInterface(fac, &IID_IDXGIFactory6, (void **)&fac6))) {
        IDXGIAdapter4 *a4 = NULL;
        HRESULT hr = IDXGIFactory6_EnumAdapterByGpuPreference(fac6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&a4);
        printf("  IDXGIFactory6::EnumAdapterByGpuPreference(0, high performance) = 0x%08x\n", (unsigned)hr);
        if (a4) {
            DXGI_ADAPTER_DESC3 d3; memset(&d3, 0, sizeof d3);
            printf("  IDXGIAdapter4::GetDesc3 = 0x%08x, dedicated video memory > 0: %d\n", (unsigned)IDXGIAdapter4_GetDesc3(a4, &d3), d3.DedicatedVideoMemory > 0);
            IDXGIOutput *o = NULL; IDXGIOutput6 *o6 = NULL;
            if (SUCCEEDED(IDXGIAdapter4_EnumOutputs(a4, 0, &o)) && SUCCEEDED(IDXGIOutput_QueryInterface(o, &IID_IDXGIOutput6, (void **)&o6))) {
                DXGI_OUTPUT_DESC1 od; memset(&od, 0, sizeof od);
                hr = IDXGIOutput6_GetDesc1(o6, &od);
                printf("  IDXGIOutput6::GetDesc1 = 0x%08x, colour space %u (%s), bits per colour %u\n", (unsigned)hr, od.ColorSpace,
                       od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? "SDR" : "HDR", od.BitsPerColor);
                UINT flags = 0; hr = IDXGIOutput6_CheckHardwareCompositionSupport(o6, &flags);
                printf("  IDXGIOutput6::CheckHardwareCompositionSupport = 0x%08x\n", (unsigned)hr);
                IDXGIOutput6_Release(o6);
            } else printf("  IDXGIOutput6: not available\n");
            if (o) IDXGIOutput_Release(o);
            IDXGIAdapter4_Release(a4);
        }
        BOOL tearing = 2; hr = IDXGIFactory6_CheckFeatureSupport(fac6, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
        printf("  IDXGIFactory5::CheckFeatureSupport(ALLOW_TEARING) = 0x%08x, %d\n", (unsigned)hr, tearing);
        IDXGIFactory6_Release(fac6);
    } else printf("  IDXGIFactory6: not available\n");

    // ---- loader-level entry points -------------------------------------------------------------------
    printf("d3d12.dll:\n");
    ID3D12DeviceRemovedExtendedDataSettings *dreds = NULL;
    HRESULT hr = D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings, (void **)&dreds);
    printf("  D3D12GetDebugInterface(DRED settings) = 0x%08x\n", (unsigned)hr);
    if (dreds) {
        ID3D12DeviceRemovedExtendedDataSettings_SetAutoBreadcrumbsEnablement(dreds, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings_SetPageFaultEnablement(dreds, D3D12_DRED_ENABLEMENT_FORCED_ON);
        IUnknown *s2 = NULL; hr = ID3D12DeviceRemovedExtendedDataSettings_QueryInterface(dreds, &IID_DREDSettings2, (void **)&s2);
        printf("  DRED settings: enablement accepted, QI Settings2 = 0x%08x\n", (unsigned)hr);
        if (s2) IUnknown_Release(s2);
        ID3D12DeviceRemovedExtendedDataSettings_Release(dreds);
    }
    // looked up at run time, as Agility-SDK-aware engines do (older d3d12.dll lack it)
    typedef HRESULT (WINAPI *PFN_GetInterface)(REFCLSID, REFIID, void **);
    PFN_GetInterface get_interface = (PFN_GetInterface)GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12GetInterface");
    if (!get_interface) get_interface = (PFN_GetInterface)GetProcAddress(LoadLibraryA("d3d12.dll"), "D3D12GetInterface");
    if (get_interface) {
        void *cfg = NULL;
        hr = get_interface(&CLSID_SDKConfiguration, &IID_SDKConfiguration, &cfg);
        printf("  D3D12GetInterface(SDK configuration) = 0x%08x (no Agility SDK)\n", (unsigned)hr);
        hr = get_interface(&CLSID_D3D12DREDClass, &IID_DREDSettings2, &cfg);
        printf("  D3D12GetInterface(DRED, Settings2) = 0x%08x\n", (unsigned)hr);
    } else printf("  D3D12GetInterface: not exported\n");
    UUID exp = D3D12ExperimentalShaderModels;
    printf("  D3D12EnableExperimentalFeatures(shader models) = 0x%08x\n", (unsigned)D3D12EnableExperimentalFeatures(1, &exp, NULL, NULL));
    HMODULE ds = LoadLibraryA("dstorage.dll");
    FARPROC dsf = ds ? GetProcAddress(ds, "DStorageGetFactory") : NULL;
    if (dsf) {
        void *f = (void *)1; hr = ((HRESULT (WINAPI *)(REFIID, void **))dsf)(&IID_IUnknown, &f);
        printf("  dstorage.dll: DStorageGetFactory = 0x%08x, factory %s\n", (unsigned)hr, f ? "set" : "NULL");
    } else printf("  dstorage.dll: not available\n");

    // ---- device ----------------------------------------------------------------------------------------
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    printf("device:\n");
    static const struct { const char *n; const GUID *g; } dev_iids[] = {
        { "ID3D12Device1", &IID_ID3D12Device1 }, { "ID3D12Device2", &IID_ID3D12Device2 }, { "ID3D12Device3", &IID_ID3D12Device3 },
        { "ID3D12Device4", &IID_ID3D12Device4 }, { "ID3D12Device5", &IID_ID3D12Device5 }, { "ID3D12Device6", &IID_ID3D12Device6 },
        { "ID3D12Device7", &IID_ID3D12Device7 }, { "ID3D12Device8", &IID_ID3D12Device8 }, { "ID3D12Device9", &IID_ID3D12Device9 },
        { "ID3D12Device10", &IID_ID3D12Device10 }, { "ID3D12Device11", &IID_Device11 }, { "ID3D12Device12", &IID_Device12 },
        { "ID3D12Device13", &IID_Device13 }, { "ID3D12Device14", &IID_Device14 }, { "ID3D12InfoQueue", &IID_ID3D12InfoQueue },
        { "ID3D12InfoQueue1", &IID_ID3D12InfoQueue1 }, { "ID3D12DeviceRemovedExtendedData2", &IID_DRED2 },
        { "ID3D12DebugDevice (debug layer off)", &IID_ID3D12DebugDevice }, { "unknown IID", &IID_Unknown_IID } };
    for (unsigned i = 0; i < sizeof dev_iids / sizeof *dev_iids; i++) qi((IUnknown *)dev, dev_iids[i].n, dev_iids[i].g);

    ID3D12InfoQueue1 *iq = NULL;
    if (SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_ID3D12InfoQueue1, (void **)&iq))) {
        ID3D12InfoQueue1_SetBreakOnSeverity(iq, D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        D3D12_INFO_QUEUE_FILTER filter; memset(&filter, 0, sizeof filter);
        HRESULT f = ID3D12InfoQueue1_PushStorageFilter(iq, &filter);
        DWORD cookie = 0; HRESULT r = ID3D12InfoQueue1_RegisterMessageCallback(iq, NULL, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie);
        printf("  InfoQueue1: %llu stored messages, PushStorageFilter 0x%08x, RegisterMessageCallback 0x%08x cookie %lu\n",
               (unsigned long long)ID3D12InfoQueue1_GetNumStoredMessages(iq), (unsigned)f, (unsigned)r, (unsigned long)cookie);
        ID3D12InfoQueue1_Release(iq);
    }
    ID3D12DeviceRemovedExtendedData1 *dred = NULL;
    if (SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_ID3D12DeviceRemovedExtendedData1, (void **)&dred))) {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc; memset(&bc, 0, sizeof bc);
        printf("  DRED1: GetAutoBreadcrumbsOutput1 = 0x%08x (DRED off)\n", (unsigned)ID3D12DeviceRemovedExtendedData1_GetAutoBreadcrumbsOutput1(dred, &bc));
        ID3D12DeviceRemovedExtendedData1_Release(dred);
    }
    printf("  removed reason = 0x%08x\n", (unsigned)ID3D12Device_GetDeviceRemovedReason(dev));

    // ---- CheckFeatureSupport -------------------------------------------------------------------------
    printf("features:\n");
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { 5, levels };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl);
    printf("  max feature level %x (0x%08x)\n", fl.MaxSupportedFeatureLevel, (unsigned)hr);
    ID3D12Device *d12 = NULL;
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&d12);
    printf("  D3D12CreateDevice(12_0) = 0x%08x\n", (unsigned)hr);
    if (d12) ID3D12Device_Release(d12);
    for (UINT m = 0x69; m >= 0x60; m--) {   // newest SDK first, stepping down as engines do
        D3D12_FEATURE_DATA_SHADER_MODEL sm = { (D3D_SHADER_MODEL)m };
        hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm);
        if (SUCCEEDED(hr)) { printf("  shader model: asked %x.%x -> %x.%x\n", m >> 4, m & 15, sm.HighestShaderModel >> 4, sm.HighestShaderModel & 15); break; }
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS o; memset(&o, 0, sizeof o); ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    printf("  binding tier %d, tiled resources tier %d, conservative raster tier %d, ROVs %d, typed UAV loads %d\n", o.ResourceBindingTier,
           o.TiledResourcesTier, o.ConservativeRasterizationTier, o.ROVsSupported, o.TypedUAVLoadAdditionalFormats);
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5; memset(&o5, 0, sizeof o5); ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6; memset(&o6, 0, sizeof o6); ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof o6);
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7; memset(&o7, 0, sizeof o7); ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7);
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12; memset(&o12, 0, sizeof o12); hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12);
    printf("  raytracing tier %d, VRS tier %d, mesh shader tier %d, sampler feedback tier %d, enhanced barriers %d (0x%08x)\n",
           o5.RaytracingTier, o6.VariableShadingRateTier, o7.MeshShaderTier, o7.SamplerFeedbackTier, o12.EnhancedBarriersSupported, (unsigned)hr);
    D3D12_FEATURE_DATA_ROOT_SIGNATURE rsv = { D3D_ROOT_SIGNATURE_VERSION_1_1 };
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_ROOT_SIGNATURE, &rsv, sizeof rsv);
    D3D12_FEATURE_DATA_ARCHITECTURE1 arch; memset(&arch, 0, sizeof arch);
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof arch);
    printf("  root signature 1.%d, UMA %d, cache-coherent UMA %d\n", rsv.HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_1, arch.UMA, arch.CacheCoherentUMA);

    // ---- resources & heaps ----------------------------------------------------------------------------
    printf("resources:\n");
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d1; memset(&d1, 0, sizeof d1);
    d1.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d1.Width = 64; d1.Height = 64; d1.DepthOrArraySize = 1; d1.MipLevels = 1;
    d1.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d1.SampleDesc.Count = 1; d1.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Device4 *dev4; CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device4, (void **)&dev4));
    ID3D12Device8 *dev8; CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device8, (void **)&dev8));
    ID3D12Device10 *dev10; CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device10, (void **)&dev10));
    ID3D12Resource *r1 = NULL, *r2 = NULL, *r3 = NULL, *p1 = NULL, *p2 = NULL;
    printf("  CreateCommittedResource1 = 0x%08x\n", (unsigned)ID3D12Device4_CreateCommittedResource1(dev4, &hp, D3D12_HEAP_FLAG_NONE,
           (const D3D12_RESOURCE_DESC *)&d1, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, NULL, &IID_ID3D12Resource, (void **)&r1));
    printf("  CreateCommittedResource2 = 0x%08x\n", (unsigned)ID3D12Device8_CreateCommittedResource2(dev8, &hp, D3D12_HEAP_FLAG_NONE,
           &d1, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, NULL, &IID_ID3D12Resource, (void **)&r2));
    printf("  CreateCommittedResource3 = 0x%08x\n", (unsigned)ID3D12Device10_CreateCommittedResource3(dev10, &hp, D3D12_HEAP_FLAG_NONE,
           &d1, D3D12_BARRIER_LAYOUT_RENDER_TARGET, NULL, NULL, 0, NULL, &IID_ID3D12Resource, (void **)&r3));
    D3D12_RESOURCE_ALLOCATION_INFO1 info1; memset(&info1, 0, sizeof info1);
    D3D12_RESOURCE_ALLOCATION_INFO ai = ID3D12Device4_GetResourceAllocationInfo1(dev4, 0, 1, (const D3D12_RESOURCE_DESC *)&d1, &info1);
    D3D12_RESOURCE_ALLOCATION_INFO ai2 = ID3D12Device8_GetResourceAllocationInfo2(dev8, 0, 1, &d1, NULL);
    typedef D3D12_RESOURCE_ALLOCATION_INFO *(WINAPI *GRAI3)(void *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT, const D3D12_RESOURCE_DESC1 *,
                                                            const UINT32 *, const DXGI_FORMAT *const *, D3D12_RESOURCE_ALLOCATION_INFO1 *);
    D3D12_RESOURCE_ALLOCATION_INFO ai3; void *dev12 = NULL;
    memset(&ai3, 0, sizeof ai3);
    if (SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_Device12, &dev12))) {
        ((GRAI3)SLOT(dev12, 80))(dev12, &ai3, 0, 1, &d1, NULL, NULL, NULL);
        IUnknown_Release((IUnknown *)dev12);
    }
    printf("  GetResourceAllocationInfo1/2/3: size %llu / %llu / %llu, same: %d\n", (unsigned long long)ai.SizeInBytes,
           (unsigned long long)ai2.SizeInBytes, (unsigned long long)ai3.SizeInBytes, ai.SizeInBytes == ai2.SizeInBytes && ai2.SizeInBytes == ai3.SizeInBytes);
    D3D12_HEAP_DESC hd = { ai.SizeInBytes * 2, { D3D12_HEAP_TYPE_DEFAULT }, ai.Alignment, D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES };
    ID3D12Heap *heap = NULL;
    printf("  CreateHeap1 = 0x%08x\n", (unsigned)ID3D12Device4_CreateHeap1(dev4, &hd, NULL, &IID_ID3D12Heap, (void **)&heap));
    if (heap) {
        qi((IUnknown *)heap, "ID3D12Heap1", &IID_Heap1_);
        printf("  CreatePlacedResource1 = 0x%08x\n", (unsigned)ID3D12Device8_CreatePlacedResource1(dev8, heap, 0, &d1,
               D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&p1));
        printf("  CreatePlacedResource2 = 0x%08x\n", (unsigned)ID3D12Device10_CreatePlacedResource2(dev10, heap, ai.SizeInBytes, &d1,
               D3D12_BARRIER_LAYOUT_RENDER_TARGET, NULL, 0, NULL, &IID_ID3D12Resource, (void **)&p2));
    }
    if (r3) {
        qi((IUnknown *)r3, "ID3D12Resource1", &IID_ID3D12Resource1);
        qi((IUnknown *)r3, "ID3D12Resource2", &IID_ID3D12Resource2);
        ID3D12Resource2 *rr2 = NULL;
        if (SUCCEEDED(ID3D12Resource_QueryInterface(r3, &IID_ID3D12Resource2, (void **)&rr2))) {
            D3D12_RESOURCE_DESC1 g = ID3D12Resource2_GetDesc1(rr2);
            printf("  ID3D12Resource2::GetDesc1: %llux%u format %d\n", (unsigned long long)g.Width, g.Height, g.Format);
            ID3D12Resource2_Release(rr2);
        }
    }
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0 };
    ID3D12Resource *res = NULL;
    hr = ID3D12Device10_CreateReservedResource2(dev10, &rd, D3D12_BARRIER_LAYOUT_COMMON, NULL, NULL, 0, NULL, &IID_ID3D12Resource, (void **)&res);
    printf("  CreateReservedResource2: %s\n", FAILED(hr) && !res ? "fails cleanly (no tiled resources)" : "created");
    if (res) ID3D12Resource_Release(res);
    // CreateSampler2 (ID3D12Device11, slot 79): D3D12_SAMPLER_DESC + Flags
    struct { D3D12_SAMPLER_DESC d; UINT flags; } s2; memset(&s2, 0, sizeof s2);
    s2.d.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; s2.d.AddressU = s2.d.AddressV = s2.d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    s2.d.MaxLOD = 100; UINT ones[4] = { 1, 1, 1, 1 }; memcpy(s2.d.BorderColor, ones, 16); s2.flags = 1;   // UINT border colour
    D3D12_DESCRIPTOR_HEAP_DESC smh = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1 };
    ID3D12DescriptorHeap *sh; CHECK(ID3D12Device_CreateDescriptorHeap(dev, &smh, &IID_ID3D12DescriptorHeap, (void **)&sh));
    void *dev11 = NULL;
    if (SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_Device11, &dev11))) {
        ((void (WINAPI *)(void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE))SLOT(dev11, 79))(dev11, &s2, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(sh));
        printf("  CreateSampler2 (UINT border colour): done\n");
        IUnknown_Release((IUnknown *)dev11);
    }

    // ---- queues, lists, fences --------------------------------------------------------------------
    printf("command lists:\n");
    ID3D12Device9 *dev9; CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device9, (void **)&dev9));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *q = NULL;
    hr = ID3D12Device9_CreateCommandQueue1(dev9, &qd, &IID_ID3D12CommandQueue, &IID_ID3D12CommandQueue, (void **)&q);
    printf("  CreateCommandQueue1 = 0x%08x\n", (unsigned)hr);
    if (!q) return 1;
    ID3D12CommandAllocator *alloc; CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    ID3D12GraphicsCommandList *cl; CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    static const struct { const char *n; const GUID *g; } list_iids[] = {
        { "ID3D12GraphicsCommandList1", &IID_ID3D12GraphicsCommandList1 }, { "ID3D12GraphicsCommandList2", &IID_ID3D12GraphicsCommandList2 },
        { "ID3D12GraphicsCommandList3", &IID_ID3D12GraphicsCommandList3 }, { "ID3D12GraphicsCommandList4", &IID_ID3D12GraphicsCommandList4 },
        { "ID3D12GraphicsCommandList5", &IID_ID3D12GraphicsCommandList5 }, { "ID3D12GraphicsCommandList6", &IID_ID3D12GraphicsCommandList6 },
        { "ID3D12GraphicsCommandList7", &IID_ID3D12GraphicsCommandList7 }, { "ID3D12GraphicsCommandList8", &IID_List8 },
        { "ID3D12GraphicsCommandList9", &IID_List9 }, { "ID3D12GraphicsCommandList10", &IID_List10 } };
    for (unsigned i = 0; i < sizeof list_iids / sizeof *list_iids; i++) qi((IUnknown *)cl, list_iids[i].n, list_iids[i].g);
    ID3D12GraphicsCommandList7 *cl7; CHECK(ID3D12GraphicsCommandList_QueryInterface(cl, &IID_ID3D12GraphicsCommandList7, (void **)&cl7));
    // enhanced barrier: RT -> copy source, then a buffer/global barrier
    D3D12_TEXTURE_BARRIER tb; memset(&tb, 0, sizeof tb);
    tb.SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET; tb.SyncAfter = D3D12_BARRIER_SYNC_COPY;
    tb.AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET; tb.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    tb.LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET; tb.LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    tb.pResource = r3; tb.Subresources.IndexOrFirstMipLevel = 0xffffffff;
    D3D12_GLOBAL_BARRIER gb = { D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_ACCESS_COMMON };
    D3D12_BARRIER_GROUP groups[2]; memset(groups, 0, sizeof groups);
    groups[0].Type = D3D12_BARRIER_TYPE_TEXTURE; groups[0].NumBarriers = 1; groups[0].pTextureBarriers = &tb;
    groups[1].Type = D3D12_BARRIER_TYPE_GLOBAL; groups[1].NumBarriers = 1; groups[1].pGlobalBarriers = &gb;
    ID3D12GraphicsCommandList7_Barrier(cl7, 2, groups);
    void *cl8 = NULL;
    if (SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(cl, &IID_List10, &cl8))) {
        ((void (WINAPI *)(void *, UINT, UINT))SLOT(cl8, 81))(cl8, 1, 1);   // OMSetFrontAndBackStencilRef
        IUnknown_Release((IUnknown *)cl8);
    }
    hr = ID3D12GraphicsCommandList7_Close(cl7);
    ID3D12CommandList *l[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, l);
    ID3D12Fence *fence; CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    qi((IUnknown *)fence, "ID3D12Fence1", &IID_ID3D12Fence1);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12CommandQueue_Signal(q, fence, 1); ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
    printf("  enhanced barriers + Close 0x%08x, executed: %s\n", (unsigned)hr, WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0 ? "yes" : "TIMEOUT");
    CloseHandle(ev);

    ID3D12GraphicsCommandList7_Release(cl7);
    ID3D12Fence_Release(fence); ID3D12GraphicsCommandList_Release(cl); ID3D12CommandAllocator_Release(alloc); ID3D12CommandQueue_Release(q);
    ID3D12DescriptorHeap_Release(sh);
    ID3D12Resource *all[] = { r1, r2, r3, p1, p2 };
    for (int i = 0; i < 5; i++) if (all[i]) ID3D12Resource_Release(all[i]);
    if (heap) ID3D12Heap_Release(heap);
    ID3D12Device9_Release(dev9); ID3D12Device10_Release(dev10); ID3D12Device8_Release(dev8); ID3D12Device4_Release(dev4);
    printf("device released, refs=%u\n", (unsigned)ID3D12Device_Release(dev));
    IDXGIFactory4_Release(fac);
    printf("ok\n");
    return 0;
}
