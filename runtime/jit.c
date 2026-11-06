// wintomac runtime JIT: code cache, world entry/exit stubs, dispatcher, block chaining, invalidation and the
// fault handler. The translator itself is in jit_x86.c (integer ISA, control flow, flags) and jit_sse.c.
//
// Design:
//  * Guest code first runs in the interpreter (runtime/interp.c). At every block boundary the interpreter
//    asks rt_jit_run(); a guest pc entered WINTOMAC_JIT_THRESHOLD times is translated as a *trace*: straight-line
//    code that runs through conditional branches (side exits) and ends at an unconditional transfer.
//  * Inside translated code ("the JIT world") all 16 guest GPRs live in fixed host registers and the flags are
//    lazy (operation + operand + result in x26..x28), so traces chain to each other without touching Ctx.
//    Ctx is synced only when leaving the world, calling native code, or running an instruction in the interpreter.
//  * Direct branches start as exits to the dispatcher and are patched into direct `b`s once the target exists
//    (QEMU TCG / box64 style chaining); indirect branches and returns go through a global lookup table.
//  * Translated pages of executable regions (VirtualAlloc/VirtualProtect PAGE_EXECUTE*) are write-protected; a
//    write fault drops the translations of that page and unprotects it. Pages that keep being written become
//    "volatile": their traces verify their own bytes on entry instead (box64's approach for mixed code/data
//    pages). Code anywhere else (missed functions in recompiled images, stack, heap) is always verified that
//    way, because a read-only page would make kernel writes into it (read(2) into a buffer there) fail.
//    FlushInstructionCache / VirtualProtect / VirtualFree drop translations of their range explicitly.
//
// Environment: WINTOMAC_JIT=0 (interpreter only), WINTOMAC_JIT_THRESHOLD=N (block entries before translating,
// default 16; 0 = translate everything at once), WINTOMAC_JIT_TRACE=1 (statistics on exit and in
// wintomac_jit.txt; with WINTOMAC_TRACE=1 also one line per translated block), WINTOMAC_JIT_DUMP=1 (host code of
// every block as wintomac_jit_<pc>.s), WINTOMAC_JIT_CACHE_KB (code cache chunk size, default 64 MB).
//  * W^X: MAP_JIT memory + pthread_jit_write_protect_np + sys_icache_invalidate (Apple's JIT porting guide).
#include "jit.h"
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

JStubs rt_jit_stubs;
int rt_jit_stat_trace;
static int jit_on = -1, jit_threshold = 16;
static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 host_page = 16384;

// ---- statistics -------------------------------------------------------------------------------------------
static struct {
    u64 blocks, insns, fallback_static, fallback_dyn, entries, links, unlinks, fast_miss, invalidated, write_faults,
        volatile_pages, align_faults, align_patches, stale_exits, code_bytes, chunks, compile_ns, interp_boundaries, rejected;
} st;
#define STAT(x) __atomic_fetch_add(&st.x, 1, __ATOMIC_RELAXED)

// ---- code cache ---------------------------------------------------------------------------------------------
static u64 CHUNK_SIZE = 64ull << 20;   // WINTOMAC_JIT_CACHE_KB (tests use small chunks to exercise switching)
typedef struct Chunk { u8 *base, *cur, *end; JStubs stubs; } Chunk;
static Chunk chunk;   // current chunk (older ones are leaked: translations in them may still be running)
static struct { u8 *lo, *hi; } chunks_seen[256];
static int nchunks_seen;

static void wx_begin(void) { pthread_jit_write_protect_np(0); }
static void wx_end(void *p, size_t n) { pthread_jit_write_protect_np(1); if (n) sys_icache_invalidate(p, n); }

// Fast lookup table for indirect branches: {guest pc, host code}, 16-byte aligned entries read with one ldp
// (single-copy atomic on FEAT_LSE2 hardware) and written with one stp.
#define FAST_BITS 16
#define FAST_N (1u << FAST_BITS)
typedef struct { u64 pc; u64 code; } __attribute__((aligned(16))) FastEnt;
static FastEnt *fast;
static inline u32 fast_idx(u64 pc) { return (u32)((pc ^ (pc >> 12)) & (FAST_N - 1)); }
static inline void fast_store(FastEnt *e, u64 pc, u64 code) {
    __asm__ volatile("stp %0, %1, [%2]" :: "r"(pc), "r"(code), "r"(e) : "memory");
}

