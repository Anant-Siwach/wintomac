// XInput mapping test. Enables the runtime's virtual gamepad (scripted: A + D-pad up + right shoulder held,
// left stick up-right, right stick fully up, right trigger fully pressed) and checks what XInput reports.
#include <windows.h>
#include <xinput.h>
#include <stdio.h>

int main(void) {
    SetEnvironmentVariableA("WINTOMAC_VIRTUAL_PAD", "1");   // runtime test hook (ignored on real Windows)
    XINPUT_STATE s = { 0 }, s2 = { 0 };
    DWORD r0 = XInputGetState(0, &s), r1 = XInputGetState(1, &s2);
    printf("slot0=%s slot1=%s\n", r0 == ERROR_SUCCESS ? "connected" : "absent", r1 == ERROR_DEVICE_NOT_CONNECTED ? "not connected" : "??");
    if (r0) return 1;
    XINPUT_GAMEPAD *g = &s.Gamepad;
    printf("buttons=0x%04x (A=%d up=%d RB=%d B=%d)\n", g->wButtons, !!(g->wButtons & XINPUT_GAMEPAD_A), !!(g->wButtons & XINPUT_GAMEPAD_DPAD_UP),
           !!(g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER), !!(g->wButtons & XINPUT_GAMEPAD_B));
    printf("triggers L=%u R=%u\n", g->bLeftTrigger, g->bRightTrigger);
    printf("left stick (%d,%d) right stick (%d,%d)\n", g->sThumbLX, g->sThumbLY, g->sThumbRX, g->sThumbRY);
    DWORD p1 = s.dwPacketNumber; XInputGetState(0, &s);
    printf("packet number stable while idle: %s\n", s.dwPacketNumber == p1 ? "yes" : "no");
    XINPUT_CAPABILITIES caps; DWORD rc = XInputGetCapabilities(0, 0, &caps);
    printf("capabilities: %s type=%u subtype=%u\n", rc ? "FAILED" : "ok", caps.Type, caps.SubType);
    XINPUT_VIBRATION v = { 20000, 40000 };
    printf("rumble: %s\n", XInputSetState(0, &v) == ERROR_SUCCESS ? "ok" : "FAILED");
    XINPUT_KEYSTROKE ks; printf("keystroke queue: %s\n", XInputGetKeystroke(0, 0, &ks) == ERROR_EMPTY ? "empty" : "??");
    return 0;
}
