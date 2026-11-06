// XInput (xinput1_3 / 1_4 / 9_1_0) on SDL3 gamepads.
// SDL maps Xbox, PlayStation, Switch Pro, MFi and other controllers to one standard layout (on macOS through
// Apple's GameController framework), which is the same shape as an Xbox 360 pad, so XInput maps 1:1.
//
// Test hook: WINTOMAC_VIRTUAL_PAD=1 attaches an SDL virtual gamepad in slot 0 with a scripted state, so the
// mapping can be verified without hardware (tests/src/input_xinput.c).
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "win32.h"

#define ERROR_DEVICE_NOT_CONNECTED 1167
#define ERROR_EMPTY 4306
#define ERROR_BAD_ARGUMENTS 160

static SDL_Gamepad *pads[4];
static SDL_JoystickID pad_ids[4];
static u32 packet[4];
static u8 last_state[4][12];
static int enabled = 1;
static u64 last_scan;
static SDL_Mutex *lock;

static SDL_Joystick *vpad;
static void apply_virtual_state(void) {   // re-applied every poll: SDL takes trigger baselines when the pad is opened
    if (!vpad) return;
    SDL_SetJoystickVirtualButton(vpad, SDL_GAMEPAD_BUTTON_SOUTH, true);         // A
    SDL_SetJoystickVirtualButton(vpad, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    SDL_SetJoystickVirtualButton(vpad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    SDL_SetJoystickVirtualAxis(vpad, SDL_GAMEPAD_AXIS_LEFTX, 16000);
    SDL_SetJoystickVirtualAxis(vpad, SDL_GAMEPAD_AXIS_LEFTY, -16000);           // SDL: up is negative
    SDL_SetJoystickVirtualAxis(vpad, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(vpad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    SDL_UpdateJoysticks();
}
static void attach_virtual_pad(void) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "wintomac virtual pad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    SDL_Joystick *j = id ? SDL_OpenJoystick(id) : NULL;
    if (!j) { rt_log("xinput: virtual pad failed: %s", SDL_GetError()); return; }
    vpad = j;
}

static void init_pads(void) {
    static int done;
    if (done) return;
    done = 1;
    lock = SDL_CreateMutex();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   // games poll XInput even when unfocused
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) rt_log("xinput: SDL gamepad init failed: %s", SDL_GetError());
    if (getenv("WINTOMAC_VIRTUAL_PAD")) attach_virtual_pad();
}

// Keep slots stable: a pad keeps its slot until disconnected; new pads fill the lowest free slot.
static void rescan(void) {
    u64 now = SDL_GetTicks();
    if (last_scan && now - last_scan < 250) return;
    last_scan = now;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int s = 0; s < 4; s++) {
        if (!pads[s]) continue;
        int still = 0;
        for (int i = 0; i < n; i++) if (ids[i] == pad_ids[s]) still = 1;
        if (!still || !SDL_GamepadConnected(pads[s])) { SDL_CloseGamepad(pads[s]); pads[s] = NULL; pad_ids[s] = 0; }
    }
    for (int i = 0; i < n; i++) {
        int have = 0;
        for (int s = 0; s < 4; s++) if (pads[s] && pad_ids[s] == ids[i]) have = 1;
        if (have) continue;
        for (int s = 0; s < 4; s++) if (!pads[s]) {
            pads[s] = SDL_OpenGamepad(ids[i]);
            if (pads[s]) { pad_ids[s] = ids[i]; if (rt_trace_enabled()) rt_log("xinput: slot %d = %s", s, SDL_GetGamepadName(pads[s])); }
            break;
        }
    }
    SDL_free(ids);
}

static i16 stick(i16 v, int invert) {  // SDL Y is positive-down, XInput positive-up
    if (!invert) return v;
    return v == -32768 ? 32767 : (i16)-v;
}
static u8 trig(i16 v) { return v <= 0 ? 0 : (u8)((v * 255 + 16383) / 32767); }

static int get_pad(u32 idx, int with_guide, u8 out[12]) {  // XINPUT_GAMEPAD
    if (idx > 3) return 0;
    init_pads();
    SDL_LockMutex(lock);
    SDL_UpdateGamepads();
    rescan();
    apply_virtual_state();
    SDL_UpdateGamepads();
    SDL_Gamepad *p = pads[idx];
    if (!p) { SDL_UnlockMutex(lock); return 0; }
    memset(out, 0, 12);
    if (enabled) {
        static const struct { SDL_GamepadButton b; u16 x; } map[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001 }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002 }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004 },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008 }, { SDL_GAMEPAD_BUTTON_START, 0x0010 }, { SDL_GAMEPAD_BUTTON_BACK, 0x0020 },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040 }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080 },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100 }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200 },
            { SDL_GAMEPAD_BUTTON_SOUTH, 0x1000 }, { SDL_GAMEPAD_BUTTON_EAST, 0x2000 }, { SDL_GAMEPAD_BUTTON_WEST, 0x4000 },
            { SDL_GAMEPAD_BUTTON_NORTH, 0x8000 } };
        u16 btn = 0;
        for (unsigned i = 0; i < sizeof map / sizeof *map; i++) if (SDL_GetGamepadButton(p, map[i].b)) btn |= map[i].x;
        if (with_guide && SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_GUIDE)) btn |= 0x0400;
        i16 lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ly = stick(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY), 1);
        i16 rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), ry = stick(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY), 1);
        memcpy(out, &btn, 2);
        out[2] = trig(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        out[3] = trig(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        memcpy(out + 4, &lx, 2); memcpy(out + 6, &ly, 2); memcpy(out + 8, &rx, 2); memcpy(out + 10, &ry, 2);
    }
    if (memcmp(out, last_state[idx], 12)) { packet[idx]++; memcpy(last_state[idx], out, 12); }
    SDL_UnlockMutex(lock);
    return 1;
}

