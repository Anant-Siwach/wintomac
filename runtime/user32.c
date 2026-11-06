// user32 (+ the bits of gdi32 window code touches) on SDL3.
// Model: one SDL window per top-level HWND, Win32 client pixels == SDL window points, SDL events are
// translated into WM_* messages and delivered through a Win32-style queue to the guest's WndProc.
#include <SDL3/SDL.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32.h"

// ---- strings ------------------------------------------------------------------------
static char *utf8_from_w(u64 w) {
    if (!w) return strdup("");
    size_t n = 0; while (LD16(w + 2 * n)) n++;
    char *o = malloc(n * 3 + 1), *p = o;
    for (size_t i = 0; i < n; i++) {
        u32 cp = LD16(w + 2 * i);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) { cp = 0x10000 + ((cp - 0xD800) << 10) + (LD16(w + 2 * i + 2) - 0xDC00); i++; }
        if (cp < 0x80) *p++ = (char)cp;
        else if (cp < 0x800) { *p++ = 0xC0 | cp >> 6; *p++ = 0x80 | (cp & 63); }
        else if (cp < 0x10000) { *p++ = 0xE0 | cp >> 12; *p++ = 0x80 | (cp >> 6 & 63); *p++ = 0x80 | (cp & 63); }
        else { *p++ = 0xF0 | cp >> 18; *p++ = 0x80 | (cp >> 12 & 63); *p++ = 0x80 | (cp >> 6 & 63); *p++ = 0x80 | (cp & 63); }
    }
    *p = 0; return o;
}
static char *name_arg(u64 p, int wide) {  // class names may be atoms (integers < 0x10000)
    char b[32];
    if (p < 0x10000) { snprintf(b, sizeof b, "#%llu", (unsigned long long)p); return strdup(b); }
    return wide ? utf8_from_w(p) : strdup((const char *)p);
}

// ---- classes and windows ----------------------------------------------------------------
typedef struct { char *name; u64 wndproc, hinst, cursor, bg; u32 style; i32 wnd_extra; u16 atom; } WClass;
typedef struct {
    SDL_Window *sdl; SDL_MetalView view;
    u64 hwnd, wndproc, userdata, hinst, parent, id;
    u32 style, exstyle; int x, y, w, h; int cls; int destroyed;  // x,y,w,h: the game's (logical) window rect
    int fs;                                                        // currently shown as macOS fullscreen
    int pixfmt;                                                    // SetPixelFormat (OpenGL), 0 = none
    int shown;                                                     // visible as far as the game knows (WINTOMAC_HEADLESS keeps the Mac window hidden)
    char *title;
    struct { u64 atom; char name[64]; u64 val; } props[16];        // SetProp/GetProp
    u8 extra[64];
    void *detached;                                                // ps5_gfx.m: Metal layer of a window created off the main thread
} Win;

static WClass classes[64]; static int nclasses;
static Win *wins[256];
static u64 g_focus;  // see first_hwnd()
// WINTOMAC_HEADLESS=1: every native window stays hidden while the game believes it is shown (tests, CI).
static int headless(void) { static int h = -1; if (h < 0) h = getenv("WINTOMAC_HEADLESS") != NULL; return h; }

static Win *win_get(u64 hwnd) { intptr_t p; return rt_handle_get(hwnd, H_WINDOW, &p) ? (Win *)p : NULL; }
static Win *win_by_sdl(SDL_WindowID id) {
    for (int i = 0; i < 256; i++) if (wins[i] && !wins[i]->destroyed && SDL_GetWindowID(wins[i]->sdl) == id) return wins[i];
    return NULL;
}

static void ensure_sdl(void) {
    static int done;
    if (done) return;
    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, headless() ? "1" : "0");   // regular app: dock icon, can take focus (headless: neither)
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");   // games: instant fullscreen, not an animated Space
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) rt_log("SDL_Init failed: %s", SDL_GetError());
    done = 1;
}

static u64 register_class(Ctx *c, u64 wc, int ex, int wide) {
    (void)c;
    if (nclasses >= 64) return 0;
    WClass *k = &classes[nclasses];
    u64 base = wc + (ex ? 4 : 0);  // WNDCLASSEX has cbSize in front; the rest of the layout is shifted by 8 (with padding)
    if (ex) {
        k->style = LD32(wc + 4); k->wndproc = LD64(wc + 8); k->wnd_extra = (i32)LD32(wc + 20); k->hinst = LD64(wc + 24);
        k->cursor = LD64(wc + 40); k->bg = LD64(wc + 48); k->name = name_arg(LD64(wc + 64), wide);
    } else {
        k->style = LD32(wc + 0); k->wndproc = LD64(wc + 8); k->wnd_extra = (i32)LD32(wc + 20); k->hinst = LD64(wc + 24);
        k->cursor = LD64(wc + 40); k->bg = LD64(wc + 48); k->name = name_arg(LD64(wc + 64), wide);
    }
    (void)base;
    k->atom = (u16)(0xC000 + nclasses);
    nclasses++;
    return k->atom;
}
SHIM(RegisterClassExW) { RET(register_class(c, A1, 1, 1)); }
SHIM(RegisterClassExA) { RET(register_class(c, A1, 1, 0)); }
SHIM(RegisterClassW) { RET(register_class(c, A1, 0, 1)); }
SHIM(RegisterClassA) { RET(register_class(c, A1, 0, 0)); }
SHIM(UnregisterClassW) { RET(1); }
SHIM(UnregisterClassA) { RET(1); }

static int find_class(u64 p, int wide) {
    if (p < 0x10000) { for (int i = 0; i < nclasses; i++) if (classes[i].atom == p) return i; return -1; }
    char *n = name_arg(p, wide);
    int r = -1;
    for (int i = 0; i < nclasses; i++) if (!strcasecmp(classes[i].name, n)) { r = i; break; }
    // Predefined system classes (message-only "Message", dialog "#32770", common controls) are registered
    // on first use. Their window procedure is DefWindowProc-like: wndproc 0 makes send() return 0.
    static const char *const sys[] = { "Message", "#32770", "Static", "Button", "Edit", "ListBox", "ComboBox", "ScrollBar", "MDIClient" };
    if (r < 0 && nclasses < 64)
        for (unsigned i = 0; i < sizeof sys / sizeof *sys; i++) if (!strcasecmp(sys[i], n)) {
            WClass *k = &classes[nclasses]; memset(k, 0, sizeof *k);
            k->name = strdup(sys[i]); k->atom = (u16)(0xC000 + nclasses); r = nclasses++;
            break;
        }
    free(n); return r;
}

static u64 send(Ctx *c, Win *w, u32 msg, u64 wp, u64 lp) {
    if (!w || !w->wndproc) return 0;
    u64 args[4] = { w->hwnd, msg, wp, lp };
    return rt_guest_call(c, w->wndproc, 4, args);
}

#define WS_VISIBLE 0x10000000u
#define CW_USEDEFAULT ((i32)0x80000000)
enum { WM_CREATE = 1, WM_DESTROY = 2, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8, WM_PAINT = 0xF,
       WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82,
       WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105,
       WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205,
       WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208, WM_MOUSEWHEEL = 0x20A };

// ---- display modes (virtual) and fullscreen -------------------------------------------------
// ChangeDisplaySettings never changes the real Mac display mode. It sets a *virtual* mode that the game sees
// everywhere (EnumDisplaySettings current mode, GetSystemMetrics, monitor rects, GetDeviceCaps). A visible
// top-level window without a caption that covers that virtual screen is shown as borderless macOS fullscreen;
// Metal scales the game's frames to the real display. (Same idea as Wine/CrossOver's mac driver.)
static struct { int w, h, hz, set; } vmode;
static int trace_display(void) { const char *e = getenv("WINTOMAC_TRACE_DISPLAY"); return e ? (atoi(e) > 1 ? 2 : 1) : 0; }  // 2 = verbose (every resize/query)
void rt_desktop_mode(int *w, int *h, int *hz) {
    ensure_sdl();
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    *w = m ? m->w : 1920; *h = m ? m->h : 1080; *hz = m && m->refresh_rate > 0 ? (int)(m->refresh_rate + 0.5f) : 60;
}
void rt_display_mode(int *w, int *h, int *hz) {
    if (vmode.set) { *w = vmode.w; *h = vmode.h; *hz = vmode.hz; return; }
    rt_desktop_mode(w, h, hz);
}
static void update_fullscreen(Win *w);
static void update_all_fullscreen(void) { for (int i = 0; i < 256; i++) if (wins[i] && !wins[i]->destroyed) update_fullscreen(wins[i]); }
// w == 0: back to the desktop mode. Returns 0 = DISP_CHANGE_SUCCESSFUL, -2 = DISP_CHANGE_BADMODE.
int rt_display_change(int w, int h, int hz, int test) {
    if (w && (w < 320 || h < 200 || w > 16384 || h > 16384)) return -2;
    if (test) return 0;
    if (!w) vmode.set = 0;
    else { int dw, dh, dhz; rt_desktop_mode(&dw, &dh, &dhz); vmode.w = w; vmode.h = h; vmode.hz = hz ? hz : dhz; vmode.set = 1; }
    if (trace_display()) { if (w) rt_log("display: virtual mode set to %dx%d", w, h); else rt_log("display: virtual mode reset to desktop"); }
    update_all_fullscreen();
    return 0;
}
#define WS_CAPTION_ 0x00C00000u
#define WS_CHILD_ 0x40000000u
static void update_fullscreen(Win *w) {
    if (!w->sdl) return;
    int vw, vh, hz; rt_display_mode(&vw, &vh, &hz);
    int visible = w->shown;
    int want = visible && !(w->style & WS_CHILD_) && (w->style & WS_CAPTION_) != WS_CAPTION_ &&
               w->x <= 0 && w->y <= 0 && w->x + w->w >= vw && w->y + w->h >= vh;
    if (want == w->fs) return;
    w->fs = want;
    if (headless()) return;   // never take over the screen in headless mode
    SDL_SetWindowFullscreen(w->sdl, want);
    if (!want) { SDL_SetWindowSize(w->sdl, w->w, w->h); SDL_SetWindowBordered(w->sdl, true); }
    SDL_SyncWindow(w->sdl);
    if (trace_display()) rt_log("display: window %s fullscreen (%dx%d)", want ? "entered" : "left", w->w, w->h);
}

