// Shader Model 6.1-6.6 coverage, headless. Each feature is a DXC-compiled shader (tests/shaders/sm6x/*.hlsl,
// blobs in tests/shaders/dx12_sm6x_blobs.h) that is run through D3D12 and checked against values computed here:
//   PASS         the pipeline was created and the results are right
//   FAIL         the pipeline was created but the results are wrong
//   UNSUPPORTED  pipeline creation failed (vkd3d-shader or MoltenVK can't do it)
// Built as a Windows exe (recompiled) and natively against libvkd3d; both must print the same.
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#else
#include <vkd3d.h>
#endif
#include "dx12_sm6x_blobs.h"

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08x\n", #x, (unsigned)hr_); return 1; } } while (0)
#define OUT_SIZE 8192

#ifndef _WIN32
static PFN_vkCreateInstance real_create_instance;
static VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    VkInstanceCreateInfo c = *ci; const char *ext[64]; uint32_t n = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && n < 63; i++) ext[n++] = ci->ppEnabledExtensionNames[i];
    ext[n++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    c.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; c.enabledExtensionCount = n; c.ppEnabledExtensionNames = ext;
    return real_create_instance(&c, a, out);
}
static PFN_vkVoidFunction VKAPI_CALL get_proc(VkInstance i, const char *name) {
    if (!strcmp(name, "vkCreateInstance")) { real_create_instance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(i, name); return (PFN_vkVoidFunction)create_instance; }
    return vkGetInstanceProcAddr(i, name);
}
static HRESULT signal_event(HANDLE e) { (void)e; return S_OK; }
static HRESULT create_device(ID3D12Device **dev) {
    struct vkd3d_instance_create_info ici = { VKD3D_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pfn_signal_event = signal_event; ici.wchar_size = 2; ici.pfn_vkGetInstanceProcAddr = get_proc;
    struct vkd3d_device_create_info dci = { VKD3D_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.minimum_feature_level = D3D_FEATURE_LEVEL_11_0; dci.instance_create_info = &ici;
    return vkd3d_create_device(&dci, &IID_ID3D12Device, (void **)dev);
}
#define SERIALIZE_RS vkd3d_serialize_root_signature
#define SERIALIZE_VRS vkd3d_serialize_versioned_root_signature
#else
static HRESULT create_device(ID3D12Device **dev) { return D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)dev); }
#define SERIALIZE_RS D3D12SerializeRootSignature
#define SERIALIZE_VRS D3D12SerializeVersionedRootSignature
#endif

static ID3D12Device *dev; static ID3D12CommandQueue *q; static ID3D12CommandAllocator *alloc; static ID3D12GraphicsCommandList *cl;
static ID3D12Fence *fence; static UINT64 fval;
static ID3D12Resource *outbuf, *zero, *inbuf, *rb, *rt, *tex;
static ID3D12RootSignature *rs_cs, *rs_gfx, *rs_heap;
static ID3D12DescriptorHeap *rtv_heap, *srv_heap, *smp_heap;
static int n_pass, n_fail, n_unsup, o4_native16;

static ID3D12Resource *buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}
static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}
static void submit(void) {
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12CommandList *l[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, l);
    ID3D12CommandQueue_Signal(q, fence, ++fval);
#ifdef _WIN32
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, fval, ev); WaitForSingleObject(ev, 10000); CloseHandle(ev);
#else
    ID3D12Fence_SetEventOnCompletion(fence, fval, NULL);
