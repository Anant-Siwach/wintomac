// wintomac interpreter: x87 FPU. Values are host doubles (ARM has no 80-bit float), 80-bit memory format
// honoured, exactly as the static lifter models x87 (recomp/lifter.py), so mixed code agrees bit for bit.
#include "interp_impl.h"

static inline int sti(IState *s, int i) { return rt_ireg[IOP(s, i)->reg.value].idx; }
static inline int is_reg(IState *s, int i) { return IOP(s, i)->type == ZYDIS_OPERAND_TYPE_REGISTER; }

static double fload(IState *s, int i, int integer) {
    const ZydisDecodedOperand *o = IOP(s, i);
    if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) return ST_(s->c, rt_ireg[o->reg.value].idx);
    u64 a = iea(s, o);
    if (integer) {
        switch (o->size) { case 16: return (double)(i16)LD16(a); case 32: return (double)(i32)LD32(a); default: return (double)(i64)LD64(a); }
    }
    switch (o->size) { case 32: return (double)LDF32(a); case 64: return LDF64(a); default: return LDF80(a); }
}

static void fcc(Ctx *c, double a, double b) {
    u16 cc = (a != a || b != b) ? 0x4500 : a < b ? 0x0100 : a == b ? 0x4000 : 0;
    c->fsw = (c->fsw & ~0x4700) | cc;
}

