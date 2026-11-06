// Steamworks: the game's steam_api64.dll is replaced by a bridge to the macOS Steamworks runtime
// (libsteam_api.dylib, which talks to the user's running Steam client). See docs/steam.md.
//
// Nothing here decides ownership, licences or login: every answer comes from the real libsteam_api.dylib and
// Steam. If no libsteam_api.dylib can be found, SteamAPI_Init fails exactly as when Steam isn't running.
//
//  - Plain exports (SteamAPI_Init, RunCallbacks, ...) are forwarded.
//  - Interface objects (ISteamUser*, ...) come back from the host for the version string the game asked for, so
//    the host object has the same interface version as the guest expects. The guest gets a wrapper whose vtable
//    is thunks; each slot is translated from the Win64/MSVC convention to the arm64/Itanium one with facts from
//    runtime/gen/steam_ifaces.h (tools/gen_steam.py): MSVC slot order (overloads reversed), float args,
//    class returns via hidden pointer, Apple stack packing, and struct conversions (callback structs are packed
//    to 8 on Windows but to 4 on macOS).
//  - Callbacks: the guest's CCallbackBase objects are registered through host proxies whose Run() converts the
//    payload to the Windows layout and calls the guest object's vtable (MSVC order: Run(p,bIOFailure,hCall),
//    Run(p), GetCallbackSizeBytes()).
//  - The flat API (SteamAPI_ISteamUser_GetSteamID, ...) dispatches on the wrapper's own interface version.
//
// Env: WINTOMAC_STEAM_API=<path to libsteam_api.dylib> (also used to select tests/mock/mock_steam_api.cpp),
//      WINTOMAC_STEAM_TRACE=1 logs every bridged call.
#include <dlfcn.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/dyld.h>

#include "win32.h"

#include "steam_abi.h"

#define STEAM_MAX_SLOTS 200

// ---- logging -----------------------------------------------------------------------------------------------
static int trace_on = -1;
static int steam_trace(void) {
    if (trace_on < 0) { const char *e = getenv("WINTOMAC_STEAM_TRACE"); trace_on = e && *e && *e != '0'; }
    return trace_on;
}
static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
static void log_once(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_once(const char *fmt, ...) {
    static char *seen[256]; static int nseen;
    char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    pthread_mutex_lock(&once_lock);
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], buf)) { pthread_mutex_unlock(&once_lock); return; }
    if (nseen < 256) seen[nseen++] = strdup(buf);
    pthread_mutex_unlock(&once_lock);
    rt_log("steam: %s", buf);
}

// ---- host library ------------------------------------------------------------------------------------------
static void *lib;
static pthread_once_t lib_once = PTHREAD_ONCE_INIT;
static char lib_path[1024];

static int try_open(const char *p) {
    if (!p || !*p) return 0;
    void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!h) { if (steam_trace()) rt_log("steam: dlopen %s: %s", p, dlerror()); return 0; }
    lib = h; snprintf(lib_path, sizeof lib_path, "%s", p);
    return 1;
}
static void lib_load(void) {
    const char *env = getenv("WINTOMAC_STEAM_API");
    if (env && *env) {
        if (!try_open(env)) rt_log("steam: WINTOMAC_STEAM_API=%s could not be loaded (%s); Steam API unavailable", env, dlerror());
        else if (steam_trace()) rt_log("steam: using %s", lib_path);
        return;
    }
    char p[1100], self[1024]; uint32_t n = sizeof self;
    const char *dirs[3] = { rt_exe_dir(), NULL, NULL };
    if (!_NSGetExecutablePath(self, &n)) { char *s = strrchr(self, '/'); if (s) { *s = 0; dirs[1] = self; } }
    for (int i = 0; i < 2 && !lib; i++) {
        if (!dirs[i]) continue;
        snprintf(p, sizeof p, "%s/libsteam_api.dylib", dirs[i]); if (try_open(p)) break;
        snprintf(p, sizeof p, "%s/../Frameworks/libsteam_api.dylib", dirs[i]); if (try_open(p)) break;
    }
    if (lib) rt_log("steam: using %s", lib_path);
    else rt_log("steam: no libsteam_api.dylib next to the game (or set WINTOMAC_STEAM_API); Steam API unavailable, "
                "SteamAPI_Init will fail as if Steam were not running (see docs/steam.md)");
}
static int have_lib(void) { pthread_once(&lib_once, lib_load); return lib != NULL; }
static void *H(const char *name) { return have_lib() ? dlsym(lib, name) : NULL; }
#define HFN(var, type, name) static __typeof__(type) var; static int var##_done; \
    if (!var##_done) { var = (type)H(name); var##_done = 1; }

// ---- callback struct conversion -----------------------------------------------------------------------------
static void conv_m2w(const SteamConv *cv, u8 *w, const u8 *m) {
    if (!cv->nf) { memcpy(w, m, cv->win_size < cv->mac_size ? cv->win_size : cv->mac_size); return; }
    for (u32 i = 0; i < cv->nf; i++) { const SteamField *f = &steam_fields[cv->first + i]; memcpy(w + f->w, m + f->m, f->n); }
}
static void conv_w2m(const SteamConv *cv, u8 *m, const u8 *w) {
    if (!cv->nf) { memcpy(m, w, cv->win_size < cv->mac_size ? cv->win_size : cv->mac_size); return; }
    for (u32 i = 0; i < cv->nf; i++) { const SteamField *f = &steam_fields[cv->first + i]; memcpy(m + f->m, w + f->w, f->n); }
}
static const SteamCallbackInfo *cb_info(int id) {
    int lo = 0, hi = STEAM_NCALLBACKS - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (steam_callbacks[mid].id == id) return &steam_callbacks[mid];
                       if (steam_callbacks[mid].id < id) lo = mid + 1; else hi = mid - 1; }
    return NULL;
}
// Layout of callback `id`: the version whose Windows size is win_size (or mac size when by_mac), else the largest.
static const SteamConv *cb_conv(int id, int size, int by_mac) {
    const SteamCallbackInfo *ci = cb_info(id);
    if (!ci) return NULL;
    const SteamConv *best = NULL;
    for (u16 i = 0; i < ci->n; i++) {
        const SteamConv *cv = &steam_convs[steam_callback_convs[ci->first + i]];
        if ((by_mac ? cv->mac_size : cv->win_size) == size) return cv;
        if (!best || cv->win_size > best->win_size) best = cv;
    }
    if (size >= 0) log_once("callback %d: no known layout of size %d; using %s (%u bytes)", id, size, best->name, best->win_size);
    return best;
}
// SteamAPICallCompleted_t carries the size of the call result: translate it to the Windows layout's size.
static void fix_completed(int id, u8 *w) {
    if (id != 703) return;
    int rid; u32 cub; memcpy(&rid, w + 8, 4); memcpy(&cub, w + 12, 4);
    const SteamConv *cv = cb_conv(rid, (int)cub, 1);
    if (cv) { cub = cv->win_size; memcpy(w + 12, &cub, 4); }
}
// Host (mac layout) payload -> malloc'd Windows layout. *wsize gets the size.
static u8 *payload_to_win(int id, const void *m, int mac_size, int want_win, u32 *wsize) {
    const SteamConv *cv = want_win >= 0 ? cb_conv(id, want_win, 0) : cb_conv(id, mac_size, 1);
    if (!cv) { u8 *w = calloc(1, mac_size > 0 ? mac_size : 1); if (m && mac_size > 0) memcpy(w, m, mac_size);
               if (wsize) *wsize = mac_size; return w; }
    u8 *w = calloc(1, cv->win_size + 1);
    if (m) conv_m2w(cv, w, m);
    fix_completed(id, w);
    if (wsize) *wsize = cv->win_size;
    return w;
}