#endif
    ID3D12CommandAllocator_Reset(alloc); ID3D12GraphicsCommandList_Reset(cl, alloc, NULL);
}
static ID3D12RootSignature *root_sig(const D3D12_ROOT_SIGNATURE_DESC *d) {
    ID3D10Blob *blob = NULL, *err = NULL; ID3D12RootSignature *r = NULL;
    if (FAILED(SERIALIZE_RS(d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return NULL;
    ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&r);
    ID3D10Blob_Release(blob);
    return r;
}

static void result(const char *name, const char *sm, int ok, const char *why) {
    printf("%-10s SM %s  %s%s%s\n", name, sm, ok > 0 ? "PASS" : ok == 0 ? "FAIL" : "UNSUPPORTED", why && *why ? ": " : "", why ? why : "");
    if (ok > 0) n_pass++; else if (ok == 0) n_fail++; else n_unsup++;
}

// Run a compute shader on rs_cs (or rs_heap) with `groups` groups; *out = mapped results (OUT_SIZE bytes) or NULL.
static uint32_t words[OUT_SIZE / 4];
static HRESULT run_cs(const unsigned char *code, size_t size, UINT groups, int heap) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = heap ? rs_heap : rs_cs; pd.CS.pShaderBytecode = code; pd.CS.BytecodeLength = size;
    ID3D12PipelineState *pso = NULL;
    HRESULT hr = pd.pRootSignature ? ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso) : E_FAIL;
    if (FAILED(hr)) return hr;
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, outbuf, 0, zero, 0, OUT_SIZE);
    barrier(outbuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    if (heap) {
        ID3D12DescriptorHeap *hs[2] = { srv_heap, smp_heap };
        ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, hs);
        ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs_heap);
    } else {
        ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs_cs);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(outbuf));
        ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(inbuf));
        static const UINT k[4] = { 7, 5, 2, 10 };
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 2, 4, k, 0);
    }
    ID3D12GraphicsCommandList_Dispatch(cl, groups, 1, 1);
    barrier(outbuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, rb, 0, outbuf, 0, OUT_SIZE);
    barrier(outbuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    submit();
    ID3D12PipelineState_Release(pso);
    void *p; D3D12_RANGE r = { 0, OUT_SIZE };
    if (FAILED(ID3D12Resource_Map(rb, 0, &r, &p))) return E_FAIL;
    memcpy(words, p, OUT_SIZE);
    D3D12_RANGE none = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &none);
    return S_OK;
}
#define RUN(blob, groups, heap, lbl, sm) \
    do { HRESULT h_ = run_cs(blob, sizeof blob, groups, heap); \
         if (FAILED(h_)) { char m[64]; snprintf(m, sizeof m, "pipeline 0x%08x", (unsigned)h_); result(#lbl, sm, -1, m); goto next_##lbl; } } while (0)

// Draw a full-screen triangle into the 8x8 RGBA32F target; px = pixel (x, y). Returns pipeline HRESULT.
static float pixels[8 * 8 * 4];
static HRESULT draw(const unsigned char *vs, size_t vs_size, const unsigned char *ps, size_t ps_size) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs_gfx;
    pd.VS.pShaderBytecode = vs; pd.VS.BytecodeLength = vs_size; pd.PS.pShaderBytecode = ps; pd.PS.BytecodeLength = ps_size;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT; pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    if (FAILED(hr)) return hr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    const float clear[4] = { -1, -1, -1, -1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs_gfx);
    D3D12_VIEWPORT vp = { 0, 0, 8, 8, 0, 1 }; D3D12_RECT sc = { 0, 0, 8, 8 };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp); ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; dst.PlacedFootprint.Footprint.Width = 8;
    dst.PlacedFootprint.Footprint.Height = 8; dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    submit();
    ID3D12PipelineState_Release(pso);
    uint8_t *p; D3D12_RANGE r = { 0, 256 * 8 };
    if (FAILED(ID3D12Resource_Map(rb, 0, &r, (void **)&p))) return E_FAIL;
    for (int y = 0; y < 8; y++) memcpy(pixels + y * 32, p + y * 256, 128);
    D3D12_RANGE none = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &none);
    return S_OK;
}
#define PX(x, y, c) pixels[((y) * 8 + (x)) * 4 + (c)]

static uint16_t f2h(float f) {   // exact for the values used here
    uint32_t b; memcpy(&b, &f, 4);
    uint32_t s = (b >> 16) & 0x8000, e = (b >> 23) & 0xff, m = b & 0x7fffff;
    return (uint16_t)(s | ((e - 112) << 10) | ((m + 0x1000) >> 13));
}

