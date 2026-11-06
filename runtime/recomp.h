// Shared definitions for recompiled code and the runtime.
#pragma once
#include <stdint.h>
#include <string.h>
#include <setjmp.h>

#ifndef RT_TSO
#define RT_TSO 0
#endif

typedef uint8_t u8;   typedef int8_t i8;
typedef uint16_t u16; typedef int16_t i16;
typedef uint32_t u32; typedef int32_t i32;
typedef uint64_t u64; typedef int64_t i64;
typedef unsigned __int128 u128; typedef __int128 i128;

typedef u64 v2u64 __attribute__((vector_size(16)));
typedef u32 v4u32 __attribute__((vector_size(16)));
typedef float v4f32 __attribute__((vector_size(16)));
typedef double v2f64 __attribute__((vector_size(16)));
// whole-register views for the lifter's vector kernels (recomp/vector.py): one NEON instruction per SSE op
typedef u8 v16u8 __attribute__((vector_size(16)));
typedef i8 v16i8 __attribute__((vector_size(16)));
typedef u16 v8u16 __attribute__((vector_size(16)));
typedef i16 v8i16 __attribute__((vector_size(16)));
typedef i32 v4i32 __attribute__((vector_size(16)));
typedef i64 v2i64 __attribute__((vector_size(16)));

typedef union V128 {
    u8 b[16]; u16 w[8]; u32 d[4]; u64 q[2];
    i8 sb[16]; i16 sw[8]; i32 sd[4]; i64 sq[2];
    float f[4]; double fd[2];
    v2u64 vq; v4u32 vd32; v4f32 vf; v2f64 vd;
    v16u8 vb; v16i8 vsb; v8u16 vw; v8i16 vsw; v4i32 vsd; v2i64 vsq;
} V128;

// Full YMM view, used for cross-lane / cross-width ops (permutes, broadcasts, widening converts, gathers).
typedef union V256 {
    V128 h[2];
    u8 b[32]; u16 w[16]; u32 d[8]; u64 q[4];
    i8 sb[32]; i16 sw[16]; i32 sd[8]; i64 sq[4];
    float f[8]; double fd[4];
} V256;

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

struct Ctx;
// Host frame record for a recompiled function that has an exception handler. Unwinding to a frame of that
// function longjmps here; the function reloads registers from Ctx and jumps to the resume label.
typedef struct RtEhFrame {
    struct RtEhFrame *prev; struct Ctx *c;
    u64 fn, entry_rsp, target;
    int seh_level;       // handler nesting depth when the frame was entered (restored on resume)
    jmp_buf jb;
} RtEhFrame;

typedef struct Ctx {
    u64 r[16];
    V128 x[16];
    V128 xh[16];   // upper 128 bits of ymm0-15
    u64 gs_base;   // -> TEB
    u64 fs_base;
    u32 mxcsr;
    u32 ftop;      // x87 stack top (0..7)
    double st[8];  // x87 registers, held as host doubles (ARM has no 80-bit float)
    u16 fsw, fcw;
    u64 resume_target;   // != 0: the callee must start at this address (secondary entry / EH resume)
    RtEhFrame *eh_top;   // innermost active exception-handling frame record (per thread)
    int seh_level;       // how many exception handlers are currently executing on this thread
} Ctx;

typedef void (*GuestFn)(Ctx *);
typedef struct RtFunc_ { u64 addr; GuestFn fn; } RtFunc;
typedef struct { u64 iat; const char *dll; const char *name; u16 ordinal; } RtImport;
typedef struct {
    const char *name, *path;      // file name, and where it was when recompiled
    u64 base, orig_base, size, entry;
    u32 is_dll, timestamp;
    const RtImport *imports; u64 nimports;
    const u64 *tls; u64 ntls;                       // TLS callbacks
    u64 tls_start, tls_end, tls_index; u32 tls_zero;  // static TLS template (IMAGE_TLS_DIRECTORY64), VAs
    const u32 *synth_relocs; u64 nsynth;            // DIR64 fixups recovered by the recompiler (image had no .reloc)
} RtImage;