// ---- interface wrappers -------------------------------------------------------------------------------------
typedef struct Wrap { u64 vtbl; void *host; const SteamIface *ifc; struct Wrap *next; } Wrap;
static Wrap *wraps;
static pthread_mutex_t wrap_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 slot_thunks[STEAM_MAX_SLOTS];
static u64 *generic_vtbl;

static const SteamIface *find_iface(const char *ver) {
    if (!ver) return NULL;
    int lo = 0, hi = STEAM_NIFACES - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2, r = strcmp(steam_ifaces[mid].version, ver);
                       if (!r) return &steam_ifaces[mid]; if (r < 0) lo = mid + 1; else hi = mid - 1; }
    return NULL;
}
static Wrap *wrap_of(u64 p) {
    if (!p) return NULL;
    pthread_mutex_lock(&wrap_lock);
    Wrap *w = wraps; while (w && (u64)w != p) w = w->next;
    pthread_mutex_unlock(&wrap_lock);
    return w;
}
static u64 *vtbl_for(const SteamIface *ifc) {
    static u64 *tables[STEAM_NIFACES];
    int n = ifc ? ifc->n : STEAM_MAX_SLOTS;
    u64 **slot = ifc ? &tables[ifc - steam_ifaces] : &generic_vtbl;
    if (!*slot) {
        if (!slot_thunks[0])
            for (int i = 0; i < STEAM_MAX_SLOTS; i++) {
                char nm[64]; snprintf(nm, sizeof nm, "__wintomac_steam_slot_%d", i);
                slot_thunks[i] = rt_thunk_for(NULL, nm);
            }
        u64 *v = calloc(n + 1, 8);
        for (int i = 0; i < n && i < STEAM_MAX_SLOTS; i++) v[i] = slot_thunks[i];
        *slot = v;
    }
    return *slot;
}
static u64 wrap(void *host, const char *ver) {
    if (!host) return 0;
    const SteamIface *ifc = find_iface(ver);
    if (!ifc) log_once("interface %s is not in the generated table; slots are forwarded 1:1 with integer arguments", ver ? ver : "(null)");
    pthread_mutex_lock(&wrap_lock);
    for (Wrap *w = wraps; w; w = w->next) if (w->host == host && w->ifc == ifc) { pthread_mutex_unlock(&wrap_lock); return (u64)w; }
    pthread_mutex_unlock(&wrap_lock);
    Wrap *w = calloc(1, sizeof *w);
    w->vtbl = (u64)vtbl_for(ifc); w->host = host; w->ifc = ifc;
    pthread_mutex_lock(&wrap_lock); w->next = wraps; wraps = w; pthread_mutex_unlock(&wrap_lock);
    if (steam_trace()) rt_log("steam: %s -> host %p, guest %p", ver, host, (void *)w);
    return (u64)w;
}

// ---- warning hook (host -> guest function pointer) -----------------------------------------------------------
static u64 guest_warning_hook;
static void warning_tramp(int severity, const char *text) {
    if (!guest_warning_hook) return;
    u64 a[2] = { (u64)(i64)severity, (u64)text };
    rt_guest_call(rt_cur_ctx ? rt_cur_ctx : rt_foreign_ctx(), guest_warning_hook, 2, a);
}

// ---- the call engine: Win64 arguments -> arm64 (Apple) call ---------------------------------------------------
enum { M_MEMBER, M_FLAT, M_PLAIN };
typedef struct { u64 a, b; } R2;
typedef struct { u8 b[512]; } RBig;
#define HARGS u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double, \
              u64, u64, u64, u64, u64, u64, u64, u64
#define CALLARGS X[0], X[1], X[2], X[3], X[4], X[5], X[6], X[7], V[0], V[1], V[2], V[3], V[4], V[5], V[6], V[7], \
                 S[0], S[1], S[2], S[3], S[4], S[5], S[6], S[7]

static u64 win_arg(Ctx *c, int pos, int is_float) {
    static const int regs[4] = { RCX, RDX, R8, R9 };
    if (pos < 4) return is_float ? c->x[pos].q[0] : c->r[regs[pos]];
    return AN(pos + 1);
}
typedef struct { int ngrn, nsrn; u32 nsaa; u64 X[8]; double V[8]; u8 stk[64]; } HostArgs;
static void put_int(HostArgs *h, u64 v, int size) {
    if (h->ngrn < 8) { h->X[h->ngrn++] = v; return; }
    if (size < 1) size = 8;
    h->nsaa = (h->nsaa + size - 1) & ~(u32)(size - 1);
    if (h->nsaa + size <= sizeof h->stk) memcpy(h->stk + h->nsaa, &v, size);
    h->nsaa += size;
}
static void put_float(HostArgs *h, u64 bits, int size) {
    if (h->nsrn < 8) { memcpy(&h->V[h->nsrn++], &bits, 8); return; }
    h->nsaa = (h->nsaa + size - 1) & ~(u32)(size - 1);
    if (h->nsaa + size <= sizeof h->stk) memcpy(h->stk + h->nsaa, &bits, size);
    h->nsaa += size;
}
static void put_rec(HostArgs *h, const u8 *data, int size) {
    if (size > 16) { put_int(h, (u64)data, 8); return; }   // caller-owned copy, passed by reference
    u64 v[2] = { 0, 0 }; memcpy(v, data, size);
    int nreg = (size + 7) / 8;
    if (h->ngrn + nreg <= 8) { for (int i = 0; i < nreg; i++) h->X[h->ngrn++] = v[i]; return; }
    h->ngrn = 8;
    h->nsaa = (h->nsaa + 7) & ~7u;
    if (h->nsaa + 8 * nreg <= sizeof h->stk) memcpy(h->stk + h->nsaa, v, 8 * nreg);
    h->nsaa += 8 * nreg;
}

