// user32 / gdi32 / winmm / shell32 / shlwapi / ole32 / oleaut32 / uxtheme / crypt32 / dbghelp / ws2_32 extras and the
// small system DLLs games link against (avrt, hid, iphlpapi, winhttp, wtsapi32, userenv, comdlg32, comctl32, ...).
// Real behaviour where it matters (winmm multimedia timers, shlwapi paths, SHFileOperation, base64, PE image helpers,
// WSA buffer I/O); UI-only features answer with the documented failure value (WINTOMAC_STUBS=1 logs each once).
// References: Microsoft Learn API documentation; Wine's .spec files for ordinal numbers. No Wine code copied.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

#include "winstr.h"

static ShimFn shim_fn(const char *name) {
    extern const Shim shims_kernel32[], shims_threads[], shims_files[], shims_extra[], shims_extra2[], shims_misc[], shims_user32[],
        shims_winsock[], shims_com[], shims_kernel32x[];
    const Shim *t[] = { shims_kernel32, shims_threads, shims_files, shims_user32, shims_misc, shims_extra, shims_extra2, shims_winsock, shims_com, shims_kernel32x };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) for (const Shim *s = t[i]; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}
// Call another shim with n (<= 10) arguments; stack argument slots are borrowed and restored.
static u64 calln(Ctx *c, const char *name, int n, const u64 *a) {
    ShimFn f = shim_fn(name); if (!f) return 0;
    u64 sp = c->r[RSP], save_r[4] = { c->r[RCX], c->r[RDX], c->r[R8], c->r[R9] }, save_s[6];
    for (int i = 0; i < 6; i++) save_s[i] = LD64(sp + 40 + 8 * i);
    u64 r4[4] = { 0 }; for (int i = 0; i < n && i < 4; i++) r4[i] = a[i];
    c->r[RCX] = r4[0]; c->r[RDX] = r4[1]; c->r[R8] = r4[2]; c->r[R9] = r4[3];
    for (int i = 4; i < n; i++) ST64(sp + 8 * (i + 1), a[i]);
    f(c);
    u64 r = c->r[RAX];
    c->r[RCX] = save_r[0]; c->r[RDX] = save_r[1]; c->r[R8] = save_r[2]; c->r[R9] = save_r[3];
    for (int i = 0; i < 6; i++) ST64(sp + 40 + 8 * i, save_s[i]);
    return r;
}
#define CALL(name, ...) ({ u64 a_[] = { __VA_ARGS__ }; calln(c, name, (int)(sizeof a_ / 8), a_); })
#define E_NOTIMPL 0x80004001u
#define E_FAIL 0x80004005u
#define E_INVALIDARG 0x80070057u
static u64 obj_next = 0x7C000000;
static u64 new_handle(void) { return __atomic_add_fetch(&obj_next, 4, __ATOMIC_SEQ_CST); }

// ==== user32 ===========================================================================================================
SHIM(MapWindowPoints) {  // (from, to, POINT*, n) -> MAKELONG(dx, dy)
    i32 p[2] = { 0, 0 };
    if (A1) CALL("ClientToScreen", A1, (u64)p);
    if (A2) CALL("ScreenToClient", A2, (u64)p);
    for (u32 i = 0; i < (u32)A4; i++) { u64 q = A3 + 8ull * i; ST32(q, (u32)((i32)LD32(q) + p[0])); ST32(q + 4, (u32)((i32)LD32(q + 4) + p[1])); }
    RET((u32)((u16)p[0] | (u32)(u16)p[1] << 16));
}
SHIM(GetAncestor) {  // (hwnd, GA_PARENT 1 / GA_ROOT 2 / GA_ROOTOWNER 3)
    u64 h = A1; if (!h) RET(0);
    if ((u32)A2 == 1) { u64 p = CALL("GetParent", h); RET(p ? p : CALL("GetDesktopWindow")); }
    for (int i = 0; i < 64; i++) { u64 p = CALL("GetParent", h); if (!p) break; h = p; }
    RET(h);
}
SHIM(IsChild) { u64 h = A2; for (int i = 0; i < 64 && h; i++) { h = CALL("GetParent", h); if (h == A1) RET(1); } RET(0); }
SHIM(GetWindow) { RET(0); }               // no owned or sibling windows are tracked: "no such window"
SHIM(GetTopWindow) { RET(0); }
SHIM(GetNextWindow) { RET(0); }
SHIM(EnumChildWindows) { RET(0); }        // no child windows
SHIM(EnumThreadWindows) { RET(1); }
SHIM(FindWindowW) { RET(0); }             // no other application's windows exist on this side
SHIM(FindWindowA) { RET(0); }
SHIM(FindWindowExW) { RET(0); }
SHIM(FindWindowExA) { RET(0); }
SHIM(WindowFromPoint) { RET(CALL("GetForegroundWindow")); }
SHIM(ChildWindowFromPoint) { RET(A1); }
SHIM(ChildWindowFromPointEx) { RET(A1); }
SHIM(RealChildWindowFromPoint) { RET(A1); }
SHIM(BringWindowToTop) { CALL("SetForegroundWindow", A1); RET(1); }
SHIM(GetCursor) { RET(CALL("LoadCursorW", 0, 32512)); }   // IDC_ARROW
SHIM(CreateCursor) { RET(CALL("LoadCursorW", 0, 32512)); }
SHIM(GetCursorInfo) {  // CURSORINFO { cbSize, flags, hCursor, ptScreenPos }
    ST32(A1 + 4, 1); ST64(A1 + 8, CALL("LoadCursorW", 0, 32512)); CALL("GetCursorPos", A1 + 16); RET(1);
}
SHIM(GetIconInfo) { WX_FAIL(GetIconInfo, 1402, 0); }    // ERROR_INVALID_CURSOR_HANDLE
SHIM(GetIconInfoExW) { WX_FAIL(GetIconInfoExW, 1402, 0); }
SHIM(DrawIconEx) { RET(1); }
SHIM(DrawIcon) { RET(1); }
SHIM(CopyIcon) { RET(A1); }
static const u32 sys_colors[31] = {   // Windows 10 defaults (COLOR_SCROLLBAR .. COLOR_MENUBAR), 0x00BBGGRR
    0xC8C8C8, 0x000000, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000, 0x000000, 0x000000, 0xB4B4B4, 0xFCF7F4,
    0x544E4D, 0xD77800, 0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0x000000, 0x000000, 0xFFFFFF, 0x696969, 0xE3E3E3, 0x000000,
    0xE1FFFF, 0x000000, 0xCC6600, 0xEAD1B9, 0xF2E4D7, 0xD77800, 0xF0F0F0 };
SHIM(GetSysColor) { u32 i = (u32)A1; RET(i < 31 ? sys_colors[i] : 0); }
SHIM(GetSysColorBrush) { u32 i = (u32)A1; RET(CALL("CreateSolidBrush", i < 31 ? sys_colors[i] : 0)); }
SHIM(MessageBeep) { RET(1); }
SHIM(CreateCaret) { RET(1); }
SHIM(DestroyCaret) { RET(1); }
SHIM(SetCaretPos) { RET(1); }
SHIM(GetCaretPos) { ST32(A1, 0); ST32(A1 + 4, 0); RET(1); }
SHIM(ShowCaret) { RET(1); }
SHIM(HideCaret) { RET(1); }
SHIM(GetCaretBlinkTime) { RET(530); }
SHIM(SetCaretBlinkTime) { RET(1); }
SHIM(RemoveMenu) { RET(1); }
SHIM(TrackPopupMenuEx) { RET(0); }        // the menu is dismissed without a selection
SHIM(DrawMenuBar) { RET(1); }
SHIM(GetMenuItemCount) { RET(0); }
SHIM(GetMenuItemID) { RET((u32)-1); }
SHIM(GetSubMenu) { RET(0); }
SHIM(GetMenuItemRect) { WX_FAIL(GetMenuItemRect, 1401, 0); }   // ERROR_INVALID_MENU_HANDLE
SHIM(HiliteMenuItem) { RET(1); }
SHIM(InsertMenuItemW) { RET(1); }
SHIM(InsertMenuItemA) { RET(1); }
SHIM(ModifyMenuW) { RET(1); }
SHIM(ModifyMenuA) { RET(1); }
SHIM(AppendMenuA) { RET(1); }
SHIM(SetMenuDefaultItem) { RET(1); }
SHIM(SetMenuInfo) { RET(1); }
SHIM(GetMenuInfo) { RET(1); }
SHIM(GetMenuState) { RET((u32)-1); }
SHIM(SendMessageTimeoutW) { u64 r = CALL("SendMessageW", A1, A2, A3, A4); if (AN(7)) ST64(AN(7), r); RET(1); }   // (h, msg, w, l, flags, timeout, *result)
SHIM(SendMessageTimeoutA) { u64 r = CALL("SendMessageA", A1, A2, A3, A4); if (AN(7)) ST64(AN(7), r); RET(1); }
SHIM(SendNotifyMessageW) { CALL("PostMessageW", A1, A2, A3, A4); RET(1); }
SHIM(SetWinEventHook) { RET(new_handle()); }   // events are never raised
SHIM(UnhookWinEvent) { RET(1); }
SHIM(ChangeWindowMessageFilter) { RET(1); }
SHIM(ChangeWindowMessageFilterEx) { RET(1); }
SHIM(AllowSetForegroundWindow) { RET(1); }
SHIM(LockSetForegroundWindow) { RET(1); }
SHIM(DisableProcessWindowsGhosting) { RET(0); }
SHIM(IsHungAppWindow) { RET(0); }
SHIM(IsProcessDPIAware) { RET(1); }
SHIM(GetWindowDpiAwarenessContext) { RET((u64)-4); }   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
SHIM(GetDpiAwarenessContextForProcess) { RET((u64)-4); }
SHIM(LogicalToPhysicalPointForPerMonitorDPI) { RET(1); }
SHIM(PhysicalToLogicalPointForPerMonitorDPI) { RET(1); }
SHIM(LogicalToPhysicalPoint) { RET(1); }
SHIM(PhysicalToLogicalPoint) { RET(1); }
SHIM(RegisterPowerSettingNotification) { RET(new_handle()); }
SHIM(UnregisterPowerSettingNotification) { RET(1); }
SHIM(RegisterSuspendResumeNotification) { RET(new_handle()); }
SHIM(UnregisterSuspendResumeNotification) { RET(1); }
SHIM(AddClipboardFormatListener) { RET(1); }
SHIM(RemoveClipboardFormatListener) { RET(1); }
SHIM(SetClipboardViewer) { RET(0); }
SHIM(ChangeClipboardChain) { RET(1); }
SHIM(GetClipboardOwner) { RET(0); }
SHIM(SetCoalescableTimer) { RET(CALL("SetTimer", A1, A2, A3, A4)); }
SHIM(SetDisplayAutoRotationPreferences) { RET(1); }
SHIM(GetDisplayAutoRotationPreferences) { if (A1) ST32(A1, 0); RET(1); }
SHIM(SetWindowDisplayAffinity) { RET(1); }
SHIM(GetWindowDisplayAffinity) { if (A2) ST32(A2, 0); RET(1); }
SHIM(SkipPointerFrameMessages) { RET(1); }
SHIM(GetPointerInfo) { WX_FAIL(GetPointerInfo, 87, 0); }
SHIM(GetPointerFrameTouchInfo) { WX_FAIL(GetPointerFrameTouchInfo, 87, 0); }
SHIM(GetPointerFrameTouchInfoHistory) { WX_FAIL(GetPointerFrameTouchInfoHistory, 87, 0); }
SHIM(GetPointerPenInfoHistory) { WX_FAIL(GetPointerPenInfoHistory, 87, 0); }
SHIM(GetPointerTouchInfo) { WX_FAIL(GetPointerTouchInfo, 87, 0); }
SHIM(IsTouchWindow) { RET(0); }
SHIM(UpdateLayeredWindow) { WX_FAIL(UpdateLayeredWindow, 50, 0); }
SHIM(UpdateLayeredWindowIndirect) { WX_FAIL(UpdateLayeredWindowIndirect, 50, 0); }
SHIM(GetLayeredWindowAttributes) { WX_FAIL(GetLayeredWindowAttributes, 87, 0); }
SHIM(GetDCEx) { RET(CALL("GetDC", A1)); }
SHIM(GetWindowDC) { RET(CALL("GetDC", A1)); }
SHIM(WindowFromDC) { RET(user32_hwnd_from_dc(A1)); }
SHIM(GetClassLongPtrW) { RET(0); }
SHIM(GetClassLongPtrA) { RET(0); }
SHIM(GetClassLongW) { RET(0); }
SHIM(GetClassLongA) { RET(0); }
SHIM(GetClassInfoA) {  // (hinst, class, WNDCLASSA*): via GetClassInfoExA (same layout from offset 8, style at 0 vs 4)
    u8 ex[80] = { 0 }; ST32((u64)ex, 80);
    if (!CALL("GetClassInfoExA", A1, A2, (u64)ex)) RET(0);
    memcpy((void *)(A3 + 8), ex + 8, 64); ST32(A3, LD32((u64)ex + 4)); ST32(A3 + 4, 0); RET(1);
}
SHIM(LoadStringA) { if (A3 && (u32)A4) ST8(A3, 0); WX_FAIL(LoadStringA, 1814, 0); }   // no string-table resources
SHIM(LoadStringW) { if (A3 && (u32)A4) ST16(A3, 0); WX_FAIL(LoadStringW, 1814, 0); }
SHIM(MapVirtualKeyExA) { RET(CALL("MapVirtualKeyW", A1, A2)); }
SHIM(GetKeyboardLayoutNameA) { strcpy((char *)A1, "00000409"); RET(1); }
SHIM(GetKeyboardLayoutNameW) { wx_u2w("00000409", A1, 9); RET(1); }
SHIM(LoadKeyboardLayoutA) { RET(0x04090409); }
SHIM(LoadKeyboardLayoutW) { RET(0x04090409); }
SHIM(ActivateKeyboardLayout) { RET(0x04090409); }
SHIM(UnloadKeyboardLayout) { RET(1); }
SHIM(SendInput) { WX_FAIL(SendInput, 5, 0); }   // injecting input into the host is not allowed
SHIM(keybd_event) { RET(0); }
SHIM(mouse_event) { RET(0); }
SHIM(BlockInput) { WX_FAIL(BlockInput, 5, 0); }
SHIM(CreateDialogParamW) { WX_FAIL(CreateDialogParamW, 1814, 0); }   // dialog templates are resources we don't load
SHIM(CreateDialogParamA) { WX_FAIL(CreateDialogParamA, 1814, 0); }
SHIM(DialogBoxParamW) { WX_FAIL(DialogBoxParamW, 1814, (u64)-1); }
SHIM(DialogBoxParamA) { WX_FAIL(DialogBoxParamA, 1814, (u64)-1); }
SHIM(FrameRect) { RET(1); }
SHIM(DrawFocusRect) { RET(1); }
SHIM(DrawEdge) { RET(1); }
SHIM(InvertRect) { RET(1); }
SHIM(SystemParametersInfoForDpi) { RET(CALL("SystemParametersInfoW", A1, A2, A3, A4)); }
SHIM(CharPrevExA) { u64 s = A2, p = A3; RET(p > s ? p - 1 : s); }   // (cp, start, cur, flags): single-byte code page
SHIM(CharNextExA) { RET(LD8(A2) ? A2 + 1 : A2); }
SHIM(CharPrevA) { RET(A2 > A1 ? A2 - 1 : A1); }
SHIM(CharPrevW) { RET(A2 > A1 ? A2 - 2 : A1); }
SHIM(CharNextA) { RET(LD8(A1) ? A1 + 1 : A1); }
SHIM(CharNextW) { RET(LD16(A1) ? A1 + 2 : A1); }
static u64 char_case(u64 p, int wide, int upper) {   // a single character (high word 0) or a string
    if (!(p >> 16)) { u32 ch = (u32)p; return upper ? (u32)towupper(ch) : (u32)towlower(ch); }
    for (u64 q = p; wide ? LD16(q) : LD8(q); q += wide ? 2 : 1) {
        if (wide) ST16(q, (u16)(upper ? towupper(LD16(q)) : towlower(LD16(q))));
        else { u8 ch = LD8(q); ST8(q, (u8)(upper ? toupper(ch) : tolower(ch))); }
    }
    return p;
}
SHIM(CharUpperW) { RET(char_case(A1, 1, 1)); }
SHIM(CharLowerW) { RET(char_case(A1, 1, 0)); }
SHIM(CharUpperA) { RET(char_case(A1, 0, 1)); }
SHIM(CharLowerA) { RET(char_case(A1, 0, 0)); }
SHIM(CharUpperBuffW) { for (u32 i = 0; i < (u32)A2; i++) ST16(A1 + 2 * i, (u16)towupper(LD16(A1 + 2 * i))); RET((u32)A2); }
SHIM(CharLowerBuffW) { for (u32 i = 0; i < (u32)A2; i++) ST16(A1 + 2 * i, (u16)towlower(LD16(A1 + 2 * i))); RET((u32)A2); }
SHIM(IsCharAlphaW) { RET(iswalpha((u16)A1) ? 1 : 0); }
SHIM(IsCharAlphaNumericW) { RET(iswalnum((u16)A1) ? 1 : 0); }
SHIM(IsCharUpperW) { RET(iswupper((u16)A1) ? 1 : 0); }
SHIM(IsCharLowerW) { RET(iswlower((u16)A1) ? 1 : 0); }
SHIM(IsCharAlphaA) { RET(isalpha((u8)A1) ? 1 : 0); }

