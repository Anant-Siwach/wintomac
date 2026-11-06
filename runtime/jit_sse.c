// wintomac runtime JIT: SSE/SSE2 (legacy encodings) -> NEON. Guest xmm registers live in Ctx (the JIT world
// keeps only GPRs in host registers); v0..v7 are scratch. VEX/AVX forms, x87 and the rarer SSE3+ instructions
// go through the interpreter (jit_emit_fallback).
//
// Floating-point results must match the interpreter (runtime/interp_sse.c), which follows x86 NaN rules (first
// NaN operand, quieted; "indefinite" = negative default NaN). ARM differs only when a NaN comes out, so
// arithmetic checks its result and re-runs the instruction in the interpreter when a lane is NaN (rare).
#include "jit.h"

#define A (&jc->a)
#define OPN(j, i) (&(j)->op[i])
#define ISREG(o) ((o)->type == ZYDIS_OPERAND_TYPE_REGISTER)
#define ISMEM(o) ((o)->type == ZYDIS_OPERAND_TYPE_MEMORY)
#define ISIMM(o) ((o)->type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
#define MN(j) ((j)->in.mnemonic)

static inline int xr(const ZydisDecodedOperand *o) {   // xmm register index, or -1
    if (!ISREG(o)) return -1;
    IReg r = rt_ireg[o->reg.value];
    return r.kind == RK_XMM ? r.idx : -1;
}
static inline int gp(const ZydisDecodedOperand *o) { return ISREG(o) && rt_ireg[o->reg.value].kind == RK_GPR && !rt_ireg[o->reg.value].hi; }
static inline int xm(const ZydisDecodedOperand *o) { return xr(o) >= 0 || ISMEM(o); }

// Guest xmm x is cached in v(16+x) within a trace (JComp.xcached/xdirty, written back by jit_xflush).
// ld_x copies it into a scratch register, st_x makes vt the new value of xmm x.
static int xget(JComp *jc, int x) {
    if (!(jc->xcached >> x & 1)) { a64_fldst_uimm(A, 4, 1, 16 + x, RCTX, (u32)CTX_X(x)); jc->xcached |= (u16)(1u << x); }
    return 16 + x;
}
static void ld_x(JComp *jc, int vt, int x) { a64_vmov(A, vt, xget(jc, x)); }
static int xdst(JComp *jc, int x) { jc->xcached |= (u16)(1u << x); jc->xdirty |= (u16)(1u << x); return 16 + x; }
// operand i as a register: the cached xmm register itself (don't write to it), or memory loaded into scratch
static int src(JComp *jc, JInsn *j, int i, int scratch, int size) {
    const ZydisDecodedOperand *o = &j->op[i];
    if (o->type == ZYDIS_OPERAND_TYPE_REGISTER && rt_ireg[o->reg.value].kind == RK_XMM) return xget(jc, rt_ireg[o->reg.value].idx);
    int base, disp;
    jit_ea(jc, j, o, &base, &disp);
    a64_fldst_any(A, size, 1, scratch, base, disp, T17);
    return scratch;
}
static void st_x(JComp *jc, int vt, int x) {
    a64_vmov(A, 16 + x, vt);
    jc->xcached |= (u16)(1u << x); jc->xdirty |= (u16)(1u << x);
}
// load operand i (xmm or memory of `size` code: 2 s, 3 d, 4 q) into vt
static void ld_op(JComp *jc, JInsn *j, int i, int vt, int size) {
    const ZydisDecodedOperand *o = OPN(j, i);
    if (xr(o) >= 0) { ld_x(jc, vt, xr(o)); return; }
    int base, disp;
    jit_ea(jc, j, o, &base, &disp);
    a64_fldst_any(A, size, 1, vt, base, disp, T17);
}
static void st_mem(JComp *jc, JInsn *j, int i, int vt, int size) {
    int base, disp;
    jit_ea(jc, j, OPN(j, i), &base, &disp);
    a64_fldst_any(A, size, 0, vt, base, disp, T17);
}

typedef struct { u16 mn; u8 kind, arg, slow; } SseOp;
enum {
    K_IBIN = 1,   // integer/logic packed binary: v0 = op(dst, src)
    K_FBIN,       // float packed binary with NaN check (arg: 0 ps, 1 pd)
    K_FSCAL,      // float scalar binary with NaN check (arg: 0 ss, 1 sd)
    K_FMINMAX,    // packed/scalar min/max (arg bits: 1 double, 2 scalar, 4 max)
    K_SQRT,       // arg bits: 1 double, 2 scalar
    K_MOV,        // movaps & co (128-bit moves)
    K_PERM,       // unpck/punpck: zip1/zip2 (arg = op << 2 | size)
    K_PACK,       // arg: 0 sswb, 1 uswb, 2 ssdw
    K_SHIFTI,     // vector shift by immediate (arg = kind << 2 | size)
    K_OTHER,
};
#define IB(m, a) { ZYDIS_MNEMONIC_##m, K_IBIN, a, 0 }
enum { IB_ADD, IB_SUB, IB_SQADD, IB_UQADD, IB_SQSUB, IB_UQSUB, IB_CMEQ, IB_CMGT, IB_MUL, IB_UMAX, IB_UMIN, IB_SMAX, IB_SMIN, IB_URHADD,
       IB_AND, IB_ANDN, IB_OR, IB_XOR };
