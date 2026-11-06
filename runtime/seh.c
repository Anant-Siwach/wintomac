// Windows x64 structured exception handling (the ntdll side), for recompiled code.
//
// The guest stack is the real x64 stack (return addresses, saved non-volatiles), and every image keeps its
// .pdata/.xdata, so frames are walked exactly as on Windows: RtlVirtualUnwind interprets the unwind codes,
// the dispatcher calls each frame's language handler (GCC's __gxx_personality_seh0, MSVC's __CxxFrameHandler,
// __C_specific_handler, ...) which is ordinary guest code, and RtlUnwindEx runs cleanups and finally restores a
// CONTEXT. Restoring is the recompiler-specific part: we longjmp into the host frame record (RtEhFrame) of the
// recompiled function that owns the target frame, reload registers from the CONTEXT and jump to the label.
//
// References: Microsoft x64 exception handling docs; Wine's dlls/ntdll/signal_x86_64.c (semantics of
// dispatch / unwind / nested unwinds); GCC's libgcc/unwind-seh.c (what its personality expects).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "win32.h"

// ---- layouts --------------------------------------------------------------------------
#define CTX_SIZE 0x4D0
#define CTX_FLAGS 0x30
#define CTX_MXCSR 0x34
#define CTX_EFLAGS 0x44
#define CTX_R(i) (0x78 + 8 * (i))          // Rax..R15 in our register order
#define CTX_RIP 0xF8
#define CTX_XMM(i) (0x1A0 + 16 * (i))
#define REC_SIZE 152
#define DISP_SIZE 80
enum { EXCEPTION_NONCONTINUABLE = 1, EXCEPTION_UNWINDING = 2, EXCEPTION_EXIT_UNWIND = 4, EXCEPTION_TARGET_UNWIND = 0x20 };
enum { ExceptionContinueExecution = 0, ExceptionContinueSearch = 1, ExceptionNestedException = 2, ExceptionCollidedUnwind = 3 };
#define STATUS_UNWIND 0xC0000027u
#define STATUS_NONCONTINUABLE_EXCEPTION 0xC0000025u

#define CR(ctx, i) (*(u64 *)((ctx) + CTX_R(i)))
#define CRIP(ctx) (*(u64 *)((ctx) + CTX_RIP))

// Scratch buffers (CONTEXTs, records) that guest handlers can point into. A ring per thread: exceptions
// that longjmp away never free anything, and nesting depth is small.
static __thread u8 *ring; static __thread unsigned ring_i;
static u64 scratch(void) {
    if (!ring) ring = aligned_alloc(64, 256 * 0x600);
    u64 p = (u64)ring + 0x600ull * (ring_i++ & 255);
    memset((void *)p, 0, 0x600);
    return p;
}

static int trace(void) { static int t = -1; if (t < 0) t = getenv("WINTOMAC_SEH_TRACE") != NULL; return t; }

// ---- function table lookup -------------------------------------------------------------
// Function tables registered at runtime (RtlAddFunctionTable): mingw DLLs and JIT-ish code register .pdata this way.
typedef struct { u64 table; u32 count; u64 base; } DynTable;
static DynTable *dyn; static int ndyn, dyn_cap;   // grows: GZDoom's ZScript JIT registers one table per function
static pthread_mutex_t dyn_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 dyn_lookup(u64 pc, u64 *image_base) {
    u64 r = 0;
    pthread_mutex_lock(&dyn_lock);
    for (int t = 0; t < ndyn && !r; t++) {
        u64 base = dyn[t].base; if (pc < base) continue;
        u32 off = (u32)(pc - base);
        for (u32 i = 0; i < dyn[t].count; i++) {
            u64 e = dyn[t].table + 12ull * i;
            if (off >= LD32(e) && off < LD32(e + 4)) { if (image_base) *image_base = base; r = e; break; }
        }
    }
    pthread_mutex_unlock(&dyn_lock);
    return r;
}
SHIM(RtlAddFunctionTable) {  // (RUNTIME_FUNCTION *table, count, base)
    pthread_mutex_lock(&dyn_lock);
    if (ndyn == dyn_cap) { int nc = dyn_cap ? 2 * dyn_cap : 256; DynTable *n = realloc(dyn, nc * sizeof *n); if (n) { dyn = n; dyn_cap = nc; } }
    int ok = ndyn < dyn_cap; if (ok) dyn[ndyn++] = (DynTable){ A1, (u32)A2, A3 };
    pthread_mutex_unlock(&dyn_lock);
    RET(ok);
}
SHIM(RtlDeleteFunctionTable) {
    pthread_mutex_lock(&dyn_lock);
    int ok = 0;
    for (int i = 0; i < ndyn; i++) if (dyn[i].table == A1) { dyn[i] = dyn[--ndyn]; ok = 1; break; }
    pthread_mutex_unlock(&dyn_lock);
    RET(ok);
}