// ==== gdi32 (no software-drawn window contents: drawing calls succeed without output) ===================================
static u64 gdi_next = 0x4C000;
static u64 gdi_new(void) { return __atomic_add_fetch(&gdi_next, 1, __ATOMIC_SEQ_CST); }
SHIM(CreatePalette) { RET(gdi_new()); }
SHIM(SelectPalette) { RET(gdi_new()); }
SHIM(RealizePalette) { RET(0); }
SHIM(GetSystemPaletteEntries) { RET(0); }   // not a palette device
SHIM(GetSystemPaletteUse) { RET(1); }       // SYSPAL_STATIC
SHIM(SetSystemPaletteUse) { RET(1); }
SHIM(SetPaletteEntries) { RET((u32)A3); }
SHIM(GetPaletteEntries) { if (A4) memset((void *)A4, 0, 4ull * (u32)A3); RET((u32)A3); }
SHIM(AnimatePalette) { RET(1); }
SHIM(UnrealizeObject) { RET(1); }
SHIM(SetDIBColorTable) { RET((u32)A3); }
SHIM(GetDIBColorTable) { RET(0); }
SHIM(SetDIBitsToDevice) { RET((u32)AN(9)); }   // scan lines "drawn"
SHIM(StretchDIBits) { RET((u32)AN(8)); }
SHIM(SetDIBits) { RET((u32)A4); }
SHIM(GdiFlush) { RET(1); }
SHIM(GetRegionData) { RET(0); }
SHIM(CreatePolygonRgn) { RET(gdi_new()); }
SHIM(CreateEllipticRgn) { RET(gdi_new()); }
SHIM(CreateRoundRectRgn) { RET(gdi_new()); }
SHIM(OffsetRgn) { RET(2); }       // SIMPLEREGION
SHIM(SelectClipRgn) { RET(2); }
SHIM(ExtSelectClipRgn) { RET(2); }
SHIM(IntersectClipRect) { RET(2); }
SHIM(GetClipBox) { if (A2) memset((void *)A2, 0, 16); RET(1); }   // NULLREGION
SHIM(DPtoLP) { RET(1); }
SHIM(LPtoDP) { RET(1); }
SHIM(GetMapMode) { RET(1); }      // MM_TEXT
SHIM(SetMapMode) { RET(1); }
SHIM(SetGraphicsMode) { RET(1); }
SHIM(GetGraphicsMode) { RET(1); }
SHIM(SetWorldTransform) { RET(1); }
SHIM(ModifyWorldTransform) { RET(1); }
SHIM(SetLayout) { RET(0); }
SHIM(GetLayout) { RET(0); }
SHIM(SetTextAlign) { RET(0); }
SHIM(SetBkColor) { RET(0xFFFFFF); }
SHIM(GetBkColor) { RET(0xFFFFFF); }
SHIM(GetTextColor) { RET(0); }
SHIM(SetStretchBltMode) { RET(1); }
SHIM(SetBrushOrgEx) { if (AN(4)) {} RET(1); }
SHIM(SetViewportOrgEx) { if (A4) memset((void *)A4, 0, 8); RET(1); }
SHIM(SetWindowOrgEx) { if (A4) memset((void *)A4, 0, 8); RET(1); }
SHIM(MoveToEx) { if (A4) memset((void *)A4, 0, 8); RET(1); }
SHIM(LineTo) { RET(1); }
SHIM(Ellipse) { RET(1); }
SHIM(Polygon) { RET(1); }
SHIM(Polyline) { RET(1); }
SHIM(PatBlt) { RET(1); }
SHIM(SaveDC) { RET(1); }
SHIM(RestoreDC) { RET(1); }
SHIM(GetPixel) { RET(0xFFFFFFFF); }   // CLR_INVALID
SHIM(SetPixelV) { RET(1); }
SHIM(GetBitmapBits) { RET(0); }
SHIM(GetObjectW) { WX_FAIL(GetObjectW, 6, 0); }
SHIM(GetObjectA) { WX_FAIL(GetObjectA, 6, 0); }
SHIM(GetObjectType) { RET(A1 ? 7 : 0); }   // OBJ_BITMAP-ish for our opaque handles
SHIM(GetCurrentObject) { RET(gdi_new()); }
SHIM(AddFontResourceExW) { WX_FAIL(AddFontResourceExW, 2, 0); }   // no GDI font rasteriser
SHIM(AddFontResourceExA) { WX_FAIL(AddFontResourceExA, 2, 0); }
SHIM(AddFontResourceW) { WX_FAIL(AddFontResourceW, 2, 0); }
SHIM(AddFontResourceA) { WX_FAIL(AddFontResourceA, 2, 0); }
SHIM(RemoveFontResourceExW) { RET(0); }
SHIM(AddFontMemResourceEx) { WX_FAIL(AddFontMemResourceEx, 87, 0); }
SHIM(RemoveFontMemResourceEx) { RET(0); }
SHIM(EnumFontFamiliesExW) { RET(1); }   // no fonts enumerated
SHIM(EnumFontFamiliesExA) { RET(1); }
SHIM(EnumFontFamiliesW) { RET(1); }
SHIM(GetTextFaceW) { if (A3 && (u32)A2) ST16(A3, 0); RET(0); }
SHIM(GetTextFaceA) { if (A3 && (u32)A2) ST8(A3, 0); RET(0); }
SHIM(GetCharABCWidthsW) { WX_FAIL(GetCharABCWidthsW, 87, 0); }
SHIM(GetCharABCWidthsI) { WX_FAIL(GetCharABCWidthsI, 87, 0); }
SHIM(GetCharABCWidthsFloatW) { WX_FAIL(GetCharABCWidthsFloatW, 87, 0); }
SHIM(GetCharWidthI) { WX_FAIL(GetCharWidthI, 87, 0); }
SHIM(GetCharWidth32W) { WX_FAIL(GetCharWidth32W, 87, 0); }
SHIM(GetGlyphOutlineW) { WX_FAIL(GetGlyphOutlineW, 87, 0xFFFFFFFF); }   // GDI_ERROR
SHIM(GetGlyphIndicesW) { WX_FAIL(GetGlyphIndicesW, 87, 0xFFFFFFFF); }
SHIM(GetOutlineTextMetricsW) { WX_FAIL(GetOutlineTextMetricsW, 87, 0); }
SHIM(GetFontData) { WX_FAIL(GetFontData, 87, 0xFFFFFFFF); }
SHIM(GetKerningPairsW) { RET(0); }
SHIM(GetFontUnicodeRanges) { RET(0); }

// ==== winmm ==============================================================================================================
// Multimedia timers: one host thread per timer calls TimeProc(id, 0, user, 0, 0) or signals the event.
typedef struct { u32 id, delay, flags; u64 cb, user; volatile int dead; } MmTimer;
static MmTimer *mm_timers[64]; static pthread_mutex_t mm_lock = PTHREAD_MUTEX_INITIALIZER; static u32 mm_next = 1;
static void *mm_thread(void *arg) {
    MmTimer *t = arg; Ctx *c = rt_foreign_ctx();
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += (long)t->delay * 1000000; while (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
        for (;;) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            if (t->dead) goto out;
            i64 left = (i64)(next.tv_sec - now.tv_sec) * 1000000 + (next.tv_nsec - now.tv_nsec) / 1000;
            if (left <= 0) break;
            usleep(left > 2000 ? 2000 : (useconds_t)left);
        }
        if (t->dead) break;
        if (t->flags & 0x30) {   // TIME_CALLBACK_EVENT_SET / _PULSE
            c->r[RCX] = t->cb; shim_fn("SetEvent")(c);
            if (t->flags & 0x20) { usleep(1000); c->r[RCX] = t->cb; shim_fn("ResetEvent")(c); }
        } else { u64 a[5] = { t->id, 0, t->user, 0, 0 }; rt_guest_call(c, t->cb, 5, a); }
        if (!(t->flags & 1)) break;   // TIME_ONESHOT
    }
