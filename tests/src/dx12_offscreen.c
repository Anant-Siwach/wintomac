// Direct3D 12, headless: device, queue, allocator, command list, root signature (serialized), PSO from DXBC,
// vertex buffer in an upload heap, root constants, clear + triangle into a 64x64 render target, copy to a
// readback buffer, fence + event wait, pixel hash.
// Built twice: as a Windows exe (recompiled; d3d12.dll is our runtime over Wine's libvkd3d) and natively for
// macOS straight against libvkd3d (reference). Both end up on MoltenVK on the same GPU, so pixels must match.
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS   /* C wrappers for methods that return structs (GetCPUDescriptorHandleForHeapStart) */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#else
#include <vkd3d.h>
#include <vkd3d_utils.h>
#endif
#include "tri12_vs.h"
#include "tri12_ps.h"

#define W 64
#define H 64
#define PITCH 256   /* D3D12_TEXTURE_DATA_PITCH_ALIGNMENT; W * 4 happens to equal it */
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08x\n", #x, (unsigned)hr_); return 1; } } while (0)

#ifndef _WIN32
// Native reference: libvkd3d directly. The Vulkan loader only enumerates MoltenVK when the instance opts into
// portability enumeration, so wrap vkGetInstanceProcAddr and add that to vkCreateInstance.
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

typedef struct { float x, y, r, g, b, a; } Vtx;

static ID3D12Resource *buffer(ID3D12Device *dev, D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    ID3D12Resource *r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r);
    if (FAILED(hr)) printf("FAIL CreateCommittedResource(buffer) = 0x%08x\n", (unsigned)hr);
    return r;
}

static void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}