static const SseOp ops[] = {
    IB(PADDB, IB_ADD << 2 | 0), IB(PADDW, IB_ADD << 2 | 1), IB(PADDD, IB_ADD << 2 | 2), IB(PADDQ, IB_ADD << 2 | 3),
    IB(PSUBB, IB_SUB << 2 | 0), IB(PSUBW, IB_SUB << 2 | 1), IB(PSUBD, IB_SUB << 2 | 2), IB(PSUBQ, IB_SUB << 2 | 3),
    IB(PADDSB, IB_SQADD << 2 | 0), IB(PADDSW, IB_SQADD << 2 | 1), IB(PADDUSB, IB_UQADD << 2 | 0), IB(PADDUSW, IB_UQADD << 2 | 1),
    IB(PSUBSB, IB_SQSUB << 2 | 0), IB(PSUBSW, IB_SQSUB << 2 | 1), IB(PSUBUSB, IB_UQSUB << 2 | 0), IB(PSUBUSW, IB_UQSUB << 2 | 1),
    IB(PCMPEQB, IB_CMEQ << 2 | 0), IB(PCMPEQW, IB_CMEQ << 2 | 1), IB(PCMPEQD, IB_CMEQ << 2 | 2),
    IB(PCMPGTB, IB_CMGT << 2 | 0), IB(PCMPGTW, IB_CMGT << 2 | 1), IB(PCMPGTD, IB_CMGT << 2 | 2),
    IB(PMULLW, IB_MUL << 2 | 1), IB(PMAXUB, IB_UMAX << 2 | 0), IB(PMINUB, IB_UMIN << 2 | 0), IB(PMAXSW, IB_SMAX << 2 | 1),
    IB(PMINSW, IB_SMIN << 2 | 1), IB(PAVGB, IB_URHADD << 2 | 0), IB(PAVGW, IB_URHADD << 2 | 1),
    IB(PAND, IB_AND << 2), IB(ANDPS, IB_AND << 2), IB(ANDPD, IB_AND << 2), IB(PANDN, IB_ANDN << 2), IB(ANDNPS, IB_ANDN << 2),
    IB(ANDNPD, IB_ANDN << 2), IB(POR, IB_OR << 2), IB(ORPS, IB_OR << 2), IB(ORPD, IB_OR << 2), IB(PXOR, IB_XOR << 2),
    IB(XORPS, IB_XOR << 2), IB(XORPD, IB_XOR << 2),
    { ZYDIS_MNEMONIC_ADDPS, K_FBIN, FP2_ADD << 1 | 0, 1 }, { ZYDIS_MNEMONIC_ADDPD, K_FBIN, FP2_ADD << 1 | 1, 1 },
    { ZYDIS_MNEMONIC_SUBPS, K_FBIN, FP2_SUB << 1 | 0, 1 }, { ZYDIS_MNEMONIC_SUBPD, K_FBIN, FP2_SUB << 1 | 1, 1 },
    { ZYDIS_MNEMONIC_MULPS, K_FBIN, FP2_MUL << 1 | 0, 1 }, { ZYDIS_MNEMONIC_MULPD, K_FBIN, FP2_MUL << 1 | 1, 1 },
    { ZYDIS_MNEMONIC_DIVPS, K_FBIN, FP2_DIV << 1 | 0, 1 }, { ZYDIS_MNEMONIC_DIVPD, K_FBIN, FP2_DIV << 1 | 1, 1 },
    { ZYDIS_MNEMONIC_ADDSS, K_FSCAL, FP2_ADD << 1 | 0, 1 }, { ZYDIS_MNEMONIC_ADDSD, K_FSCAL, FP2_ADD << 1 | 1, 1 },
    { ZYDIS_MNEMONIC_SUBSS, K_FSCAL, FP2_SUB << 1 | 0, 1 }, { ZYDIS_MNEMONIC_SUBSD, K_FSCAL, FP2_SUB << 1 | 1, 1 },
    { ZYDIS_MNEMONIC_MULSS, K_FSCAL, FP2_MUL << 1 | 0, 1 }, { ZYDIS_MNEMONIC_MULSD, K_FSCAL, FP2_MUL << 1 | 1, 1 },
    { ZYDIS_MNEMONIC_DIVSS, K_FSCAL, FP2_DIV << 1 | 0, 1 }, { ZYDIS_MNEMONIC_DIVSD, K_FSCAL, FP2_DIV << 1 | 1, 1 },
    { ZYDIS_MNEMONIC_MINPS, K_FMINMAX, 0, 0 }, { ZYDIS_MNEMONIC_MINPD, K_FMINMAX, 1, 0 }, { ZYDIS_MNEMONIC_MINSS, K_FMINMAX, 2, 0 },
    { ZYDIS_MNEMONIC_MINSD, K_FMINMAX, 3, 0 }, { ZYDIS_MNEMONIC_MAXPS, K_FMINMAX, 4, 0 }, { ZYDIS_MNEMONIC_MAXPD, K_FMINMAX, 5, 0 },
    { ZYDIS_MNEMONIC_MAXSS, K_FMINMAX, 6, 0 }, { ZYDIS_MNEMONIC_MAXSD, K_FMINMAX, 7, 0 },
    { ZYDIS_MNEMONIC_SQRTPS, K_SQRT, 0, 1 }, { ZYDIS_MNEMONIC_SQRTPD, K_SQRT, 1, 1 }, { ZYDIS_MNEMONIC_SQRTSS, K_SQRT, 2, 1 },
    { ZYDIS_MNEMONIC_SQRTSD, K_SQRT, 3, 1 },
    { ZYDIS_MNEMONIC_MOVAPS, K_MOV, 0, 0 }, { ZYDIS_MNEMONIC_MOVUPS, K_MOV, 0, 0 }, { ZYDIS_MNEMONIC_MOVAPD, K_MOV, 0, 0 },
    { ZYDIS_MNEMONIC_MOVUPD, K_MOV, 0, 0 }, { ZYDIS_MNEMONIC_MOVDQA, K_MOV, 0, 0 }, { ZYDIS_MNEMONIC_MOVDQU, K_MOV, 0, 0 },
    { ZYDIS_MNEMONIC_LDDQU, K_MOV, 0, 0 }, { ZYDIS_MNEMONIC_MOVNTPS, K_MOV, 0, 0 }, { ZYDIS_MNEMONIC_MOVNTPD, K_MOV, 0, 0 },
    { ZYDIS_MNEMONIC_MOVNTDQ, K_MOV, 0, 0 },
    { ZYDIS_MNEMONIC_UNPCKLPS, K_PERM, 3 << 2 | 2, 0 }, { ZYDIS_MNEMONIC_UNPCKHPS, K_PERM, 7 << 2 | 2, 0 },
    { ZYDIS_MNEMONIC_UNPCKLPD, K_PERM, 3 << 2 | 3, 0 }, { ZYDIS_MNEMONIC_UNPCKHPD, K_PERM, 7 << 2 | 3, 0 },
    { ZYDIS_MNEMONIC_PUNPCKLBW, K_PERM, 3 << 2 | 0, 0 }, { ZYDIS_MNEMONIC_PUNPCKHBW, K_PERM, 7 << 2 | 0, 0 },
    { ZYDIS_MNEMONIC_PUNPCKLWD, K_PERM, 3 << 2 | 1, 0 }, { ZYDIS_MNEMONIC_PUNPCKHWD, K_PERM, 7 << 2 | 1, 0 },
    { ZYDIS_MNEMONIC_PUNPCKLDQ, K_PERM, 3 << 2 | 2, 0 }, { ZYDIS_MNEMONIC_PUNPCKHDQ, K_PERM, 7 << 2 | 2, 0 },
    { ZYDIS_MNEMONIC_PUNPCKLQDQ, K_PERM, 3 << 2 | 3, 0 }, { ZYDIS_MNEMONIC_PUNPCKHQDQ, K_PERM, 7 << 2 | 3, 0 },
    { ZYDIS_MNEMONIC_PACKSSWB, K_PACK, 0, 0 }, { ZYDIS_MNEMONIC_PACKUSWB, K_PACK, 1, 0 }, { ZYDIS_MNEMONIC_PACKSSDW, K_PACK, 2, 0 },
    // shift kinds: 0 shl, 1 lshr, 2 ashr, 3 byte shift left (pslldq), 4 byte shift right (psrldq)
    { ZYDIS_MNEMONIC_PSLLW, K_SHIFTI, 0 << 2 | 1, 0 }, { ZYDIS_MNEMONIC_PSLLD, K_SHIFTI, 0 << 2 | 2, 0 }, { ZYDIS_MNEMONIC_PSLLQ, K_SHIFTI, 0 << 2 | 3, 0 },
    { ZYDIS_MNEMONIC_PSRLW, K_SHIFTI, 1 << 2 | 1, 0 }, { ZYDIS_MNEMONIC_PSRLD, K_SHIFTI, 1 << 2 | 2, 0 }, { ZYDIS_MNEMONIC_PSRLQ, K_SHIFTI, 1 << 2 | 3, 0 },
    { ZYDIS_MNEMONIC_PSRAW, K_SHIFTI, 2 << 2 | 1, 0 }, { ZYDIS_MNEMONIC_PSRAD, K_SHIFTI, 2 << 2 | 2, 0 },
    { ZYDIS_MNEMONIC_PSLLDQ, K_SHIFTI, 3 << 2, 0 }, { ZYDIS_MNEMONIC_PSRLDQ, K_SHIFTI, 4 << 2, 0 },
    { ZYDIS_MNEMONIC_MOVSS, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_MOVSD, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_MOVD, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_MOVQ, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_MOVLPS, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_MOVLPD, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_MOVHPS, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_MOVHPD, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_MOVHLPS, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_MOVLHPS, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_PSHUFD, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_PSHUFLW, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_PSHUFHW, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_SHUFPS, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_SHUFPD, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_PEXTRW, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_PINSRW, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_PMOVMSKB, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_MOVMSKPS, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_MOVMSKPD, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_UCOMISS, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_UCOMISD, K_OTHER, 1, 0 }, { ZYDIS_MNEMONIC_COMISS, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_COMISD, K_OTHER, 1, 0 },
    { ZYDIS_MNEMONIC_CVTSI2SS, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_CVTSI2SD, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_CVTTSS2SI, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_CVTTSD2SI, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_CVTSS2SI, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_CVTSD2SI, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_CVTSS2SD, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_CVTSD2SS, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_CVTDQ2PS, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_CVTPS2PD, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_CVTPD2PS, K_OTHER, 0, 0 },
    { ZYDIS_MNEMONIC_CMPPS, K_OTHER, 0, 0 }, { ZYDIS_MNEMONIC_CMPPD, K_OTHER, 1, 0 }, { ZYDIS_MNEMONIC_CMPSS, K_OTHER, 2, 0 },
    { ZYDIS_MNEMONIC_CMPSD, K_OTHER, 3, 0 },
};
static const SseOp *find_op(ZydisMnemonic m) {
    static const SseOp *tab[ZYDIS_MNEMONIC_MAX_VALUE + 1];
    static int init;
    if (!init) { for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) tab[ops[i].mn] = &ops[i]; __atomic_store_n(&init, 1, __ATOMIC_RELEASE); }
    return tab[m];
}

