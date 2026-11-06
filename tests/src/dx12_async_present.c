// Asynchronous DXGI presentation for D3D12 (runtime/d3d12_swapchain.c), in a hidden window (never shown).
// Each frame: CPU work (a spin of `cpu_ms`), then a GPU-heavy full-screen pixel shader into the back buffer, Present.
//   1. synchronous presentation (WINTOMAC_DXGI_SYNC_PRESENT=1, read when the swapchain is created)
//   2. asynchronous presentation (the default): the CPU builds frame n+1 while the GPU renders frame n
//   3. the frame-latency waitable with SetMaximumFrameLatency(1) and (2): after each wait, every frame older than
//      the latency must have finished on the GPU, and no wait may time out
//   4. the last presented buffer is read back and checked
// Frame times depend on the machine, so they are printed only with WINTOMAC_BENCH=1 (to stdout); `make check`
// compares the rest with tests/expected/dx12_async_present.out.
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "dx12_gs_blobs.h"

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08x\n", #x, (unsigned)hr_); return 1; } } while (0)
#define FRAMES 48
#define SIZE 512

static ID3D12Device *dev; static ID3D12CommandQueue *q; static ID3D12CommandAllocator *alloc[3]; static ID3D12GraphicsCommandList *cl;
static ID3D12Fence *fence; static UINT64 fval; static HANDLE ev;
static ID3D12RootSignature *rs; static ID3D12PipelineState *pso; static ID3D12DescriptorHeap *rtv_heap; static ID3D12Resource *rb;
static IDXGIFactory4 *fac; static HWND hwnd; static int bench; static UINT iterations = 300;
static LARGE_INTEGER qpf;

static double now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / qpf.QuadPart; }
static void spin(double ms) { double end = now_ms() + ms; while (now_ms() < end) {} }
static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}
static void wait_fence(UINT64 v) {
    if (ID3D12Fence_GetCompletedValue(fence) >= v) return;
    ID3D12Fence_SetEventOnCompletion(fence, v, ev); WaitForSingleObject(ev, 10000);
}

// Record + submit one frame into the current back buffer; signals `fence` = frame number afterwards.
static int render(IDXGISwapChain3 *sc, UINT frame, int readback) {
    UINT idx = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
    ID3D12Resource *bb; CHECK(IDXGISwapChain3_GetBuffer(sc, idx, &IID_ID3D12Resource, (void **)&bb));
    // allocator idx was last used by the frame that rendered this buffer; that frame is done once the swapchain
    // hands the buffer back, but check the fence anyway, as games do
    if (frame >= 3) wait_fence(frame - 2);
    ID3D12CommandAllocator_Reset(alloc[frame % 3]);
    ID3D12GraphicsCommandList_Reset(cl, alloc[frame % 3], pso);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    rtv.ptr += idx * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Device_CreateRenderTargetView(dev, bb, NULL, rtv);
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    UINT k[2] = { iterations, frame };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 2, k, 0);
    D3D12_VIEWPORT vp = { 0, 0, SIZE, SIZE, 0, 1 }; D3D12_RECT scr = { 0, 0, SIZE, SIZE };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp); ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &scr);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    if (readback) {
        barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, src = { bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dst.PlacedFootprint.Footprint.Width = SIZE;
        dst.PlacedFootprint.Footprint.Height = SIZE; dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = SIZE * 4;
        src.SubresourceIndex = 0;
        ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    } else barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    CHECK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandList *l[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, l);
    CHECK(ID3D12CommandQueue_Signal(q, fence, frame + 1));
    ID3D12Resource_Release(bb);
    return 0;
}

static IDXGISwapChain3 *make_swapchain(UINT flags) {
    DXGI_SWAP_CHAIN_DESC1 sd = { SIZE, SIZE, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, 3,
                                 DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, flags };
    IDXGISwapChain1 *sc1 = NULL; IDXGISwapChain3 *sc = NULL;
    if (FAILED(IDXGIFactory4_CreateSwapChainForHwnd(fac, (IUnknown *)q, hwnd, &sd, NULL, NULL, &sc1))) return NULL;
    IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc);
    IDXGISwapChain1_Release(sc1);
    return sc;
}

// Plain loop: CPU work, render, Present(0). Returns the average frame time in ms.
static double run_plain(const char *name, double cpu_ms) {
    IDXGISwapChain3 *sc = make_swapchain(0);
    if (!sc) { printf("FAIL swapchain\n"); return -1; }
    wait_fence(fval);
    double t0 = 0, worst = 0, last = 0;
    for (UINT f = 0; f < FRAMES; f++) {
        if (f == 4) t0 = last = now_ms();   // skip warm-up (pipeline compilation, first acquire)
        spin(cpu_ms);
        if (render(sc, (UINT)fval + f, 0)) return -1;
        IDXGISwapChain3_Present(sc, 0, 0);
        if (f >= 4) { double t = now_ms(); if (t - last > worst) worst = t - last; last = t; }
    }
    fval += FRAMES; wait_fence(fval);
    double avg = (now_ms() - t0) / (FRAMES - 4);
    UINT pc = 0; IDXGISwapChain3_GetLastPresentCount(sc, &pc);
    printf("%s: %u presents\n", name, pc);
    if (bench) printf("  [bench] %s: %.2f ms/frame (worst %.2f) with %.1f ms CPU work per frame\n", name, avg, worst, cpu_ms);
    IDXGISwapChain3_Release(sc);
    return avg;
}

