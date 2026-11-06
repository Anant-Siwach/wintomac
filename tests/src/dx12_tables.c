// Direct3D 12 resource binding, headless: a 4x4 texture uploaded with GetCopyableFootprints + CopyTextureRegion,
// a shader-visible CBV_SRV_UAV heap (CBV + SRV table at a non-zero heap offset, the SRV copied in from a CPU-only
// heap with CopyDescriptorsSimple), a shader-visible sampler heap (point sampler table), a static (linear) sampler and
// root constants. A full-screen triangle is drawn once with SM 5.0 (DXBC) and once with SM 6.0 (DXIL) shaders
// (tests/shaders/tables/tables.hlsl); both images are hashed and must agree.
// Built as a Windows exe (recompiled) and natively against libvkd3d (reference); dx12mtl_tables runs it on the
// Metal backend, where the tables are Metal argument buffers.
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
#include "dx12_tables_blobs.h"

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
static ID3D12Fence *fence; static UINT64 fval;

static ID3D12Resource *buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    ID3D12Resource *r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r);
    if (FAILED(hr)) printf("FAIL CreateCommittedResource(buffer) = 0x%08x\n", (unsigned)hr);
    return r;
}
static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}
static int submit_and_wait(void) {
    CHECK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
    CHECK(ID3D12CommandQueue_Signal(q, fence, ++fval));
#ifdef _WIN32
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(ID3D12Fence_SetEventOnCompletion(fence, fval, ev));
    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { printf("FAIL fence wait timed out\n"); return 1; }
    CloseHandle(ev);
#else
    CHECK(ID3D12Fence_SetEventOnCompletion(fence, fval, NULL));
#endif
    CHECK(ID3D12CommandAllocator_Reset(alloc));
    CHECK(ID3D12GraphicsCommandList_Reset(cl, alloc, NULL));
    return 0;
}

