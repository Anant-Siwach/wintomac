// Internals shared by the interpreter's translation units (runtime/interp*.c). Not for generated code.
#pragma once
#define RT_TSO 1          // x86 ordering for guest memory accesses, like the generated code (acquire/release)
#include "win32.h"
#include "interp.h"
#include <Zydis/Zydis.h>

enum { RK_NONE, RK_GPR, RK_XMM, RK_YMM, RK_ST, RK_RIP, RK_SEG, RK_FLAGS, RK_MMX, RK_OTHER };
typedef struct { u8 kind, idx, hi; u16 bits; } IReg;
extern IReg rt_ireg[ZYDIS_REGISTER_MAX_VALUE + 1];

typedef struct IState {
    Ctx *c;
    u64 pc, next;          // current instruction, fall-through address
    u64 entry_rsp;         // rsp at entry: a `ret` that pops above it returns to the host caller
    u8 cf, pf, af, zf, sf, of, df;
    const ZydisDecodedInstruction *in;
    const ZydisDecodedOperand *op;   // operands (visible ones first)
} IState;

// Instruction handlers return one of these.
enum { X_NEXT, X_HOST_RET, X_UNIMPL };

#define IOP(s, i) (&(s)->op[i])
#define NVIS(s) ((s)->in->operand_count_visible)
#define IS_VEX(s) ((s)->in->encoding == ZYDIS_INSTRUCTION_ENCODING_VEX || (s)->in->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX)

static inline u64 imask(int w) { return w >= 64 ? ~0ull : (1ull << w) - 1; }
static inline u64 sext(u64 v, int w) { return w >= 64 ? v : (u64)((i64)(v << (64 - w)) >> (64 - w)); }

static inline u64 greg_rd(IState *s, ZydisRegister r) {
    IReg ri = rt_ireg[r];
    if (ri.kind == RK_RIP) return s->next;
    u64 v = s->c->r[ri.idx];
    if (ri.hi) return (v >> 8) & 0xFF;
    return v & imask(ri.bits);
}
static inline void greg_wr(IState *s, ZydisRegister r, u64 v) {
    IReg ri = rt_ireg[r];
    u64 *p = &s->c->r[ri.idx];
    if (ri.hi) *p = (*p & ~0xFF00ull) | ((v & 0xFF) << 8);
    else if (ri.bits == 64) *p = v;
    else if (ri.bits == 32) *p = (u32)v;
    else if (ri.bits == 16) *p = (*p & ~0xFFFFull) | (u16)v;
    else *p = (*p & ~0xFFull) | (u8)v;
}

static inline u64 iea(IState *s, const ZydisDecodedOperand *o) {
    u64 a = (u64)o->mem.disp.value;
    if (o->mem.base != ZYDIS_REGISTER_NONE) a += greg_rd(s, o->mem.base);
    if (o->mem.index != ZYDIS_REGISTER_NONE) a += greg_rd(s, o->mem.index) * o->mem.scale;
    if (s->in->address_width == 32) a = (u32)a;
    if (o->mem.segment == ZYDIS_REGISTER_GS) a += s->c->gs_base;
    else if (o->mem.segment == ZYDIS_REGISTER_FS) a += s->c->fs_base;
    return a;
}

static inline u64 ild(u64 a, int w) {
    switch (w) { case 8: return LD8(a); case 16: return LD16(a); case 32: return LD32(a); default: return LD64(a); }
}
static inline void ist(u64 a, int w, u64 v) {
    switch (w) { case 8: ST8(a, (u8)v); break; case 16: ST16(a, (u16)v); break; case 32: ST32(a, (u32)v); break; default: ST64(a, v); }
}

// Integer operand access (GPR, memory, immediate). Immediates come sign-extended from Zydis when signed.
static inline u64 rdi(IState *s, int i) {
    const ZydisDecodedOperand *o = IOP(s, i);
    switch (o->type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: return greg_rd(s, o->reg.value);
    case ZYDIS_OPERAND_TYPE_MEMORY: return ild(iea(s, o), o->size);
    case ZYDIS_OPERAND_TYPE_IMMEDIATE: return o->imm.value.u;
    default: return 0;
    }
}
static inline void wri(IState *s, int i, u64 v) {
    const ZydisDecodedOperand *o = IOP(s, i);
    if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) greg_wr(s, o->reg.value, v);
    else if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) ist(iea(s, o), o->size, v);
}

static inline void iszp(IState *s, u64 r, int w) {
    r &= imask(w);
    s->zf = r == 0; s->sf = (r >> (w - 1)) & 1; s->pf = PAR(r);
}

int rt_interp_vec(IState *s);     // interp_sse.c: SSE..SSE4.2, common AVX/AVX2/FMA
int rt_interp_x87(IState *s);     // interp_x87.c