int main(void) {
#ifndef _WIN32
    setenv("VKD3D_DEBUG", "err", 0); setenv("VKD3D_SHADER_DEBUG", "err", 0);   // as runtime/runtime.c does
#endif
    CHECK(create_device(&dev));
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { (D3D_SHADER_MODEL)0x66 };
    HRESULT smhr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm);
    printf("reported shader model (asked 6.6): %x.%x (hr 0x%08x)\n", sm.HighestShaderModel >> 4, sm.HighestShaderModel & 0xf, (unsigned)smhr);
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1; memset(&o1, 0, sizeof o1);
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1);
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4; memset(&o4, 0, sizeof o4);
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof o4);
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3; memset(&o3, 0, sizeof o3);
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof o3);
    D3D12_FEATURE_DATA_D3D12_OPTIONS o0; memset(&o0, 0, sizeof o0);
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o0, sizeof o0);
    o4_native16 = o4.Native16BitShaderOpsSupported;
    printf("wave ops=%d lanes=%u..%u int64=%d native16bit=%d barycentrics=%d binding tier=%d\n", o1.WaveOps, o1.WaveLaneCountMin,
           o1.WaveLaneCountMax, o1.Int64ShaderOps, o4.Native16BitShaderOpsSupported, o3.BarycentricsSupported, o0.ResourceBindingTier);

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    outbuf = buffer(D3D12_HEAP_TYPE_DEFAULT, OUT_SIZE, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    zero = buffer(D3D12_HEAP_TYPE_UPLOAD, OUT_SIZE, D3D12_RESOURCE_STATE_GENERIC_READ, 0);
    inbuf = buffer(D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, 0);
    rb = buffer(D3D12_HEAP_TYPE_READBACK, OUT_SIZE, D3D12_RESOURCE_STATE_COPY_DEST, 0);
    if (!outbuf || !zero || !inbuf || !rb) { printf("FAIL buffers\n"); return 1; }
    void *p; D3D12_RANGE none = { 0, 0 };
    CHECK(ID3D12Resource_Map(zero, 0, &none, &p)); memset(p, 0, OUT_SIZE); ID3D12Resource_Unmap(zero, 0, NULL);
    CHECK(ID3D12Resource_Map(inbuf, 0, &none, &p));
    uint32_t *in = p; for (int i = 0; i < 64; i++) in[i] = i;
    float onefive = 1.5f; in[64] = 0x9abcdef0u; in[65] = 0x12345678u; memcpy(&in[66], &onefive, 4);
    in[67] = f2h(2.0f) | (uint32_t)f2h(-0.25f) << 16;
    ID3D12Resource_Unmap(inbuf, 0, NULL);

    D3D12_ROOT_PARAMETER rp[3]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[0].Descriptor.ShaderRegister = 0;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[1].Descriptor.ShaderRegister = 0;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[2].Constants.ShaderRegister = 0; rp[2].Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 0, NULL, 0 };
    if (!(rs_cs = root_sig(&rsd))) { printf("FAIL compute root signature\n"); return 1; }
    D3D12_ROOT_SIGNATURE_DESC gsd = { 0, NULL, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    if (!(rs_gfx = root_sig(&gsd))) { printf("FAIL graphics root signature\n"); return 1; }
    // SM 6.6 dynamic resources: an empty root signature that may index the heaps directly (version 1.1 flags).
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd; memset(&vd, 0, sizeof vd);
    vd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vd.Desc_1_1.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)(0x400 | 0x800);   // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED
    ID3D10Blob *vblob = NULL, *verr = NULL;
    HRESULT vhr = SERIALIZE_VRS(&vd, &vblob, &verr);
    if (SUCCEEDED(vhr)) {
        vhr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(vblob), ID3D10Blob_GetBufferSize(vblob), &IID_ID3D12RootSignature, (void **)&rs_heap);
        ID3D10Blob_Release(vblob);
    }
    printf("heap-indexing root signature: 0x%08x\n", (unsigned)vhr);

    // render target for the pixel-shader features
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 8, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap));

    // descriptor heaps for dynamic resources: [0] raw UAV of outbuf, [1] raw SRV of inbuf, [2] 2x2 texture; sampler [0] point
    D3D12_DESCRIPTOR_HEAP_DESC sh = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &sh, &IID_ID3D12DescriptorHeap, (void **)&srv_heap));
    D3D12_DESCRIPTOR_HEAP_DESC smh = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &smh, &IID_ID3D12DescriptorHeap, (void **)&smp_heap));
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE hcpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud; memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = OUT_SIZE / 4; ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(dev, outbuf, NULL, &ud, hcpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd; memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R32_TYPELESS; sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.NumElements = 1024; sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    hcpu.ptr += inc; ID3D12Device_CreateShaderResourceView(dev, inbuf, &sd, hcpu);
    D3D12_RESOURCE_DESC xd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, 0 };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &xd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex));
    CHECK(ID3D12Resource_Map(zero, 0, &none, &p));
    ((uint32_t *)p)[0] = 10; ((uint32_t *)p)[1] = 200; ((uint32_t *)p)[64] = 30; ((uint32_t *)p)[65] = 40;   // texel (1,0).r = 200
    ID3D12Resource_Unmap(zero, 0, NULL);
    D3D12_TEXTURE_COPY_LOCATION tdst = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, tsrc = { zero, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    tsrc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tsrc.PlacedFootprint.Footprint.Width = 2; tsrc.PlacedFootprint.Footprint.Height = 2;
    tsrc.PlacedFootprint.Footprint.Depth = 1; tsrc.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &tdst, 0, 0, 0, &tsrc, NULL);
    barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    submit();
    CHECK(ID3D12Resource_Map(zero, 0, &none, &p)); memset(p, 0, OUT_SIZE); ID3D12Resource_Unmap(zero, 0, NULL);
    hcpu.ptr += inc; ID3D12Device_CreateShaderResourceView(dev, tex, NULL, hcpu);
    D3D12_SAMPLER_DESC smp; memset(&smp, 0, sizeof smp);
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; smp.MaxLOD = 1000;
    ID3D12Device_CreateSampler(dev, &smp, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(smp_heap));

    // ---- SM 6.0 wave intrinsics ---------------------------------------------------------------------
    RUN(sm6x_wave60, 1, 0, wave, "6.0");
    {
        uint32_t W = words[0]; int bad = 0; char why[128] = "";
        for (uint32_t t = 0; t < 64 && W && !bad; t++) {
            uint32_t lane = t % W, base = t - lane, n = W, *w = words + t * 16, sum = 0, pre = 1, e[16];
            for (uint32_t i = 0; i < n; i++) sum += base + i;
            for (uint32_t i = base; i < t; i++) pre *= i < 4 ? 2 : 1;
            e[0] = W; e[1] = sum; e[2] = lane; e[3] = base; e[4] = n / 2; e[5] = base + n - 1; e[6] = 1;
            e[7] = base + (lane + 1) % n; e[8] = n >= 32 ? 0xffffffffu : (1u << n) - 1; e[9] = lane == 0; e[10] = pre;
            e[11] = n < 5 ? n : 5; e[12] = n > 3; e[13] = 1; e[14] = lane; e[15] = lane;
            for (int i = 0; i < 16; i++) if (w[i] != e[i]) { snprintf(why, sizeof why, "thread %u word %d = %u, want %u", t, i, w[i], e[i]); bad = 1; break; }
        }
        if (!W) snprintf(why, sizeof why, "lane count 0");
        result("wave", "6.0", W && !bad, why);
        printf("           wave size %u\n", W);
    }