out:
    pthread_mutex_lock(&mm_lock);
    for (int i = 0; i < 64; i++) if (mm_timers[i] == t) mm_timers[i] = NULL;
    pthread_mutex_unlock(&mm_lock);
    free(t);
    return NULL;
}
SHIM(timeSetEvent) {  // (delay ms, resolution, callback, user, flags) -> timer id (0 on failure)
    if (!(u32)A1 || (u32)A1 > 1000000) RET(0);
    MmTimer *t = calloc(1, sizeof *t); t->delay = (u32)A1; t->cb = A3; t->user = A4; t->flags = (u32)AN(5);
    pthread_mutex_lock(&mm_lock);
    int slot = -1; for (int i = 0; i < 64; i++) if (!mm_timers[i]) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&mm_lock); free(t); RET(0); }
    t->id = mm_next++; mm_timers[slot] = t;
    pthread_mutex_unlock(&mm_lock);
    pthread_t th; pthread_create(&th, NULL, mm_thread, t); pthread_detach(th);
    RET(t->id);
}
SHIM(timeKillEvent) {
    pthread_mutex_lock(&mm_lock);
    for (int i = 0; i < 64; i++) if (mm_timers[i] && mm_timers[i]->id == (u32)A1) { mm_timers[i]->dead = 1; mm_timers[i] = NULL; pthread_mutex_unlock(&mm_lock); RET(0); }
    pthread_mutex_unlock(&mm_lock);
    RET(97);   // TIMERR_NOCANDO / MMSYSERR_INVALPARAM
}
SHIM(timeGetDevCaps) { if ((u32)A2 < 8) RET(97); ST32(A1, 1); ST32(A1 + 4, 1000000); RET(0); }   // TIMECAPS { min, max }
SHIM(timeGetSystemTime) { u32 t = (u32)CALL("timeGetTime"); ST32(A1, 4); ST32(A1 + 4, t); RET(0); }   // MMTIME { TIME_MS, ms }
// Joysticks through winmm: none attached (games use XInput/raw input instead).
SHIM(joyGetNumDevs) { RET(16); }
SHIM(joyGetPosEx) { RET(167); }    // JOYERR_UNPLUGGED
SHIM(joyGetPos) { RET(167); }
SHIM(joyGetDevCapsA) { RET(165); } // JOYERR_PARMS
SHIM(joyGetDevCapsW) { RET(165); }
SHIM(joySetCapture) { RET(165); }
SHIM(joyReleaseCapture) { RET(165); }
// MIDI / MCI / aux / mixer: no devices.
SHIM(midiOutGetNumDevs) { RET(0); }
SHIM(midiInGetNumDevs) { RET(0); }
#define MM_BADID(n) SHIM(n) { RET(2); }       /* MMSYSERR_BADDEVICEID */
#define MM_BADH(n) SHIM(n) { RET(5); }        /* MMSYSERR_INVALHANDLE */
MM_BADID(midiOutGetDevCapsA) MM_BADID(midiOutGetDevCapsW) MM_BADID(midiInGetDevCapsA) MM_BADID(midiInGetDevCapsW) MM_BADID(midiOutOpen)
MM_BADID(midiInOpen) MM_BADID(midiStreamOpen) MM_BADID(mixerOpen) MM_BADID(mixerGetDevCapsA) MM_BADID(mixerGetDevCapsW) MM_BADID(auxGetDevCapsA)
MM_BADH(midiOutClose) MM_BADH(midiOutShortMsg) MM_BADH(midiOutLongMsg) MM_BADH(midiOutPrepareHeader) MM_BADH(midiOutUnprepareHeader) MM_BADH(midiOutReset)
MM_BADH(midiOutGetVolume) MM_BADH(midiOutSetVolume) MM_BADH(midiInClose) MM_BADH(midiInStart) MM_BADH(midiInStop) MM_BADH(midiInReset)
MM_BADH(midiInPrepareHeader) MM_BADH(midiInUnprepareHeader) MM_BADH(midiInAddBuffer) MM_BADH(midiStreamClose) MM_BADH(midiStreamOut)
MM_BADH(midiStreamProperty) MM_BADH(midiStreamRestart) MM_BADH(midiStreamStop) MM_BADH(midiStreamPause) MM_BADH(waveInStop)
MM_BADH(mixerClose) MM_BADH(mixerGetLineInfoA) MM_BADH(mixerGetLineInfoW) MM_BADH(mixerGetLineControlsA) MM_BADH(mixerGetLineControlsW)
MM_BADH(mixerGetControlDetailsA) MM_BADH(mixerGetControlDetailsW) MM_BADH(mixerSetControlDetails) MM_BADH(waveOutGetVolume) MM_BADH(waveOutSetVolume)
SHIM(mixerGetNumDevs) { RET(0); }
SHIM(auxGetNumDevs) { RET(0); }
static u32 mm_text(u64 buf, u32 n, const char *s, int wide) { if (!buf || !n) return 11; if (wide) wx_u2w(s, buf, n); else snprintf((char *)buf, n, "%s", s); return 0; }
SHIM(midiOutGetErrorTextA) { RET(mm_text(A2, (u32)A3, "The specified device is not installed.", 0)); }
SHIM(midiOutGetErrorTextW) { RET(mm_text(A2, (u32)A3, "The specified device is not installed.", 1)); }
SHIM(midiInGetErrorTextA) { RET(mm_text(A2, (u32)A3, "The specified device is not installed.", 0)); }
SHIM(waveOutGetErrorTextA) { RET(mm_text(A2, (u32)A3, "No device driver is present.", 0)); }
SHIM(waveInGetErrorTextA) { RET(mm_text(A2, (u32)A3, "No device driver is present.", 0)); }
SHIM(mciSendCommandA) { RET(263); }   // MCIERR_INVALID_DEVICE_NAME
SHIM(mciSendCommandW) { RET(263); }
SHIM(mciSendStringA) { if (A2 && (u32)A3) ST8(A2, 0); RET(263); }
SHIM(mciSendStringW) { if (A2 && (u32)A3) ST16(A2, 0); RET(263); }
SHIM(mciGetErrorStringA) { mm_text(A2, (u32)A3, "The specified device is not open or is not recognized by MCI.", 0); RET(1); }
SHIM(mciGetErrorStringW) { mm_text(A2, (u32)A3, "The specified device is not open or is not recognized by MCI.", 1); RET(1); }
SHIM(PlaySoundW) { RET(0); }   // system sounds are not played
SHIM(PlaySoundA) { RET(0); }
SHIM(sndPlaySoundW) { RET(0); }
SHIM(sndPlaySoundA) { RET(0); }

// ==== shell32 / shlwapi ===================================================================================================
SHIM(SHGetSpecialFolderPathW) {  // (hwnd, path, csidl, create)
    RET(CALL("SHGetFolderPathW", A1, A3 | ((u32)A4 ? 0x8000 : 0), 0, 0, A2) == 0);
}
SHIM(SHGetSpecialFolderPathA) { RET(CALL("SHGetFolderPathA", A1, A3 | ((u32)A4 ? 0x8000 : 0), 0, 0, A2) == 0); }
SHIM(SHGetFolderPathAndSubDirW) {  // (hwnd, csidl, token, flags, subdir, path)
    u64 sub = AN(5), out = AN(6);
    u64 r = CALL("SHGetFolderPathW", A1, A2, A3, A4, out); if (r) RET(r);
    if (sub && LD16(sub)) {
        char p[2048], s[1024]; wx_w2u(out, p, sizeof p); wx_w2u(sub, s, sizeof s);
        strncat(p, "\\", sizeof p - strlen(p) - 1); strncat(p, s, sizeof p - strlen(p) - 1);
        if ((u32)A2 & 0x8000) { char host[4096]; rt_host_path(p, host, sizeof host); for (char *q = host + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(host, 0755); *q = '/'; } mkdir(host, 0755); }
        wx_u2w(p, out, 260);
    }
    RET(0);
}
static u64 to_wide_tmp(const char *s, u16 *buf, size_t n) { if (!s) return 0; wx_u2w(s, (u64)buf, n); return (u64)buf; }
SHIM(ShellExecuteA) {  // (hwnd, op, file, params, dir, show)
    u16 op[64], file[2048], par[2048];
    RET(CALL("ShellExecuteW", A1, to_wide_tmp((char *)A2, op, 64), to_wide_tmp((char *)A3, file, 2048), to_wide_tmp((char *)A4, par, 2048), 0, AN(6)));
}
SHIM(ShellExecuteExW) {  // SHELLEXECUTEINFOW { cbSize, fMask, hwnd@8, verb@16, file@24, params@32, dir@40, nShow@48, hInstApp@56 }
    u64 i = A1, r = CALL("ShellExecuteW", LD64(i + 8), LD64(i + 16), LD64(i + 24), LD64(i + 32), LD64(i + 40), LD32(i + 48));
    ST64(i + 56, r); if (r <= 32) { rt_set_last_error(r == 2 ? 2 : 1155); RET(0); }   // ERROR_NO_ASSOCIATION
    RET(1);
}
SHIM(ShellExecuteExA) { WX_FAIL(ShellExecuteExA, 1155, 0); }
SHIM(SHCreateItemFromParsingName) { if (AN(4)) {} ST64(A4, 0); RET(E_NOTIMPL); }   // (path, bindctx, riid, ppv)
SHIM(SHCreateItemFromIDList) { ST64(A3, 0); RET(E_NOTIMPL); }
SHIM(SHCreateItemInKnownFolder) { ST64(AN(5), 0); RET(E_NOTIMPL); }
SHIM(SHGetKnownFolderIDList) { ST64(A4, 0); RET(E_FAIL); }
SHIM(SHGetKnownFolderItem) { ST64(AN(5), 0); RET(E_NOTIMPL); }
SHIM(SHGetMalloc) { ST64(A1, 0); RET(E_NOTIMPL); }
SHIM(SHGetPropertyStoreForWindow) { ST64(A3, 0); RET(E_NOTIMPL); }
SHIM(SHChangeNotify) { RET(0); }
SHIM(SetCurrentProcessExplicitAppUserModelID) { RET(0); }
SHIM(GetCurrentProcessExplicitAppUserModelID) { ST64(A1, 0); RET(E_FAIL); }
SHIM(SHGetStockIconInfo) { RET(E_NOTIMPL); }
SHIM(SHGetFileInfoW) { RET(0); }
SHIM(SHGetFileInfoA) { RET(0); }
SHIM(DragQueryPoint) { if (A2) { ST32(A2, 0); ST32(A2 + 4, 0); } RET(0); }
SHIM(Shell_NotifyIconGetRect) { RET(E_FAIL); }
SHIM(SHAppBarMessage) { RET(0); }
SHIM(SHEmptyRecycleBinW) { RET(0); }
SHIM(ILFree) { free((void *)A1); RET(0); }
SHIM(SHGetDesktopFolder) { ST64(A1, 0); RET(E_NOTIMPL); }
static int rm_rf(const char *p) {
    struct stat st; if (lstat(p, &st)) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p); struct dirent *e;
        if (d) { while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { char s[4096]; snprintf(s, sizeof s, "%s/%s", p, e->d_name); rm_rf(s); } closedir(d); }
        return rmdir(p);
    }
    return unlink(p);
}
static int copy_tree(const char *a, const char *b) {
    struct stat st; if (stat(a, &st)) return -1;
    if (S_ISDIR(st.st_mode)) {
        mkdir(b, 0755); DIR *d = opendir(a); struct dirent *e; int r = 0;
        if (d) { while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { char x[4096], y[4096]; snprintf(x, sizeof x, "%s/%s", a, e->d_name); snprintf(y, sizeof y, "%s/%s", b, e->d_name); r |= copy_tree(x, y); } closedir(d); }
        return r;
    }
    FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL; if (!in || !out) { if (in) fclose(in); return -1; }
    char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out); return 0;
}
SHIM(SHFileOperationW) {  // SHFILEOPSTRUCTW { hwnd, u32 wFunc @8, pFrom @16, pTo @24, u16 flags @32, BOOL aborted @36, ... }
    u64 op = A1; u32 fn = LD32(op + 8); u64 from = LD64(op + 16), to = LD64(op + 24);
    char dst[4096] = ""; if (to && LD16(to)) { char w[2048]; wx_w2u(to, w, sizeof w); rt_host_path(w, dst, sizeof dst); }
    int err = 0;
    for (u64 p = from; p && LD16(p); p += 2 * (wx_wlen(p) + 1)) {   // double-NUL-terminated list
        char w[2048], src[4096]; wx_w2u(p, w, sizeof w); rt_host_path(w, src, sizeof src);
        struct stat st; if (stat(src, &st)) { err = 2; continue; }
        char target[8192]; struct stat dt;
        if (*dst && !stat(dst, &dt) && S_ISDIR(dt.st_mode) && fn != 4) { const char *b = strrchr(src, '/'); snprintf(target, sizeof target, "%s/%s", dst, b ? b + 1 : src); }
        else snprintf(target, sizeof target, "%s", dst);
        switch (fn) {
        case 3: if (rm_rf(src)) err = 5; break;                                    // FO_DELETE (the recycle bin is skipped)
        case 1: case 4: if (rename(src, target)) err = errno == ENOENT ? 2 : 5; break;   // FO_MOVE / FO_RENAME
        case 2: if (copy_tree(src, target)) err = 5; break;                          // FO_COPY
        default: err = 87;
        }
    }
    ST32(op + 36, 0);
    RET(err);
}
// Path helpers working on UTF-16 (W) or bytes (A) in place.
typedef struct { u64 p; int w; } PS;
static u32 pget(PS s, size_t i) { return s.w ? LD16(s.p + 2 * i) : LD8(s.p + i); }
static void pset(PS s, size_t i, u32 v) { if (s.w) ST16(s.p + 2 * i, (u16)v); else ST8(s.p + i, (u8)v); }
static size_t plen(PS s) { size_t n = 0; while (pget(s, n)) n++; return n; }
static u64 paddr(PS s, size_t i) { return s.p + (s.w ? 2 * i : i); }
static int is_sep(u32 ch) { return ch == '\\' || ch == '/'; }
static size_t root_len(PS s) {   // "C:\" -> 3, "\\server\share\" style -> 2, "\" -> 1
    if (pget(s, 0) && pget(s, 1) == ':') return is_sep(pget(s, 2)) ? 3 : 2;
    if (is_sep(pget(s, 0)) && is_sep(pget(s, 1))) return 2;
    return is_sep(pget(s, 0)) ? 1 : 0;
}
static u64 find_file_name(PS s) {
    size_t n = plen(s), b = 0;
    for (size_t i = 0; i < n; i++) if ((is_sep(pget(s, i)) || pget(s, i) == ':') && i + 1 < n) b = i + 1;
    return paddr(s, b);
}
static u64 find_ext(PS s) {
    size_t n = plen(s), e = n;
    for (size_t i = 0; i < n; i++) { u32 ch = pget(s, i); if (ch == '.') e = i; else if (is_sep(ch) || ch == ' ') e = n; }
    return paddr(s, e);
}
SHIM(PathFindFileNameW) { PS s = { A1, 1 }; RET(find_file_name(s)); }
SHIM(PathFindFileNameA) { PS s = { A1, 0 }; RET(find_file_name(s)); }
SHIM(PathFindExtensionW) { PS s = { A1, 1 }; RET(find_ext(s)); }
SHIM(PathFindExtensionA) { PS s = { A1, 0 }; RET(find_ext(s)); }
static int remove_file_spec(PS s) {
    size_t n = plen(s), r = root_len(s); long last = -1;
    for (size_t i = 0; i < n; i++) if (is_sep(pget(s, i))) last = (long)i;
    if (last < 0) { if (r == 2 && pget(s, 1) == ':') { if (n == 2) return 0; pset(s, 2, 0); return 1; } if (n) { pset(s, 0, 0); return 1; } return 0; }
    if ((size_t)last < r) { if (n > (size_t)last + 1) { pset(s, (size_t)last + 1, 0); return 1; } return 0; }
    pset(s, (size_t)last, 0); return 1;
}
SHIM(PathRemoveFileSpecW) { PS s = { A1, 1 }; RET(remove_file_spec(s)); }
SHIM(PathRemoveFileSpecA) { PS s = { A1, 0 }; RET(remove_file_spec(s)); }
static u64 add_backslash(PS s) { size_t n = plen(s); if (n && !is_sep(pget(s, n - 1))) { if (n + 1 >= 260) return 0; pset(s, n, '\\'); pset(s, n + 1, 0); n++; } return paddr(s, n); }
SHIM(PathAddBackslashW) { PS s = { A1, 1 }; RET(add_backslash(s)); }
SHIM(PathAddBackslashA) { PS s = { A1, 0 }; RET(add_backslash(s)); }
static u64 remove_backslash(PS s) { size_t n = plen(s); if (n > root_len(s) && n && is_sep(pget(s, n - 1))) { pset(s, n - 1, 0); return paddr(s, n - 1); } return paddr(s, n ? n - 1 : 0); }
SHIM(PathRemoveBackslashW) { PS s = { A1, 1 }; RET(remove_backslash(s)); }
SHIM(PathRemoveBackslashA) { PS s = { A1, 0 }; RET(remove_backslash(s)); }
SHIM(PathRemoveExtensionW) { PS s = { A1, 1 }; u64 e = find_ext(s); if (LD16(e)) ST16(e, 0); RET(0); }
SHIM(PathRemoveExtensionA) { PS s = { A1, 0 }; u64 e = find_ext(s); if (LD8(e)) ST8(e, 0); RET(0); }
SHIM(PathRenameExtensionW) { PS s = { A1, 1 }; u64 e = find_ext(s); size_t base = (e - A1) / 2, el = wx_wlen(A2); if (base + el >= 260) RET(0); memcpy((void *)e, (void *)A2, 2 * (el + 1)); RET(1); }
SHIM(PathStripPathW) { PS s = { A1, 1 }; u64 f = find_file_name(s); if (f != A1) memmove((void *)A1, (void *)f, 2 * (wx_wlen(f) + 1)); RET(0); }
SHIM(PathStripPathA) { PS s = { A1, 0 }; u64 f = find_file_name(s); if (f != A1) memmove((void *)A1, (void *)f, strlen((char *)f) + 1); RET(0); }
static int is_relative(PS s) { u32 a = pget(s, 0); if (!a) return 1; if (is_sep(a)) return 0; if (pget(s, 1) == ':') return 0; return 1; }
SHIM(PathIsRelativeW) { PS s = { A1, 1 }; RET(A1 ? is_relative(s) : 0); }
SHIM(PathIsRelativeA) { PS s = { A1, 0 }; RET(A1 ? is_relative(s) : 0); }
static int path_kind(u64 p, int wide) {   // 0 missing, 1 file, 2 dir
    char in[2048], host[4096]; if (!p) return 0;
    if (wide) wx_w2u(p, in, sizeof in); else snprintf(in, sizeof in, "%s", (char *)p);
    rt_host_path(in, host, sizeof host); struct stat st; if (stat(host, &st)) return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}
