// OpenGL for Windows games: WGL (opengl32.dll) and the GDI pixel-format/SwapBuffers calls, on SDL3's
// OpenGL contexts over Apple's OpenGL.framework (GL 2.1 legacy or 4.1 core, running on Metal).
//
// The ~3000 gl* entry points are generated (tools/gen_opengl.py -> runtime/gen/gl_thunks.c) and call the
// framework directly; this file owns contexts, pixel formats, presentation and wglGetProcAddress.
//
// Context versions: macOS has exactly two. A legacy wglCreateContext, or anything asking for < 3.0 or a
// compatibility profile below 3.2, gets 2.1 legacy; 3.x/4.x requests get 4.1 core (the highest macOS
// offers). A request above 4.1 fails the way a Windows driver would (ERROR_INVALID_VERSION_ARB), so games
// fall back. A compatibility profile >= 3.0 is answered with core 4.1 and logged: most such games only
// use core features, and macOS has no 3.x compatibility contexts at all.
//
// Resolution: the drawable is sized to the game's logical client size (CGL surface backing size), so
// virtual display modes (e.g. 1024x768 shown fullscreen on a 1470x956 screen) render at the size the
// game set its viewport for and macOS scales the result to the window.
#define GL_SILENCE_DEPRECATION 1
#include "gl_bridge.h"
#include <SDL3/SDL.h>
#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *gl_lib;
void (*gl_host_proc(const char *name))(void) {
    if (!gl_lib) gl_lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
    return gl_lib ? (void (*)(void))dlsym(gl_lib, name) : NULL;
}
void gl_missing(const char *name) {
    static int n;
    if (n++ < 20) rt_log("opengl: macOS has no %s", name);
}
static int gl_trace(void) { static int t = -1; if (t < 0) t = getenv("WINTOMAC_GL_TRACE") != NULL; return t; }

// ---- pixel formats ----------------------------------------------------------------------
// Eight formats, all RGBA8 + depth 24 + stencil 8, double buffered: 1-4 = 0/2/4/8x MSAA, 5-8 = same + sRGB.
enum { NFORMATS = 8 };
static int fmt_samples(int f) { static const int s[4] = { 0, 2, 4, 8 }; return f >= 1 && f <= NFORMATS ? s[(f - 1) & 3] : 0; }
static int fmt_srgb(int f) { return f > 4; }
static int fmt_pick(int samples, int srgb) {
    int i = samples <= 0 ? 0 : samples <= 2 ? 1 : samples <= 4 ? 2 : 3;
    return 1 + i + (srgb ? 4 : 0);
}
static void describe(int f, u64 pfd) {  // PIXELFORMATDESCRIPTOR (40 bytes)
    memset((void *)pfd, 0, 40);
    ST16(pfd, 40); ST16(pfd + 2, 1);
    ST32(pfd + 4, 0x4 | 0x20 | 0x1);  // PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER
    u8 *b = (u8 *)pfd;
    b[8] = 0; b[9] = 32;               // PFD_TYPE_RGBA, cColorBits
    b[10] = 8; b[11] = 16; b[12] = 8; b[13] = 8; b[14] = 8; b[15] = 0; b[16] = 8; b[17] = 24;
    b[23] = 24; b[24] = 8;             // depth, stencil
    (void)f;
}

static u64 hwnd_of(u64 hdc) {
    u64 h = user32_hwnd_from_dc(hdc);
    if (!h && gl_trace()) rt_log("opengl: unknown HDC %llx", (unsigned long long)hdc);
    return h;
}