u64 rt_seh_lookup(u64 pc, u64 *image_base) {
    u64 ib = rt_module_base_for(pc, NULL);
    if (!ib) return dyn_lookup(pc, image_base);
    u32 pe = LD32(ib + 0x3c);
    u32 rva = LD32(ib + pe + 24 + 112 + 8 * 3), size = LD32(ib + pe + 24 + 112 + 8 * 3 + 4);
    u32 off = (u32)(pc - ib); u64 lo = 0, hi = size / 12;
    while (lo < hi) {
        u64 mid = (lo + hi) / 2, e = ib + rva + 12 * mid;
        if (off < LD32(e)) hi = mid; else if (off >= LD32(e + 4)) lo = mid + 1;
        else { if (image_base) *image_base = ib; return e; }
    }
    return 0;
}

// ---- RtlVirtualUnwind ------------------------------------------------------------------------
// Unwinds one frame in the guest CONTEXT at `ctx`. Returns the language handler for `type`
// (1 = exception handler, 2 = termination/unwind handler) if this frame has one and pc is in its body.
static u64 virtual_unwind(u32 type, u64 base, u64 pc, u64 fe, u64 ctx, u64 *hdata, u64 *estab) {
    u64 handler = 0; int primary = 1, machframe = 0;
    u64 begin = base + LD32(fe), ui = base + LD32(fe + 8);
    u64 off = pc - begin;
    *estab = 0; if (hdata) *hdata = 0;
    for (int chain = 0; chain < 32; chain++) {
        u8 vf = LD8(ui), prolog = LD8(ui + 1), n = LD8(ui + 2), fr = LD8(ui + 3);
        u32 flags = vf >> 3, freg = fr & 15, foff = fr >> 4;
        int in_prolog = primary && off < prolog;
        if (primary) {
            int fp_set = 0;
            if (freg) {
                if (!in_prolog) fp_set = 1;
                else for (u32 i = 0; i < n; i++) { u16 c = LD16(ui + 4 + 2 * i); if (((c >> 8) & 15) == 3 && (c & 0xFF) <= off) fp_set = 1; }
            }
            *estab = fp_set ? CR(ctx, freg) - 16ull * foff : CR(ctx, RSP);
        }
        for (u32 i = 0; i < n; ) {
            u16 code = LD16(ui + 4 + 2 * i);
            u32 cofs = code & 0xFF, op = (code >> 8) & 15, info = code >> 12;
            int skip = in_prolog && cofs > off;   // this prolog step hasn't executed yet
            u32 slots = 1;
            switch (op) {
            case 0: if (!skip) { CR(ctx, info) = LD64(CR(ctx, RSP)); CR(ctx, RSP) += 8; } break;                       // PUSH_NONVOL
            case 1: if (info == 0) { slots = 2; if (!skip) CR(ctx, RSP) += 8ull * LD16(ui + 4 + 2 * (i + 1)); }       // ALLOC_LARGE
                    else { slots = 3; if (!skip) CR(ctx, RSP) += LD32(ui + 4 + 2 * (i + 1)); } break;
            case 2: if (!skip) CR(ctx, RSP) += 8ull * info + 8; break;                                                 // ALLOC_SMALL
            case 3: if (!skip) CR(ctx, RSP) = CR(ctx, freg) - 16ull * foff; break;                                     // SET_FPREG
            case 4: slots = 2; if (!skip) CR(ctx, info) = LD64(CR(ctx, RSP) + 8ull * LD16(ui + 4 + 2 * (i + 1))); break;  // SAVE_NONVOL
            case 5: slots = 3; if (!skip) CR(ctx, info) = LD64(CR(ctx, RSP) + LD32(ui + 4 + 2 * (i + 1))); break;      // SAVE_NONVOL_FAR
            case 6: slots = 2; break;                                                                                  // EPILOG (v2) / SAVE_XMM (v1)
            case 7: slots = 3; break;                                                                                  // SPARE / SAVE_XMM_FAR (v1)
            case 8: slots = 2; if (!skip) memcpy((void *)(ctx + CTX_XMM(info)), (void *)(CR(ctx, RSP) + 16ull * LD16(ui + 4 + 2 * (i + 1))), 16); break;  // SAVE_XMM128
            case 9: slots = 3; if (!skip) memcpy((void *)(ctx + CTX_XMM(info)), (void *)(CR(ctx, RSP) + LD32(ui + 4 + 2 * (i + 1))), 16); break;          // SAVE_XMM128_FAR
            case 10: if (!skip) { if (info) CR(ctx, RSP) += 8; CRIP(ctx) = LD64(CR(ctx, RSP)); CR(ctx, RSP) = LD64(CR(ctx, RSP) + 24); machframe = 1; } break;  // PUSH_MACHFRAME
            default: break;
            }
            i += slots;
        }
        u64 tail = ui + 4 + 2 * ((n + 1u) & ~1u);
        if (flags & 4) {                      // CHAININFO: continue with the parent's codes (fully executed)
            fe = tail; ui = base + LD32(fe + 8); primary = 0;
            continue;
        }
        if ((flags & type) && !in_prolog && primary) { handler = base + LD32(tail); if (hdata) *hdata = tail + 4; }
        else if ((flags & type) && !primary) { handler = base + LD32(tail); if (hdata) *hdata = tail + 4; }
        break;
    }
    if (!machframe) { CRIP(ctx) = LD64(CR(ctx, RSP)); CR(ctx, RSP) += 8; }
    return handler;
}