int rt_interp_x87(IState *s) {
    Ctx *c = s->c;
    const ZydisDecodedInstruction *in = s->in;
    int nv = NVIS(s);
    ZydisMnemonic mn = in->mnemonic;
    switch (mn) {
    case ZYDIS_MNEMONIC_FLD: case ZYDIS_MNEMONIC_FILD: { double v = fload(s, 0, mn == ZYDIS_MNEMONIC_FILD); FPUSH(c, v); return X_NEXT; }
    case ZYDIS_MNEMONIC_FST: case ZYDIS_MNEMONIC_FSTP: case ZYDIS_MNEMONIC_FIST: case ZYDIS_MNEMONIC_FISTP: case ZYDIS_MNEMONIC_FISTTP: {
        double v = ST_(c, 0);
        const ZydisDecodedOperand *o = IOP(s, 0);
        if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) ST_(c, sti(s, 0)) = v;
        else {
            u64 a = iea(s, o);
            if (mn == ZYDIS_MNEMONIC_FST || mn == ZYDIS_MNEMONIC_FSTP) {
                if (o->size == 32) { float f = (float)v; u32 b; memcpy(&b, &f, 4); ST32(a, b); }
                else if (o->size == 64) { u64 b; memcpy(&b, &v, 8); ST64(a, b); }
                else STF80(a, v);
            } else {
                double r = mn == ZYDIS_MNEMONIC_FISTTP ? __builtin_trunc(v) : __builtin_rint(v);
                if (o->size == 16) ST16(a, (u16)(i16)r); else if (o->size == 32) ST32(a, (u32)(i32)r); else ST64(a, (u64)(i64)r);
            }
        }
        if (mn != ZYDIS_MNEMONIC_FST && mn != ZYDIS_MNEMONIC_FIST) (void)FPOP(c);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_FLDZ: FPUSH(c, 0.0); return X_NEXT;
    case ZYDIS_MNEMONIC_FLD1: FPUSH(c, 1.0); return X_NEXT;
    case ZYDIS_MNEMONIC_FLDPI: FPUSH(c, 3.14159265358979323846); return X_NEXT;
    case ZYDIS_MNEMONIC_FLDL2E: FPUSH(c, 1.44269504088896340736); return X_NEXT;
    case ZYDIS_MNEMONIC_FLDLN2: FPUSH(c, 0.69314718055994530942); return X_NEXT;
    case ZYDIS_MNEMONIC_FLDL2T: FPUSH(c, 3.32192809488736234787); return X_NEXT;
    case ZYDIS_MNEMONIC_FLDLG2: FPUSH(c, 0.30102999566398119521); return X_NEXT;
    case ZYDIS_MNEMONIC_FCHS: ST_(c, 0) = -ST_(c, 0); return X_NEXT;
    case ZYDIS_MNEMONIC_FABS: ST_(c, 0) = __builtin_fabs(ST_(c, 0)); return X_NEXT;
    case ZYDIS_MNEMONIC_FSQRT: ST_(c, 0) = __builtin_sqrt(ST_(c, 0)); return X_NEXT;
    case ZYDIS_MNEMONIC_FRNDINT: ST_(c, 0) = __builtin_rint(ST_(c, 0)); return X_NEXT;
    case ZYDIS_MNEMONIC_FXCH: { int i = nv ? sti(s, 0) : 1; double t = ST_(c, 0); ST_(c, 0) = ST_(c, i); ST_(c, i) = t; return X_NEXT; }
    case ZYDIS_MNEMONIC_FFREE: case ZYDIS_MNEMONIC_FNOP: case ZYDIS_MNEMONIC_FNCLEX: case ZYDIS_MNEMONIC_FNSTENV: case ZYDIS_MNEMONIC_FLDENV:
        return X_NEXT;
    case ZYDIS_MNEMONIC_FINCSTP: c->ftop = (c->ftop + 1) & 7; return X_NEXT;
    case ZYDIS_MNEMONIC_FDECSTP: c->ftop = (c->ftop - 1) & 7; return X_NEXT;

    case ZYDIS_MNEMONIC_FADD: case ZYDIS_MNEMONIC_FADDP: case ZYDIS_MNEMONIC_FIADD:
    case ZYDIS_MNEMONIC_FSUB: case ZYDIS_MNEMONIC_FSUBP: case ZYDIS_MNEMONIC_FISUB:
    case ZYDIS_MNEMONIC_FSUBR: case ZYDIS_MNEMONIC_FSUBRP: case ZYDIS_MNEMONIC_FISUBR:
    case ZYDIS_MNEMONIC_FMUL: case ZYDIS_MNEMONIC_FMULP: case ZYDIS_MNEMONIC_FIMUL:
    case ZYDIS_MNEMONIC_FDIV: case ZYDIS_MNEMONIC_FDIVP: case ZYDIS_MNEMONIC_FIDIV:
    case ZYDIS_MNEMONIC_FDIVR: case ZYDIS_MNEMONIC_FDIVRP: case ZYDIS_MNEMONIC_FIDIVR: {
        int integer = mn == ZYDIS_MNEMONIC_FIADD || mn == ZYDIS_MNEMONIC_FISUB || mn == ZYDIS_MNEMONIC_FISUBR || mn == ZYDIS_MNEMONIC_FIMUL ||
                      mn == ZYDIS_MNEMONIC_FIDIV || mn == ZYDIS_MNEMONIC_FIDIVR;
        int pop = mn == ZYDIS_MNEMONIC_FADDP || mn == ZYDIS_MNEMONIC_FSUBP || mn == ZYDIS_MNEMONIC_FSUBRP || mn == ZYDIS_MNEMONIC_FMULP ||
                  mn == ZYDIS_MNEMONIC_FDIVP || mn == ZYDIS_MNEMONIC_FDIVRP;
        int rev = mn == ZYDIS_MNEMONIC_FSUBR || mn == ZYDIS_MNEMONIC_FSUBRP || mn == ZYDIS_MNEMONIC_FISUBR ||
                  mn == ZYDIS_MNEMONIC_FDIVR || mn == ZYDIS_MNEMONIC_FDIVRP || mn == ZYDIS_MNEMONIC_FIDIVR;
        int d; double src;
        if (nv == 0) { d = 1; src = ST_(c, 0); }
        else if (nv == 1) { d = 0; src = fload(s, 0, integer); }
        else { d = sti(s, 0); src = ST_(c, sti(s, 1)); }
        double dst = ST_(c, d), a = rev ? src : dst, b = rev ? dst : src, r;
        switch (mn) {
        case ZYDIS_MNEMONIC_FADD: case ZYDIS_MNEMONIC_FADDP: case ZYDIS_MNEMONIC_FIADD: r = a + b; break;
        case ZYDIS_MNEMONIC_FMUL: case ZYDIS_MNEMONIC_FMULP: case ZYDIS_MNEMONIC_FIMUL: r = a * b; break;
        case ZYDIS_MNEMONIC_FDIV: case ZYDIS_MNEMONIC_FDIVP: case ZYDIS_MNEMONIC_FIDIV:
        case ZYDIS_MNEMONIC_FDIVR: case ZYDIS_MNEMONIC_FDIVRP: case ZYDIS_MNEMONIC_FIDIVR: r = a / b; break;
        default: r = a - b; break;
        }
        ST_(c, d) = r;
        if (pop) (void)FPOP(c);
        return X_NEXT; }

    case ZYDIS_MNEMONIC_FCOM: case ZYDIS_MNEMONIC_FCOMP: case ZYDIS_MNEMONIC_FCOMPP: case ZYDIS_MNEMONIC_FUCOM:
    case ZYDIS_MNEMONIC_FUCOMP: case ZYDIS_MNEMONIC_FUCOMPP: case ZYDIS_MNEMONIC_FICOM: case ZYDIS_MNEMONIC_FICOMP: {
        double b = nv == 0 ? ST_(c, 1) : fload(s, nv - 1, mn == ZYDIS_MNEMONIC_FICOM || mn == ZYDIS_MNEMONIC_FICOMP);
        fcc(c, ST_(c, 0), b);
        int pops = (mn == ZYDIS_MNEMONIC_FCOMPP || mn == ZYDIS_MNEMONIC_FUCOMPP) ? 2 :
                   (mn == ZYDIS_MNEMONIC_FCOMP || mn == ZYDIS_MNEMONIC_FUCOMP || mn == ZYDIS_MNEMONIC_FICOMP) ? 1 : 0;
        while (pops--) (void)FPOP(c);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_FCOMI: case ZYDIS_MNEMONIC_FCOMIP: case ZYDIS_MNEMONIC_FUCOMI: case ZYDIS_MNEMONIC_FUCOMIP: {
        double a = ST_(c, 0), b = ST_(c, sti(s, nv - 1));
        if (a != a || b != b) { s->zf = s->pf = s->cf = 1; }
        else { s->zf = a == b; s->pf = 0; s->cf = a < b; }
        s->of = s->sf = s->af = 0;
        if (mn == ZYDIS_MNEMONIC_FCOMIP || mn == ZYDIS_MNEMONIC_FUCOMIP) (void)FPOP(c);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_FTST: fcc(c, ST_(c, 0), 0.0); return X_NEXT;
    case ZYDIS_MNEMONIC_FXAM: {
        double a = ST_(c, 0);
        u16 cc = a != a ? 0x0100 : __builtin_isinf(a) ? 0x0500 : a == 0 ? 0x4000 :
                 __builtin_fpclassify(0, 0, 0, 0x4400, 0x4000, a) ? 0x4400 : 0x0400;
        if (__builtin_signbit(a)) cc |= 0x0200;
        c->fsw = (c->fsw & ~0x4700) | cc;
        return X_NEXT; }
    case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVE: case ZYDIS_MNEMONIC_FCMOVBE: case ZYDIS_MNEMONIC_FCMOVU:
    case ZYDIS_MNEMONIC_FCMOVNB: case ZYDIS_MNEMONIC_FCMOVNE: case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNU: {
        int t;
        switch (mn) {
        case ZYDIS_MNEMONIC_FCMOVB: t = s->cf; break;
        case ZYDIS_MNEMONIC_FCMOVE: t = s->zf; break;
        case ZYDIS_MNEMONIC_FCMOVBE: t = s->cf | s->zf; break;
        case ZYDIS_MNEMONIC_FCMOVU: t = s->pf; break;
        case ZYDIS_MNEMONIC_FCMOVNB: t = !s->cf; break;
        case ZYDIS_MNEMONIC_FCMOVNE: t = !s->zf; break;
        case ZYDIS_MNEMONIC_FCMOVNBE: t = !(s->cf | s->zf); break;
        default: t = !s->pf; break;
        }
        if (t) ST_(c, 0) = ST_(c, sti(s, nv - 1));
        return X_NEXT; }

    case ZYDIS_MNEMONIC_FNSTSW: {
        u16 v = (u16)(c->fsw & ~0x3800) | (u16)(c->ftop << 11);
        const ZydisDecodedOperand *o = IOP(s, 0);
        if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) greg_wr(s, o->reg.value, v); else ST16(iea(s, o), v);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_FNINIT: c->ftop = 0; c->fcw = 0x27f; c->fsw = 0; return X_NEXT;
    case ZYDIS_MNEMONIC_FNSTCW: ST16(iea(s, IOP(s, 0)), c->fcw); return X_NEXT;
    case ZYDIS_MNEMONIC_FLDCW: c->fcw = LD16(iea(s, IOP(s, 0))); return X_NEXT;

    case ZYDIS_MNEMONIC_FPREM: ST_(c, 0) = __builtin_fmod(ST_(c, 0), ST_(c, 1)); c->fsw &= ~0x4700; return X_NEXT;
    case ZYDIS_MNEMONIC_FPREM1: ST_(c, 0) = __builtin_remainder(ST_(c, 0), ST_(c, 1)); c->fsw &= ~0x4700; return X_NEXT;
    case ZYDIS_MNEMONIC_FSIN: ST_(c, 0) = __builtin_sin(ST_(c, 0)); c->fsw &= ~0x0400; return X_NEXT;
    case ZYDIS_MNEMONIC_FCOS: ST_(c, 0) = __builtin_cos(ST_(c, 0)); c->fsw &= ~0x0400; return X_NEXT;
    case ZYDIS_MNEMONIC_FSINCOS: { double x = ST_(c, 0); ST_(c, 0) = __builtin_sin(x); FPUSH(c, __builtin_cos(x)); c->fsw &= ~0x0400; return X_NEXT; }
    case ZYDIS_MNEMONIC_FPTAN: ST_(c, 0) = __builtin_tan(ST_(c, 0)); FPUSH(c, 1.0); c->fsw &= ~0x0400; return X_NEXT;
    case ZYDIS_MNEMONIC_FPATAN: ST_(c, 1) = __builtin_atan2(ST_(c, 1), ST_(c, 0)); (void)FPOP(c); return X_NEXT;
    case ZYDIS_MNEMONIC_F2XM1: ST_(c, 0) = __builtin_exp2(ST_(c, 0)) - 1.0; return X_NEXT;
    case ZYDIS_MNEMONIC_FYL2X: ST_(c, 1) = ST_(c, 1) * __builtin_log2(ST_(c, 0)); (void)FPOP(c); return X_NEXT;
    case ZYDIS_MNEMONIC_FYL2XP1: ST_(c, 1) = ST_(c, 1) * __builtin_log2(ST_(c, 0) + 1.0); (void)FPOP(c); return X_NEXT;
    case ZYDIS_MNEMONIC_FSCALE: ST_(c, 0) = __builtin_ldexp(ST_(c, 0), (int)__builtin_trunc(ST_(c, 1))); return X_NEXT;
    case ZYDIS_MNEMONIC_FXTRACT: { double x = ST_(c, 0); int e; double m = __builtin_frexp(x, &e); ST_(c, 0) = (double)(e - 1); FPUSH(c, m * 2.0); return X_NEXT; }
    default: return X_UNIMPL;
    }
    (void)is_reg;
}
