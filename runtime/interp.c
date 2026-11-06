// wintomac runtime fallback: x86-64 interpreter (integer ISA, control flow, interop with recompiled code).
//
// Decoding is done by Zydis (MIT, zyantific). Instruction semantics follow the Intel SDM and the static
// lifter (recomp/lifter.py) so interpreted and recompiled code agree; box64 (MIT, ptitSeb) was the reference
// for the trickier corners (flag rules of shifts/rotates, string ops, cmpxchg).
//
// Interop model (mirrors the generated code's calling convention):
//  * Recompiled code reaches us through rt_call/rt_jump with the return address already on the guest stack.
//    We remember rsp at entry; the guest `ret` that pops above it is the one that returns to our host caller,
//    so we return host-side, exactly like a recompiled function's `ret` does.
//  * Guest calls made by interpreted code push the return address as usual. If the target is an import thunk
//    or a recompiled function entry we call it natively (rt_try_native); its `ret` pops the address and returns
//    host-side, and we continue at the fall-through. Otherwise we just continue interpreting at the target.
//  * A `jmp` to native code is a tail call: the native callee pops the return address that was at [rsp];
//    afterwards we behave like a `ret` to that address (host return if it was our caller's).
#include "interp_impl.h"
#include "jit.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

IReg rt_ireg[ZYDIS_REGISTER_MAX_VALUE + 1];
static ZydisDecoder decoder;
static ZydisFormatter formatter;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void init_tables(void) {
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    for (int i = 0; i < 16; i++) {
        rt_ireg[ZYDIS_REGISTER_RAX + i] = (IReg){RK_GPR, i, 0, 64};
        rt_ireg[ZYDIS_REGISTER_EAX + i] = (IReg){RK_GPR, i, 0, 32};
        rt_ireg[ZYDIS_REGISTER_AX + i] = (IReg){RK_GPR, i, 0, 16};
        rt_ireg[ZYDIS_REGISTER_XMM0 + i] = (IReg){RK_XMM, i, 0, 128};
        rt_ireg[ZYDIS_REGISTER_YMM0 + i] = (IReg){RK_YMM, i, 0, 256};
    }
    for (int i = 0; i < 4; i++) {
        rt_ireg[ZYDIS_REGISTER_AL + i] = (IReg){RK_GPR, i, 0, 8};
        rt_ireg[ZYDIS_REGISTER_AH + i] = (IReg){RK_GPR, i, 1, 8};
        rt_ireg[ZYDIS_REGISTER_SPL + i] = (IReg){RK_GPR, 4 + i, 0, 8};
    }
    for (int i = 0; i < 8; i++) {
        rt_ireg[ZYDIS_REGISTER_R8B + i] = (IReg){RK_GPR, 8 + i, 0, 8};
        rt_ireg[ZYDIS_REGISTER_ST0 + i] = (IReg){RK_ST, i, 0, 80};
        rt_ireg[ZYDIS_REGISTER_MM0 + i] = (IReg){RK_MMX, i, 0, 64};
    }
    rt_ireg[ZYDIS_REGISTER_RIP] = (IReg){RK_RIP, 0, 0, 64};
}

// ---- executable regions (VirtualAlloc/VirtualProtect with PAGE_EXECUTE*) ---------------------------------
#define MAX_REGIONS 1024
static struct { u64 lo, hi; } regions[MAX_REGIONS];
static int nregions;
static pthread_mutex_t region_lock = PTHREAD_MUTEX_INITIALIZER;
int rt_interp_exec_in_images;

void rt_interp_mark_exec(u64 addr, u64 size) {
    if (!size) return;
    u64 lo = addr & ~0xFFFull, hi = (addr + size + 0xFFF) & ~0xFFFull;
    pthread_mutex_lock(&region_lock);
    int slot = -1;
    for (int i = 0; i < nregions; i++) {
        if (regions[i].lo <= lo && hi <= regions[i].hi) { pthread_mutex_unlock(&region_lock); return; }
        if (slot < 0 && regions[i].lo == regions[i].hi) slot = i;
    }
    if (slot < 0 && nregions < MAX_REGIONS) slot = nregions;
    if (slot >= 0) {
        regions[slot].lo = lo; regions[slot].hi = hi;
        if (slot == nregions) __atomic_store_n(&nregions, nregions + 1, __ATOMIC_RELEASE);
    }
    u64 isz;
    if (rt_module_base_for(lo, &isz) || rt_module_base_for(hi - 1, &isz)) rt_interp_exec_in_images = 1;
    pthread_mutex_unlock(&region_lock);
    if (rt_trace_enabled()) rt_log("interp: executable region %llx-%llx", (unsigned long long)lo, (unsigned long long)hi);
}

void rt_jit_unmap(u64 addr, u64 size);
void rt_interp_unmark_exec(u64 addr, u64 size) {
    u64 lo = addr & ~0xFFFull, hi = size ? (addr + size + 0xFFF) & ~0xFFFull : lo + 1;
    u64 gone[16][2]; int ng = 0;
    pthread_mutex_lock(&region_lock);
    for (int i = 0; i < nregions; i++)
        if (regions[i].lo < hi && lo < regions[i].hi && (size || regions[i].lo == lo)) {
            if (ng < 16) { gone[ng][0] = regions[i].lo; gone[ng][1] = regions[i].hi; ng++; }
            regions[i].lo = regions[i].hi = 0;
        }
    pthread_mutex_unlock(&region_lock);
    if (size) rt_jit_unmap(lo, hi - lo);
    else for (int i = 0; i < ng; i++) rt_jit_unmap(gone[i][0], gone[i][1] - gone[i][0]);
}

int rt_interp_is_exec(u64 a) {
    int n = __atomic_load_n(&nregions, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) if (a >= regions[i].lo && a < regions[i].hi) return 1;
    return 0;
}

// ---- decode cache ------------------------------------------------------------------------------------
// Per thread, direct mapped, validated against the instruction's current bytes: self-modifying code and
// reused JIT buffers are re-decoded automatically.
#define DC_SIZE 1024
typedef struct { u64 pc; u8 len, branch, bytes[15]; ZydisDecodedInstruction in; ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT]; } DEnt;
static pthread_key_t dc_key;
static __thread DEnt *dcache;
static void dc_free(void *p) { free(p); }
static void dc_key_init(void) { pthread_key_create(&dc_key, dc_free); }
static pthread_once_t dc_once = PTHREAD_ONCE_INIT;

static const DEnt *decode(u64 pc) {
    if (__builtin_expect(!dcache, 0)) {
        pthread_once(&dc_once, dc_key_init);
        dcache = calloc(DC_SIZE, sizeof(DEnt));
        pthread_setspecific(dc_key, dcache);
    }
    DEnt *e = &dcache[(pc ^ (pc >> 10)) & (DC_SIZE - 1)];
    if (e->pc == pc && e->len && !memcmp(e->bytes, (const void *)pc, e->len)) return e;
    if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, (const void *)pc, 15, &e->in, e->op))) { e->pc = 0; e->len = 0; return NULL; }
    e->pc = pc; e->len = e->in.length; memcpy(e->bytes, (const void *)pc, e->len);
    ZydisInstructionCategory cat = e->in.meta.category;
    e->branch = cat == ZYDIS_CATEGORY_COND_BR || cat == ZYDIS_CATEGORY_UNCOND_BR || cat == ZYDIS_CATEGORY_CALL || cat == ZYDIS_CATEGORY_RET;
    return e;
}

