// Workarounds for Direct3D 12 features Metal/MoltenVK lacks (runtime/d3d12_pso.c), headless:
//   * geometry shaders: a pass-through GS (DXBC and DXIL) is dropped and draws like no GS; point expansion and
//     layered rendering create their PSOs but their draws are skipped; the next normal PSO draws again
//   * stream output: capture-only PSOs skip draws, PSOs with a rasterized stream draw
//   * predication: always executes (no conditional rendering)
//   * depth clip disable: emulated with depth clamp
//   * sampler border colours: snapped to the nearest Vulkan border colour
//   * null SRVs (2D, 3D, cube, buffer) read zero
//   * tiled resources: tier NOT_SUPPORTED, CreateReservedResource fails cleanly
// Output is checked against tests/expected/dx12_gs_fallback.out (a native libvkd3d build has none of these).
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <d3d12.h>
#include "dx12_gs_blobs.h"

#define W 64
#define H 64
#define PITCH 256
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08x\n", #x, (unsigned)hr_); return 1; } } while (0)
#define BC(name) { name, sizeof name }

static ID3D12Device *dev; static ID3D12CommandQueue *q; static ID3D12CommandAllocator *alloc; static ID3D12GraphicsCommandList *cl;
static ID3D12Fence *fence; static UINT64 fval; static HANDLE ev;
static ID3D12Resource *rt, *rb, *pred, *tex;
static ID3D12RootSignature *rs_draw, *rs_tex;
static ID3D12DescriptorHeap *rtv_heap, *srv_heap, *smp_heap;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv;

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
    ID3D12Fence_SetEventOnCompletion(fence, fval, ev); WaitForSingleObject(ev, 10000);
    ID3D12CommandAllocator_Reset(alloc); ID3D12GraphicsCommandList_Reset(cl, alloc, NULL);
}
static ID3D12RootSignature *root_sig(const D3D12_ROOT_SIGNATURE_DESC *d) {
    ID3D10Blob *blob = NULL, *err = NULL; ID3D12RootSignature *r = NULL;
    if (FAILED(D3D12SerializeRootSignature(d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return NULL;
    ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&r);
    ID3D10Blob_Release(blob);
    return r;
}

typedef struct { const void *p; SIZE_T n; } Code;
static const Code none = { NULL, 0 };
static D3D12_SO_DECLARATION_ENTRY so_entry = { 0, "SV_Position", 0, 0, 4, 0 };
static UINT so_stride = 16;
// so: 0 none, 1 capture only, 2 capture + rasterized stream 0
static ID3D12PipelineState *pso(ID3D12RootSignature *rs, Code vs, Code gs, Code ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE topo, int so, BOOL depth_clip, HRESULT *hr) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d; memset(&d, 0, sizeof d);
    d.pRootSignature = rs;
    d.VS.pShaderBytecode = vs.p; d.VS.BytecodeLength = vs.n; d.GS.pShaderBytecode = gs.p; d.GS.BytecodeLength = gs.n;
    d.PS.pShaderBytecode = ps.p; d.PS.BytecodeLength = ps.n;
    if (so) {
        d.StreamOutput.pSODeclaration = &so_entry; d.StreamOutput.NumEntries = 1; d.StreamOutput.pBufferStrides = &so_stride;
        d.StreamOutput.NumStrides = 1; d.StreamOutput.RasterizedStream = so == 1 ? D3D12_SO_NO_RASTERIZED_STREAM : 0;
    }
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; d.SampleMask = 0xffffffff;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; d.RasterizerState.DepthClipEnable = depth_clip;
    d.PrimitiveTopologyType = topo;
    d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    ID3D12PipelineState *p = NULL;
    *hr = ID3D12Device_CreateGraphicsPipelineState(dev, &d, &IID_ID3D12PipelineState, (void **)&p);
    return p;
}

// Clear, run `draws` draws (each: PSO, topology, vertex count), read slice 0 back. Prints lit pixels + hash.
typedef struct { ID3D12PipelineState *p; D3D_PRIMITIVE_TOPOLOGY topo; UINT verts; } Draw;
static uint8_t px[H * PITCH];
static UINT srv_slot;   // descriptor used by the texture tests (null SRVs each get a fresh one)
static uint64_t run(const char *name, ID3D12RootSignature *rs, const float *k, int nk, const Draw *draws, int n, ID3D12Resource *predicate) {
    const float clear[4] = { 0, 0, 0, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    if (rs == rs_tex) {
        ID3D12DescriptorHeap *hs[2] = { srv_heap, smp_heap };
        ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, hs);
        D3D12_GPU_DESCRIPTOR_HANDLE g = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap);
        g.ptr += srv_slot * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, g);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 2, ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(smp_heap));
    }
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, nk, k, 0);
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 }; D3D12_RECT sc = { 0, 0, W, H };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp); ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    if (predicate) ID3D12GraphicsCommandList_SetPredication(cl, predicate, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    for (int i = 0; i < n; i++) {
        ID3D12GraphicsCommandList_SetPipelineState(cl, draws[i].p);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, draws[i].topo);
        ID3D12GraphicsCommandList_DrawInstanced(cl, draws[i].verts, 1, 0, 0);
    }
    if (predicate) ID3D12GraphicsCommandList_SetPredication(cl, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dst.PlacedFootprint.Footprint.Width = W;
    dst.PlacedFootprint.Footprint.Height = H; dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = PITCH;
    src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    submit();
    uint8_t *p; D3D12_RANGE rr = { 0, sizeof px };
    if (FAILED(ID3D12Resource_Map(rb, 0, &rr, (void **)&p))) return 0;
    memcpy(px, p, sizeof px);
    D3D12_RANGE nr = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &nr);
    uint64_t h = 1469598103934665603ULL; int lit = 0;
    for (int y = 0; y < H; y++) for (int i = 0; i < W * 4; i++) { h ^= px[y * PITCH + i]; h *= 1099511628211ULL; }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (px[y * PITCH + x * 4] || px[y * PITCH + x * 4 + 1] || px[y * PITCH + x * 4 + 2]) lit++;
    uint8_t *c = px + 32 * PITCH + 32 * 4;
    printf("%-28s lit=%4d centre=%u,%u,%u,%u hash=%016llx\n", name, lit, c[0], c[1], c[2], c[3], (unsigned long long)h);
    return h;
}