static int in_stack(Ctx *c, u64 sp) {
    u64 hi = LD64(c->gs_base + 8), lo = LD64(c->gs_base + 16);
    return sp >= lo && sp < hi;
}
// one frame up when there is no unwind info (leaf function): return address at [rsp]
static int leaf_unwind(Ctx *c, u64 ctx) {
    if (!in_stack(c, CR(ctx, RSP))) return 0;
    CRIP(ctx) = LD64(CR(ctx, RSP)); CR(ctx, RSP) += 8;
    return CRIP(ctx) != 0;
}

// ---- contexts ---------------------------------------------------------------------------
static void capture(Ctx *c, u64 ctx, u64 rip, u64 rsp) {
    for (int i = 0; i < 16; i++) CR(ctx, i) = c->r[i];
    CR(ctx, RSP) = rsp; CRIP(ctx) = rip;
    ST32(ctx + CTX_FLAGS, 0x10001F); ST32(ctx + CTX_MXCSR, c->mxcsr); ST32(ctx + CTX_EFLAGS, 0x202);
    ST32(ctx + 0x100 + 24, c->mxcsr);  // FltSave.MxCsr
    for (int i = 0; i < 16; i++) memcpy((void *)(ctx + CTX_XMM(i)), &c->x[i], 16);
}
// The caller of the shim we're in: return address at [rsp], its rsp is ours + 8.
static void capture_caller(Ctx *c, u64 ctx) { capture(c, ctx, LD64(c->r[RSP]), c->r[RSP] + 8); }

static void load(Ctx *c, u64 ctx) {
    for (int i = 0; i < 16; i++) c->r[i] = CR(ctx, i);
    for (int i = 0; i < 16; i++) memcpy(&c->x[i], (void *)(ctx + CTX_XMM(i)), 16);
    c->mxcsr = LD32(ctx + CTX_MXCSR) ? LD32(ctx + CTX_MXCSR) : c->mxcsr;
}

// ---- resuming recompiled code ---------------------------------------------------------------
static void miss_label(Ctx *c, u64 target, const char *why) {
    FILE *f = fopen("wintomac_misses.txt", "a");
    if (f) { fprintf(f, "label 0x%llx\n", (unsigned long long)target); fclose(f); }
    rt_log("cannot resume at 0x%llx: %s (logged to wintomac_misses.txt; recompile with --seeds-file)", (unsigned long long)target, why);
    (void)c; exit(77);
}

static void resume(Ctx *c, u64 ctx) __attribute__((noreturn));
static void resume(Ctx *c, u64 ctx) {
    u64 target = CRIP(ctx), sp = CR(ctx, RSP);
    RtEhFrame *f = c->eh_top;
    while (f && f->entry_rsp <= sp) f = f->prev;   // innermost recompiled frame that encloses the target frame
    if (trace()) rt_log("resume at %llx rsp=%llx -> frame of f_%llx", (unsigned long long)target, (unsigned long long)sp, f ? (unsigned long long)f->fn : 0ull);
    if (!f) miss_label(c, target, "no resumable recompiled frame encloses it");
    load(c, ctx);
    c->eh_top = f;
    c->seh_level = f->seh_level;   // handlers between the raise and here are abandoned
    f->target = target;
    _longjmp(f->jb, 1);
}