SHIM(PathFileExistsW) { RET(path_kind(A1, 1) != 0); }
SHIM(PathFileExistsA) { RET(path_kind(A1, 0) != 0); }
SHIM(PathIsDirectoryW) { RET(path_kind(A1, 1) == 2 ? 0x10 : 0); }
SHIM(PathIsDirectoryA) { RET(path_kind(A1, 0) == 2 ? 0x10 : 0); }
SHIM(PathIsFileSpecW) { PS s = { A1, 1 }; size_t n = plen(s); for (size_t i = 0; i < n; i++) if (is_sep(pget(s, i)) || pget(s, i) == ':') RET(0); RET(1); }
SHIM(PathIsRootW) { PS s = { A1, 1 }; size_t n = plen(s); RET((n == 3 && pget(s, 1) == ':' && is_sep(pget(s, 2))) || (n == 1 && is_sep(pget(s, 0)))); }
SHIM(PathGetArgsW) {  // first space outside quotes
    int q = 0; u64 p = A1;
    for (; LD16(p); p += 2) { u16 ch = LD16(p); if (ch == '"') q = !q; else if (ch == ' ' && !q) RET(p + 2); }
    RET(p);
}
// PathCombine / PathAppend / PathCanonicalize: canonicalise "." and ".." like shlwapi (no filesystem access).
static void canonicalize(const char *in, char *out, size_t n) {
    char tmp[4096]; snprintf(tmp, sizeof tmp, "%s", in);
    for (char *p = tmp; *p; p++) if (*p == '/') *p = '\\';
    char prefix[16] = ""; char *rest = tmp;
    if (tmp[0] && tmp[1] == ':') { snprintf(prefix, sizeof prefix, "%c:", tmp[0]); rest = tmp + 2; }
    int abs = *rest == '\\'; int unc = abs && rest[1] == '\\';
    char *comps[256]; int nc = 0; char *save = NULL;
    for (char *c2 = strtok_r(rest, "\\", &save); c2 && nc < 256; c2 = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(c2, ".")) continue;
        if (!strcmp(c2, "..")) { if (nc) nc--; continue; }
        comps[nc++] = c2;
    }
    int trailing = in[0] && (in[strlen(in) - 1] == '\\' || in[strlen(in) - 1] == '/');
    size_t o = (size_t)snprintf(out, n, "%s%s", prefix, unc ? "\\\\" : abs ? "\\" : "");
    for (int i = 0; i < nc && o < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", comps[i], i + 1 < nc || trailing ? "\\" : "");
    if (!*out) snprintf(out, n, "\\");
}
static void get_arg(u64 p, int wide, char *out, size_t n) { if (!p) { *out = 0; return; } if (wide) wx_w2u(p, out, n); else snprintf(out, n, "%s", (char *)p); }
static u64 put_arg(const char *s, u64 dst, int wide) { if (wide) wx_u2w(s, dst, 260); else snprintf((char *)dst, 260, "%s", s); return dst; }
static u64 path_combine(u64 dst, u64 dir, u64 file, int wide) {
    char d[2048], f[2048], j[4200], o[4200];
    get_arg(dir, wide, d, sizeof d); get_arg(file, wide, f, sizeof f);
    if (!dir && !file) return 0;
    if (f[0] == '\\' || f[0] == '/' || (f[0] && f[1] == ':')) {   // rooted file part
        if ((f[0] == '\\' || f[0] == '/') && f[1] != '\\' && d[0] && d[1] == ':') snprintf(j, sizeof j, "%c:%s", d[0], f); else snprintf(j, sizeof j, "%s", f);
    } else if (!*d) snprintf(j, sizeof j, "%s", f);
    else snprintf(j, sizeof j, "%s%s%s", d, (d[strlen(d) - 1] == '\\' || d[strlen(d) - 1] == '/' || !*f) ? "" : "\\", f);
    canonicalize(j, o, sizeof o);
    if (strlen(o) >= 260) { if (wide) ST16(dst, 0); else ST8(dst, 0); return 0; }
    return put_arg(o, dst, wide);
}
SHIM(PathCombineW) { RET(path_combine(A1, A2, A3, 1)); }
SHIM(PathCombineA) { RET(path_combine(A1, A2, A3, 0)); }
SHIM(PathAppendW) { RET(path_combine(A1, A1, A2, 1) != 0); }
SHIM(PathAppendA) { RET(path_combine(A1, A1, A2, 0) != 0); }
SHIM(PathCanonicalizeW) { char in[2048], o[4200]; get_arg(A2, 1, in, sizeof in); canonicalize(in, o, sizeof o); if (strlen(o) >= 260) RET(0); put_arg(o, A1, 1); RET(1); }
SHIM(PathCanonicalizeA) { char in[2048], o[4200]; get_arg(A2, 0, in, sizeof in); canonicalize(in, o, sizeof o); if (strlen(o) >= 260) RET(0); put_arg(o, A1, 0); RET(1); }
SHIM(PathQuoteSpacesW) { size_t n = wx_wlen(A1); int sp = 0; for (size_t i = 0; i < n; i++) if (LD16(A1 + 2 * i) == ' ') sp = 1; if (!sp || n + 3 > 260) RET(0);
                         memmove((void *)(A1 + 2), (void *)A1, 2 * n); ST16(A1, '"'); ST16(A1 + 2 * (n + 1), '"'); ST16(A1 + 2 * (n + 2), 0); RET(1); }