typedef struct { u8 *host; u64 guest; const SteamConv *cv; int out; } Fixup;

// Calls `fn` (host) with arguments taken from the guest context. `self` is the host `this` (M_MEMBER/M_FLAT).
static void invoke(Ctx *c, int mode, void *fn, void *self, const SteamArg *ret, const SteamArg *args, int nargs,
                   int pos, u64 sret, const char *what) {
    HostArgs h; memset(&h, 0, sizeof h);
    Fixup fx[8]; int nfx = 0;
    u8 recbuf[8][32];
    if (mode != M_PLAIN) h.X[h.ngrn++] = (u64)self;
    for (int i = 0; i < nargs; i++, pos++) {
        const SteamArg *a = &args[i];
        u64 v = win_arg(c, pos, a->kind == K_FLT);
        switch (a->kind) {
        case K_INT: {
            u64 x = v;
            if (a->size < 8) {
                u64 m = (1ull << (8 * a->size)) - 1; x &= m;
                if ((a->flags & 1) && (x >> (8 * a->size - 1))) x |= ~m;
            }
            put_int(&h, x, a->size); break;
        }
        case K_FLT: put_float(&h, a->size == 4 ? (v & 0xFFFFFFFFull) : v, a->size); break;
        case K_REC: {
            int wsz = a->flags ? a->flags : a->size;
            const u8 *src = (wsz == 1 || wsz == 2 || wsz == 4 || wsz == 8) ? (const u8 *)&v : (const u8 *)v;
            u8 *buf = i < 8 && a->size <= 32 ? recbuf[i] : malloc(a->size);
            memset(buf, 0, a->size); memcpy(buf, src, wsz < a->size ? wsz : a->size);
            put_rec(&h, buf, a->size); break;
        }
        case K_PREC: {
            if (!v) { put_int(&h, 0, 8); break; }
            const SteamConv *cv = &steam_convs[a->conv];
            u8 *m = calloc(1, cv->mac_size + 8);
            conv_w2m(cv, m, (const u8 *)v);
            if (nfx < 8) fx[nfx++] = (Fixup){ m, v, cv, !(a->flags & 1) };
            put_int(&h, (u64)m, 8); break;
        }
        case K_POLY:
            if (v) {
                log_once("%s: guest callback objects (ISteam*Response) are not bridged yet; call skipped", what);
                for (int k = 0; k < nfx; k++) free(fx[k].host);
                c->r[RAX] = 0; return;
            }
            put_int(&h, 0, 8); break;
        case K_FN:
            if (v) log_once("%s: guest function pointer argument not bridged; passing NULL", what);
            put_int(&h, 0, 8); break;
        case K_HOOK:
            guest_warning_hook = v;
            put_int(&h, v ? (u64)warning_tramp : 0, 8); break;
        default: put_int(&h, v, 8); break;
        }
    }
    u64 X[8], S[8]; double V[8];
    memcpy(X, h.X, sizeof X); memcpy(V, h.V, sizeof V); memcpy(S, h.stk, sizeof S);
    u64 rax = 0;
    switch (ret->kind) {
    case K_FLT: {
        double d = ((double (*)(HARGS))fn)(CALLARGS);
        u64 bits; memcpy(&bits, &d, 8);
        memset(&c->x[0], 0, 16);
        c->x[0].q[0] = ret->size == 4 ? (bits & 0xFFFFFFFFull) : bits;
        break;
    }
    case K_REC: {
        u8 out[512]; memset(out, 0, sizeof out);
        if (ret->size > 16) { RBig r = ((RBig (*)(HARGS))fn)(CALLARGS); memcpy(out, &r, ret->size); }
        else { R2 r = ((R2 (*)(HARGS))fn)(CALLARGS); memcpy(out, &r, 16); }
        int wsz = ret->flags ? ret->flags : ret->size;
        if (sret) { memcpy((void *)sret, out, wsz); rax = sret; }
        else memcpy(&rax, out, wsz < 8 ? wsz : 8);
        break;
    }
    default: {
        R2 r = ((R2 (*)(HARGS))fn)(CALLARGS);
        rax = r.a;
        if (ret->kind == K_INT && ret->size < 8) rax &= (1ull << (8 * ret->size)) - 1;
        if (ret->kind == K_IFACE) {
            // version string is argument `flags` (0-based, after this) of the original call
            const char *ver = NULL;
            int vpos = (mode == M_PLAIN ? 0 : 1) + ret->flags;
            ver = (const char *)win_arg(c, vpos, 0);
            rax = wrap((void *)r.a, ver);
        }
        break;
    }
    }
    for (int k = 0; k < nfx; k++) { if (fx[k].out) conv_m2w(fx[k].cv, (u8 *)fx[k].guest, fx[k].host); free(fx[k].host); }
    if (steam_trace()) rt_log("steam: %s = 0x%llx", what, (unsigned long long)rax);
    c->r[RAX] = rax;
}

// ISteamUtils::GetAPICallResult(hCall, pCallback, cubCallback, iCallbackExpected, pbFailed): the buffer is in the
// guest's (Windows) layout; fetch the host layout and convert.
static void get_api_call_result(Ctx *c, void *fn, void *self, int pos) {
    u64 call = win_arg(c, pos, 0), buf = win_arg(c, pos + 1, 0), failed = win_arg(c, pos + 4, 0);
    int cub = (int)win_arg(c, pos + 2, 0), id = (int)win_arg(c, pos + 3, 0);
    const SteamConv *cv = cb_conv(id, cub, 0);
    int msz = cv ? cv->mac_size : cub;
    u8 *m = calloc(1, msz + 8);
    u8 ok = ((u8 (*)(void *, u64, void *, int, int, void *))fn)(self, call, m, msz, id, (void *)failed);
    if (ok && buf) { if (cv) { memset((void *)buf, 0, cub); conv_m2w(cv, (u8 *)buf, m); fix_completed(id, (u8 *)buf); }
                     else memcpy((void *)buf, m, cub); }
    free(m);
    c->r[RAX] = ok;
}