// ---- guest memory: identity mapped, unaligned-safe -------------------------
#define RT_INLINE static inline __attribute__((always_inline))
// Plain (unordered) accessors. Also used directly for stack accesses (thread-private) via the S variants.
RT_INLINE u8  LDS8 (u64 a) { u8  v; memcpy(&v, (void *)a, 1); return v; }
RT_INLINE u16 LDS16(u64 a) { u16 v; memcpy(&v, (void *)a, 2); return v; }
RT_INLINE u32 LDS32(u64 a) { u32 v; memcpy(&v, (void *)a, 4); return v; }
RT_INLINE u64 LDS64(u64 a) { u64 v; memcpy(&v, (void *)a, 8); return v; }
RT_INLINE void STS8 (u64 a, u8  v) { memcpy((void *)a, &v, 1); }
RT_INLINE void STS16(u64 a, u16 v) { memcpy((void *)a, &v, 2); }
RT_INLINE void STS32(u64 a, u32 v) { memcpy((void *)a, &v, 4); }
RT_INLINE void STS64(u64 a, u64 v) { memcpy((void *)a, &v, 8); }

#if RT_TSO
// x86 is TSO: loads are not reordered with older loads, stores not with older stores. ARM is weaker, so
// shared-memory accesses become acquire loads / release stores (LDAPR/STLR on Apple silicon), which is
// exactly TSO (store->load reordering stays allowed). Misaligned accesses can't be single-copy atomic on
// ARM; they fall back to plain accesses (x86 code that relies on those being ordered is vanishingly rare).
#define RT_ACQ(T, n) RT_INLINE T LD##n(u64 a) { if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) return __atomic_load_n((T *)a, __ATOMIC_ACQUIRE); return LDS##n(a); }
#define RT_REL(T, n) RT_INLINE void ST##n(u64 a, T v) { if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) __atomic_store_n((T *)a, v, __ATOMIC_RELEASE); else STS##n(a, v); }
RT_ACQ(u8, 8) RT_ACQ(u16, 16) RT_ACQ(u32, 32) RT_ACQ(u64, 64)
RT_REL(u8, 8) RT_REL(u16, 16) RT_REL(u32, 32) RT_REL(u64, 64)
#else
// No ordering, but still relaxed atomics, never plain memcpy: otherwise clang may hoist a guest spin-wait's
// load out of its loop (it can't see other threads), which hangs the program. Same ldr/str machine code.
#define RT_RLX_LD(T, n) RT_INLINE T LD##n(u64 a) { if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) return __atomic_load_n((T *)a, __ATOMIC_RELAXED); return LDS##n(a); }
#define RT_RLX_ST(T, n) RT_INLINE void ST##n(u64 a, T v) { if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) __atomic_store_n((T *)a, v, __ATOMIC_RELAXED); else STS##n(a, v); }
RT_RLX_LD(u8, 8) RT_RLX_LD(u16, 16) RT_RLX_LD(u32, 32) RT_RLX_LD(u64, 64)
RT_RLX_ST(u8, 8) RT_RLX_ST(u16, 16) RT_RLX_ST(u32, 32) RT_RLX_ST(u64, 64)
#endif
RT_INLINE V128 LDX(u64 a) { V128 v; memcpy(&v, (void *)a, 16); return v; }
RT_INLINE V128 LDV8(u64 a) { V128 v = {0}; memcpy(&v, (void *)a, 1); return v; }
RT_INLINE V128 LDV16(u64 a) { V128 v = {0}; memcpy(&v, (void *)a, 2); return v; }
RT_INLINE V128 LDV32(u64 a) { V128 v = {0}; memcpy(&v, (void *)a, 4); return v; }
RT_INLINE V128 LDV64(u64 a) { V128 v = {0}; memcpy(&v, (void *)a, 8); return v; }
RT_INLINE float LDF32(u64 a) { float v; memcpy(&v, (void *)a, 4); return v; }
RT_INLINE double LDF64(u64 a) { double v; memcpy(&v, (void *)a, 8); return v; }
RT_INLINE void STX (u64 a, V128 v) { memcpy((void *)a, &v, 16); }

RT_INLINE i8  SAT_I8 (i32 v) { return v > 127 ? 127 : v < -128 ? -128 : (i8)v; }
RT_INLINE u8  SAT_U8 (i32 v) { return v > 255 ? 255 : v < 0 ? 0 : (u8)v; }
RT_INLINE i16 SAT_I16(i32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (i16)v; }
RT_INLINE u16 SAT_U16(i32 v) { return v > 65535 ? 65535 : v < 0 ? 0 : (u16)v; }
RT_INLINE u64 PDEP64(u64 src, u64 mask) { u64 r = 0; for (u64 bb = 1; mask; bb += bb) { if (src & bb) r |= mask & -mask; mask &= mask - 1; } return r; }
RT_INLINE u64 PEXT64(u64 src, u64 mask) { u64 r = 0; for (u64 bb = 1; mask; bb += bb) { if (src & mask & -mask) r |= bb; mask &= mask - 1; } return r; }
RT_INLINE float  RNDF(float v, int m)  { switch (m & 3) { case 0: return __builtin_rintf(v); case 1: return __builtin_floorf(v); case 2: return __builtin_ceilf(v); default: return __builtin_truncf(v); } }
RT_INLINE double RNDD(double v, int m) { switch (m & 3) { case 0: return __builtin_rint(v);  case 1: return __builtin_floor(v);  case 2: return __builtin_ceil(v);  default: return __builtin_trunc(v); } }

