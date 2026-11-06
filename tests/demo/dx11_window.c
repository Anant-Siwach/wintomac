// Direct3D 11 + DXGI windowed demo on DXVK: swapchain, HLSL compiled at runtime by D3DCompile, a dynamic
// constant buffer updated every frame, instanced drawing. Plain Win32; nothing here knows about macOS.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char hlsl[] =
    "cbuffer Frame : register(b0) { float time; float aspect; float2 pad; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VSOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  float a = time * 0.6 + iid * 0.3926991;                       // 16 instances around a ring\n"
    "  float r = 0.55 + 0.1 * sin(time * 2.0 + iid);\n"
    "  float2 center = float2(cos(a), sin(a)) * r;\n"
    "  float t = -(vid * 2.0943951) + time * (iid % 2 ? 2.0 : -2.0); // clockwise winding: D3D11 culls CCW by default\n"
    "  float2 p = center + float2(cos(t), sin(t)) * 0.12;\n"
    "  VSOut o; o.pos = float4(p.x / aspect, p.y, 0, 1);\n"
    "  float h = frac(iid / 16.0 + time * 0.05);\n"
    "  o.col = float4(saturate(abs(h * 6 - 3) - 1), saturate(2 - abs(h * 6 - 2)), saturate(2 - abs(h * 6 - 4)), 1);\n"
    "  o.col.rgb *= 0.6 + 0.4 * (vid == 0);\n"
    "  return o; }\n"
    "float4 ps_main(VSOut i) : SV_Target { return i.col; }\n";

static int resized;
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SIZE) resized = 1;
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { DestroyWindow(h); return 0; }
    if (m == WM_KEYDOWN) printf("key vk=0x%02x\n", (unsigned)w);
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv) {
    int max_frames = argc > 1 ? atoi(argv[1]) : 0;
    WNDCLASSA wc = { 0, wndproc, 0, 0, GetModuleHandleA(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, "dx11demo" };
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "dx11demo", "Direct3D 11 on DXVK, recompiled for arm64 macOS", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 960, 600, NULL, NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2; sd.OutputWindow = hwnd; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain *sc; ID3D11Device *dev; ID3D11DeviceContext *ctx; D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDeviceAndSwapChain failed 0x%08lx\n", (unsigned long)hr); return 1; }
    IDXGIDevice *xdev; IDXGIAdapter *ad; DXGI_ADAPTER_DESC adesc;
    ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xdev); IDXGIDevice_GetAdapter(xdev, &ad); IDXGIAdapter_GetDesc(ad, &adesc);
    char name[128]; WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, name, sizeof name, NULL, NULL);
    printf("DXGI adapter: %s, feature level %x.%x\n", name, fl >> 12, (fl >> 8) & 0xF);

    ID3DBlob *vsb, *psb, *err = NULL;
    if (FAILED(D3DCompile(hlsl, sizeof hlsl - 1, "demo.hlsl", NULL, NULL, "vs_main", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(hlsl, sizeof hlsl - 1, "demo.hlsl", NULL, NULL, "ps_main", "ps_5_0", 0, 0, &psb, &err))) {
        printf("shader compile failed: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 1;
    }
    ID3D11VertexShader *vs; ID3D11PixelShader *ps;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    D3D11_BUFFER_DESC cbd = { 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    ID3D11Buffer *cb; ID3D11Device_CreateBuffer(dev, &cbd, NULL, &cb);

    ID3D11RenderTargetView *rtv = NULL; UINT W = 0, H = 0;
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    int frames = 0, running = 1;
    while (running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) running = 0; DispatchMessageA(&msg); }
        if (!running) break;
        if (resized || !rtv) {
            RECT rc; GetClientRect(hwnd, &rc); W = rc.right ? rc.right : 1; H = rc.bottom ? rc.bottom : 1;
            if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
            IDXGISwapChain_ResizeBuffers(sc, 0, W, H, DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D *bb; IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv); ID3D11Texture2D_Release(bb);
            resized = 0;
        }
        QueryPerformanceCounter(&t); float s = (float)(t.QuadPart - t0.QuadPart) / (float)f.QuadPart;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float *p = m.pData; p[0] = s; p[1] = (float)W / (float)H; ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
        }
        float clear[4] = { 0.03f, 0.04f, 0.09f + 0.03f * sinf(s), 1 };
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0); ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_DrawInstanced(ctx, 3, 16, 0, 0);
        if (FAILED(IDXGISwapChain_Present(sc, 1, 0))) { printf("Present failed\n"); break; }
        frames++;
        if (frames % 120 == 0) { printf("%d frames, %.1f fps\n", frames, frames / s); fflush(stdout); }
        if (max_frames && frames >= max_frames) running = 0;
    }
    printf("presented %d frames, exiting cleanly\n", frames);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    ID3D11Buffer_Release(cb); ID3D11PixelShader_Release(ps); ID3D11VertexShader_Release(vs);
    IDXGISwapChain_Release(sc); ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev);
    return 0;
}
