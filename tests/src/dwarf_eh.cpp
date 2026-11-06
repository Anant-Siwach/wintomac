// Itanium C++ ABI exceptions with DWARF CFI in a Windows PE (clang -fdwarf-exceptions, see Makefile), the way
// AnyPS5's libraries and relinked PS5 programs work: no SEH, the unwinder is ordinary guest code that walks .eh_frame,
// reads the LSDA (.gcc_except_table), restores registers and *jumps* into a landing pad (register restore + jmp, like
// AnyPS5 libc.prx's LibcRestoreRegisters). The minimal C++ runtime below (__cxa_throw, __cxa_begin_catch, _Unwind_Resume,
// a CFI interpreter and a personality) stands in for libc.prx, so the test needs nothing but the mingw CRT.
// Covers: cleanups across frames (-> _Unwind_Resume), catch by type and catch(...), rethrow, throw from a catch block,
// callee-saved registers restored at the landing pad, many throws (frame-record leaks).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef uint64_t u64;
typedef int64_t i64;

// ---------------------------------------------------------------- the test program
static int live;
struct Guard { const char *n; int quiet; Guard(const char *s, int q = 0) : n(s), quiet(q) { live++; } ~Guard() { live--; if (!quiet) printf("  ~Guard %s\n", n); } };
struct Boom { int code; };

__attribute__((noinline)) static void l3(int v) { Guard g("l3"); if (v == 1) throw v; if (v == 2) throw Boom{v * 100}; }
__attribute__((noinline)) static int l2(int v) { Guard g("l2"); l3(v); return 2; }
__attribute__((noinline)) static int l1(int v) { Guard g("l1"); return l2(v) + 1; }
__attribute__((noinline)) static void rethrower() { try { throw 42; } catch (int i) { printf("  inner caught %d, rethrowing\n", i); throw; } }
__attribute__((noinline)) static void throw_in_catch() { try { throw 5; } catch (int) { Guard g("in-catch"); throw Boom{7}; } }
__attribute__((noinline)) static long deep(int n) { Guard g("d", 1); if (!n) throw n; return deep(n - 1) + 1; }
// callee-saved registers live across the throwing call must have their values at the landing pad
__attribute__((noinline)) static long keep_regs(long a, long b) {
    long x = a * 3, y = b * 5, z = a ^ b, w = a + b;
    try { l1(1); } catch (int) { return x + y + z + w; }
    return -1;
}

// no LSDA (nothing to clean up) but an over-aligned local: an rbp frame (`and rsp,-64`) whose CFA is rbp-based, so
// the unwinder needs this frame's live rbp; the exception comes from a callback called through a pointer
typedef void (*Callback)(int);
__attribute__((noinline)) static void throw_cb(int v) { throw Boom{v}; }
static Callback volatile the_cb = throw_cb;
__attribute__((noinline)) static int aligned_mid(Callback f, int v) {
    alignas(64) volatile char buf[64];
    buf[0] = (char)v; f(v);
    return buf[0];
}

int main() {
    printf("DWARF C++ exceptions\n");
    printf("l1(0) = %d\n", l1(0));
    try { l1(1); } catch (int v) { printf("caught int %d live=%d\n", v, live); }
    try { l1(2); } catch (Boom &b) { printf("caught Boom %d live=%d\n", b.code, live); }
    try { l1(2); } catch (int) { printf("wrong\n"); } catch (...) { printf("caught (...) live=%d\n", live); }
    try { rethrower(); } catch (int i) { printf("outer caught rethrown %d\n", i); }
    try { throw_in_catch(); } catch (Boom &b) { printf("caught the new one %d live=%d\n", b.code, live); }
    printf("keep_regs = %ld\n", keep_regs(1000, 33));
    try { aligned_mid(the_cb, 9); } catch (Boom &b) { printf("caught Boom %d through an rbp-framed function\n", b.code); }
    long total = 0;
    for (int i = 0; i < 500; i++) { try { total += deep(i % 20); } catch (int n) { total += n + 1; } }
    printf("500 throws: total=%ld live=%d\n", total, live);
    return 0;
}

// ---------------------------------------------------------------- minimal Itanium C++ runtime
// Typeinfo symbols the compiler references (contents unused: types match by typeinfo address).
extern "C" { void *_ZTVN10__cxxabiv117__class_type_infoE[4]; const void *_ZTIi[2] = { 0, "i" }; }