int main(void) {
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT }, up = { D3D12_HEAP_TYPE_UPLOAD }, rp = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, sizeof px, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb));
    bd.Width = 256;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&pred));
    void *m; D3D12_RANGE nr = { 0, 0 };
    CHECK(ID3D12Resource_Map(pred, 0, &nr, &m)); memset(m, 0, 256); ID3D12Resource_Unmap(pred, 0, NULL);   // predicate value 0
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    D3D12_RENDER_TARGET_VIEW_DESC rvd; memset(&rvd, 0, sizeof rvd);
    rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY; rvd.Texture2DArray.ArraySize = 2;
    ID3D12Device_CreateRenderTargetView(dev, rt, &rvd, rtv);
    D3D12_DESCRIPTOR_HEAP_DESC sh = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &sh, &IID_ID3D12DescriptorHeap, (void **)&srv_heap));
    D3D12_DESCRIPTOR_HEAP_DESC smh = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &smh, &IID_ID3D12DescriptorHeap, (void **)&smp_heap));

    D3D12_ROOT_PARAMETER p[3]; memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT };
    if (!(rs_draw = root_sig(&rsd))) { printf("FAIL root signature\n"); return 1; }
    D3D12_DESCRIPTOR_RANGE r0 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 }, r1 = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0 };
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable.NumDescriptorRanges = 1; p[1].DescriptorTable.pDescriptorRanges = &r0;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[2].DescriptorTable.NumDescriptorRanges = 1; p[2].DescriptorTable.pDescriptorRanges = &r1;
    p[1].ShaderVisibility = p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rsd.NumParameters = 3;
    if (!(rs_tex = root_sig(&rsd))) { printf("FAIL texture root signature\n"); return 1; }

    D3D12_FEATURE_DATA_D3D12_OPTIONS o; memset(&o, 0, sizeof o);
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    printf("tiled resources tier=%d, VP/RT index from VS without GS=%d\n", o.TiledResourcesTier,
           o.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation);

    // ---- geometry shaders ----------------------------------------------------------------------------
    static const float z0[4] = { 0.5f };
    HRESULT h1, h2, h3, h4, h5;
    Code vs5 = BC(gs_vs_dxbc), ps5 = BC(gs_ps_dxbc), vs6 = BC(gs_vs_dxil), ps6 = BC(gs_ps_dxil);
    ID3D12PipelineState *plain = pso(rs_draw, vs5, none, ps5, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0, TRUE, &h1);
    ID3D12PipelineState *pass5 = pso(rs_draw, vs5, (Code)BC(gs_gs_pass_dxbc), ps5, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0, TRUE, &h2);
    ID3D12PipelineState *pass6 = pso(rs_draw, vs6, (Code)BC(gs_gs_pass_dxil), ps6, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0, TRUE, &h3);
    ID3D12PipelineState *expand = pso(rs_draw, vs5, (Code)BC(gs_gs_expand_dxbc), ps5, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, 0, TRUE, &h4);
    ID3D12PipelineState *layer = pso(rs_draw, vs5, (Code)BC(gs_gs_layer_dxbc), ps5, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0, TRUE, &h5);
    printf("PSOs: plain 0x%08x, pass-through DXBC 0x%08x DXIL 0x%08x, point expansion 0x%08x, layered 0x%08x\n",
           (unsigned)h1, (unsigned)h2, (unsigned)h3, (unsigned)h4, (unsigned)h5);
    if (!plain || !pass5 || !pass6 || !expand || !layer) { printf("FAIL PSO creation\n"); return 1; }
    Draw d_plain = { plain, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3 };
    uint64_t ref = run("no GS", rs_draw, z0, 1, &d_plain, 1, NULL);
    Draw d_pass5 = { pass5, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3 }, d_pass6 = { pass6, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3 };
    printf("  pass-through GS DXBC: %s\n", run("pass-through GS (DXBC)", rs_draw, z0, 1, &d_pass5, 1, NULL) == ref ? "same as no GS" : "DIFFERS");
    printf("  pass-through GS DXIL: %s\n", run("pass-through GS (DXIL)", rs_draw, z0, 1, &d_pass6, 1, NULL) == ref ? "same as no GS" : "DIFFERS");
    Draw d_expand = { expand, D3D_PRIMITIVE_TOPOLOGY_POINTLIST, 3 }, d_layer = { layer, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3 };
    run("point expansion GS (skipped)", rs_draw, z0, 1, &d_expand, 1, NULL);
    run("layered GS (skipped)", rs_draw, z0, 1, &d_layer, 1, NULL);
    Draw mixed[2] = { d_expand, d_plain };
    printf("  skipped PSO, then plain PSO: %s\n", run("skip then plain", rs_draw, z0, 1, mixed, 2, NULL) == ref ? "plain draw rendered" : "WRONG");

    // ---- stream output -------------------------------------------------------------------------------
    HRESULT h6, h7;
    ID3D12PipelineState *so_only = pso(rs_draw, vs5, none, none, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 1, TRUE, &h6);
    ID3D12PipelineState *so_rast = pso(rs_draw, vs5, none, ps5, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 2, TRUE, &h7);
    printf("PSOs: stream output capture-only 0x%08x, rasterized 0x%08x\n", (unsigned)h6, (unsigned)h7);
    if (!so_only || !so_rast) { printf("FAIL SO PSO creation\n"); return 1; }
    Draw d_so1 = { so_only, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3 }, d_so2 = { so_rast, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3 };
    run("SO capture-only (skipped)", rs_draw, z0, 1, &d_so1, 1, NULL);
    printf("  SO + rasterized stream: %s\n", run("SO + rasterized stream", rs_draw, z0, 1, &d_so2, 1, NULL) == ref ? "same as no SO" : "DIFFERS");

    // ---- predication (value 0, EQUAL_ZERO: Windows would skip the draw) ---------------------------------
    printf("  predicated draw: %s\n", run("predicated (always draws)", rs_draw, z0, 1, &d_plain, 1, pred) == ref ? "executed" : "skipped");

    // ---- depth clip disable: z = 1.5 is beyond the far plane ------------------------------------------
    static const float zfar[4] = { 1.5f };
    HRESULT h8; ID3D12PipelineState *noclip = pso(rs_draw, vs5, none, ps5, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0, FALSE, &h8);
    if (!noclip) { printf("FAIL depth clip PSO 0x%08x\n", (unsigned)h8); return 1; }
    Draw d_noclip = { noclip, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3 };
    run("z=1.5, depth clip on", rs_draw, zfar, 1, &d_plain, 1, NULL);
    run("z=1.5, depth clip off", rs_draw, zfar, 1, &d_noclip, 1, NULL);

    // ---- sampler border colours and null descriptors: the triangle, textured --------------------------------
    D3D12_RESOURCE_DESC xd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, 0 };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &xd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void **)&tex));
    D3D12_CPU_DESCRIPTOR_HANDLE srv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap), smp = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(smp_heap);
    ID3D12Device_CreateShaderResourceView(dev, tex, NULL, srv);
    static const float uv_out[4] = { 0.5f, -2.0f, -2.0f };   // z, uv
    static const struct { const char *name; float c[4]; } borders[] = {
        { "border {0.9,0.9,0.9,1}", { 0.9f, 0.9f, 0.9f, 1 } }, { "border {0.2,0.3,0.1,0.8}", { 0.2f, 0.3f, 0.1f, 0.8f } },
        { "border {1,0,0,0.2}", { 1, 0, 0, 0.2f } }, { "border {1,1,1,1}", { 1, 1, 1, 1 } } };
    HRESULT h9; ID3D12PipelineState *texpso = pso(rs_tex, vs5, none, (Code)BC(gs_ps_tex_dxbc), D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0, TRUE, &h9);
    if (!texpso) { printf("FAIL texture PSO 0x%08x\n", (unsigned)h9); return 1; }
    Draw d_tex = { texpso, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3 };
    for (int i = 0; i < 4; i++) {
        D3D12_SAMPLER_DESC sd; memset(&sd, 0, sizeof sd);
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER; sd.MaxLOD = 100;
        memcpy(sd.BorderColor, borders[i].c, 16);
        ID3D12Device_CreateSampler(dev, &sd, smp);
        run(borders[i].name, rs_tex, uv_out, 3, &d_tex, 1, NULL);
        uint8_t *c = px + 32 * PITCH + 18 * 4;   // inside the triangle
        printf("  -> sampled %u,%u,%u,%u\n", c[0], c[1], c[2], c[3]);
    }
    D3D12_SAMPLER_DESC sd; memset(&sd, 0, sizeof sd);
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; sd.MaxLOD = 100;
    ID3D12Device_CreateSampler(dev, &sd, smp);
    static const struct { const char *name; D3D12_SRV_DIMENSION dim; const unsigned char *ps; SIZE_T n; } nulls[] = {
        { "null SRV Texture2D", D3D12_SRV_DIMENSION_TEXTURE2D, gs_ps_tex_dxbc, sizeof gs_ps_tex_dxbc },
        { "null SRV Texture3D", D3D12_SRV_DIMENSION_TEXTURE3D, gs_ps_tex3d_dxbc, sizeof gs_ps_tex3d_dxbc },
        { "null SRV TextureCube", D3D12_SRV_DIMENSION_TEXTURECUBE, gs_ps_cube_dxbc, sizeof gs_ps_cube_dxbc },
        { "null SRV Buffer", D3D12_SRV_DIMENSION_BUFFER, gs_ps_buf_dxbc, sizeof gs_ps_buf_dxbc } };
    static const float uv_in[4] = { 0.5f, 0.5f, 0.5f };
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC vd; memset(&vd, 0, sizeof vd);
        vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; vd.ViewDimension = nulls[i].dim; vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vd.Texture2D.MipLevels = 1;   // same offset for 3D / cube MipLevels; buffer: FirstElement 0 .. NumElements 1
        if (nulls[i].dim == D3D12_SRV_DIMENSION_BUFFER) { vd.Buffer.FirstElement = 0; vd.Buffer.NumElements = 1; }
        srv_slot = 1 + i;   // a never-used slot, so a missing view can't be masked by the previous one
        D3D12_CPU_DESCRIPTOR_HANDLE slot = srv;
        slot.ptr += srv_slot * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ID3D12Device_CreateShaderResourceView(dev, NULL, &vd, slot);
        HRESULT hn; ID3D12PipelineState *np = pso(rs_tex, vs5, none, (Code){ nulls[i].ps, nulls[i].n }, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0, TRUE, &hn);
        if (!np) { printf("%s: PSO 0x%08x\n", nulls[i].name, (unsigned)hn); continue; }
        Draw dn = { np, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3 };
        run(nulls[i].name, rs_tex, uv_in, 3, &dn, 1, NULL);
        uint8_t *c = px + 32 * PITCH + 18 * 4;
        printf("  -> sampled %u,%u,%u,%u\n", c[0], c[1], c[2], c[3]);
        ID3D12PipelineState_Release(np);
    }

    // ---- tiled (reserved) resources --------------------------------------------------------------------
    ID3D12Resource *reserved = NULL;
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0 };
    HRESULT hr = ID3D12Device_CreateReservedResource(dev, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&reserved);
    printf("CreateReservedResource: %s\n", FAILED(hr) && !reserved ? "fails cleanly" : "created");
    if (reserved) ID3D12Resource_Release(reserved);

    ID3D12PipelineState *all[] = { plain, pass5, pass6, expand, layer, so_only, so_rast, noclip, texpso };
    for (unsigned i = 0; i < sizeof all / sizeof *all; i++) ID3D12PipelineState_Release(all[i]);
    ID3D12RootSignature_Release(rs_draw); ID3D12RootSignature_Release(rs_tex);
    ID3D12DescriptorHeap_Release(rtv_heap); ID3D12DescriptorHeap_Release(srv_heap); ID3D12DescriptorHeap_Release(smp_heap);
    ID3D12Resource_Release(tex); ID3D12Resource_Release(pred); ID3D12Resource_Release(rb); ID3D12Resource_Release(rt);
    ID3D12Fence_Release(fence); ID3D12GraphicsCommandList_Release(cl); ID3D12CommandAllocator_Release(alloc); ID3D12CommandQueue_Release(q);
    CloseHandle(ev);
    printf("device released, refs=%u\nok\n", (unsigned)ID3D12Device_Release(dev));
    return 0;
}
