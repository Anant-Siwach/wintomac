// Display modes and fullscreen, the way games (and DXVK's DXGI/D3D9 fullscreen) do it: ChangeDisplaySettings
// to a mode, then a WS_POPUP window covering the screen. On macOS this becomes a virtual mode + borderless
// fullscreen; the game must see its own resolution everywhere. Trace lines come from the runtime.
#include <windows.h>
#include <stdio.h>

static int last_w, last_h;
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SIZE) { last_w = LOWORD(l); last_h = HIWORD(l); }
    return DefWindowProcA(h, m, w, l);
}
static void show_state(HWND hwnd, const char *what) {
    RECT cr, wr; GetClientRect(hwnd, &cr); GetWindowRect(hwnd, &wr);
    printf("%s: client %ldx%ld at (%ld,%ld), last WM_SIZE %dx%d\n", what, cr.right, cr.bottom, wr.left, wr.top, last_w, last_h);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetEnvironmentVariableA("WINTOMAC_TRACE_DISPLAY", "1");   // runtime hook: log virtual mode / fullscreen transitions
    DEVMODEA cur = { 0 }, reg = { 0 }, m = { 0 }; cur.dmSize = reg.dmSize = m.dmSize = sizeof(DEVMODEA);
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur); EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &reg);
    int n = 0; while (EnumDisplaySettingsA(NULL, n, &m)) n++;
    printf("modes listed: %s, current == registry: %s, bpp=%lu\n", n > 0 ? "yes" : "no",
           cur.dmPelsWidth == reg.dmPelsWidth && cur.dmPelsHeight == reg.dmPelsHeight ? "yes" : "no", cur.dmBitsPerPel);
    DWORD desk_w = cur.dmPelsWidth;

    DEVMODEA want = { 0 }; want.dmSize = sizeof want; want.dmPelsWidth = 1280; want.dmPelsHeight = 720; want.dmBitsPerPel = 32;
    want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    printf("CDS_TEST 1280x720: %ld\n", ChangeDisplaySettingsA(&want, CDS_TEST));
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur);
    printf("unchanged after test: %s\n", cur.dmPelsWidth == desk_w ? "yes" : "no");
    printf("CDS_FULLSCREEN 1280x720: %ld\n", ChangeDisplaySettingsA(&want, CDS_FULLSCREEN));
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur);
    MONITORINFO mi = { sizeof mi }; GetMonitorInfoA(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    printf("current mode %lux%lu, metrics %dx%d, monitor %ldx%ld\n", cur.dmPelsWidth, cur.dmPelsHeight,
           GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), mi.rcMonitor.right, mi.rcMonitor.bottom);

    WNDCLASSA wc = { 0, wndproc, 0, 0, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "fs" }; RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "fs", "fullscreen test", WS_POPUP | WS_VISIBLE, 0, 0, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    show_state(hwnd, "popup covering screen");
    SetWindowLongPtrA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPos(hwnd, NULL, 100, 100, 800, 600, SWP_NOZORDER | SWP_FRAMECHANGED);
    show_state(hwnd, "back to windowed");
    SetWindowLongPtrA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 1280, 720, SWP_FRAMECHANGED);
    show_state(hwnd, "fullscreen again");
    printf("restore desktop: %ld\n", ChangeDisplaySettingsA(NULL, 0));
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur);
    printf("current is desktop again: %s\n", cur.dmPelsWidth == desk_w ? "yes" : "no");
    DEVMODEA bad = want; bad.dmPelsWidth = 10; bad.dmPelsHeight = 10;
    printf("10x10 rejected: %s\n", ChangeDisplaySettingsA(&bad, 0) == DISP_CHANGE_BADMODE ? "yes" : "no");
    DestroyWindow(hwnd);
    return 0;
}