static void call_method(Ctx *c, int mode, Wrap *w, const SteamMethod *m, int pos, const char *what) {
    void *fn = ((void **)*(void **)w->host)[m->host];
    if (!strcmp(m->name, "GetAPICallResult") && !strcmp(w->ifc->cls, "ISteamUtils")) { get_api_call_result(c, fn, w->host, pos); return; }
    u64 sret = 0;
    if (m->ret.kind == K_REC) {
        int wsz = m->ret.flags ? m->ret.flags : m->ret.size;
        // MSVC: member functions return every class via a hidden pointer (after this); flat (free) functions
        // only those that aren't 1/2/4/8 bytes, and then before the first argument.
        if (mode == M_MEMBER) sret = win_arg(c, pos++, 0);
        else if (!(wsz == 1 || wsz == 2 || wsz == 4 || wsz == 8)) sret = win_arg(c, 0, 0);
    }
    invoke(c, mode, fn, w->host, &m->ret, &steam_arg_pool[m->args], m->nargs, pos, sret, what);
}

// Guest vtable slot i of a wrapper.
static void steam_slot(Ctx *c, int i) {
    Wrap *w = (Wrap *)A1;
    if (!w->ifc) {   // unknown interface version: 1:1, integer args only
        void *fn = ((void **)*(void **)w->host)[i];
        R2 r = ((R2 (*)(void *, u64, u64, u64, u64, u64, u64, u64))fn)(w->host, A2, A3, A4, AN(5), AN(6), AN(7), AN(8));
        RET(r.a);
    }
    if (i >= w->ifc->n) { log_once("%s: slot %d out of range", w->ifc->version, i); RET(0); }
    const SteamMethod *m = &steam_methods[w->ifc->start + i];
    char what[160]; snprintf(what, sizeof what, "%s::%s", w->ifc->version, m->key);
    if (m->host == 0xFFFF) { log_once("%s: virtual destructor called; ignored", what); RET(0); }
    call_method(c, M_MEMBER, w, m, 1, what);
}
#define SLOT1(i) SHIM(steam_slot_##i) { steam_slot(c, i); }
#define SLOT10(i) SLOT1(i##0) SLOT1(i##1) SLOT1(i##2) SLOT1(i##3) SLOT1(i##4) SLOT1(i##5) SLOT1(i##6) SLOT1(i##7) SLOT1(i##8) SLOT1(i##9)
#define SLOTS(X) X() X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19)
SLOTS(SLOT10)   // steam_slot_0 .. steam_slot_199 (STEAM_MAX_SLOTS)

// ---- flat API -----------------------------------------------------------------------------------------------
static const SteamMethod *flat_find(const SteamFlatMethod *f, const SteamIface *ifc) {
    const SteamMethod *ms = &steam_methods[ifc->start], *byname = NULL; int nbyname = 0;
    for (int i = 0; i < ifc->n; i++) {
        if (!strcmp(ms[i].key, f->key)) return &ms[i];
        if (!strcmp(ms[i].name, f->method)) { byname = &ms[i]; nbyname++; }
    }
    return nbyname == 1 ? byname : NULL;
}
static void steam_flat_method(Ctx *c, int idx) {
    const SteamFlatMethod *f = &steam_flat_methods[idx];
    int pos = 1;
    Wrap *w = wrap_of(A1);
    if (!w && (w = wrap_of(A2))) pos = 2;   // struct returned through a hidden pointer before self
    if (!w) { if (A1) log_once("%s: self %llx is not a Steam interface from this bridge", f->name, (unsigned long long)A1); RET(0); }
    if (!w->ifc) { log_once("%s: interface version unknown; call skipped", f->name); RET(0); }
    const SteamMethod *m = flat_find(f, w->ifc);
    if (!m) { log_once("%s: no such method in %s", f->name, w->ifc->version); RET(0); }
    call_method(c, M_FLAT, w, m, pos, f->name);
}
static void steam_flat_plain_call(Ctx *c, int idx) {
    const SteamFlatPlain *f = &steam_flat_plain[idx];
    void *fn = H(f->name);
    if (!fn) { log_once("%s: not exported by %s", f->name, lib ? lib_path : "(no libsteam_api.dylib)"); memset(&c->x[0], 0, 16); RET(0); }
    // interface pointers among the arguments are unwrapped
    SteamArg args[24]; int n = f->nargs < 24 ? f->nargs : 24;
    memcpy(args, &steam_arg_pool[f->args], n * sizeof *args);
    int pos = 0; u64 sret = 0;
    if (f->ret.kind == K_REC) { int wsz = f->ret.flags ? f->ret.flags : f->ret.size;
                                if (!(wsz == 1 || wsz == 2 || wsz == 4 || wsz == 8)) sret = win_arg(c, pos++, 0); }
    static const int regs[4] = { RCX, RDX, R8, R9 };
    u64 saved[24]; int nsaved = 0;   // unwrap in place, restore afterwards (the guest owns these slots)
    for (int i = 0; i < n; i++) {
        if (args[i].kind != K_PTR) continue;
        int p = pos + i; u64 v = win_arg(c, p, 0); Wrap *w = wrap_of(v);
        if (!w || nsaved >= 24) continue;
        saved[nsaved++] = ((u64)p << 48) | (v & 0xFFFFFFFFFFFFull);
        if (p < 4) c->r[regs[p]] = (u64)w->host; else ST64(c->r[RSP] + 8ull * (p + 1), (u64)w->host);
    }
    invoke(c, M_PLAIN, fn, NULL, &f->ret, args, n, pos, sret, f->name);
    for (int k = 0; k < nsaved; k++) {
        int p = (int)(saved[k] >> 48); u64 v = saved[k] & 0xFFFFFFFFFFFFull;
        if (p >= 4) ST64(c->r[RSP] + 8ull * (p + 1), v);   // registers are volatile anyway
    }
}
static void steam_flat_accessor(Ctx *c, int idx);
#define FLATM(i, name) SHIM(name) { steam_flat_method(c, i); }
#define FLATP(i, name) SHIM(name) { steam_flat_plain_call(c, i); }
#define FLATA(i, name) SHIM(name) { steam_flat_accessor(c, i); }
STEAM_FLAT_METHOD_LIST(FLATM)
STEAM_FLAT_PLAIN_LIST(FLATP)
STEAM_FLAT_ACCESSOR_LIST(FLATA)

// ---- init / shutdown / plain exports ------------------------------------------------------------------------
static u64 init_counter = 1;   // SteamInternal_ContextInit re-runs accessors when this changes