static void emit_stubs(Chunk *ch) {
    A64 A = { (u32 *)ch->cur, (u32 *)ch->cur, (u32 *)ch->end, 0 }, *a = &A;
    // enter(Ctx *c = x0, JState *js = x1, code = x2) -> exit reason
    u32 *enter = a64_here(a);
    a64_ldstp(a, 0, 3, 29, 30, XSP, -128);
    a64_mov_sp(a, 29, XSP);
    a64_stp(a, 19, 20, XSP, 16); a64_stp(a, 21, 22, XSP, 32); a64_stp(a, 23, 24, XSP, 48);
    a64_stp(a, 25, 26, XSP, 64); a64_stp(a, 27, 28, XSP, 80);
    a64_addsub_imm(a, 1, 1, 0, XSP, XSP, 32, 0);
    a64_str(a, 1, 1, XSP, FR_JS);
    a64_mov_imm(a, T3, (u64)fast);
    a64_str(a, 1, T3, XSP, FR_FAST);
    a64_ldr(a, 1, T3, 1, __builtin_offsetof(JState, entry_rsp));
    a64_str(a, 1, T3, XSP, FR_ENTRY_RSP);
    a64_mov(a, 1, RCTX, 0);
    a64_ldr(a, 1, RFRES, 1, __builtin_offsetof(JState, rflags));
    a64_movz(a, 1, RFOP, 0, 0);
    a64_movz(a, 1, RFA, 0, 0);
    for (int i = 0; i < 16; i += 2) a64_ldp(a, GR(i), GR(i + 1), RCTX, CTX_R(i));
    a64_br(a, 2);
    // exit: x0 = guest pc, x1 = aux, x2 = reason
    u32 *ex = a64_here(a);
    for (int i = 0; i < 16; i += 2) a64_stp(a, GR(i), GR(i + 1), RCTX, CTX_R(i));
    a64_ldr(a, 1, T3, XSP, FR_JS);
    a64_str(a, 1, 0, T3, __builtin_offsetof(JState, exit_pc));
    a64_str(a, 1, 1, T3, __builtin_offsetof(JState, exit_aux));
    a64_str(a, 1, RFOP, T3, __builtin_offsetof(JState, fop));
    a64_str(a, 1, RFA, T3, __builtin_offsetof(JState, fa));
    a64_str(a, 1, RFRES, T3, __builtin_offsetof(JState, fres));
    a64_mov(a, 1, 0, 2);
    a64_addsub_imm(a, 1, 0, 0, XSP, XSP, 32, 0);
    a64_ldp(a, 19, 20, XSP, 16); a64_ldp(a, 21, 22, XSP, 32); a64_ldp(a, 23, 24, XSP, 48);
    a64_ldp(a, 25, 26, XSP, 64); a64_ldp(a, 27, 28, XSP, 80);
    a64_ldstp(a, 1, 1, 29, 30, XSP, 128);
    a64_ret(a);
    // lookup: x0 = guest target, x2 = reason if not translated
    u32 *lk = a64_here(a);
    a64_ldr(a, 1, T3, XSP, FR_FAST);
    a64_log_reg(a, 1, 2, 0, T4, 0, 0, SH_LSR, 12);
    a64_log_imm(a, 1, 0, T4, T4, FAST_N - 1);
    a64_addsub_reg(a, 1, 0, 0, T3, T3, T4, SH_LSL, 4);
    a64_ldp(a, T4, T5, T3, 0);
    a64_cmp(a, 1, T4, 0);
    a64_put(a, 0x54000000u | (2u << 5) | C_NE);   // b.ne +2 (to the exit below)
    a64_br(a, T5);
    a64_movz(a, 1, 1, 0, 0);
    a64_b(a, ex);
    // mat: lazy flags -> RFLAGS in x28, x26 = 0. Clobbers x0-x5, x16, x17, NZCV.
    u32 *mat = a64_here(a);
    u32 *mat_cbz = a64_here(a); a64_put(a, 0);
    a64_ldstp(a, 0, 3, 29, 30, XSP, -16);
    for (int i = 0; i < 10; i += 2) a64_stp(a, GR(i), GR(i + 1), RCTX, CTX_R(i));
    a64_addsub_imm(a, 1, 1, 0, XSP, XSP, 256, 0);                       // cached guest xmm registers (v16-v31)
    for (int i = 0; i < 16; i += 2) a64_ldstp_q(a, 0, 16 + i, 17 + i, XSP, 16 * i);
    a64_mov(a, 1, 0, RFOP); a64_mov(a, 1, 1, RFA); a64_mov(a, 1, 2, RFRES);
    a64_mov_imm(a, T16, (u64)rt_jit_flags);
    a64_blr(a, T16);
    a64_mov(a, 1, RFRES, 0);
    a64_movz(a, 1, RFOP, 0, 0);
    for (int i = 0; i < 16; i += 2) a64_ldstp_q(a, 1, 16 + i, 17 + i, XSP, 16 * i);
    a64_addsub_imm(a, 1, 0, 0, XSP, XSP, 256, 0);
    for (int i = 0; i < 10; i += 2) a64_ldp(a, GR(i), GR(i + 1), RCTX, CTX_R(i));
    a64_ldstp(a, 1, 1, 29, 30, XSP, 16);
    u32 *mat_ret = a64_here(a);
    a64_ret(a);
    *mat_cbz = 0xB4000000u | ((u32)(mat_ret - mat_cbz) & 0x7FFFF) << 5 | RFOP;   // cbz x26, ret
    // call_c: x16 = function, x0.. = arguments. Guest registers synced to Ctx around the call.
    u32 *cc = a64_here(a);
    a64_ldstp(a, 0, 3, 29, 30, XSP, -16);
    for (int i = 0; i < 16; i += 2) a64_stp(a, GR(i), GR(i + 1), RCTX, CTX_R(i));
    a64_blr(a, T16);
    for (int i = 0; i < 16; i += 2) a64_ldp(a, GR(i), GR(i + 1), RCTX, CTX_R(i));
    a64_ldstp(a, 1, 1, 29, 30, XSP, 16);
    a64_ret(a);
    ch->stubs = (JStubs){ (u32 (*)(Ctx *, JState *, void *))enter, ex, lk, mat, cc };
    ch->cur = (u8 *)a64_here(a);
    ch->cur = (u8 *)(((u64)ch->cur + 63) & ~63ull);
}

static int new_chunk(void) {
    void *p = mmap(NULL, CHUNK_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) { rt_log("jit: cannot map %llu MB of MAP_JIT memory; JIT disabled", (unsigned long long)(CHUNK_SIZE >> 20)); return 0; }
    Chunk ch = { p, p, (u8 *)p + CHUNK_SIZE };
    if (chunk.base && nchunks_seen < 256) { chunks_seen[nchunks_seen].lo = chunk.base; chunks_seen[nchunks_seen].hi = chunk.end; nchunks_seen++; }
    wx_begin();
    emit_stubs(&ch);
    wx_end(ch.base, ch.cur - ch.base);
    chunk = ch;
    rt_jit_stubs = ch.stubs;
    st.chunks++;
    return 1;
}