// Start of the function a RUNTIME_FUNCTION belongs to: follow UNW_FLAG_CHAININFO links to the root chunk.
u64 rt_func_root(u64 base, u64 fe) {
    for (int i = 0; i < 32; i++) {
        u64 ui = base + LD32(fe + 8);
        if (!((LD8(ui) >> 3) & 4)) break;
        u8 n = LD8(ui + 2);
        fe = ui + 4 + 2 * ((n + 1u) & ~1u);
    }
    return base + LD32(fe);
}

// A function's resume switch didn't contain the target: it belongs to another chunk (e.g. a GCC .cold part
// that runs in this function's frame). Enter that chunk at the target; when it returns, so do we.
extern GuestFn rt_lookup_fn(u64 addr);
void rt_resume_elsewhere(Ctx *c, u64 target, u64 self) {
    u64 base, fe = rt_seh_lookup(target, &base);
    u64 owner = fe ? rt_func_root(base, fe) : 0;
    GuestFn fn = owner ? rt_lookup_fn(owner) : NULL;
    if (!fn || owner == self) {
        extern void rt_note_miss(u64 target, int label); extern int rt_interp_disabled(void);
        extern void rt_interp_run(Ctx *c, u64 pc);
        if (!rt_interp_disabled()) { rt_note_miss(target, 1); rt_interp_run(c, target); return; }   // runtime/interp.c
        miss_label(c, target, "not a label of the recompiled function that owns it");
    }
    c->resume_target = target;
    fn(c);
}

// ---- Itanium C++ ABI (DWARF) landing pads ------------------------------------------------------
// Code built for the Itanium ABI (AnyPS5's libraries and relinked PS5 programs: .eh_frame + LSDA, not SEH) carries
// its own unwinder in guest code (AnyPS5 libc.prx: __cxa_throw -> _Unwind_RaiseException -> personality). It
// resumes a frame the x86 way: LibcRestoreRegisters loads every register, sets rsp to the target frame and jumps to
// the landing pad, which is in the middle of a recompiled function whose host frame is further up the host stack.
// The recompiler gives each such function a frame record and makes its pads labels (recomp/analysis.py
// _dwarf_eh); it lists every (pad, owner) pair in the __DATA,__wtm_ehpad section (recomp/lifter.py). A jump to an
// address that is not a function start ends up here: if it is a pad and the innermost frame record enclosing the
// new rsp belongs to its owner, longjmp there (dropping the host frames of the unwinder and of every guest frame
// it unwound) and enter the label with the registers the unwinder put in Ctx. _Unwind_Resume after a cleanup
// pad, rethrow and nested exceptions are ordinary guest calls into the unwinder that end in another such jump.
// Reference: Itanium C++ ABI, Exception Handling (itanium-cxx-abi.github.io/cxx-abi/abi-eh.html), LLVM libunwind.
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
typedef struct { u64 pad, owner; } EhPad;
static EhPad *ehpads; static size_t nehpads;
static pthread_once_t ehpads_once = PTHREAD_ONCE_INIT;
static int ehpad_cmp(const void *a, const void *b) {
    const EhPad *x = a, *y = b;
    return x->pad < y->pad ? -1 : x->pad > y->pad ? 1 : x->owner < y->owner ? -1 : x->owner > y->owner;
}
static void ehpads_init(void) {
    unsigned long size = 0;
    const u8 *sec = getsectiondata(&_mh_execute_header, "__DATA", "__wtm_ehpad", &size);
    if (!sec || !size) return;
    size_t n = size / sizeof(EhPad), k = 0;
    ehpads = malloc(n * sizeof(EhPad));
    if (!ehpads) return;
    for (size_t i = 0; i < n; i++) { EhPad e; memcpy(&e, sec + i * sizeof e, sizeof e); if (e.pad) ehpads[k++] = e; }
    qsort(ehpads, k, sizeof *ehpads, ehpad_cmp);
    nehpads = k;
}
static int is_pad_of(u64 target, u64 owner) {
    size_t lo = 0, hi = nehpads;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (ehpads[mid].pad < target) lo = mid + 1; else hi = mid; }
    int any = 0;
    for (; lo < nehpads && ehpads[lo].pad == target; lo++) { any = 1; if (ehpads[lo].owner == owner) return 2; }
    return any;
}

