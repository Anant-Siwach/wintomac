// Direct3D 9 windowed demo on DXVK: a spinning, color-cycling triangle fan plus an orbiting triangle.
// Plain Win32 + D3D9 fixed function; nothing here knows it is running on a Mac.
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { float x, y, z, rhw; DWORD color; } Vtx;
static int running = 1;
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { DestroyWindow(h); return 0; }
    if (m == WM_KEYDOWN) printf("key vk=0x%02x\n", (unsigned)w);
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}
static DWORD hsv(float h) {  // hue 0..1 -> ARGB
    float r = fabsf(h * 6 - 3) - 1, g = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
    r = r < 0 ? 0 : r > 1 ? 1 : r; g = g < 0 ? 0 : g > 1 ? 1 : g; b = b < 0 ? 0 : b > 1 ? 1 : b;
    return 0xFF000000 | ((DWORD)(r * 255) << 16) | ((DWORD)(g * 255) << 8) | (DWORD)(b * 255);
}

int main(int argc, char **argv) {
    int max_frames = argc > 1 ? atoi(argv[1]) : 0;
    WNDCLASSA wc = { 0, wndproc, 0, 0, GetModuleHandleA(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, "dx9demo" };
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "dx9demo", "Direct3D 9 on DXVK, recompiled for arm64 macOS", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9 *dev;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) { printf("CreateDevice failed\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 id; IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id); printf("D3D9 adapter: %s\n", id.Description);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    int frames = 0;
    while (running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) running = 0; DispatchMessageA(&msg); }
        if (!running) break;
        RECT rc; GetClientRect(hwnd, &rc);
        float W = (float)rc.right, H = (float)rc.bottom, cx = W / 2, cy = H / 2, R = (W < H ? W : H) * 0.38f;
        QueryPerformanceCounter(&t); float s = (float)(t.QuadPart - t0.QuadPart) / (float)f.QuadPart;
        Vtx fan[26]; fan[0] = (Vtx){ cx, cy, 0.5f, 1, 0xFFFFFFFF };
        for (int i = 0; i <= 24; i++) {
            float a = s * 0.8f + i * 6.2831853f / 24, rr = R * (0.75f + 0.25f * sinf(s * 3 + i * 0.9f));
            fan[i + 1] = (Vtx){ cx + rr * cosf(a), cy + rr * sinf(a), 0.5f, 1, hsv(fmodf(i / 24.f + s * 0.1f, 1.f)) };
        }
        Vtx orb[3]; float oa = -s * 1.7f, ox = cx + R * 1.15f * cosf(oa), oy = cy + R * 1.15f * sinf(oa);
        for (int i = 0; i < 3; i++) { float a = s * 4 + i * 2.0943951f; orb[i] = (Vtx){ ox + 28 * cosf(a), oy + 28 * sinf(a), 0.4f, 1, (i == 0 ? 0xC0FF4040 : i == 1 ? 0xC040FF40 : 0xC04060FF) }; }
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(10, 12, 28), 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 24, fan, sizeof(Vtx));
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, orb, sizeof(Vtx));
        IDirect3DDevice9_EndScene(dev);
        if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("Present failed\n"); break; }
        frames++;
        if (frames % 120 == 0) { printf("%d frames, %.1f fps\n", frames, frames / s); fflush(stdout); }
        if (max_frames && frames >= max_frames) running = 0;
    }
    printf("presented %d frames, exiting cleanly\n", frames);
    IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
    return 0;
}