// Registers in DWARF order: rax rdx rcx rbx rsi rdi rbp rsp r8..r15, [16] = return address / rip
struct Regs { u64 r[17]; };
extern "C" void cap_regs(Regs *);
extern "C" __attribute__((noreturn)) void restore_regs(const Regs *);
asm(".text\n.globl cap_regs\ncap_regs:\n"
    "movq %rax,0(%rcx)\nmovq %rdx,8(%rcx)\nmovq %rcx,16(%rcx)\nmovq %rbx,24(%rcx)\nmovq %rsi,32(%rcx)\nmovq %rdi,40(%rcx)\n"
    "movq %rbp,48(%rcx)\nleaq 8(%rsp),%rax\nmovq %rax,56(%rcx)\nmovq %r8,64(%rcx)\nmovq %r9,72(%rcx)\nmovq %r10,80(%rcx)\n"
    "movq %r11,88(%rcx)\nmovq %r12,96(%rcx)\nmovq %r13,104(%rcx)\nmovq %r14,112(%rcx)\nmovq %r15,120(%rcx)\n"
    "movq (%rsp),%rax\nmovq %rax,128(%rcx)\nret\n"
    ".globl restore_regs\nrestore_regs:\n"
    "movq %rcx,%r10\nmovq 128(%r10),%r11\nmovq 0(%r10),%rax\nmovq 8(%r10),%rdx\nmovq 16(%r10),%rcx\nmovq 24(%r10),%rbx\n"
    "movq 32(%r10),%rsi\nmovq 40(%r10),%rdi\nmovq 48(%r10),%rbp\nmovq 56(%r10),%rsp\nmovq 64(%r10),%r8\nmovq 72(%r10),%r9\n"
    "movq 96(%r10),%r12\nmovq 104(%r10),%r13\nmovq 112(%r10),%r14\nmovq 120(%r10),%r15\njmp *%r11\n");

static const uint8_t *eh_lo, *eh_hi;   // this image's .eh_frame
static void find_eh_frame() {
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(s[i].Name, ".ehfram", 8)) { eh_lo = base + s[i].VirtualAddress; eh_hi = eh_lo + s[i].Misc.VirtualSize; }
}
static u64 uleb(const uint8_t **p) { u64 r = 0; int sh = 0; uint8_t b; do { b = *(*p)++; r |= (u64)(b & 127) << sh; sh += 7; } while (b & 128); return r; }
static i64 sleb(const uint8_t **p) { u64 r = 0; int sh = 0; uint8_t b; do { b = *(*p)++; r |= (u64)(b & 127) << sh; sh += 7; } while (b & 128); if (sh < 64 && (b & 64)) r |= ~0ull << sh; return (i64)r; }
static u64 enc(const uint8_t **p, uint8_t e) {
    if (e == 0xff) return 0;
    const uint8_t *at = *p; u64 v;
    switch (e & 15) {
    case 0: case 4: case 12: memcpy(&v, at, 8); *p += 8; break;
    case 1: v = uleb(p); break;
    case 3: { uint32_t t; memcpy(&t, at, 4); v = t; *p += 4; break; }
    case 11: { int32_t t; memcpy(&t, at, 4); v = (u64)(i64)t; *p += 4; break; }
    case 9: v = (u64)sleb(p); break;
    default: printf("encoding %x\n", e); exit(9);
    }
    if (v && (e & 0x70) == 0x10) v += (u64)at;
    if (v && (e & 0x80)) v = *(u64 *)v;
    return v;
}

struct Frame { u64 start, end, lsda; i64 daf; const uint8_t *cie_ins, *cie_end, *ins, *ins_end; };
static int find_fde(u64 pc, Frame *f) {
    for (const uint8_t *p = eh_lo; p + 8 <= eh_hi; ) {
        uint32_t len; memcpy(&len, p, 4);
        if (!len) { p += 4; continue; }
        const uint8_t *rec = p + 4, *next = rec + len;
        uint32_t cie_off; memcpy(&cie_off, rec, 4);
        if (cie_off) {
            const uint8_t *cie = rec - cie_off, *c = cie + 9;   // length, id, version
            const char *aug = (const char *)c; c += strlen(aug) + 1;
            uleb(&c); i64 daf = sleb(&c); uleb(&c);
            uint8_t renc = 0, lenc = 0xff;
            uint32_t cie_len; memcpy(&cie_len, cie, 4);
            const uint8_t *cie_end = cie + 4 + cie_len;
            if (aug[0] == 'z') {
                u64 n = uleb(&c); const uint8_t *e = c + n;
                for (const char *a = aug + 1; *a; a++)
                    if (*a == 'R') renc = *c++; else if (*a == 'L') lenc = *c++; else if (*a == 'P') { uint8_t pe = *c++; enc(&c, pe); }
                c = e;
            }
            const uint8_t *q = rec + 4;
            u64 start = enc(&q, renc), range = enc(&q, renc & 15);
            if (pc >= start && pc < start + range) {
                u64 lsda = 0;
                if (aug[0] == 'z') { u64 n = uleb(&q); const uint8_t *e = q + n; if (lenc != 0xff) lsda = enc(&q, lenc); q = e; }
                *f = (Frame){ start, start + range, lsda, daf, c, cie_end, q, next };
                return 1;
            }
        }
        p = next;
    }
    return 0;
}