// Returns only if target isn't a landing pad that can be resumed this way.
void rt_landing_pad(Ctx *c, u64 target) {
    pthread_once(&ehpads_once, ehpads_init);
    if (!nehpads) return;
    u64 sp = c->r[RSP];
    RtEhFrame *f = c->eh_top;
    while (f && f->entry_rsp <= sp) f = f->prev;   // innermost recompiled frame that encloses the target frame
    int k = is_pad_of(target, f ? f->fn : 0);
    if (k != 2) {
        if (k == 1) rt_log("landing pad 0x%llx (rsp=%llx): the enclosing frame record is %s0x%llx, not its owner",
                           (unsigned long long)target, (unsigned long long)sp, f ? "f_" : "", f ? (unsigned long long)f->fn : 0ull);
        return;
    }
    if (trace()) rt_log("landing pad %llx rsp=%llx -> frame of f_%llx", (unsigned long long)target, (unsigned long long)sp, (unsigned long long)f->fn);
    c->eh_top = f;
    c->seh_level = f->seh_level;
    f->target = target;
    _longjmp(f->jb, 1);
}

// ---- dispatch ---------------------------------------------------------------------------
// On Windows, handlers run inside ntdll executor frames (RtlpExecuteHandlerForException/ForUnwind) that sit
// between the handler and the frames being dispatched. A walk started *inside* a handler (a nested raise, or a
// handler calling RtlUnwindEx) climbs through those frames and continues in the original chain. Here handlers
// are called from host code, so the guest chain ends at rt_guest_call's sentinel return address (0). Each
// handler invocation records how a walk continues past it, with Windows' rules:
//   dispatch walk crossing a dispatch level   -> that dispatch's raise context (the original frames)
//   any walk crossing an unwind level         -> unwind: the outer unwind's current frame (collided unwind)
//                                                dispatch: the unwind's caller chain
//   unwind walk crossing a dispatch level     -> that dispatch's raise context
typedef struct { int unwind; u64 origin; u64 frame; } Level;
static __thread Level levels[64];
#define nlevels (c->seh_level)   // per thread, in Ctx so a resume can restore it from the frame record

// Called when a walk hits pc == 0. Returns 1 and loads `cur` with the continuation, or 0 at the true end.
static int cross(Ctx *c, u64 cur, int start_level, int *crossings, int unwind_walk) {
    (void)c;
    int idx = start_level - 1 - *crossings;
    if (idx < 0) return 0;
    (*crossings)++;
    Level *L = &levels[idx];
    u64 src = L->unwind ? (unwind_walk ? L->frame : L->origin) : L->origin;
    memcpy((void *)cur, (void *)src, CTX_SIZE);
    return 1;
}

static u32 call_handler(Ctx *c, int unwind, u64 origin, u64 frame, u64 handler, u64 rec, u64 estab, u64 ctx, u64 disp) {
    if (nlevels >= 64) { rt_log("SEH: handlers nested too deeply"); exit(78); }
    Level *L = &levels[nlevels++];
    L->unwind = unwind;
    L->origin = scratch(); memcpy((void *)L->origin, (void *)origin, CTX_SIZE);
    L->frame = scratch(); if (frame) memcpy((void *)L->frame, (void *)frame, CTX_SIZE);
    u64 args[4] = { rec, estab, ctx, disp };
    if (trace()) rt_log("  %s handler %llx code=%08x flags=%x frame=%llx pc=%llx level=%d", unwind ? "unwind" : "dispatch",
                        (unsigned long long)handler, LD32(rec), LD32(rec + 4), (unsigned long long)estab, (unsigned long long)LD64(disp), nlevels);
    u32 r = (u32)rt_guest_call(c, handler, 4, args);
    nlevels--;
    return r;
}

static void fill_disp(u64 d, u64 pc, u64 base, u64 fe, u64 estab, u64 target, u64 ctx, u64 handler, u64 hdata, u64 history) {
    ST64(d + 0, pc); ST64(d + 8, base); ST64(d + 16, fe); ST64(d + 24, estab); ST64(d + 32, target);
    ST64(d + 40, ctx); ST64(d + 48, handler); ST64(d + 56, hdata); ST64(d + 64, history); ST32(d + 72, 0);
}