int jit_sse_classify(const JInsn *j) {
    const ZydisDecodedInstruction *in = &j->in;
    if (in->encoding != ZYDIS_INSTRUCTION_ENCODING_LEGACY) return 0;
    if (in->meta.isa_ext != ZYDIS_ISA_EXT_SSE && in->meta.isa_ext != ZYDIS_ISA_EXT_SSE2) return 0;
    const SseOp *op = find_op(in->mnemonic);
    if (!op) return 0;
    const ZydisDecodedOperand *o0 = &j->op[0], *o1 = &j->op[1];
    int nv = in->operand_count_visible;
    for (int i = 0; i < nv; i++) {
        const ZydisDecodedOperand *o = &j->op[i];
        if (ISREG(o) && rt_ireg[o->reg.value].kind != RK_XMM && rt_ireg[o->reg.value].kind != RK_GPR) return 0;   // mmx
        if (ISMEM(o) && o->mem.segment != ZYDIS_REGISTER_DS && o->mem.segment != ZYDIS_REGISTER_SS && o->mem.segment != ZYDIS_REGISTER_FS &&
            o->mem.segment != ZYDIS_REGISTER_GS && o->mem.segment != ZYDIS_REGISTER_ES && o->mem.segment != ZYDIS_REGISTER_CS) return 0;
        if (ISMEM(o) && in->address_width != 64) return 0;
    }
    switch (op->kind) {
    case K_IBIN: case K_FBIN: case K_FSCAL: case K_FMINMAX: case K_SQRT: case K_PERM: case K_PACK:
        return nv == 2 && xr(o0) >= 0 && xm(o1);
    case K_MOV: return nv == 2 && ((xr(o0) >= 0 && xm(o1)) || (ISMEM(o0) && xr(o1) >= 0));
    case K_SHIFTI: return nv == 2 && xr(o0) >= 0 && ISIMM(o1);
    default: break;
    }
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_MOVSS: case ZYDIS_MNEMONIC_MOVSD:
        return nv == 2 && ((xr(o0) >= 0 && xm(o1)) || (ISMEM(o0) && xr(o1) >= 0));
    case ZYDIS_MNEMONIC_MOVD: case ZYDIS_MNEMONIC_MOVQ:
        if (nv != 2) return 0;
        if (xr(o0) >= 0) return xr(o1) >= 0 || gp(o1) || ISMEM(o1);
        return xr(o1) >= 0 && (gp(o0) || ISMEM(o0));
    case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVLPD: case ZYDIS_MNEMONIC_MOVHPS: case ZYDIS_MNEMONIC_MOVHPD:
        return nv == 2 && ((xr(o0) >= 0 && ISMEM(o1)) || (ISMEM(o0) && xr(o1) >= 0));
    case ZYDIS_MNEMONIC_MOVHLPS: case ZYDIS_MNEMONIC_MOVLHPS: return nv == 2 && xr(o0) >= 0 && xr(o1) >= 0;
    case ZYDIS_MNEMONIC_PSHUFD: case ZYDIS_MNEMONIC_PSHUFLW: case ZYDIS_MNEMONIC_PSHUFHW: case ZYDIS_MNEMONIC_SHUFPS: case ZYDIS_MNEMONIC_SHUFPD:
        return nv == 3 && xr(o0) >= 0 && xm(o1) && ISIMM(&j->op[2]);
    case ZYDIS_MNEMONIC_PEXTRW: return nv == 3 && gp(o0) && xr(o1) >= 0 && ISIMM(&j->op[2]);
    case ZYDIS_MNEMONIC_PINSRW: return nv == 3 && xr(o0) >= 0 && (gp(o1) || ISMEM(o1)) && ISIMM(&j->op[2]);
    case ZYDIS_MNEMONIC_PMOVMSKB: case ZYDIS_MNEMONIC_MOVMSKPS: case ZYDIS_MNEMONIC_MOVMSKPD: return nv == 2 && gp(o0) && xr(o1) >= 0;
    case ZYDIS_MNEMONIC_UCOMISS: case ZYDIS_MNEMONIC_UCOMISD: case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_COMISD:
        return nv == 2 && xr(o0) >= 0 && xm(o1);
    case ZYDIS_MNEMONIC_CVTSI2SS: case ZYDIS_MNEMONIC_CVTSI2SD: return nv == 2 && xr(o0) >= 0 && (gp(o1) || ISMEM(o1)) && o1->size >= 32;
    case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_CVTTSD2SI: case ZYDIS_MNEMONIC_CVTSS2SI: case ZYDIS_MNEMONIC_CVTSD2SI:
        return nv == 2 && gp(o0) && o0->size >= 32 && xm(o1);
    case ZYDIS_MNEMONIC_CVTSS2SD: case ZYDIS_MNEMONIC_CVTSD2SS: case ZYDIS_MNEMONIC_CVTDQ2PS: case ZYDIS_MNEMONIC_CVTPS2PD: case ZYDIS_MNEMONIC_CVTPD2PS:
        return nv == 2 && xr(o0) >= 0 && xm(o1);
    case ZYDIS_MNEMONIC_CMPPS: case ZYDIS_MNEMONIC_CMPPD: case ZYDIS_MNEMONIC_CMPSS: case ZYDIS_MNEMONIC_CMPSD:
        return nv == 3 && xr(o0) >= 0 && xm(o1) && ISIMM(&j->op[2]) && j->op[2].imm.value.u < 8;
    default: return 0;
    }
}