SHIM(SteamAPI_Init) {
    HFN(f, u8 (*)(void), "SteamAPI_Init");
    u8 ok = f ? f() : 0;
    if (ok) init_counter++;
    if (!ok && lib) rt_log("steam: SteamAPI_Init failed (is Steam running, logged in, and does this account own the app? see docs/steam.md)");
    RET(ok);
}
SHIM(SteamAPI_InitSafe) {
    HFN(f, u8 (*)(void), "SteamAPI_InitSafe");
    HFN(g, u8 (*)(void), "SteamAPI_Init");
    u8 ok = f ? f() : g ? g() : 0;
    if (ok) init_counter++;
    RET(ok);
}
// ESteamAPIInitResult SteamInternal_SteamAPI_Init(const char *pszInternalCheckInterfaceVersions, SteamErrMsg *pOutErrMsg)
static u64 init_ex(const char *versions, char *err) {
    HFN(f, int (*)(const char *, char *), "SteamInternal_SteamAPI_Init");
    HFN(g, u8 (*)(void), "SteamAPI_Init");
    int r;
    if (f) r = f(versions, err);
    else if (g) { r = g() ? 0 : 1; if (r && err) snprintf(err, 1024, "SteamAPI_Init failed"); }
    else { r = 2; if (err) snprintf(err, 1024, "No libsteam_api.dylib found (wintomac); Steam is not available"); }
    if (!r) init_counter++;
    else rt_log("steam: SteamAPI_InitEx failed (%d): %s", r, err ? err : "");
    return (u64)(u32)r;
}
SHIM(SteamInternal_SteamAPI_Init) { RET(init_ex((const char *)A1, (char *)A2)); }
SHIM(SteamAPI_InitFlat) {
    HFN(f, int (*)(char *), "SteamAPI_InitFlat");
    if (f) { int r = f((char *)A1); if (!r) init_counter++; RET((u32)r); }
    RET(init_ex(NULL, (char *)A1));
}
SHIM(SteamAPI_Shutdown) { HFN(f, void (*)(void), "SteamAPI_Shutdown"); if (f) f(); init_counter++; RET(0); }
SHIM(SteamAPI_IsSteamRunning) { HFN(f, u8 (*)(void), "SteamAPI_IsSteamRunning"); RET(f ? f() : 0); }
SHIM(SteamAPI_RestartAppIfNecessary) {
    HFN(f, u8 (*)(u32), "SteamAPI_RestartAppIfNecessary");
    u8 r = f ? f((u32)A1) : 0;
    if (r) rt_log("steam: SteamAPI_RestartAppIfNecessary(%u) = true: Steam asked to relaunch the game through the "
                  "client; the game will exit. Put steam_appid.txt next to the game or set SteamAppId (docs/steam.md)", (u32)A1);
    RET(r);
}
SHIM(SteamAPI_GetHSteamUser) { HFN(f, int (*)(void), "SteamAPI_GetHSteamUser"); RET((u32)(f ? f() : 0)); }
SHIM(SteamAPI_GetHSteamPipe) { HFN(f, int (*)(void), "SteamAPI_GetHSteamPipe"); RET((u32)(f ? f() : 0)); }
SHIM(SteamAPI_ReleaseCurrentThreadMemory) { HFN(f, void (*)(void), "SteamAPI_ReleaseCurrentThreadMemory"); if (f) f(); RET(0); }
SHIM(SteamAPI_SetMiniDumpComment) { RET(0); }
SHIM(SteamAPI_WriteMiniDump) { RET(0); }              // takes Windows EXCEPTION_POINTERS; nothing to do on macOS
SHIM(SteamAPI_UseBreakpadCrashHandler) { RET(0); }
SHIM(SteamAPI_SetBreakpadAppID) { RET(0); }
SHIM(SteamAPI_SetTryCatchCallbacks) { HFN(f, void (*)(u8), "SteamAPI_SetTryCatchCallbacks"); if (f) f((u8)A1); RET(0); }
SHIM(SteamAPI_GetSteamInstallPath) { HFN(f, const char *(*)(void), "SteamAPI_GetSteamInstallPath"); RET(f ? f() : NULL); }

// Game server
SHIM(SteamInternal_GameServer_Init) {   // (unIP, usLegacySteamPort, usGamePort, usQueryPort, eServerMode, pchVersion)
    HFN(f, u8 (*)(u32, u16, u16, u16, int, const char *), "SteamInternal_GameServer_Init");
    u8 r = f ? f((u32)A1, (u16)A2, (u16)A3, (u16)A4, (int)AN(5), (const char *)AN(6)) : 0;
    if (r) init_counter++;
    RET(r);
}
SHIM(SteamGameServer_Init) {
    HFN(f, u8 (*)(u32, u16, u16, u16, int, const char *), "SteamGameServer_Init");
    HFN(g, u8 (*)(u32, u16, u16, u16, int, const char *), "SteamInternal_GameServer_Init");
    if (!f) f = g;
    u8 r = f ? f((u32)A1, (u16)A2, (u16)A3, (u16)A4, (int)AN(5), (const char *)AN(6)) : 0;
    if (r) init_counter++;
    RET(r);
}
SHIM(SteamInternal_GameServer_Init_V2) {   // (unIP, usGamePort, usQueryPort, eServerMode, pchVersion, versions, err)
    HFN(f, int (*)(u32, u16, u16, int, const char *, const char *, char *), "SteamInternal_GameServer_Init_V2");
    int r = 2;
    if (f) r = f((u32)A1, (u16)A2, (u16)A3, (int)A4, (const char *)AN(5), (const char *)AN(6), (char *)AN(7));
    else if (AN(7)) snprintf((char *)AN(7), 1024, "No libsteam_api.dylib found (wintomac)");
    if (!r) init_counter++;
    RET((u32)r);
}
SHIM(SteamGameServer_Shutdown) { HFN(f, void (*)(void), "SteamGameServer_Shutdown"); if (f) f(); init_counter++; RET(0); }
SHIM(SteamGameServer_RunCallbacks) { HFN(f, void (*)(void), "SteamGameServer_RunCallbacks"); if (f) f(); RET(0); }
SHIM(SteamGameServer_ReleaseCurrentThreadMemory) { HFN(f, void (*)(void), "SteamGameServer_ReleaseCurrentThreadMemory"); if (f) f(); RET(0); }
SHIM(SteamGameServer_BSecure) { HFN(f, u8 (*)(void), "SteamGameServer_BSecure"); RET(f ? f() : 0); }
SHIM(SteamGameServer_GetSteamID) { HFN(f, u64 (*)(void), "SteamGameServer_GetSteamID"); RET(f ? f() : 0); }
SHIM(SteamGameServer_GetHSteamPipe) { HFN(f, int (*)(void), "SteamGameServer_GetHSteamPipe"); RET((u32)(f ? f() : 0)); }
SHIM(SteamGameServer_GetHSteamUser) { HFN(f, int (*)(void), "SteamGameServer_GetHSteamUser"); RET((u32)(f ? f() : 0)); }
SHIM(SteamGameServer_GetIPCCallCount) { HFN(f, u32 (*)(void), "SteamGameServer_GetIPCCallCount"); RET(f ? f() : 0); }