SHIM(ChoosePixelFormat) { RET(hwnd_of(A1) || A1 ? 1 : 0); }
SHIM(DescribePixelFormat) {  // (hdc, format, nBytes, ppfd) -> number of formats
    if (A4 && (u32)A3 >= 40) describe((int)A2, A4);
    RET(NFORMATS);
}
SHIM(SetPixelFormat) {  // (hdc, format, ppfd)
    u64 h = hwnd_of(A1);
    int f = (int)A2 >= 1 && (int)A2 <= NFORMATS ? (int)A2 : 1;
    if (!h) RET(1);
    if (user32_pixel_format(h, 0)) RET(user32_pixel_format(h, 0) == f);  // Windows: a window's format is set once
    user32_pixel_format(h, f);
    user32_gl_window(h, NULL, NULL);   // switch the native window to OpenGL now, on the creating (main) thread
    if (gl_trace()) rt_log("opengl: SetPixelFormat hwnd=%llx format %d (%dx MSAA%s)", (unsigned long long)h, f, fmt_samples(f), fmt_srgb(f) ? ", sRGB" : "");
    RET(1);
}
SHIM(GetPixelFormat) { u64 h = hwnd_of(A1); RET(h ? user32_pixel_format(h, 0) : 0); }

// ---- contexts ---------------------------------------------------------------------------
typedef struct {
    SDL_GLContext sdl;
    u64 hwnd;            // window it was created on
    int bw, bh;          // CGL backing size last applied
    int core;            // 4.1 core vs 2.1 legacy
} GLRC;

static _Thread_local u64 cur_rc, cur_dc;

static GLRC *rc_get(u64 h) { intptr_t p; return rt_handle_get(h, H_GLRC, &p) ? (GLRC *)p : NULL; }

// Render at the game's logical size: if it differs from the window's point size, give the drawable a fixed
// backing size and let macOS scale it. Call with the context current.
static void apply_backing(GLRC *rc, SDL_Window *win, int lw, int lh) {
    int pw, ph; SDL_GetWindowSize(win, &pw, &ph);
    int want = lw > 0 && lh > 0 && (lw != pw || lh != ph);
    int bw = want ? lw : 0, bh = want ? lh : 0;
    if (bw == rc->bw && bh == rc->bh) return;
    CGLContextObj cgl = CGLGetCurrentContext();
    if (!cgl) return;
    if (want) { GLint sz[2] = { lw, lh }; CGLSetParameter(cgl, kCGLCPSurfaceBackingSize, sz); CGLEnable(cgl, kCGLCESurfaceBackingSize); }
    else CGLDisable(cgl, kCGLCESurfaceBackingSize);
    rc->bw = bw; rc->bh = bh;
    if (gl_trace()) rt_log("opengl: backing size %dx%d (window %dx%d)", lw, lh, pw, ph);
}

static int make_current(u64 hdc, u64 hrc) {
    if (!hrc) {
        SDL_GL_MakeCurrent(NULL, NULL);
        cur_rc = cur_dc = 0;
        return 1;
    }
    GLRC *rc = rc_get(hrc);
    if (!rc) { rt_set_last_error(2000); return 0; }  // ERROR_INVALID_PIXEL_FORMAT-ish: invalid context
    u64 h = hdc ? hwnd_of(hdc) : 0;
    if (!h) h = rc->hwnd;
    int lw = 0, lh = 0;
    SDL_Window *win = user32_gl_window(h, &lw, &lh);
    if (!win) win = user32_gl_window(h = rc->hwnd, &lw, &lh);
    if (!win || !SDL_GL_MakeCurrent(win, rc->sdl)) { rt_log("opengl: wglMakeCurrent failed: %s", SDL_GetError()); return 0; }
    apply_backing(rc, win, lw, lh);
    cur_rc = hrc; cur_dc = hdc;
    return 1;
}

