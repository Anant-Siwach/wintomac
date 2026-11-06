// Direct3D 12 presentation through DXGI, in a hidden window (never shown).
// dxgi.dll is DXVK's (recompiled with the test: adapters/outputs/factory); the swapchain is ours (IWineDXGISwapChainFactory
// on the D3D12 queue). Each frame clears the current back buffer to a different colour, presents, and then reads the
// presented buffer back, so the output is deterministic. Covers GetBuffer, GetCurrentBackBufferIndex, Present,
// the frame-latency waitable object, GetDesc/GetDesc1, Get/SetFullscreenState queries, ResizeBuffers (explicit and
// 0 = client size), GetParent/GetDevice/GetContainingOutput.
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08x\n", #x, (unsigned)hr_); return 1; } } while (0)

static ID3D12Device *dev; static ID3D12CommandQueue *q; static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *cl; static ID3D12Fence *fence; static UINT64 fval;
static ID3D12DescriptorHeap *rtv_heap; static ID3D12Resource *rb;

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}
static int submit_and_wait(void) {
    CHECK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandList *l[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, l);
    CHECK(ID3D12CommandQueue_Signal(q, fence, ++fval));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(ID3D12Fence_SetEventOnCompletion(fence, fval, ev));
    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { printf("FAIL fence timeout\n"); return 1; }
    CloseHandle(ev);
    CHECK(ID3D12CommandAllocator_Reset(alloc));
    CHECK(ID3D12GraphicsCommandList_Reset(cl, alloc, NULL));
    return 0;
}

// Clear the current back buffer, present it, read it back: prints its centre/corner pixels and a hash.
static int frame(IDXGISwapChain3 *sc, int n, const float col[4]) {
    UINT idx = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
    ID3D12Resource *bb; CHECK(IDXGISwapChain3_GetBuffer(sc, idx, &IID_ID3D12Resource, (void **)&bb));
    D3D12_RESOURCE_DESC d = ID3D12Resource_GetDesc(bb);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    ID3D12Device_CreateRenderTargetView(dev, bb, NULL, rtv);
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, col, 0, NULL);
    barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    if (submit_and_wait()) return 1;
    CHECK(IDXGISwapChain3_Present(sc, 1, 0));
    // read the presented buffer back
    UINT w = (UINT)d.Width, h = d.Height, pitch = (w * 4 + 255) & ~255u;
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, src = { bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    dst.PlacedFootprint.Footprint.Format = d.Format; dst.PlacedFootprint.Footprint.Width = w; dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch; src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    if (submit_and_wait()) return 1;
    uint8_t *px; D3D12_RANGE rr = { 0, (SIZE_T)pitch * h };
    CHECK(ID3D12Resource_Map(rb, 0, &rr, (void **)&px));
    uint64_t hs = 1469598103934665603ULL;
    for (UINT y = 0; y < h; y++) for (UINT i = 0; i < w * 4; i++) { hs ^= px[y * pitch + i]; hs *= 1099511628211ULL; }
    uint8_t *ce = px + (h / 2) * pitch + (w / 2) * 4, *co = px + (h - 1) * pitch + (w - 1) * 4;
    printf("frame %d: buffer %u (%ux%u) centre=%u,%u,%u,%u corner=%u,%u,%u,%u hash=%016llx\n", n, idx, w, h,
           ce[0], ce[1], ce[2], ce[3], co[0], co[1], co[2], co[3], (unsigned long long)hs);
    D3D12_RANGE none = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &none);
    ID3D12Resource_Release(bb);
    return 0;
}

static void print_desc(IDXGISwapChain3 *sc, const char *when) {
    DXGI_SWAP_CHAIN_DESC1 d1; DXGI_SWAP_CHAIN_DESC d; BOOL fs = 2; IDXGIOutput *o = (IDXGIOutput *)1;
    IDXGISwapChain3_GetDesc1(sc, &d1); IDXGISwapChain3_GetDesc(sc, &d); IDXGISwapChain3_GetFullscreenState(sc, &fs, &o);
    printf("%s: %ux%u fmt=%u buffers=%u effect=%u flags=%u | desc %ux%u windowed=%d | fullscreen=%d output=%d\n", when, d1.Width, d1.Height,
           d1.Format, d1.BufferCount, d1.SwapEffect, d1.Flags, d.BufferDesc.Width, d.BufferDesc.Height, d.Windowed, fs, o != NULL);
    if (o) IDXGIOutput_Release(o);
}