next_wave:
    RUN(sm6x_int64, 1, 0, int64, "6.0");
    {
        uint64_t a = (5ull << 32) | 7, b = 0x123456789abcdef0ull, e[5] = { a * b, b >> 7, b / a, b % a, (uint64_t)(-(int64_t)a >> 3) };
        int ok = 1; char why[96] = "";
        for (int i = 0; i < 5; i++) {
            uint64_t g = words[2 * i] | (uint64_t)words[2 * i + 1] << 32;
            if (g != e[i]) { ok = 0; snprintf(why, sizeof why, "result %d = %016llx, want %016llx", i, (unsigned long long)g, (unsigned long long)e[i]); break; }
        }
        result("int64", "6.0", ok, why);
    }
next_int64:
    RUN(sm6x_f16, 1, 0, f16, "6.2");
    {
        uint32_t e[4] = { f2h(2.75f) | (uint32_t)0x3da8 << 16, 1u | 0xfda8u << 16, 0x40300000u, 0xfffffda8u };
        int ok = 1; char why[160] = "";
        for (int i = 0; i < 4; i++) if (words[i] != e[i]) { ok = 0; snprintf(why, sizeof why, "word %d = %08x, want %08x", i, words[i], e[i]); break; }
        // 16-bit types are optional (OPTIONS4.Native16BitShaderOpsSupported); vkd3d-shader treats them as 32-bit.
        if (!ok && !o4_native16) { ok = -1; snprintf(why + strlen(why), sizeof why - strlen(why), "; not claimed (Native16BitShaderOpsSupported=0)"); }
        result("f16", "6.2", ok, why);
    }