static u64 create_context(u64 hdc, u64 share, int major, int minor, int profile, int flags) {
    u64 h = hwnd_of(hdc);
    if (!h) { rt_set_last_error(6); return 0; }  // ERROR_INVALID_HANDLE
    int lw, lh;
    SDL_Window *win = user32_gl_window(h, &lw, &lh);
    if (!win) return 0;
    int f = user32_pixel_format(h, 0); if (!f) f = 1;
    int core = major >= 3;
    if (major > 4 || (major == 4 && minor > 1)) {
        rt_log("opengl: game asked for OpenGL %d.%d; macOS tops out at 4.1", major, minor);
        rt_set_last_error(0x2095);  // ERROR_INVALID_VERSION_ARB
        return 0;
    }
    if (core && profile == 2) rt_log("opengl: %d.%d compatibility profile requested; using 4.1 core (macOS has no 3.x+ compatibility contexts)", major, minor);
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, fmt_samples(f) ? 1 : 0);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, fmt_samples(f));
    SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, fmt_srgb(f));
    if (core) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    }
    (void)flags;
    // SDL creates the new context current; remember what was current on this thread and restore it after.
    SDL_Window *prev_win = SDL_GL_GetCurrentWindow(); SDL_GLContext prev = SDL_GL_GetCurrentContext();
    GLRC *srd = share ? rc_get(share) : NULL;
    if (srd) { SDL_GL_MakeCurrent(win, srd->sdl); SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1); }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    SDL_GL_MakeCurrent(prev_win, prev);
    if (!ctx) { rt_log("opengl: context creation failed: %s", SDL_GetError()); rt_set_last_error(0x2096); return 0; }
    GLRC *rc = calloc(1, sizeof *rc);
    rc->sdl = ctx; rc->hwnd = h; rc->core = core;
    u64 hrc = rt_handle_new(H_GLRC, (intptr_t)rc);
    if (gl_trace()) rt_log("opengl: context %llx: asked %d.%d%s, got %s", (unsigned long long)hrc, major, minor,
                                   profile == 1 ? " core" : profile == 2 ? " compat" : "", core ? "4.1 core" : "2.1 legacy");
    return hrc;
}

SHIM(wglCreateContext) { RET(create_context(A1, 0, 1, 0, 0, 0)); }
SHIM(wglCreateLayerContext) { RET((int)A2 == 0 ? create_context(A1, 0, 1, 0, 0, 0) : 0); }
SHIM(wglCreateContextAttribsARB) {  // (hdc, share, const int *attribs)
    int major = 1, minor = 0, profile = 0, flags = 0;
    for (const i32 *a = (const i32 *)A3; a && a[0]; a += 2)
        switch (a[0]) {
        case 0x2091: major = a[1]; break;     // WGL_CONTEXT_MAJOR_VERSION_ARB
        case 0x2092: minor = a[1]; break;     // WGL_CONTEXT_MINOR_VERSION_ARB
        case 0x2094: flags = a[1]; break;     // WGL_CONTEXT_FLAGS_ARB
        case 0x9126: profile = a[1]; break;   // WGL_CONTEXT_PROFILE_MASK_ARB: 1 core, 2 compatibility
        }
    if (major >= 3 && profile == 0) profile = (major == 3 && minor < 2) ? 0 : 1;  // 3.2+ default: core
    RET(create_context(A1, A2, major, minor, profile, flags));
}
SHIM(wglDeleteContext) {
    GLRC *rc = rc_get(A1);
    if (!rc) RET(0);
    if (cur_rc == A1) make_current(0, 0);
    SDL_GL_DestroyContext(rc->sdl);
    rt_handle_close(A1); free(rc);
    RET(1);
}
SHIM(wglMakeCurrent) { RET(make_current(A1, A2)); }
SHIM(wglMakeContextCurrentARB) { RET(make_current(A1, A3)); }  // (draw dc, read dc, rc)
SHIM(wglGetCurrentContext) { RET(cur_rc); }
SHIM(wglGetCurrentDC) { RET(cur_dc); }
SHIM(wglGetCurrentReadDCARB) { RET(cur_dc); }
SHIM(wglShareLists) {  // macOS can only share at creation time; fine when nothing was created in the second yet
    if (gl_trace()) rt_log("opengl: wglShareLists(%llx, %llx) ignored", (unsigned long long)A1, (unsigned long long)A2);
    RET(1);
}
SHIM(wglCopyContext) { RET(0); }

// ---- presentation ------------------------------------------------------------------------
static int swap(u64 hdc) {
    u64 h = hdc ? hwnd_of(hdc) : 0;
    GLRC *rc = rc_get(cur_rc);
    if (!h && rc) h = rc->hwnd;
    int lw, lh;
    SDL_Window *win = h ? user32_gl_window(h, &lw, &lh) : NULL;
    if (!win) return 0;
    if (rc) apply_backing(rc, win, lw, lh);   // follows resizes and display-mode changes
    return SDL_GL_SwapWindow(win);
}
SHIM(SwapBuffers) { RET(swap(A1)); }
SHIM(wglSwapBuffers) { RET(swap(A1)); }
SHIM(wglSwapLayerBuffers) { RET(swap(A1)); }
SHIM(wglSwapIntervalEXT) { RET(SDL_GL_SetSwapInterval((int)A1)); }
SHIM(wglGetSwapIntervalEXT) { int i = 0; SDL_GL_GetSwapInterval(&i); RET(i); }