int main(void) {
    WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "dx12present";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "dx12present", "dx12_present", WS_OVERLAPPEDWINDOW /* no WS_VISIBLE: never shown */,
                                0, 0, 160, 120, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("FAIL CreateWindowEx\n"); return 1; }
    printf("window visible=%d\n", IsWindowVisible(hwnd));

    IDXGIFactory4 *fac; CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&fac));
    IDXGIAdapter1 *ad; CHECK(IDXGIFactory4_EnumAdapters1(fac, 0, &ad));
    CHECK(D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    printf("device on DXGI adapter 0 ok\n");
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 1024 * 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb));

    DXGI_SWAP_CHAIN_DESC1 sd = { 64, 48, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, 3,
                                 DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED,
                                 DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT };
    IDXGISwapChain1 *sc1; CHECK(IDXGIFactory4_CreateSwapChainForHwnd(fac, (IUnknown *)q, hwnd, &sd, NULL, NULL, &sc1));
    IDXGISwapChain3 *sc; CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));
    IDXGISwapChain1_Release(sc1);
    print_desc(sc, "created");

    IDXGIFactory4 *parent = NULL; CHECK(IDXGISwapChain3_GetParent(sc, &IID_IDXGIFactory4, (void **)&parent));
    ID3D12CommandQueue *qq = NULL; CHECK(IDXGISwapChain3_GetDevice(sc, &IID_ID3D12CommandQueue, (void **)&qq));
    IDXGIOutput *out = NULL; HRESULT ohr = IDXGISwapChain3_GetContainingOutput(sc, &out);
    printf("parent is factory=%d, device is queue=%d, containing output=%d\n", parent == fac, qq == q, SUCCEEDED(ohr) && out);
    if (out) IDXGIOutput_Release(out);
    IDXGIFactory4_Release(parent); ID3D12CommandQueue_Release(qq);

    IDXGISwapChain2 *sc2; CHECK(IDXGISwapChain3_QueryInterface(sc, &IID_IDXGISwapChain2, (void **)&sc2));
    CHECK(IDXGISwapChain2_SetMaximumFrameLatency(sc2, 2));
    HANDLE wait = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
    printf("latency waitable=%d\n", wait != NULL);

    static const float cols[5][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 0, 1 }, { 0, 1, 1, 1 } };
    for (int i = 0; i < 4; i++) {
        if (wait && WaitForSingleObject(wait, 2000) != WAIT_OBJECT_0) { printf("FAIL latency wait\n"); return 1; }
        if (frame(sc, i, cols[i])) return 1;
    }
    UINT pc = 0; IDXGISwapChain3_GetLastPresentCount(sc, &pc);
    printf("present count=%u\n", pc);

    CHECK(IDXGISwapChain3_ResizeBuffers(sc, 2, 96, 32, DXGI_FORMAT_B8G8R8A8_UNORM, sd.Flags));
    print_desc(sc, "resized");
    if (frame(sc, 4, cols[4]) || frame(sc, 5, cols[0])) return 1;
    CHECK(IDXGISwapChain3_ResizeBuffers(sc, 0, 0, 0, DXGI_FORMAT_UNKNOWN, sd.Flags));   // 0x0: the window's client size
    print_desc(sc, "client size");
    if (frame(sc, 6, cols[1])) return 1;

    // fullscreen state round trip (the window stays hidden: user32 only goes fullscreen for visible windows)
    CHECK(IDXGISwapChain3_SetFullscreenState(sc, TRUE, NULL));
    print_desc(sc, "fullscreen");
    CHECK(IDXGISwapChain3_SetFullscreenState(sc, FALSE, NULL));
    print_desc(sc, "windowed");
    printf("window still hidden=%d\n", !IsWindowVisible(hwnd));

    CloseHandle(wait);
    IDXGISwapChain2_Release(sc2);
    ULONG left = IDXGISwapChain3_Release(sc);
    printf("swapchain released, refs=%u\n", (unsigned)left);
    ID3D12Resource_Release(rb); ID3D12DescriptorHeap_Release(rtv_heap); ID3D12Fence_Release(fence);
    ID3D12GraphicsCommandList_Release(cl); ID3D12CommandAllocator_Release(alloc); ID3D12CommandQueue_Release(q);
    printf("device released, refs=%u\n", (unsigned)ID3D12Device_Release(dev));
    IDXGIAdapter1_Release(ad); IDXGIFactory4_Release(fac);
    DestroyWindow(hwnd);
    printf("ok\n");
    return 0;
}