const char *rt_interp_disasm(u64 pc, char *buf, size_t n) {
    pthread_once(&init_once, init_tables);
    ZydisDecodedInstruction in; ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];
    if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, (const void *)pc, 15, &in, op))) { snprintf(buf, n, "(undecodable)"); return buf; }
    ZydisFormatterFormatInstruction(&formatter, &in, op, in.operand_count_visible, buf, n, pc, NULL);
    return buf;
}

// ---- statistics (WINTOMAC_INTERP_TRACE) -----------------------------------------------------------------
// Counts interpreter entries and executed instructions per entry address and keeps wintomac_interp.txt up to
// date (hottest first, --seeds-file format; runtime-generated code is commented out since it can't be
// recompiled ahead of time). Rewritten when a new address shows up and at most once a second otherwise,
// because the runtime leaves through _exit (ExitProcess) and atexit handlers never run.
static int trace_mode = -1;
#define ST_SLOTS 4096
typedef struct { u64 pc, entries, insns; u8 jit; } Stat;   // jit: 1 runtime-generated code, 2 lifter fallback site
static int is_fallback_site(u64 ip);
static Stat stats[ST_SLOTS];
static u64 stats_last_dump;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static int stat_cmp(const void *a, const void *b) {
    const Stat *x = a, *y = b;
    return x->insns < y->insns ? 1 : x->insns > y->insns ? -1 : (x->pc > y->pc) - (x->pc < y->pc);
}
static void stats_dump_locked(void) {
    static Stat sorted[ST_SLOTS];
    int n = 0;
    for (int i = 0; i < ST_SLOTS; i++) if (stats[i].pc) sorted[n++] = stats[i];
    qsort(sorted, n, sizeof sorted[0], stat_cmp);
    FILE *f = fopen("wintomac_interp.txt.tmp", "w");
    if (!f) return;
    fprintf(f, "# wintomac interpreter profile: entry address, times entered, guest instructions interpreted\n");
    fprintf(f, "# feed back to the recompiler: python -m recomp ... --seeds-file wintomac_interp.txt\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "%s0x%llx  # entries=%llu insns=%llu%s\n", sorted[i].jit ? "# " : "", (unsigned long long)sorted[i].pc,
                (unsigned long long)sorted[i].entries, (unsigned long long)sorted[i].insns,
                sorted[i].jit == 1 ? " (runtime-generated code)" : sorted[i].jit == 2 ? " (recompiled code continued here: instruction the lifter can't translate)" : "");
    fclose(f);
    rename("wintomac_interp.txt.tmp", "wintomac_interp.txt");
}
static void stats_dump(void) { pthread_mutex_lock(&stats_lock); stats_dump_locked(); pthread_mutex_unlock(&stats_lock); }
static void stats_add(u64 pc, u64 insns) {
    pthread_mutex_lock(&stats_lock);
    u64 h = (pc * 0x9E3779B97F4A7C15ull) >> 52;
    int fresh = 0;
    for (int k = 0; k < ST_SLOTS; k++) {
        Stat *e = &stats[(h + k) & (ST_SLOTS - 1)];
        if (e->pc == pc || !e->pc) {
            if (!e->pc) { e->pc = pc; e->jit = rt_interp_is_exec(pc) ? 1 : is_fallback_site(pc) ? 2 : 0; fresh = 1; }
            e->entries++; e->insns += insns; break;
        }
    }
    u64 now = rt_rdtsc();   // mach_absolute_time: 24 MHz ticks on Apple silicon
    if (fresh || now - stats_last_dump > 24000000ull) { stats_last_dump = now; stats_dump_locked(); }
    pthread_mutex_unlock(&stats_lock);
}

// ---- helpers -----------------------------------------------------------------------------------------------
static inline int cond(IState *s, int cc) {
    int r;
    switch (cc >> 1) {
    case 0: r = s->of; break;
    case 1: r = s->cf; break;
    case 2: r = s->zf; break;
    case 3: r = s->cf | s->zf; break;
    case 4: r = s->sf; break;
    case 5: r = s->pf; break;
    case 6: r = s->sf != s->of; break;
    default: r = s->zf || s->sf != s->of; break;
    }
    return (cc & 1) ? !r : r;
}