int main(void) {
    CHECK(create_device(&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));

    // 4x4 RGBA8 texture, uploaded through a placed footprint
    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, 0 };
    ID3D12Resource *tex; CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowsize, total;
    ID3D12Device_GetCopyableFootprints(dev, &td, 0, 1, 0, &fp, &rows, &rowsize, &total);
    printf("footprint: pitch=%u rows=%u rowsize=%llu total=%llu\n", fp.Footprint.RowPitch, rows, (unsigned long long)rowsize, (unsigned long long)total);
    ID3D12Resource *up = buffer(D3D12_HEAP_TYPE_UPLOAD, total + 256, D3D12_RESOURCE_STATE_GENERIC_READ); if (!up) return 1;
    uint8_t *p; CHECK(ID3D12Resource_Map(up, 0, NULL, (void **)&p));
    for (UINT y = 0; y < 4; y++)
        for (UINT x = 0; x < 4; x++) {
            uint8_t *t = p + fp.Offset + y * fp.Footprint.RowPitch + x * 4;
            t[0] = (uint8_t)(x * 80); t[1] = (uint8_t)(y * 80); t[2] = (uint8_t)((x ^ y) * 60); t[3] = 255;
        }
    // constant buffer (tint) right after the texture data, 256-byte aligned
    UINT64 cb_off = (total + 255) & ~255ull;
    const float tint[4] = { 1.0f, 0.5f, 1.0f, 1.0f };
    memcpy(p + cb_off, tint, sizeof tint);
    ID3D12Resource_Unmap(up, 0, NULL);
    D3D12_TEXTURE_COPY_LOCATION dst = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, src = { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.SubresourceIndex = 0; src.PlacedFootprint = fp;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (submit_and_wait()) return 1;

    // descriptors: CPU-only staging heap -> shader-visible heap; table = entries 1 (CBV) and 2 (SRV)
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap; CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    D3D12_DESCRIPTOR_HEAP_DESC sd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
    ID3D12DescriptorHeap *stage; CHECK(ID3D12Device_CreateDescriptorHeap(dev, &sd, &IID_ID3D12DescriptorHeap, (void **)&stage));
    D3D12_DESCRIPTOR_HEAP_DESC smd = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *sheap; CHECK(ID3D12Device_CreateDescriptorHeap(dev, &smd, &IID_ID3D12DescriptorHeap, (void **)&sheap));
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT sinc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_CPU_DESCRIPTOR_HANDLE c0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    D3D12_GPU_DESCRIPTOR_HANDLE g0 = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = { ID3D12Resource_GetGPUVirtualAddress(up) + cb_off, 256 };
    ID3D12Device_CreateConstantBufferView(dev, &cbv, (D3D12_CPU_DESCRIPTOR_HANDLE){ c0.ptr + 1 * inc });
    D3D12_SHADER_RESOURCE_VIEW_DESC srv; memset(&srv, 0, sizeof srv);
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(dev, tex, &srv, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(stage));
    ID3D12Device_CopyDescriptorsSimple(dev, 1, (D3D12_CPU_DESCRIPTOR_HANDLE){ c0.ptr + 2 * inc },
                                       ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(stage), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SAMPLER_DESC smp; memset(&smp, 0, sizeof smp);
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = 1000.0f; smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    D3D12_CPU_DESCRIPTOR_HANDLE s0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(sheap);
    ID3D12Device_CreateSampler(dev, &smp, (D3D12_CPU_DESCRIPTOR_HANDLE){ s0.ptr + sinc });   // sampler table starts at entry 1
    D3D12_GPU_DESCRIPTOR_HANDLE sg0 = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(sheap);

    // root signature: [0] table { CBV b0, SRV t0 }, [1] table { sampler s0 }, [2] 4 constants b1; static sampler s1
    D3D12_DESCRIPTOR_RANGE r0[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND } };
    D3D12_DESCRIPTOR_RANGE r1 = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[3]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable.NumDescriptorRanges = 2; rp[0].DescriptorTable.pDescriptorRanges = r0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &r1;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[2].Constants.ShaderRegister = 1; rp[2].Constants.Num32BitValues = 4;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss; memset(&ss, 0, sizeof ss);
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ss.MaxLOD = 1000.0f; ss.ShaderRegister = 1; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3D10Blob *blob, *err = NULL;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
    ID3D12RootSignature *rs; CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                                    &IID_ID3D12RootSignature, (void **)&rs));
    ID3D10Blob_Release(blob);

    // render target + readback
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    ID3D12Resource *rt; CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap; CHECK(ID3D12Device_CreateDescriptorHeap(dev, &rhd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, PITCH * H, D3D12_RESOURCE_STATE_COPY_DEST); if (!rb) return 1;

    uint64_t hashes[2];
    for (int v = 0; v < 2; v++) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
        pd.pRootSignature = rs;
        if (v == 0) { pd.VS.pShaderBytecode = tables_vs_dxbc; pd.VS.BytecodeLength = sizeof tables_vs_dxbc; pd.PS.pShaderBytecode = tables_ps_dxbc; pd.PS.BytecodeLength = sizeof tables_ps_dxbc; }
        else { pd.VS.pShaderBytecode = tables_vs_dxil; pd.VS.BytecodeLength = sizeof tables_vs_dxil; pd.PS.pShaderBytecode = tables_ps_dxil; pd.PS.BytecodeLength = sizeof tables_ps_dxil; }
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xffffffff;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
        ID3D12PipelineState *pso; CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));

        const float black[4] = { 0, 0, 0, 1 }, add[4] = { 0.0f, 0.0f, 0.125f, 0.0f };
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, black, 0, NULL);
        ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
        ID3D12DescriptorHeap *heaps[2] = { heap, sheap };
        ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, heaps);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, (D3D12_GPU_DESCRIPTOR_HANDLE){ g0.ptr + 1 * inc });
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, (D3D12_GPU_DESCRIPTOR_HANDLE){ sg0.ptr + sinc });
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 2, 4, add, 0);
        D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 }; D3D12_RECT sc = { 0, 0, W, H };
        ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
        barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION d2 = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, s2 = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        d2.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d2.PlacedFootprint.Footprint.Width = W;
        d2.PlacedFootprint.Footprint.Height = H; d2.PlacedFootprint.Footprint.Depth = 1; d2.PlacedFootprint.Footprint.RowPitch = PITCH;
        s2.SubresourceIndex = 0;
        ID3D12GraphicsCommandList_CopyTextureRegion(cl, &d2, 0, 0, 0, &s2, NULL);
        barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (submit_and_wait()) return 1;

        uint8_t *px; D3D12_RANGE rr = { 0, PITCH * H };
        CHECK(ID3D12Resource_Map(rb, 0, &rr, (void **)&px));
        uint64_t h = 1469598103934665603ULL;
        for (int y = 0; y < H; y++) for (int i = 0; i < W * 4; i++) { h ^= px[y * PITCH + i]; h *= 1099511628211ULL; }
        hashes[v] = h;
        uint8_t *a = px + 4 * PITCH + 4 * 4, *b = px + 20 * PITCH + 28 * 4;
        printf("%s: (4,4)=%u,%u,%u,%u (28,20)=%u,%u,%u,%u hash=%016llx\n", v ? "sm6" : "sm5", a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], (unsigned long long)h);
        D3D12_RANGE wr = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &wr);
        ID3D12PipelineState_Release(pso);
    }
    printf("dxil matches dxbc: %d\n", hashes[0] == hashes[1]);

    ID3D12Resource_Release(rb); ID3D12DescriptorHeap_Release(rtv_heap); ID3D12Resource_Release(rt); ID3D12RootSignature_Release(rs);
    ID3D12DescriptorHeap_Release(sheap); ID3D12DescriptorHeap_Release(stage); ID3D12DescriptorHeap_Release(heap);
    ID3D12Resource_Release(up); ID3D12Resource_Release(tex);
    ID3D12Fence_Release(fence); ID3D12GraphicsCommandList_Release(cl); ID3D12CommandAllocator_Release(alloc); ID3D12CommandQueue_Release(q);
    ULONG left = ID3D12Device_Release(dev);
    printf("device released, refs=%u\nok\n", (unsigned)left);
    return 0;
}