// ---- block table ------------------------------------------------------------------------------------------
#define BH_N 65536
static JBlock *bhash[BH_N];
static JBlock *all_blocks;
static inline u32 bh_idx(u64 pc) { return (u32)((pc * 0x9E3779B97F4A7C15ull) >> 48); }
static JBlock *find_block(u64 pc) {   // lock-free: blocks are never freed, removal keeps chains intact
    for (JBlock *b = __atomic_load_n(&bhash[bh_idx(pc)], __ATOMIC_ACQUIRE); b; b = __atomic_load_n(&b->hnext, __ATOMIC_ACQUIRE))
        if (b->pc == pc && !__atomic_load_n(&b->dead, __ATOMIC_ACQUIRE)) return b;
    return NULL;
}

// ---- guest pages holding translated code ----------------------------------------------------------------
enum { PG_NONE, PG_PROT, PG_VOLATILE };
typedef struct JPage { u64 pg; int state, orig_prot, faults; u64 gen; JBlock *blocks; struct JPage *next; } JPage;
#define PH_N 4096
static JPage *phash[PH_N];
static inline u32 ph_idx(u64 pg) { return (u32)((pg * 0x9E3779B97F4A7C15ull) >> 52); }
static JPage *find_page(u64 pg) {
    for (JPage *p = __atomic_load_n(&phash[ph_idx(pg)], __ATOMIC_ACQUIRE); p; p = p->next) if (p->pg == pg) return p;
    return NULL;
}
static JPage *get_page(u64 pg) {   // under jit_lock
    JPage *p = find_page(pg);
    if (p) return p;
    p = calloc(1, sizeof *p);
    p->pg = pg;
    p->next = phash[ph_idx(pg)];
    __atomic_store_n(&phash[ph_idx(pg)], p, __ATOMIC_RELEASE);
    return p;
}
static int query_prot(u64 addr) {
    mach_vm_address_t a = addr; mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return -1;
    if (a > addr) return -1;
    return info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
}
// Make sure the page is write-protected (or volatile) before its bytes are decoded; returns its generation.
// Only pages of executable regions (VirtualAlloc/VirtualProtect with PAGE_EXECUTE*: JIT output) are protected.
// A read-only page also makes the kernel's writes into it fail (read(2) returns EFAULT instead of faulting), so
// code found elsewhere (recompiled images, stack, heap) gets self-checking traces instead.
static int page_in_exec_region(u64 pg) {
    u64 isz;
    if (rt_module_base_for(pg, &isz)) return 0;
    for (u64 k = 0; k < host_page; k += 4096) if (rt_interp_is_exec(pg + k)) return 1;
    return 0;
}
static void page_prepare(JPage *p) {
    if (p->state != PG_NONE) return;
    int prot = query_prot(p->pg);
    if (prot < 0 || !(prot & VM_PROT_WRITE)) { p->orig_prot = prot < 0 ? 0 : prot; p->state = PG_PROT; return; }   // read-only already
    p->orig_prot = prot;
    if (!page_in_exec_region(p->pg)) { p->state = PG_VOLATILE; return; }
    if (mprotect((void *)p->pg, host_page, prot & ~VM_PROT_WRITE) == 0) p->state = PG_PROT;
    else p->state = PG_VOLATILE;
}

// ---- linking / killing ---------------------------------------------------------------------------------------
static void patch_word(u32 *at, u32 w) { wx_begin(); *at = w; wx_end(at, 4); }

static void kill_block(JBlock *b) {   // under jit_lock
    if (b->dead) return;
    __atomic_store_n(&b->dead, 1, __ATOMIC_RELEASE);
    JBlock **pp = &bhash[bh_idx(b->pc)];
    while (*pp && *pp != b) pp = &(*pp)->hnext;
    if (*pp) __atomic_store_n(pp, b->hnext, __ATOMIC_RELEASE);
    FastEnt *f = &fast[fast_idx(b->pc)];
    if (f->code == (u64)b->code) fast_store(f, 0, 0);
    wx_begin();
    *b->code = a64_b_enc(b->code, b->stale);   // racing threads that still jump here leave through the stale stub
    sys_icache_invalidate(b->code, 4);
    if (b->gate) { *b->gate = a64_b_enc(b->gate, b->gate_stale); sys_icache_invalidate(b->gate, 4); }   // running loops leave too
    for (JLink *l = b->in; l; l = l->next) { *l->site = l->orig; sys_icache_invalidate(l->site, 4); st.unlinks++; }
    pthread_jit_write_protect_np(1);
    while (b->in) { JLink *l = b->in; b->in = l->next; free(l); }
    st.invalidated++;
}

static void kill_page_blocks(JPage *p, u64 lo, u64 hi) {   // under jit_lock
    for (JBlock *b = p->blocks; b; b = (b->page[0] == p->pg) ? b->pnext[0] : b->pnext[1])
        if (!b->dead && b->pc < hi && lo < b->end) kill_block(b);
    // drop dead blocks from the list
    JBlock **pp = &p->blocks;
    while (*pp) {
        JBlock *b = *pp; int k = b->page[0] == p->pg ? 0 : 1;
        if (b->dead) *pp = b->pnext[k]; else pp = &b->pnext[k];
    }
}