// SSE4.2 PCMP{E,I}STR{I,M}. la/lb < 0: implicit (null-terminated) lengths. Flags out: CF ZF SF OF.
static inline V128 rt_pcmpstr(V128 a, V128 b, i64 la, i64 lb, int imm, int want_index, u8 fl[4]) {
    int words = imm & 1, sgn = (imm >> 1) & 1, agg = (imm >> 2) & 3, pol = (imm >> 4) & 3, msb = (imm >> 6) & 1;
    int n = words ? 8 : 16;
    #define EL(v, i) (words ? (sgn ? (i64)v.sw[i] : (i64)v.w[i]) : (sgn ? (i64)v.sb[i] : (i64)v.b[i]))
    if (la < 0) { la = n; for (int i = 0; i < n; i++) if (EL(a, i) == 0) { la = i; break; } }
    else { if (la > n) la = n; }
    if (lb < 0) { lb = n; for (int i = 0; i < n; i++) if (EL(b, i) == 0) { lb = i; break; } }
    else { if (lb > n) lb = n; }
    u32 r1 = 0;
    for (int j = 0; j < n; j++) {
        int bit = 0;
        if (agg == 0) { if (j < lb) for (int i = 0; i < la; i++) if (EL(a, i) == EL(b, j)) { bit = 1; break; } }
        else if (agg == 1) { if (j < lb) for (int i = 0; i + 1 < la; i += 2) if (EL(a, i) <= EL(b, j) && EL(b, j) <= EL(a, i + 1)) { bit = 1; break; } }
        else if (agg == 2) { bit = (j < la && j < lb) ? EL(a, j) == EL(b, j) : (j >= la && j >= lb); }
        else {
            bit = 1;
            for (int k = 0; k < n - j && bit; k++) {
                if (k >= la) break;                      // a exhausted: matched
                if (j + k >= lb) { bit = 0; break; }      // b exhausted first
                if (EL(a, k) != EL(b, j + k)) bit = 0;
            }
        }
        r1 |= (u32)bit << j;
    }
    #undef EL
    u32 all = (1u << n) - 1, r2 = r1;
    if (pol == 1) r2 = ~r1 & all;
    else if (pol == 3) r2 = (r1 ^ ((1u << lb) - 1)) & all;
    fl[0] = r2 != 0; fl[1] = lb < n; fl[2] = la < n; fl[3] = r2 & 1;
    V128 out = {0};
    if (want_index) out.d[0] = r2 ? (msb ? 31 - __builtin_clz(r2) : __builtin_ctz(r2)) : (u32)n;
    else if (!msb) out.d[0] = r2;
    else for (int j = 0; j < n; j++) { if (words) out.w[j] = (r2 >> j & 1) ? 0xFFFF : 0; else out.b[j] = (r2 >> j & 1) ? 0xFF : 0; }
    return out;
}

RT_INLINE u8 PAR(u64 v) { return !__builtin_parity((u32)(u8)v); }

// x86 float->int conversions: out-of-range / NaN produce the "integer indefinite" value.
RT_INLINE i32 CVTTS2I32(float f)  { return (f >= -2147483648.0f && f < 2147483648.0f) ? (i32)f : INT32_MIN; }
RT_INLINE i64 CVTTS2I64(float f)  { return (f >= -9223372036854775808.0f && f < 9223372036854775808.0f) ? (i64)f : INT64_MIN; }
RT_INLINE i32 CVTTD2I32(double d) { return (d > -2147483649.0 && d < 2147483648.0) ? (i32)d : INT32_MIN; }
RT_INLINE i64 CVTTD2I64(double d) { return (d >= -9223372036854775808.0 && d < 9223372036854775808.0) ? (i64)d : INT64_MIN; }
RT_INLINE i32 CVTS2I32(float f)   { return CVTTS2I32(__builtin_rintf(f)); }
RT_INLINE i64 CVTS2I64(float f)   { return CVTTS2I64(__builtin_rintf(f)); }
RT_INLINE i32 CVTD2I32(double d)  { return CVTTD2I32(__builtin_rint(d)); }
RT_INLINE i64 CVTD2I64(double d)  { return CVTTD2I64(__builtin_rint(d)); }