// Phase 1: find a handler. Returns 1 for "continue execution" (a handler fixed things up), 0 if unhandled.
static int dispatch(Ctx *c, u64 rec, u64 ctx) {
    u64 cur = scratch(), disp = scratch();
    memcpy((void *)cur, (void *)ctx, CTX_SIZE);
    int start_level = nlevels, crossings = 0;
    for (int depth = 0; depth < 100000; depth++) {
        u64 pc = CRIP(cur), base = 0;
        if (!pc) { if (cross(c, cur, start_level, &crossings, 0)) continue; break; }
        u64 fe = rt_seh_lookup(pc, &base);
        if (!fe) { if (!leaf_unwind(c, cur) && !(CRIP(cur) == 0 && cross(c, cur, start_level, &crossings, 0))) break; continue; }
        u64 hdata, estab;
        u64 handler = virtual_unwind(1, base, pc, fe, cur, &hdata, &estab);
        if (handler) {
            fill_disp(disp, pc, base, fe, estab, 0, cur, handler, hdata, 0);
            u32 r = call_handler(c, 0, ctx, 0, handler, rec, estab, ctx, disp);
            if (r == ExceptionContinueExecution) {
                if (LD32(rec + 4) & EXCEPTION_NONCONTINUABLE) { rt_log("handler continued a non-continuable exception"); exit(78); }
                return 1;
            }
        }
        if (CRIP(cur) && !in_stack(c, CR(cur, RSP))) break;
    }
    return 0;
}

// Phase 2: unwind to target_frame, running termination handlers, then resume at target_ip. Never returns.
static void unwind_ex(Ctx *c, u64 target_frame, u64 target_ip, u64 rec, u64 retval, u64 ctxbuf, u64 history, u64 start) __attribute__((noreturn));
static void unwind_ex(Ctx *c, u64 target_frame, u64 target_ip, u64 rec, u64 retval, u64 ctxbuf, u64 history, u64 start) {
    if (!rec) { rec = scratch(); ST32(rec, STATUS_UNWIND); ST64(rec + 16, CRIP(start)); }
    ST32(rec + 4, LD32(rec + 4) | EXCEPTION_UNWINDING | (target_frame ? 0 : EXCEPTION_EXIT_UNWIND));
    if (!ctxbuf) ctxbuf = scratch();
    u64 cur = scratch(), frame = scratch(), disp = scratch(), origin = scratch();
    memcpy((void *)cur, (void *)start, CTX_SIZE);
    memcpy((void *)origin, (void *)start, CTX_SIZE);
    int start_level = nlevels, crossings = 0;
    if (trace()) rt_log("unwind to frame %llx ip %llx", (unsigned long long)target_frame, (unsigned long long)target_ip);
    for (int depth = 0; ; depth++) {
        if (depth > 100000) { rt_log("RtlUnwindEx: runaway walk"); exit(78); }
        u64 pc = CRIP(cur), base = 0;
        if (!pc) {
            if (cross(c, cur, start_level, &crossings, 1)) continue;
            rt_log("RtlUnwindEx: target frame %llx not found", (unsigned long long)target_frame); exit(78);
        }
        u64 fe = rt_seh_lookup(pc, &base);
        memcpy((void *)frame, (void *)cur, CTX_SIZE);
        if (!fe) {
            if (!leaf_unwind(c, cur) && CRIP(cur) != 0) { rt_log("RtlUnwindEx: broken frame chain at %llx", (unsigned long long)pc); exit(78); }
            continue;
        }
        u64 hdata, estab;
        u64 handler = virtual_unwind(2, base, pc, fe, cur, &hdata, &estab);
        if (target_frame && estab > target_frame) { rt_log("RtlUnwindEx: unwound past target frame %llx", (unsigned long long)target_frame); exit(78); }
        int is_target = target_frame && estab == target_frame;
        if (is_target) ST32(rec + 4, LD32(rec + 4) | EXCEPTION_TARGET_UNWIND);
        memcpy((void *)ctxbuf, (void *)frame, CTX_SIZE);
        if (handler) {
            fill_disp(disp, pc, base, fe, estab, target_ip, ctxbuf, handler, hdata, history);
            call_handler(c, 1, origin, frame, handler, rec, estab, ctxbuf, disp);
        }
        if (is_target) break;
        ST32(rec + 4, LD32(rec + 4) & ~EXCEPTION_TARGET_UNWIND);
    }
    CR(ctxbuf, RAX) = retval;
    CRIP(ctxbuf) = target_ip;
    resume(c, ctxbuf);
}

// ---- unhandled exceptions --------------------------------------------------------------------
static u64 top_filter;
SHIM(SetUnhandledExceptionFilter) { u64 old = top_filter; top_filter = A1; RET(old); }