void rt_jit_invalidate(u64 addr, u64 size) {
    if (jit_on <= 0 || !size) return;
    u64 lo = addr, hi = addr + size;
    pthread_mutex_lock(&jit_lock);
    for (u64 pg = lo & ~(host_page - 1); pg < hi; pg += host_page) {
        JPage *p = find_page(pg);
        if (!p) continue;
        kill_page_blocks(p, lo, hi);
        p->gen++;
        if (!p->blocks && p->state == PG_PROT && (p->orig_prot & VM_PROT_WRITE)) {   // nothing left to guard
            mprotect((void *)pg, host_page, p->orig_prot);
            p->state = PG_NONE;
        }
    }
    pthread_mutex_unlock(&jit_lock);
}
// VirtualFree: the range is gone; forget its pages so they are never mprotect()ed again.
void rt_jit_unmap(u64 addr, u64 size) {
    if (jit_on <= 0 || !size) return;
    rt_jit_invalidate(addr, size);
    pthread_mutex_lock(&jit_lock);
    for (u64 pg = addr & ~(host_page - 1); pg < addr + size; pg += host_page) {
        JPage *p = find_page(pg);
        if (!p) continue;
        // forget the page: it may be mapped again (VirtualAlloc reuses addresses) and is then protected afresh
        if (!(pg >= addr && pg + host_page <= addr + size) && p->state == PG_PROT && (p->orig_prot & VM_PROT_WRITE))
            mprotect((void *)pg, host_page, p->orig_prot);
        p->state = PG_NONE; p->faults = 0; p->gen++;
    }
    pthread_mutex_unlock(&jit_lock);
}

// ---- configuration ----------------------------------------------------------------------------------------
static void *stats_thread(void *p);
static void jit_init(void) {
    const char *e = getenv("WINTOMAC_JIT");
    int on = !(e && e[0] == '0');
    if (rt_interp_disabled()) on = 0;
    const char *t = getenv("WINTOMAC_JIT_THRESHOLD");
    if (t) jit_threshold = atoi(t);
    rt_jit_stat_trace = getenv("WINTOMAC_JIT_TRACE") && getenv("WINTOMAC_JIT_TRACE")[0] != '0';
    host_page = (u64)getpagesize();
    const char *cm = getenv("WINTOMAC_JIT_CACHE_KB");
    if (cm && atoi(cm) >= 256 && atoi(cm) <= 120 * 1024) CHUNK_SIZE = (u64)atoi(cm) << 10;
    if (on) {
        fast = calloc(FAST_N, sizeof *fast);
        if (!fast || !new_chunk()) on = 0;
    }
    if (on) {
        extern void rt_jit_install_handler(void);
        rt_jit_install_handler();
        if (rt_jit_stat_trace) {
            atexit(rt_jit_dump_stats);
            rt_log("jit: on, threshold %d (statistics in wintomac_jit.txt)", jit_threshold);
            pthread_t t;
            if (pthread_create(&t, NULL, stats_thread, NULL) == 0) pthread_detach(t);
        }
    }
    __atomic_store_n(&jit_on, on, __ATOMIC_RELEASE);
}
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
int rt_jit_enabled(void) {
    if (__builtin_expect(jit_on < 0, 0)) pthread_once(&init_once, jit_init);
    return jit_on;
}

// ---- compilation ------------------------------------------------------------------------------------------
extern int jit_compile_trace(JComp *jc, u64 pc, int checked);   // jit_x86.c: decode + translate into jc->a
static JInsn insn_buf[JMAX_INSN];
#define NOJIT_N 4096
static u64 nojit[NOJIT_N];   // block starts whose traces are left to the interpreter (mostly untranslatable)

static u64 now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec; }

static JBlock *compile(u64 pc) {
    u64 t0 = rt_jit_stat_trace ? now_ns() : 0;
    pthread_mutex_lock(&jit_lock);
    JBlock *b = find_block(pc);
    if (b) { pthread_mutex_unlock(&jit_lock); return b; }
    // Protect the page(s) first, then read the code: a write that races with decoding either faults (and bumps
    // the generation, so we discard this translation) or lands before we decode.
    u64 pg0 = pc & ~(host_page - 1);
    JPage *p0 = get_page(pg0), *p1 = NULL;
    page_prepare(p0);
    u64 gen0 = p0->gen;
    if ((u64)(chunk.end - chunk.cur) < (128u << 10) && !new_chunk()) { pthread_mutex_unlock(&jit_lock); return NULL; }
    JComp *jc = calloc(1, sizeof *jc);
    jc->ins = insn_buf;
    jc->trace_mode = rt_jit_stat_trace;
    jc->reject_fallback_heavy = jit_threshold > 0;   // (threshold 0 is the test mode: translate everything)
    int checked = p0->state == PG_VOLATILE;
    wx_begin();
    for (int attempt = 0; attempt < 2; attempt++) {
        jc->a = (A64){ (u32 *)chunk.cur, (u32 *)chunk.cur, (u32 *)chunk.end, 0 };
        jc->nexit = 0; jc->n = 0; jc->fail = 0;
        b = calloc(1, sizeof *b);
        b->pc = pc;
        jc->pc = pc;
        if (rt_jit_stat_trace) b->counter = calloc(1, sizeof(u64));
        jc->counter = b->counter;
        int ok = jit_compile_trace(jc, pc, checked);
        if (!ok) jc->fail = 1;
        if (ok && jc->n) {
            u64 end = jc->ins[jc->n - 1].pc + jc->ins[jc->n - 1].in.length;
            u64 pg1 = (end - 1) & ~(host_page - 1);
            if (pg1 != pg0 && !p1) {
                p1 = get_page(pg1);
                page_prepare(p1);
                if (p1->state == PG_VOLATILE && !checked) { checked = 1; free(b->counter); free(b); continue; }   // retranslate with entry check
            }
        }
        break;
    }
    if (!b || jc->fail || !jc->n || jc->a.overflow) {
        pthread_jit_write_protect_np(1);
        if (jc->rejected) { __atomic_store_n(&nojit[(pc * 0x9E3779B97F4A7C15ull) >> 52], pc, __ATOMIC_RELAXED); st.rejected++; }
        pthread_mutex_unlock(&jit_lock);
        if (b) { free(b->counter); free(b); }
        free(jc);
        return NULL;
    }
    u64 gen1 = p1 ? p1->gen : 0;
    (void)gen1;
    b->code = jc->entry;
    b->stale = jc->stale;
    b->gate = jc->gate; b->gate_stale = jc->gate_stale;
    b->end = jc->ins[jc->n - 1].pc + jc->ins[jc->n - 1].in.length;
    b->ninsn = (u32)jc->n;
    for (int i = 0; i < jc->n; i++) b->nfallback += jc->ins[i].fallback;
    b->checked = (u8)checked;
    u32 *endp = a64_here(&jc->a);
    b->size = (u32)((u8 *)endp - (u8 *)jc->entry);
    wx_end(jc->entry, b->size);
    if (p0->gen != gen0) {   // the page was written while we decoded: drop
        pthread_mutex_unlock(&jit_lock);
        free(b->counter); free(b); free(jc);
        return NULL;
    }
    chunk.cur = (u8 *)(((u64)endp + 15) & ~15ull);
    b->page[0] = pg0; b->pnext[0] = p0->blocks; p0->blocks = b;
    if (p1 && p1 != p0) { b->page[1] = p1->pg; b->pnext[1] = p1->blocks; p1->blocks = b; }
    else b->page[1] = ~0ull;
    b->hnext = bhash[bh_idx(pc)];
    b->all = all_blocks; all_blocks = b;
    __atomic_store_n(&bhash[bh_idx(pc)], b, __ATOMIC_RELEASE);
    st.blocks++; st.insns += b->ninsn; st.fallback_static += b->nfallback; st.code_bytes += b->size;
    pthread_mutex_unlock(&jit_lock);
    free(jc);
    static int dump = -1;
    if (dump < 0) dump = getenv("WINTOMAC_JIT_DUMP") != NULL;
    if (dump) {   // host code of every block as an assembler file (assemble with clang -c, read with objdump -d)
        char path[128]; snprintf(path, sizeof path, "wintomac_jit_%llx.s", (unsigned long long)b->pc);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "// guest %llx-%llx, %u insns\n.text\n", (unsigned long long)b->pc, (unsigned long long)b->end, b->ninsn);
            for (u32 i = 0; i < b->size / 4; i++) fprintf(f, ".word 0x%08x\n", b->code[i]);
            fclose(f);
        }
    }
    if (rt_jit_stat_trace) {
        st.compile_ns += now_ns() - t0;
        if (rt_trace_enabled()) rt_log("jit: block %llx-%llx: %u insns (%u interpreted), %u bytes%s", (unsigned long long)b->pc,
                                       (unsigned long long)b->end, b->ninsn, b->nfallback, b->size, b->checked ? ", self-checking" : "");
    }
    return b;
}