// ---- x87 ------------------------------------------------------------------
#define ST_(c, i) ((c)->st[((c)->ftop + (i)) & 7])
RT_INLINE void FPUSH(Ctx *c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
RT_INLINE double FPOP(Ctx *c) { double v = c->st[c->ftop]; c->ftop = (c->ftop + 1) & 7; return v; }
// 80-bit extended <-> double (games/CRTs inspect the bytes, e.g. mingw printf's mantissa extraction)
RT_INLINE double LDF80(u64 a) {
    u64 m = LD64(a); u16 se = LD16(a + 8); int e = se & 0x7fff; int s = se >> 15;
    double r;
    if (e == 0 && m == 0) r = 0.0;
    else if (e == 0x7fff) r = (m << 1) ? __builtin_nan("") : __builtin_inf();
    else r = __builtin_ldexp((double)m, e - 16383 - 63);
    return s ? -r : r;
}
RT_INLINE void STF80(u64 a, double d) {
    u64 bits; memcpy(&bits, &d, 8);
    int s = bits >> 63, e = (bits >> 52) & 0x7ff; u64 f = bits & ((1ull << 52) - 1);
    u64 m; int e80;
    if (e == 0 && f == 0) { m = 0; e80 = 0; }
    else if (e == 0x7ff) { m = (1ull << 63) | (f << 11); e80 = 0x7fff; }
    else if (e == 0) { int sh = __builtin_clzll(f) - 11; m = (f << (11 + sh)); e80 = 1 - 1023 - sh + 16383; }  // denormal double -> normal f80
    else { m = (1ull << 63) | (f << 11); e80 = e - 1023 + 16383; }
    ST64(a, m); ST16(a + 8, (u16)(e80 | (s << 15)));
}

RT_INLINE void rt_eh_push(RtEhFrame *f, Ctx *c, u64 fn, u64 rsp) { f->prev = c->eh_top; f->c = c; f->fn = fn; f->entry_rsp = rsp; f->target = 0; f->seh_level = c->seh_level; c->eh_top = f; }
RT_INLINE void rt_eh_pop(RtEhFrame *f) { f->c->eh_top = f->prev; }
void rt_resume_elsewhere(Ctx *c, u64 target, u64 self);  // enter the chunk that owns target, then return

// ---- runtime services called from recompiled code ------------------------
void rt_call(Ctx *c, u64 target);   // return address already pushed by caller
// Indirect call with a per-call-site inline cache: `ic` remembers the last recompiled function the site called
// (an immutable rt_funcs[] entry, so one pointer load is race-free). Code made executable inside a recompiled
// image (rt_interp_exec_in_images) must go through rt_call's interpreter check, so the fast path stops then.
extern int rt_interp_exec_in_images;
void rt_call_ic(Ctx *c, u64 target, const struct RtFunc_ **ic);
void rt_jump(Ctx *c, u64 target);   // tail transfer (no push)
// rt_unimplemented / rt_fell_off return: the interpreter (runtime/interp.c) runs the rest of the function from ip.
void rt_unimplemented(Ctx *c, u64 ip, const char *what, u64 rflags);   // rflags: live arithmetic flags
void rt_trap(Ctx *c, u64 ip, const char *what) __attribute__((noreturn));
void rt_fell_off(Ctx *c, u64 ip, u64 rflags);
void rt_cpuid(Ctx *c);
u64  rt_rdtsc(void);
u64  rt_random64(void);
u32  rt_crc32c(u32 crc, u64 v, int nbytes);

// ---- emitted by the recompiler -------------------------------------------
extern const u64 rt_image_base, rt_image_size, rt_nfuncs, rt_nimages;   // rt_image_*: the main program
extern const u32 rt_subsystem;
extern const RtFunc rt_funcs[];                                          // all images, sorted by address
extern const RtImage rt_images[];                                        // [0] = the program

RT_INLINE void rt_icall(Ctx *c, u64 target, const RtFunc **ic) {
    const RtFunc *e = __atomic_load_n(ic, __ATOMIC_RELAXED);
    if (__builtin_expect(e != 0 && e->addr == target && !rt_interp_exec_in_images, 1)) e->fn(c);
    else rt_call_ic(c, target, ic);
}