int jit_sse_has_slow_path(const JInsn *j) {
    if (j->in.encoding != ZYDIS_INSTRUCTION_ENCODING_LEGACY) return 0;
    const SseOp *op = find_op(j->in.mnemonic);
    if (!op) return 0;
    if (op->slow) return 1;
    switch (j->in.mnemonic) {
    case ZYDIS_MNEMONIC_CVTSS2SD: case ZYDIS_MNEMONIC_CVTSD2SS: case ZYDIS_MNEMONIC_CVTPS2PD: case ZYDIS_MNEMONIC_CVTPD2PS: return 1;
    default: return 0;
    }
}

// Any NaN lane in v (4 x f32 or 2 x f64): interpret the instruction instead (x86 NaN rules)
static void nan_check_vec(JComp *jc, JInsn *j, int v, int dbl) {
    if (dbl) a64_vfcmeq(A, 1, 7, v, v); else a64_vfcmeq(A, 0, 7, v, v);
    a64_put(A, 0x6EB1A800u | 7 << 5 | 7);   // uminv s7, v7.4s
    a64_fmov_to_gp(A, 0, T3, 7);
    jit_slow_path_cbz(jc, j, 0, 0, T3);
}
static void nan_check_scalar(JComp *jc, JInsn *j, int v, int dbl) {
    a64_fcmp(A, dbl, v, v);
    jit_slow_path_cond(jc, j, C_VS);
}

