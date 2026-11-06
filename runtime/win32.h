// Helpers for writing Win32 API shims against the guest context (Win64 calling convention).
#pragma once
#include "recomp.h"

// At shim entry the guest return address is at [rsp]; args 1-4 in rcx/rdx/r8/r9,
// arg 5+ at [rsp + 8*n] (past the 32-byte home space).
#define A1 (c->r[RCX])
#define A2 (c->r[RDX])
#define A3 (c->r[R8])
#define A4 (c->r[R9])
#define AN(n) LD64(c->r[RSP] + 8 * (n))
#define RET(v) do { c->r[RAX] = (u64)(v); return; } while (0)
#define XMM_ARG(i) (c->x[(i) - 1])

typedef void (*ShimFn)(Ctx *);
typedef struct { const char *dll; const char *name; ShimFn fn; } Shim;

#define SHIM(name) static void sh_##name(Ctx *c)

// Each shim module exposes a NULL-terminated table.
extern const Shim shims_kernel32[];
extern const Shim shims_msvcrt[];
extern const Shim shims_threads[];
extern const Shim shims_vulkan[];
extern const Shim shims_vulkan_gen[];
extern const Shim shims_user32[];
extern const Shim shims_files[];
extern const Shim shims_misc[];
extern const Shim shims_d3dcompiler[];
extern const Shim shims_d3dreflect[];
extern const Shim shims_d3d12[], shims_d3d12_gen[], shims_d3d12_ext[], shims_d3d12_pso[], shims_dxgi12[];   // d3d12*.c (libvkd3d) + gen/d3d12_thunks.h
extern const Shim shims_seh[];
extern const Shim shims_com[];
extern const Shim shims_xaudio2[];
extern const Shim shims_xinput[];

const char *rt_exe_dir(void);                         // host directory of the program
const char *rt_user_data_dir(void);                   // C:\users\... + HKCU store: ~/Library/Application Support/wintomac[/<WINTOMAC_APPNAME>]
void rt_host_path(const char *win, char *out, size_t n); // Windows path -> host path
void rt_files_init(const char *exe_dir);
void rt_set_last_error(u32 e);

// Guest context of the thread currently inside a shim (set on every import call).
extern __thread Ctx *rt_cur_ctx;
Ctx *rt_foreign_ctx(void);   // guest context for the current host thread (created on first use)
int rt_signal_event(u64 handle);   // SetEvent from host threads
extern const Shim shims_dsound[];
extern const Shim shims_wasapi[];   // wasapi.c: mmdevapi / IAudioClient (Core Audio)
extern const Shim shims_winsock[];
const char *rt_winsock_ordinal(u16 ord);
// user32 <-> vulkan: CAMetalLayer* backing a guest HWND (created on first use).
void *user32_metal_layer(u64 hwnd);
u64 user32_hwnd_from_dc(u64 hdc);
int user32_pixel_format(u64 hwnd, int set);
void *user32_gl_window(u64 hwnd, int *lw, int *lh);

u64 rt_thunk_for(const char *dll, const char *name);  // 0 if no shim

// Recompiled modules (the exe and any DLLs recompiled with it).
u64 rt_module_handle(const char *name);                 // NULL name = the program; 0 if not one of ours
u64 rt_module_load(const char *name);                   // LoadLibrary: runs DllMain on first load; 0 if not ours
u64 rt_module_export(u64 base, const char *name, u16 ordinal);
const char *rt_module_path(u64 base);
u64 rt_module_base_for(u64 addr, u64 *size);            // image containing addr, or 0

// Handles: small table mapping guest HANDLE values to host objects.
enum { H_NONE, H_FILE, H_WAIT, H_WINDOW, H_GLRC };
u64 rt_handle_new(int kind, intptr_t host);
int rt_handle_get(u64 h, int kind, intptr_t *host);
void rt_handle_close(u64 h);

// Guest heap (identity mapped => host malloc works directly).
u64 rt_trace_enabled(void);
void rt_guest_stack(Ctx *c);   // diagnostics: print guest return addresses
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Calling back into guest code from a shim (WndProc, thread start, qsort, atexit...).
u64 rt_guest_call(Ctx *c, u64 fn, int nargs, const u64 *args);

extern u64 rt_teb, rt_peb;
extern int rt_argc;
extern char **rt_argv;

// API coverage additions (tools/api_coverage.py): registry/crypto, kernel32 part 2, CRT part 2, user32/shell/etc., vcruntime.
extern const Shim shims_advapi32[], shims_kernel32x[], shims_crt2[], shims_win32x[];
extern const Shim shims_steam[];   // steam.c: steam_api64.dll -> macOS libsteam_api.dylib (docs/steam.md)
void rt_bind_data_imports(u64 base);   // msvcrt data exports (_acmdln, _fmode, ...): point IAT slots at the variables