// ---- interface accessors --------------------------------------------------------------------------------------
enum { ACC_USER, ACC_GAMESERVER, ACC_GLOBAL };
static void *host_iface(int kind, const char *ver) {
    HFN(user, void *(*)(int, const char *), "SteamInternal_FindOrCreateUserInterface");
    HFN(gs, void *(*)(int, const char *), "SteamInternal_FindOrCreateGameServerInterface");
    HFN(hu, int (*)(void), "SteamAPI_GetHSteamUser");
    HFN(gsu, int (*)(void), "SteamGameServer_GetHSteamUser");
    HFN(create, void *(*)(const char *), "SteamInternal_CreateInterface");
    if (!lib) return NULL;
    if (kind == ACC_GAMESERVER && gs) return gs(gsu ? gsu() : 0, ver);
    if (kind == ACC_GLOBAL && user) return user(0, ver);
    if (kind == ACC_USER && user) return user(hu ? hu() : 0, ver);
    if (!create) { log_once("%s: libsteam_api.dylib has no interface accessors", ver); return NULL; }
    // Old libsteam_api: ISteamClient::GetISteamGenericInterface(hUser, hPipe, version)
    static const char *cver[] = { "SteamClient021", "SteamClient020", "SteamClient017", "SteamClient016", "SteamClient015", "SteamClient014", "SteamClient012", NULL };
    for (int i = 0; cver[i]; i++) {
        void *cl = create(cver[i]); const SteamIface *ifc = find_iface(cver[i]);
        if (!cl || !ifc) continue;
        for (int k = 0; k < ifc->n; k++)
            if (!strcmp(steam_methods[ifc->start + k].name, "GetISteamGenericInterface")) {
                HFN(hp, int (*)(void), "SteamAPI_GetHSteamPipe");
                HFN(gsp, int (*)(void), "SteamGameServer_GetHSteamPipe");
                int u = kind == ACC_GAMESERVER ? (gsu ? gsu() : 0) : (hu ? hu() : 0);
                int p = kind == ACC_GAMESERVER ? (gsp ? gsp() : 0) : (hp ? hp() : 0);
                void *fn = ((void **)*(void **)cl)[steam_methods[ifc->start + k].host];
                return ((void *(*)(void *, int, int, const char *))fn)(cl, u, p, ver);
            }
    }
    return NULL;
}
SHIM(SteamInternal_FindOrCreateUserInterface) {
    HFN(f, void *(*)(int, const char *), "SteamInternal_FindOrCreateUserInterface");
    RET(wrap(f ? f((int)A1, (const char *)A2) : NULL, (const char *)A2));
}
SHIM(SteamInternal_FindOrCreateGameServerInterface) {
    HFN(f, void *(*)(int, const char *), "SteamInternal_FindOrCreateGameServerInterface");
    RET(wrap(f ? f((int)A1, (const char *)A2) : NULL, (const char *)A2));
}
SHIM(SteamInternal_CreateInterface) {
    HFN(f, void *(*)(const char *), "SteamInternal_CreateInterface");
    RET(wrap(f ? f((const char *)A1) : NULL, (const char *)A1));
}
// void *SteamInternal_ContextInit(void *pContextInitData): { void (*pFn)(void *ctx); uintp counter; ctx... }
SHIM(SteamInternal_ContextInit) {
    u64 d = A1;
    if (LD64(d + 8) != init_counter) {
        u64 arg = d + 16;
        rt_guest_call(c, LD64(d), 1, &arg);
        ST64(d + 8, init_counter);
    }
    RET(d + 16);
}
static void steam_flat_accessor(Ctx *c, int idx) {
    const SteamFlatAccessor *a = &steam_flat_accessors[idx];
    RET(wrap(host_iface(a->kind, a->version), a->version));
}

// Pre-1.4x SDKs exported SteamUser(), SteamFriends(), ... from steam_api64.dll itself; the interface versions
// they returned were compiled into that DLL, so we read them from the game's own steam_api64.dll.
static const char *legacy_version(const char *cls) {
    static char *dll; static size_t dll_len; static int tried;
    if (!tried) {
        tried = 1;
        static const char *names[] = { "steam_api64.dll", "Steam_api64.dll", "STEAM_API64.DLL", NULL };
        for (int i = 0; names[i] && !dll; i++) {
            char p[1200]; snprintf(p, sizeof p, "%s/%s", rt_exe_dir(), names[i]);
            FILE *f = fopen(p, "rb"); if (!f) continue;
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            dll = malloc(n > 0 ? n : 1); dll_len = fread(dll, 1, n > 0 ? n : 0, f); fclose(f);
        }
    }
    const char *found = NULL, *newest = NULL;
    for (int i = 0; i < STEAM_NIFACES; i++) {
        const SteamIface *f = &steam_ifaces[i];
        if (strcmp(f->cls, cls)) continue;
        if (!newest || strcmp(f->version, newest) > 0) newest = f->version;
        if (dll && memmem(dll, dll_len, f->version, strlen(f->version) + 1) && (!found || strcmp(f->version, found) > 0)) found = f->version;
    }
    if (!found) log_once("%s: version not found in the game's steam_api64.dll; using %s", cls, newest ? newest : "?");
    return found ? found : newest;
}
static u64 legacy(int kind, const char *cls) {
    const char *ver = legacy_version(cls);
    if (!ver || !have_lib()) return 0;
    if (!strcmp(cls, "ISteamClient")) { HFN(f, void *(*)(const char *), "SteamInternal_CreateInterface"); return wrap(f ? f(ver) : NULL, ver); }
    return wrap(host_iface(kind, ver), ver);
}
#define LEGACY(fn, kind, cls) SHIM(fn) { RET(legacy(kind, cls)); }
LEGACY(SteamClient, ACC_GLOBAL, "ISteamClient")
LEGACY(SteamUser, ACC_USER, "ISteamUser")
LEGACY(SteamFriends, ACC_USER, "ISteamFriends")
LEGACY(SteamUtils, ACC_USER, "ISteamUtils")
LEGACY(SteamMatchmaking, ACC_USER, "ISteamMatchmaking")
LEGACY(SteamMatchmakingServers, ACC_USER, "ISteamMatchmakingServers")
LEGACY(SteamUserStats, ACC_USER, "ISteamUserStats")
LEGACY(SteamApps, ACC_USER, "ISteamApps")
LEGACY(SteamNetworking, ACC_USER, "ISteamNetworking")
LEGACY(SteamRemoteStorage, ACC_USER, "ISteamRemoteStorage")
LEGACY(SteamScreenshots, ACC_USER, "ISteamScreenshots")
LEGACY(SteamHTTP, ACC_USER, "ISteamHTTP")
LEGACY(SteamController, ACC_USER, "ISteamController")
LEGACY(SteamUGC, ACC_USER, "ISteamUGC")
LEGACY(SteamAppList, ACC_USER, "ISteamAppList")
LEGACY(SteamMusic, ACC_USER, "ISteamMusic")
LEGACY(SteamMusicRemote, ACC_USER, "ISteamMusicRemote")
LEGACY(SteamHTMLSurface, ACC_USER, "ISteamHTMLSurface")
LEGACY(SteamInventory, ACC_USER, "ISteamInventory")
LEGACY(SteamVideo, ACC_USER, "ISteamVideo")
LEGACY(SteamParentalSettings, ACC_USER, "ISteamParentalSettings")
LEGACY(SteamGameServer, ACC_GAMESERVER, "ISteamGameServer")
LEGACY(SteamGameServerUtils, ACC_GAMESERVER, "ISteamUtils")
LEGACY(SteamGameServerNetworking, ACC_GAMESERVER, "ISteamNetworking")
LEGACY(SteamGameServerStats, ACC_GAMESERVER, "ISteamGameServerStats")
LEGACY(SteamGameServerHTTP, ACC_GAMESERVER, "ISteamHTTP")
LEGACY(SteamGameServerUGC, ACC_GAMESERVER, "ISteamUGC")
LEGACY(SteamGameServerInventory, ACC_GAMESERVER, "ISteamInventory")