// Execution counts of untranslated block entries (racy by design: an approximate count is enough).
#define HOT_N 16384
static struct { u64 pc; u32 n; } hot[HOT_N];
static int is_hot(u64 pc) {
    if (jit_threshold <= 0) return 1;
    if (__atomic_load_n(&nojit[(pc * 0x9E3779B97F4A7C15ull) >> 52], __ATOMIC_RELAXED) == pc) return 0;
    u32 i = (u32)((pc * 0x9E3779B97F4A7C15ull) >> 50);
    if (hot[i].pc != pc) { hot[i].pc = pc; hot[i].n = 0; }
    return ++hot[i].n >= (u32)jit_threshold;
}

static JBlock *lookup_or_compile(u64 pc) {
    JBlock *b = find_block(pc);
    if (b) return b;
    if (!is_hot(pc)) return NULL;
    return compile(pc);
}

static void link_site(u32 *site, JBlock *t) {
    int64_t d = (u8 *)t->code - (u8 *)site;
    if (d >= (128ll << 20) || d < -(128ll << 20)) return;   // different chunk, out of b range
    pthread_mutex_lock(&jit_lock);
    if (!t->dead && *site == a64_b_enc(site, site + 1)) {   // not linked yet (still `b .+4`)
        JLink *l = malloc(sizeof *l);
        l->site = site; l->orig = *site; l->next = t->in; t->in = l;
        patch_word(site, a64_b_enc(site, t->code));
        st.links++;
    }
    pthread_mutex_unlock(&jit_lock);
}
static void fast_insert(JBlock *b) {
    pthread_mutex_lock(&jit_lock);
    if (!b->dead) fast_store(&fast[fast_idx(b->pc)], b->pc, (u64)b->code);
    pthread_mutex_unlock(&jit_lock);
}

// ---- the dispatcher -------------------------------------------------------------------------------------------
u64 rt_interp_get_rflags(IState *s);
void rt_interp_set_rflags(IState *s, u64 f);

int rt_jit_run(IState *s) {
    if (rt_jit_stat_trace) STAT(interp_boundaries);
    JBlock *b = lookup_or_compile(s->pc);
    if (!b) return X_NEXT;
    Ctx *c = s->c;
    JState js;
    js.c = c; js.entry_rsp = s->entry_rsp; js.df = s->df; js.rflags = rt_interp_get_rflags(s) & F6;
    int ret = X_NEXT;
    for (;;) {
        if (rt_jit_stat_trace) STAT(entries);
        u32 r = rt_jit_stubs.enter(c, &js, b->code);
        js.rflags = rt_jit_flags(js.fop, js.fa, js.fres);
        u64 t = js.exit_pc;
        if (r == EXIT_HOSTRET) { ret = X_HOST_RET; break; }
        if (r == EXIT_INTERP) { s->pc = t; break; }
        if (r == EXIT_STALE) {
            if (rt_jit_stat_trace) STAT(stale_exits);
            JBlock *ob = find_block(t);
            if (ob && ob->checked) {   // a self-checking block whose bytes changed
                pthread_mutex_lock(&jit_lock);
                kill_block(ob);
                pthread_mutex_unlock(&jit_lock);
            }
        }
        if (r == EXIT_LINK || r == EXIT_JMP) {
            // jump semantics (interp.c do_jump): a native target is a tail call that pops [rsp] when it returns
            if (!rt_interp_is_exec(t) && rt_jit_is_native(t)) {
                u64 ra = LDS64(c->r[RSP]);
                if (rt_try_native(c, t)) {
                    if (c->r[RSP] > js.entry_rsp) { ret = X_HOST_RET; break; }
                    t = ra;
                    r = EXIT_RET;
                }
            }
        }
        JBlock *nb = lookup_or_compile(t);
        if (!nb) { s->pc = t; break; }
        if (r == EXIT_LINK && js.exit_aux) link_site((u32 *)js.exit_aux, nb);
        else if (r != EXIT_STALE) { if (rt_jit_stat_trace) STAT(fast_miss); fast_insert(nb); }
        b = nb;
    }
    rt_interp_set_rflags(s, js.rflags);
    s->df = js.df;
    return ret;
}