// One frame up: CFI rules at pc (registers 0..16 only; xmm saves are skipped).
struct Rule { int kind; i64 off; };
static void run_cfi(const uint8_t *p, const uint8_t *end, i64 daf, u64 loc, u64 pc, Rule *rules, int *cfa_reg, i64 *cfa_off, const Rule *init) {
    Rule stack[8][17]; int sreg[8]; i64 soff[8]; int depth = 0;
    while (p < end && loc <= pc) {
        uint8_t op = *p++;
        int reg;
        if ((op & 0xc0) == 0x40) { loc += op & 0x3f; continue; }
        if ((op & 0xc0) == 0x80) { reg = op & 0x3f; i64 o = (i64)uleb(&p) * daf; if (reg < 17) rules[reg] = (Rule){ 1, o }; continue; }
        if ((op & 0xc0) == 0xc0) { reg = op & 0x3f; if (reg < 17) rules[reg] = init[reg]; continue; }
        switch (op) {
        case 0: break;
        case 2: loc += *p++; break;
        case 3: { uint16_t d; memcpy(&d, p, 2); p += 2; loc += d; break; }
        case 4: { uint32_t d; memcpy(&d, p, 4); p += 4; loc += d; break; }
        case 5: { reg = (int)uleb(&p); i64 o = (i64)uleb(&p) * daf; if (reg < 17) rules[reg] = (Rule){ 1, o }; break; }
        case 0x11: { reg = (int)uleb(&p); i64 o = sleb(&p) * daf; if (reg < 17) rules[reg] = (Rule){ 1, o }; break; }
        case 6: reg = (int)uleb(&p); if (reg < 17) rules[reg] = init[reg]; break;
        case 7: case 8: reg = (int)uleb(&p); if (reg < 17) rules[reg] = (Rule){ 0, 0 }; break;
        case 10: memcpy(stack[depth], rules, sizeof stack[0]); sreg[depth] = *cfa_reg; soff[depth] = *cfa_off; depth++; break;
        case 11: depth--; memcpy(rules, stack[depth], sizeof stack[0]); *cfa_reg = sreg[depth]; *cfa_off = soff[depth]; break;
        case 12: *cfa_reg = (int)uleb(&p); *cfa_off = (i64)uleb(&p); break;
        case 13: *cfa_reg = (int)uleb(&p); break;
        case 14: *cfa_off = (i64)uleb(&p); break;
        case 0x12: *cfa_reg = (int)uleb(&p); *cfa_off = sleb(&p) * daf; break;
        case 0x13: *cfa_off = sleb(&p) * daf; break;
        case 0x2e: uleb(&p); break;
        default: printf("CFA op %x\n", op); exit(9);
        }
    }
}
static int step(Regs *r, Frame *f) {
    if (!find_fde(r->r[16] - 1, f)) return 0;
    Rule init[17] = {}, rules[17];
    int cfa_reg = 7; i64 cfa_off = 0;
    run_cfi(f->cie_ins, f->cie_end, f->daf, 0, ~0ull, init, &cfa_reg, &cfa_off, init);
    memcpy(rules, init, sizeof rules);
    run_cfi(f->ins, f->ins_end, f->daf, f->start, r->r[16] - 1, rules, &cfa_reg, &cfa_off, init);
    u64 cfa = r->r[cfa_reg] + cfa_off;
    Regs n = *r;
    for (int i = 0; i < 17; i++) if (rules[i].kind == 1) n.r[i] = *(u64 *)(cfa + rules[i].off);
    n.r[7] = cfa;
    *r = n;
    return 1;
}

// Exceptions: header + object. Only one exception in flight plus a stack of caught ones (enough here).
struct Exc { const void *type; void (*dtor)(void *); u64 target_cfa; int handler; Exc *next_caught; int rethrown; };
static Exc *caught;
static Exc *hdr(void *obj) { return (Exc *)obj - 1; }

