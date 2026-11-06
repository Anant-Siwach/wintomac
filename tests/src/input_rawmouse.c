// Raw input: device enumeration, WM_INPUT + GetRawInputData (as most games read it), GetRawInputBuffer (as SDL3
// reads it), and the "mouse look" pattern (raw input + hidden cursor + tiny ClipCursor => host relative mode).
// Mouse events are scripted by a runtime test hook: +10.5,-3 / +0.5,+1 / -4,+0.25, left click, wheel down.
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetEnvironmentVariableA("WINTOMAC_TEST_RAW_MOUSE", "1");
    SetEnvironmentVariableA("WINTOMAC_TRACE_INPUT", "1");
    UINT n = 0; GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    RAWINPUTDEVICELIST list[8]; UINT got = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
    RID_DEVICE_INFO info = { sizeof info }; UINT isz = sizeof info;
    GetRawInputDeviceInfoA(list[0].hDevice, RIDI_DEVICEINFO, &info, &isz);
    char name[256]; UINT nsz = sizeof name; UINT nlen = GetRawInputDeviceInfoA(list[0].hDevice, RIDI_DEVICENAME, name, &nsz);
    printf("devices=%u types=%lu,%lu mouse buttons=%lu name_ok=%s\n", got, list[0].dwType, list[1].dwType, info.mouse.dwNumberOfButtons,
           nlen > 4 && !strncmp(name, "\\\\?\\HID", 7) ? "yes" : "no");

    WNDCLASSA wc = { 0, wndproc, 0, 0, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "raw" }; RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "raw", "raw input", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
    RAWINPUTDEVICE rid = { 1, 2, 0, hwnd };
    printf("register: %d\n", RegisterRawInputDevices(&rid, 1, sizeof rid));
    long sx = 0, sy = 0; int msgs = 0, down = 0, up = 0, wheel = 0;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 1000 && msgs < 6) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message != WM_INPUT) continue;
            UINT sz = 0; GetRawInputData((HRAWINPUT)m.lParam, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER));
            RAWINPUT ri; sz = sizeof ri;
            if (GetRawInputData((HRAWINPUT)m.lParam, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != sizeof ri || ri.header.dwType != RIM_TYPEMOUSE) continue;
            msgs++; sx += ri.data.mouse.lLastX; sy += ri.data.mouse.lLastY;
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) down++;
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) up++;
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) wheel = (SHORT)ri.data.mouse.usButtonData;
        }
        Sleep(5);
    }
    printf("WM_INPUT packets=%d sum dx=%ld dy=%ld left down/up=%d/%d wheel=%d\n", msgs, sx, sy, down, up, wheel);

    // Same events again, read the SDL3 way: drain with GetRawInputBuffer after MsgWaitForMultipleObjects(QS_RAWINPUT).
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    DWORD w = MsgWaitForMultipleObjects(0, NULL, FALSE, 1000, QS_RAWINPUT);
    Sleep(50);
    { MSG m; while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {} }   // let the pump deliver everything first
    RAWINPUT buf[16]; UINT bsz = sizeof buf; long bx = 0, by = 0; UINT total = 0, k;
    while ((k = GetRawInputBuffer(buf, &bsz, sizeof(RAWINPUTHEADER))) > 0 && k != (UINT)-1) {
        for (UINT i = 0; i < k; i++) { bx += buf[i].data.mouse.lLastX; by += buf[i].data.mouse.lLastY; }
        total += k; bsz = sizeof buf;
    }
    printf("MsgWait(QS_RAWINPUT)=%s, GetRawInputBuffer packets=%u sum dx=%ld dy=%ld\n", w == WAIT_OBJECT_0 ? "input" : "timeout", total, bx, by);

    ShowCursor(FALSE); RECT r = { 100, 100, 102, 102 }; ClipCursor(&r);    // mouse look on
    ClipCursor(NULL); ShowCursor(TRUE);                                       // and off again
    rid.dwFlags = RIDEV_REMOVE; rid.hwndTarget = NULL; RegisterRawInputDevices(&rid, 1, sizeof rid);
    DestroyWindow(hwnd);
    return 0;
}