SHIM(PathUnquoteSpacesW) { size_t n = wx_wlen(A1); if (n >= 2 && LD16(A1) == '"' && LD16(A1 + 2 * (n - 1)) == '"') { memmove((void *)A1, (void *)(A1 + 2), 2 * (n - 2)); ST16(A1 + 2 * (n - 2), 0); RET(1); } RET(0); }
SHIM(PathMatchSpecW) { RET(0); }
SHIM(StrStrIW) { size_t nl = wx_wlen(A2); if (!nl) RET(A1); for (u64 p = A1; LD16(p); p += 2) { size_t i = 0; for (; i < nl; i++) if (towlower(LD16(p + 2 * i)) != towlower(LD16(A2 + 2 * i))) break; if (i == nl) RET(p); } RET(0); }
SHIM(StrStrIA) { RET(strcasestr((char *)A1, (char *)A2)); }
SHIM(StrStrW) { size_t nl = wx_wlen(A2); if (!nl) RET(A1); for (u64 p = A1; LD16(p); p += 2) if (!memcmp((void *)p, (void *)A2, 2 * nl)) RET(p); RET(0); }
SHIM(StrRStrIW) {  // (start, end or NULL, find)
    size_t nl = wx_wlen(A3); u64 end = A2 ? A2 : A1 + 2 * wx_wlen(A1); if (!nl) RET(0);
    for (u64 p = end; p > A1; ) { p -= 2; size_t i = 0; for (; i < nl; i++) if (towlower(LD16(p + 2 * i)) != towlower(LD16(A3 + 2 * i))) break; if (i == nl) RET(p); }
    RET(0);
}
SHIM(StrCmpIW) { for (size_t i = 0; ; i++) { u32 a = (u32)towlower(LD16(A1 + 2 * i)), b = (u32)towlower(LD16(A2 + 2 * i)); if (a != b || !a) RET((u32)(a < b ? -1 : a > b)); } }
SHIM(StrCmpW) { for (size_t i = 0; ; i++) { u32 a = LD16(A1 + 2 * i), b = LD16(A2 + 2 * i); if (a != b || !a) RET((u32)(a < b ? -1 : a > b)); } }
SHIM(StrCmpNIW) { for (size_t i = 0; i < (u32)A3; i++) { u32 a = (u32)towlower(LD16(A1 + 2 * i)), b = (u32)towlower(LD16(A2 + 2 * i)); if (a != b || !a) RET((u32)(a < b ? -1 : a > b)); } RET(0); }
SHIM(StrChrW) { for (u64 p = A1; LD16(p); p += 2) if (LD16(p) == (u16)A2) RET(p); RET(0); }
SHIM(StrDupW) { size_t n = wx_wlen(A1); u16 *w = malloc(2 * (n + 1)); memcpy(w, (void *)A1, 2 * (n + 1)); RET(w); }
SHIM(SHStrDupW) { if (!A1) { ST64(A2, 0); RET(E_INVALIDARG); } size_t n = wx_wlen(A1); u16 *w = malloc(2 * (n + 1)); memcpy(w, (void *)A1, 2 * (n + 1)); ST64(A2, (u64)w); RET(0); }
SHIM(SHStrDupA) { if (!A1) { ST64(A2, 0); RET(E_INVALIDARG); } size_t n = wx_u2w((char *)A1, 0, 0); u64 w = (u64)malloc(2 * n); wx_u2w((char *)A1, w, n); ST64(A2, w); RET(0); }
SHIM(SHLoadIndirectString) { RET(E_FAIL); }
SHIM(AssocQueryStringW) { RET(0x80070483u); }   // HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION)
SHIM(AssocQueryStringA) { RET(0x80070483u); }
SHIM(SHCreateStreamOnFileA) { ST64(A3, 0); RET(E_NOTIMPL); }
SHIM(SHCreateStreamOnFileW) { ST64(A3, 0); RET(E_NOTIMPL); }
SHIM(SHCreateStreamOnFileEx) { ST64(AN(6), 0); RET(E_NOTIMPL); }
SHIM(IsOS) { RET(0); }
// QISearch(that, const QITAB *tab, riid, ppv): QITAB { const IID *piid; DWORD offset } terminated by a NULL piid.
SHIM(QISearch) {
    static const u8 IID_IUnknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    u64 that = A1, tab = A2, riid = A3, ppv = A4;
    if (!ppv) RET(0x80004003u);   // E_POINTER
    for (u64 e = tab; LD64(e); e += 16)
        if (!memcmp((void *)LD64(e), (void *)riid, 16) || (e == tab && !memcmp((void *)riid, IID_IUnknown, 16))) {
            u64 obj = that + LD32(e + 8), vt = LD64(obj);
            rt_guest_call(c, LD64(vt + 8), 1, &obj);   // AddRef
            ST64(ppv, obj); RET(0);
        }
    ST64(ppv, 0); RET(0x80004002u);   // E_NOINTERFACE
}

// ==== ole32 / oleaut32 =====================================================================================================
SHIM(StringFromCLSID) {  // (clsid, LPOLESTR*) -> "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" in CoTaskMem
    const u8 *g = (const u8 *)A1; char s[64];
    snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", LD32(A1), LD16(A1 + 4), LD16(A1 + 6),
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    u64 w = (u64)malloc(2 * 39); wx_u2w(s, w, 39); ST64(A2, w); RET(0);
}
SHIM(StringFromIID) { sh_StringFromCLSID(c); }
SHIM(IIDFromString) { RET(CALL("CLSIDFromString", A1, A2)); }
SHIM(GetErrorInfo) { if (A2) ST64(A2, 0); RET(1); }   // S_FALSE: no error object
SHIM(SetErrorInfo) { RET(0); }
SHIM(CreateErrorInfo) { if (A1) ST64(A1, 0); RET(E_NOTIMPL); }
SHIM(CoLockObjectExternal) { RET(0); }
SHIM(CoCreateFreeThreadedMarshaler) { if (A2) ST64(A2, 0); RET(E_NOTIMPL); }
SHIM(CreateBindCtx) { if (A2) ST64(A2, 0); RET(E_NOTIMPL); }
SHIM(CoGetMalloc) { if (A2) ST64(A2, 0); RET(E_NOTIMPL); }
SHIM(DoDragDrop) { if (A4) ST32(A4, 0); RET(0x00040101u); }   // DRAGDROP_S_CANCEL
SHIM(OleSetClipboard) { RET(0x800401D0u); }   // CLIPBRD_E_CANT_OPEN: use the user32 clipboard
SHIM(OleGetClipboard) { if (A1) ST64(A1, 0); RET(0x800401D0u); }
SHIM(OleFlushClipboard) { RET(0); }
SHIM(OleIsCurrentClipboard) { RET(1); }   // S_FALSE
SHIM(OleLoadFromStream) { if (A3) ST64(A3, 0); RET(E_NOTIMPL); }
SHIM(OleSaveToStream) { RET(E_NOTIMPL); }
SHIM(CoRegisterMessageFilter) { if (A2) ST64(A2, 0); RET(0); }
// BSTRs: u32 byte length prefix, as SysAllocString in win_extra.c.
static u64 bstr_alloc(const void *src, u32 bytes) { u8 *b = malloc(4 + (size_t)bytes + 2); ST32((u64)b, bytes); if (src) memcpy(b + 4, src, bytes); else memset(b + 4, 0, bytes); b[4 + bytes] = 0; b[5 + bytes] = 0; return (u64)(b + 4); }
SHIM(SysAllocStringLen) { RET(bstr_alloc((void *)A1, 2 * (u32)A2)); }
SHIM(SysAllocStringByteLen) { RET(bstr_alloc((void *)A1, (u32)A2)); }
SHIM(SysStringByteLen) { RET(A1 ? LD32(A1 - 4) : 0); }
SHIM(SysReAllocStringLen) { u64 n = bstr_alloc((void *)A2, 2 * (u32)A3); if (LD64(A1)) free((void *)(LD64(A1) - 4)); ST64(A1, n); RET(1); }
SHIM(SysReAllocString) { u64 n = bstr_alloc((void *)A2, A2 ? 2 * (u32)wx_wlen(A2) : 0); if (LD64(A1)) free((void *)(LD64(A1) - 4)); ST64(A1, n); RET(1); }
SHIM(VariantInit) { ST16(A1, 0); RET(0); }   // VT_EMPTY
SHIM(VariantClear) {  // VARIANT { u16 vt; u16 reserved[3]; union @8 }
    u16 vt = LD16(A1);
    if (vt == 8 && LD64(A1 + 8)) free((void *)(LD64(A1 + 8) - 4));             // VT_BSTR
    else if ((vt == 9 || vt == 13) && LD64(A1 + 8)) { u64 o = LD64(A1 + 8); rt_guest_call(c, LD64(LD64(o) + 16), 1, &o); }   // IUnknown::Release
    memset((void *)A1, 0, 24); RET(0);
}
SHIM(SafeArrayCreateVector) { WX_FAIL(SafeArrayCreateVector, 8, 0); }
SHIM(SafeArrayPutElement) { RET(E_NOTIMPL); }
SHIM(SafeArrayDestroy) { RET(0); }

// ==== uxtheme / dwmapi / comctl32 / comdlg32 =================================================================================
SHIM(IsThemeActive) { RET(0); }
SHIM(IsAppThemed) { RET(0); }
SHIM(IsCompositionActive) { RET(0); }
SHIM(OpenThemeData) { rt_set_last_error(E_NOTIMPL); RET(0); }   // classic (unthemed) look: callers fall back
SHIM(OpenThemeDataForDpi) { rt_set_last_error(E_NOTIMPL); RET(0); }
SHIM(OpenThemeDataEx) { rt_set_last_error(E_NOTIMPL); RET(0); }
SHIM(CloseThemeData) { RET(0); }
SHIM(SetWindowTheme) { RET(0); }
SHIM(SetWindowThemeAttribute) { RET(0); }
SHIM(EnableThemeDialogTexture) { RET(0); }
SHIM(GetCurrentThemeName) { RET(E_FAIL); }
SHIM(BufferedPaintInit) { RET(0); }
SHIM(BufferedPaintUnInit) { RET(0); }
#define THEME_NI(n) SHIM(n) { RET(E_NOTIMPL); }
THEME_NI(DrawThemeBackground) THEME_NI(DrawThemeBackgroundEx) THEME_NI(DrawThemeText) THEME_NI(DrawThemeTextEx) THEME_NI(GetThemeBackgroundRegion)
THEME_NI(GetThemeBool) THEME_NI(GetThemeColor) THEME_NI(GetThemeEnumValue) THEME_NI(GetThemeInt) THEME_NI(GetThemeMargins) THEME_NI(GetThemePartSize)
THEME_NI(GetThemePropertyOrigin) THEME_NI(GetThemeSysFont) THEME_NI(GetThemeTransitionDuration) THEME_NI(GetThemeBackgroundContentRect)
THEME_NI(GetThemeFont) THEME_NI(GetThemePosition) THEME_NI(GetThemeMetric) THEME_NI(DrawThemeParentBackground) THEME_NI(GetThemeTextExtent)
SHIM(IsThemeBackgroundPartiallyTransparent) { RET(0); }
SHIM(IsThemePartDefined) { RET(0); }
SHIM(DwmDefWindowProc) { if (AN(5)) ST64(AN(5), 0); RET(0); }
SHIM(DwmGetColorizationColor) { if (A1) ST32(A1, 0xC40078D7); if (A2) ST32(A2, 0); RET(0); }
SHIM(DwmSetIconicThumbnail) { RET(0); }
SHIM(DwmInvalidateIconicBitmaps) { RET(0); }
SHIM(InitCommonControls) { RET(0); }
SHIM(InitCommonControlsEx) { RET(1); }
SHIM(TaskDialogIndirect) { RET(E_NOTIMPL); }
SHIM(ImageList_Create) { RET(0); }
SHIM(ImageList_Destroy) { RET(1); }
SHIM(GetOpenFileNameW) { RET(0); }   // dialogs: the user "cancelled" (CommDlgExtendedError() == 0)
SHIM(GetOpenFileNameA) { RET(0); }
SHIM(GetSaveFileNameW) { RET(0); }
SHIM(GetSaveFileNameA) { RET(0); }
SHIM(ChooseColorW) { RET(0); }
SHIM(ChooseFontW) { RET(0); }
SHIM(CommDlgExtendedError) { RET(0); }