// Waitable object with latency L: after each wait, frames older than L must have finished on the GPU.
static int run_waitable(UINT latency) {
    IDXGISwapChain3 *sc = make_swapchain(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (!sc) { printf("FAIL waitable swapchain\n"); return 1; }
    IDXGISwapChain2 *sc2; CHECK(IDXGISwapChain3_QueryInterface(sc, &IID_IDXGISwapChain2, (void **)&sc2));
    CHECK(IDXGISwapChain2_SetMaximumFrameLatency(sc2, latency));
    HANDLE w = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
    wait_fence(fval);
    UINT64 base = fval; int timeouts = 0, too_far = 0;
    for (UINT f = 0; f < FRAMES; f++) {
        if (WaitForSingleObject(w, 5000) != WAIT_OBJECT_0) timeouts++;
        // frames base+1 .. base+f were submitted; at most `latency` of them may still be in flight
        UINT64 done = ID3D12Fence_GetCompletedValue(fence);
        if (f > latency && done + latency < base + f) too_far++;
        spin(1.0);
        if (render(sc, (UINT)(base + f), f == FRAMES - 1)) return 1;
        IDXGISwapChain2_Present(sc2, 0, 0);
    }
    fval = base + FRAMES; wait_fence(fval);
    printf("waitable, latency %u: %d frames, %d wait timeouts, %d frames started with more than %u in flight\n",
           latency, FRAMES, timeouts, too_far, latency);
    CloseHandle(w);
    IDXGISwapChain2_Release(sc2);
    IDXGISwapChain3_Release(sc);
    return timeouts || too_far;
}

int main(void) {
    QueryPerformanceFrequency(&qpf);
    char b[8]; bench = GetEnvironmentVariableA("WINTOMAC_BENCH", b, sizeof b) > 0;
    WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "dx12async";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "dx12async", "dx12_async_present", WS_OVERLAPPEDWINDOW /* never shown */, 0, 0, SIZE, SIZE, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("FAIL CreateWindowEx\n"); return 1; }
    printf("window visible=%d\n", IsWindowVisible(hwnd));
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&fac));
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    for (int i = 0; i < 3; i++)
        CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc[i]));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    ID3D12GraphicsCommandList_Close(cl);
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    D3D12_HEAP_PROPERTIES rp = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, SIZE * SIZE * 4, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb));
    D3D12_ROOT_PARAMETER p; memset(&p, 0, sizeof p);
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p.Constants.Num32BitValues = 2; p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &p, 0, NULL, 0 };
    ID3D10Blob *blob, *err = NULL;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    ID3D10Blob_Release(blob);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = gs_vs_full_dxbc; pd.VS.BytecodeLength = sizeof gs_vs_full_dxbc;
    pd.PS.pShaderBytecode = gs_ps_heavy_dxbc; pd.PS.BytecodeLength = sizeof gs_ps_heavy_dxbc;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));

    // Calibrate: make one frame's GPU work take roughly as long as the CPU work (so overlap is visible).
    if (bench) {
        IDXGISwapChain3 *sc = make_swapchain(0);
        for (int pass = 0; pass < 6 && sc; pass++) {
            double t = now_ms();
            render(sc, (UINT)fval, 0); fval++; wait_fence(fval);
            double g = now_ms() - t;
            if (pass >= 2 && g > 0.5) { iterations = (UINT)(iterations * 6.0 / g); if (iterations < 50) iterations = 50; }
            IDXGISwapChain3_Present(sc, 0, 0);
        }
        if (sc) IDXGISwapChain3_Release(sc);
        printf("  [bench] %u shader iterations per pixel\n", iterations);
    }

    SetEnvironmentVariableA("WINTOMAC_DXGI_SYNC_PRESENT", "1");
    double t_sync = run_plain("synchronous present", 6.0);
    SetEnvironmentVariableA("WINTOMAC_DXGI_SYNC_PRESENT", NULL);
    double t_async = run_plain("asynchronous present", 6.0);
    if (t_sync < 0 || t_async < 0) return 1;
    if (bench) {
        printf("  [bench] async/sync frame time: %.2f\n", t_async / t_sync);
        run_plain("asynchronous present, GPU only", 0.0);   // the GPU-bound floor
    }
    if (run_waitable(1) || run_waitable(2)) printf("FAIL frame latency\n");

    // the last waitable frame was read back (frame number base+FRAMES-1): R = frame & 255, G = 128, B = frame % 7 / 7
    uint8_t *px; D3D12_RANGE rr = { 0, SIZE * SIZE * 4 };
    CHECK(ID3D12Resource_Map(rb, 0, &rr, (void **)&px));
    UINT last = (UINT)fval - 1;
    uint8_t er = (uint8_t)(last & 255), eb = (uint8_t)((last % 7) * 255 / 7.0 + 0.5);
    uint8_t *c = px + (SIZE / 2 * SIZE + SIZE / 2) * 4;
    int ok = abs(c[0] - er) <= 1 && abs(c[1] - 128) <= 1 && abs(c[2] - eb) <= 1 && c[3] == 255;
    printf("last frame pixels %s\n", ok ? "correct" : "WRONG");
    if (!ok) printf("  got %u,%u,%u,%u want %u,128,%u,255\n", c[0], c[1], c[2], c[3], er, eb);
    D3D12_RANGE nr = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &nr);

    ID3D12PipelineState_Release(pso); ID3D12RootSignature_Release(rs); ID3D12Resource_Release(rb); ID3D12DescriptorHeap_Release(rtv_heap);
    ID3D12Fence_Release(fence); ID3D12GraphicsCommandList_Release(cl);
    for (int i = 0; i < 3; i++) ID3D12CommandAllocator_Release(alloc[i]);
    ID3D12CommandQueue_Release(q);
    printf("device released, refs=%u\n", (unsigned)ID3D12Device_Release(dev));
    IDXGIFactory4_Release(fac); CloseHandle(ev); DestroyWindow(hwnd);
    printf("window still hidden=%d\nok\n", !IsWindowVisible(hwnd));
    return 0;
}