next_f16:
    RUN(sm6x_dot4, 1, 0, dot4add, "6.4");
    {
        float f; memcpy(&f, &words[2], 4);
        char why[96]; snprintf(why, sizeof why, "u8 %u (80) i8 %d (9) dot2add %g (1.5)", words[0], (int)words[1], f);
        result("dot4add", "6.4", words[0] == 80 && words[1] == 9 && f == 1.5f, why);
    }
next_dot4add:
    RUN(sm6x_wave65, 1, 0, wavematch, "6.5");
    {
        uint32_t W = words[3]; int bad = !W; char why[96] = "";
        for (uint32_t t = 0; t < 64 && !bad; t++) {
            uint32_t lane = t % W, base = t - lane, cnt = 0, pre = 0, *w = words + t * 4;
            for (uint32_t i = 0; i < W; i++) if ((base + i) % 3 == t % 3) { cnt++; if (i < lane) pre++; }
            if (w[0] != cnt || w[1] != pre || w[2] != pre) { bad = 1; snprintf(why, sizeof why, "thread %u: %u %u %u, want %u %u %u", t, w[0], w[1], w[2], cnt, pre, pre); }
        }
        result("wavematch", "6.5", !bad, why);
    }
next_wavematch:
    RUN(sm6x_atomic64, 1, 0, atomic64, "6.6");
    {
        uint32_t e[6] = { 2016, 128, 5, 63u << 8, 64, 64 };
        int ok = 1; char why[96] = "";
        for (int i = 0; i < 6; i++) if (words[i] != e[i]) { ok = 0; snprintf(why, sizeof why, "word %d = %u, want %u", i, words[i], e[i]); break; }
        result("atomic64", "6.6", ok, why);
    }
next_atomic64:
    RUN(sm6x_dynres, 1, 1, dynres, "6.6");
    {
        char why[64]; snprintf(why, sizeof why, "%u (103) %u (200)", words[0], words[1]);
        result("dynres", "6.6", words[0] == 103 && words[1] == 200, why);
    }
next_dynres:
    RUN(sm6x_pack66, 1, 0, pack, "6.6");
    {
        uint32_t e[8] = { 0xff030201u, 1, 127, 0xffffffffu, 0xffffff80u, 0x05fb7f80u, 0, 32 };
        int ok = 1; char why[96] = "";
        for (int i = 0; i < 8; i++) if (words[i] != e[i]) { ok = 0; snprintf(why, sizeof why, "word %d = %08x, want %08x", i, words[i], e[i]); break; }
        result("pack", "6.6", ok, why);
    }
next_pack:
    RUN(sm6x_csderiv, 1, 0, csderiv, "6.6");
    {
        int ok = 1; char why[96] = "";
        for (uint32_t y = 0; y < 8 && ok; y++) for (uint32_t x = 0; x < 8 && ok; x++) {
            uint32_t *w = words + (y * 8 + x) * 3; float dx, dy; memcpy(&dx, &w[0], 4); memcpy(&dy, &w[1], 4);
            float ex = (float)(4 * (x / 2) + 1);
            if (dx != ex || dy != 10.0f || w[2] != (x ^ 1) * 100 + y) { ok = 0; snprintf(why, sizeof why, "(%u,%u): ddx %g ddy %g quad %u", x, y, dx, dy, w[2]); }
        }
        result("csderiv", "6.6", ok, why);
    }
next_csderiv:
    RUN(sm6x_rayquery, 1, 0, rayquery, "6.5");
    result("rayquery", "6.5", 0, "pipeline created without an acceleration structure");