// ---- callbacks and call results -----------------------------------------------------------------------------
// Host-side stand-in for a guest CCallbackBase. First fields follow the Itanium CCallbackBase layout
// (vptr, uint8 m_nCallbackFlags, int m_iCallback) because libsteam_api reads and writes them.
typedef struct Proxy {
    void **vptr; u8 flags; int icb;
    u64 guest; int win_size; const SteamConv *cv;
    struct Proxy *next;
} Proxy;
static Proxy *proxies;
static pthread_mutex_t proxy_lock = PTHREAD_MUTEX_INITIALIZER;

static void deliver(Proxy *p, void *param, int with_call, u8 io_failure, u64 call) {
    Ctx *c = rt_cur_ctx ? rt_cur_ctx : rt_foreign_ctx();
    u8 *w;
    if (p->cv) { w = calloc(1, p->cv->win_size + 8); if (param) conv_m2w(p->cv, w, param); fix_completed(p->icb, w); }
    else { w = calloc(1, p->win_size + 8); if (param) memcpy(w, param, p->win_size); }
    u64 gv = LD64(p->guest);
    if (steam_trace()) rt_log("steam: callback %d -> guest %llx%s", p->icb, (unsigned long long)p->guest, with_call ? " (call result)" : "");
    if (with_call) { u64 a[4] = { p->guest, (u64)w, io_failure, call }; rt_guest_call(c, LD64(gv + 0), 4, a); }
    else { u64 a[2] = { p->guest, (u64)w }; rt_guest_call(c, LD64(gv + 8), 2, a); }
    free(w);
}
static void proxy_run(Proxy *p, void *param) { deliver(p, param, 0, 0, 0); }
static void proxy_run_call(Proxy *p, void *param, u8 io_failure, u64 call) { deliver(p, param, 1, io_failure, call); }
static int proxy_size(Proxy *p) { return p->cv ? p->cv->mac_size : p->win_size; }
static void *proxy_vtbl[3] = { (void *)proxy_run, (void *)proxy_run_call, (void *)proxy_size };

static Proxy *proxy_for(Ctx *c, u64 g, int id) {
    pthread_mutex_lock(&proxy_lock);
    Proxy *p = proxies; while (p && p->guest != g) p = p->next;
    if (!p) { p = calloc(1, sizeof *p); p->vptr = proxy_vtbl; p->guest = g; p->next = proxies; proxies = p; }
    pthread_mutex_unlock(&proxy_lock);
    u64 gv = LD64(g);
    p->win_size = (int)rt_guest_call(c, LD64(gv + 16), 1, &g);   // guest GetCallbackSizeBytes()
    p->icb = id;
    p->cv = cb_conv(id, p->win_size, 0);
    if (!p->cv && !cb_info(id)) log_once("callback %d: unknown struct; payload passed through unconverted", id);
    p->flags = LD8(g + 8);
    return p;
}
static Proxy *proxy_find(u64 g) {
    pthread_mutex_lock(&proxy_lock);
    Proxy *p = proxies; while (p && p->guest != g) p = p->next;
    pthread_mutex_unlock(&proxy_lock);
    return p;
}

SHIM(SteamAPI_RegisterCallback) {   // (CCallbackBase *pCallback, int iCallback)
    u64 g = A1; int id = (int)A2;
    Proxy *p = proxy_for(c, g, id);
    HFN(f, void (*)(void *, int), "SteamAPI_RegisterCallback");
    if (f) f(p, id); else p->flags |= 1;
    ST8(g + 8, p->flags); ST32(g + 12, (u32)id);
    RET(0);
}
SHIM(SteamAPI_UnregisterCallback) {
    u64 g = A1; Proxy *p = proxy_find(g);
    HFN(f, void (*)(void *), "SteamAPI_UnregisterCallback");
    if (p) { if (f) f(p); p->flags &= ~1; ST8(g + 8, p->flags); }
    RET(0);
}
SHIM(SteamAPI_RegisterCallResult) {   // (CCallbackBase *pCallback, SteamAPICall_t hAPICall)
    u64 g = A1;
    Proxy *p = proxy_for(c, g, (int)LD32(g + 12));
    HFN(f, void (*)(void *, u64), "SteamAPI_RegisterCallResult");
    if (f) f(p, A2);
    ST8(g + 8, p->flags);
    RET(0);
}
SHIM(SteamAPI_UnregisterCallResult) {
    u64 g = A1; Proxy *p = proxy_find(g);
    HFN(f, void (*)(void *, u64), "SteamAPI_UnregisterCallResult");
    if (p && f) f(p, A2);
    if (p) ST8(g + 8, p->flags);
    RET(0);
}
SHIM(SteamAPI_RunCallbacks) { HFN(f, void (*)(void), "SteamAPI_RunCallbacks"); if (f) f(); RET(0); }