// xmm result in v0 -> dst register
static void put_dst(JComp *jc, JInsn *j, int v) { st_x(jc, v, xr(OPN(j, 0))); }

// convert float in v (s or d) to integer (x86 truncation / "integer indefinite" rules), result in T1
static void cvt_to_int(JComp *jc, int v, int dbl, int w64, int round) {
    if (round) a64_frintx(A, dbl, v, v);
    a64_fcvtzs(A, 1, dbl, T1, v);
    if (w64) {
        a64_mov_imm(A, T5, 0x7FFFFFFFFFFFFFFFull);
        a64_fcmp(A, dbl, v, v);
        a64_ccmp(A, 1, T1, T5, 4, C_VC);           // not NaN: compare with the saturated value; NaN: Z=1
        a64_addsub_imm(A, 1, 0, 0, T5, T5, 1, 0);   // INT64_MIN
        a64_csel(A, 1, T1, T5, T1, C_EQ);
    } else {
        a64_sxtw(A, T2, T1);
        a64_fcmp(A, dbl, v, v);
        a64_ccmp(A, 1, T1, T2, 0, C_VC);           // not NaN: fits in 32 bits? NaN: Z=0
        a64_movz(A, 0, T5, 0x8000, 1);
        a64_csel(A, 0, T1, T1, T5, C_EQ);
    }
    jc->nzcv = 0;
}

static void ld_weights(JComp *jc, int vt, int kind) {   // pmovmskb / movmskps / movmskpd bit weights
    static const u8 wb[16] __attribute__((aligned(16))) = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    static const u32 ws[4] __attribute__((aligned(16))) = {1, 2, 4, 8};
    static const u64 wd[2] __attribute__((aligned(16))) = {1, 2};
    a64_mov_imm(A, T2, kind == 0 ? (u64)wb : kind == 1 ? (u64)ws : (u64)wd);
    a64_fldst_uimm(A, 4, 1, vt, T2, 0);
}