next_rayquery:
    {   // mesh shader: pipeline state stream { root signature, MS, RT formats }
        struct { uint32_t t0; uint32_t pad0; ID3D12RootSignature *rs; uint32_t t1; uint32_t pad1; D3D12_SHADER_BYTECODE ms;
                 uint32_t t2; struct D3D12_RT_FORMAT_ARRAY rtf; uint32_t pad2; } st;
        memset(&st, 0, sizeof st);
        st.t0 = 0; st.rs = rs_gfx; st.t1 = 25 /* MS */; st.ms.pShaderBytecode = sm6x_mesh; st.ms.BytecodeLength = sizeof sm6x_mesh;
        st.t2 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS; st.rtf.NumRenderTargets = 1; st.rtf.RTFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D12_PIPELINE_STATE_STREAM_DESC sdsc = { sizeof st, &st };
        ID3D12Device2 *d2 = NULL; ID3D12PipelineState *pso = NULL; HRESULT h = E_NOINTERFACE;
        if (SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device2, (void **)&d2))) {
            h = ID3D12Device2_CreatePipelineState(d2, &sdsc, &IID_ID3D12PipelineState, (void **)&pso);
            ID3D12Device2_Release(d2);
        }
        char why[64]; snprintf(why, sizeof why, "pipeline 0x%08x", (unsigned)h);
        if (pso) { ID3D12PipelineState_Release(pso); result("mesh", "6.5", 0, "pipeline created, not drawn"); }
        else result("mesh", "6.5", -1, why);
    }
    {
        HRESULT h = draw(sm6x_bary_vs, sizeof sm6x_bary_vs, sm6x_bary_ps, sizeof sm6x_bary_ps);
        char why[128]; snprintf(why, sizeof why, "pipeline 0x%08x", (unsigned)h);
        if (FAILED(h)) result("bary", "6.1", -1, why);
        else {
            float s = PX(2, 2, 0) + PX(2, 2, 1) + PX(2, 2, 2);
            snprintf(why, sizeof why, "b=(%.3f %.3f %.3f) ids %.0f", PX(2, 2, 0), PX(2, 2, 1), PX(2, 2, 2), PX(2, 2, 3));
            result("bary", "6.1", fabsf(s - 1.0f) < 0.01f && PX(2, 2, 3) == 121110.0f, why);
        }
    }
    {
        HRESULT h = draw(sm6x_viewid_vs, sizeof sm6x_viewid_vs, sm6x_quad_ps, sizeof sm6x_quad_ps);
        char why[64]; snprintf(why, sizeof why, "pipeline 0x%08x", (unsigned)h);
        result("viewid", "6.1", FAILED(h) ? -1 : PX(0, 0, 3) == 1.0f, FAILED(h) ? why : "");
    }
    {
        HRESULT h = draw(sm6x_wave_vs, sizeof sm6x_wave_vs, sm6x_quad_ps, sizeof sm6x_quad_ps);
        char why[64]; snprintf(why, sizeof why, "pipeline 0x%08x", (unsigned)h);
        result("wave-vs", "6.0", FAILED(h) ? -1 : PX(4, 4, 3) == 1.0f, FAILED(h) ? why : "");
    }
    {
        HRESULT h = draw(sm6x_bary_vs, sizeof sm6x_bary_vs, sm6x_quad_ps, sizeof sm6x_quad_ps);
        char why[96]; snprintf(why, sizeof why, "pipeline 0x%08x", (unsigned)h);
        if (FAILED(h)) result("quad-ps", "6.0", -1, why);
        else {
            snprintf(why, sizeof why, "(2,3) = %.0f,%.0f", PX(2, 3, 0) * 255, PX(2, 3, 1) * 255);
            result("quad-ps", "6.0", fabsf(PX(2, 3, 0) * 255 - 3) < 0.01f && fabsf(PX(2, 3, 1) * 255 - 2) < 0.01f, why);
        }
    }
    printf("%d pass, %d fail, %d unsupported\n", n_pass, n_fail, n_unsup);

    ID3D12RootSignature_Release(rs_cs); ID3D12RootSignature_Release(rs_gfx); if (rs_heap) ID3D12RootSignature_Release(rs_heap);
    ID3D12DescriptorHeap_Release(rtv_heap); ID3D12DescriptorHeap_Release(srv_heap); ID3D12DescriptorHeap_Release(smp_heap);
    ID3D12Resource_Release(tex); ID3D12Resource_Release(rt); ID3D12Resource_Release(outbuf); ID3D12Resource_Release(zero);
    ID3D12Resource_Release(inbuf); ID3D12Resource_Release(rb);
    ID3D12Fence_Release(fence); ID3D12GraphicsCommandList_Release(cl); ID3D12CommandAllocator_Release(alloc); ID3D12CommandQueue_Release(q);
    printf("device released, refs=%u\nok\n", (unsigned)ID3D12Device_Release(dev));
    return 0;
}