// ---- helpers called from translated code ---------------------------------------------------------------------
u64 rt_jit_h_interp(JState *js, u64 pc, u64 fop, u64 fa, u64 fres) {
    IState s;
    memset(&s, 0, sizeof s);
    s.c = js->c; s.pc = pc; s.entry_rsp = js->entry_rsp;
    rt_interp_set_rflags(&s, rt_jit_flags(fop, fa, fres));
    s.df = js->df;
    __atomic_fetch_add(&st.fallback_dyn, 1, __ATOMIC_RELAXED);
    int r = rt_interp_step(&s);
    js->rflags = rt_interp_get_rflags(&s) & F6;
    js->df = s.df;
    if (r == X_HOST_RET) return EXIT_HOSTRET;
    if (s.pc != s.next) { js->exit_pc = s.pc; return EXIT_RET; }
    return 0;
}
// Native targets of indirect calls/jumps get a stub in the fast table, so the next transfer to them doesn't
// leave translated code: the stub calls the recompiled function (or import) with the guest state synced, and
// then continues at the return address it popped, like a `jmp` to native code (a `call` pushed that address).
#define NS_N 1024
static struct { u64 t; u32 *stub; } nstubs[NS_N];
static void native_stub(u64 t) {
    pthread_mutex_lock(&jit_lock);
    u32 h = (u32)((t * 0x9E3779B97F4A7C15ull) >> 54);
    u32 *stub = nstubs[h].t == t ? nstubs[h].stub : NULL;
    if (!stub && (u64)(chunk.end - chunk.cur) > 4096) {
        GuestFn fn = rt_lookup_fn(t);
        wx_begin();
        A64 A = { (u32 *)chunk.cur, (u32 *)chunk.cur, (u32 *)chunk.end, 0 }, *a = &A;
        stub = a64_here(a);
        a64_mov(a, 1, 0, RCTX);
        if (fn) a64_mov_imm(a, T16, (u64)fn);
        else { a64_mov_imm(a, 1, t); a64_mov_imm(a, T16, (u64)rt_try_native); }
        a64_bl(a, chunk.stubs.call_c);
        a64_ldst_simm(a, 3, 1, 0, GR(RSP), -8, 0);          // ldur x0, [rsp - 8]: the return address it popped
        a64_ldr(a, 1, T3, XSP, FR_ENTRY_RSP);
        a64_cmp(a, 1, GR(RSP), T3);
        a64_put(a, 0x54000000u | (3u << 5) | C_HI);          // b.hi +3: returned past our host caller's frame
        a64_movz(a, 1, 2, EXIT_RET, 0);
        a64_b(a, chunk.stubs.lookup);
        a64_movz(a, 1, 2, EXIT_HOSTRET, 0);
        a64_b(a, chunk.stubs.exit);
        wx_end(stub, (u8 *)a->p - (u8 *)stub);
        chunk.cur = (u8 *)(((u64)a->p + 15) & ~15ull);
        nstubs[h].t = t; nstubs[h].stub = stub;
    }
    if (stub) {
        FastEnt *f = &fast[fast_idx(t)];
        fast_store(f, t, (u64)stub);
    }
    pthread_mutex_unlock(&jit_lock);
}

// call to a target that isn't in the fast table: run it natively if it is a recompiled function / import
// (return address already pushed; the native callee pops it). 1: done, continue after the call.
u64 rt_jit_h_call(JState *js, u64 target) {
    Ctx *c = js->c;
    if (!rt_interp_is_exec(target) && rt_jit_is_native(target)) {
        native_stub(target);
        if (rt_try_native(c, target)) return 1;
    }
    js->exit_pc = target;
    return 0;
}
// jmp to a target that isn't in the fast table: a native target is a tail call (it pops [rsp] when it returns).
// 0: not native (dispatch to the target), 1: done, continue at exit_pc (the popped return address), 2: host return
u64 rt_jit_h_native_jmp(JState *js, u64 target) {
    Ctx *c = js->c;
    js->exit_pc = target;
    if (rt_interp_is_exec(target)) return 0;
    u64 ra = LDS64(c->r[RSP]);
    if (!rt_jit_is_native(target)) return 0;
    native_stub(target);
    if (!rt_try_native(c, target)) return 0;
    if (c->r[RSP] > js->entry_rsp) return 2;
    js->exit_pc = ra;
    return 1;
}