static u64 create_window(Ctx *c, int wide) {
    u32 exstyle = (u32)A1; u64 cls = A2, title = A3; u32 style = (u32)A4;
    i32 x = (i32)AN(5), y = (i32)AN(6), w = (i32)AN(7), h = (i32)AN(8);
    u64 parent = AN(9), menu = AN(10), hinst = AN(11), param = AN(12);
    int k = find_class(cls, wide);
    if (k < 0) { char *n = name_arg(cls, wide); rt_log("CreateWindowEx: unknown class '%s'", n); free(n); return 0; }
    ensure_sdl();
    if (w == CW_USEDEFAULT || w <= 0) { w = 1280; h = 720; }
    if (h == CW_USEDEFAULT || h <= 0) h = 720;
    char *t = title ? (wide ? utf8_from_w(title) : strdup((char *)title)) : strdup("");
    char *title_copy = strdup(t);
    Win *win = calloc(1, sizeof *win);
    win->shown = (style & WS_VISIBLE) != 0;
    win->wndproc = classes[k].wndproc; win->hinst = hinst; win->style = style; win->exstyle = exstyle;
    win->parent = parent; win->id = menu; win->w = w; win->h = h; win->cls = k; win->title = title_copy;
    win->x = x == CW_USEDEFAULT ? 0 : x; win->y = x == CW_USEDEFAULT ? 0 : y;
    if ((parent && (style & 0x40000000u)) || parent == (u64)-3 || !pthread_main_np()) {
        // WS_CHILD, HWND_MESSAGE (message-only), or created off the main thread (Cocoa forbids that): no native window
        win->sdl = NULL;
    } else {
        win->sdl = SDL_CreateWindow(t, w, h, SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | ((style & WS_VISIBLE) && !headless() ? 0 : SDL_WINDOW_HIDDEN));
        if (!win->sdl) { rt_log("SDL_CreateWindow failed: %s", SDL_GetError()); free(t); free(win); return 0; }
        // Launched from a terminal we are not a bundled app, so macOS won't bring us forward on its own.
        const char *pos = getenv("WINTOMAC_WINDOW_POS");  // "x,y" overrides CW_USEDEFAULT placement
        int px, py;
        if (x != CW_USEDEFAULT) SDL_SetWindowPosition(win->sdl, x, y);
        else if (pos && sscanf(pos, "%d,%d", &px, &py) == 2) SDL_SetWindowPosition(win->sdl, px, py);
        else SDL_SetWindowPosition(win->sdl, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        if (x == CW_USEDEFAULT) { SDL_SyncWindow(win->sdl); SDL_GetWindowPosition(win->sdl, &win->x, &win->y); }
        if ((style & WS_CAPTION_) != WS_CAPTION_) SDL_SetWindowBordered(win->sdl, false);
        if ((style & WS_VISIBLE) && !headless()) SDL_RaiseWindow(win->sdl);
        SDL_StartTextInput(win->sdl);
    }
    free(t);
    win->hwnd = rt_handle_new(H_WINDOW, (intptr_t)win);
    for (int i = 0; i < 256; i++) if (!wins[i]) { wins[i] = win; break; }
    // CREATESTRUCT for WM_NCCREATE / WM_CREATE
    u8 *cs = calloc(1, 80);
    ST64((u64)cs + 0, param); ST64((u64)cs + 8, hinst); ST64((u64)cs + 16, menu); ST64((u64)cs + 24, parent);
    ST32((u64)cs + 32, (u32)h); ST32((u64)cs + 36, (u32)w); ST32((u64)cs + 40, 0); ST32((u64)cs + 44, 0);
    ST32((u64)cs + 48, style); ST64((u64)cs + 56, title); ST64((u64)cs + 64, cls); ST32((u64)cs + 72, exstyle);
    if (win->wndproc && !send(c, win, WM_NCCREATE, 0, (u64)cs)) { rt_log("CreateWindowEx: WM_NCCREATE returned FALSE"); }
    if ((i64)send(c, win, WM_CREATE, 0, (u64)cs) == -1) { free(cs); return 0; }
    free(cs);
    send(c, win, WM_SIZE, 0, (u32)w | ((u64)(u32)h << 16));
    if ((style & WS_VISIBLE) && win->sdl) g_focus = win->hwnd;
    if (style & WS_VISIBLE) { send(c, win, WM_SHOWWINDOW, 1, 0); send(c, win, WM_ACTIVATE, 1, 0); send(c, win, WM_SETFOCUS, 0, 0); }
    update_fullscreen(win);
    return win->hwnd;
}
SHIM(CreateWindowExW) { RET(create_window(c, 1)); }
SHIM(CreateWindowExA) { RET(create_window(c, 0)); }

static void destroy_window(Ctx *c, Win *w) {
    if (!w || w->destroyed) return;
    send(c, w, WM_DESTROY, 0, 0);
    send(c, w, WM_NCDESTROY, 0, 0);
    w->destroyed = 1;
    if (w->view) SDL_Metal_DestroyView(w->view);
    if (w->sdl) SDL_DestroyWindow(w->sdl);
    w->sdl = NULL; w->view = NULL;
}
SHIM(DestroyWindow) { Win *w = win_get(A1); if (!w) RET(0); destroy_window(c, w); RET(1); }

void *gfx_detached_layer(u64 hwnd, int w, int h, const char *title, int visible);   // ps5_gfx.m
void *gfx_detached_metal_layer(void *d);
void gfx_detached_show(void *d, int w, int h, const char *title);
void gfx_detached_set_title(void *d, const char *title);
void *user32_metal_layer(u64 hwnd) {
    Win *w = win_get(hwnd);
    if (w && !w->sdl && !w->destroyed && !(w->parent && (w->style & 0x40000000u)) && w->parent != (u64)-3) {
        // created off the main thread (AnyPS5's VideoOut presenter): a detached layer, shown via the main thread
        if (!w->detached) w->detached = gfx_detached_layer(w->hwnd, w->w, w->h, w->title, w->shown && !headless());
        return gfx_detached_metal_layer(w->detached);
    }
    if (!w || !w->sdl) return NULL;
    if (!w->view) w->view = SDL_Metal_CreateView(w->sdl);
    return w->view ? SDL_Metal_GetLayer(w->view) : NULL;
}

static void detached_test_keys(u64 hwnd);   // below
SHIM(ShowWindow) {
    Win *w = win_get(A1); if (!w) RET(0);
    int was = w->shown;
    u32 cmd = (u32)A2;
    if (!w->sdl) {   // no native window (created off the main thread): track visibility; ps5_gfx.m shows its layer
        w->shown = cmd != 0;
        int top = !(w->parent && (w->style & 0x40000000u)) && w->parent != (u64)-3;
        if (w->shown && top) {
            g_focus = w->hwnd;
            if (!was) { send(c, w, WM_SHOWWINDOW, 1, 0); send(c, w, WM_ACTIVATE, 1, 0); send(c, w, WM_SETFOCUS, 0, 0); }
            if (w->detached && !headless()) gfx_detached_show(w->detached, w->w, w->h, w->title);
            if (!was) detached_test_keys(w->hwnd);
        }
        RET(was);
    }
    if (cmd == 0) { w->shown = 0; SDL_HideWindow(w->sdl); }
    else { w->shown = 1; g_focus = w->hwnd;
           if (!headless()) { SDL_ShowWindow(w->sdl); SDL_RaiseWindow(w->sdl); if (cmd == 3) SDL_MaximizeWindow(w->sdl); if (cmd == 6 || cmd == 2) SDL_MinimizeWindow(w->sdl); }
           if (!was) { send(c, w, WM_SHOWWINDOW, 1, 0); send(c, w, WM_ACTIVATE, 1, 0); send(c, w, WM_SETFOCUS, 0, 0); } }
    update_fullscreen(w);
    RET(was);
}
SHIM(UpdateWindow) { RET(1); }
static void set_title(Win *w, char *t) {
    if (!w) { free(t); return; }
    free(w->title); w->title = t;
    if (w->detached) gfx_detached_set_title(w->detached, t);
    if (w->sdl && pthread_main_np()) SDL_SetWindowTitle(w->sdl, t);
}
SHIM(SetWindowTextW) { set_title(win_get(A1), utf8_from_w(A2)); RET(1); }
SHIM(SetWindowTextA) { set_title(win_get(A1), strdup(A2 ? (const char *)A2 : "")); RET(1); }

static void client_size(Win *w, int *cw, int *ch) {  // the game's logical size, even while shown fullscreen
    if (w) { *cw = w->w; *ch = w->h; } else { *cw = *ch = 0; }
}
SHIM(GetClientRect) {
    Win *w = win_get(A1); int cw, ch; if (!w) RET(0); client_size(w, &cw, &ch);
    if (trace_display() > 1) rt_log("display: GetClientRect hwnd=%llx -> %dx%d", (unsigned long long)A1, cw, ch);
    ST32(A2, 0); ST32(A2 + 4, 0); ST32(A2 + 8, (u32)cw); ST32(A2 + 12, (u32)ch); RET(1);
}
SHIM(GetWindowRect) {
    Win *w = win_get(A1); int cw, ch, x, y; if (!w) RET(0); client_size(w, &cw, &ch);
    x = w->x; y = w->y;
    ST32(A2, (u32)x); ST32(A2 + 4, (u32)y); ST32(A2 + 8, (u32)(x + cw)); ST32(A2 + 12, (u32)(y + ch)); RET(1);
}
SHIM(AdjustWindowRect) { RET(1); }      // our windows have no non-client area: outer == client
SHIM(AdjustWindowRectEx) { RET(1); }
SHIM(AdjustWindowRectExForDpi) { RET(1); }
// Move/resize the logical window; the Mac window follows unless it is (or is about to be) fullscreen.
static void place(Ctx *c, Win *w, int move, int x, int y, int size, int cw, int ch) {
    if (trace_display() > 1) rt_log("display: place hwnd=%llx move=%d (%d,%d) size=%d (%dx%d) [was %d,%d %dx%d]", (unsigned long long)w->hwnd,
                                move, x, y, size, cw, ch, w->x, w->y, w->w, w->h);
    int resized = size && (cw != w->w || ch != w->h);
    if (move) { w->x = x; w->y = y; }
    if (size) { w->w = cw; w->h = ch; }
    update_fullscreen(w);
    if (!w->fs && w->sdl) {
        if (size) SDL_SetWindowSize(w->sdl, w->w, w->h);
        if (move) SDL_SetWindowPosition(w->sdl, w->x, w->y);
    }
    if (resized) send(c, w, WM_SIZE, 0, (u32)w->w | ((u64)(u32)w->h << 16));
}
SHIM(SetWindowPos) {  // (hwnd, after, x, y, cx, cy, flags)
    Win *w = win_get(A1); if (!w || !w->sdl) RET(0);
    u32 fl = (u32)AN(7);
    if (trace_display() > 1) rt_log("display: SetWindowPos hwnd=%llx (%d,%d) %dx%d flags=%x", (unsigned long long)A1, (i32)A3, (i32)A4, (i32)AN(5), (i32)AN(6), fl);
    if (fl & 0x40) { w->shown = 1; if (!headless()) SDL_ShowWindow(w->sdl); }   // SWP_SHOWWINDOW
    if (fl & 0x80) { w->shown = 0; SDL_HideWindow(w->sdl); }                    // SWP_HIDEWINDOW
    place(c, w, !(fl & 2), (int)(i32)A3, (int)(i32)A4, !(fl & 1), (int)(i32)AN(5), (int)(i32)AN(6));
    RET(1);
}
SHIM(MoveWindow) {
    Win *w = win_get(A1); if (!w || !w->sdl) RET(0);
    place(c, w, 1, (int)(i32)A2, (int)(i32)A3, 1, (int)(i32)A4, (int)(i32)AN(5));
    RET(1);
}
SHIM(IsWindowVisible) { Win *w = win_get(A1); RET(w && w->shown); }
SHIM(IsIconic) { Win *w = win_get(A1); RET(w && w->sdl && (SDL_GetWindowFlags(w->sdl) & SDL_WINDOW_MINIMIZED)); }
SHIM(IsWindow) { Win *w = win_get(A1); RET(w && !w->destroyed); }

#define GWLP_WNDPROC (-4)
#define GWLP_HINSTANCE (-6)
#define GWLP_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWLP_USERDATA (-21)
static u64 *wlong(Win *w, i32 idx) {
    static u64 dummy;
    switch (idx) {
    case GWLP_WNDPROC: return &w->wndproc;
    case GWLP_USERDATA: return &w->userdata;
    case GWLP_HINSTANCE: return &w->hinst;
    case GWLP_ID: return &w->id;
    default: if (idx >= 0 && idx + 8 <= (i32)sizeof w->extra) return (u64 *)(w->extra + idx); return &dummy;
    }
}
SHIM(SetWindowLongPtrW) {
    Win *w = win_get(A1); if (!w) RET(0); i32 idx = (i32)A2;
    if (idx == GWL_STYLE) {
        u64 o = w->style; w->style = (u32)A3;
        if (w->sdl && !w->fs) SDL_SetWindowBordered(w->sdl, (w->style & WS_CAPTION_) == WS_CAPTION_);
        update_fullscreen(w);
        RET(o);
    }
    if (idx == GWL_EXSTYLE) { u64 o = w->exstyle; w->exstyle = (u32)A3; RET(o); }
    u64 *p = wlong(w, idx); u64 old = *p; *p = A3; RET(old);
}
SHIM(GetWindowLongPtrW) {
    Win *w = win_get(A1); if (!w) RET(0); i32 idx = (i32)A2;
    if (idx == GWL_STYLE) RET(w->style);
    if (idx == GWL_EXSTYLE) RET(w->exstyle);
    RET(*wlong(w, idx));
}
SHIM(SetWindowLongPtrA) { sh_SetWindowLongPtrW(c); }
SHIM(GetWindowLongPtrA) { sh_GetWindowLongPtrW(c); }
SHIM(SetWindowLongW) { u64 r; sh_SetWindowLongPtrW(c); r = c->r[RAX]; RET((u32)r); }
SHIM(GetWindowLongW) { u64 r; sh_GetWindowLongPtrW(c); r = c->r[RAX]; RET((u32)r); }
SHIM(SetWindowLongA) { sh_SetWindowLongW(c); }
SHIM(GetWindowLongA) { sh_GetWindowLongW(c); }

// ---- messages -------------------------------------------------------------------------
typedef struct { u64 hwnd; u32 msg; u64 wp, lp; u32 time; i32 x, y; } Msg;
#define QCAP 1024
static Msg queue[QCAP]; static int qhead, qtail;
static int quit_posted; static int quit_code;
static u8 keystate[256];
static int mouse_x, mouse_y; static u32 mouse_buttons;

static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static void post(u64 hwnd, u32 msg, u64 wp, u64 lp) {
    pthread_mutex_lock(&qlock);
    int nt = (qtail + 1) % QCAP;
    if (nt != qhead) {  // full: drop
        queue[qtail] = (Msg){ hwnd, msg, wp, lp, (u32)SDL_GetTicks(), mouse_x, mouse_y };
        qtail = nt;
    }
    pthread_mutex_unlock(&qlock);
}

// ---- raw input (WM_INPUT / GetRawInputData / GetRawInputBuffer) -----------------------------------
// Packets are produced on the main thread from SDL mouse events and consumed by whichever guest thread asked
// (SDL3 reads them on its own raw-input thread with GetRawInputBuffer). RAWINPUT (x64): header
// { u32 type; u32 size; HANDLE device; WPARAM wparam } then RAWMOUSE { u16 flags; u16 pad; u16 buttonFlags;
// u16 buttonData; u32 rawButtons; i32 lastX, lastY; u32 extra } = 48 bytes.
#define RAW_MOUSE_DEV 0x5A01ull
#define RAW_KBD_DEV 0x5A02ull
#define RAWQ 1024
typedef struct { u16 bflags; i16 bdata; i32 dx, dy; u8 consumed; } RawPkt;   // consumed: read via GetRawInputData
static RawPkt rawq[RAWQ]; static u64 raw_head, raw_tail;      // monotonic indices; HRAWINPUT = 0x7A000000 | (index & 0xFFFFFF)
static pthread_mutex_t raw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t raw_cond = PTHREAD_COND_INITIALIZER;
static int raw_mouse; static u64 raw_mouse_hwnd; static u32 raw_mouse_flags;
static float raw_fx, raw_fy;                                  // fractional motion carried between events
static int relative_active;

static void raw_push(u16 bflags, i16 bdata, i32 dx, i32 dy) {
    pthread_mutex_lock(&raw_lock);
    if (raw_tail - raw_head >= RAWQ) raw_head++;              // overflow: drop oldest
    u64 idx = raw_tail++;
    rawq[idx % RAWQ] = (RawPkt){ bflags, bdata, dx, dy, 0 };
    pthread_cond_broadcast(&raw_cond);
    pthread_mutex_unlock(&raw_lock);
    if (!(raw_mouse_flags & 0x100) || raw_mouse_hwnd)          // WM_INPUT to the target (or focus) window
        post(raw_mouse_hwnd, 0x00FF, 0 /* RIM_INPUT */, 0x7A000000ull | (idx & 0xFFFFFF));
}
static void write_raw(u64 dst, const RawPkt *p) {
    memset((void *)dst, 0, 48);
    ST32(dst, 0 /* RIM_TYPEMOUSE */); ST32(dst + 4, 48); ST64(dst + 8, RAW_MOUSE_DEV); ST64(dst + 16, 0);
    ST16(dst + 24, 0 /* MOUSE_MOVE_RELATIVE */); ST16(dst + 28, p->bflags); ST16(dst + 30, (u16)p->bdata);
    ST32(dst + 36, (u32)p->dx); ST32(dst + 40, (u32)p->dy);
}
static void update_relative(void);

// ---- keyboard: SDL (USB HID) scancode -> PC set-1 scancode, extended flag, Windows VK, US-layout chars --------
// Windows puts set-1 scancodes (+ extended bit 24) in WM_KEY* lParam; SDL3's own Windows backend and many games
// key their tables on those, so they must be exact.
typedef struct { u8 sc; u8 ext; u8 vk; char lo, hi; } KeyInfo;
static KeyInfo keymap[256];
static void init_keymap(void) {
    static int done; if (done) return; done = 1;
    static const u8 letter_sc[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    for (int i = 0; i < 26; i++) keymap[SDL_SCANCODE_A + i] = (KeyInfo){ letter_sc[i], 0, (u8)('A' + i), (char)('a' + i), (char)('A' + i) };
    static const char digit_hi[10] = { ')', '!', '@', '#', '$', '%', '^', '&', '*', '(' };
    for (int i = 0; i < 9; i++) keymap[SDL_SCANCODE_1 + i] = (KeyInfo){ (u8)(0x02 + i), 0, (u8)('1' + i), (char)('1' + i), digit_hi[i + 1] };
    keymap[SDL_SCANCODE_0] = (KeyInfo){ 0x0B, 0, '0', '0', ')' };
    struct { int s; KeyInfo k; } t[] = {
        { SDL_SCANCODE_RETURN, { 0x1C, 0, 0x0D, '\r', '\r' } }, { SDL_SCANCODE_ESCAPE, { 0x01, 0, 0x1B, 27, 27 } },
        { SDL_SCANCODE_BACKSPACE, { 0x0E, 0, 0x08, 8, 8 } }, { SDL_SCANCODE_TAB, { 0x0F, 0, 0x09, '\t', '\t' } },
        { SDL_SCANCODE_SPACE, { 0x39, 0, 0x20, ' ', ' ' } }, { SDL_SCANCODE_MINUS, { 0x0C, 0, 0xBD, '-', '_' } },
        { SDL_SCANCODE_EQUALS, { 0x0D, 0, 0xBB, '=', '+' } }, { SDL_SCANCODE_LEFTBRACKET, { 0x1A, 0, 0xDB, '[', '{' } },
        { SDL_SCANCODE_RIGHTBRACKET, { 0x1B, 0, 0xDD, ']', '}' } }, { SDL_SCANCODE_BACKSLASH, { 0x2B, 0, 0xDC, '\\', '|' } },
        { SDL_SCANCODE_SEMICOLON, { 0x27, 0, 0xBA, ';', ':' } }, { SDL_SCANCODE_APOSTROPHE, { 0x28, 0, 0xDE, '\'', '"' } },
        { SDL_SCANCODE_GRAVE, { 0x29, 0, 0xC0, '`', '~' } }, { SDL_SCANCODE_COMMA, { 0x33, 0, 0xBC, ',', '<' } },
        { SDL_SCANCODE_PERIOD, { 0x34, 0, 0xBE, '.', '>' } }, { SDL_SCANCODE_SLASH, { 0x35, 0, 0xBF, '/', '?' } },
        { SDL_SCANCODE_CAPSLOCK, { 0x3A, 0, 0x14, 0, 0 } }, { SDL_SCANCODE_PRINTSCREEN, { 0x37, 1, 0x2C, 0, 0 } },
        { SDL_SCANCODE_SCROLLLOCK, { 0x46, 0, 0x91, 0, 0 } }, { SDL_SCANCODE_PAUSE, { 0x45, 0, 0x13, 0, 0 } },
        { SDL_SCANCODE_INSERT, { 0x52, 1, 0x2D, 0, 0 } }, { SDL_SCANCODE_HOME, { 0x47, 1, 0x24, 0, 0 } },
        { SDL_SCANCODE_PAGEUP, { 0x49, 1, 0x21, 0, 0 } }, { SDL_SCANCODE_DELETE, { 0x53, 1, 0x2E, 0, 0 } },
        { SDL_SCANCODE_END, { 0x4F, 1, 0x23, 0, 0 } }, { SDL_SCANCODE_PAGEDOWN, { 0x51, 1, 0x22, 0, 0 } },
        { SDL_SCANCODE_RIGHT, { 0x4D, 1, 0x27, 0, 0 } }, { SDL_SCANCODE_LEFT, { 0x4B, 1, 0x25, 0, 0 } },
        { SDL_SCANCODE_DOWN, { 0x50, 1, 0x28, 0, 0 } }, { SDL_SCANCODE_UP, { 0x48, 1, 0x26, 0, 0 } },
        { SDL_SCANCODE_NUMLOCKCLEAR, { 0x45, 1, 0x90, 0, 0 } }, { SDL_SCANCODE_KP_DIVIDE, { 0x35, 1, 0x6F, '/', '/' } },
        { SDL_SCANCODE_KP_MULTIPLY, { 0x37, 0, 0x6A, '*', '*' } }, { SDL_SCANCODE_KP_MINUS, { 0x4A, 0, 0x6D, '-', '-' } },
        { SDL_SCANCODE_KP_PLUS, { 0x4E, 0, 0x6B, '+', '+' } }, { SDL_SCANCODE_KP_ENTER, { 0x1C, 1, 0x0D, '\r', '\r' } },
        { SDL_SCANCODE_KP_1, { 0x4F, 0, 0x61, '1', '1' } }, { SDL_SCANCODE_KP_2, { 0x50, 0, 0x62, '2', '2' } },
        { SDL_SCANCODE_KP_3, { 0x51, 0, 0x63, '3', '3' } }, { SDL_SCANCODE_KP_4, { 0x4B, 0, 0x64, '4', '4' } },
        { SDL_SCANCODE_KP_5, { 0x4C, 0, 0x65, '5', '5' } }, { SDL_SCANCODE_KP_6, { 0x4D, 0, 0x66, '6', '6' } },
        { SDL_SCANCODE_KP_7, { 0x47, 0, 0x67, '7', '7' } }, { SDL_SCANCODE_KP_8, { 0x48, 0, 0x68, '8', '8' } },
        { SDL_SCANCODE_KP_9, { 0x49, 0, 0x69, '9', '9' } }, { SDL_SCANCODE_KP_0, { 0x52, 0, 0x60, '0', '0' } },
        { SDL_SCANCODE_KP_PERIOD, { 0x53, 0, 0x6E, '.', '.' } }, { SDL_SCANCODE_NONUSBACKSLASH, { 0x56, 0, 0xE2, '\\', '|' } },
        { SDL_SCANCODE_APPLICATION, { 0x5D, 1, 0x5D, 0, 0 } },
        { SDL_SCANCODE_LCTRL, { 0x1D, 0, 0xA2, 0, 0 } }, { SDL_SCANCODE_LSHIFT, { 0x2A, 0, 0xA0, 0, 0 } },
        { SDL_SCANCODE_LALT, { 0x38, 0, 0xA4, 0, 0 } }, { SDL_SCANCODE_LGUI, { 0x5B, 1, 0x5B, 0, 0 } },
        { SDL_SCANCODE_RCTRL, { 0x1D, 1, 0xA3, 0, 0 } }, { SDL_SCANCODE_RSHIFT, { 0x36, 0, 0xA1, 0, 0 } },
        { SDL_SCANCODE_RALT, { 0x38, 1, 0xA5, 0, 0 } }, { SDL_SCANCODE_RGUI, { 0x5C, 1, 0x5C, 0, 0 } },
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) keymap[t[i].s] = t[i].k;
    for (int i = 0; i < 12; i++) keymap[SDL_SCANCODE_F1 + i] = (KeyInfo){ (u8)(i < 10 ? 0x3B + i : 0x57 + (i - 10)), 0, (u8)(0x70 + i), 0, 0 };
}
static u8 generic_vk(u8 vk) {   // what WM_KEYDOWN reports: VK_SHIFT/CONTROL/MENU, not the left/right variants
    return vk == 0xA0 || vk == 0xA1 ? 0x10 : vk == 0xA2 || vk == 0xA3 ? 0x11 : vk == 0xA4 || vk == 0xA5 ? 0x12 : vk;
}
static const KeyInfo *key_by_vk(u32 vk) {
    init_keymap();
    for (int i = 0; i < 256; i++) if (keymap[i].vk && (keymap[i].vk == vk || generic_vk(keymap[i].vk) == vk) && !(keymap[i].ext && keymap[i].vk >= 0xA0 && vk < 0xA0)) return &keymap[i];
    return NULL;
}
static const KeyInfo *key_by_sc(u32 sc, int ext) {
    init_keymap();
    for (int i = 0; i < 256; i++) if (keymap[i].vk && keymap[i].sc == sc && keymap[i].ext == ext) return &keymap[i];
    return NULL;
}

static u32 vk_from_sdl(SDL_Keycode k, SDL_Scancode sc) {
    if (k >= 'a' && k <= 'z') return (u32)(k - 'a' + 'A');
    if (k >= '0' && k <= '9') return (u32)k;
    switch (k) {
    case SDLK_ESCAPE: return 0x1B; case SDLK_RETURN: return 0x0D; case SDLK_SPACE: return 0x20; case SDLK_TAB: return 0x09;
    case SDLK_BACKSPACE: return 0x08; case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26; case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28; case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10; case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12; case SDLK_DELETE: return 0x2E; case SDLK_INSERT: return 0x2D;
    case SDLK_HOME: return 0x24; case SDLK_END: return 0x23; case SDLK_PAGEUP: return 0x21; case SDLK_PAGEDOWN: return 0x22;
    case SDLK_LGUI: return 0x5B; case SDLK_RGUI: return 0x5C; case SDLK_CAPSLOCK: return 0x14;
    case SDLK_MINUS: return 0xBD; case SDLK_EQUALS: return 0xBB; case SDLK_COMMA: return 0xBC; case SDLK_PERIOD: return 0xBE;
    case SDLK_SLASH: return 0xBF; case SDLK_SEMICOLON: return 0xBA; case SDLK_APOSTROPHE: return 0xDE; case SDLK_GRAVE: return 0xC0;
    case SDLK_LEFTBRACKET: return 0xDB; case SDLK_RIGHTBRACKET: return 0xDD; case SDLK_BACKSLASH: return 0xDC;
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (u32)(k - SDLK_F1);
    (void)sc; return 0;
}

// WM_KEY* for one key event (SDL scancode/keycode), shared by SDL windows and detached-layer windows (ps5_gfx.m).
// chars: also post WM_CHAR (detached windows have no SDL text input).
static u32 vk_from_sdl(SDL_Keycode k, SDL_Scancode sc);
static void deliver_key(Win *w, int scancode, SDL_Keycode key, int down, int repeat, int chars) {
    init_keymap();
    const KeyInfo *ki = scancode >= 0 && scancode < 256 && keymap[scancode].vk ? &keymap[scancode] : NULL;
    u32 vk = ki ? ki->vk : vk_from_sdl(key, (SDL_Scancode)scancode); if (!vk) return;
    u8 gvk = generic_vk((u8)vk);
    keystate[vk] = down; keystate[gvk] = down;
    if (gvk != vk) keystate[gvk] = keystate[gvk == 0x10 ? 0xA0 : gvk == 0x11 ? 0xA2 : 0xA4] || keystate[gvk == 0x10 ? 0xA1 : gvk == 0x11 ? 0xA3 : 0xA5];
    int alt = keystate[0x12];
    u64 lp = 1 | ((u64)(ki ? ki->sc : 0) << 16) | ((u64)(ki && ki->ext) << 24) | ((u64)alt << 29) |
             (down ? (repeat ? 1ull << 30 : 0) : (3ull << 30));
    u32 msg = (alt || gvk == 0x12 || vk == 0x79 /* F10 */) && !keystate[0x11] ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (down ? WM_KEYDOWN : WM_KEYUP);
    post(w->hwnd, msg, gvk, lp);
    if (chars && down && ki && ki->lo && !keystate[0x11] && !alt) post(w->hwnd, WM_CHAR, (u8)(keystate[0x10] ? ki->hi : ki->lo), 1);
}

static void pump(void) {
    if (!pthread_main_np()) return;   // Cocoa: events can only be read on the main thread
    ensure_sdl();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        Win *w = NULL;
        switch (e.type) {
        case SDL_EVENT_QUIT:
            for (int i = 0; i < 256; i++) if (wins[i] && !wins[i]->destroyed && wins[i]->sdl) { post(wins[i]->hwnd, WM_CLOSE, 0, 0); break; }
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if ((w = win_by_sdl(e.window.windowID))) post(w->hwnd, WM_CLOSE, 0, 0);
            break;
        case SDL_EVENT_WINDOW_RESIZED:   // user resized the Mac window (ignored while fullscreen: the game keeps its size)
            if ((w = win_by_sdl(e.window.windowID)) && !w->fs && (e.window.data1 != w->w || e.window.data2 != w->h)) {
                w->w = e.window.data1; w->h = e.window.data2;
                post(w->hwnd, WM_SIZE, 0, (u32)w->w | ((u64)(u32)w->h << 16));
            }
            break;
        case SDL_EVENT_WINDOW_MOVED:
            if ((w = win_by_sdl(e.window.windowID)) && !w->fs) { w->x = e.window.data1; w->y = e.window.data2; }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if ((w = win_by_sdl(e.window.windowID))) { g_focus = w->hwnd; post(w->hwnd, WM_ACTIVATEAPP, 1, 0); post(w->hwnd, WM_SETFOCUS, 0, 0); }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if ((w = win_by_sdl(e.window.windowID))) { post(w->hwnd, WM_ACTIVATEAPP, 0, 0); post(w->hwnd, WM_KILLFOCUS, 0, 0); }
            break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
            if ((w = win_by_sdl(e.key.windowID))) deliver_key(w, e.key.scancode, e.key.key, e.type == SDL_EVENT_KEY_DOWN, e.key.repeat, 0);
            break;
        case SDL_EVENT_TEXT_INPUT:
            if ((w = win_by_sdl(e.text.windowID)))
                for (const unsigned char *s = (const unsigned char *)e.text.text; *s; s++) if (*s < 0x80) post(w->hwnd, WM_CHAR, *s, 1);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (raw_mouse) {
                raw_fx += e.motion.xrel; raw_fy += e.motion.yrel;
                i32 dx = (i32)raw_fx, dy = (i32)raw_fy;
                if (dx || dy) { raw_fx -= (float)dx; raw_fy -= (float)dy; raw_push(0, 0, dx, dy); }
            }
            if (relative_active) break;               // a locked cursor has no meaningful absolute position
            if ((w = win_by_sdl(e.motion.windowID))) {
                mouse_x = (int)e.motion.x; mouse_y = (int)e.motion.y;
                post(w->hwnd, WM_MOUSEMOVE, mouse_buttons, (u16)mouse_x | ((u64)(u16)mouse_y << 16));
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (!(w = win_by_sdl(e.button.windowID))) break;
            int down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN; u32 m = 0, bit = 0;
            if (e.button.button == SDL_BUTTON_LEFT) { m = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; bit = 1; keystate[1] = down; }
            else if (e.button.button == SDL_BUTTON_RIGHT) { m = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; bit = 2; keystate[2] = down; }
            else if (e.button.button == SDL_BUTTON_MIDDLE) { m = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; bit = 0x10; keystate[4] = down; }
            if (raw_mouse) {   // RI_MOUSE_{LEFT,RIGHT,MIDDLE}_BUTTON_{DOWN,UP}, X buttons
                static const u16 rb[6][2] = { {0,0}, {0x1,0x2}, {0x10,0x20}, {0x4,0x8}, {0x40,0x80}, {0x100,0x200} };
                if (e.button.button >= 1 && e.button.button <= 5) raw_push(rb[e.button.button][down ? 0 : 1], 0, 0, 0);
            }
            if (!m) break;
            mouse_buttons = down ? (mouse_buttons | bit) : (mouse_buttons & ~bit);
            post(w->hwnd, m, mouse_buttons, (u16)(int)e.button.x | ((u64)(u16)(int)e.button.y << 16));
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            if (raw_mouse && e.wheel.y) raw_push(0x0400 /* RI_MOUSE_WHEEL */, (i16)(e.wheel.y * 120), 0, 0);
            if (raw_mouse && e.wheel.x) raw_push(0x0800 /* RI_MOUSE_HWHEEL */, (i16)(e.wheel.x * 120), 0, 0);
            if ((w = win_by_sdl(e.wheel.windowID))) post(w->hwnd, WM_MOUSEWHEEL, ((u64)(u16)(i16)(e.wheel.y * 120) << 16) | mouse_buttons, 0);
            break;
        default: break;
        }
    }
}

// ---- input for windows without an SDL window (detached Metal layers, ps5_gfx.m) ----------------------------------
// ps5_gfx.m turns NSEvents of the NSWindow holding the layer into these calls (main thread); the messages go to the
// window's queue like SDL-sourced input, so the program's own window procedure (AnyPS5's SDL2) sees normal Win32 input.
void user32_detached_key(u64 hwnd, int sdl_scancode, int down, int repeat) {
    Win *w = win_get(hwnd); if (w) deliver_key(w, sdl_scancode, SDL_GetKeyFromScancode((SDL_Scancode)sdl_scancode, 0, false), down, repeat, 1);
}
void user32_detached_mouse(u64 hwnd, int kind, int x, int y, int arg) {   // kind 0 move, 1 button down, 2 up (arg 1 L 2 R 3 M), 3 wheel (arg = 120ths)
    Win *w = win_get(hwnd); if (!w) return;
    mouse_x = x; mouse_y = y;
    u64 lp = (u16)x | ((u64)(u16)y << 16);
    if (kind == 0) { post(hwnd, WM_MOUSEMOVE, mouse_buttons, lp); return; }
    if (kind == 3) { post(hwnd, WM_MOUSEWHEEL, ((u64)(u16)(i16)arg << 16) | mouse_buttons, lp); return; }
    int down = kind == 1; u32 bit = arg == 1 ? 1 : arg == 2 ? 2 : 0x10;
    u32 m = arg == 1 ? (down ? WM_LBUTTONDOWN : WM_LBUTTONUP) : arg == 2 ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_MBUTTONDOWN : WM_MBUTTONUP);
    keystate[arg == 1 ? 1 : arg == 2 ? 2 : 4] = down;
    mouse_buttons = down ? (mouse_buttons | bit) : (mouse_buttons & ~bit);
    post(hwnd, m, mouse_buttons, lp);
}
void user32_detached_focus(u64 hwnd, int on) {
    if (on) { g_focus = hwnd; post(hwnd, WM_ACTIVATEAPP, 1, 0); post(hwnd, WM_SETFOCUS, 0, 0); }
    else { post(hwnd, WM_ACTIVATEAPP, 0, 0); post(hwnd, WM_KILLFOCUS, 0, 0); }
}
// Test hook (like WINTOMAC_VIRTUAL_PAD / WINTOMAC_TEST_RAW_MOUSE): WINTOMAC_TEST_DETACHED_KEYS=1 types a scripted key
// sequence into the first detached window shown: Return (t 1.0-1.5 s), Up arrow (2.0-2.5 s), W (3.0-3.5 s).
// AnyPS5's built-in mapping turns them into Cross, D-pad up and left stick up (tests/ps5/pad_input.c).
static void *detached_keys_thread(void *arg) {
    u64 hwnd = (u64)(uintptr_t)arg;
    static const struct { int ms, sc, down; } seq[] = {
        { 1000, SDL_SCANCODE_RETURN, 1 }, { 1500, SDL_SCANCODE_RETURN, 0 }, { 2000, SDL_SCANCODE_UP, 1 }, { 2500, SDL_SCANCODE_UP, 0 },
        { 3000, SDL_SCANCODE_W, 1 }, { 3500, SDL_SCANCODE_W, 0 } };
    int at = 0;
    for (unsigned i = 0; i < sizeof seq / sizeof *seq; i++) {
        usleep((useconds_t)(seq[i].ms - at) * 1000); at = seq[i].ms;
        user32_detached_key(hwnd, seq[i].sc, seq[i].down, 0);
    }
    return NULL;
}
static void detached_test_keys(u64 hwnd) {
    static int started;
    if (started || !getenv("WINTOMAC_TEST_DETACHED_KEYS")) return;
    started = 1;
    pthread_t t; pthread_create(&t, NULL, detached_keys_thread, (void *)(uintptr_t)hwnd); pthread_detach(t);
}

static int match(const Msg *m, u64 hwnd, u32 lo, u32 hi) {
    if (hwnd && m->hwnd != hwnd) return 0;
    if (lo || hi) return m->msg >= lo && m->msg <= hi;
    return 1;
}
static void write_msg(u64 p, const Msg *m) {
    ST64(p, m->hwnd); ST32(p + 8, m->msg); ST32(p + 12, 0); ST64(p + 16, m->wp); ST64(p + 24, m->lp);
    ST32(p + 32, m->time); ST32(p + 36, (u32)m->x); ST32(p + 40, (u32)m->y); ST32(p + 44, 0);
}
static int take_locked(u64 out, u64 hwnd, u32 lo, u32 hi, int remove);
static int take(u64 out, u64 hwnd, u32 lo, u32 hi, int remove) {
    pthread_mutex_lock(&qlock); int r = take_locked(out, hwnd, lo, hi, remove); pthread_mutex_unlock(&qlock); return r;
}
static int take_locked(u64 out, u64 hwnd, u32 lo, u32 hi, int remove) {
    if (quit_posted && qhead == qtail) {
        Msg q = { 0, WM_QUIT, (u64)(i64)quit_code, 0, (u32)SDL_GetTicks(), 0, 0 };
        write_msg(out, &q); if (remove) quit_posted = 0; return 1;
    }
    for (int i = qhead; i != qtail; i = (i + 1) % QCAP) {
        if (!match(&queue[i], hwnd, lo, hi)) continue;
        write_msg(out, &queue[i]);
        if (remove) {  // remove entry i, preserving order
            for (int j = i; (j + 1) % QCAP != qtail; j = (j + 1) % QCAP) queue[j] = queue[(j + 1) % QCAP];
            qtail = (qtail - 1 + QCAP) % QCAP;
        }
        return 1;
    }
    return 0;
}
SHIM(PeekMessageW) { pump(); RET(take(A1, A2, (u32)A3, (u32)A4, (int)(AN(5) & 1))); }
SHIM(PeekMessageA) { sh_PeekMessageW(c); }
SHIM(GetMessageW) {
    for (;;) {
        pump();
        if (take(A1, A2, (u32)A3, (u32)A4, 1)) RET(LD32(A1 + 8) != WM_QUIT);
        SDL_WaitEventTimeout(NULL, 10);
    }
}
SHIM(GetMessageA) { sh_GetMessageW(c); }
SHIM(WaitMessage) { SDL_WaitEventTimeout(NULL, 10); RET(1); }
SHIM(TranslateMessage) { RET(0); }  // WM_CHAR comes from SDL text input instead
SHIM(DispatchMessageW) {
    u64 m = A1; Win *w = win_get(LD64(m));
    if (!w) RET(0);
    RET(send(c, w, LD32(m + 8), LD64(m + 16), LD64(m + 24)));
}
SHIM(DispatchMessageA) { sh_DispatchMessageW(c); }
SHIM(PostQuitMessage) { quit_posted = 1; quit_code = (int)(i32)A1; RET(0); }
SHIM(PostMessageW) { post(A1, (u32)A2, A3, A4); RET(1); }
SHIM(PostMessageA) { sh_PostMessageW(c); }
SHIM(SendMessageW) { Win *w = win_get(A1); RET(w ? send(c, w, (u32)A2, A3, A4) : 0); }
SHIM(SendMessageA) { sh_SendMessageW(c); }
SHIM(CallWindowProcW) { u64 args[4] = { A2, A3, A4, AN(5) }; RET(rt_guest_call(c, A1, 4, args)); }
SHIM(CallWindowProcA) { sh_CallWindowProcW(c); }
SHIM(DefWindowProcW) {
    u32 msg = (u32)A2;
    switch (msg) {
    case WM_CLOSE: { Win *w = win_get(A1); destroy_window(c, w); RET(0); }
    case WM_NCCREATE: RET(1);
    default: RET(0);
    }
}
SHIM(DefWindowProcA) { sh_DefWindowProcW(c); }
SHIM(GetQueueStatus) { pump(); RET(qhead != qtail ? 0x00FF00FF : 0); }
extern u32 rt_wait_handles(u32 n, u64 handles, int all, u32 ms);   // threads.c
// Returns WAIT_OBJECT_0+i for a handle, WAIT_OBJECT_0+n when input matching `mask` is available, or WAIT_TIMEOUT.
static u32 msg_wait(u32 n, u64 handles, int all, u32 ms, u32 mask) {
    u64 start = SDL_GetTicks();
    for (;;) {
        if (n) { u32 r = rt_wait_handles(n, handles, all, 0); if (r < n) return r; }
        pump();
        pthread_mutex_lock(&raw_lock); int raw = raw_tail != raw_head; pthread_mutex_unlock(&raw_lock);
        if (raw && (mask & 0x0400)) return n;                                // QS_RAWINPUT
        pthread_mutex_lock(&qlock); int msgs = qhead != qtail || quit_posted; pthread_mutex_unlock(&qlock);
        if (msgs && (mask & ~0x0400u)) return n;
        if (ms != 0xFFFFFFFF && SDL_GetTicks() - start >= ms) return 0x102;  // WAIT_TIMEOUT
        if (pthread_main_np()) SDL_WaitEventTimeout(NULL, 2);
        else {
            pthread_mutex_lock(&raw_lock);
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 2000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            if (raw_tail == raw_head) pthread_cond_timedwait(&raw_cond, &raw_lock, &ts);
            pthread_mutex_unlock(&raw_lock);
        }
    }
}
SHIM(MsgWaitForMultipleObjects) { RET(msg_wait((u32)A1, A2, (int)(u32)A3, (u32)A4, (u32)AN(5))); }   // (n, handles, all, ms, mask)
SHIM(MsgWaitForMultipleObjectsEx) { RET(msg_wait((u32)A1, A2, (int)((u32)AN(5) & 1), (u32)A3, (u32)A4)); }   // (n, handles, ms, mask, flags)

// ---- input state ---------------------------------------------------------------------
SHIM(GetKeyState) { u32 vk = (u32)A1 & 0xFF; RET(keystate[vk] ? 0xFF80 : 0); }
SHIM(GetAsyncKeyState) { u32 vk = (u32)A1 & 0xFF; RET(keystate[vk] ? 0x8000 : 0); }
SHIM(GetKeyboardState) { memcpy((void *)A1, keystate, 256); for (int i = 0; i < 256; i++) ((u8 *)A1)[i] = keystate[i] ? 0x80 : 0; RET(1); }
SHIM(GetCursorPos) { float x, y; SDL_GetGlobalMouseState(&x, &y); ST32(A1, (u32)(int)x); ST32(A1 + 4, (u32)(int)y); RET(1); }
SHIM(SetCursorPos) { SDL_WarpMouseGlobal((float)(i32)A1, (float)(i32)A2); RET(1); }
SHIM(ScreenToClient) {
    Win *w = win_get(A1); int x = 0, y = 0; if (w && w->sdl) SDL_GetWindowPosition(w->sdl, &x, &y);
    ST32(A2, (u32)((i32)LD32(A2) - x)); ST32(A2 + 4, (u32)((i32)LD32(A2 + 4) - y)); RET(1);
}
SHIM(ClientToScreen) {
    Win *w = win_get(A1); int x = 0, y = 0; if (w && w->sdl) SDL_GetWindowPosition(w->sdl, &x, &y);
    ST32(A2, (u32)((i32)LD32(A2) + x)); ST32(A2 + 4, (u32)((i32)LD32(A2 + 4) + y)); RET(1);
}
static int cursor_count, cursor_null, clip_tiny;
SHIM(ShowCursor) { cursor_count += (int)(u32)A1 ? 1 : -1; update_relative(); RET((u32)cursor_count); }
SHIM(SetCursor) { static u64 cur = 0x10001; u64 old = cur; cur = A1; cursor_null = !A1; update_relative(); RET(old); }
SHIM(LoadCursorW) { RET(0x10001); }
SHIM(LoadCursorA) { RET(0x10001); }
SHIM(LoadIconW) { RET(0x10002); }
SHIM(LoadIconA) { RET(0x10002); }
SHIM(LoadImageW) { RET(0x10003); }
SHIM(LoadImageA) { RET(0x10003); }
SHIM(ClipCursor) {  // RECT* or NULL. A clip of a few pixels with raw input = "mouse look" (SDL's relative mode)
    clip_tiny = A1 && (i32)LD32(A1 + 8) - (i32)LD32(A1) <= 4 && (i32)LD32(A1 + 12) - (i32)LD32(A1 + 4) <= 4;
    update_relative(); RET(1);
}
SHIM(GetClipCursor) { int w, h, hz; extern void rt_display_mode(int *, int *, int *); rt_display_mode(&w, &h, &hz);
    ST32(A1, 0); ST32(A1 + 4, 0); ST32(A1 + 8, (u32)w); ST32(A1 + 12, (u32)h); RET(1); }
static void update_relative(void) {
    int want = raw_mouse && (clip_tiny || cursor_count < 0 || cursor_null);
    if (pthread_main_np()) { if (want) SDL_HideCursor(); else if (cursor_count >= 0 && !cursor_null) SDL_ShowCursor(); }
    if (want == relative_active || !pthread_main_np()) return;
    relative_active = want;
    for (int i = 0; i < 256; i++) if (wins[i] && !wins[i]->destroyed && wins[i]->sdl) SDL_SetWindowRelativeMouseMode(wins[i]->sdl, want);
    if (getenv("WINTOMAC_TRACE_INPUT")) rt_log("input: relative mouse %s", want ? "on" : "off");
}
SHIM(SetCapture) { RET(0); }
SHIM(ReleaseCapture) { RET(1); }
SHIM(GetCapture) { RET(0); }

// ---- OpenGL support (runtime/opengl.c) ------------------------------------------------
// GetDC hands out 0x20000 + the low 16 bits of the HWND; map it back.
u64 user32_hwnd_from_dc(u64 hdc) {
    if (hdc < 0x20000 || hdc > 0x2FFFF) return 0;
    for (int i = 0; i < 256; i++)
        if (wins[i] && !wins[i]->destroyed && (wins[i]->hwnd & 0xFFFF) == hdc - 0x20000) return wins[i]->hwnd;
    return 0;
}
int user32_pixel_format(u64 hwnd, int set) {  // set > 0 stores it; returns the window's format
    Win *w = win_get(hwnd); if (!w) return 0;
    if (set > 0) w->pixfmt = set;
    return w->pixfmt;
}
// The SDL window behind an HWND, made OpenGL-capable. Windows are created for Metal (Vulkan/DXVK), and
// SDL can't put a GL context on those, so the first time a game sets a pixel format the native window is
// swapped for an identical one with SDL_WINDOW_OPENGL. The HWND, its logical size and state stay the same.
void *user32_gl_window(u64 hwnd, int *lw, int *lh) {
    Win *w = win_get(hwnd);
    if (!w || !w->sdl) return NULL;
    if (lw) { *lw = w->w; *lh = w->h; }
    SDL_WindowFlags f = SDL_GetWindowFlags(w->sdl);
    if (f & SDL_WINDOW_OPENGL) return w->sdl;
    if (w->view) { rt_log("opengl: window %llx already renders through Metal/Vulkan", (unsigned long long)hwnd); return NULL; }
    if (!pthread_main_np()) { rt_log("opengl: can't recreate window %llx off the main thread", (unsigned long long)hwnd); return NULL; }
    int x, y, ww, hh;
    SDL_GetWindowPosition(w->sdl, &x, &y); SDL_GetWindowSize(w->sdl, &ww, &hh);
    SDL_Window *n = SDL_CreateWindow(w->title ? w->title : "", ww, hh,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | (f & (SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS)));
    if (!n) { rt_log("opengl: SDL_CreateWindow failed: %s", SDL_GetError()); return NULL; }
    SDL_SetWindowPosition(n, x, y);
    SDL_DestroyWindow(w->sdl);
    w->sdl = n;
    if (w->fs && !headless()) SDL_SetWindowFullscreen(n, true);
    if (w->shown && !headless()) { SDL_ShowWindow(n); SDL_RaiseWindow(n); }
    SDL_StartTextInput(n);
    if (relative_active) SDL_SetWindowRelativeMouseMode(n, true);
    SDL_SyncWindow(n);
    return n;
}

// ---- focus, desktop, misc -------------------------------------------------------------
// Focus/foreground: the window the user is actually interacting with. Hidden helper windows
// (SDL's message-only "input catcher", created before the game window) must never be reported,
// or SDL decides the game window has no keyboard focus and drops every key.
static u64 first_hwnd(void) {
    Win *f = win_get(g_focus);
    if (f && !f->destroyed && f->sdl) return g_focus;
    for (int i = 0; i < 256; i++) if (wins[i] && !wins[i]->destroyed && wins[i]->sdl) return wins[i]->hwnd;
    return 0;
}
static u64 set_focus(u64 hwnd) {  // returns the previous focus window
    u64 prev = first_hwnd();
    Win *w = win_get(hwnd);
    if (w && w->sdl && !w->destroyed) g_focus = hwnd;
    return prev;
}
SHIM(SetFocus) { RET(set_focus(A1)); }
SHIM(GetFocus) { RET(first_hwnd()); }
SHIM(GetActiveWindow) { RET(first_hwnd()); }
SHIM(GetForegroundWindow) { RET(first_hwnd()); }
SHIM(SetForegroundWindow) { Win *w = win_get(A1); if (w && w->sdl && !headless()) SDL_RaiseWindow(w->sdl); set_focus(A1); RET(1); }
SHIM(SetActiveWindow) { RET(set_focus(A1)); }
SHIM(GetDesktopWindow) { RET(0x10010); }
SHIM(GetSystemMetrics) {
    int vw, vh, hz; rt_display_mode(&vw, &vh, &hz);
    switch ((i32)A1) {
    case 0: case 16: case 78: RET(vw);                 // SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
    case 1: case 17: case 79: RET(vh);
    case 80: RET(1);                                  // SM_CMONITORS
    default: RET(0);
    }
}
SHIM(MonitorFromWindow) { RET(0x10020); }
SHIM(MonitorFromRect) { RET(0x10020); }   // one virtual monitor
SHIM(MonitorFromPoint) { RET(0x10020); }
SHIM(GetMonitorInfoW) {  // MONITORINFO: cbSize, rcMonitor, rcWork, dwFlags
    int w, h, hz; rt_display_mode(&w, &h, &hz);
    for (int i = 0; i < 2; i++) { u64 r = A2 + 4 + 16 * i; ST32(r, 0); ST32(r + 4, 0); ST32(r + 8, (u32)w); ST32(r + 12, (u32)h); }
    ST32(A2 + 36, 1);
    if (LD32(A2) >= 104) { const char *n = "\\\\.\\DISPLAY1"; for (int i = 0; ; i++) { ST16(A2 + 40 + 2 * i, (u8)n[i]); if (!n[i]) break; } }  // MONITORINFOEXW.szDevice
    RET(1);
}
SHIM(GetMonitorInfoA) {
    u64 p = A2; u32 cb = LD32(p); sh_GetMonitorInfoW(c);
    if (cb >= 72) { memset((void *)(p + 40), 0, 32); strcpy((char *)(p + 40), "\\\\.\\DISPLAY1"); }  // MONITORINFOEXA.szDevice
    RET(1);
}
SHIM(MessageBoxW) { char *t = utf8_from_w(A2), *cap = utf8_from_w(A3); fprintf(stderr, "[MessageBox] %s: %s\n", cap, t); free(t); free(cap); RET(1); }
SHIM(MessageBoxA) { fprintf(stderr, "[MessageBox] %s: %s\n", A3 ? (char *)A3 : "", A2 ? (char *)A2 : ""); RET(1); }
SHIM(SetProcessDPIAware) { RET(1); }
SHIM(SetProcessDpiAwarenessContext) { RET(1); }
SHIM(GetDpiForWindow) { RET(96); }
SHIM(GetDC) { RET(0x20000 + (A1 & 0xFFFF)); }
SHIM(ReleaseDC) { RET(1); }
SHIM(BeginPaint) { memset((void *)A2, 0, 72); ST64(A2, 0x20000); RET(0x20000); }
SHIM(EndPaint) { RET(1); }
SHIM(ValidateRect) { RET(1); }
SHIM(InvalidateRect) { RET(1); }
SHIM(GetStockObject) { RET(0x30000 + (u32)A1); }
SHIM(SetRect) { ST32(A1, (u32)A2); ST32(A1 + 4, (u32)A3); ST32(A1 + 8, (u32)A4); ST32(A1 + 12, (u32)AN(5)); RET(1); }
SHIM(OffsetRect) { for (int i = 0; i < 4; i++) ST32(A1 + 4 * i, LD32(A1 + 4 * i) + (u32)(i & 1 ? A3 : A2)); RET(1); }
SHIM(SetTimer) { RET(A2 ? A2 : 1); }
SHIM(KillTimer) { RET(1); }
// RAWINPUTDEVICE { u16 usagePage, usage; u32 flags; HWND target } (16 bytes)
SHIM(RegisterRawInputDevices) {
    for (u32 i = 0; i < (u32)A2; i++) {
        u64 d = A1 + 16ull * i;
        u16 page = LD16(d), usage = LD16(d + 2); u32 fl = LD32(d + 4);
        if (page != 1 || usage != 2) continue;                 // generic desktop / mouse (keyboard: WM_KEY* is used)
        if (fl & 1) { raw_mouse = 0; raw_mouse_hwnd = 0; }     // RIDEV_REMOVE
        else { raw_mouse = 1; raw_mouse_hwnd = LD64(d + 8); raw_mouse_flags = fl; raw_fx = raw_fy = 0; }
        if (getenv("WINTOMAC_TRACE_INPUT")) rt_log("input: raw mouse %s (flags %x)", raw_mouse ? "registered" : "removed", fl);
    }
    update_relative();
    // Test hook (like WINTOMAC_VIRTUAL_PAD): scripted mouse events, processed by pump() exactly like real ones.
    if (raw_mouse && getenv("WINTOMAC_TEST_RAW_MOUSE")) {
        ensure_sdl();
        SDL_Window *sw = NULL; for (int i = 0; i < 256 && !sw; i++) if (wins[i] && !wins[i]->destroyed && wins[i]->sdl) sw = wins[i]->sdl;
        SDL_WindowID id = sw ? SDL_GetWindowID(sw) : 0;
        static const float mv[3][2] = { { 10.5f, -3.0f }, { 0.5f, 1.0f }, { -4.0f, 0.25f } };
        for (int i = 0; i < 3; i++) { SDL_Event e = { 0 }; e.type = SDL_EVENT_MOUSE_MOTION; e.motion.windowID = id; e.motion.xrel = mv[i][0]; e.motion.yrel = mv[i][1]; SDL_PushEvent(&e); }
        SDL_Event b = { 0 }; b.type = SDL_EVENT_MOUSE_BUTTON_DOWN; b.button.windowID = id; b.button.button = SDL_BUTTON_LEFT; b.button.down = true; SDL_PushEvent(&b);
        b.type = SDL_EVENT_MOUSE_BUTTON_UP; b.button.down = false; SDL_PushEvent(&b);
        SDL_Event wh = { 0 }; wh.type = SDL_EVENT_MOUSE_WHEEL; wh.wheel.windowID = id; wh.wheel.y = -1; SDL_PushEvent(&wh);
    }
    RET(1);
}
SHIM(GetRegisteredRawInputDevices) {
    if (!raw_mouse) { ST32(A2, 0); RET(0); }
    if (!A1 || LD32(A2) < 1) { ST32(A2, 1); rt_set_last_error(122); RET((u32)-1); }
    ST16(A1, 1); ST16(A1 + 2, 2); ST32(A1 + 4, raw_mouse_flags); ST64(A1 + 8, raw_mouse_hwnd); RET(1);
}
// GetRawInputData(HRAWINPUT, command, data, *size, headerSize)
SHIM(GetRawInputData) {
    u64 h = A1; u32 cmd = (u32)A2;
    if ((h & ~0xFFFFFFull) != 0x7A000000ull) { rt_set_last_error(6); RET((u32)-1); }
    u32 need = cmd == 0x10000005 ? 24 : 48;                     // RID_HEADER : RID_INPUT
    if (!A3) { ST32(A4, need); RET(0); }
    if (LD32(A4) < need) { rt_set_last_error(122); RET((u32)-1); }
    pthread_mutex_lock(&raw_lock);
    u64 idx = (raw_tail & ~0xFFFFFFull) | (h & 0xFFFFFF);
    if (idx >= raw_tail) idx -= 0x1000000;
    int ok = idx >= raw_head && idx < raw_tail;
    RawPkt p = ok ? rawq[idx % RAWQ] : (RawPkt){ 0 };
    if (ok && cmd != 0x10000005) rawq[idx % RAWQ].consumed = 1;            // read once, either way (like Windows)
    while (raw_head < raw_tail && rawq[raw_head % RAWQ].consumed) raw_head++;
    pthread_mutex_unlock(&raw_lock);
    if (!ok) { rt_set_last_error(6); RET((u32)-1); }
    u8 tmp[48]; write_raw((u64)tmp, &p); memcpy((void *)A3, tmp, need);
    RET(need);
}
// GetRawInputBuffer(data, *size, headerSize): drains queued packets into 8-byte-aligned RAWINPUT records.
SHIM(GetRawInputBuffer) {
    if (!A1) { ST32(A2, 48); RET(0); }
    u32 cap = LD32(A2), n = 0;
    pthread_mutex_lock(&raw_lock);
    while (raw_head < raw_tail && (n + 1) * 48 <= cap) {
        if (!rawq[raw_head % RAWQ].consumed) { write_raw(A1 + 48ull * n, &rawq[raw_head % RAWQ]); n++; }
        raw_head++;
    }
    u64 head = raw_head;
    pthread_mutex_unlock(&raw_lock);
    // Drained input no longer has a WM_INPUT to deliver (Windows removes them too): purge, so the queue can't flood.
    pthread_mutex_lock(&qlock);
    int o = qhead;
    for (int i = qhead; i != qtail; i = (i + 1) % QCAP) {
        Msg *m = &queue[i];
        if (m->msg == 0x00FF && (m->lp & ~0xFFFFFFull) == 0x7A000000ull) {
            u64 idx = (head & ~0xFFFFFFull) | (m->lp & 0xFFFFFF); if (idx > head) idx -= 0x1000000;
            if (idx < head) continue;
        }
        queue[o] = *m; o = (o + 1) % QCAP;
    }
    qtail = o;
    pthread_mutex_unlock(&qlock);
    RET(n);
}
// One mouse and one keyboard. RAWINPUTDEVICELIST { HANDLE device; u32 type } (16 bytes)
SHIM(GetRawInputDeviceList) {
    if (!A1) { ST32(A2, 2); RET(0); }
    if (LD32(A2) < 2) { ST32(A2, 2); rt_set_last_error(122); RET((u32)-1); }
    ST64(A1, RAW_MOUSE_DEV); ST32(A1 + 8, 0); ST64(A1 + 16, RAW_KBD_DEV); ST32(A1 + 24, 1);
    RET(2);
}
static u32 raw_dev_info(u64 dev, u32 cmd, u64 data, u64 size, int wide) {
    int mouse = dev == RAW_MOUSE_DEV;
    if (dev != RAW_MOUSE_DEV && dev != RAW_KBD_DEV) { rt_set_last_error(6); return (u32)-1; }
    if (cmd == 0x20000007) {   // RIDI_DEVICENAME (size in characters)
        const char *nm = mouse ? "\\\\?\\HID#VID_05AC&PID_0001#wintomac_mouse#{378de44c-56ef-11d1-bc8c-00a0c91405dd}"
                               : "\\\\?\\HID#VID_05AC&PID_0002#wintomac_keyboard#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}";
        u32 len = (u32)strlen(nm) + 1;
        if (!data || LD32(size) < len) { ST32(size, len); if (data) { rt_set_last_error(122); return (u32)-1; } return 0; }
        if (wide) for (u32 i = 0; i < len; i++) ST16(data + 2 * i, (u8)nm[i]); else memcpy((void *)data, nm, len);
        return len - 1;
    }
    if (cmd == 0x2000000b) {   // RIDI_DEVICEINFO: RID_DEVICE_INFO (32 bytes)
        if (!data || LD32(size) < 32) { ST32(size, 32); if (data) { rt_set_last_error(122); return (u32)-1; } return 0; }
        memset((void *)data, 0, 32); ST32(data, 32); ST32(data + 4, mouse ? 0 : 1);
        if (mouse) { ST32(data + 8, 0x100); ST32(data + 12, 5); ST32(data + 16, 125); ST32(data + 20, 1); }
        else { ST32(data + 8, 0x51); ST32(data + 12, 0); ST32(data + 16, 1); ST32(data + 20, 12); ST32(data + 24, 3); ST32(data + 28, 101); }
        return 32;
    }
    rt_set_last_error(1); return (u32)-1;   // RIDI_PREPARSEDDATA etc.: not a HID device here
}
SHIM(GetRawInputDeviceInfoA) { RET(raw_dev_info(A1, (u32)A2, A3, A4, 0)); }
SHIM(GetRawInputDeviceInfoW) { RET(raw_dev_info(A1, (u32)A2, A3, A4, 1)); }
SHIM(DefRawInputProc) { RET(0); }

// ---- keyboard queries (US layout, from keymap) ---------------------------------------------------
SHIM(MapVirtualKeyW) {  // (code, type): 0 VK->VSC, 1 VSC->VK, 2 VK->CHAR, 3 VSC->VK_EX, 4 VK->VSC_EX
    u32 code = (u32)A1, type = (u32)A2; const KeyInfo *k;
    switch (type) {
    case 0: case 4: k = key_by_vk(code); RET(k ? (type == 4 && k->ext ? 0xE000u | k->sc : k->sc) : 0);
    case 1: case 3: {
        int ext = (code >> 8) == 0xE0; k = key_by_sc(code & 0xFF, ext);
        if (!k && !ext) k = key_by_sc(code & 0xFF, 1);
        RET(k ? (type == 1 ? generic_vk(k->vk) : k->vk) : 0);
    }
    case 2: k = key_by_vk(code); RET(k && k->hi ? (u32)(k->lo >= 'a' && k->lo <= 'z' ? k->hi : k->lo) : 0);
    default: RET(0);
    }
}
SHIM(MapVirtualKeyA) { sh_MapVirtualKeyW(c); }
SHIM(MapVirtualKeyExW) { sh_MapVirtualKeyW(c); }
static int key_char(u32 vk, u64 state) {
    const KeyInfo *k = key_by_vk(vk); if (!k || !k->lo) return 0;
    int shift = state ? (LD8(state + 0x10) & 0x80) != 0 : keystate[0x10];
    int caps = state ? (LD8(state + 0x14) & 1) != 0 : 0;
    int letter = k->lo >= 'a' && k->lo <= 'z';
    return (shift ^ (letter && caps)) ? k->hi : k->lo;
}
SHIM(ToUnicode) { int ch = key_char((u32)A1, A3); if (!ch || !(i32)AN(5)) RET(0); ST16(A4, (u16)ch); if ((i32)AN(5) > 1) ST16(A4 + 2, 0); RET(1); }   // (vk, scan, state, buf, n, flags)
SHIM(ToUnicodeEx) { sh_ToUnicode(c); }
SHIM(ToAscii) { int ch = key_char((u32)A1, A3); if (!ch) RET(0); ST16(A4, (u16)ch); RET(1); }   // (vk, scan, state, WORD *out, flags)
SHIM(VkKeyScanA) {
    init_keymap(); char ch = (char)A1;
    for (int i = 0; i < 256; i++) { if (!keymap[i].vk) continue; if (keymap[i].lo == ch) RET(generic_vk(keymap[i].vk)); if (keymap[i].hi == ch) RET(0x100 | generic_vk(keymap[i].vk)); }
    RET(0xFFFF);
}
SHIM(VkKeyScanW) { sh_VkKeyScanA(c); }
SHIM(GetKeyboardLayout) { RET(0x04090409); }   // en-US
SHIM(GetKeyboardLayoutList) { if ((i32)A1 > 0 && A2) ST64(A2, 0x04090409); RET(1); }
SHIM(GetKeyNameTextW) {  // (lParam, buf, n)
    u32 sc = (u32)(A1 >> 16) & 0xFF; int ext = (A1 >> 24) & 1; const KeyInfo *k = key_by_sc(sc, ext);
    char b[16]; if (k && k->hi > ' ') snprintf(b, sizeof b, "%c", k->hi); else snprintf(b, sizeof b, "Key %02X", sc);
    i32 n = (i32)A3; if (n <= 0) RET(0); i32 i = 0; for (; b[i] && i < n - 1; i++) ST16(A2 + 2 * i, (u8)b[i]); ST16(A2 + 2 * i, 0); RET(i);
}

// ---- classes, properties, text, enumeration ---------------------------------------------------------
static u32 class_info(u64 cls, u64 out, int wide) {   // WNDCLASSEX: cbSize, style, wndproc, extra, hinst, icon, cursor, bg, menu, name, iconsm
    int k = find_class(cls, wide); if (k < 0) { rt_set_last_error(1411); return 0; }   // ERROR_CLASS_DOES_NOT_EXIST
    ST32(out + 4, classes[k].style); ST64(out + 8, classes[k].wndproc); ST32(out + 16, 0); ST32(out + 20, (u32)classes[k].wnd_extra);
    ST64(out + 24, classes[k].hinst); ST64(out + 40, classes[k].cursor); ST64(out + 48, classes[k].bg);
    return classes[k].atom;
}
SHIM(GetClassInfoExW) { RET(class_info(A2, A3, 1)); }   // (hinst, name, WNDCLASSEXW*)
SHIM(GetClassInfoExA) { RET(class_info(A2, A3, 0)); }
SHIM(GetClassInfoW) { u8 tmp[80]; u32 a = class_info(A2, (u64)tmp, 1); if (a) memcpy((void *)A3, tmp + 4, 72); RET(a); }
static int prop_slot(Win *w, u64 key, int wide, int create) {
    char nm[64] = ""; u64 atom = 0;
    if (key < 0x10000) atom = key; else if (wide) { char *t = utf8_from_w(key); snprintf(nm, sizeof nm, "%s", t); free(t); } else snprintf(nm, sizeof nm, "%s", (char *)key);
    for (int i = 0; i < 16; i++) if ((atom && w->props[i].atom == atom) || (!atom && w->props[i].name[0] && !strcmp(w->props[i].name, nm))) return i;
    if (!create) return -1;
    for (int i = 0; i < 16; i++) if (!w->props[i].atom && !w->props[i].name[0]) { w->props[i].atom = atom; snprintf(w->props[i].name, 64, "%s", nm); return i; }
    return -1;
}
static u64 set_prop(u64 hwnd, u64 key, u64 val, int wide) { Win *w = win_get(hwnd); int i = w ? prop_slot(w, key, wide, 1) : -1; if (i < 0) return 0; w->props[i].val = val; return 1; }
static u64 get_prop(u64 hwnd, u64 key, int wide) { Win *w = win_get(hwnd); int i = w ? prop_slot(w, key, wide, 0) : -1; return i < 0 ? 0 : w->props[i].val; }
static u64 remove_prop(u64 hwnd, u64 key, int wide) {
    Win *w = win_get(hwnd); int i = w ? prop_slot(w, key, wide, 0) : -1; if (i < 0) return 0;
    u64 v = w->props[i].val; memset(&w->props[i], 0, sizeof w->props[i]); return v;
}
SHIM(SetPropW) { RET(set_prop(A1, A2, A3, 1)); }
SHIM(SetPropA) { RET(set_prop(A1, A2, A3, 0)); }
SHIM(GetPropW) { RET(get_prop(A1, A2, 1)); }
SHIM(GetPropA) { RET(get_prop(A1, A2, 0)); }
SHIM(RemovePropW) { RET(remove_prop(A1, A2, 1)); }
SHIM(RemovePropA) { RET(remove_prop(A1, A2, 0)); }
SHIM(GetWindowTextLengthW) { Win *w = win_get(A1); RET(w && w->title ? strlen(w->title) : 0); }
SHIM(GetWindowTextLengthA) { sh_GetWindowTextLengthW(c); }
SHIM(GetWindowTextW) {  // (hwnd, buf, n)
    Win *w = win_get(A1); i32 n = (i32)A3; if (!w || n <= 0) RET(0);
    const char *t = w->title ? w->title : ""; i32 i = 0; for (; t[i] && i < n - 1; i++) ST16(A2 + 2 * i, (u8)t[i]); ST16(A2 + 2 * i, 0); RET(i);
}
SHIM(GetWindowTextA) { Win *w = win_get(A1); i32 n = (i32)A3; if (!w || n <= 0) RET(0); snprintf((char *)A2, (size_t)n, "%s", w->title ? w->title : ""); RET(strlen((char *)A2)); }
SHIM(GetWindowThreadProcessId) { if (A2) ST32(A2, (u32)getpid()); RET(1); }
SHIM(EnumWindows) {  // (WNDENUMPROC, lparam): top-level windows
    for (int i = 0; i < 256; i++) {
        Win *w = wins[i]; if (!w || w->destroyed || (w->style & 0x40000000u)) continue;
        u64 args[2] = { w->hwnd, A2 };
        if (!(u32)rt_guest_call(c, A1, 2, args)) break;
    }
    RET(1);
}
SHIM(IsZoomed) { Win *w = win_get(A1); RET(w && w->sdl && pthread_main_np() && (SDL_GetWindowFlags(w->sdl) & SDL_WINDOW_MAXIMIZED)); }
SHIM(GetWindowPlacement) {  // WINDOWPLACEMENT { len, flags, showCmd, ptMin, ptMax, rcNormal }
    Win *w = win_get(A1); if (!w) RET(0);
    u64 p = A2; ST32(p + 4, 0); ST32(p + 8, 1 /* SW_SHOWNORMAL */); memset((void *)(p + 12), 0, 16);
    ST32(p + 28, (u32)w->x); ST32(p + 32, (u32)w->y); ST32(p + 36, (u32)(w->x + w->w)); ST32(p + 40, (u32)(w->y + w->h)); RET(1);
}
SHIM(SetWindowPlacement) { RET(1); }
SHIM(SetParent) { Win *w = win_get(A1); if (!w) RET(0); u64 old = w->parent; w->parent = A2; RET(old); }
SHIM(GetParent) { Win *w = win_get(A1); RET(w ? w->parent : 0); }
SHIM(GetMessagePos) { RET((u32)(u16)mouse_x | ((u32)(u16)mouse_y << 16)); }
SHIM(GetMessageTime) { RET((u32)SDL_GetTicks()); }
SHIM(GetMessageExtraInfo) { RET(0); }
SHIM(PostThreadMessageW) { post(0, (u32)A2, A3, A4); RET(1); }
SHIM(PostThreadMessageA) { sh_PostThreadMessageW(c); }
SHIM(GetDoubleClickTime) { RET(500); }
static char *wm_names[256]; static int nwm;
static u32 register_message(const char *name) {
    for (int i = 0; i < nwm; i++) if (!strcasecmp(wm_names[i], name)) return 0xC000u + (u32)i;
    if (nwm >= 256) return 0;
    wm_names[nwm] = strdup(name); return 0xC000u + (u32)nwm++;
}
SHIM(RegisterWindowMessageA) { RET(register_message((const char *)A1)); }
SHIM(RegisterWindowMessageW) { char *n = utf8_from_w(A1); u32 r = register_message(n); free(n); RET(r); }
SHIM(RegisterClipboardFormatW) { char *n = utf8_from_w(A1); u32 r = register_message(n) + 0x100; free(n); RET(r); }
SHIM(RegisterClipboardFormatA) { RET(register_message((const char *)A1) + 0x100); }
SHIM(SystemParametersInfoW) {  // (action, uiParam, pv, winini)
    u32 a = (u32)A1; u64 pv = A3;
    switch (a) {
    case 0x0030: { int w, h, hz; extern void rt_display_mode(int *, int *, int *); rt_display_mode(&w, &h, &hz);   // SPI_GETWORKAREA
                   ST32(pv, 0); ST32(pv + 4, 0); ST32(pv + 8, (u32)w); ST32(pv + 12, (u32)h); RET(1); }
    case 0x003A: case 0x0034: case 0x0032:   // SPI_GETSTICKYKEYS / TOGGLEKEYS / FILTERKEYS: { cbSize, dwFlags, ... }
        if (pv) { u32 cb = LD32(pv); if (cb >= 8 && cb <= 64) memset((void *)(pv + 4), 0, cb - 4); } RET(1);
    case 0x003B: case 0x0035: case 0x0033: RET(1);   // the matching SET* calls: accepted, nothing to change
    case 0x0003: if (pv) { ST32(pv, 6); ST32(pv + 4, 10); ST32(pv + 8, 1); } RET(1);   // SPI_GETMOUSE
    case 0x0070: if (pv) ST32(pv, 10); RET(1);       // SPI_GETMOUSESPEED
    case 0x0068: if (pv) ST32(pv, 3); RET(1);        // SPI_GETWHEELSCROLLLINES
    case 0x2006: if (pv) ST32(pv, 0); RET(1);        // SPI_GETMOUSEVANISH etc.
    default: if (getenv("WINTOMAC_VERBOSE")) rt_log("SystemParametersInfo(0x%x) unsupported", a); RET(0);
    }
}
SHIM(SystemParametersInfoA) { sh_SystemParametersInfoW(c); }
SHIM(SetRectEmpty) { memset((void *)A1, 0, 16); RET(1); }
SHIM(PtInRect) { i32 x = (i32)(u32)A2, y = (i32)(u32)(A2 >> 32); RET(x >= (i32)LD32(A1) && x < (i32)LD32(A1 + 8) && y >= (i32)LD32(A1 + 4) && y < (i32)LD32(A1 + 12)); }   // POINT by value
SHIM(IntersectRect) {
    i32 l = (i32)LD32(A2) > (i32)LD32(A3) ? (i32)LD32(A2) : (i32)LD32(A3), t = (i32)LD32(A2 + 4) > (i32)LD32(A3 + 4) ? (i32)LD32(A2 + 4) : (i32)LD32(A3 + 4);
    i32 r = (i32)LD32(A2 + 8) < (i32)LD32(A3 + 8) ? (i32)LD32(A2 + 8) : (i32)LD32(A3 + 8), b = (i32)LD32(A2 + 12) < (i32)LD32(A3 + 12) ? (i32)LD32(A2 + 12) : (i32)LD32(A3 + 12);
    if (l >= r || t >= b) { memset((void *)A1, 0, 16); RET(0); }
    ST32(A1, (u32)l); ST32(A1 + 4, (u32)t); ST32(A1 + 8, (u32)r); ST32(A1 + 12, (u32)b); RET(1);
}
SHIM(TrackMouseEvent) { RET(1); }
SHIM(FlashWindowEx) { RET(1); }
SHIM(FlashWindow) { RET(1); }
SHIM(EnableWindow) { RET(0); }
SHIM(IsWindowEnabled) { RET(1); }
SHIM(SetLayeredWindowAttributes) { RET(1); }
SHIM(SetWindowRgn) { RET(1); }
SHIM(SetClassLongPtrA) { RET(0); }
SHIM(SetClassLongPtrW) { RET(0); }
SHIM(GetUpdateRect) { if (A2) memset((void *)A2, 0, 16); RET(0); }
SHIM(FillRect) { RET(1); }
SHIM(DrawTextW) { RET(0); }
SHIM(AttachThreadInput) { RET(1); }
// Hooks are accepted but never invoked (no global input hooks on macOS).
SHIM(SetWindowsHookExW) { static u64 next = 0x6000; RET(next++); }
SHIM(SetWindowsHookExA) { sh_SetWindowsHookExW(c); }
SHIM(UnhookWindowsHookEx) { RET(1); }
SHIM(CallNextHookEx) { RET(0); }
SHIM(RegisterDeviceNotificationW) { RET(0x6100); }
SHIM(RegisterDeviceNotificationA) { RET(0x6100); }
SHIM(UnregisterDeviceNotification) { RET(1); }
// Menus: games rarely show them; handles are opaque and operations succeed.
static u64 menu_next = 0x6200;
SHIM(CreatePopupMenu) { RET(menu_next++); }
SHIM(CreateMenu) { RET(menu_next++); }
SHIM(DestroyMenu) { RET(1); }
SHIM(DeleteMenu) { RET(1); }
SHIM(InsertMenuW) { RET(1); }
SHIM(AppendMenuW) { RET(1); }
SHIM(CheckMenuItem) { RET(0); }
SHIM(EnableMenuItem) { RET(0); }
SHIM(GetMenu) { RET(0); }
SHIM(SetMenu) { RET(1); }
SHIM(GetSystemMenu) { RET(0); }
SHIM(GetMenuItemInfoW) { RET(0); }
SHIM(SetMenuItemInfoW) { RET(1); }
SHIM(TrackPopupMenu) { RET(0); }
// Clipboard: empty for now (SDL falls back cleanly).
SHIM(OpenClipboard) { RET(1); }
SHIM(CloseClipboard) { RET(1); }
SHIM(EmptyClipboard) { RET(1); }
SHIM(GetClipboardData) { RET(0); }
SHIM(SetClipboardData) { RET(0); }
SHIM(IsClipboardFormatAvailable) { RET(0); }
SHIM(EnumClipboardFormats) { RET(0); }
SHIM(GetClipboardFormatNameA) { RET(0); }
SHIM(GetClipboardFormatNameW) { RET(0); }
SHIM(GetClipboardSequenceNumber) { RET(0); }
// Dialogs: none (SDL_ShowMessageBox reports failure and the game continues).
SHIM(DialogBoxIndirectParamW) { RET((u64)-1); }
SHIM(EndDialog) { RET(1); }
SHIM(GetDlgItem) { RET(0); }
static u64 icon_next = 0x6300;
SHIM(CreateIconFromResource) { RET(icon_next++); }
SHIM(CreateIconFromResourceEx) { RET(icon_next++); }

const Shim shims_user32[] = {
#define E2(n) { NULL, #n, sh_##n },
    E2(MapVirtualKeyW) E2(MapVirtualKeyA) E2(MapVirtualKeyExW) E2(ToUnicode) E2(ToUnicodeEx) E2(ToAscii) E2(VkKeyScanA) E2(VkKeyScanW)
    E2(GetKeyboardLayout) E2(GetKeyboardLayoutList) E2(GetKeyNameTextW) E2(GetClassInfoExW) E2(GetClassInfoExA) E2(GetClassInfoW)
    E2(SetPropW) E2(SetPropA) E2(GetPropW) E2(GetPropA) E2(RemovePropW) E2(RemovePropA) E2(GetWindowTextLengthW) E2(GetWindowTextLengthA)
    E2(GetWindowTextW) E2(GetWindowTextA) E2(GetWindowThreadProcessId) E2(EnumWindows) E2(IsZoomed) E2(GetWindowPlacement)
    E2(SetWindowPlacement) E2(SetParent) E2(GetParent) E2(GetMessagePos) E2(GetMessageTime) E2(GetMessageExtraInfo)
    E2(PostThreadMessageW) E2(PostThreadMessageA) E2(GetDoubleClickTime) E2(RegisterWindowMessageA) E2(RegisterWindowMessageW)
    E2(RegisterClipboardFormatW) E2(RegisterClipboardFormatA) E2(SystemParametersInfoW) E2(SystemParametersInfoA) E2(SetRectEmpty)
    E2(PtInRect) E2(IntersectRect) E2(TrackMouseEvent) E2(FlashWindowEx) E2(FlashWindow) E2(EnableWindow) E2(IsWindowEnabled)
    E2(SetLayeredWindowAttributes) E2(SetWindowRgn) E2(SetClassLongPtrA) E2(SetClassLongPtrW) E2(GetUpdateRect) E2(FillRect)
    E2(DrawTextW) E2(AttachThreadInput) E2(SetWindowsHookExW) E2(SetWindowsHookExA) E2(UnhookWindowsHookEx) E2(CallNextHookEx)
    E2(RegisterDeviceNotificationW) E2(RegisterDeviceNotificationA) E2(UnregisterDeviceNotification) E2(CreatePopupMenu)
    E2(CreateMenu) E2(DestroyMenu) E2(DeleteMenu) E2(InsertMenuW) E2(AppendMenuW) E2(CheckMenuItem) E2(EnableMenuItem) E2(GetMenu)
    E2(SetMenu) E2(GetSystemMenu) E2(GetMenuItemInfoW) E2(SetMenuItemInfoW) E2(TrackPopupMenu) E2(OpenClipboard) E2(CloseClipboard)
    E2(EmptyClipboard) E2(GetClipboardData) E2(SetClipboardData) E2(IsClipboardFormatAvailable) E2(EnumClipboardFormats)
    E2(GetClipboardFormatNameA) E2(GetClipboardFormatNameW) E2(GetClipboardSequenceNumber) E2(DialogBoxIndirectParamW)
    E2(EndDialog) E2(GetDlgItem) E2(CreateIconFromResource) E2(CreateIconFromResourceEx)
#undef E2
#define E(n) { NULL, #n, sh_##n }
    E(RegisterClassExW), E(RegisterClassExA), E(RegisterClassW), E(RegisterClassA), E(UnregisterClassW), E(UnregisterClassA),
    E(CreateWindowExW), E(CreateWindowExA), E(DestroyWindow), E(ShowWindow), E(UpdateWindow), E(SetWindowTextW), E(SetWindowTextA),
    E(GetClientRect), E(GetWindowRect), E(AdjustWindowRect), E(AdjustWindowRectEx), E(AdjustWindowRectExForDpi), E(SetWindowPos),
    E(MoveWindow), E(IsWindowVisible), E(IsIconic), E(IsWindow), E(SetWindowLongPtrW), E(GetWindowLongPtrW), E(SetWindowLongPtrA),
    E(GetWindowLongPtrA), E(SetWindowLongW), E(GetWindowLongW), E(SetWindowLongA), E(GetWindowLongA),
    E(PeekMessageW), E(PeekMessageA), E(GetMessageW), E(GetMessageA), E(WaitMessage), E(TranslateMessage), E(DispatchMessageW),
    E(DispatchMessageA), E(PostQuitMessage), E(PostMessageW), E(PostMessageA), E(SendMessageW), E(SendMessageA),
    E(CallWindowProcW), E(CallWindowProcA), E(DefWindowProcW), E(DefWindowProcA), E(GetQueueStatus), E(MsgWaitForMultipleObjects),
    E(GetKeyState), E(GetAsyncKeyState), E(GetKeyboardState), E(GetCursorPos), E(SetCursorPos), E(ScreenToClient), E(ClientToScreen),
    E(ShowCursor), E(SetCursor), E(LoadCursorW), E(LoadCursorA), E(LoadIconW), E(LoadIconA), E(LoadImageW), E(LoadImageA),
    E(ClipCursor), E(SetCapture), E(ReleaseCapture), E(GetCapture), E(SetFocus), E(GetFocus), E(GetActiveWindow),
    E(GetForegroundWindow), E(SetForegroundWindow), E(SetActiveWindow), E(GetDesktopWindow), E(GetSystemMetrics),
    E(MonitorFromWindow), E(MonitorFromRect), E(MonitorFromPoint), E(GetMonitorInfoW), E(GetMonitorInfoA), E(MessageBoxW), E(MessageBoxA),
    E(SetProcessDPIAware), E(SetProcessDpiAwarenessContext), E(GetDpiForWindow), E(GetDC), E(ReleaseDC), E(BeginPaint),
    E(EndPaint), E(SetRect), E(OffsetRect), E(ValidateRect), E(InvalidateRect), E(GetStockObject), E(SetTimer), E(KillTimer),
    E(RegisterRawInputDevices), E(GetRegisteredRawInputDevices), E(GetRawInputData), E(GetRawInputBuffer),
    E(GetRawInputDeviceList), E(GetRawInputDeviceInfoA), E(GetRawInputDeviceInfoW), E(DefRawInputProc),
    E(MsgWaitForMultipleObjectsEx), E(GetClipCursor),
#undef E
    { 0, 0, 0 }
};
