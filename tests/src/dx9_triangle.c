// Direct3D 9 fixed-function triangle, rendered offscreen and read back. Runs on DXVK (recompiled d3d9.dll)
// -> our Vulkan bridge -> MoltenVK -> Metal. No shaders: D3D9's fixed-function pipeline does the work.
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdint.h>

#define W 64
#define H 64
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08lx\n", #x, (unsigned long)hr_); return 1; } } while (0)
typedef struct { float x, y, z, rhw; DWORD color; } Vtx;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(void) {
    WNDCLASSA wc = { 0, wndproc, 0, 0, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "dx9test" };
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "dx9test", "dx9", WS_OVERLAPPEDWINDOW, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("FAIL Direct3DCreate9\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 id; CHECK(IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id));
    printf("adapter: %s\n", id.Description[0] ? "present" : "empty");
    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    IDirect3DDevice9 *dev;
    CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));

    Vtx tri[3] = { { 32.f, 6.f, 0.5f, 1.f, 0xFFFF2020 }, { 58.f, 56.f, 0.5f, 1.f, 0xFF20FF20 }, { 6.f, 56.f, 0.5f, 1.f, 0xFF2040FF } };
    CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(13, 13, 26), 1.0f, 0));
    CHECK(IDirect3DDevice9_BeginScene(dev));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(Vtx)));
    CHECK(IDirect3DDevice9_EndScene(dev));

    IDirect3DSurface9 *bb, *sys;
    CHECK(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL));
    CHECK(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys));
    D3DLOCKED_RECT lr; CHECK(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY));
    int lit = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint32_t p = ((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[x]; if ((p & 0xFFFFFF) != 0x0D0D1A) lit++; }
    uint32_t corner = ((uint32_t *)lr.pBits)[0] & 0xFFFFFF, center = ((uint32_t *)((uint8_t *)lr.pBits + 40 * lr.Pitch))[32] & 0xFFFFFF;
    printf("corner=%06x center_has_rgb=%d%d%d lit_pixels_in_range=%d\n", corner, (center >> 16 & 0xFF) > 40, (center >> 8 & 0xFF) > 40, (center & 0xFF) > 40, lit > 1100 && lit < 1500);
    for (int y = 0; y < H; y += 4) {
        for (int x = 0; x < W; x += 2) {
            uint32_t p = ((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[x]; int r = p >> 16 & 0xFF, g = p >> 8 & 0xFF, b = p & 0xFF;
            putchar(r > 128 && r > g && r > b ? 'R' : g > 128 && g > b ? 'G' : b > 128 ? 'B' : (r > 40 || g > 40 || b > 60) ? '+' : '.');
        }
        putchar('\n');
    }
    IDirect3DSurface9_UnlockRect(sys);
    CHECK(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL));
    IDirect3DSurface9_Release(sys); IDirect3DSurface9_Release(bb);
    IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
    DestroyWindow(hwnd);
    printf("ok\n");
    return 0;
}