// Manual dispatch (SDK 1.48+): callbacks are pulled by the game; payloads are converted to the Windows layout.
static __thread u8 *md_buf;
SHIM(SteamAPI_ManualDispatch_Init) { HFN(f, void (*)(void), "SteamAPI_ManualDispatch_Init"); if (f) f(); RET(0); }
SHIM(SteamAPI_ManualDispatch_RunFrame) { HFN(f, void (*)(int), "SteamAPI_ManualDispatch_RunFrame"); if (f) f((int)A1); RET(0); }
SHIM(SteamAPI_ManualDispatch_GetNextCallback) {   // (HSteamPipe, CallbackMsg_t *) -> bool
    HFN(f, u8 (*)(int, void *), "SteamAPI_ManualDispatch_GetNextCallback");
    struct { int user, id; u8 *param; int cub; int pad; } m = { 0 };
    if (!f || !f((int)A1, &m)) RET(0);
    free(md_buf);
    u32 wsz; md_buf = payload_to_win(m.id, m.param, m.cub, -1, &wsz);
    u64 out = A2;
    ST32(out, (u32)m.user); ST32(out + 4, (u32)m.id); ST64(out + 8, (u64)md_buf); ST32(out + 16, wsz);
    RET(1);
}
SHIM(SteamAPI_ManualDispatch_FreeLastCallback) {
    HFN(f, void (*)(int), "SteamAPI_ManualDispatch_FreeLastCallback");
    if (f) f((int)A1);
    free(md_buf); md_buf = NULL;
    RET(0);
}
SHIM(SteamAPI_ManualDispatch_GetAPICallResult) {   // (pipe, hCall, pCallback, cubCallback, iCallbackExpected, pbFailed)
    HFN(f, u8 (*)(int, u64, void *, int, int, void *), "SteamAPI_ManualDispatch_GetAPICallResult");
    if (!f) RET(0);
    u64 buf = A3; int cub = (int)A4, id = (int)AN(5);
    const SteamConv *cv = cb_conv(id, cub, 0);
    int msz = cv ? cv->mac_size : cub;
    u8 *m = calloc(1, msz + 8);
    u8 ok = f((int)A1, A2, m, msz, id, (void *)AN(6));
    if (ok && buf) { memset((void *)buf, 0, cub); if (cv) { conv_m2w(cv, (u8 *)buf, m); fix_completed(id, (u8 *)buf); } else memcpy((void *)buf, m, cub); }
    free(m);
    RET(ok);
}

// ---- table ---------------------------------------------------------------------------------------------------
#define SLOTE1(i) { NULL, "__wintomac_steam_slot_" #i, sh_steam_slot_##i },
#define SLOTE10(i) SLOTE1(i##0) SLOTE1(i##1) SLOTE1(i##2) SLOTE1(i##3) SLOTE1(i##4) SLOTE1(i##5) SLOTE1(i##6) SLOTE1(i##7) SLOTE1(i##8) SLOTE1(i##9)
#define FLATE(i, name) { NULL, #name, sh_##name },
#define E(n) { NULL, #n, sh_##n },
const Shim shims_steam[] = {
    E(SteamAPI_Init) E(SteamAPI_InitSafe) E(SteamAPI_InitFlat) E(SteamInternal_SteamAPI_Init) E(SteamAPI_Shutdown)
    E(SteamAPI_IsSteamRunning) E(SteamAPI_RestartAppIfNecessary) E(SteamAPI_GetHSteamUser) E(SteamAPI_GetHSteamPipe)
    E(SteamAPI_ReleaseCurrentThreadMemory) E(SteamAPI_SetMiniDumpComment) E(SteamAPI_WriteMiniDump)
    E(SteamAPI_UseBreakpadCrashHandler) E(SteamAPI_SetBreakpadAppID) E(SteamAPI_SetTryCatchCallbacks)
    E(SteamAPI_GetSteamInstallPath)
    E(SteamInternal_GameServer_Init) E(SteamGameServer_Init) E(SteamInternal_GameServer_Init_V2) E(SteamGameServer_Shutdown)
    E(SteamGameServer_RunCallbacks) E(SteamGameServer_ReleaseCurrentThreadMemory) E(SteamGameServer_BSecure)
    E(SteamGameServer_GetSteamID) E(SteamGameServer_GetHSteamPipe) E(SteamGameServer_GetHSteamUser)
    E(SteamGameServer_GetIPCCallCount)
    E(SteamInternal_FindOrCreateUserInterface) E(SteamInternal_FindOrCreateGameServerInterface)
    E(SteamInternal_CreateInterface) E(SteamInternal_ContextInit)
    E(SteamClient) E(SteamUser) E(SteamFriends) E(SteamUtils) E(SteamMatchmaking) E(SteamMatchmakingServers)
    E(SteamUserStats) E(SteamApps) E(SteamNetworking) E(SteamRemoteStorage) E(SteamScreenshots) E(SteamHTTP)
    E(SteamController) E(SteamUGC) E(SteamAppList) E(SteamMusic) E(SteamMusicRemote) E(SteamHTMLSurface)
    E(SteamInventory) E(SteamVideo) E(SteamParentalSettings) E(SteamGameServer) E(SteamGameServerUtils)
    E(SteamGameServerNetworking) E(SteamGameServerStats) E(SteamGameServerHTTP) E(SteamGameServerUGC)
    E(SteamGameServerInventory)
    E(SteamAPI_RegisterCallback) E(SteamAPI_UnregisterCallback) E(SteamAPI_RegisterCallResult)
    E(SteamAPI_UnregisterCallResult) E(SteamAPI_RunCallbacks)
    E(SteamAPI_ManualDispatch_Init) E(SteamAPI_ManualDispatch_RunFrame) E(SteamAPI_ManualDispatch_GetNextCallback)
    E(SteamAPI_ManualDispatch_FreeLastCallback) E(SteamAPI_ManualDispatch_GetAPICallResult)
    STEAM_FLAT_METHOD_LIST(FLATE) STEAM_FLAT_PLAIN_LIST(FLATE) STEAM_FLAT_ACCESSOR_LIST(FLATE)
    SLOTS(SLOTE10)
    { NULL, NULL, NULL }
};