static void unhandled(Ctx *c, u64 rec, u64 ctx) {
    if (top_filter) {
        u64 ptrs = scratch(); ST64(ptrs, rec); ST64(ptrs + 8, ctx);
        u64 args[1] = { ptrs };
        i32 r = (i32)rt_guest_call(c, top_filter, 1, args);
        if (r == -1) return;  // EXCEPTION_CONTINUE_EXECUTION
    }
    u64 pc = LD64(rec + 16);
    u64 isz, ib = rt_module_base_for(pc, &isz);
    const char *p = ib ? rt_module_path(ib) : NULL;
    rt_log("unhandled exception 0x%08x at 0x%llx (%s+0x%llx)", LD32(rec), (unsigned long long)pc, p ? p : "?", (unsigned long long)(pc - ib));
    static const char *rn[16] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15" };
    for (int i = 0; i < 16; i++) fprintf(stderr, "  %-3s=%016llx%s", rn[i], (unsigned long long)CR(ctx, i), i % 4 == 3 ? "\n" : "");
    // WINTOMAC_PEEK=0x1400..,0x1401..: dump guest u32/u64 values at those addresses (debugging aid)
    const char *peek = getenv("WINTOMAC_PEEK");
    for (const char *q = peek; q && *q; ) {
        char *end; u64 a = strtoull(q, &end, 0); if (end == q) break;
        fprintf(stderr, "  peek 0x%llx: u32=%u (0x%x) u64=0x%llx\n", (unsigned long long)a, LD32(a), LD32(a), (unsigned long long)LD64(a));
        q = *end ? end + 1 : end;
    }
    fflush(NULL);
    _exit(3);
}
SHIM(UnhandledExceptionFilter) { RET(0); }  // EXCEPTION_CONTINUE_SEARCH

static void raise(Ctx *c, u64 rec, u64 ctx) {
    if (trace()) rt_log("raise %08x at %llx", LD32(rec), (unsigned long long)LD64(rec + 16));
    if (dispatch(c, rec, ctx)) { load(c, ctx); c->r[RSP] = CR(ctx, RSP) - 8; return; }  // continue: return to caller
    unhandled(c, rec, ctx);
    load(c, ctx); c->r[RSP] = CR(ctx, RSP) - 8;
}

// Faults detected by recompiled code (integer divide by zero, int3, ud2): raise them as Windows does.
// Registers were synced into Ctx before the trap call; the faulting instruction is `ip`.
void rt_raise_hw(Ctx *c, u64 ip, u32 code) {
    u64 rec = scratch(), ctx = scratch();
    capture(c, ctx, ip, c->r[RSP]);
    ST32(rec, code); ST32(rec + 4, 0); ST64(rec + 16, ip); ST32(rec + 24, 0);
    if (dispatch(c, rec, ctx)) {
        rt_log("exception 0x%08x at 0x%llx: a handler asked to continue execution at the faulting instruction (unsupported)", code, (unsigned long long)ip);
        _exit(3);
    }
    unhandled(c, rec, ctx);
    _exit(3);
}

// ---- shims ------------------------------------------------------------------------------
SHIM(RaiseException) {  // (code, flags, nargs, args*)
    u32 code = (u32)A1;
    if (code == 0x406D1388) RET(0);  // MSVC SetThreadName: only meaningful to debuggers
    u64 rec = scratch(), ctx = scratch();
    capture_caller(c, ctx);
    u32 n = (u32)A3 > 15 ? 15 : (u32)A3;
    ST32(rec, code); ST32(rec + 4, (u32)A2 & EXCEPTION_NONCONTINUABLE); ST64(rec + 8, 0); ST64(rec + 16, CRIP(ctx)); ST32(rec + 24, n);
    for (u32 i = 0; i < n; i++) ST64(rec + 32 + 8 * i, LD64(A4 + 8 * i));
    raise(c, rec, ctx);
}
SHIM(RtlRaiseException) {  // (EXCEPTION_RECORD*)
    u64 ctx = scratch(); capture_caller(c, ctx);
    if (!LD64(A1 + 16)) ST64(A1 + 16, CRIP(ctx));
    raise(c, A1, ctx);
}
SHIM(RtlUnwindEx) {  // (target_frame, target_ip, rec, retval, ctx, history)
    u64 start = scratch(); capture_caller(c, start);
    unwind_ex(c, A1, A2, A3, A4, AN(5), AN(6), start);
}
SHIM(RtlUnwind) {  // (target_frame, target_ip, rec, retval)
    u64 start = scratch(); capture_caller(c, start);
    unwind_ex(c, A1, A2, A3, A4, 0, 0, start);
}
SHIM(RtlRestoreContext) { resume(c, A1); }
SHIM(RtlCaptureContext) { capture_caller(c, A1); RET(0); }
SHIM(RtlLookupFunctionEntry) { u64 base = 0, fe = rt_seh_lookup(A1, &base); if (A2) ST64(A2, fe ? base : 0); RET(fe); }
SHIM(RtlPcToFileHeader) { u64 b = rt_module_base_for(A1, NULL); ST64(A2, b); RET(b); }
// (type, base, pc, fe, ctx, *hdata, *estab, ctxptrs)
SHIM(RtlVirtualUnwind) {
    u64 hdata = 0, estab = 0;
    u64 h = virtual_unwind((u32)A1, A2, A3, A4, AN(5), &hdata, &estab);
    if (AN(6)) ST64(AN(6), hdata);
    if (AN(7)) ST64(AN(7), estab);
    RET(h);
}
SHIM(RtlCaptureStackBackTrace) {  // (skip, count, frames*, hash*)
    u64 ctx = scratch(); capture_caller(c, ctx);
    u32 skip = (u32)A1, cnt = (u32)A2, n = 0;
    for (u32 i = 0; n < cnt && i < 512; i++) {
        u64 pc = CRIP(ctx), base; if (!pc) break;
        if (i >= skip) ST64(A3 + 8 * n++, pc);
        u64 fe = rt_seh_lookup(pc, &base), hd, es;
        if (fe) virtual_unwind(0, base, pc, fe, ctx, &hd, &es); else if (!leaf_unwind(c, ctx)) break;
    }
    if (AN(4)) ST32(AN(4), 0);
    RET(n);
}