static u32 get_state(u32 idx, u64 st, int with_guide) {
    u8 g[12];
    if (!st) return ERROR_BAD_ARGUMENTS;
    if (!get_pad(idx, with_guide, g)) return ERROR_DEVICE_NOT_CONNECTED;
    ST32(st, packet[idx]); memcpy((void *)(st + 4), g, 12);
    return 0;
}
SHIM(XInputGetState) { RET(get_state((u32)A1, A2, 0)); }
SHIM(XInputGetStateEx) { RET(get_state((u32)A1, A2, 1)); }     // undocumented ordinal 100: includes the Guide button

SHIM(XInputSetState) {  // (idx, XINPUT_VIBRATION { WORD left, right })
    u8 g[12];
    if (!get_pad((u32)A1, 0, g)) RET(ERROR_DEVICE_NOT_CONNECTED);
    u16 lo = enabled ? LD16(A2) : 0, hi = enabled ? LD16(A2 + 2) : 0;
    SDL_RumbleGamepad(pads[(u32)A1], lo, hi, (lo || hi) ? 5000 : 0);   // XInput rumble lasts until changed
    RET(0);
}
SHIM(XInputGetCapabilities) {  // (idx, flags, XINPUT_CAPABILITIES*)
    u8 g[12];
    if (!get_pad((u32)A1, 0, g)) RET(ERROR_DEVICE_NOT_CONNECTED);
    u64 p = A3; memset((void *)p, 0, 20);
    ST8(p, 1); ST8(p + 1, 1);                                   // XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD
    ST16(p + 2, 0x0004);                                        // XINPUT_CAPS_FFB_SUPPORTED
    ST16(p + 4, 0xF3FF); ST8(p + 6, 0xFF); ST8(p + 7, 0xFF);    // every button / full trigger range
    for (int i = 0; i < 4; i++) ST16(p + 8 + 2 * i, 0xFFC0);    // stick resolution
    ST16(p + 16, 0xFFFF); ST16(p + 18, 0xFFFF);                 // vibration motors
    RET(0);
}
SHIM(XInputGetBatteryInformation) {  // (idx, devtype, XINPUT_BATTERY_INFORMATION { BYTE type, level })
    u8 g[12];
    if (!get_pad((u32)A1, 0, g)) RET(ERROR_DEVICE_NOT_CONNECTED);
    int pct = -1; SDL_PowerState ps = SDL_GetGamepadPowerInfo(pads[(u32)A1], &pct);
    ST8(A3, ps == SDL_POWERSTATE_ON_BATTERY ? 2 /* ALKALINE */ : 1 /* WIRED */);
    ST8(A3 + 1, pct < 0 ? 3 : pct > 70 ? 3 : pct > 35 ? 2 : pct > 10 ? 1 : 0);
    RET(0);
}
SHIM(XInputGetKeystroke) { u8 g[12]; RET(get_pad((u32)A1, 0, g) ? ERROR_EMPTY : ERROR_DEVICE_NOT_CONNECTED); }
SHIM(XInputPowerOffController) { RET(0); }
SHIM(XInputEnable) { enabled = (u32)A1 != 0; RET(0); }
SHIM(XInputGetDSoundAudioDeviceGuids) { u8 g[12]; if (!get_pad((u32)A1, 0, g)) RET(ERROR_DEVICE_NOT_CONNECTED); memset((void *)A2, 0, 16); memset((void *)A3, 0, 16); RET(0); }
SHIM(XInputGetAudioDeviceIds) { u8 g[12]; if (!get_pad((u32)A1, 0, g)) RET(ERROR_DEVICE_NOT_CONNECTED); if (A3) ST32(A3, 0); if (AN(5)) ST32(AN(5), 0); RET(0); }

const Shim shims_xinput[] = {
#define E(n) { NULL, #n, sh_##n }
    E(XInputGetState), E(XInputGetStateEx), E(XInputSetState), E(XInputGetCapabilities), E(XInputGetBatteryInformation),
    E(XInputGetKeystroke), E(XInputPowerOffController), E(XInputEnable), E(XInputGetDSoundAudioDeviceGuids), E(XInputGetAudioDeviceIds),
#undef E
    { 0, 0, 0 }
};