static inline int icas(u64 a, int w, u64 *expected, u64 desired) {
    switch (w) {
    case 8:  { u8 e = (u8)*expected;  int ok = __atomic_compare_exchange_n((u8 *)a, &e, (u8)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    case 16: { u16 e = (u16)*expected; int ok = __atomic_compare_exchange_n((u16 *)a, &e, (u16)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    case 32: { u32 e = (u32)*expected; int ok = __atomic_compare_exchange_n((u32 *)a, &e, (u32)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    default: { u64 e = *expected; int ok = __atomic_compare_exchange_n((u64 *)a, &e, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    }
}
static inline u64 ixchg(u64 a, int w, u64 v) {
    switch (w) {
    case 8: return __atomic_exchange_n((u8 *)a, (u8)v, __ATOMIC_SEQ_CST);
    case 16: return __atomic_exchange_n((u16 *)a, (u16)v, __ATOMIC_SEQ_CST);
    case 32: return __atomic_exchange_n((u32 *)a, (u32)v, __ATOMIC_SEQ_CST);
    default: return __atomic_exchange_n((u64 *)a, v, __ATOMIC_SEQ_CST);
    }
}

static u64 alu(IState *s, ZydisMnemonic mn, u64 a, u64 b, int w) {
    u64 m = imask(w), r, sign = 1ull << (w - 1);
    a &= m; b &= m;
    switch (mn) {
    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_XADD: {
        u64 cin = mn == ZYDIS_MNEMONIC_ADC ? s->cf : 0;
        u128 t = (u128)a + b + cin; r = (u64)t & m;
        s->cf = (u8)((t >> w) & 1); s->of = (((a ^ r) & (b ^ r)) & sign) != 0; s->af = ((a ^ b ^ r) >> 4) & 1;
        break; }
    case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_CMP: {
        u64 cin = mn == ZYDIS_MNEMONIC_SBB ? s->cf : 0;
        r = (a - b - cin) & m;
        s->cf = (u128)a < (u128)b + cin; s->of = (((a ^ b) & (a ^ r)) & sign) != 0; s->af = ((a ^ b ^ r) >> 4) & 1;
        break; }
    case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_TEST: r = a & b; s->cf = s->of = s->af = 0; break;
    case ZYDIS_MNEMONIC_OR: r = a | b; s->cf = s->of = s->af = 0; break;
    case ZYDIS_MNEMONIC_XOR: r = a ^ b; s->cf = s->of = s->af = 0; break;
    case ZYDIS_MNEMONIC_INC: r = (a + 1) & m; s->of = r == sign; s->af = ((a ^ r) >> 4) & 1; break;
    case ZYDIS_MNEMONIC_DEC: r = (a - 1) & m; s->of = a == sign; s->af = ((a ^ r) >> 4) & 1; break;
    case ZYDIS_MNEMONIC_NEG: r = (0 - a) & m; s->cf = a != 0; s->of = a == sign; s->af = ((a ^ r) >> 4) & 1; break;
    case ZYDIS_MNEMONIC_NOT: return ~a & m;
    default: return a;
    }
    iszp(s, r, w);
    return r;
}

static u64 shift(IState *s, ZydisMnemonic mn, u64 a, u64 cnt, int w) {
    u64 m = imask(w); a &= m;
    cnt &= w == 64 ? 63 : 31;
    if (!cnt) return a;
    u64 r = a;
    switch (mn) {
    case ZYDIS_MNEMONIC_SHL:
        r = cnt < (u64)w ? (a << cnt) & m : 0;
        s->cf = cnt <= (u64)w ? (a >> (w - cnt)) & 1 : 0;
        s->of = ((r >> (w - 1)) & 1) ^ s->cf; s->af = 0; iszp(s, r, w); break;
    case ZYDIS_MNEMONIC_SHR:
        r = cnt < (u64)w ? a >> cnt : 0;
        s->cf = cnt <= (u64)w ? (a >> (cnt - 1)) & 1 : 0;
        s->of = (a >> (w - 1)) & 1; s->af = 0; iszp(s, r, w); break;
    case ZYDIS_MNEMONIC_SAR: {
        i64 sa = (i64)sext(a, w);
        r = (u64)(sa >> (cnt < 63 ? cnt : 63)) & m;
        s->cf = (sa >> (cnt - 1 < 63 ? cnt - 1 : 63)) & 1;
        s->of = 0; s->af = 0; iszp(s, r, w); break; }
    case ZYDIS_MNEMONIC_ROL: {
        u64 k = cnt % w;
        r = k ? ((a << k) | (a >> (w - k))) & m : a;
        s->cf = r & 1; s->of = ((r >> (w - 1)) & 1) ^ s->cf; break; }
    case ZYDIS_MNEMONIC_ROR: {
        u64 k = cnt % w;
        r = k ? ((a >> k) | (a << (w - k))) & m : a;
        s->cf = (r >> (w - 1)) & 1; s->of = s->cf ^ ((r >> (w - 2)) & 1); break; }
    case ZYDIS_MNEMONIC_RCL: {
        u64 k = w == 8 ? cnt % 9 : w == 16 ? cnt % 17 : cnt;
        for (u64 i = 0; i < k; i++) { u8 out = (r >> (w - 1)) & 1; r = ((r << 1) | s->cf) & m; s->cf = out; }
        s->of = ((r >> (w - 1)) & 1) ^ s->cf; break; }
    case ZYDIS_MNEMONIC_RCR: {
        u64 k = w == 8 ? cnt % 9 : w == 16 ? cnt % 17 : cnt;
        s->of = ((r >> (w - 1)) & 1) ^ s->cf;
        for (u64 i = 0; i < k; i++) { u8 out = r & 1; r = (r >> 1) | ((u64)s->cf << (w - 1)); s->cf = out; }
        break; }
    default: break;
    }
    return r;
}

static inline void wr_gw(Ctx *c, int i, int w, u64 v) {
    if (w == 64) c->r[i] = v; else if (w == 32) c->r[i] = (u32)v; else if (w == 16) c->r[i] = (c->r[i] & ~0xFFFFull) | (u16)v;
    else c->r[i] = (c->r[i] & ~0xFFull) | (u8)v;
}
static inline void push64(Ctx *c, u64 v) { c->r[RSP] -= 8; STS64(c->r[RSP], v); }
static inline u64 pop64(Ctx *c) { u64 v = LDS64(c->r[RSP]); c->r[RSP] += 8; return v; }

static u64 get_rflags(IState *s) {
    return (u64)s->cf | 2 | ((u64)s->pf << 2) | ((u64)s->af << 4) | ((u64)s->zf << 6) | ((u64)s->sf << 7) | (1u << 9) |
           ((u64)s->df << 10) | ((u64)s->of << 11);
}
static void set_rflags(IState *s, u64 f) {
    s->cf = f & 1; s->pf = (f >> 2) & 1; s->af = (f >> 4) & 1; s->zf = (f >> 6) & 1; s->sf = (f >> 7) & 1;
    s->df = (f >> 10) & 1; s->of = (f >> 11) & 1;
}
u64 rt_interp_get_rflags(IState *s) { return get_rflags(s); }
void rt_interp_set_rflags(IState *s, u64 f) {   // arithmetic flags only (DF is kept)
    s->cf = f & 1; s->pf = (f >> 2) & 1; s->af = (f >> 4) & 1; s->zf = (f >> 6) & 1; s->sf = (f >> 7) & 1; s->of = (f >> 11) & 1;
}

// ---- control transfer ------------------------------------------------------------------------------------
static int do_ret(IState *s, u64 extra) {
    Ctx *c = s->c;
    u64 ra = LDS64(c->r[RSP]);
    c->r[RSP] += 8 + extra;
    if (c->r[RSP] > s->entry_rsp) return X_HOST_RET;   // the return address our host caller pushed
    s->pc = ra;
    return X_NEXT;
}

static int do_jump(IState *s, u64 t) {
    Ctx *c = s->c;
    if (!rt_interp_is_exec(t)) {
        u64 ra = LDS64(c->r[RSP]);
        if (rt_try_native(c, t)) {     // tail call: the native callee's ret popped [rsp]
            if (c->r[RSP] > s->entry_rsp) return X_HOST_RET;
            s->pc = ra;
            return X_NEXT;
        }
    }
    s->pc = t;
    return X_NEXT;
}

static int do_call(IState *s, u64 t) {
    Ctx *c = s->c;
    push64(c, s->next);
    if (!rt_interp_is_exec(t) && rt_try_native(c, t)) { s->pc = s->next; return X_NEXT; }
    s->pc = t;
    return X_NEXT;
}

static inline u64 branch_target(IState *s, int i) {
    const ZydisDecodedOperand *o = IOP(s, i);
    if (o->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) return s->next + (u64)o->imm.value.s;
    return rdi(s, i);
}

// ---- string instructions ------------------------------------------------------------------------------------
static int string_op(IState *s) {
    Ctx *c = s->c;
    const ZydisDecodedInstruction *in = s->in;
    int n;
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_CMPSB: n = 1; break;
    case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_CMPSW: n = 2; break;
    case ZYDIS_MNEMONIC_MOVSD: case ZYDIS_MNEMONIC_STOSD: case ZYDIS_MNEMONIC_LODSD: case ZYDIS_MNEMONIC_SCASD: case ZYDIS_MNEMONIC_CMPSD: n = 4; break;
    default: n = 8; break;
    }
    int w = n * 8;
    int rep = (in->attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE)) != 0;
    int repne = (in->attributes & ZYDIS_ATTRIB_HAS_REPNE) != 0;
    i64 step = s->df ? -n : n;
    int a32 = in->address_width == 32;
    #define AREG(R_) (a32 ? (u64)(u32)c->r[R_] : c->r[R_])
    #define AADV(R_) (c->r[R_] = a32 ? (u32)(c->r[R_] + step) : c->r[R_] + step)
    #define CNT() (a32 ? (u64)(u32)c->r[RCX] : c->r[RCX])
    #define DECCNT() (c->r[RCX] = a32 ? (u32)(c->r[RCX] - 1) : c->r[RCX] - 1)
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD: case ZYDIS_MNEMONIC_MOVSQ:
        if (rep) {
            u64 cnt = CNT(), d = AREG(RDI), sr = AREG(RSI), bytes = cnt * n;
            if (cnt && !s->df && !a32 && (d + bytes <= sr || sr + bytes <= d || d <= sr)) {
                memmove((void *)d, (void *)sr, bytes);
                c->r[RDI] += bytes; c->r[RSI] += bytes; c->r[RCX] = 0;
            } else for (; CNT(); DECCNT()) { ist(AREG(RDI), w, ild(AREG(RSI), w)); AADV(RDI); AADV(RSI); }
        } else { ist(AREG(RDI), w, ild(AREG(RSI), w)); AADV(RDI); AADV(RSI); }
        break;
    case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD: case ZYDIS_MNEMONIC_STOSQ: {
        u64 v = c->r[RAX] & imask(w);
        if (rep) {
            u64 cnt = CNT(), d = AREG(RDI);
            if (cnt && n == 1 && !s->df && !a32) { memset((void *)d, (int)v, cnt); c->r[RDI] += cnt; c->r[RCX] = 0; }
            else for (; CNT(); DECCNT()) { ist(AREG(RDI), w, v); AADV(RDI); }
        } else { ist(AREG(RDI), w, v); AADV(RDI); }
        break; }
    case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD: case ZYDIS_MNEMONIC_LODSQ: {
        static const ZydisRegister acc[9] = {0, ZYDIS_REGISTER_AL, ZYDIS_REGISTER_AX, 0, ZYDIS_REGISTER_EAX, 0, 0, 0, ZYDIS_REGISTER_RAX};
        if (rep) { for (; CNT(); DECCNT()) { greg_wr(s, acc[n], ild(AREG(RSI), w)); AADV(RSI); } }
        else { greg_wr(s, acc[n], ild(AREG(RSI), w)); AADV(RSI); }
        break; }
    case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD: case ZYDIS_MNEMONIC_SCASQ: {
        u64 a = c->r[RAX];
        if (rep) {
            for (; CNT();) {
                alu(s, ZYDIS_MNEMONIC_CMP, a, ild(AREG(RDI), w), w); AADV(RDI); DECCNT();
                if (repne ? s->zf : !s->zf) break;
            }
        } else { alu(s, ZYDIS_MNEMONIC_CMP, a, ild(AREG(RDI), w), w); AADV(RDI); }
        break; }
    case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD: case ZYDIS_MNEMONIC_CMPSQ:
        if (rep) {
            for (; CNT();) {
                alu(s, ZYDIS_MNEMONIC_CMP, ild(AREG(RSI), w), ild(AREG(RDI), w), w); AADV(RSI); AADV(RDI); DECCNT();
                if (repne ? s->zf : !s->zf) break;
            }
        } else { alu(s, ZYDIS_MNEMONIC_CMP, ild(AREG(RSI), w), ild(AREG(RDI), w), w); AADV(RSI); AADV(RDI); }
        break;
    default: return X_UNIMPL;
    }
    #undef AREG
    #undef AADV
    #undef CNT
    #undef DECCNT
    return X_NEXT;
}

// ---- integer instructions --------------------------------------------------------------------------------
static int exec_int(IState *s) {
    Ctx *c = s->c;
    const ZydisDecodedInstruction *in = s->in;
    ZydisMnemonic mn = in->mnemonic;
    int nv = NVIS(s);
    int w = nv ? IOP(s, 0)->size : in->operand_width;
    int lock = (in->attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0;
    int mem0 = nv && IOP(s, 0)->type == ZYDIS_OPERAND_TYPE_MEMORY;

    switch (mn) {
    case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_PAUSE: case ZYDIS_MNEMONIC_ENDBR64: case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1: case ZYDIS_MNEMONIC_PREFETCHT2: case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHW:
    case ZYDIS_MNEMONIC_PREFETCH: case ZYDIS_MNEMONIC_CLFLUSH: case ZYDIS_MNEMONIC_CLFLUSHOPT: case ZYDIS_MNEMONIC_FWAIT:
        return X_NEXT;
    case ZYDIS_MNEMONIC_LFENCE: case ZYDIS_MNEMONIC_MFENCE: case ZYDIS_MNEMONIC_SFENCE:
        __atomic_thread_fence(__ATOMIC_SEQ_CST); return X_NEXT;

    case ZYDIS_MNEMONIC_MOV:
        if (IOP(s, 0)->type == ZYDIS_OPERAND_TYPE_REGISTER && rt_ireg[IOP(s, 0)->reg.value].kind != RK_GPR) return X_UNIMPL;
        if (IOP(s, 1)->type == ZYDIS_OPERAND_TYPE_REGISTER && rt_ireg[IOP(s, 1)->reg.value].kind != RK_GPR) return X_UNIMPL;
        wri(s, 0, rdi(s, 1)); return X_NEXT;
    case ZYDIS_MNEMONIC_MOVZX: wri(s, 0, rdi(s, 1)); return X_NEXT;
    case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD: wri(s, 0, sext(rdi(s, 1), IOP(s, 1)->size)); return X_NEXT;
    case ZYDIS_MNEMONIC_MOVBE: {
        u64 v = rdi(s, 1);
        v = w == 16 ? __builtin_bswap16((u16)v) : w == 32 ? __builtin_bswap32((u32)v) : __builtin_bswap64(v);
        wri(s, 0, v); return X_NEXT; }
    case ZYDIS_MNEMONIC_MOVNTI: wri(s, 0, rdi(s, 1)); return X_NEXT;
    case ZYDIS_MNEMONIC_LEA: {
        const ZydisDecodedOperand *o = IOP(s, 1);
        u64 a = (u64)o->mem.disp.value;
        if (o->mem.base != ZYDIS_REGISTER_NONE) a += greg_rd(s, o->mem.base);
        if (o->mem.index != ZYDIS_REGISTER_NONE) a += greg_rd(s, o->mem.index) * o->mem.scale;
        if (in->address_width == 32) a = (u32)a;
        wri(s, 0, a); return X_NEXT; }

    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_TEST: case ZYDIS_MNEMONIC_INC:
    case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_NEG: case ZYDIS_MNEMONIC_NOT: {
        int binary = mn != ZYDIS_MNEMONIC_INC && mn != ZYDIS_MNEMONIC_DEC && mn != ZYDIS_MNEMONIC_NEG && mn != ZYDIS_MNEMONIC_NOT;
        u64 b = binary ? rdi(s, 1) : 0;
        if (lock && mem0) {
            u64 a = iea(s, IOP(s, 0)), old = ild(a, w);
            u8 cf0 = s->cf;
            for (;;) { s->cf = cf0; u64 r = alu(s, mn, old, b, w); if (icas(a, w, &old, r)) break; }
        } else {
            u64 r = alu(s, mn, rdi(s, 0), b, w);
            if (mn != ZYDIS_MNEMONIC_CMP && mn != ZYDIS_MNEMONIC_TEST) wri(s, 0, r);
        }
        return X_NEXT; }

    case ZYDIS_MNEMONIC_XCHG:
        if (mem0 || IOP(s, 1)->type == ZYDIS_OPERAND_TYPE_MEMORY) {
            int mi = mem0 ? 0 : 1, ri = 1 - mi;
            u64 old = ixchg(iea(s, IOP(s, mi)), w, rdi(s, ri));
            wri(s, ri, old);
        } else { u64 a = rdi(s, 0), b = rdi(s, 1); wri(s, 0, b); wri(s, 1, a); }
        return X_NEXT;
    case ZYDIS_MNEMONIC_XADD: {
        u64 b = rdi(s, 1), old;
        if (mem0) {
            u64 a = iea(s, IOP(s, 0)); old = ild(a, w);
            for (;;) { u64 r = (old + b) & imask(w); if (icas(a, w, &old, r)) break; }
        } else { old = rdi(s, 0); wri(s, 0, old + b); }
        alu(s, ZYDIS_MNEMONIC_ADD, old, b, w);
        wri(s, 1, old);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_CMPXCHG: {
        static const ZydisRegister acc[9] = {0, ZYDIS_REGISTER_AL, ZYDIS_REGISTER_AX, 0, ZYDIS_REGISTER_EAX, 0, 0, 0, ZYDIS_REGISTER_RAX};
        u64 ac = greg_rd(s, acc[w / 8]), src = rdi(s, 1), old;
        if (mem0) {
            u64 a = iea(s, IOP(s, 0)); old = ac;
            icas(a, w, &old, src);
        } else { old = rdi(s, 0); wri(s, 0, old == (ac & imask(w)) ? src : old); }   // a 32-bit dest is zero-extended either way
        alu(s, ZYDIS_MNEMONIC_CMP, ac, old, w);
        if (!s->zf) greg_wr(s, acc[w / 8], old);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_CMPXCHG8B: {
        u64 a = iea(s, IOP(s, 0));
        u64 exp = (u32)c->r[RAX] | ((u64)(u32)c->r[RDX] << 32), des = (u32)c->r[RBX] | ((u64)(u32)c->r[RCX] << 32);
        if (icas(a, 64, &exp, des)) s->zf = 1;
        else { s->zf = 0; c->r[RAX] = (u32)exp; c->r[RDX] = exp >> 32; }
        return X_NEXT; }
    case ZYDIS_MNEMONIC_CMPXCHG16B: {
        u64 a = iea(s, IOP(s, 0));
        u128 exp = (u128)c->r[RAX] | ((u128)c->r[RDX] << 64), des = (u128)c->r[RBX] | ((u128)c->r[RCX] << 64);
        if (__atomic_compare_exchange_n((u128 *)a, &exp, des, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) s->zf = 1;
        else { s->zf = 0; c->r[RAX] = (u64)exp; c->r[RDX] = (u64)(exp >> 64); }
        return X_NEXT; }

    case ZYDIS_MNEMONIC_PUSH: {
        u64 v = rdi(s, 0);
        if (in->operand_width == 16) { c->r[RSP] -= 2; STS16(c->r[RSP], (u16)v); }
        else push64(c, v);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_POP:
        if (in->operand_width == 16) { u16 v = LDS16(c->r[RSP]); c->r[RSP] += 2; wri(s, 0, v); }
        else { u64 v = pop64(c); wri(s, 0, v); }
        return X_NEXT;
    case ZYDIS_MNEMONIC_PUSHFQ: push64(c, get_rflags(s)); return X_NEXT;
    case ZYDIS_MNEMONIC_POPFQ: set_rflags(s, pop64(c)); return X_NEXT;
    case ZYDIS_MNEMONIC_LEAVE: c->r[RSP] = c->r[RBP]; c->r[RBP] = pop64(c); return X_NEXT;
    case ZYDIS_MNEMONIC_ENTER:
        if (IOP(s, 1)->imm.value.u & 31) return X_UNIMPL;
        push64(c, c->r[RBP]); c->r[RBP] = c->r[RSP]; c->r[RSP] -= IOP(s, 0)->imm.value.u & 0xFFFF;
        return X_NEXT;

    case ZYDIS_MNEMONIC_CBW: c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | (u16)(i16)(i8)c->r[RAX]; return X_NEXT;
    case ZYDIS_MNEMONIC_CWDE: c->r[RAX] = (u32)(i32)(i16)c->r[RAX]; return X_NEXT;
    case ZYDIS_MNEMONIC_CDQE: c->r[RAX] = (u64)(i64)(i32)c->r[RAX]; return X_NEXT;
    case ZYDIS_MNEMONIC_CWD: c->r[RDX] = (c->r[RDX] & ~0xFFFFull) | (((i16)c->r[RAX] < 0) ? 0xFFFF : 0); return X_NEXT;
    case ZYDIS_MNEMONIC_CDQ: c->r[RDX] = ((i32)c->r[RAX] < 0) ? 0xFFFFFFFFull : 0; return X_NEXT;
    case ZYDIS_MNEMONIC_CQO: c->r[RDX] = ((i64)c->r[RAX] < 0) ? ~0ull : 0; return X_NEXT;

    case ZYDIS_MNEMONIC_MUL: case ZYDIS_MNEMONIC_IMUL: {
        int sgn = mn == ZYDIS_MNEMONIC_IMUL;
        if (nv == 1) {
            u64 b = rdi(s, 0);
            if (w == 8) {
                u64 a = c->r[RAX] & 0xFF; u16 r = sgn ? (u16)((i16)(i8)a * (i16)(i8)b) : (u16)(a * (u8)b);
                c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | r;
                s->cf = s->of = sgn ? (i16)r != (i8)r : (r >> 8) != 0;
                iszp(s, r, 8);
            } else {
                u64 a = c->r[RAX] & imask(w); u128 full; u64 lo, hi;
                if (sgn) { i128 p = (i128)(i64)sext(a, w) * (i64)sext(b, w); full = (u128)p; }
                else full = (u128)a * (b & imask(w));
                lo = (u64)full & imask(w); hi = (u64)(full >> w) & imask(w);
                if (w == 64) hi = (u64)(full >> 64);
                if (sgn) s->cf = s->of = hi != ((lo >> (w - 1) & 1) ? imask(w) : 0);
                else s->cf = s->of = hi != 0;
                if (w == 16) { c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | lo; c->r[RDX] = (c->r[RDX] & ~0xFFFFull) | hi; }
                else { c->r[RAX] = lo; c->r[RDX] = hi; }
                iszp(s, lo, w);
            }
        } else {
            u64 a = nv == 3 ? rdi(s, 1) : rdi(s, 0), b = nv == 3 ? rdi(s, 2) : rdi(s, 1);
            i128 p = (i128)(i64)sext(a, w) * (i64)sext(b, w);
            u64 r = (u64)p & imask(w);
            s->cf = s->of = (i128)(i64)sext(r, w) != p;
            iszp(s, r, w);
            wri(s, 0, r);
        }
        return X_NEXT; }
    case ZYDIS_MNEMONIC_DIV: case ZYDIS_MNEMONIC_IDIV: {
        u64 b = rdi(s, 0) & imask(w);
        if (!b) rt_trap(c, s->pc, "#DE");
        if (mn == ZYDIS_MNEMONIC_DIV) {
            u128 n = w == 8 ? (c->r[RAX] & 0xFFFF) : ((u128)(c->r[RDX] & imask(w)) << w) | (c->r[RAX] & imask(w));
            if (w == 64) n = ((u128)c->r[RDX] << 64) | c->r[RAX];
            u128 q = n / b, r = n % b;
            if (q >> w) rt_trap(c, s->pc, "#DE");
            if (w == 8) c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | (u64)q | ((u64)r << 8);
            else { wr_gw(c, RAX, w, (u64)q); wr_gw(c, RDX, w, (u64)r); }
        } else {
            i128 n = w == 8 ? (i128)(i16)c->r[RAX] : w == 64 ? (i128)(((u128)c->r[RDX] << 64) | c->r[RAX])
                   : (i128)(i64)sext(((c->r[RDX] & imask(w)) << w) | (c->r[RAX] & imask(w)), 2 * w);
            i64 d = (i64)sext(b, w);
            if (d == -1 && n == -(i128)((u128)1 << (2 * w - 1))) rt_trap(c, s->pc, "#DE");
            i128 q = n / d, r = n % d;
            i128 lim = (i128)1 << (w - 1);
            if (q >= lim || q < -lim) rt_trap(c, s->pc, "#DE");
            if (w == 8) c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | (u8)q | ((u64)(u8)r << 8);
            else { wr_gw(c, RAX, w, (u64)q); wr_gw(c, RDX, w, (u64)r); }
        }
        return X_NEXT; }

    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCL: case ZYDIS_MNEMONIC_RCR: {
        u64 cnt = nv >= 2 ? rdi(s, 1) : 1;
        wri(s, 0, shift(s, mn, rdi(s, 0), cnt, w));
        return X_NEXT; }
    case ZYDIS_MNEMONIC_SHLD: case ZYDIS_MNEMONIC_SHRD: {
        u64 a = rdi(s, 0), b = rdi(s, 1) & imask(w), cnt = rdi(s, 2) & (w == 64 ? 63 : 31), r;
        if (!cnt) { wri(s, 0, a); return X_NEXT; }   // flags unchanged, but a 32-bit destination is still zero-extended
        if (cnt > (u64)w) cnt = w;   // undefined on hardware (16-bit only)
        a &= imask(w);
        if (mn == ZYDIS_MNEMONIC_SHLD) {
            r = cnt == (u64)w ? b : ((a << cnt) | (b >> (w - cnt))) & imask(w);
            s->cf = (a >> (w - cnt)) & 1;
        } else {
            r = cnt == (u64)w ? b : ((a >> cnt) | (b << (w - cnt))) & imask(w);
            s->cf = (a >> (cnt - 1)) & 1;
        }
        s->of = ((r ^ a) >> (w - 1)) & 1;
        iszp(s, r, w);
        wri(s, 0, r);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_SHLX: case ZYDIS_MNEMONIC_SHRX: case ZYDIS_MNEMONIC_SARX: {
        u64 a = rdi(s, 1) & imask(w), cnt = rdi(s, 2) & (w == 64 ? 63 : 31);
        u64 r = mn == ZYDIS_MNEMONIC_SHLX ? a << cnt : mn == ZYDIS_MNEMONIC_SHRX ? a >> cnt : (u64)((i64)sext(a, w) >> cnt);
        wri(s, 0, r & imask(w)); return X_NEXT; }
    case ZYDIS_MNEMONIC_RORX: {
        u64 a = rdi(s, 1) & imask(w), k = rdi(s, 2) % w;
        wri(s, 0, k ? ((a >> k) | (a << (w - k))) & imask(w) : a); return X_NEXT; }

    case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR: case ZYDIS_MNEMONIC_BTC: {
        u64 off = rdi(s, 1);
        if (mem0) {
            u64 a = iea(s, IOP(s, 0));
            if (IOP(s, 1)->type == ZYDIS_OPERAND_TYPE_REGISTER) { i64 so = (i64)sext(off, w); a += (u64)((so >> (w == 64 ? 6 : w == 32 ? 5 : 4)) * (w / 8)); }
            u64 bit = off & (w - 1), m = 1ull << bit, old = ild(a, w), r;
            if (mn == ZYDIS_MNEMONIC_BT) { s->cf = (old >> bit) & 1; return X_NEXT; }
            for (;;) {
                r = mn == ZYDIS_MNEMONIC_BTS ? old | m : mn == ZYDIS_MNEMONIC_BTR ? old & ~m : old ^ m;
                if (!lock) { ist(a, w, r); break; }
                if (icas(a, w, &old, r)) break;
            }
            s->cf = (old >> bit) & 1;
        } else {
            u64 v = rdi(s, 0), bit = off & (w - 1), m = 1ull << bit;
            s->cf = (v >> bit) & 1;
            if (mn == ZYDIS_MNEMONIC_BTS) wri(s, 0, v | m);
            else if (mn == ZYDIS_MNEMONIC_BTR) wri(s, 0, v & ~m);
            else if (mn == ZYDIS_MNEMONIC_BTC) wri(s, 0, v ^ m);
        }
        return X_NEXT; }
    case ZYDIS_MNEMONIC_BSF: case ZYDIS_MNEMONIC_BSR: {
        u64 v = rdi(s, 1) & imask(w);
        if (!v) { s->zf = 1; return X_NEXT; }   // destination unchanged (as on Intel/AMD hardware)
        s->zf = 0;
        wri(s, 0, mn == ZYDIS_MNEMONIC_BSF ? (u64)__builtin_ctzll(v) : (u64)(63 - __builtin_clzll(v)));
        return X_NEXT; }
    case ZYDIS_MNEMONIC_TZCNT: case ZYDIS_MNEMONIC_LZCNT: {
        u64 v = rdi(s, 1) & imask(w), r;
        if (mn == ZYDIS_MNEMONIC_TZCNT) r = v ? (u64)__builtin_ctzll(v) : (u64)w;
        else r = v ? (u64)(__builtin_clzll(v) - (64 - w)) : (u64)w;
        s->cf = v == 0; s->zf = r == 0;
        wri(s, 0, r); return X_NEXT; }
    case ZYDIS_MNEMONIC_POPCNT: {
        u64 v = rdi(s, 1) & imask(w);
        s->zf = v == 0; s->cf = s->of = s->sf = s->af = s->pf = 0;
        wri(s, 0, (u64)__builtin_popcountll(v)); return X_NEXT; }
    case ZYDIS_MNEMONIC_BSWAP: {
        u64 v = rdi(s, 0);
        wri(s, 0, w == 64 ? __builtin_bswap64(v) : __builtin_bswap32((u32)v)); return X_NEXT; }

    // BMI1/BMI2/ADX
    case ZYDIS_MNEMONIC_ANDN: {
        u64 r = ~rdi(s, 1) & rdi(s, 2) & imask(w);
        s->cf = s->of = 0; s->zf = r == 0; s->sf = (r >> (w - 1)) & 1;
        wri(s, 0, r); return X_NEXT; }
    case ZYDIS_MNEMONIC_BLSI: case ZYDIS_MNEMONIC_BLSMSK: case ZYDIS_MNEMONIC_BLSR: {
        u64 a = rdi(s, 1) & imask(w), r;
        if (mn == ZYDIS_MNEMONIC_BLSI) { r = a & (0 - a); s->cf = a != 0; }
        else if (mn == ZYDIS_MNEMONIC_BLSMSK) { r = (a ^ (a - 1)) & imask(w); s->cf = a == 0; }
        else { r = a & (a - 1); s->cf = a == 0; }
        r &= imask(w); s->of = 0; s->zf = r == 0; s->sf = (r >> (w - 1)) & 1;
        wri(s, 0, r); return X_NEXT; }
    case ZYDIS_MNEMONIC_BZHI: {
        u64 a = rdi(s, 1) & imask(w), n = rdi(s, 2) & 0xFF, r = n < (u64)w ? a & ((1ull << n) - 1) : a;
        s->cf = n > (u64)(w - 1); s->of = 0; s->zf = r == 0; s->sf = (r >> (w - 1)) & 1;
        wri(s, 0, r); return X_NEXT; }
    case ZYDIS_MNEMONIC_BEXTR: {
        u64 a = rdi(s, 1) & imask(w), ctl = rdi(s, 2), st = ctl & 0xFF, ln = (ctl >> 8) & 0xFF;
        u64 r = st >= (u64)w ? 0 : a >> st;
        if (ln < 64) r &= (1ull << ln) - 1;
        s->cf = s->of = 0; s->zf = r == 0;
        wri(s, 0, r); return X_NEXT; }
    case ZYDIS_MNEMONIC_PDEP: wri(s, 0, PDEP64(rdi(s, 1), rdi(s, 2)) & imask(w)); return X_NEXT;
    case ZYDIS_MNEMONIC_PEXT: wri(s, 0, PEXT64(rdi(s, 1), rdi(s, 2) & imask(w))); return X_NEXT;
    case ZYDIS_MNEMONIC_MULX: {
        u128 p = (u128)(c->r[RDX] & imask(w)) * (rdi(s, 2) & imask(w));
        u64 lo = (u64)p & imask(w), hi = (u64)(p >> w) & imask(w);
        wri(s, 1, lo); wri(s, 0, hi); return X_NEXT; }
    case ZYDIS_MNEMONIC_ADCX: case ZYDIS_MNEMONIC_ADOX: {
        u8 *f = mn == ZYDIS_MNEMONIC_ADCX ? &s->cf : &s->of;
        u128 t = (u128)(rdi(s, 0) & imask(w)) + (rdi(s, 1) & imask(w)) + *f;
        *f = (u8)((t >> w) & 1);
        wri(s, 0, (u64)t & imask(w)); return X_NEXT; }

    case ZYDIS_MNEMONIC_CRC32: {
        u64 crc = rdi(s, 0); int nb = IOP(s, 1)->size / 8;
        wri(s, 0, rt_crc32c((u32)crc, rdi(s, 1), nb)); return X_NEXT; }
    case ZYDIS_MNEMONIC_RDRAND: case ZYDIS_MNEMONIC_RDSEED:
        wri(s, 0, rt_random64()); s->cf = 1; s->of = s->sf = s->zf = s->af = s->pf = 0; return X_NEXT;

    case ZYDIS_MNEMONIC_CLC: s->cf = 0; return X_NEXT;
    case ZYDIS_MNEMONIC_STC: s->cf = 1; return X_NEXT;
    case ZYDIS_MNEMONIC_CMC: s->cf = !s->cf; return X_NEXT;
    case ZYDIS_MNEMONIC_CLD: s->df = 0; return X_NEXT;
    case ZYDIS_MNEMONIC_STD: s->df = 1; return X_NEXT;
    case ZYDIS_MNEMONIC_LAHF:
        c->r[RAX] = (c->r[RAX] & ~0xFF00ull) | ((get_rflags(s) & 0xD5) | 2) << 8; return X_NEXT;
    case ZYDIS_MNEMONIC_SAHF: {
        u8 ah = (u8)(c->r[RAX] >> 8);
        s->sf = ah >> 7 & 1; s->zf = ah >> 6 & 1; s->af = ah >> 4 & 1; s->pf = ah >> 2 & 1; s->cf = ah & 1; return X_NEXT; }

    case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_SETNB:
    case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_SETNBE:
    case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_SETNP:
    case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_SETNLE:
        wri(s, 0, (u64)cond(s, in->opcode & 15)); return X_NEXT;
    case ZYDIS_MNEMONIC_CMOVO: case ZYDIS_MNEMONIC_CMOVNO: case ZYDIS_MNEMONIC_CMOVB: case ZYDIS_MNEMONIC_CMOVNB:
    case ZYDIS_MNEMONIC_CMOVZ: case ZYDIS_MNEMONIC_CMOVNZ: case ZYDIS_MNEMONIC_CMOVBE: case ZYDIS_MNEMONIC_CMOVNBE:
    case ZYDIS_MNEMONIC_CMOVS: case ZYDIS_MNEMONIC_CMOVNS: case ZYDIS_MNEMONIC_CMOVP: case ZYDIS_MNEMONIC_CMOVNP:
    case ZYDIS_MNEMONIC_CMOVL: case ZYDIS_MNEMONIC_CMOVNL: case ZYDIS_MNEMONIC_CMOVLE: case ZYDIS_MNEMONIC_CMOVNLE: {
        u64 v = rdi(s, 1);   // the source is read (and may fault) even when the condition is false
        wri(s, 0, cond(s, in->opcode & 15) ? v : rdi(s, 0));
        return X_NEXT; }

    case ZYDIS_MNEMONIC_XLAT: {
        u64 a = c->r[RBX] + (c->r[RAX] & 0xFF);
        if (in->address_width == 32) a = (u32)a;
        c->r[RAX] = (c->r[RAX] & ~0xFFull) | LD8(a); return X_NEXT; }
    case ZYDIS_MNEMONIC_CPUID: rt_cpuid(c); return X_NEXT;
    case ZYDIS_MNEMONIC_RDTSC: { u64 t = rt_rdtsc(); c->r[RAX] = (u32)t; c->r[RDX] = t >> 32; return X_NEXT; }
    case ZYDIS_MNEMONIC_RDTSCP: { u64 t = rt_rdtsc(); c->r[RAX] = (u32)t; c->r[RDX] = t >> 32; c->r[RCX] = 0; return X_NEXT; }
    case ZYDIS_MNEMONIC_XGETBV: c->r[RAX] = 7; c->r[RDX] = 0; return X_NEXT;
    case ZYDIS_MNEMONIC_INT3: rt_trap(c, s->pc, "int3");
    case ZYDIS_MNEMONIC_UD2: rt_trap(c, s->pc, "ud2");
    case ZYDIS_MNEMONIC_HLT: rt_trap(c, s->pc, "hlt");
    case ZYDIS_MNEMONIC_INT: { char b[32]; snprintf(b, sizeof b, "int %llu", (unsigned long long)IOP(s, 0)->imm.value.u); rt_trap(c, s->pc, b); }

    default: return X_UNIMPL;
    }
}

// ---- one instruction ------------------------------------------------------------------------------------------
static int exec(IState *s) {
    Ctx *c = s->c;
    const ZydisDecodedInstruction *in = s->in;
    s->pc = s->next;
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_JMP: return do_jump(s, branch_target(s, 0));
    case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_JNB:
    case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_JNBE:
    case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_JNP:
    case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_JNLE:
        if (cond(s, in->opcode & 15)) return do_jump(s, branch_target(s, 0));
        return X_NEXT;
    case ZYDIS_MNEMONIC_JRCXZ: if (!c->r[RCX]) return do_jump(s, branch_target(s, 0)); return X_NEXT;
    case ZYDIS_MNEMONIC_JECXZ: if (!(u32)c->r[RCX]) return do_jump(s, branch_target(s, 0)); return X_NEXT;
    case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE: {
        int a32 = in->address_width == 32;
        u64 n = a32 ? (u32)(c->r[RCX] - 1) : c->r[RCX] - 1;
        c->r[RCX] = n;
        int t = n != 0 && (in->mnemonic == ZYDIS_MNEMONIC_LOOP || (in->mnemonic == ZYDIS_MNEMONIC_LOOPE ? s->zf : !s->zf));
        if (t) return do_jump(s, branch_target(s, 0));
        return X_NEXT; }
    case ZYDIS_MNEMONIC_CALL: return do_call(s, branch_target(s, 0));
    case ZYDIS_MNEMONIC_RET: return do_ret(s, NVIS(s) ? IOP(s, 0)->imm.value.u & 0xFFFF : 0);
    default: break;
    }
    if (in->meta.category == ZYDIS_CATEGORY_STRINGOP) return string_op(s);
    int r = exec_int(s);
    if (r != X_UNIMPL) return r;
    if (in->meta.isa_ext == ZYDIS_ISA_EXT_X87) {
        r = rt_interp_x87(s);
        if (r != X_UNIMPL) return r;
    }
    return rt_interp_vec(s);
}

static void interp_fatal(Ctx *c, u64 ip, const char *what) __attribute__((noreturn));
static void interp_fatal(Ctx *c, u64 ip, const char *what) {
    static const char *n[] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
    rt_log("unimplemented instruction at 0x%llx: %s (interpreter)", (unsigned long long)ip, what);
    for (int i = 0; i < 16; i++) fprintf(stderr, "  %-3s=%016llx%s", n[i], (unsigned long long)c->r[i], i % 4 == 3 ? "\n" : "");
    exit(71);
}

void rt_interp_run(Ctx *c, u64 pc) { rt_interp_run_ex(c, pc, NULL); }

// One instruction at s->pc (the JIT's fallback for instructions it doesn't translate).
int rt_interp_step(IState *s) {
    const DEnt *e = decode(s->pc);
    if (!e) {
        rt_log("interp: cannot decode instruction at 0x%llx", (unsigned long long)s->pc);
        rt_trap(s->c, s->pc, "ud2");
    }
    s->in = &e->in; s->op = e->op;
    s->next = s->pc + e->in.length;
    u64 ip = s->pc;
    int r = exec(s);
    if (r == X_UNIMPL) {
        char buf[160];
        rt_interp_disasm(ip, buf, sizeof buf);
        interp_fatal(s->c, ip, buf);
    }
    return r;
}

void rt_interp_run_ex(Ctx *c, u64 pc, u64 *rflags) {
    pthread_once(&init_once, init_tables);
    if (trace_mode < 0) {
        trace_mode = getenv("WINTOMAC_INTERP_TRACE") ? 1 : 0;
        if (trace_mode) atexit(stats_dump);
    }
    IState s;
    memset(&s, 0, sizeof s);
    s.c = c; s.pc = pc; s.entry_rsp = c->r[RSP];
    if (rflags) set_rflags(&s, *rflags);
    u64 entry = pc, n = 0;
    if (trace_mode && rt_trace_enabled()) rt_log("interp: enter %llx rsp=%llx", (unsigned long long)pc, (unsigned long long)c->r[RSP]);
    int jit = rt_jit_enabled(), boundary = 1;
    for (;;) {
        // Block boundary (entry, or after a control transfer): hot code continues in the JIT (runtime/jit.c).
        // When it hands back, at least one instruction is interpreted before asking again.
        if (jit && boundary) {
            boundary = 0;
            if (rt_jit_run(&s) == X_HOST_RET) break;
        }
        const DEnt *e = decode(s.pc);
        if (!e) {
            rt_log("interp: cannot decode instruction at 0x%llx", (unsigned long long)s.pc);
            rt_trap(c, s.pc, "ud2");
        }
        s.in = &e->in; s.op = e->op;
        s.next = s.pc + e->in.length;
        u64 ip = s.pc;
        n++;
        int br = e->branch;   // (a native call made by exec may reuse this decode-cache slot)
        int r = exec(&s);
        if (__builtin_expect(r == X_NEXT, 1)) { boundary = br; continue; }
        if (r == X_HOST_RET) break;
        char buf[160];
        rt_interp_disasm(ip, buf, sizeof buf);
        interp_fatal(c, ip, buf);
    }
    if (rflags) *rflags = get_rflags(&s);
    if (trace_mode) stats_add(entry, n);
}

// ---- miss log / switches / shims --------------------------------------------------------------------------
int rt_interp_disabled(void) {
    static int v = -1;
    if (v < 0) v = getenv("WINTOMAC_NO_INTERP") != NULL;
    return v;
}

void rt_note_miss(u64 target, int label) {
    static u64 seen[4096];
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    if (rt_interp_is_exec(target)) return;
    pthread_mutex_lock(&lk);
    u64 h = (target * 0x9E3779B97F4A7C15ull) >> 52;
    int fresh = 1;
    for (int k = 0; k < 4096; k++) {
        u64 *e = &seen[(h + k) & 4095];
        if (*e == target) { fresh = 0; break; }
        if (!*e) { *e = target; break; }
    }
    pthread_mutex_unlock(&lk);
    if (!fresh) return;
    FILE *f = fopen("wintomac_misses.txt", "a");
    if (f) { fprintf(f, label ? "label 0x%llx\n" : "0x%llx\n", (unsigned long long)target); fclose(f); }
    if (rt_trace_enabled() || getenv("WINTOMAC_INTERP_TRACE"))
        rt_log("interpreting unrecompiled %s 0x%llx (logged to wintomac_misses.txt for --seeds-file)", label ? "label" : "address",
               (unsigned long long)target);
}

static u64 fallback_seen[1024];   // ips where recompiled code handed over (not function entries)
static int is_fallback_site(u64 ip) {
    for (int k = 0; k < 1024; k++) {
        u64 e = __atomic_load_n(&fallback_seen[(ip / 4 + k) & 1023], __ATOMIC_RELAXED);
        if (e == ip) return 1;
        if (!e) return 0;
    }
    return 0;
}
void rt_note_fallback(u64 ip, const char *what) {
    u64 *seen = fallback_seen;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lk);
    int fresh = 1;
    for (int k = 0; k < 1024; k++) {
        u64 *e = &seen[(ip / 4 + k) & 1023];
        if (*e == ip) { fresh = 0; break; }
        if (!*e) { *e = ip; break; }
    }
    pthread_mutex_unlock(&lk);
    if (fresh && (rt_trace_enabled() || getenv("WINTOMAC_INTERP_TRACE")))
        rt_log("interp: recompiled code at 0x%llx continues in the interpreter (%s)", (unsigned long long)ip, what);
}

// JITs call this after writing code. The interpreter re-validates decoded instructions against memory; the
// JIT drops translations of the range (writes to translated pages are caught anyway, see runtime/jit.c, but
// pages that are not write-protected rely on this or on their entry checks). size 0 / NULL: everything we know.
SHIM(FlushInstructionCache) {   // (process, base, size)
    if (A2) rt_jit_invalidate(A2, A3 ? A3 : 1);
    RET(1);
}
const Shim shims_interp[] = {
    { NULL, "FlushInstructionCache", sh_FlushInstructionCache },
    { NULL, NULL, NULL }
};

// VirtualAlloc / VirtualProtect hook (kernel32.c). Fresh allocations with PAGE_EXECUTE* hold runtime-generated
// code: always interpreted. A recompiled image made executable+writable may be patched code, but usually is
// not (CRTs, hooks that don't touch what we run), so image ranges are only taken over when
// WINTOMAC_INTERP_PATCHED=1 is set; otherwise the event is just logged under WINTOMAC_TRACE.
void rt_interp_note_protect(u64 addr, u64 size, u32 prot, int is_alloc) {
    if (!is_alloc) rt_jit_invalidate(addr, size);   // the guest is about to (re)write or retire this code
    if (!(prot & 0xF0)) return;   // PAGE_EXECUTE (0x10), _READ (0x20), _READWRITE (0x40), _WRITECOPY (0x80)
    u64 isz;
    if (!is_alloc && rt_module_base_for(addr, &isz)) {
        if (!getenv("WINTOMAC_INTERP_PATCHED")) {
            if (rt_trace_enabled()) rt_log("interp: VirtualProtect(%llx, %llx, %x) on a recompiled image ignored (set WINTOMAC_INTERP_PATCHED=1 to interpret it)",
                                           (unsigned long long)addr, (unsigned long long)size, prot);
            return;
        }
    }
    rt_interp_mark_exec(addr, size);
}