// __C_specific_handler (MSVC __try/__except/__finally scope tables), implemented natively.
// SCOPE_TABLE { u32 Count; { u32 Begin, End, Handler, JumpTarget } [Count] } at DispatcherContext->HandlerData.
SHIM(__C_specific_handler) {  // (rec, establisher, ctx, disp)
    u64 rec = A1, estab = A2, disp = A4;
    u64 base = LD64(disp + 8), pc = LD64(disp + 0) - base, scope = LD64(disp + 56);
    u32 n = LD32(scope);
    u32 flags = LD32(rec + 4);
    if (!(flags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))) {
        for (u32 i = LD32(disp + 72); i < n; i++) {
            u64 e = scope + 4 + 16ull * i;
            u32 b = LD32(e), en = LD32(e + 4), h = LD32(e + 8), j = LD32(e + 12);
            if (pc < b || pc >= en || !j) continue;
            i32 verdict = 1;
            if (h != 1) {  // filter funclet: (EXCEPTION_POINTERS*, establisher)
                u64 ptrs = scratch(); ST64(ptrs, rec); ST64(ptrs + 8, A3);
                u64 args[2] = { ptrs, estab };
                verdict = (i32)rt_guest_call(c, base + h, 2, args);
            }
            if (verdict < 0) RET(ExceptionContinueExecution);
            if (verdict > 0) {  // EXCEPTION_EXECUTE_HANDLER: unwind to the __except block
                u64 start = scratch(); capture_caller(c, start);   // our caller is the dispatcher: walk crosses into it
                unwind_ex(c, estab, base + j, rec, LD32(rec), LD64(disp + 40), LD64(disp + 64), start);
            }
        }
        RET(ExceptionContinueSearch);
    }
    u64 target = LD64(disp + 32) - base;
    for (u32 i = LD32(disp + 72); i < n; i++) {
        u64 e = scope + 4 + 16ull * i;
        u32 b = LD32(e), en = LD32(e + 4), h = LD32(e + 8), j = LD32(e + 12);
        if (pc < b || pc >= en) continue;
        if ((flags & EXCEPTION_TARGET_UNWIND) && j && target == j) break;   // reached the __except we unwind to
        if (!j) {  // __finally funclet: (BOOLEAN abnormal_termination, establisher)
            ST32(disp + 72, i + 1);
            u64 args[2] = { 1, estab };
            rt_guest_call(c, base + h, 2, args);
        }
    }
    RET(ExceptionContinueSearch);
}

const Shim shims_seh[] = {
#define E(n) { NULL, #n, sh_##n }
    E(RtlAddFunctionTable), E(RtlDeleteFunctionTable), E(RaiseException), E(RtlRaiseException), E(RtlUnwindEx), E(RtlUnwind), E(RtlRestoreContext), E(RtlCaptureContext),
    E(RtlLookupFunctionEntry), E(RtlPcToFileHeader), E(RtlVirtualUnwind), E(RtlCaptureStackBackTrace),
    E(__C_specific_handler), E(SetUnhandledExceptionFilter), E(UnhandledExceptionFilter),
#undef E
    { 0, 0, 0 }
};