// ---- lazy flags -------------------------------------------------------------------------------------------------
u64 rt_jit_flags(u64 op, u64 a, u64 res) {
    int kind = (int)((op >> 2) & 63), sz = (int)(op & 3), w = 8 << sz;
    u64 aux = op >> 8;
    if (kind == JF_MAT) return res & F6;
    u64 m = imask(w), sign = 1ull << (w - 1), r = res & m, b;
    a &= m;
    u64 cf = 0, of = 0, af = 0;
    switch (kind) {
    case JF_ADD: b = (r - a) & m; cf = r < a; of = (((a ^ r) & (b ^ r)) & sign) != 0; af = ((a ^ b ^ r) >> 4) & 1; break;
    case JF_SUB: b = (a - r) & m; cf = a < b; of = (((a ^ b) & (a ^ r)) & sign) != 0; af = ((a ^ b ^ r) >> 4) & 1; break;
    case JF_LOGIC: break;
    case JF_INC: cf = aux & 1; of = r == sign; af = ((a ^ r) >> 4) & 1; break;
    case JF_DEC: cf = aux & 1; of = a == sign; af = ((a ^ r) >> 4) & 1; break;
    case JF_ADC: { u64 ci = aux & 1; b = (r - a - ci) & m; cf = ci ? r <= a : r < a; of = (((a ^ r) & (b ^ r)) & sign) != 0; af = ((a ^ b ^ r) >> 4) & 1; break; }
    case JF_SBB: { u64 ci = aux & 1; b = (a - r - ci) & m; cf = ci ? a <= b : a < b; of = (((a ^ b) & (a ^ r)) & sign) != 0; af = ((a ^ b ^ r) >> 4) & 1; break; }
    case JF_SHL: { u64 n = aux & 0xFF; cf = n <= (u64)w ? (a >> (w - n)) & 1 : 0; of = ((r >> (w - 1)) & 1) ^ cf; break; }
    case JF_SHR: { u64 n = aux & 0xFF; cf = n <= (u64)w ? (a >> (n - 1)) & 1 : 0; of = (a >> (w - 1)) & 1; break; }
    case JF_SAR: { u64 n = aux & 0xFF; i64 sa = (i64)sext(a, w); cf = (sa >> (n - 1 < 63 ? n - 1 : 63)) & 1; break; }
    case JF_MUL: cf = of = aux & 1; break;
    default: break;
    }
    u64 zf = r == 0, sf = (r >> (w - 1)) & 1, pf = PAR(r);
    return cf | pf << 2 | af << 4 | zf << 6 | sf << 7 | of << 11;
}

// ---- faults: writes to translated pages, misaligned RCpc accesses ----------------------------------------------
static struct sigaction old_segv, old_bus;

static int in_code_cache(u64 pc) {
    if (chunk.base && pc >= (u64)chunk.base && pc < (u64)chunk.end) return 1;
    for (int i = 0; i < nchunks_seen; i++) if (pc >= (u64)chunks_seen[i].lo && pc < (u64)chunks_seen[i].hi) return 1;
    return 0;
}

// A misaligned LDAPUR/STLUR/LDAPR/STLR crossing a 16-byte boundary (FEAT_LSE2 only covers accesses inside one):
// emulate it, and patch the site to an out-of-line plain access with explicit barriers (keeps TSO ordering).
static int fix_misaligned(ucontext_t *uc) {
    u64 pc = uc->uc_mcontext->__ss.__pc;
    if (!in_code_cache(pc)) return 0;
    u32 w = *(u32 *)pc;
    int size = (int)(w >> 30), rt = (int)(w & 31), rn = (int)((w >> 5) & 31), load;
    i64 imm = 0;
    if ((w & 0x3F200C00u) == 0x19000000u && ((w >> 22) & 3) <= 1) { load = (w >> 22) & 1; imm = (i64)((u64)((w >> 12) & 0x1FF) << 55) >> 55; }
    else if ((w & 0x3FFFFC00u) == 0x38BFC000u) load = 1;
    else if ((w & 0x3FFFFC00u) == 0x089FFC00u) load = 0;
    else return 0;
    if (rn == 31) return 0;
    u64 *X = uc->uc_mcontext->__ss.__x;   // x0..x28; x29 = fp, x30 = lr follow in the struct
    u64 base = rn == 29 ? uc->uc_mcontext->__ss.__fp : rn == 30 ? uc->uc_mcontext->__ss.__lr : X[rn];
    u64 addr = base + (u64)imm, n = 1ull << size;
    if (load) {
        u64 v = 0; memcpy(&v, (void *)addr, n);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (rt < 29) X[rt] = v; else if (rt == 29) uc->uc_mcontext->__ss.__fp = v; else if (rt == 30) uc->uc_mcontext->__ss.__lr = v;
    } else {
        u64 v = rt == 31 ? 0 : rt < 29 ? X[rt] : rt == 29 ? uc->uc_mcontext->__ss.__fp : uc->uc_mcontext->__ss.__lr;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        memcpy((void *)addr, &v, n);
    }
    uc->uc_mcontext->__ss.__pc = pc + 4;
    STAT(align_faults);
    // patch: b stub; stub = [dmb ish] ldur/stur [dmb ishld]; b pc+4
    if (pthread_mutex_trylock(&jit_lock) == 0) {
        if (*(u32 *)pc == w && (u64)(chunk.end - chunk.cur) > 64 && pc >= (u64)chunk.base && pc < (u64)chunk.end) {
            wx_begin();
            A64 A = { (u32 *)chunk.cur, (u32 *)chunk.cur, (u32 *)chunk.end, 0 }, *a = &A;
            if (!load) a64_dmb_ish(a);
            a64_ldst_simm(a, size, load, rt, rn, (int)imm, 0);
            if (load) a64_dmb_ishld(a);
            a64_b(a, (u32 *)pc + 1);
            u32 *stub = (u32 *)chunk.cur;
            *(u32 *)pc = a64_b_enc((u32 *)pc, stub);
            sys_icache_invalidate(stub, (u8 *)a->p - (u8 *)stub);
            sys_icache_invalidate((void *)pc, 4);
            chunk.cur = (u8 *)(((u64)a->p + 15) & ~15ull);
            pthread_jit_write_protect_np(1);
            STAT(align_patches);
        }
        pthread_mutex_unlock(&jit_lock);
    }
    return 1;
}