int jit_sse_translate(JComp *jc, JInsn *j) {
    const SseOp *op = find_op(MN(j));
    if (!op) return 0;
    ZydisMnemonic m = MN(j);
    const ZydisDecodedOperand *o0 = OPN(j, 0), *o1 = OPN(j, 1);
    int first = jc->nexit;
    switch (op->kind) {
    case K_IBIN: {
        int a = xget(jc, xr(o0)), b = src(jc, j, 1, 2, 4), d = xdst(jc, xr(o0));
        int k = op->arg >> 2, sz = op->arg & 3;
        switch (k) {
        case IB_ADD: a64_vadd(A, sz, d, a, b); break;
        case IB_SUB: a64_vsub(A, sz, d, a, b); break;
        case IB_SQADD: a64_vsqadd(A, sz, d, a, b); break;
        case IB_UQADD: a64_vuqadd(A, sz, d, a, b); break;
        case IB_SQSUB: a64_vsqsub(A, sz, d, a, b); break;
        case IB_UQSUB: a64_vuqsub(A, sz, d, a, b); break;
        case IB_CMEQ: a64_vcmeq(A, sz, d, a, b); break;
        case IB_CMGT: a64_vcmgt(A, sz, d, a, b); break;
        case IB_MUL: a64_vmul(A, sz, d, a, b); break;
        case IB_UMAX: a64_vumax(A, sz, d, a, b); break;
        case IB_UMIN: a64_vumin(A, sz, d, a, b); break;
        case IB_SMAX: a64_vsmax(A, sz, d, a, b); break;
        case IB_SMIN: a64_vsmin(A, sz, d, a, b); break;
        case IB_URHADD: a64_vurhadd(A, sz, d, a, b); break;
        case IB_AND: a64_vand(A, d, a, b); break;
        case IB_ANDN: a64_vbic(A, d, b, a); break;   // ~dst & src
        case IB_OR: a64_vorr(A, d, a, b); break;
        default: a64_veor(A, d, a, b); break;
        }
        return 1; }
    case K_FBIN: {
        int dbl = op->arg & 1, f = op->arg >> 1;
        int a = xget(jc, xr(o0)), b = src(jc, j, 1, 2, 4);
        switch (f) {
        case FP2_ADD: a64_vfadd(A, dbl, 0, a, b); break;
        case FP2_SUB: a64_vfsub(A, dbl, 0, a, b); break;
        case FP2_MUL: a64_vfmul(A, dbl, 0, a, b); break;
        default: a64_vfdiv(A, dbl, 0, a, b); break;
        }
        nan_check_vec(jc, j, 0, dbl);
        a64_vmov(A, xdst(jc, xr(o0)), 0);
        jit_slow_resume(jc, first);
        return 1; }
    case K_FSCAL: {
        int dbl = op->arg & 1, f = op->arg >> 1;
        int a = xget(jc, xr(o0)), b = src(jc, j, 1, 2, dbl ? 3 : 2);
        a64_fp2(A, dbl, (u32)f, 0, a, b);
        nan_check_scalar(jc, j, 0, dbl);
        a64_vins_elem(A, dbl ? 3 : 2, xdst(jc, xr(o0)), 0, 0, 0);
        jit_slow_resume(jc, first);
        return 1; }
    case K_FMINMAX: {   // min: a < b ? a : b ; max: a > b ? a : b  (second operand on NaN / equal)
        int dbl = op->arg & 1, scal = op->arg & 2, mx = op->arg & 4;
        int a = xget(jc, xr(o0)), b = src(jc, j, 1, 2, scal ? (dbl ? 3 : 2) : 4);
        if (mx) a64_vfcmgt(A, dbl, 3, a, b); else a64_vfcmgt(A, dbl, 3, b, a);
        a64_vbsl(A, 3, a, b);
        if (scal) a64_vins_elem(A, dbl ? 3 : 2, xdst(jc, xr(o0)), 0, 3, 0);
        else a64_vmov(A, xdst(jc, xr(o0)), 3);
        return 1; }
    case K_SQRT: {
        int dbl = op->arg & 1, scal = op->arg & 2;
        if (scal) {
            xget(jc, xr(o0));
            int b = src(jc, j, 1, 2, dbl ? 3 : 2);
            a64_fsqrt(A, dbl, 0, b);
            nan_check_scalar(jc, j, 0, dbl);
            a64_vins_elem(A, dbl ? 3 : 2, xdst(jc, xr(o0)), 0, 0, 0);
        } else {
            int b = src(jc, j, 1, 2, 4);
            a64_vfsqrt(A, dbl, 0, b);
            nan_check_vec(jc, j, 0, dbl);
            a64_vmov(A, xdst(jc, xr(o0)), 0);
        }
        jit_slow_resume(jc, first);
        return 1; }
    case K_MOV:
        if (ISMEM(o0)) st_mem(jc, j, 0, xget(jc, xr(o1)), 4);
        else if (ISMEM(o1)) { int base, disp; jit_ea(jc, j, o1, &base, &disp); a64_fldst_any(A, 4, 1, xdst(jc, xr(o0)), base, disp, T17); }
        else if (xr(o0) != xr(o1)) { int b = xget(jc, xr(o1)); a64_vmov(A, xdst(jc, xr(o0)), b); }
        return 1;
    case K_PERM: {
        int a = xget(jc, xr(o0)), b = src(jc, j, 1, 2, 4);
        a64_vperm(A, op->arg >> 2, op->arg & 3, 0, a, b);
        a64_vmov(A, xdst(jc, xr(o0)), 0);
        return 1; }
    case K_PACK:
        ld_x(jc, 1, xr(o0)); ld_op(jc, j, 1, 2, 4);
        if (op->arg == 1) { a64_vsqxtun(A, 0, 0, 0, 1); a64_vsqxtun(A, 1, 0, 0, 2); }
        else { int sz = op->arg == 2 ? 1 : 0; a64_vsqxtn(A, 0, sz, 0, 1); a64_vsqxtn(A, 1, sz, 0, 2); }
        put_dst(jc, j, 0);
        return 1;
    case K_SHIFTI: {
        int k = op->arg >> 2, sz = op->arg & 3, es = 8 << sz;
        u64 n = o1->imm.value.u & 0xFF;
        ld_x(jc, 1, xr(o0));
        if (k == 3 || k == 4) {
            if (n >= 16) a64_vmovi0(A, 0);
            else if (n == 0) a64_vmov(A, 0, 1);
            else { a64_vmovi0(A, 2); if (k == 4) a64_vext(A, 0, 1, 2, (int)n); else a64_vext(A, 0, 2, 1, (int)(16 - n)); }
        } else if (n == 0) a64_vmov(A, 0, 1);
        else if (k == 2) a64_vsshr(A, sz, 0, 1, n >= (u64)es ? es : (int)n);
        else if (n >= (u64)es) a64_vmovi0(A, 0);
        else if (k == 0) a64_vshl_imm(A, sz, 0, 1, (int)n);
        else a64_vushr(A, sz, 0, 1, (int)n);
        put_dst(jc, j, 0);
        return 1; }
    default: break;
    }
    switch (m) {
    case ZYDIS_MNEMONIC_MOVSS: case ZYDIS_MNEMONIC_MOVSD: {
        int sz = m == ZYDIS_MNEMONIC_MOVSS ? 2 : 3;
        if (ISMEM(o0)) st_mem(jc, j, 0, xget(jc, xr(o1)), sz);
        else if (ISMEM(o1)) { int base, disp; jit_ea(jc, j, o1, &base, &disp); a64_fldst_any(A, sz, 1, xdst(jc, xr(o0)), base, disp, T17); }   // zeroes the rest
        else { xget(jc, xr(o0)); int b = xget(jc, xr(o1)); a64_vins_elem(A, sz, xdst(jc, xr(o0)), 0, b, 0); }
        return 1; }
    case ZYDIS_MNEMONIC_MOVD: case ZYDIS_MNEMONIC_MOVQ: {
        int w = o0->size == 128 ? o1->size : o0->size;   // 32 or 64 (xmm<->xmm movq: 128/128)
        if (xr(o0) >= 0) {
            if (xr(o1) >= 0) { ld_x(jc, 1, xr(o1)); a64_put(A, 0x0EA01C00u | 1 << 16 | 1 << 5 | 0); }   // mov v0.8b, v1.8b: zero upper
            else if (ISMEM(o1)) ld_op(jc, j, 1, 0, w == 64 ? 3 : 2);
            else { int r = jit_rd_int(jc, j, 1, T1); a64_fmov_from_gp(A, w == 64, 0, r); }
            put_dst(jc, j, 0);
        } else {
            ld_x(jc, 0, xr(o1));
            if (ISMEM(o0)) st_mem(jc, j, 0, 0, w == 64 ? 3 : 2);
            else { a64_fmov_to_gp(A, w == 64, T1, 0); jit_wr_int(jc, j, 0, T1); }
        }
        return 1; }
    case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVLPD: case ZYDIS_MNEMONIC_MOVHPS: case ZYDIS_MNEMONIC_MOVHPD: {
        int hi = m == ZYDIS_MNEMONIC_MOVHPS || m == ZYDIS_MNEMONIC_MOVHPD;
        if (ISMEM(o0)) {
            ld_x(jc, 0, xr(o1));
            if (hi) a64_vins_elem(A, 3, 0, 0, 0, 1);
            st_mem(jc, j, 0, 0, 3);
        } else {
            ld_x(jc, 1, xr(o0)); ld_op(jc, j, 1, 2, 3);
            a64_vins_elem(A, 3, 1, hi, 2, 0);
            put_dst(jc, j, 1);
        }
        return 1; }
    case ZYDIS_MNEMONIC_MOVHLPS: case ZYDIS_MNEMONIC_MOVLHPS:
        ld_x(jc, 1, xr(o0)); ld_x(jc, 2, xr(o1));
        if (m == ZYDIS_MNEMONIC_MOVHLPS) a64_vins_elem(A, 3, 1, 0, 2, 1); else a64_vins_elem(A, 3, 1, 1, 2, 0);
        put_dst(jc, j, 1);
        return 1;
    case ZYDIS_MNEMONIC_PSHUFD: case ZYDIS_MNEMONIC_SHUFPS: {
        int imm = (int)OPN(j, 2)->imm.value.u;
        int b = src(jc, j, 1, 2, 4), a = m == ZYDIS_MNEMONIC_SHUFPS ? xget(jc, xr(o0)) : b;
        for (int i = 0; i < 4; i++) a64_vins_elem(A, 2, 0, i, i < 2 ? a : b, (imm >> (2 * i)) & 3);
        a64_vmov(A, xdst(jc, xr(o0)), 0);
        return 1; }
    case ZYDIS_MNEMONIC_SHUFPD: {
        int imm = (int)OPN(j, 2)->imm.value.u;
        ld_x(jc, 1, xr(o0)); ld_op(jc, j, 1, 2, 4);
        a64_vins_elem(A, 3, 0, 0, 1, imm & 1);
        a64_vins_elem(A, 3, 0, 1, 2, (imm >> 1) & 1);
        put_dst(jc, j, 0);
        return 1; }
    case ZYDIS_MNEMONIC_PSHUFLW: case ZYDIS_MNEMONIC_PSHUFHW: {
        int imm = (int)OPN(j, 2)->imm.value.u, hi = m == ZYDIS_MNEMONIC_PSHUFHW;
        ld_op(jc, j, 1, 2, 4);
        a64_vmov(A, 0, 2);
        for (int i = 0; i < 4; i++) a64_vins_elem(A, 1, 0, i + 4 * hi, 2, ((imm >> (2 * i)) & 3) + 4 * hi);
        put_dst(jc, j, 0);
        return 1; }
    case ZYDIS_MNEMONIC_PEXTRW:
        ld_x(jc, 0, xr(o1));
        a64_vumov(A, 1, T1, 0, (int)(OPN(j, 2)->imm.value.u & 7));
        jit_wr_int(jc, j, 0, T1);
        return 1;
    case ZYDIS_MNEMONIC_PINSRW: {
        int r = jit_rd_int(jc, j, 1, T1);
        ld_x(jc, 0, xr(o0));
        a64_vins_gp(A, 1, 0, (int)(OPN(j, 2)->imm.value.u & 7), r);
        put_dst(jc, j, 0);
        return 1; }
    case ZYDIS_MNEMONIC_PMOVMSKB: case ZYDIS_MNEMONIC_MOVMSKPS: case ZYDIS_MNEMONIC_MOVMSKPD: {
        int kind = m == ZYDIS_MNEMONIC_PMOVMSKB ? 0 : m == ZYDIS_MNEMONIC_MOVMSKPS ? 1 : 2;
        ld_x(jc, 0, xr(o1));
        a64_vsshr(A, kind == 0 ? 0 : kind == 1 ? 2 : 3, 0, 0, kind == 0 ? 8 : kind == 1 ? 32 : 64);   // lanes -> all sign bits
        ld_weights(jc, 1, kind);
        a64_vand(A, 0, 0, 1);
        if (kind == 0) {
            a64_vext(A, 1, 0, 0, 8);
            a64_vaddv_b(A, 0, 0, 0);    // low 8 lanes
            a64_vaddv_b(A, 0, 1, 1);    // high 8 lanes
            a64_fmov_to_gp(A, 0, T1, 0); a64_fmov_to_gp(A, 0, T2, 1);
            a64_log_reg(A, 0, 1, 0, T1, T1, T2, SH_LSL, 8);
        } else if (kind == 1) {
            a64_put(A, 0x4EB1B800u | 0 << 5 | 0);   // addv s0, v0.4s
            a64_fmov_to_gp(A, 0, T1, 0);
        } else {
            a64_vumov(A, 3, T1, 0, 0); a64_vumov(A, 3, T2, 0, 1);
            a64_orr(A, 0, T1, T1, T2);
        }
        jit_wr_int(jc, j, 0, T1);   // 32-bit destination: zero-extended
        return 1; }
    case ZYDIS_MNEMONIC_UCOMISS: case ZYDIS_MNEMONIC_UCOMISD: case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_COMISD: {
        int dbl = op->arg;
        int a = xget(jc, xr(o0)), b = src(jc, j, 1, 2, dbl ? 3 : 2);
        a64_fcmp(A, dbl, a, b);
        if (j->live_after & F6) {
            // CF = less or unordered, ZF = equal or unordered, PF = unordered; OF SF AF = 0
            a64_cset(A, 1, T1, C_LT);
            a64_cset(A, 1, T2, C_VS);
            a64_cset(A, 1, T3, C_EQ);
            a64_orr(A, 1, T3, T3, T2);
            a64_log_reg(A, 1, 1, 0, RFRES, T1, T2, SH_LSL, 2);
            a64_log_reg(A, 1, 1, 0, RFRES, RFRES, T3, SH_LSL, 6);
            a64_movz(A, 1, RFOP, 0, 0);
            jc->fk = JF_MAT;
            jc->nzcv = 1;   // NZCV still holds the fcmp result: a following jcc can use it directly
        } else jc->nzcv = 0;
        return 1; }
    case ZYDIS_MNEMONIC_CVTSI2SS: case ZYDIS_MNEMONIC_CVTSI2SD: {
        int dbl = m == ZYDIS_MNEMONIC_CVTSI2SD;
        int r = jit_rd_int(jc, j, 1, T1);
        xget(jc, xr(o0));
        a64_scvtf(A, o1->size == 64, dbl, 0, r);
        a64_vins_elem(A, dbl ? 3 : 2, xdst(jc, xr(o0)), 0, 0, 0);
        return 1; }
    case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_CVTTSD2SI: case ZYDIS_MNEMONIC_CVTSS2SI: case ZYDIS_MNEMONIC_CVTSD2SI: {
        int dbl = m == ZYDIS_MNEMONIC_CVTTSD2SI || m == ZYDIS_MNEMONIC_CVTSD2SI;
        int round = m == ZYDIS_MNEMONIC_CVTSS2SI || m == ZYDIS_MNEMONIC_CVTSD2SI;
        int v = src(jc, j, 1, 0, dbl ? 3 : 2);
        if (round) { a64_frintx(A, dbl, 0, v); v = 0; }
        cvt_to_int(jc, v, dbl, o0->size == 64, 0);
        jit_wr_int(jc, j, 0, T1);
        return 1; }
    case ZYDIS_MNEMONIC_CVTSS2SD: case ZYDIS_MNEMONIC_CVTSD2SS: {
        int todbl = m == ZYDIS_MNEMONIC_CVTSS2SD;
        xget(jc, xr(o0));
        int b = src(jc, j, 1, 2, todbl ? 2 : 3);
        if (todbl) a64_fcvt_s2d(A, 0, b); else a64_fcvt_d2s(A, 0, b);
        nan_check_scalar(jc, j, 0, todbl);
        a64_vins_elem(A, todbl ? 3 : 2, xdst(jc, xr(o0)), 0, 0, 0);
        jit_slow_resume(jc, first);
        return 1; }
    case ZYDIS_MNEMONIC_CVTDQ2PS:
        ld_op(jc, j, 1, 2, 4);
        a64_vscvtf(A, 0, 0, 2);
        put_dst(jc, j, 0);
        return 1;
    case ZYDIS_MNEMONIC_CVTPS2PD:
        ld_op(jc, j, 1, 2, ISMEM(o1) ? 3 : 4);
        a64_vfcvtl(A, 0, 2);
        nan_check_vec(jc, j, 0, 1);
        put_dst(jc, j, 0);
        jit_slow_resume(jc, first);
        return 1;
    case ZYDIS_MNEMONIC_CVTPD2PS:
        ld_op(jc, j, 1, 2, 4);
        a64_vfcvtn(A, 0, 2);
        nan_check_vec(jc, j, 0, 0);
        put_dst(jc, j, 0);
        jit_slow_resume(jc, first);
        return 1;
    case ZYDIS_MNEMONIC_CMPPS: case ZYDIS_MNEMONIC_CMPPD: case ZYDIS_MNEMONIC_CMPSS: case ZYDIS_MNEMONIC_CMPSD: {
        int dbl = op->arg & 1, scal = op->arg & 2, pred = (int)(OPN(j, 2)->imm.value.u & 7);
        ld_x(jc, 1, xr(o0)); ld_op(jc, j, 1, 2, scal ? (dbl ? 3 : 2) : 4);
        switch (pred & 3) {
        case 0: a64_vfcmeq(A, dbl, 0, 1, 2); break;                          // eq / neq
        case 1: a64_vfcmgt(A, dbl, 0, 2, 1); break;                          // lt / nlt
        case 2: a64_vfcmge(A, dbl, 0, 2, 1); break;                          // le / nle
        default: a64_vfcmeq(A, dbl, 3, 1, 1); a64_vfcmeq(A, dbl, 4, 2, 2); a64_vand(A, 0, 3, 4); break;   // ord (unord = not ord)
        }
        if (!!(pred & 4) != ((pred & 3) == 3)) a64_vnot(A, 0, 0);
        if (scal) { a64_vins_elem(A, dbl ? 3 : 2, 1, 0, 0, 0); put_dst(jc, j, 1); }
        else put_dst(jc, j, 0);
        return 1; }
    default: return 0;
    }
}