// LSDA of frame f at pc: 0 = nothing, 1 = cleanup, 2 = handler (selector in *sel)
static int lsda_lookup(const Frame *f, u64 pc, const void *type, u64 *pad, int *sel) {
    if (!f->lsda) return 0;
    const uint8_t *p = (const uint8_t *)f->lsda;
    uint8_t lpenc = *p++; u64 lpstart = lpenc == 0xff ? f->start : enc(&p, lpenc);
    uint8_t ttenc = *p++; const uint8_t *ttbase = 0;
    if (ttenc != 0xff) { u64 o = uleb(&p); ttbase = p + o; }
    uint8_t csenc = *p++; u64 cslen = uleb(&p);
    const uint8_t *cs = p, *actions = p + cslen;
    while (cs < actions) {
        u64 s = enc(&cs, csenc), l = enc(&cs, csenc), lp = enc(&cs, csenc), a = uleb(&cs);
        if (pc < f->start + s || pc >= f->start + s + l) continue;
        if (!lp) return 0;
        *pad = lpstart + lp;
        if (!a) { *sel = 0; return 1; }
        const uint8_t *ap = actions + a - 1;
        int cleanup = 0;
        for (;;) {
            i64 filter = sleb(&ap); const uint8_t *here = ap; i64 next = sleb(&ap);
            if (filter > 0) {
                const uint8_t *tp = ttbase - filter * 4;   // sdata4-sized entries (ttenc & 15 == 0x0b/0x03)
                if ((ttenc & 15) == 0 || (ttenc & 15) == 4) tp = ttbase - filter * 8;
                u64 ti = enc(&tp, ttenc);
                if (!ti || (const void *)ti == type) { *sel = (int)filter; return 2; }
            } else if (filter == 0) cleanup = 1;
            if (!next) break;
            ap = here + next;
        }
        if (cleanup) { *sel = 0; return 1; }
        return 0;
    }
    return 0;
}

// phase 2 from `r` (already stepped out of the runtime): run cleanups, stop at the handler frame
static __attribute__((noreturn)) void phase2(Regs r, Exc *e) {
    for (;;) {
        Frame f;
        Regs at = r;
        if (!step(&r, &f)) { printf("phase 2: lost the frame chain\n"); exit(9); }
        u64 pad; int sel;
        int k = lsda_lookup(&f, at.r[16] - 1, e->type, &pad, &sel);
        if (k == 2 && r.r[7] != e->target_cfa) k = 0;   // a matching catch above the chosen handler frame: not ours
        if (k) {
            at.r[0] = (u64)(e + 1);   // rax: the exception (this runtime passes the object itself)
            at.r[1] = (u64)sel;
            at.r[16] = pad;
            restore_regs(&at);
        }
    }
}

extern "C" {
void *__cxa_allocate_exception(size_t n) { Exc *e = (Exc *)calloc(1, sizeof(Exc) + n); return e + 1; }
void __cxa_free_exception(void *o) { free(hdr(o)); }
__attribute__((noreturn)) void __cxa_throw(void *obj, void *type, void (*dtor)(void *)) {
    Exc *e = hdr(obj); e->type = type; e->dtor = dtor;
    if (!eh_lo) find_eh_frame();
    Regs start; cap_regs(&start);
    Frame f;
    Regs r = start;
    if (!step(&r, &f)) { printf("no unwind info for __cxa_throw\n"); exit(9); }   // out of __cxa_throw
    Regs from = r;
    for (;;) {   // phase 1: find the handler frame
        Regs at = r;
        if (!step(&r, &f)) { printf("terminate: unhandled exception\n"); exit(3); }
        u64 pad; int sel;
        if (lsda_lookup(&f, at.r[16] - 1, type, &pad, &sel) == 2) { e->target_cfa = r.r[7]; break; }
    }
    phase2(from, e);
}
__attribute__((noreturn)) void _Unwind_Resume(void *obj) {
    Regs r; cap_regs(&r);
    Frame f;
    if (!step(&r, &f)) { printf("_Unwind_Resume: no unwind info\n"); exit(9); }   // out of _Unwind_Resume
    phase2(r, hdr(obj));
}
void *__cxa_begin_catch(void *obj) { Exc *e = hdr(obj); e->next_caught = caught; caught = e; e->rethrown = 0; return obj; }
void __cxa_end_catch(void) {
    Exc *e = caught; if (!e) return;
    caught = e->next_caught;
    if (e->rethrown) return;   // still in flight
    if (e->dtor) e->dtor(e + 1);
    free(e);
}
__attribute__((noreturn)) void __cxa_rethrow(void) {
    Exc *e = caught; e->rethrown = 1;   // the catch block's own __cxa_end_catch cleanup pops it
    __cxa_throw(e + 1, (void *)e->type, e->dtor);
}
void __gxx_personality_v0(void) { printf("personality called\n"); exit(9); }   // the runtime above reads LSDAs itself
__attribute__((noreturn)) void _ZSt9terminatev(void) { printf("terminate\n"); exit(3); }
__attribute__((noreturn)) void __cxa_call_terminate(void *) { printf("terminate\n"); exit(3); }
}