int rt_jit_handle_fault(int sig, siginfo_t *si, void *ucv) {
    if (jit_on <= 0) return 0;
    ucontext_t *uc = ucv;
    u32 esr = uc ? (u32)uc->uc_mcontext->__es.__esr : 0;
    u32 ec = esr >> 26, dfsc = esr & 0x3F;
    if (uc && (ec == 0x24 || ec == 0x25) && dfsc == 0x21) return fix_misaligned(uc);   // alignment fault
    u64 addr = (u64)si->si_addr, pg = addr & ~(host_page - 1);
    JPage *p = find_page(pg);
    if (!p) return 0;
    if (uc && (ec == 0x24 || ec == 0x25) && (!(esr & (1u << 6)) || (dfsc & 0x3C) != 0x0C)) return 0;   // only write permission faults are ours
    pthread_mutex_lock(&jit_lock);
    int handled = 0;
    if (p->state == PG_PROT && (p->orig_prot & VM_PROT_WRITE)) {
        kill_page_blocks(p, pg, pg + host_page);
        p->gen++;
        p->faults++;
        STAT(write_faults);
        p->state = p->faults >= 8 ? PG_VOLATILE : PG_NONE;
        if (p->state == PG_VOLATILE) STAT(volatile_pages);
        handled = mprotect((void *)pg, host_page, p->orig_prot) == 0;
    } else if (p->state != PG_PROT && (p->orig_prot & VM_PROT_WRITE)) {
        // another thread unprotected it meanwhile: retry the store (if the page is still mapped)
        handled = mprotect((void *)pg, host_page, p->orig_prot) == 0;
    }
    pthread_mutex_unlock(&jit_lock);
    (void)sig;
    return handled;
}

static void jit_signal(int sig, siginfo_t *si, void *uc) {
    if (rt_jit_handle_fault(sig, si, uc)) return;
    ucontext_t *u = uc;
    if (u && in_code_cache(u->uc_mcontext->__ss.__pc)) {   // a guest fault in translated code: make Ctx current for the crash report
        Ctx *c = (Ctx *)u->uc_mcontext->__ss.__x[RCTX];
        if (c) for (int i = 0; i < 16; i++) c->r[i] = u->uc_mcontext->__ss.__x[GR(i)];
    }
    struct sigaction *old = sig == SIGBUS ? &old_bus : &old_segv;
    if (old->sa_flags & SA_SIGINFO) { if (old->sa_sigaction) { old->sa_sigaction(sig, si, uc); return; } }
    else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) { old->sa_handler(sig); return; }
    signal(sig, SIG_DFL);   // default action: re-executing the faulting instruction kills the process
}

void rt_jit_install_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = jit_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
}

// ---- statistics output ---------------------------------------------------------------------------------------------
static void stats_print(FILE *f) {
    u64 dyn = 0, dyn_fb = st.fallback_dyn;
    pthread_mutex_lock(&jit_lock);
    for (JBlock *b = all_blocks; b; b = b->all) if (b->counter) dyn += *b->counter * (b->ninsn - b->nfallback);
    pthread_mutex_unlock(&jit_lock);
    fprintf(f, "[jit] blocks %llu, guest insns translated %llu (%llu via interpreter, %.2f%%), code %llu KB in %llu chunk(s), compile %.1f ms\n",
            (unsigned long long)st.blocks, (unsigned long long)st.insns, (unsigned long long)st.fallback_static,
            st.insns ? 100.0 * st.fallback_static / st.insns : 0.0, (unsigned long long)(st.code_bytes >> 10), (unsigned long long)st.chunks, st.compile_ns / 1e6);
    fprintf(f, "[jit] executed ~%llu guest insns as translated code + %llu through the interpreter fallback (%.3f%% fallback)\n",
            (unsigned long long)dyn, (unsigned long long)dyn_fb, dyn + dyn_fb ? 100.0 * dyn_fb / (dyn + dyn_fb) : 0.0);
    fprintf(f, "[jit] world entries %llu, links %llu (unlinked %llu), indirect lookups that missed %llu, interpreter block boundaries %llu, traces left to the interpreter %llu\n",
            (unsigned long long)st.entries, (unsigned long long)st.links, (unsigned long long)st.unlinks, (unsigned long long)st.fast_miss,
            (unsigned long long)st.interp_boundaries, (unsigned long long)st.rejected);
    fprintf(f, "[jit] invalidated blocks %llu, write faults %llu, volatile pages %llu, stale exits %llu, misaligned RCpc faults %llu (sites patched %llu)\n",
            (unsigned long long)st.invalidated, (unsigned long long)st.write_faults, (unsigned long long)st.volatile_pages,
            (unsigned long long)st.stale_exits, (unsigned long long)st.align_faults, (unsigned long long)st.align_patches);
}
static void stats_file(void) {
    FILE *f = fopen("wintomac_jit.txt.tmp", "w");
    if (!f) return;
    stats_print(f);
    fclose(f);
    rename("wintomac_jit.txt.tmp", "wintomac_jit.txt");
}
// The runtime usually leaves through _exit (ExitProcess, the CRT's exit), so besides the final dump the
// statistics are kept current in wintomac_jit.txt.
static void *stats_thread(void *p) { (void)p; for (;;) { usleep(20000); stats_file(); } return NULL; }
void rt_jit_dump_stats(void) {
    if (!rt_jit_stat_trace || jit_on <= 0) return;
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_RELAXED)) return;
    stats_print(stderr);
    fflush(stderr);
    stats_file();
}

// ---- C call from translated code --------------------------------------------------------------------------------------
void jit_emit_call_c(JComp *jc, void *fn) {
    A64 *a = &jc->a;
    jit_xflush(jc);     // C code reads Ctx and clobbers v16-v31
    jc->xcached = 0;
    a64_mov_imm(a, T16, (u64)fn);
    a64_bl(a, rt_jit_stubs.call_c);
    jc->nzcv = 0;
}