// ==== crypt32: base64 / hex encoding; certificate stores are not available ==================================================
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t enc(const u8 *in, u32 n, u32 flags, char *out) {   // CRYPT_STRING_BASE64 1, BASE64HEADER 0, HEX 4, HEXRAW 12; NOCRLF 0x40000000
    u32 fmt = flags & 0xFFFF; int crlf = !(flags & 0x40000000); size_t o = 0;
    if (fmt == 4 || fmt == 12) {
        for (u32 i = 0; i < n; i++) { o += (size_t)sprintf(out + o, fmt == 12 ? "%02x" : (i % 16 ? " %02x" : "%02x"), in[i]); if (fmt == 4 && crlf && (i % 16 == 15 || i + 1 == n)) { out[o++] = '\r'; out[o++] = '\n'; } }
        out[o] = 0; return o;
    }
    if (fmt == 0) o += (size_t)sprintf(out + o, "-----BEGIN CERTIFICATE-----%s", crlf ? "\r\n" : "");
    for (u32 i = 0, col = 0; i < n; i += 3) {
        u32 v = (u32)in[i] << 16 | (i + 1 < n ? (u32)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = b64[v >> 18 & 63]; out[o++] = b64[v >> 12 & 63]; out[o++] = i + 1 < n ? b64[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? b64[v & 63] : '=';
        col += 4; if (crlf && (col == 64 || i + 3 >= n)) { out[o++] = '\r'; out[o++] = '\n'; col = 0; }
    }
    if (fmt == 0) o += (size_t)sprintf(out + o, "-----END CERTIFICATE-----%s", crlf ? "\r\n" : "");
    out[o] = 0; return o;
}
static int b64v(int ch) { const char *p = ch ? strchr(b64, ch) : NULL; return p ? (int)(p - b64) : -1; }
static long dec(const char *s, size_t n, u32 fmt, u8 *out) {
    size_t o = 0;
    if (fmt == 4 || fmt == 12) {
        int hi = -1;
        for (size_t i = 0; i < n; i++) { int ch = s[i], v = isxdigit(ch) ? (isdigit(ch) ? ch - '0' : (tolower(ch) - 'a' + 10)) : -1; if (v < 0) continue; if (hi < 0) hi = v; else { out[o++] = (u8)(hi << 4 | v); hi = -1; } }
        return (long)o;
    }
    const char *b = s, *e = s + n;
    if (fmt == 0 || fmt == 6) { const char *h = strstr(s, "-----BEGIN"); if (h) { const char *nl = strchr(h + 10, '\n'); b = nl ? nl + 1 : e; const char *f = strstr(b, "-----END"); if (f) e = f; } }
    u32 acc = 0; int bits = 0;
    for (const char *p = b; p < e; p++) { if (*p == '=') break; int v = b64v(*p); if (v < 0) continue; acc = acc << 6 | (u32)v; bits += 6; if (bits >= 8) { bits -= 8; out[o++] = (u8)(acc >> bits); } }
    return (long)o;
}
static int bin_to_str(Ctx *c, int wide) {   // (bin, n, flags, out, *cch)
    char *s = malloc(4 * (size_t)(u32)A2 + 256); size_t len = enc((const u8 *)A1, (u32)A2, (u32)A3, s);
    u32 cap = LD32(AN(5));
    if (!A4) { ST32(AN(5), (u32)len + 1); free(s); return 1; }
    if (cap < len + 1) { ST32(AN(5), (u32)len + 1); free(s); rt_set_last_error(234); return 0; }
    if (wide) wx_u2w(s, A4, cap); else memcpy((void *)A4, s, len + 1);
    ST32(AN(5), (u32)len); free(s); return 1;
}
SHIM(CryptBinaryToStringA) { RET(bin_to_str(c, 0)); }
SHIM(CryptBinaryToStringW) { RET(bin_to_str(c, 1)); }
static int str_to_bin(Ctx *c, int wide) {   // (str, cch, flags, out, *cb, *skip, *flagsOut)
    size_t n = (u32)A2; char *s;
    if (wide) { if (!n) n = wx_wlen(A1); s = malloc(n + 1); for (size_t i = 0; i < n; i++) { u16 ch = LD16(A1 + 2 * i); s[i] = ch < 128 ? (char)ch : '?'; } s[n] = 0; }
    else { if (!n) n = strlen((char *)A1); s = malloc(n + 1); memcpy(s, (void *)A1, n); s[n] = 0; }
    u32 fmt = (u32)A3; if (fmt == 7) fmt = strstr(s, "-----BEGIN") ? 0 : 1;   // CRYPT_STRING_BASE64_ANY
    u8 *tmp = malloc(n + 4); long len = dec(s, n, fmt, tmp); free(s);
    u32 cap = LD32(AN(5));
    if (A4 && cap < (u32)len) { ST32(AN(5), (u32)len); free(tmp); rt_set_last_error(234); return 0; }
    if (A4) memcpy((void *)A4, tmp, (size_t)len);
    ST32(AN(5), (u32)len); if (AN(6)) ST32(AN(6), 0); if (AN(7)) ST32(AN(7), fmt);
    free(tmp); return 1;
}
SHIM(CryptStringToBinaryA) { RET(str_to_bin(c, 0)); }
SHIM(CryptStringToBinaryW) { RET(str_to_bin(c, 1)); }
SHIM(CertOpenStore) { WX_FAIL(CertOpenStore, 2, 0); }
SHIM(CertOpenSystemStoreA) { WX_FAIL(CertOpenSystemStoreA, 2, 0); }
SHIM(CertOpenSystemStoreW) { WX_FAIL(CertOpenSystemStoreW, 2, 0); }
SHIM(CertCloseStore) { RET(1); }
SHIM(CertFreeCertificateContext) { RET(1); }
SHIM(CertEnumCertificatesInStore) { rt_set_last_error(0x80092004u); RET(0); }   // CRYPT_E_NOT_FOUND
SHIM(CertFindCertificateInStore) { rt_set_last_error(0x80092004u); RET(0); }
SHIM(CertDuplicateCertificateContext) { RET(A1); }

// ==== dbghelp: PE image helpers on loaded images; no symbol information =====================================================
static u32 sym_opts;
SHIM(SymGetOptions) { RET(sym_opts); }
SHIM(SymLoadModule64) { RET(AN(5) ? AN(5) : 0x140000000ull); }    // (proc, file, image, module, base, size)
SHIM(SymLoadModuleExW) { RET(AN(5) ? AN(5) : 0x140000000ull); }
SHIM(SymUnloadModule64) { RET(1); }
SHIM(SymRefreshModuleList) { RET(1); }
SHIM(SymGetModuleInfo64) { WX_FAIL(SymGetModuleInfo64, 126, 0); }   // ERROR_MOD_NOT_FOUND
SHIM(SymGetModuleInfoW64) { WX_FAIL(SymGetModuleInfoW64, 126, 0); }
SHIM(SymGetSymFromAddr64) { WX_FAIL(SymGetSymFromAddr64, 487, 0); }
SHIM(SymFromAddrW) { WX_FAIL(SymFromAddrW, 487, 0); }
SHIM(SymGetLineFromAddrW64) { WX_FAIL(SymGetLineFromAddrW64, 487, 0); }
SHIM(SymGetModuleBase64) { u64 sz; RET(rt_module_base_for(A2, &sz)); }
SHIM(SymFunctionTableAccess64) { RET(0); }
SHIM(SymSetSearchPathW) { RET(1); }
SHIM(SymGetSearchPathW) { if (A2 && (u32)A3) ST16(A2, 0); RET(1); }
SHIM(StackWalk64) { RET(0); }
SHIM(StackWalkEx) { RET(0); }
SHIM(MiniDumpWriteDump) { WX_FAIL(MiniDumpWriteDump, 50, 0); }
SHIM(UnDecorateSymbolName) { size_t n = strlen((char *)A1); if (!(u32)A3) RET(0); snprintf((char *)A2, (u32)A3, "%s", (char *)A1); RET(n < (u32)A3 ? n : (u32)A3 - 1); }
static u64 nt_headers(u64 base) { if (!base || LD16(base) != 0x5A4D) return 0; u64 nt = base + LD32(base + 0x3C); return LD32(nt) == 0x4550 ? nt : 0; }
SHIM(ImageNtHeader) { RET(nt_headers(A1)); }
SHIM(ImageDirectoryEntryToDataEx) {  // (base, mappedAsImage, dir, *size, *section)
    u64 nt = nt_headers(A1); if (AN(5)) ST64(AN(5), 0);
    if (!nt || (u32)A3 >= 16) { if (A4) ST32(A4, 0); RET(0); }
    u64 opt = nt + 24; u32 rva = LD32(opt + 112 + 8 * (u32)A3), sz = LD32(opt + 116 + 8 * (u32)A3);
    if (A4) ST32(A4, sz);
    RET(rva ? A1 + rva : 0);
}
SHIM(ImageDirectoryEntryToData) { u64 s = AN(5), sp = c->r[RSP]; ST64(sp + 40, 0); sh_ImageDirectoryEntryToDataEx(c); u64 r = c->r[RAX]; ST64(sp + 40, s); RET(r); }
SHIM(ImageRvaToVa) { RET(A2 + (u32)A3); }   // (nt, base, rva, *section): images here are mapped as images
SHIM(ImageRvaToSection) { RET(0); }

// ==== ws2_32 extras ==========================================================================================================
SHIM(WSAHtonl) { if (!A3) RET(0xFFFFFFFF); ST32(A3, __builtin_bswap32((u32)A2)); RET(0); }   // (s, value, *out)
SHIM(WSANtohl) { if (!A3) RET(0xFFFFFFFF); ST32(A3, __builtin_bswap32((u32)A2)); RET(0); }
SHIM(WSAHtons) { if (!A3) RET(0xFFFFFFFF); ST16(A3, __builtin_bswap16((u16)A2)); RET(0); }
SHIM(WSANtohs) { if (!A3) RET(0xFFFFFFFF); ST16(A3, __builtin_bswap16((u16)A2)); RET(0); }
SHIM(WSACreateEvent) { RET(CALL("CreateEventW", 0, 1, 0, 0)); }   // manual-reset, non-signalled
SHIM(WSACloseEvent) { RET(CALL("CloseHandle", A1)); }
SHIM(WSASetEvent) { RET(CALL("SetEvent", A1)); }
SHIM(WSAResetEvent) { RET(CALL("ResetEvent", A1)); }
SHIM(WSAWaitForMultipleEvents) { RET(CALL("WaitForMultipleObjects", A1, A2, A3, A4)); }   // WSA_WAIT_* match WAIT_*
SHIM(WSAEventSelect) { CALL("WSASetLastError", 10045); RET((u32)-1); }   // WSAEOPNOTSUPP: use select()/non-blocking sockets
SHIM(WSAEnumNetworkEvents) { CALL("WSASetLastError", 10045); RET((u32)-1); }
SHIM(WSAAsyncSelect) { CALL("WSASetLastError", 10045); RET((u32)-1); }
SHIM(WSAConnect) { RET(CALL("connect", A1, A2, A3)); }   // (s, name, len, caller, callee, sqos, gqos)
SHIM(WSAAccept) {  // (s, addr, *len, condition, data): a condition callback is not consulted
    if (A4) wx_stub_log("WSAAccept(condition callback)");
    RET(CALL("accept", A1, A2, A3));
}
// WSABUF { u32 len; char *buf @8 }. Overlapped requests are not supported (the call fails with WSAEOPNOTSUPP).
SHIM(WSASend) {  // (s, bufs, n, *sent, flags, ov, cr)
    if (AN(6) || AN(7)) { CALL("WSASetLastError", 10045); RET((u32)-1); }
    u64 s = A1, bufs = A2, n = (u32)A3, psent = A4, flags = (u32)AN(5); u32 total = 0;
    for (u64 i = 0; i < n; i++) {
        u32 len = LD32(bufs + 16 * i); u64 b = LD64(bufs + 16 * i + 8);
        u32 r = (u32)CALL("send", s, b, len, flags);
        if (r == 0xFFFFFFFF) { if (!total) RET((u32)-1); break; }
        total += r; if (r < len) break;
    }
    if (psent) ST32(psent, total); RET(0);
}
SHIM(WSARecv) {  // (s, bufs, n, *recvd, *flags, ov, cr): fills the first buffer
    if (AN(6) || AN(7)) { CALL("WSASetLastError", 10045); RET((u32)-1); }
    u64 s = A1, bufs = A2, precv = A4, pflags = AN(5);
    u32 r = (u32)CALL("recv", s, LD64(bufs + 8), LD32(bufs), pflags ? LD32(pflags) : 0);
    if (r == 0xFFFFFFFF) RET((u32)-1);
    if (precv) ST32(precv, r); if (pflags) ST32(pflags, 0); RET(0);
}
SHIM(WSASendTo) {  // (s, bufs, n, *sent, flags, to, tolen, ov, cr): first buffer
    if (AN(8) || AN(9)) { CALL("WSASetLastError", 10045); RET((u32)-1); }
    u64 s = A1, bufs = A2, psent = A4, flags = (u32)AN(5), to = AN(6), tolen = (u32)AN(7);
    u32 r = (u32)CALL("sendto", s, LD64(bufs + 8), LD32(bufs), flags, to, tolen);
    if (r == 0xFFFFFFFF) RET((u32)-1);
    if (psent) ST32(psent, r); RET(0);
}
SHIM(WSARecvFrom) {  // (s, bufs, n, *recvd, *flags, from, *fromlen, ov, cr)
    if (AN(8) || AN(9)) { CALL("WSASetLastError", 10045); RET((u32)-1); }
    u64 s = A1, bufs = A2, precv = A4, pflags = AN(5), from = AN(6), fromlen = AN(7);
    u32 r = (u32)CALL("recvfrom", s, LD64(bufs + 8), LD32(bufs), pflags ? LD32(pflags) : 0, from, fromlen);
    if (r == 0xFFFFFFFF) RET((u32)-1);
    if (precv) ST32(precv, r); if (pflags) ST32(pflags, 0); RET(0);
}

// ==== small system DLLs ======================================================================================================
SHIM(AvSetMmThreadCharacteristicsW) { if (A2) ST32(A2, 1); RET(new_handle()); }   // (task, *index)
SHIM(AvSetMmThreadCharacteristicsA) { if (A2) ST32(A2, 1); RET(new_handle()); }
SHIM(AvRevertMmThreadCharacteristics) { RET(1); }
SHIM(AvSetMmThreadPriority) { RET(1); }
SHIM(HidD_GetPreparsedData) { WX_FAIL(HidD_GetPreparsedData, 6, 0); }   // no HID devices are exposed (controllers go through XInput)
SHIM(HidD_FreePreparsedData) { RET(1); }
SHIM(HidD_GetProductString) { WX_FAIL(HidD_GetProductString, 6, 0); }
SHIM(HidD_GetManufacturerString) { WX_FAIL(HidD_GetManufacturerString, 6, 0); }
SHIM(HidD_GetSerialNumberString) { WX_FAIL(HidD_GetSerialNumberString, 6, 0); }
SHIM(HidD_GetAttributes) { WX_FAIL(HidD_GetAttributes, 6, 0); }
SHIM(HidD_GetHidGuid) { static const u8 g[16] = { 0xB2, 0x55, 0x1E, 0x4D, 0x6F, 0xF1, 0xCF, 0x11, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 }; memcpy((void *)A1, g, 16); RET(0); }
SHIM(HidP_GetCaps) { RET(0xC0110001u); }   // HIDP_STATUS_INVALID_PREPARSED_DATA
SHIM(HidD_SetFeature) { WX_FAIL(HidD_SetFeature, 6, 0); }
SHIM(HidD_GetFeature) { WX_FAIL(HidD_GetFeature, 6, 0); }
SHIM(HidD_SetOutputReport) { WX_FAIL(HidD_SetOutputReport, 6, 0); }
SHIM(WTSQuerySessionInformationW) { WX_FAIL(WTSQuerySessionInformationW, 50, 0); }
SHIM(WTSFreeMemory) { RET(0); }
SHIM(WTSRegisterSessionNotification) { RET(1); }
SHIM(WTSUnRegisterSessionNotification) { RET(1); }
SHIM(NetApiBufferFree) { RET(0); }
SHIM(NetShareEnum) { RET(50); }
SHIM(NetGetJoinInformation) { if (A2) ST64(A2, 0); if (A3) ST32(A3, 2); RET(0); }   // NetSetupWorkgroupName... no name
SHIM(WNetGetUniversalNameW) { RET(2250); }   // ERROR_NOT_CONNECTED
SHIM(WNetGetConnectionW) { RET(2250); }
SHIM(WinHttpOpen) { RET(new_handle()); }
SHIM(WinHttpCloseHandle) { RET(1); }
SHIM(WinHttpGetIEProxyConfigForCurrentUser) { WX_FAIL(WinHttpGetIEProxyConfigForCurrentUser, 2, 0); }
SHIM(WinHttpGetDefaultProxyConfiguration) { ST32(A1, 1); ST64(A1 + 8, 0); ST64(A1 + 16, 0); RET(1); }   // WINHTTP_ACCESS_TYPE_NO_PROXY
SHIM(WinHttpGetProxyForUrl) { WX_FAIL(WinHttpGetProxyForUrl, 12180, 0); }   // ERROR_WINHTTP_AUTODETECTION_FAILED
SHIM(WinHttpSetTimeouts) { RET(1); }
SHIM(DnsQueryEx) { RET(9003); }   // DNS_ERROR_RCODE_NAME_ERROR: use getaddrinfo
SHIM(DnsQuery_A) { if (AN(5)) ST64(AN(5), 0); RET(9003); }
SHIM(DnsQuery_W) { if (AN(5)) ST64(AN(5), 0); RET(9003); }
SHIM(DnsFree) { RET(0); }
SHIM(DnsRecordListFree) { RET(0); }
SHIM(GetAdaptersAddresses) { WX_FAIL(GetAdaptersAddresses, 232, 232); }   // ERROR_NO_DATA (returned)
SHIM(GetAdaptersInfo) { WX_FAIL(GetAdaptersInfo, 232, 232); }
SHIM(GetNetworkParams) { WX_FAIL(GetNetworkParams, 50, 50); }
SHIM(GetBestInterfaceEx) { WX_FAIL(GetBestInterfaceEx, 50, 50); }
SHIM(GetBestInterface) { WX_FAIL(GetBestInterface, 50, 50); }
SHIM(GetIfEntry2) { RET(50); }
SHIM(NotifyAddrChange) { RET(50); }
SHIM(ConvertInterfaceIndexToLuid) { RET(2); }
SHIM(ConvertInterfaceLuidToIndex) { RET(2); }
SHIM(ConvertInterfaceLuidToGuid) { RET(2); }
SHIM(ConvertInterfaceLuidToNameW) { RET(2); }
SHIM(ConvertInterfaceNameToLuidW) { RET(2); }
SHIM(if_nametoindex) { RET(if_nametoindex((const char *)A1)); }
SHIM(if_indextoname) { RET(if_indextoname((u32)A1, (char *)A2) ? A2 : 0); }
SHIM(GetUserProfileDirectoryW) {  // (token, buf, *cch incl NUL)
    const char *p = "C:\\users\\wintomac"; u32 need = (u32)strlen(p) + 1, cap = LD32(A3); ST32(A3, need);
    if (!A2 || cap < need) { rt_set_last_error(122); RET(0); }
    wx_u2w(p, A2, cap); RET(1);
}
SHIM(DirectInput8Create) { WX_FAIL(DirectInput8Create, 50, E_FAIL); }   // DirectInput is not implemented (XInput / raw input are)
SHIM(DirectInputCreateA) { WX_FAIL(DirectInputCreateA, 50, E_FAIL); }
SHIM(DirectInputCreateW) { WX_FAIL(DirectInputCreateW, 50, E_FAIL); }
SHIM(ldap_init) { WX_FAIL(ldap_init, 50, 0); }       // LDAP (libcurl's optional ldap:// support) is unavailable
SHIM(ldap_sslinit) { WX_FAIL(ldap_sslinit, 50, 0); }
SHIM(ldap_set_option) { RET(0x59); }                  // LDAP_PARAM_ERROR... any non-success
SHIM(ldap_bind_s) { RET(0x51); }                      // LDAP_SERVER_DOWN
SHIM(ldap_simple_bind_s) { RET(0x51); }
SHIM(ldap_unbind_s) { RET(0); }
SHIM(ldap_err2string) { static char s[] = "Can't contact LDAP server"; RET(s); }

// ---- remaining one-offs from the corpus -------------------------------------------------------------------------------
SHIM(ImmGetDefaultIMEWnd) { RET(0); }
SHIM(ImmGetVirtualKey) { RET(0); }   // no IME processed the key
SHIM(ImmSetCompositionFontA) { RET(1); }
SHIM(SetupDiGetDeviceInterfaceDetailW) { WX_FAIL(SetupDiGetDeviceInterfaceDetailW, 259, 0); }   // ERROR_NO_MORE_ITEMS (no devices enumerated)
SHIM(SetupDiOpenDevRegKey) { WX_FAIL(SetupDiOpenDevRegKey, 259, (u64)-1); }
SHIM(SetupDiOpenDeviceInterfaceW) { WX_FAIL(SetupDiOpenDeviceInterfaceW, 259, 0); }
SHIM(NtQueryInformationFile) { RET(0xC0000002u); }   // STATUS_NOT_IMPLEMENTED
SHIM(NtWriteFile) {  // (h, event, apc, apcctx, IO_STATUS_BLOCK*, buf, len, *offset, key)
    u64 iosb = AN(5), buf = AN(6), len = (u32)AN(7); u32 done = 0;
    u64 ok = CALL("WriteFile", A1, buf, len, (u64)&done, 0);
    if (iosb) { ST64(iosb, ok ? 0 : 0xC0000185u); ST64(iosb + 8, done); }
    RET(ok ? 0 : 0xC0000185u);   // STATUS_IO_DEVICE_ERROR
}
SHIM(capGetDriverDescriptionA) { RET(0); }   // no capture drivers
SHIM(capCreateCaptureWindowA) { WX_FAIL(capCreateCaptureWindowA, 50, 0); }
SHIM(DWriteCreateFactory) { if (A3) ST64(A3, 0); WX_FAIL(DWriteCreateFactory, 50, E_NOTIMPL); }   // no DirectWrite: callers fall back to their own font code
SHIM(RoGetActivationFactory) { if (A3) ST64(A3, 0); RET(0x80040154u); }   // REGDB_E_CLASSNOTREG: no WinRT classes
SHIM(RoInitialize) { RET(0); }
SHIM(RoUninitialize) { RET(0); }
SHIM(WindowsCreateStringReference) { RET(E_NOTIMPL); }
SHIM(WindowsCreateString) { if (A3) ST64(A3, 0); RET(E_NOTIMPL); }
SHIM(WindowsDeleteString) { RET(0); }
SHIM(RoOriginateLanguageException) { RET(0); }
SHIM(RoOriginateErrorW) { RET(0); }

const Shim shims_win32x[] = {
#define E(n) { NULL, #n, sh_##n },
    E(MapWindowPoints) E(GetAncestor) E(IsChild) E(GetWindow) E(GetTopWindow) E(GetNextWindow) E(EnumChildWindows) E(EnumThreadWindows)
    E(FindWindowW) E(FindWindowA) E(FindWindowExW) E(FindWindowExA) E(WindowFromPoint) E(ChildWindowFromPoint) E(ChildWindowFromPointEx)
    E(RealChildWindowFromPoint) E(BringWindowToTop) E(GetCursor) E(CreateCursor) E(GetCursorInfo) E(GetIconInfo) E(GetIconInfoExW)
    E(DrawIconEx) E(DrawIcon) E(CopyIcon) E(GetSysColor) E(GetSysColorBrush) E(MessageBeep) E(CreateCaret) E(DestroyCaret) E(SetCaretPos)
    E(GetCaretPos) E(ShowCaret) E(HideCaret) E(GetCaretBlinkTime) E(SetCaretBlinkTime) E(RemoveMenu) E(TrackPopupMenuEx) E(DrawMenuBar)
    E(GetMenuItemCount) E(GetMenuItemID) E(GetSubMenu) E(GetMenuItemRect) E(HiliteMenuItem) E(InsertMenuItemW) E(InsertMenuItemA)
    E(ModifyMenuW) E(ModifyMenuA) E(AppendMenuA) E(SetMenuDefaultItem) E(SetMenuInfo) E(GetMenuInfo) E(GetMenuState)
    E(SendMessageTimeoutW) E(SendMessageTimeoutA) E(SendNotifyMessageW) E(SetWinEventHook) E(UnhookWinEvent) E(ChangeWindowMessageFilter)
    E(ChangeWindowMessageFilterEx) E(AllowSetForegroundWindow) E(LockSetForegroundWindow) E(DisableProcessWindowsGhosting)
    E(IsHungAppWindow) E(IsProcessDPIAware) E(GetWindowDpiAwarenessContext) E(GetDpiAwarenessContextForProcess)
    E(LogicalToPhysicalPointForPerMonitorDPI) E(PhysicalToLogicalPointForPerMonitorDPI) E(LogicalToPhysicalPoint) E(PhysicalToLogicalPoint)
    E(RegisterPowerSettingNotification) E(UnregisterPowerSettingNotification) E(RegisterSuspendResumeNotification)
    E(UnregisterSuspendResumeNotification) E(AddClipboardFormatListener) E(RemoveClipboardFormatListener) E(SetClipboardViewer)
    E(ChangeClipboardChain) E(GetClipboardOwner) E(SetCoalescableTimer) E(SetDisplayAutoRotationPreferences)
    E(GetDisplayAutoRotationPreferences) E(SetWindowDisplayAffinity) E(GetWindowDisplayAffinity) E(SkipPointerFrameMessages)
    E(GetPointerInfo) E(GetPointerFrameTouchInfo) E(GetPointerFrameTouchInfoHistory) E(GetPointerPenInfoHistory) E(GetPointerTouchInfo)
    E(IsTouchWindow) E(UpdateLayeredWindow) E(UpdateLayeredWindowIndirect) E(GetLayeredWindowAttributes) E(GetDCEx) E(GetWindowDC)
    E(WindowFromDC) E(GetClassLongPtrW) E(GetClassLongPtrA) E(GetClassLongW) E(GetClassLongA) E(GetClassInfoA) E(LoadStringA) E(LoadStringW)
    E(MapVirtualKeyExA) E(GetKeyboardLayoutNameA) E(GetKeyboardLayoutNameW) E(LoadKeyboardLayoutA) E(LoadKeyboardLayoutW)
    E(ActivateKeyboardLayout) E(UnloadKeyboardLayout) E(SendInput) E(keybd_event) E(mouse_event) E(BlockInput) E(CreateDialogParamW)
    E(CreateDialogParamA) E(DialogBoxParamW) E(DialogBoxParamA) E(FrameRect) E(DrawFocusRect) E(DrawEdge) E(InvertRect)
    E(SystemParametersInfoForDpi) E(CharPrevExA) E(CharNextExA) E(CharPrevA) E(CharPrevW) E(CharNextA) E(CharNextW) E(CharUpperW)
    E(CharLowerW) E(CharUpperA) E(CharLowerA) E(CharUpperBuffW) E(CharLowerBuffW) E(IsCharAlphaW) E(IsCharAlphaNumericW) E(IsCharUpperW)
    E(IsCharLowerW) E(IsCharAlphaA)
    E(CreatePalette) E(SelectPalette) E(RealizePalette) E(GetSystemPaletteEntries) E(GetSystemPaletteUse) E(SetSystemPaletteUse)
    E(SetPaletteEntries) E(GetPaletteEntries) E(AnimatePalette) E(UnrealizeObject) E(SetDIBColorTable) E(GetDIBColorTable)
    E(SetDIBitsToDevice) E(StretchDIBits) E(SetDIBits) E(GdiFlush) E(GetRegionData) E(CreatePolygonRgn) E(CreateEllipticRgn)
    E(CreateRoundRectRgn) E(OffsetRgn) E(SelectClipRgn) E(ExtSelectClipRgn) E(IntersectClipRect) E(GetClipBox) E(DPtoLP) E(LPtoDP)
    E(GetMapMode) E(SetMapMode) E(SetGraphicsMode) E(GetGraphicsMode) E(SetWorldTransform) E(ModifyWorldTransform) E(SetLayout)
    E(GetLayout) E(SetTextAlign) E(SetBkColor) E(GetBkColor) E(GetTextColor) E(SetStretchBltMode) E(SetBrushOrgEx) E(SetViewportOrgEx)
    E(SetWindowOrgEx) E(MoveToEx) E(LineTo) E(Ellipse) E(Polygon) E(Polyline) E(PatBlt) E(SaveDC) E(RestoreDC) E(GetPixel) E(SetPixelV)
    E(GetBitmapBits) E(GetObjectW) E(GetObjectA) E(GetObjectType) E(GetCurrentObject) E(AddFontResourceExW) E(AddFontResourceExA)
    E(AddFontResourceW) E(AddFontResourceA) E(RemoveFontResourceExW) E(AddFontMemResourceEx) E(RemoveFontMemResourceEx)
    E(EnumFontFamiliesExW) E(EnumFontFamiliesExA) E(EnumFontFamiliesW) E(GetTextFaceW) E(GetTextFaceA) E(GetCharABCWidthsW)
    E(GetCharABCWidthsI) E(GetCharABCWidthsFloatW) E(GetCharWidthI) E(GetCharWidth32W) E(GetGlyphOutlineW) E(GetGlyphIndicesW)
    E(GetOutlineTextMetricsW) E(GetFontData) E(GetKerningPairsW) E(GetFontUnicodeRanges)
    E(timeSetEvent) E(timeKillEvent) E(timeGetDevCaps) E(timeGetSystemTime) E(joyGetNumDevs) E(joyGetPosEx) E(joyGetPos) E(joyGetDevCapsA)
    E(joyGetDevCapsW) E(joySetCapture) E(joyReleaseCapture) E(midiOutGetNumDevs) E(midiInGetNumDevs) E(midiOutGetDevCapsA)
    E(midiOutGetDevCapsW) E(midiInGetDevCapsA) E(midiInGetDevCapsW) E(midiOutOpen) E(midiInOpen) E(midiStreamOpen) E(mixerOpen)
    E(mixerGetDevCapsA) E(mixerGetDevCapsW) E(auxGetDevCapsA) E(midiOutClose) E(midiOutShortMsg) E(midiOutLongMsg) E(midiOutPrepareHeader)
    E(midiOutUnprepareHeader) E(midiOutReset) E(midiOutGetVolume) E(midiOutSetVolume) E(midiInClose) E(midiInStart) E(midiInStop)
    E(midiInReset) E(midiInPrepareHeader) E(midiInUnprepareHeader) E(midiInAddBuffer) E(midiStreamClose) E(midiStreamOut)
    E(midiStreamProperty) E(midiStreamRestart) E(midiStreamStop) E(midiStreamPause) E(waveInStop) E(mixerClose) E(mixerGetLineInfoA)
    E(mixerGetLineInfoW) E(mixerGetLineControlsA) E(mixerGetLineControlsW) E(mixerGetControlDetailsA) E(mixerGetControlDetailsW)
    E(mixerSetControlDetails) E(waveOutGetVolume) E(waveOutSetVolume) E(mixerGetNumDevs) E(auxGetNumDevs) E(midiOutGetErrorTextA)
    E(midiOutGetErrorTextW) E(midiInGetErrorTextA) E(waveOutGetErrorTextA) E(waveInGetErrorTextA) E(mciSendCommandA) E(mciSendCommandW)
    E(mciSendStringA) E(mciSendStringW) E(mciGetErrorStringA) E(mciGetErrorStringW) E(PlaySoundW) E(PlaySoundA) E(sndPlaySoundW)
    E(sndPlaySoundA)
    E(SHGetSpecialFolderPathW) E(SHGetSpecialFolderPathA) E(SHGetFolderPathAndSubDirW) E(ShellExecuteA) E(ShellExecuteExW)
    E(ShellExecuteExA) E(SHCreateItemFromParsingName) E(SHCreateItemFromIDList) E(SHCreateItemInKnownFolder) E(SHGetKnownFolderIDList)
    E(SHGetKnownFolderItem) E(SHGetMalloc) E(SHGetPropertyStoreForWindow) E(SHChangeNotify) E(SetCurrentProcessExplicitAppUserModelID)
    E(GetCurrentProcessExplicitAppUserModelID) E(SHGetStockIconInfo) E(SHGetFileInfoW) E(SHGetFileInfoA) E(DragQueryPoint)
    E(Shell_NotifyIconGetRect) E(SHAppBarMessage) E(SHEmptyRecycleBinW) E(ILFree) E(SHGetDesktopFolder) E(SHFileOperationW)
    E(PathFindFileNameW) E(PathFindFileNameA) E(PathFindExtensionW) E(PathFindExtensionA) E(PathRemoveFileSpecW) E(PathRemoveFileSpecA)
    E(PathAddBackslashW) E(PathAddBackslashA) E(PathRemoveBackslashW) E(PathRemoveBackslashA) E(PathRemoveExtensionW)
    E(PathRemoveExtensionA) E(PathRenameExtensionW) E(PathStripPathW) E(PathStripPathA) E(PathIsRelativeW) E(PathIsRelativeA)
    E(PathFileExistsW) E(PathFileExistsA) E(PathIsDirectoryW) E(PathIsDirectoryA) E(PathIsFileSpecW) E(PathIsRootW) E(PathGetArgsW)
    E(PathCombineW) E(PathCombineA) E(PathAppendW) E(PathAppendA) E(PathCanonicalizeW) E(PathCanonicalizeA) E(PathQuoteSpacesW)
    E(PathUnquoteSpacesW) E(PathMatchSpecW) E(StrStrIW) E(StrStrIA) E(StrStrW) E(StrRStrIW) E(StrCmpIW) E(StrCmpW) E(StrCmpNIW) E(StrChrW)
    E(StrDupW) E(SHStrDupW) E(SHStrDupA) E(SHLoadIndirectString) E(AssocQueryStringW) E(AssocQueryStringA) E(SHCreateStreamOnFileA)
    E(SHCreateStreamOnFileW) E(SHCreateStreamOnFileEx) E(IsOS) E(QISearch)
    E(StringFromCLSID) E(StringFromIID) E(IIDFromString) E(GetErrorInfo) E(SetErrorInfo) E(CreateErrorInfo) E(CoLockObjectExternal)
    E(CoCreateFreeThreadedMarshaler) E(CreateBindCtx) E(CoGetMalloc) E(DoDragDrop) E(OleSetClipboard) E(OleGetClipboard) E(OleFlushClipboard)
    E(OleIsCurrentClipboard) E(OleLoadFromStream) E(OleSaveToStream) E(CoRegisterMessageFilter) E(SysAllocStringLen) E(SysAllocStringByteLen)
    E(SysStringByteLen) E(SysReAllocStringLen) E(SysReAllocString) E(VariantInit) E(VariantClear) E(SafeArrayCreateVector)
    E(SafeArrayPutElement) E(SafeArrayDestroy)
    E(IsThemeActive) E(IsAppThemed) E(IsCompositionActive) E(OpenThemeData) E(OpenThemeDataForDpi) E(OpenThemeDataEx) E(CloseThemeData)
    E(SetWindowTheme) E(SetWindowThemeAttribute) E(EnableThemeDialogTexture) E(GetCurrentThemeName) E(BufferedPaintInit) E(BufferedPaintUnInit)
    E(DrawThemeBackground) E(DrawThemeBackgroundEx) E(DrawThemeText) E(DrawThemeTextEx) E(GetThemeBackgroundRegion) E(GetThemeBool)
    E(GetThemeColor) E(GetThemeEnumValue) E(GetThemeInt) E(GetThemeMargins) E(GetThemePartSize) E(GetThemePropertyOrigin) E(GetThemeSysFont)
    E(GetThemeTransitionDuration) E(GetThemeBackgroundContentRect) E(GetThemeFont) E(GetThemePosition) E(GetThemeMetric)
    E(DrawThemeParentBackground) E(GetThemeTextExtent) E(IsThemeBackgroundPartiallyTransparent) E(IsThemePartDefined)
    E(DwmDefWindowProc) E(DwmGetColorizationColor) E(DwmSetIconicThumbnail) E(DwmInvalidateIconicBitmaps) E(InitCommonControls)
    E(InitCommonControlsEx) E(TaskDialogIndirect) E(ImageList_Create) E(ImageList_Destroy) E(GetOpenFileNameW) E(GetOpenFileNameA)
    E(GetSaveFileNameW) E(GetSaveFileNameA) E(ChooseColorW) E(ChooseFontW) E(CommDlgExtendedError)
    E(CryptBinaryToStringA) E(CryptBinaryToStringW) E(CryptStringToBinaryA) E(CryptStringToBinaryW) E(CertOpenStore) E(CertOpenSystemStoreA)
    E(CertOpenSystemStoreW) E(CertCloseStore) E(CertFreeCertificateContext) E(CertEnumCertificatesInStore) E(CertFindCertificateInStore)
    E(CertDuplicateCertificateContext)
    E(SymGetOptions) E(SymLoadModule64) E(SymLoadModuleExW) E(SymUnloadModule64) E(SymRefreshModuleList) E(SymGetModuleInfo64)
    E(SymGetModuleInfoW64) E(SymGetSymFromAddr64) E(SymFromAddrW) E(SymGetLineFromAddrW64) E(SymGetModuleBase64) E(SymFunctionTableAccess64)
    E(SymSetSearchPathW) E(SymGetSearchPathW) E(StackWalk64) E(StackWalkEx) E(MiniDumpWriteDump) E(UnDecorateSymbolName) E(ImageNtHeader)
    E(ImageDirectoryEntryToDataEx) E(ImageDirectoryEntryToData) E(ImageRvaToVa) E(ImageRvaToSection)
    E(WSAHtonl) E(WSANtohl) E(WSAHtons) E(WSANtohs) E(WSACreateEvent) E(WSACloseEvent) E(WSASetEvent) E(WSAResetEvent)
    E(WSAWaitForMultipleEvents) E(WSAEventSelect) E(WSAEnumNetworkEvents) E(WSAAsyncSelect) E(WSAConnect) E(WSAAccept) E(WSASend) E(WSARecv)
    E(WSASendTo) E(WSARecvFrom)
    E(AvSetMmThreadCharacteristicsW) E(AvSetMmThreadCharacteristicsA) E(AvRevertMmThreadCharacteristics) E(AvSetMmThreadPriority)
    E(HidD_GetPreparsedData) E(HidD_FreePreparsedData) E(HidD_GetProductString) E(HidD_GetManufacturerString) E(HidD_GetSerialNumberString)
    E(HidD_GetAttributes) E(HidD_GetHidGuid) E(HidP_GetCaps) E(HidD_SetFeature) E(HidD_GetFeature) E(HidD_SetOutputReport)
    E(WTSQuerySessionInformationW) E(WTSFreeMemory) E(WTSRegisterSessionNotification) E(WTSUnRegisterSessionNotification)
    E(NetApiBufferFree) E(NetShareEnum) E(NetGetJoinInformation) E(WNetGetUniversalNameW) E(WNetGetConnectionW) E(WinHttpOpen)
    E(WinHttpCloseHandle) E(WinHttpGetIEProxyConfigForCurrentUser) E(WinHttpGetDefaultProxyConfiguration) E(WinHttpGetProxyForUrl)
    E(WinHttpSetTimeouts) E(DnsQueryEx) E(DnsQuery_A) E(DnsQuery_W) E(DnsFree) E(DnsRecordListFree) E(GetAdaptersAddresses) E(GetAdaptersInfo)
    E(GetNetworkParams) E(GetBestInterfaceEx) E(GetBestInterface) E(GetIfEntry2) E(NotifyAddrChange) E(ConvertInterfaceIndexToLuid)
    E(ConvertInterfaceLuidToIndex) E(ConvertInterfaceLuidToGuid) E(ConvertInterfaceLuidToNameW) E(ConvertInterfaceNameToLuidW)
    E(if_nametoindex) E(if_indextoname) E(GetUserProfileDirectoryW) E(DirectInput8Create) E(DirectInputCreateA) E(DirectInputCreateW)
    E(ImmGetDefaultIMEWnd) E(ImmGetVirtualKey) E(ImmSetCompositionFontA) E(SetupDiGetDeviceInterfaceDetailW) E(SetupDiOpenDevRegKey)
    E(SetupDiOpenDeviceInterfaceW) E(NtQueryInformationFile) E(NtWriteFile) E(capGetDriverDescriptionA) E(capCreateCaptureWindowA)
    E(DWriteCreateFactory) E(RoGetActivationFactory) E(RoInitialize) E(RoUninitialize) E(WindowsCreateStringReference) E(WindowsCreateString)
    E(WindowsDeleteString) E(RoOriginateLanguageException) E(RoOriginateErrorW)
    E(ldap_init) E(ldap_sslinit) E(ldap_set_option) E(ldap_bind_s) E(ldap_simple_bind_s) E(ldap_unbind_s) E(ldap_err2string)
#undef E
    { 0, 0, 0 }
};