int main(void) {
    ID3D12Device *dev; CHECK(create_device(&dev));
    printf("device ok, nodes=%u\n", ID3D12Device_GetNodeCount(dev));
    D3D12_FEATURE_DATA_D3D12_OPTIONS opt; memset(&opt, 0, sizeof opt);
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof opt));
    printf("resource binding tier %d\n", opt.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_1);

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *q; CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    ID3D12CommandAllocator *alloc; CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));

    // render target + RTV heap
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R8G8B8A8_UNORM, { { 0.05f, 0.05f, 0.1f, 1.0f } } };
    ID3D12Resource *rt; CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                                   &IID_ID3D12Resource, (void **)&rt));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap; CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    printf("rtv increment nonzero=%d\n", ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV) != 0);

    // vertex buffer (upload heap, mapped and filled by the CPU)
    static const Vtx tri[3] = { { 0.0f, 0.7f, 1, 0, 0, 1 }, { 0.7f, -0.6f, 0, 1, 0, 1 }, { -0.7f, -0.6f, 0, 0, 1, 1 } };
    ID3D12Resource *vb = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof tri, D3D12_RESOURCE_STATE_GENERIC_READ); if (!vb) return 1;
    void *p; D3D12_RANGE none = { 0, 0 };
    CHECK(ID3D12Resource_Map(vb, 0, &none, &p)); memcpy(p, tri, sizeof tri); ID3D12Resource_Unmap(vb, 0, NULL);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof tri, sizeof(Vtx) };
    ID3D12Resource *rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, PITCH * H, D3D12_RESOURCE_STATE_COPY_DEST); if (!rb) return 1;

    // root signature: 8 root constants at b0 (rot, tint), serialized through D3D12SerializeRootSignature
    D3D12_ROOT_PARAMETER rp = { D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS };
    rp.Constants.ShaderRegister = 0; rp.Constants.Num32BitValues = 8; rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3D10Blob *blob, *err = NULL;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
    printf("root signature blob %u bytes\n", (unsigned)ID3D10Blob_GetBufferSize(blob));
    ID3D12RootSignature *rs; CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                                    &IID_ID3D12RootSignature, (void **)&rs));
    ID3D10Blob_Release(blob);

    D3D12_INPUT_ELEMENT_DESC ied[2] = { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                                        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = tri12_vs; pd.VS.BytecodeLength = sizeof tri12_vs;
    pd.PS.pShaderBytecode = tri12_ps; pd.PS.BytecodeLength = sizeof tri12_ps;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.InputLayout.pInputElementDescs = ied; pd.InputLayout.NumElements = 2;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso; CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));

    ID3D12GraphicsCommandList *cl;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    ID3D12Fence *fence; CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));

    // interface identity: objects hand back the same device pointer; QueryInterface reaches derived interfaces
    ID3D12Device *dev2 = NULL; CHECK(ID3D12Resource_GetDevice(rt, &IID_ID3D12Device, (void **)&dev2));
    ID3D12GraphicsCommandList1 *cl1 = NULL; HRESULT qhr = ID3D12GraphicsCommandList_QueryInterface(cl, &IID_ID3D12GraphicsCommandList1, (void **)&cl1);
    printf("same device=%d, list1 qi=%d same object=%d\n", dev2 == dev, SUCCEEDED(qhr), (void *)cl1 == (void *)cl);
    if (dev2) ID3D12Device_Release(dev2);
    if (cl1) ID3D12GraphicsCommandList1_Release(cl1);

    // pass 1: clear only; pass 2: clear + rotated, tinted triangle. Each pass is read back and hashed.
    for (int pass = 0; pass < 2; pass++) {
        if (pass) { CHECK(ID3D12CommandAllocator_Reset(alloc)); CHECK(ID3D12GraphicsCommandList_Reset(cl, alloc, pso)); }
        const float clear[4] = { 0.05f, 0.05f, 0.1f, 1.0f };
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, clear, 0, NULL);
        if (pass) {
            float consts[8] = { 0.9553365f, 0.2955202f, 0, 0, 1.0f, 0.8f, 1.0f, 1.0f };   // rot = (cos .3, sin .3), tint
            ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 8, consts, 0);
            D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 }; D3D12_RECT sc = { 0, 0, W, H };
            ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
            ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
            ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &vbv);
            ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
        }
        transition(cl, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = W; dst.PlacedFootprint.Footprint.Height = H;
        dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = PITCH;
        src.SubresourceIndex = 0;
        ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        transition(cl, rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        CHECK(ID3D12GraphicsCommandList_Close(cl));
        ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
        CHECK(ID3D12CommandQueue_Signal(q, fence, pass + 1));
#ifdef _WIN32   // guest event: signalled by libvkd3d's fence worker thread through the runtime
        HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        CHECK(ID3D12Fence_SetEventOnCompletion(fence, pass + 1, ev));
        if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { printf("FAIL fence wait timed out\n"); return 1; }
        CloseHandle(ev);
#else
        CHECK(ID3D12Fence_SetEventOnCompletion(fence, pass + 1, NULL));
#endif
        printf("pass %d: fence=%llu\n", pass, (unsigned long long)ID3D12Fence_GetCompletedValue(fence));

        uint8_t *px; D3D12_RANGE rr = { 0, PITCH * H };
        CHECK(ID3D12Resource_Map(rb, 0, &rr, (void **)&px));
        uint64_t h = 1469598103934665603ULL; int lit = 0;
        for (int y = 0; y < H; y++)
            for (int i = 0; i < W * 4; i++) { h ^= px[y * PITCH + i]; h *= 1099511628211ULL; }
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t *q8 = px + y * PITCH + x * 4; if (q8[0] > 40 || q8[1] > 40) lit++; }
        uint8_t *c = px + 32 * PITCH + 32 * 4;
        printf("pass %d: center rgba=%u,%u,%u,%u corner rgba=%u,%u,%u,%u\n", pass, c[0], c[1], c[2], c[3], px[0], px[1], px[2], px[3]);
        printf("pass %d: lit pixels=%d hash=%016llx\n", pass, lit, (unsigned long long)h);
        if (pass)
            for (int y = 0; y < H; y += 4) {
                for (int x = 0; x < W; x += 2) { uint8_t *q8 = px + y * PITCH + x * 4; putchar(q8[0] > 128 ? 'R' : q8[1] > 128 ? 'G' : q8[2] > 128 ? 'B' : (q8[0] > 40 || q8[1] > 40) ? '+' : '.'); }
                putchar('\n');
            }
        D3D12_RANGE wr = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &wr);
    }

    ID3D12Fence_Release(fence); ID3D12GraphicsCommandList_Release(cl); ID3D12PipelineState_Release(pso); ID3D12RootSignature_Release(rs);
    ID3D12Resource_Release(rb); ID3D12Resource_Release(vb); ID3D12DescriptorHeap_Release(rtv_heap); ID3D12Resource_Release(rt);
    ID3D12CommandAllocator_Release(alloc); ID3D12CommandQueue_Release(q);
    ULONG left = ID3D12Device_Release(dev);
    printf("device released, refs=%u\nok\n", (unsigned)left);
    return 0;
}