// ---- WGL extensions -----------------------------------------------------------------------
static const char wgl_exts[] =
    "WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_ARB_pixel_format WGL_ARB_create_context "
    "WGL_ARB_create_context_profile WGL_ARB_multisample WGL_ARB_framebuffer_sRGB WGL_EXT_framebuffer_sRGB "
    "WGL_EXT_swap_control WGL_ARB_make_current_read";
SHIM(wglGetExtensionsStringARB) { RET((u64)wgl_exts); }
SHIM(wglGetExtensionsStringEXT) { RET((u64)wgl_exts); }

static int fmt_attrib(int f, int a) {
    switch (a) {
    case 0x2000: return NFORMATS;          // WGL_NUMBER_PIXEL_FORMATS_ARB
    case 0x2001: case 0x2010: case 0x2011: return 1;  // DRAW_TO_WINDOW, SUPPORT_OPENGL, DOUBLE_BUFFER
    case 0x2003: return 0x2027;            // ACCELERATION: WGL_FULL_ACCELERATION_ARB
    case 0x2007: return 0x2028;            // SWAP_METHOD: WGL_SWAP_EXCHANGE_ARB
    case 0x2013: return 0x202B;            // PIXEL_TYPE: WGL_TYPE_RGBA_ARB
    case 0x2014: return 32;                // COLOR_BITS
    case 0x2015: case 0x2017: case 0x2019: case 0x201B: return 8;  // RED/GREEN/BLUE/ALPHA_BITS
    case 0x2016: return 16; case 0x2018: return 8; case 0x201A: return 0; case 0x201C: return 24;  // shifts
    case 0x2022: return 24;                // DEPTH_BITS
    case 0x2023: return 8;                 // STENCIL_BITS
    case 0x2041: return fmt_samples(f) ? 1 : 0;  // SAMPLE_BUFFERS
    case 0x2042: return fmt_samples(f);    // SAMPLES
    case 0x20A9: return fmt_srgb(f);       // FRAMEBUFFER_SRGB_CAPABLE (ARB and EXT share the value)
    default: return 0;                     // stereo, aux buffers, accum, pbuffer, ...
    }
}
SHIM(wglGetPixelFormatAttribivARB) {  // (hdc, format, layer, n, const int *attribs, int *values)
    const i32 *at = (const i32 *)AN(5); i32 *v = (i32 *)AN(6);
    for (u32 i = 0; i < (u32)A4; i++) v[i] = fmt_attrib((int)A2, at[i]);
    RET(1);
}
SHIM(wglGetPixelFormatAttribfvARB) {
    const i32 *at = (const i32 *)AN(5); float *v = (float *)AN(6);
    for (u32 i = 0; i < (u32)A4; i++) v[i] = (float)fmt_attrib((int)A2, at[i]);
    RET(1);
}
SHIM(wglChoosePixelFormatARB) {  // (hdc, const int *iattr, const float *fattr, max, int *formats, UINT *count)
    int samples = 0, srgb = 0, ok = 1;
    for (const i32 *a = (const i32 *)A2; a && a[0]; a += 2)
        switch (a[0]) {
        case 0x2042: samples = a[1]; break;
        case 0x2041: if (!a[1]) samples = 0; break;
        case 0x20A9: srgb = a[1]; break;
        case 0x2012: if (a[1]) ok = 0; break;            // stereo: no
        case 0x2020: case 0x2021: if (a[1]) ok = 0; break;  // accumulation buffers: no
        case 0x2014: if (a[1] > 32) ok = 0; break;       // > 32-bit colour (e.g. 10-bit): no
        case 0x2022: if (a[1] > 24) ok = 0; break;
        }
    u32 max = (u32)A4;
    if (!ok || !max) { if (AN(6)) ST32(AN(6), 0); RET(1); }
    if (AN(5)) ST32(AN(5), (u32)fmt_pick(samples, srgb));
    if (AN(6)) ST32(AN(6), 1);
    RET(1);
}

