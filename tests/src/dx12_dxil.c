// Direct3D 12 with Shader Model 6 (DXIL), headless. Two scenes, each drawn once with SM 5.0 (DXBC) and once with
// SM 6.0 (DXIL) builds of the same HLSL (tests/shaders/dx12_sm6_blobs.h, from vkd3d-proton's test suite):
//   A: full-screen triangle from SV_VertexID, constant-colour pixel shader
//   B: vertex buffer + POSITION passthrough, pixel shader reading a cbuffer (root constants) and SV_Coverage
// The DXBC and DXIL pixels must agree. Built as a Windows exe (recompiled) and natively against libvkd3d.
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#else
#include <vkd3d.h>
#endif
#include "dx12_sm6_blobs.h"

#define W 32
#define H 32
#define PITCH 256
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08x\n", #x, (unsigned)hr_); return 1; } } while (0)

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
#else
static HRESULT create_device(ID3D12Device **dev) { return D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)dev); }
#endif

static ID3D12Device *dev; static ID3D12CommandQueue *q; static ID3D12CommandAllocator *alloc; static ID3D12GraphicsCommandList *cl;
static ID3D12Fence *fence; static UINT64 fval; static ID3D12Resource *rt, *rb, *vb; static D3D12_CPU_DESCRIPTOR_HANDLE rtv;
static ID3D12RootSignature *rs;

static ID3D12Resource *buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
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

// Draw with the given shaders; returns the hash of the result, prints the centre pixel. *ok = 0 on failure.
static uint64_t draw(const char *name, const void *vs, size_t vs_size, const void *ps, size_t ps_size, int vertex_buffer, int *ok) {
    D3D12_INPUT_ELEMENT_DESC ied = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = vs; pd.VS.BytecodeLength = vs_size; pd.PS.pShaderBytecode = ps; pd.PS.BytecodeLength = ps_size;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    if (vertex_buffer) { pd.InputLayout.pInputElementDescs = &ied; pd.InputLayout.NumElements = 1; }
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    if (FAILED(hr)) { printf("%s: CreateGraphicsPipelineState FAILED 0x%08x\n", name, (unsigned)hr); *ok = 0; return 0; }
    const float clear[4] = { 0, 0, 0, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    float alpha = 0.5f; ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 1, &alpha, 0);
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 }; D3D12_RECT sc = { 0, 0, W, H };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp); ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (vertex_buffer) {
        D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), 48, 16 };
        ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &vbv);
    }
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dst.PlacedFootprint.Footprint.Width = W;
    dst.PlacedFootprint.Footprint.Height = H; dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = PITCH;
    src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
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
    ID3D12PipelineState_Release(pso);
    uint8_t *px; D3D12_RANGE rr = { 0, PITCH * H };
    if (FAILED(ID3D12Resource_Map(rb, 0, &rr, (void **)&px))) { *ok = 0; return 0; }
    uint64_t h = 1469598103934665603ULL; int lit = 0;
    for (int y = 0; y < H; y++) for (int i = 0; i < W * 4; i++) { h ^= px[y * PITCH + i]; h *= 1099511628211ULL; }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (px[y * PITCH + x * 4] || px[y * PITCH + x * 4 + 1] || px[y * PITCH + x * 4 + 2]) lit++;
    uint8_t *c = px + (H / 2) * PITCH + (W / 2) * 4;
    printf("%s: centre=%u,%u,%u,%u lit=%d hash=%016llx\n", name, c[0], c[1], c[2], c[3], lit, (unsigned long long)h);
    D3D12_RANGE none = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &none);
    return h;
}

int main(void) {
    CHECK(create_device(&dev));
    // What does the device claim? Ask for the highest model this header knows; the runtime lowers it.
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_7 };
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm));
    printf("highest shader model: %x.%x\n", sm.HighestShaderModel >> 4, sm.HighestShaderModel & 0xf);
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1; memset(&o1, 0, sizeof o1);
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1)))
        printf("wave ops=%d lanes=%u..%u\n", o1.WaveOps, o1.WaveLaneCountMin, o1.WaveLaneCountMax);

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *heap; CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    rb = buffer(D3D12_HEAP_TYPE_READBACK, PITCH * H, D3D12_RESOURCE_STATE_COPY_DEST);
    static const float tri[12] = { -0.8f, -0.8f, 0, 1, 0.0f, 0.9f, 0, 1, 0.8f, -0.8f, 0, 1 };
    vb = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof tri, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!rb || !vb) { printf("FAIL buffers\n"); return 1; }
    void *p; D3D12_RANGE none = { 0, 0 }; CHECK(ID3D12Resource_Map(vb, 0, &none, &p)); memcpy(p, tri, sizeof tri); ID3D12Resource_Unmap(vb, 0, NULL);

    D3D12_ROOT_PARAMETER rp = { D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS };
    rp.Constants.ShaderRegister = 0; rp.Constants.Num32BitValues = 1; rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3D10Blob *blob, *err = NULL;
#ifdef _WIN32
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
#else
    CHECK(vkd3d_serialize_root_signature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
#endif
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    ID3D10Blob_Release(blob);

    int ok_a5 = 1, ok_a6 = 1, ok_b5 = 1, ok_b6 = 1;
    uint64_t a5 = draw("A sm5", sm6_vs_fullscreen_dxbc, sizeof sm6_vs_fullscreen_dxbc, sm6_ps_render_a8_dxbc, sizeof sm6_ps_render_a8_dxbc, 0, &ok_a5);
    uint64_t a6 = draw("A sm6", sm6_vs_fullscreen_dxil, sizeof sm6_vs_fullscreen_dxil, sm6_ps_render_a8_dxil, sizeof sm6_ps_render_a8_dxil, 0, &ok_a6);
    uint64_t b5 = draw("B sm5", sm6_vs_passthrough_dxbc, sizeof sm6_vs_passthrough_dxbc, sm6_ps_atoc_dxbc, sizeof sm6_ps_atoc_dxbc, 1, &ok_b5);
    uint64_t b6 = draw("B sm6", sm6_vs_passthrough_dxil, sizeof sm6_vs_passthrough_dxil, sm6_ps_atoc_dxil, sizeof sm6_ps_atoc_dxil, 1, &ok_b6);
    printf("scene A: dxil %s dxbc\n", ok_a5 && ok_a6 && a5 == a6 ? "matches" : "DIFFERS from");
    printf("scene B: dxil %s dxbc\n", ok_b5 && ok_b6 && b5 == b6 ? "matches" : "DIFFERS from");
    ID3D12RootSignature_Release(rs); ID3D12Resource_Release(vb); ID3D12Resource_Release(rb); ID3D12DescriptorHeap_Release(heap);
    ID3D12Resource_Release(rt); ID3D12Fence_Release(fence); ID3D12GraphicsCommandList_Release(cl); ID3D12CommandAllocator_Release(alloc);
    ID3D12CommandQueue_Release(q);
    printf("device released, refs=%u\nok\n", (unsigned)ID3D12Device_Release(dev));
    return 0;
}