// ---- function lookup ------------------------------------------------------------------------
static const char *const wgl_names[] = {
    "wglCreateContextAttribsARB", "wglGetExtensionsStringARB", "wglGetExtensionsStringEXT", "wglChoosePixelFormatARB",
    "wglGetPixelFormatAttribivARB", "wglGetPixelFormatAttribfvARB", "wglSwapIntervalEXT", "wglGetSwapIntervalEXT",
    "wglMakeContextCurrentARB", "wglGetCurrentReadDCARB", NULL };
SHIM(wglGetProcAddress) {
    const char *name = (const char *)A1;
    u64 r = 0;
    if (!name) RET(0);
    for (int i = 0; wgl_names[i]; i++) if (!strcmp(wgl_names[i], name)) { r = rt_thunk_for(NULL, name); break; }
    if (!r && !strncmp(name, "gl", 2) && gl_host_proc(name)) r = rt_thunk_for(NULL, name);
    if (gl_trace() || (!r && getenv("WINTOMAC_VERBOSE"))) rt_log("wglGetProcAddress(%s) = %llx", name, (unsigned long long)r);
    RET(r);
}
SHIM(wglGetDefaultProcAddress) { sh_wglGetProcAddress(c); }

// Fonts and layers: rarely used (old GL demos); report failure so callers fall back.
SHIM(wglUseFontBitmapsA) { RET(0); }
SHIM(wglUseFontBitmapsW) { RET(0); }
SHIM(wglUseFontOutlinesA) { RET(0); }
SHIM(wglUseFontOutlinesW) { RET(0); }
SHIM(wglDescribeLayerPlane) { RET(0); }
SHIM(wglRealizeLayerPalette) { RET(0); }
SHIM(wglSetLayerPaletteEntries) { RET(0); }
SHIM(wglGetLayerPaletteEntries) { RET(0); }
SHIM(wglChoosePixelFormat) { sh_ChoosePixelFormat(c); }
SHIM(wglSetPixelFormat) { sh_SetPixelFormat(c); }
SHIM(wglGetPixelFormat) { sh_GetPixelFormat(c); }
SHIM(wglDescribePixelFormat) { sh_DescribePixelFormat(c); }

const Shim shims_opengl[] = {
#define E(n) { NULL, #n, sh_##n },
    E(ChoosePixelFormat) E(DescribePixelFormat) E(SetPixelFormat) E(GetPixelFormat) E(SwapBuffers)
    E(wglCreateContext) E(wglCreateLayerContext) E(wglCreateContextAttribsARB) E(wglDeleteContext) E(wglMakeCurrent)
    E(wglMakeContextCurrentARB) E(wglGetCurrentContext) E(wglGetCurrentDC) E(wglGetCurrentReadDCARB) E(wglShareLists)
    E(wglCopyContext) E(wglSwapBuffers) E(wglSwapLayerBuffers) E(wglSwapIntervalEXT) E(wglGetSwapIntervalEXT)
    E(wglGetExtensionsStringARB) E(wglGetExtensionsStringEXT) E(wglGetPixelFormatAttribivARB) E(wglGetPixelFormatAttribfvARB)
    E(wglChoosePixelFormatARB) E(wglGetProcAddress) E(wglGetDefaultProcAddress) E(wglUseFontBitmapsA) E(wglUseFontBitmapsW)
    E(wglUseFontOutlinesA) E(wglUseFontOutlinesW) E(wglDescribeLayerPlane) E(wglRealizeLayerPalette)
    E(wglSetLayerPaletteEntries) E(wglGetLayerPaletteEntries) E(wglChoosePixelFormat) E(wglSetPixelFormat)
    E(wglGetPixelFormat) E(wglDescribePixelFormat)
#undef E
    { 0, 0, 0 }
};
