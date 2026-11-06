// wintomac interpreter: SSE, SSE2, SSE3, SSSE3, SSE4.1/4.2 and the common AVX/AVX2/FMA3 forms.
//
// Same register model and semantics as the static lifter (recomp/vector.py): legacy SSE leaves bits 128-255
// untouched, VEX.128 zeroes them, VEX.256 writes both halves. Most ops are 128-bit *lane kernels*
// k(d, a, b, c, imm, L) where d starts as a copy of a (gives legacy "dst is src1" and VEX "upper elements
// from src1" semantics for scalar ops); the engine runs the kernel once per 128-bit lane.
#include "interp_impl.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// ---- mnemonic normalisation: vaddps -> addps, FMA name parsing -----------------------------------------------
static u16 base_mn[ZYDIS_MNEMONIC_MAX_VALUE + 1];
enum { FMA_MADD = 1, FMA_MSUB, FMA_NMADD, FMA_NMSUB, FMA_MADDSUB, FMA_MSUBADD };
typedef struct { u8 kind, scalar, dbl; u16 form; } FmaInfo;
static FmaInfo fma_info[ZYDIS_MNEMONIC_MAX_VALUE + 1];
static pthread_once_t mn_once = PTHREAD_ONCE_INIT;

typedef struct { const char *s; int m; } MnName;
static int mn_cmp(const void *a, const void *b) { return strcmp(((const MnName *)a)->s, ((const MnName *)b)->s); }
static void init_mn(void) {
    static MnName names[ZYDIS_MNEMONIC_MAX_VALUE + 1];
    int n = 0;
    for (int m = 0; m <= ZYDIS_MNEMONIC_MAX_VALUE; m++) {
        base_mn[m] = (u16)m;
        const char *s = ZydisMnemonicGetString((ZydisMnemonic)m);
        if (s) names[n++] = (MnName){s, m};
    }
    qsort(names, n, sizeof names[0], mn_cmp);
    for (int i = 0; i < n; i++) {
        const char *s = names[i].s;
        if (s[0] != 'v') continue;
        MnName key = {s + 1, 0};
        MnName *hit = bsearch(&key, names, n, sizeof names[0], mn_cmp);
        if (hit) base_mn[names[i].m] = (u16)hit->m;
        static const struct { const char *p; int k; } kinds[] = {
            {"vfmaddsub", FMA_MADDSUB}, {"vfmsubadd", FMA_MSUBADD}, {"vfnmadd", FMA_NMADD}, {"vfnmsub", FMA_NMSUB},
            {"vfmadd", FMA_MADD}, {"vfmsub", FMA_MSUB}};
        for (int k = 0; k < 6; k++) {
            size_t pl = strlen(kinds[k].p);
            if (strncmp(s, kinds[k].p, pl)) continue;
            const char *t = s + pl;
            if (strlen(t) != 5) break;
            int form = atoi((char[4]){t[0], t[1], t[2], 0});
            if (form != 132 && form != 213 && form != 231) break;
            if (t[3] != 'p' && t[3] != 's') break;
            fma_info[names[i].m] = (FmaInfo){(u8)kinds[k].k, t[3] == 's', t[4] == 'd', (u16)form};
            break;
        }
    }
}

// ---- operand plumbing --------------------------------------------------------------------------------------
static inline int vreg(IState *s, int i, int *bits) {   // xmm/ymm register index, or -1
    const ZydisDecodedOperand *o = IOP(s, i);
    if (o->type != ZYDIS_OPERAND_TYPE_REGISTER) return -1;
    IReg r = rt_ireg[o->reg.value];
    if (r.kind != RK_XMM && r.kind != RK_YMM) return -1;
    if (bits) *bits = r.bits;
    return r.idx;
}
static inline int is_mem(IState *s, int i) { return IOP(s, i)->type == ZYDIS_OPERAND_TYPE_MEMORY; }
static inline int is_imm(IState *s, int i) { return IOP(s, i)->type == ZYDIS_OPERAND_TYPE_IMMEDIATE; }

static V128 lane_src(IState *s, int i, int L) {
    const ZydisDecodedOperand *o = IOP(s, i);
    if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) {
        IReg r = rt_ireg[o->reg.value];
        if (r.kind == RK_XMM || r.kind == RK_YMM) return L ? s->c->xh[r.idx] : s->c->x[r.idx];
        V128 v = {0}; v.q[0] = greg_rd(s, o->reg.value); return v;
    }
    if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) {
        u64 a = iea(s, o); int n = o->size / 8;
        if (n >= 16) return LDX(a + 16ull * L);
        if (L) return (V128){0};
        switch (n) { case 1: return LDV8(a); case 2: return LDV16(a); case 4: return LDV32(a); default: return LDV64(a); }
    }
    return (V128){0};
}

static int vwidth(IState *s) {
    for (int i = 0; i < s->in->operand_count; i++) {
        int bits;
        if (vreg(s, i, &bits) >= 0 && bits == 256) return 256;
        if (is_mem(s, i) && IOP(s, i)->size == 256) return 256;
    }
    return 128;
}

static inline void set_x(IState *s, int r, V128 lo) { s->c->x[r] = lo; if (IS_VEX(s)) s->c->xh[r] = (V128){0}; }

static V256 full_src(IState *s, int i, int nbytes) {
    V256 t; memset(&t, 0, sizeof t);
    const ZydisDecodedOperand *o = IOP(s, i);
    if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) {
        int bits, r = vreg(s, i, &bits);
        if (r >= 0) { t.h[0] = s->c->x[r]; if (bits == 256) t.h[1] = s->c->xh[r]; }
        else t.q[0] = greg_rd(s, o->reg.value);
    } else if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) {
        memcpy(&t, (void *)iea(s, o), nbytes ? nbytes : o->size / 8);
    }
    return t;
}
static void full_dst(IState *s, int i, V256 t) {
    const ZydisDecodedOperand *o = IOP(s, i);
    if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) { memcpy((void *)iea(s, o), &t, o->size / 8); return; }
    int bits, r = vreg(s, i, &bits);
    s->c->x[r] = t.h[0];
    if (bits == 256) s->c->xh[r] = t.h[1]; else if (IS_VEX(s)) s->c->xh[r] = (V128){0};
}

// ---- lane kernels --------------------------------------------------------------------------------------------
// arity: 1 unary (src = last non-imm operand), 2 binary, 3 ternary (blendv / FMA). 0 = not a lane op.
static int lane_arity(int m) {
    switch (m) {
    case ZYDIS_MNEMONIC_SQRTPS: case ZYDIS_MNEMONIC_SQRTPD: case ZYDIS_MNEMONIC_RCPPS: case ZYDIS_MNEMONIC_RSQRTPS:
    case ZYDIS_MNEMONIC_MOVSHDUP: case ZYDIS_MNEMONIC_MOVSLDUP: case ZYDIS_MNEMONIC_MOVDDUP:
    case ZYDIS_MNEMONIC_PABSB: case ZYDIS_MNEMONIC_PABSW: case ZYDIS_MNEMONIC_PABSD:
    case ZYDIS_MNEMONIC_PSHUFD: case ZYDIS_MNEMONIC_PSHUFLW: case ZYDIS_MNEMONIC_PSHUFHW:
    case ZYDIS_MNEMONIC_ROUNDPS: case ZYDIS_MNEMONIC_ROUNDPD: case ZYDIS_MNEMONIC_CVTDQ2PS: case ZYDIS_MNEMONIC_CVTPS2DQ:
    case ZYDIS_MNEMONIC_CVTTPS2DQ: case ZYDIS_MNEMONIC_PHMINPOSUW:
        return 1;
    case ZYDIS_MNEMONIC_BLENDVPS: case ZYDIS_MNEMONIC_BLENDVPD: case ZYDIS_MNEMONIC_PBLENDVB:
        return 3;
    case ZYDIS_MNEMONIC_ADDPS: case ZYDIS_MNEMONIC_ADDPD: case ZYDIS_MNEMONIC_ADDSS: case ZYDIS_MNEMONIC_ADDSD:
    case ZYDIS_MNEMONIC_SUBPS: case ZYDIS_MNEMONIC_SUBPD: case ZYDIS_MNEMONIC_SUBSS: case ZYDIS_MNEMONIC_SUBSD:
    case ZYDIS_MNEMONIC_MULPS: case ZYDIS_MNEMONIC_MULPD: case ZYDIS_MNEMONIC_MULSS: case ZYDIS_MNEMONIC_MULSD:
    case ZYDIS_MNEMONIC_DIVPS: case ZYDIS_MNEMONIC_DIVPD: case ZYDIS_MNEMONIC_DIVSS: case ZYDIS_MNEMONIC_DIVSD:
    case ZYDIS_MNEMONIC_MINPS: case ZYDIS_MNEMONIC_MINPD: case ZYDIS_MNEMONIC_MINSS: case ZYDIS_MNEMONIC_MINSD:
    case ZYDIS_MNEMONIC_MAXPS: case ZYDIS_MNEMONIC_MAXPD: case ZYDIS_MNEMONIC_MAXSS: case ZYDIS_MNEMONIC_MAXSD:
    case ZYDIS_MNEMONIC_SQRTSS: case ZYDIS_MNEMONIC_SQRTSD: case ZYDIS_MNEMONIC_RCPSS: case ZYDIS_MNEMONIC_RSQRTSS:
    case ZYDIS_MNEMONIC_ADDSUBPS: case ZYDIS_MNEMONIC_ADDSUBPD: case ZYDIS_MNEMONIC_HADDPS: case ZYDIS_MNEMONIC_HSUBPS:
    case ZYDIS_MNEMONIC_HADDPD: case ZYDIS_MNEMONIC_HSUBPD: case ZYDIS_MNEMONIC_PHADDW: case ZYDIS_MNEMONIC_PHSUBW:
    case ZYDIS_MNEMONIC_PHADDD: case ZYDIS_MNEMONIC_PHSUBD: case ZYDIS_MNEMONIC_PHADDSW: case ZYDIS_MNEMONIC_PHSUBSW:
    case ZYDIS_MNEMONIC_ANDPS: case ZYDIS_MNEMONIC_ANDPD: case ZYDIS_MNEMONIC_PAND: case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ORPD:
    case ZYDIS_MNEMONIC_POR: case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_XORPD: case ZYDIS_MNEMONIC_PXOR:
    case ZYDIS_MNEMONIC_ANDNPS: case ZYDIS_MNEMONIC_ANDNPD: case ZYDIS_MNEMONIC_PANDN:
    case ZYDIS_MNEMONIC_UNPCKLPS: case ZYDIS_MNEMONIC_UNPCKHPS: case ZYDIS_MNEMONIC_UNPCKLPD: case ZYDIS_MNEMONIC_UNPCKHPD:
    case ZYDIS_MNEMONIC_PUNPCKLBW: case ZYDIS_MNEMONIC_PUNPCKHBW: case ZYDIS_MNEMONIC_PUNPCKLWD: case ZYDIS_MNEMONIC_PUNPCKHWD:
    case ZYDIS_MNEMONIC_PUNPCKLDQ: case ZYDIS_MNEMONIC_PUNPCKHDQ: case ZYDIS_MNEMONIC_PUNPCKLQDQ: case ZYDIS_MNEMONIC_PUNPCKHQDQ:
    case ZYDIS_MNEMONIC_MOVHLPS: case ZYDIS_MNEMONIC_MOVLHPS:
    case ZYDIS_MNEMONIC_PADDB: case ZYDIS_MNEMONIC_PADDW: case ZYDIS_MNEMONIC_PADDD: case ZYDIS_MNEMONIC_PADDQ:
    case ZYDIS_MNEMONIC_PSUBB: case ZYDIS_MNEMONIC_PSUBW: case ZYDIS_MNEMONIC_PSUBD: case ZYDIS_MNEMONIC_PSUBQ:
    case ZYDIS_MNEMONIC_PCMPEQB: case ZYDIS_MNEMONIC_PCMPEQW: case ZYDIS_MNEMONIC_PCMPEQD: case ZYDIS_MNEMONIC_PCMPEQQ:
    case ZYDIS_MNEMONIC_PCMPGTB: case ZYDIS_MNEMONIC_PCMPGTW: case ZYDIS_MNEMONIC_PCMPGTD: case ZYDIS_MNEMONIC_PCMPGTQ:
    case ZYDIS_MNEMONIC_PMINSB: case ZYDIS_MNEMONIC_PMINSW: case ZYDIS_MNEMONIC_PMINSD: case ZYDIS_MNEMONIC_PMAXSB:
    case ZYDIS_MNEMONIC_PMAXSW: case ZYDIS_MNEMONIC_PMAXSD: case ZYDIS_MNEMONIC_PMINUB: case ZYDIS_MNEMONIC_PMINUW:
    case ZYDIS_MNEMONIC_PMINUD: case ZYDIS_MNEMONIC_PMAXUB: case ZYDIS_MNEMONIC_PMAXUW: case ZYDIS_MNEMONIC_PMAXUD:
    case ZYDIS_MNEMONIC_PSIGNB: case ZYDIS_MNEMONIC_PSIGNW: case ZYDIS_MNEMONIC_PSIGND:
    case ZYDIS_MNEMONIC_PADDSB: case ZYDIS_MNEMONIC_PADDSW: case ZYDIS_MNEMONIC_PSUBSB: case ZYDIS_MNEMONIC_PSUBSW:
    case ZYDIS_MNEMONIC_PADDUSB: case ZYDIS_MNEMONIC_PADDUSW: case ZYDIS_MNEMONIC_PSUBUSB: case ZYDIS_MNEMONIC_PSUBUSW:
    case ZYDIS_MNEMONIC_PMULLW: case ZYDIS_MNEMONIC_PMULLD: case ZYDIS_MNEMONIC_PMULHW: case ZYDIS_MNEMONIC_PMULHUW:
    case ZYDIS_MNEMONIC_PMULHRSW: case ZYDIS_MNEMONIC_PMULUDQ: case ZYDIS_MNEMONIC_PMULDQ: case ZYDIS_MNEMONIC_PMADDWD:
    case ZYDIS_MNEMONIC_PMADDUBSW: case ZYDIS_MNEMONIC_PAVGB: case ZYDIS_MNEMONIC_PAVGW: case ZYDIS_MNEMONIC_PSADBW:
    case ZYDIS_MNEMONIC_PSHUFB: case ZYDIS_MNEMONIC_PACKSSWB: case ZYDIS_MNEMONIC_PACKUSWB: case ZYDIS_MNEMONIC_PACKSSDW:
    case ZYDIS_MNEMONIC_PACKUSDW: case ZYDIS_MNEMONIC_VPSLLVD: case ZYDIS_MNEMONIC_VPSLLVQ: case ZYDIS_MNEMONIC_VPSRLVD:
    case ZYDIS_MNEMONIC_VPSRLVQ: case ZYDIS_MNEMONIC_VPSRAVD:
    case ZYDIS_MNEMONIC_SHUFPS: case ZYDIS_MNEMONIC_SHUFPD: case ZYDIS_MNEMONIC_BLENDPS: case ZYDIS_MNEMONIC_BLENDPD:
    case ZYDIS_MNEMONIC_PBLENDW: case ZYDIS_MNEMONIC_VPBLENDD: case ZYDIS_MNEMONIC_PALIGNR:
    case ZYDIS_MNEMONIC_ROUNDSS: case ZYDIS_MNEMONIC_ROUNDSD: case ZYDIS_MNEMONIC_DPPS: case ZYDIS_MNEMONIC_DPPD:
    case ZYDIS_MNEMONIC_CMPPS: case ZYDIS_MNEMONIC_CMPPD: case ZYDIS_MNEMONIC_CMPSS: case ZYDIS_MNEMONIC_CMPSD:
    case ZYDIS_MNEMONIC_CVTSS2SD: case ZYDIS_MNEMONIC_CVTSD2SS:
        return 2;
    default: return 0;
    }
}

static int cmp_pred(int p, double a, double b) {
    int un = a != a || b != b;
    switch (p & 15) {
    case 0: return a == b;           case 1: return a < b;          case 2: return a <= b;        case 3: return un;
    case 4: return !(a == b);        case 5: return !(a < b);       case 6: return !(a <= b);     case 7: return !un;
    case 8: return a == b || un;     case 9: return !(a >= b);      case 10: return !(a > b);     case 11: return 0;
    case 12: return a < b || a > b;  case 13: return a >= b;        case 14: return a > b;        default: return 1;
    }
}

// x86 NaN rules where they differ from ARM's: an invalid operation on non-NaN inputs (0/0, inf-inf, sqrt(-1))
// yields the negative "real indefinite" QNaN (ARM's default NaN is positive).
// NaN operands: x86 returns the first NaN source (quieted) even if a later one is signaling; ARM prefers
// signaling NaNs. So pick it ourselves.
static inline float xf(float r, float a, float b) {
    if (__builtin_expect(r != r, 0)) {
        u32 u;
        if (a != a) { memcpy(&u, &a, 4); u |= 1u << 22; }
        else if (b != b) { memcpy(&u, &b, 4); u |= 1u << 22; }
        else u = 0xFFC00000u;
        memcpy(&r, &u, 4);
    }
    return r;
}
static inline double xd(double r, double a, double b) {
    if (__builtin_expect(r != r, 0)) {
        u64 u;
        if (a != a) { memcpy(&u, &a, 8); u |= 1ull << 51; }
        else if (b != b) { memcpy(&u, &b, 8); u |= 1ull << 51; }
        else u = 0xFFF8000000000000ull;
        memcpy(&r, &u, 8);
    }
    return r;
}
#define FOP(x, op, y) xf((x) op (y), (x), (y))
#define DOP(x, op, y) xd((x) op (y), (x), (y))
#define EW(field, n, expr) do { for (int i = 0; i < (n); i++) d->field[i] = (expr); } while (0)
#define SAT8(v) SAT_I8(v)
static void lane_kernel(IState *s, int m, V128 *d, V128 a, V128 b, V128 cc, int imm, int L) {
    switch (m) {
    case ZYDIS_MNEMONIC_ADDPS: EW(f, 4, FOP(a.f[i], +, b.f[i])); break;
    case ZYDIS_MNEMONIC_ADDPD: EW(fd, 2, DOP(a.fd[i], +, b.fd[i])); break;
    case ZYDIS_MNEMONIC_ADDSS: d->f[0] = FOP(a.f[0], +, b.f[0]); break;
    case ZYDIS_MNEMONIC_ADDSD: d->fd[0] = DOP(a.fd[0], +, b.fd[0]); break;
    case ZYDIS_MNEMONIC_SUBPS: EW(f, 4, FOP(a.f[i], -, b.f[i])); break;
    case ZYDIS_MNEMONIC_SUBPD: EW(fd, 2, DOP(a.fd[i], -, b.fd[i])); break;
    case ZYDIS_MNEMONIC_SUBSS: d->f[0] = FOP(a.f[0], -, b.f[0]); break;
    case ZYDIS_MNEMONIC_SUBSD: d->fd[0] = DOP(a.fd[0], -, b.fd[0]); break;
    case ZYDIS_MNEMONIC_MULPS: EW(f, 4, FOP(a.f[i], *, b.f[i])); break;
    case ZYDIS_MNEMONIC_MULPD: EW(fd, 2, DOP(a.fd[i], *, b.fd[i])); break;
    case ZYDIS_MNEMONIC_MULSS: d->f[0] = FOP(a.f[0], *, b.f[0]); break;
    case ZYDIS_MNEMONIC_MULSD: d->fd[0] = DOP(a.fd[0], *, b.fd[0]); break;
    case ZYDIS_MNEMONIC_DIVPS: EW(f, 4, FOP(a.f[i], /, b.f[i])); break;
    case ZYDIS_MNEMONIC_DIVPD: EW(fd, 2, DOP(a.fd[i], /, b.fd[i])); break;
    case ZYDIS_MNEMONIC_DIVSS: d->f[0] = FOP(a.f[0], /, b.f[0]); break;
    case ZYDIS_MNEMONIC_DIVSD: d->fd[0] = DOP(a.fd[0], /, b.fd[0]); break;
    // x86 min/max: the second operand if either is NaN or both are zero
    case ZYDIS_MNEMONIC_MINPS: EW(f, 4, a.f[i] < b.f[i] ? a.f[i] : b.f[i]); break;
    case ZYDIS_MNEMONIC_MAXPS: EW(f, 4, a.f[i] > b.f[i] ? a.f[i] : b.f[i]); break;
    case ZYDIS_MNEMONIC_MINPD: EW(fd, 2, a.fd[i] < b.fd[i] ? a.fd[i] : b.fd[i]); break;
    case ZYDIS_MNEMONIC_MAXPD: EW(fd, 2, a.fd[i] > b.fd[i] ? a.fd[i] : b.fd[i]); break;
    case ZYDIS_MNEMONIC_MINSS: d->f[0] = a.f[0] < b.f[0] ? a.f[0] : b.f[0]; break;
    case ZYDIS_MNEMONIC_MAXSS: d->f[0] = a.f[0] > b.f[0] ? a.f[0] : b.f[0]; break;
    case ZYDIS_MNEMONIC_MINSD: d->fd[0] = a.fd[0] < b.fd[0] ? a.fd[0] : b.fd[0]; break;
    case ZYDIS_MNEMONIC_MAXSD: d->fd[0] = a.fd[0] > b.fd[0] ? a.fd[0] : b.fd[0]; break;
    case ZYDIS_MNEMONIC_SQRTPS: EW(f, 4, xf(__builtin_sqrtf(b.f[i]), b.f[i], b.f[i])); break;
    case ZYDIS_MNEMONIC_SQRTPD: EW(fd, 2, xd(__builtin_sqrt(b.fd[i]), b.fd[i], b.fd[i])); break;
    case ZYDIS_MNEMONIC_SQRTSS: d->f[0] = xf(__builtin_sqrtf(b.f[0]), b.f[0], b.f[0]); break;
    case ZYDIS_MNEMONIC_SQRTSD: d->fd[0] = xd(__builtin_sqrt(b.fd[0]), b.fd[0], b.fd[0]); break;
    case ZYDIS_MNEMONIC_RCPPS: EW(f, 4, 1.0f / b.f[i]); break;
    case ZYDIS_MNEMONIC_RSQRTPS: EW(f, 4, 1.0f / __builtin_sqrtf(b.f[i])); break;
    case ZYDIS_MNEMONIC_RCPSS: d->f[0] = 1.0f / b.f[0]; break;
    case ZYDIS_MNEMONIC_RSQRTSS: d->f[0] = 1.0f / __builtin_sqrtf(b.f[0]); break;
    case ZYDIS_MNEMONIC_ADDSUBPS: EW(f, 4, (i & 1) ? FOP(a.f[i], +, b.f[i]) : FOP(a.f[i], -, b.f[i])); break;
    case ZYDIS_MNEMONIC_ADDSUBPD: EW(fd, 2, (i & 1) ? DOP(a.fd[i], +, b.fd[i]) : DOP(a.fd[i], -, b.fd[i])); break;
    case ZYDIS_MNEMONIC_HADDPS: { V128 t; for (int i = 0; i < 2; i++) { t.f[i] = FOP(a.f[2*i], +, a.f[2*i+1]); t.f[i+2] = FOP(b.f[2*i], +, b.f[2*i+1]); } *d = t; break; }
    case ZYDIS_MNEMONIC_HSUBPS: { V128 t; for (int i = 0; i < 2; i++) { t.f[i] = FOP(a.f[2*i], -, a.f[2*i+1]); t.f[i+2] = FOP(b.f[2*i], -, b.f[2*i+1]); } *d = t; break; }
    case ZYDIS_MNEMONIC_HADDPD: { double t0 = DOP(a.fd[0], +, a.fd[1]), t1 = DOP(b.fd[0], +, b.fd[1]); d->fd[0] = t0; d->fd[1] = t1; break; }
    case ZYDIS_MNEMONIC_HSUBPD: { double t0 = DOP(a.fd[0], -, a.fd[1]), t1 = DOP(b.fd[0], -, b.fd[1]); d->fd[0] = t0; d->fd[1] = t1; break; }
    case ZYDIS_MNEMONIC_PHADDW: for (int i = 0; i < 4; i++) { d->w[i] = a.w[2*i] + a.w[2*i+1]; d->w[i+4] = b.w[2*i] + b.w[2*i+1]; } break;
    case ZYDIS_MNEMONIC_PHSUBW: for (int i = 0; i < 4; i++) { d->w[i] = a.w[2*i] - a.w[2*i+1]; d->w[i+4] = b.w[2*i] - b.w[2*i+1]; } break;
    case ZYDIS_MNEMONIC_PHADDD: for (int i = 0; i < 2; i++) { d->d[i] = a.d[2*i] + a.d[2*i+1]; d->d[i+2] = b.d[2*i] + b.d[2*i+1]; } break;
    case ZYDIS_MNEMONIC_PHSUBD: for (int i = 0; i < 2; i++) { d->d[i] = a.d[2*i] - a.d[2*i+1]; d->d[i+2] = b.d[2*i] - b.d[2*i+1]; } break;
    case ZYDIS_MNEMONIC_PHADDSW: for (int i = 0; i < 4; i++) { d->sw[i] = SAT_I16(a.sw[2*i] + a.sw[2*i+1]); d->sw[i+4] = SAT_I16(b.sw[2*i] + b.sw[2*i+1]); } break;
    case ZYDIS_MNEMONIC_PHSUBSW: for (int i = 0; i < 4; i++) { d->sw[i] = SAT_I16(a.sw[2*i] - a.sw[2*i+1]); d->sw[i+4] = SAT_I16(b.sw[2*i] - b.sw[2*i+1]); } break;
    case ZYDIS_MNEMONIC_ANDPS: case ZYDIS_MNEMONIC_ANDPD: case ZYDIS_MNEMONIC_PAND: EW(q, 2, a.q[i] & b.q[i]); break;
    case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ORPD: case ZYDIS_MNEMONIC_POR: EW(q, 2, a.q[i] | b.q[i]); break;
    case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_XORPD: case ZYDIS_MNEMONIC_PXOR: EW(q, 2, a.q[i] ^ b.q[i]); break;
    case ZYDIS_MNEMONIC_ANDNPS: case ZYDIS_MNEMONIC_ANDNPD: case ZYDIS_MNEMONIC_PANDN: EW(q, 2, ~a.q[i] & b.q[i]); break;
    #define UNPCK(field, n, high) do { int h_ = (n) / 2, o_ = (high) ? h_ : 0; \
        for (int i = 0; i < h_; i++) { d->field[2*i] = a.field[i + o_]; d->field[2*i+1] = b.field[i + o_]; } } while (0)
    case ZYDIS_MNEMONIC_UNPCKLPS: case ZYDIS_MNEMONIC_PUNPCKLDQ: UNPCK(d, 4, 0); break;
    case ZYDIS_MNEMONIC_UNPCKHPS: case ZYDIS_MNEMONIC_PUNPCKHDQ: UNPCK(d, 4, 1); break;
    case ZYDIS_MNEMONIC_UNPCKLPD: case ZYDIS_MNEMONIC_PUNPCKLQDQ: UNPCK(q, 2, 0); break;
    case ZYDIS_MNEMONIC_UNPCKHPD: case ZYDIS_MNEMONIC_PUNPCKHQDQ: UNPCK(q, 2, 1); break;
    case ZYDIS_MNEMONIC_PUNPCKLBW: UNPCK(b, 16, 0); break;
    case ZYDIS_MNEMONIC_PUNPCKHBW: UNPCK(b, 16, 1); break;
    case ZYDIS_MNEMONIC_PUNPCKLWD: UNPCK(w, 8, 0); break;
    case ZYDIS_MNEMONIC_PUNPCKHWD: UNPCK(w, 8, 1); break;
    #undef UNPCK
    case ZYDIS_MNEMONIC_MOVHLPS: d->q[0] = b.q[1]; break;
    case ZYDIS_MNEMONIC_MOVLHPS: d->q[1] = b.q[0]; break;
    case ZYDIS_MNEMONIC_MOVSHDUP: d->d[0] = d->d[1] = b.d[1]; d->d[2] = d->d[3] = b.d[3]; break;
    case ZYDIS_MNEMONIC_MOVSLDUP: d->d[0] = d->d[1] = b.d[0]; d->d[2] = d->d[3] = b.d[2]; break;
    case ZYDIS_MNEMONIC_MOVDDUP: d->q[0] = d->q[1] = b.q[0]; break;

    case ZYDIS_MNEMONIC_PADDB: EW(b, 16, (u8)(a.b[i] + b.b[i])); break;
    case ZYDIS_MNEMONIC_PADDW: EW(w, 8, (u16)(a.w[i] + b.w[i])); break;
    case ZYDIS_MNEMONIC_PADDD: EW(d, 4, a.d[i] + b.d[i]); break;
    case ZYDIS_MNEMONIC_PADDQ: EW(q, 2, a.q[i] + b.q[i]); break;
    case ZYDIS_MNEMONIC_PSUBB: EW(b, 16, (u8)(a.b[i] - b.b[i])); break;
    case ZYDIS_MNEMONIC_PSUBW: EW(w, 8, (u16)(a.w[i] - b.w[i])); break;
    case ZYDIS_MNEMONIC_PSUBD: EW(d, 4, a.d[i] - b.d[i]); break;
    case ZYDIS_MNEMONIC_PSUBQ: EW(q, 2, a.q[i] - b.q[i]); break;
    case ZYDIS_MNEMONIC_PCMPEQB: EW(b, 16, a.b[i] == b.b[i] ? 0xFF : 0); break;
    case ZYDIS_MNEMONIC_PCMPEQW: EW(w, 8, a.w[i] == b.w[i] ? 0xFFFF : 0); break;
    case ZYDIS_MNEMONIC_PCMPEQD: EW(d, 4, a.d[i] == b.d[i] ? ~0u : 0); break;
    case ZYDIS_MNEMONIC_PCMPEQQ: EW(q, 2, a.q[i] == b.q[i] ? ~0ull : 0); break;
    case ZYDIS_MNEMONIC_PCMPGTB: EW(b, 16, a.sb[i] > b.sb[i] ? 0xFF : 0); break;
    case ZYDIS_MNEMONIC_PCMPGTW: EW(w, 8, a.sw[i] > b.sw[i] ? 0xFFFF : 0); break;
    case ZYDIS_MNEMONIC_PCMPGTD: EW(d, 4, a.sd[i] > b.sd[i] ? ~0u : 0); break;
    case ZYDIS_MNEMONIC_PCMPGTQ: EW(q, 2, a.sq[i] > b.sq[i] ? ~0ull : 0); break;
    case ZYDIS_MNEMONIC_PMINSB: EW(sb, 16, a.sb[i] < b.sb[i] ? a.sb[i] : b.sb[i]); break;
    case ZYDIS_MNEMONIC_PMINSW: EW(sw, 8, a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i]); break;
    case ZYDIS_MNEMONIC_PMINSD: EW(sd, 4, a.sd[i] < b.sd[i] ? a.sd[i] : b.sd[i]); break;
    case ZYDIS_MNEMONIC_PMAXSB: EW(sb, 16, a.sb[i] > b.sb[i] ? a.sb[i] : b.sb[i]); break;
    case ZYDIS_MNEMONIC_PMAXSW: EW(sw, 8, a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i]); break;
    case ZYDIS_MNEMONIC_PMAXSD: EW(sd, 4, a.sd[i] > b.sd[i] ? a.sd[i] : b.sd[i]); break;
    case ZYDIS_MNEMONIC_PMINUB: EW(b, 16, a.b[i] < b.b[i] ? a.b[i] : b.b[i]); break;
    case ZYDIS_MNEMONIC_PMINUW: EW(w, 8, a.w[i] < b.w[i] ? a.w[i] : b.w[i]); break;
    case ZYDIS_MNEMONIC_PMINUD: EW(d, 4, a.d[i] < b.d[i] ? a.d[i] : b.d[i]); break;
    case ZYDIS_MNEMONIC_PMAXUB: EW(b, 16, a.b[i] > b.b[i] ? a.b[i] : b.b[i]); break;
    case ZYDIS_MNEMONIC_PMAXUW: EW(w, 8, a.w[i] > b.w[i] ? a.w[i] : b.w[i]); break;
    case ZYDIS_MNEMONIC_PMAXUD: EW(d, 4, a.d[i] > b.d[i] ? a.d[i] : b.d[i]); break;
    case ZYDIS_MNEMONIC_PABSB: EW(b, 16, (u8)(b.sb[i] < 0 ? -b.sb[i] : b.sb[i])); break;
    case ZYDIS_MNEMONIC_PABSW: EW(w, 8, (u16)(b.sw[i] < 0 ? -b.sw[i] : b.sw[i])); break;
    case ZYDIS_MNEMONIC_PABSD: EW(d, 4, (u32)(b.sd[i] < 0 ? -(i64)b.sd[i] : b.sd[i])); break;
    case ZYDIS_MNEMONIC_PSIGNB: EW(sb, 16, (i8)(b.sb[i] < 0 ? -a.sb[i] : b.sb[i] == 0 ? 0 : a.sb[i])); break;
    case ZYDIS_MNEMONIC_PSIGNW: EW(sw, 8, (i16)(b.sw[i] < 0 ? -a.sw[i] : b.sw[i] == 0 ? 0 : a.sw[i])); break;
    case ZYDIS_MNEMONIC_PSIGND: EW(sd, 4, (i32)(b.sd[i] < 0 ? (i32)(0u - (u32)a.sd[i]) : b.sd[i] == 0 ? 0 : a.sd[i])); break;
    case ZYDIS_MNEMONIC_PADDSB: EW(sb, 16, SAT_I8((i32)a.sb[i] + b.sb[i])); break;
    case ZYDIS_MNEMONIC_PADDSW: EW(sw, 8, SAT_I16((i32)a.sw[i] + b.sw[i])); break;
    case ZYDIS_MNEMONIC_PSUBSB: EW(sb, 16, SAT_I8((i32)a.sb[i] - b.sb[i])); break;
    case ZYDIS_MNEMONIC_PSUBSW: EW(sw, 8, SAT_I16((i32)a.sw[i] - b.sw[i])); break;
    case ZYDIS_MNEMONIC_PADDUSB: EW(b, 16, SAT_U8((i32)a.b[i] + b.b[i])); break;
    case ZYDIS_MNEMONIC_PADDUSW: EW(w, 8, SAT_U16((i32)a.w[i] + b.w[i])); break;
    case ZYDIS_MNEMONIC_PSUBUSB: EW(b, 16, SAT_U8((i32)a.b[i] - b.b[i])); break;
    case ZYDIS_MNEMONIC_PSUBUSW: EW(w, 8, SAT_U16((i32)a.w[i] - b.w[i])); break;
    case ZYDIS_MNEMONIC_PMULLW: EW(w, 8, (u16)(a.w[i] * b.w[i])); break;
    case ZYDIS_MNEMONIC_PMULLD: EW(d, 4, (u32)((u64)a.d[i] * b.d[i])); break;
    case ZYDIS_MNEMONIC_PMULHW: EW(w, 8, (u16)(((i32)a.sw[i] * b.sw[i]) >> 16)); break;
    case ZYDIS_MNEMONIC_PMULHUW: EW(w, 8, (u16)(((u32)a.w[i] * b.w[i]) >> 16)); break;
    case ZYDIS_MNEMONIC_PMULHRSW: EW(w, 8, (u16)(((((i32)a.sw[i] * b.sw[i]) >> 14) + 1) >> 1)); break;
    case ZYDIS_MNEMONIC_PMULUDQ: d->q[0] = (u64)a.d[0] * b.d[0]; d->q[1] = (u64)a.d[2] * b.d[2]; break;
    case ZYDIS_MNEMONIC_PMULDQ: d->sq[0] = (i64)a.sd[0] * b.sd[0]; d->sq[1] = (i64)a.sd[2] * b.sd[2]; break;
    case ZYDIS_MNEMONIC_PMADDWD:
        for (int i = 0; i < 4; i++) d->sd[i] = (i32)((u32)((i32)a.sw[2*i] * b.sw[2*i]) + (u32)((i32)a.sw[2*i+1] * b.sw[2*i+1]));
        break;
    case ZYDIS_MNEMONIC_PMADDUBSW: for (int i = 0; i < 8; i++) d->sw[i] = SAT_I16((i32)a.b[2*i] * b.sb[2*i] + (i32)a.b[2*i+1] * b.sb[2*i+1]); break;
    case ZYDIS_MNEMONIC_PAVGB: EW(b, 16, (u8)(((u32)a.b[i] + b.b[i] + 1) >> 1)); break;
    case ZYDIS_MNEMONIC_PAVGW: EW(w, 8, (u16)(((u32)a.w[i] + b.w[i] + 1) >> 1)); break;
    case ZYDIS_MNEMONIC_PSADBW:
        for (int j = 0; j < 2; j++) { u32 t = 0; for (int i = 0; i < 8; i++) { int x = (int)a.b[8*j+i] - b.b[8*j+i]; t += x < 0 ? -x : x; } d->q[j] = t; }
        break;
    case ZYDIS_MNEMONIC_PSHUFB: EW(b, 16, (b.b[i] & 0x80) ? 0 : a.b[b.b[i] & 15]); break;
    case ZYDIS_MNEMONIC_PACKSSWB: for (int i = 0; i < 8; i++) { d->sb[i] = SAT_I8(a.sw[i]); d->sb[8+i] = SAT_I8(b.sw[i]); } break;
    case ZYDIS_MNEMONIC_PACKUSWB: for (int i = 0; i < 8; i++) { d->b[i] = SAT_U8(a.sw[i]); d->b[8+i] = SAT_U8(b.sw[i]); } break;
    case ZYDIS_MNEMONIC_PACKSSDW: for (int i = 0; i < 4; i++) { d->sw[i] = SAT_I16(a.sd[i]); d->sw[4+i] = SAT_I16(b.sd[i]); } break;
    case ZYDIS_MNEMONIC_PACKUSDW: for (int i = 0; i < 4; i++) { d->w[i] = SAT_U16(a.sd[i]); d->w[4+i] = SAT_U16(b.sd[i]); } break;
    case ZYDIS_MNEMONIC_VPSLLVD: EW(d, 4, b.d[i] > 31 ? 0 : a.d[i] << b.d[i]); break;
    case ZYDIS_MNEMONIC_VPSLLVQ: EW(q, 2, b.q[i] > 63 ? 0 : a.q[i] << b.q[i]); break;
    case ZYDIS_MNEMONIC_VPSRLVD: EW(d, 4, b.d[i] > 31 ? 0 : a.d[i] >> b.d[i]); break;
    case ZYDIS_MNEMONIC_VPSRLVQ: EW(q, 2, b.q[i] > 63 ? 0 : a.q[i] >> b.q[i]); break;
    case ZYDIS_MNEMONIC_VPSRAVD: EW(d, 4, (u32)(a.sd[i] >> (b.d[i] > 31 ? 31 : b.d[i]))); break;
    case ZYDIS_MNEMONIC_SHUFPS: d->d[0] = a.d[imm & 3]; d->d[1] = a.d[imm >> 2 & 3]; d->d[2] = b.d[imm >> 4 & 3]; d->d[3] = b.d[imm >> 6 & 3]; break;
    case ZYDIS_MNEMONIC_SHUFPD: d->q[0] = a.q[imm >> (2 * L) & 1]; d->q[1] = b.q[imm >> (2 * L + 1) & 1]; break;
    case ZYDIS_MNEMONIC_PSHUFD: for (int i = 0; i < 4; i++) d->d[i] = b.d[imm >> (2 * i) & 3]; break;
    case ZYDIS_MNEMONIC_PSHUFLW: *d = b; for (int i = 0; i < 4; i++) d->w[i] = b.w[imm >> (2 * i) & 3]; break;
    case ZYDIS_MNEMONIC_PSHUFHW: *d = b; for (int i = 0; i < 4; i++) d->w[4 + i] = b.w[4 + (imm >> (2 * i) & 3)]; break;
    case ZYDIS_MNEMONIC_BLENDPS: for (int i = 0; i < 4; i++) if (imm >> (4 * L + i) & 1) d->d[i] = b.d[i]; break;
    case ZYDIS_MNEMONIC_BLENDPD: for (int i = 0; i < 2; i++) if (imm >> (2 * L + i) & 1) d->q[i] = b.q[i]; break;
    case ZYDIS_MNEMONIC_PBLENDW: for (int i = 0; i < 8; i++) if (imm >> i & 1) d->w[i] = b.w[i]; break;
    case ZYDIS_MNEMONIC_VPBLENDD: for (int i = 0; i < 4; i++) if (imm >> (4 * L + i) & 1) d->d[i] = b.d[i]; break;
    case ZYDIS_MNEMONIC_BLENDVPS: EW(d, 4, (cc.d[i] >> 31) ? b.d[i] : a.d[i]); break;
    case ZYDIS_MNEMONIC_BLENDVPD: EW(q, 2, (cc.q[i] >> 63) ? b.q[i] : a.q[i]); break;
    case ZYDIS_MNEMONIC_PBLENDVB: EW(b, 16, (cc.b[i] >> 7) ? b.b[i] : a.b[i]); break;
    case ZYDIS_MNEMONIC_PALIGNR: {
        u8 t[32]; memcpy(t, &b, 16); memcpy(t + 16, &a, 16);
        for (int i = 0; i < 16; i++) d->b[i] = i + imm < 32 ? t[i + imm] : 0;
        break; }
    case ZYDIS_MNEMONIC_ROUNDPS: EW(f, 4, RNDF(b.f[i], (imm & 4) ? 0 : imm & 3)); break;
    case ZYDIS_MNEMONIC_ROUNDPD: EW(fd, 2, RNDD(b.fd[i], (imm & 4) ? 0 : imm & 3)); break;
    case ZYDIS_MNEMONIC_ROUNDSS: d->f[0] = RNDF(b.f[0], (imm & 4) ? 0 : imm & 3); break;
    case ZYDIS_MNEMONIC_ROUNDSD: d->fd[0] = RNDD(b.fd[0], (imm & 4) ? 0 : imm & 3); break;
    case ZYDIS_MNEMONIC_DPPS: {
        float p[4];
        for (int i = 0; i < 4; i++) p[i] = (imm >> (4 + i) & 1) ? a.f[i] * b.f[i] : 0.0f;
        float t = (p[0] + p[1]) + (p[2] + p[3]);
        for (int i = 0; i < 4; i++) d->f[i] = (imm >> i & 1) ? t : 0.0f;
        break; }
    case ZYDIS_MNEMONIC_DPPD: {
        double p0 = (imm >> 4 & 1) ? a.fd[0] * b.fd[0] : 0.0, p1 = (imm >> 5 & 1) ? a.fd[1] * b.fd[1] : 0.0, t = p0 + p1;
        d->fd[0] = (imm & 1) ? t : 0.0; d->fd[1] = (imm & 2) ? t : 0.0;
        break; }
    case ZYDIS_MNEMONIC_CMPPS: EW(d, 4, cmp_pred(imm, a.f[i], b.f[i]) ? ~0u : 0); break;
    case ZYDIS_MNEMONIC_CMPPD: EW(q, 2, cmp_pred(imm, a.fd[i], b.fd[i]) ? ~0ull : 0); break;
    case ZYDIS_MNEMONIC_CMPSS: d->d[0] = cmp_pred(imm, a.f[0], b.f[0]) ? ~0u : 0; break;
    case ZYDIS_MNEMONIC_CMPSD: d->q[0] = cmp_pred(imm, a.fd[0], b.fd[0]) ? ~0ull : 0; break;
    case ZYDIS_MNEMONIC_CVTDQ2PS: EW(f, 4, (float)b.sd[i]); break;
    case ZYDIS_MNEMONIC_CVTPS2DQ: EW(d, 4, (u32)CVTS2I32(b.f[i])); break;
    case ZYDIS_MNEMONIC_CVTTPS2DQ: EW(d, 4, (u32)CVTTS2I32(b.f[i])); break;
    case ZYDIS_MNEMONIC_CVTSS2SD: d->fd[0] = (double)b.f[0]; break;
    case ZYDIS_MNEMONIC_CVTSD2SS: d->f[0] = (float)b.fd[0]; break;
    case ZYDIS_MNEMONIC_PHMINPOSUW: {
        u16 mv = b.w[0]; int mi = 0;
        for (int i = 1; i < 8; i++) if (b.w[i] < mv) { mv = b.w[i]; mi = i; }
        *d = (V128){0}; d->w[0] = mv; d->w[1] = (u16)mi;
        break; }
    default: break;
    }
    (void)s;
}

static void fma_kernel(const FmaInfo *fi, V128 *d, V128 a, V128 b, V128 c) {
    int n = fi->scalar ? 1 : fi->dbl ? 2 : 4;
    for (int i = 0; i < n; i++) {
        if (fi->dbl) {
            double A = a.fd[i], B = b.fd[i], C = c.fd[i], x, y, z;
            if (fi->form == 132) { x = A; y = C; z = B; } else if (fi->form == 213) { x = B; y = A; z = C; } else { x = B; y = C; z = A; }
            double r;
            switch (fi->kind) {
            case FMA_MADD: r = __builtin_fma(x, y, z); break;
            case FMA_MSUB: r = __builtin_fma(x, y, -z); break;
            case FMA_NMADD: r = __builtin_fma(-x, y, z); break;
            case FMA_NMSUB: r = __builtin_fma(-x, y, -z); break;
            case FMA_MADDSUB: r = (i & 1) ? __builtin_fma(x, y, z) : __builtin_fma(x, y, -z); break;
            default: r = (i & 1) ? __builtin_fma(x, y, -z) : __builtin_fma(x, y, z); break;
            }
            if (r != r) {   // x86: the first NaN multiplicand/addend (x, y, z) propagates, quieted and un-negated
                u64 u; double q = x != x ? x : y != y ? y : z != z ? z : 0;
                if (q != q) { memcpy(&u, &q, 8); u |= 1ull << 51; } else u = 0xFFF8000000000000ull;
                memcpy(&r, &u, 8);
            }
            d->fd[i] = r;
        } else {
            float A = a.f[i], B = b.f[i], C = c.f[i], x, y, z;
            if (fi->form == 132) { x = A; y = C; z = B; } else if (fi->form == 213) { x = B; y = A; z = C; } else { x = B; y = C; z = A; }
            float r;
            switch (fi->kind) {
            case FMA_MADD: r = __builtin_fmaf(x, y, z); break;
            case FMA_MSUB: r = __builtin_fmaf(x, y, -z); break;
            case FMA_NMADD: r = __builtin_fmaf(-x, y, z); break;
            case FMA_NMSUB: r = __builtin_fmaf(-x, y, -z); break;
            case FMA_MADDSUB: r = (i & 1) ? __builtin_fmaf(x, y, z) : __builtin_fmaf(x, y, -z); break;
            default: r = (i & 1) ? __builtin_fmaf(x, y, -z) : __builtin_fmaf(x, y, z); break;
            }
            if (r != r) {
                u32 u; float q = x != x ? x : y != y ? y : z != z ? z : 0;
                if (q != q) { memcpy(&u, &q, 4); u |= 1u << 22; } else u = 0xFFC00000u;
                memcpy(&r, &u, 4);
            }
            d->f[i] = r;
        }
    }
}

static int last_imm(IState *s) {
    int nv = NVIS(s);
    return nv && is_imm(s, nv - 1) ? (int)(IOP(s, nv - 1)->imm.value.u & 0xFF) : 0;
}

// Generic lane engine (mirrors VectorMixin.vop in recomp/vector.py).
static int vop(IState *s, int arity, int m, const FmaInfo *fi) {
    int vex = IS_VEX(s), nv = NVIS(s);
    int nops = nv - (nv && is_imm(s, nv - 1) ? 1 : 0);
    int D, A, B, C = -1;
    if (arity == 1) { D = 0; A = 0; B = nops > 1 ? 1 : 0; }
    else if (arity == 2) { D = 0; A = vex ? 1 : 0; B = vex ? 2 : 1; }
    else if (fi) { D = 0; A = 0; B = 1; C = 2; }
    else if (vex) { D = 0; A = 1; B = 2; C = 3; }
    else { D = 0; A = 0; B = 1; C = 2; }
    int dbits, dr = vreg(s, D, &dbits);
    if (dr < 0) return X_UNIMPL;
    int lanes = vwidth(s) == 256 ? 2 : 1, imm = last_imm(s);
    V128 out[2];
    for (int L = 0; L < lanes; L++) {
        V128 a = lane_src(s, A, L), b = lane_src(s, B, L), c = C >= 0 ? lane_src(s, C, L) : (V128){0};
        V128 d = a;
        if (fi) fma_kernel(fi, &d, a, b, c);
        else lane_kernel(s, m, &d, a, b, c, imm, L);
        out[L] = d;
    }
    s->c->x[dr] = out[0];
    if (lanes == 2) s->c->xh[dr] = out[1];
    else if (vex) s->c->xh[dr] = (V128){0};
    return X_NEXT;
}

// ---- packed shifts: count from imm or from the low 64 bits of an xmm/m128 -----------------------------------------
static int vshift(IState *s, int m) {
    int vex = IS_VEX(s), nv = NVIS(s);
    int S = vex ? 1 : 0, cnt_i = vex ? 2 : 1;
    if (nv <= cnt_i) return X_UNIMPL;
    u64 cnt = is_imm(s, cnt_i) ? (IOP(s, cnt_i)->imm.value.u & 0xFF) : lane_src(s, cnt_i, 0).q[0];
    int dbits, dr = vreg(s, 0, &dbits);
    int lanes = vwidth(s) == 256 ? 2 : 1;
    V128 out[2];
    for (int L = 0; L < lanes; L++) {
        V128 a = lane_src(s, S, L), dd = a, *d = &dd;
        switch (m) {
        case ZYDIS_MNEMONIC_PSLLW: EW(w, 8, cnt > 15 ? 0 : (u16)(a.w[i] << cnt)); break;
        case ZYDIS_MNEMONIC_PSLLD: EW(d, 4, cnt > 31 ? 0 : a.d[i] << cnt); break;
        case ZYDIS_MNEMONIC_PSLLQ: EW(q, 2, cnt > 63 ? 0 : a.q[i] << cnt); break;
        case ZYDIS_MNEMONIC_PSRLW: EW(w, 8, cnt > 15 ? 0 : (u16)(a.w[i] >> cnt)); break;
        case ZYDIS_MNEMONIC_PSRLD: EW(d, 4, cnt > 31 ? 0 : a.d[i] >> cnt); break;
        case ZYDIS_MNEMONIC_PSRLQ: EW(q, 2, cnt > 63 ? 0 : a.q[i] >> cnt); break;
        case ZYDIS_MNEMONIC_PSRAW: EW(sw, 8, (i16)(a.sw[i] >> (cnt > 15 ? 15 : cnt))); break;
        case ZYDIS_MNEMONIC_PSRAD: EW(sd, 4, a.sd[i] >> (cnt > 31 ? 31 : cnt)); break;
        case ZYDIS_MNEMONIC_PSLLDQ: dd = (V128){0}; if (cnt < 16) memcpy(&dd.b[cnt], &a.b[0], 16 - cnt); break;
        case ZYDIS_MNEMONIC_PSRLDQ: dd = (V128){0}; if (cnt < 16) memcpy(&dd.b[0], &a.b[cnt], 16 - cnt); break;
        default: return X_UNIMPL;
        }
        out[L] = dd;
    }
    s->c->x[dr] = out[0];
    if (lanes == 2) s->c->xh[dr] = out[1]; else if (vex) s->c->xh[dr] = (V128){0};
    return X_NEXT;
}

// ---- PMOVZX / PMOVSX (widening; source is the low part of xmm or a narrower memory operand) ----------------------
static int pmovx(IState *s, int m) {
    int sw, dw, sgn;
    switch (m) {
    case ZYDIS_MNEMONIC_PMOVSXBW: sw = 1; dw = 2; sgn = 1; break;  case ZYDIS_MNEMONIC_PMOVZXBW: sw = 1; dw = 2; sgn = 0; break;
    case ZYDIS_MNEMONIC_PMOVSXBD: sw = 1; dw = 4; sgn = 1; break;  case ZYDIS_MNEMONIC_PMOVZXBD: sw = 1; dw = 4; sgn = 0; break;
    case ZYDIS_MNEMONIC_PMOVSXBQ: sw = 1; dw = 8; sgn = 1; break;  case ZYDIS_MNEMONIC_PMOVZXBQ: sw = 1; dw = 8; sgn = 0; break;
    case ZYDIS_MNEMONIC_PMOVSXWD: sw = 2; dw = 4; sgn = 1; break;  case ZYDIS_MNEMONIC_PMOVZXWD: sw = 2; dw = 4; sgn = 0; break;
    case ZYDIS_MNEMONIC_PMOVSXWQ: sw = 2; dw = 8; sgn = 1; break;  case ZYDIS_MNEMONIC_PMOVZXWQ: sw = 2; dw = 8; sgn = 0; break;
    case ZYDIS_MNEMONIC_PMOVSXDQ: sw = 4; dw = 8; sgn = 1; break;  case ZYDIS_MNEMONIC_PMOVZXDQ: sw = 4; dw = 8; sgn = 0; break;
    default: return X_UNIMPL;
    }
    int dbits; vreg(s, 0, &dbits);
    int n = dbits / 8 / dw;
    V256 src = full_src(s, 1, is_mem(s, 1) ? n * sw : 0), out; memset(&out, 0, sizeof out);
    for (int i = 0; i < n; i++) {
        u64 v = 0; memcpy(&v, &src.b[i * sw], sw);
        if (sgn) v = sext(v, sw * 8);
        memcpy(&out.b[i * dw], &v, dw);
    }
    full_dst(s, 0, out);
    return X_NEXT;
}

static void comis(IState *s, double a, double b) {
    if (a != a || b != b) { s->zf = s->pf = s->cf = 1; }
    else { s->zf = a == b; s->pf = 0; s->cf = a < b; }
    s->of = s->sf = s->af = 0;
}

// ---- entry ---------------------------------------------------------------------------------------------------------
int rt_interp_vec(IState *s) {
    pthread_once(&mn_once, init_mn);
    Ctx *c = s->c;
    const ZydisDecodedInstruction *in = s->in;
    int nv = NVIS(s), vex = IS_VEX(s);
    int mn = in->mnemonic, m = base_mn[mn];
    for (int i = 0; i < nv; i++)   // MMX forms are not supported
        if (IOP(s, i)->type == ZYDIS_OPERAND_TYPE_REGISTER && rt_ireg[IOP(s, i)->reg.value].kind == RK_MMX) return X_UNIMPL;

    if (fma_info[mn].kind) return vop(s, 3, mn, &fma_info[mn]);
    if ((mn == ZYDIS_MNEMONIC_VPERMILPS || mn == ZYDIS_MNEMONIC_VPERMILPD) && is_imm(s, nv - 1)) {
        int dbits, dr = vreg(s, 0, &dbits), imm = last_imm(s), lanes = dbits == 256 ? 2 : 1;
        V128 out[2];
        for (int L = 0; L < lanes; L++) {
            V128 b = lane_src(s, 1, L), d;
            if (mn == ZYDIS_MNEMONIC_VPERMILPS) for (int i = 0; i < 4; i++) d.d[i] = b.d[imm >> (2 * i) & 3];
            else for (int i = 0; i < 2; i++) d.q[i] = b.q[imm >> (2 * L + i) & 1];
            out[L] = d;
        }
        c->x[dr] = out[0]; c->xh[dr] = lanes == 2 ? out[1] : (V128){0};
        return X_NEXT;
    }
    if (mn == ZYDIS_MNEMONIC_VPERMILPS || mn == ZYDIS_MNEMONIC_VPERMILPD) {
        int dbits, dr = vreg(s, 0, &dbits), lanes = dbits == 256 ? 2 : 1;
        V128 out[2];
        for (int L = 0; L < lanes; L++) {
            V128 a = lane_src(s, 1, L), b = lane_src(s, 2, L), d;
            if (mn == ZYDIS_MNEMONIC_VPERMILPS) for (int i = 0; i < 4; i++) d.d[i] = a.d[b.d[i] & 3];
            else for (int i = 0; i < 2; i++) d.q[i] = a.q[(b.q[i] >> 1) & 1];
            out[L] = d;
        }
        c->x[dr] = out[0]; c->xh[dr] = lanes == 2 ? out[1] : (V128){0};
        return X_NEXT;
    }
    int ar = lane_arity(m);
    if (ar) return vop(s, ar, m, NULL);

    switch (m) {
    // ---- moves ----
    case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVAPD: case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_MOVDQA: case ZYDIS_MNEMONIC_MOVDQU: case ZYDIS_MNEMONIC_LDDQU: case ZYDIS_MNEMONIC_MOVNTDQ:
    case ZYDIS_MNEMONIC_MOVNTPS: case ZYDIS_MNEMONIC_MOVNTPD: case ZYDIS_MNEMONIC_MOVNTDQA: {
        if (is_mem(s, 0)) {
            int bits, r = vreg(s, 1, &bits); u64 a = iea(s, IOP(s, 0));
            STX(a, c->x[r]); if (bits == 256) STX(a + 16, c->xh[r]);
            return X_NEXT;
        }
        int bits, r = vreg(s, 0, &bits);
        V128 lo = lane_src(s, 1, 0);
        if (bits == 256) { V128 hi = lane_src(s, 1, 1); c->x[r] = lo; c->xh[r] = hi; }
        else set_x(s, r, lo);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_MOVSS: case ZYDIS_MNEMONIC_MOVSD: {
        int w = m == ZYDIS_MNEMONIC_MOVSS ? 32 : 64;
        if (is_mem(s, 0)) {
            V128 v = c->x[vreg(s, 1, NULL)];
            if (w == 32) ST32(iea(s, IOP(s, 0)), v.d[0]); else ST64(iea(s, IOP(s, 0)), v.q[0]);
            return X_NEXT;
        }
        int r = vreg(s, 0, NULL);
        if (is_mem(s, 1)) { V128 v = w == 32 ? LDV32(iea(s, IOP(s, 1))) : LDV64(iea(s, IOP(s, 1))); set_x(s, r, v); return X_NEXT; }
        V128 a = vex ? c->x[vreg(s, 1, NULL)] : c->x[r], b = c->x[vreg(s, vex ? 2 : 1, NULL)];
        if (w == 32) a.d[0] = b.d[0]; else a.q[0] = b.q[0];
        set_x(s, r, a);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_MOVD: case ZYDIS_MNEMONIC_MOVQ: {
        int w = m == ZYDIS_MNEMONIC_MOVD ? 32 : 64;
        int r0 = vreg(s, 0, NULL), r1 = vreg(s, 1, NULL);
        if (r0 >= 0) {   // to xmm: zero-extend
            V128 v = {0};
            if (r1 >= 0) v.q[0] = c->x[r1].q[0];
            else if (is_mem(s, 1)) v = w == 32 ? LDV32(iea(s, IOP(s, 1))) : LDV64(iea(s, IOP(s, 1)));
            else v.q[0] = greg_rd(s, IOP(s, 1)->reg.value) & imask(w);
            set_x(s, r0, v);
            return X_NEXT;
        }
        V128 v = c->x[r1];
        if (is_mem(s, 0)) { if (w == 32) ST32(iea(s, IOP(s, 0)), v.d[0]); else ST64(iea(s, IOP(s, 0)), v.q[0]); }
        else greg_wr(s, IOP(s, 0)->reg.value, w == 32 ? v.d[0] : v.q[0]);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_MOVHPS: case ZYDIS_MNEMONIC_MOVHPD: case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVLPD: {
        int hi = m == ZYDIS_MNEMONIC_MOVHPS || m == ZYDIS_MNEMONIC_MOVHPD;
        if (is_mem(s, 0)) { ST64(iea(s, IOP(s, 0)), c->x[vreg(s, 1, NULL)].q[hi]); return X_NEXT; }
        int r = vreg(s, 0, NULL);
        V128 a = vex ? c->x[vreg(s, 1, NULL)] : c->x[r];
        a.q[hi] = LD64(iea(s, IOP(s, vex ? 2 : 1)));
        set_x(s, r, a);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_MASKMOVDQU: case ZYDIS_MNEMONIC_VMASKMOVDQU: {   // bytes of xmm1 whose mask byte (xmm2) has bit 7 set -> [rdi]
        V128 v = c->x[vreg(s, 0, NULL)], k = c->x[vreg(s, 1, NULL)];
        u8 *p = (u8 *)c->r[RDI];
        for (int i = 0; i < 16; i++) if (k.b[i] & 0x80) p[i] = v.b[i];
        return X_NEXT; }
    case ZYDIS_MNEMONIC_MOVMSKPS: case ZYDIS_MNEMONIC_MOVMSKPD: case ZYDIS_MNEMONIC_PMOVMSKB: {
        int bits, r = vreg(s, 1, &bits); u32 v = 0;
        V256 t; t.h[0] = c->x[r]; t.h[1] = c->xh[r];
        int lanes = bits == 256 ? 2 : 1;
        if (m == ZYDIS_MNEMONIC_MOVMSKPS) for (int i = 0; i < 4 * lanes; i++) v |= (t.d[i] >> 31) << i;
        else if (m == ZYDIS_MNEMONIC_MOVMSKPD) for (int i = 0; i < 2 * lanes; i++) v |= (u32)(t.q[i] >> 63) << i;
        else for (int i = 0; i < 16 * lanes; i++) v |= (u32)(t.b[i] >> 7) << i;
        greg_wr(s, IOP(s, 0)->reg.value, v);
        return X_NEXT; }

    case ZYDIS_MNEMONIC_PSLLW: case ZYDIS_MNEMONIC_PSLLD: case ZYDIS_MNEMONIC_PSLLQ: case ZYDIS_MNEMONIC_PSRLW:
    case ZYDIS_MNEMONIC_PSRLD: case ZYDIS_MNEMONIC_PSRLQ: case ZYDIS_MNEMONIC_PSRAW: case ZYDIS_MNEMONIC_PSRAD:
    case ZYDIS_MNEMONIC_PSLLDQ: case ZYDIS_MNEMONIC_PSRLDQ:
        return vshift(s, m);

    case ZYDIS_MNEMONIC_PMOVSXBW: case ZYDIS_MNEMONIC_PMOVZXBW: case ZYDIS_MNEMONIC_PMOVSXBD: case ZYDIS_MNEMONIC_PMOVZXBD:
    case ZYDIS_MNEMONIC_PMOVSXBQ: case ZYDIS_MNEMONIC_PMOVZXBQ: case ZYDIS_MNEMONIC_PMOVSXWD: case ZYDIS_MNEMONIC_PMOVZXWD:
    case ZYDIS_MNEMONIC_PMOVSXWQ: case ZYDIS_MNEMONIC_PMOVZXWQ: case ZYDIS_MNEMONIC_PMOVSXDQ: case ZYDIS_MNEMONIC_PMOVZXDQ:
        return pmovx(s, m);

    // ---- element insert / extract ----
    case ZYDIS_MNEMONIC_PINSRB: case ZYDIS_MNEMONIC_PINSRW: case ZYDIS_MNEMONIC_PINSRD: case ZYDIS_MNEMONIC_PINSRQ: {
        int r = vreg(s, 0, NULL), imm = last_imm(s);
        V128 a = vex ? c->x[vreg(s, 1, NULL)] : c->x[r];
        u64 v = rdi(s, vex ? 2 : 1);
        switch (m) {
        case ZYDIS_MNEMONIC_PINSRB: a.b[imm & 15] = (u8)v; break;
        case ZYDIS_MNEMONIC_PINSRW: a.w[imm & 7] = (u16)v; break;
        case ZYDIS_MNEMONIC_PINSRD: a.d[imm & 3] = (u32)v; break;
        default: a.q[imm & 1] = v; break;
        }
        set_x(s, r, a);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_PEXTRB: case ZYDIS_MNEMONIC_PEXTRW: case ZYDIS_MNEMONIC_PEXTRD: case ZYDIS_MNEMONIC_PEXTRQ:
    case ZYDIS_MNEMONIC_EXTRACTPS: {
        V128 v = c->x[vreg(s, 1, NULL)]; int imm = last_imm(s); u64 r;
        switch (m) {
        case ZYDIS_MNEMONIC_PEXTRB: r = v.b[imm & 15]; break;
        case ZYDIS_MNEMONIC_PEXTRW: r = v.w[imm & 7]; break;
        case ZYDIS_MNEMONIC_PEXTRQ: r = v.q[imm & 1]; break;
        default: r = v.d[imm & 3]; break;
        }
        if (is_mem(s, 0)) ist(iea(s, IOP(s, 0)), IOP(s, 0)->size, r);
        else greg_wr(s, IOP(s, 0)->reg.value, r);   // 32-bit register writes zero-extend
        return X_NEXT; }
    case ZYDIS_MNEMONIC_INSERTPS: {
        int r = vreg(s, 0, NULL), imm = last_imm(s), bi = vex ? 2 : 1;
        V128 a = vex ? c->x[vreg(s, 1, NULL)] : c->x[r];
        u32 v = is_mem(s, bi) ? LD32(iea(s, IOP(s, bi))) : c->x[vreg(s, bi, NULL)].d[(imm >> 6) & 3];
        a.d[(imm >> 4) & 3] = v;
        for (int i = 0; i < 4; i++) if (imm >> i & 1) a.d[i] = 0;
        set_x(s, r, a);
        return X_NEXT; }

    // ---- conversions ----
    case ZYDIS_MNEMONIC_CVTSI2SS: case ZYDIS_MNEMONIC_CVTSI2SD: {
        int r = vreg(s, 0, NULL), si = vex ? 2 : 1;
        V128 a = vex ? c->x[vreg(s, 1, NULL)] : c->x[r];
        i64 v = (i64)sext(rdi(s, si), IOP(s, si)->size);
        if (m == ZYDIS_MNEMONIC_CVTSI2SS) a.f[0] = (float)v; else a.fd[0] = (double)v;
        set_x(s, r, a);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_CVTSS2SI: case ZYDIS_MNEMONIC_CVTTSD2SI: case ZYDIS_MNEMONIC_CVTSD2SI: {
        V128 v = lane_src(s, 1, 0); int w = IOP(s, 0)->size; u64 r;
        switch (m) {
        case ZYDIS_MNEMONIC_CVTTSS2SI: r = w == 64 ? (u64)CVTTS2I64(v.f[0]) : (u32)CVTTS2I32(v.f[0]); break;
        case ZYDIS_MNEMONIC_CVTSS2SI: r = w == 64 ? (u64)CVTS2I64(v.f[0]) : (u32)CVTS2I32(v.f[0]); break;
        case ZYDIS_MNEMONIC_CVTTSD2SI: r = w == 64 ? (u64)CVTTD2I64(v.fd[0]) : (u32)CVTTD2I32(v.fd[0]); break;
        default: r = w == 64 ? (u64)CVTD2I64(v.fd[0]) : (u32)CVTD2I32(v.fd[0]); break;
        }
        greg_wr(s, IOP(s, 0)->reg.value, r);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_CVTDQ2PD: case ZYDIS_MNEMONIC_CVTPS2PD: {
        int dbits; vreg(s, 0, &dbits); int n = dbits / 64;
        V256 src = full_src(s, 1, is_mem(s, 1) ? n * 4 : 0), out;
        for (int i = 0; i < n; i++) out.fd[i] = m == ZYDIS_MNEMONIC_CVTDQ2PD ? (double)src.sd[i] : (double)src.f[i];
        if (n == 2) memset(&out.h[1], 0, 16);
        full_dst(s, 0, out);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_CVTPD2PS: case ZYDIS_MNEMONIC_CVTPD2DQ: case ZYDIS_MNEMONIC_CVTTPD2DQ: {
        int sbits = 128, b;
        if (vreg(s, 1, &b) >= 0) sbits = b; else if (is_mem(s, 1)) sbits = IOP(s, 1)->size;
        int n = sbits / 64;
        V256 src = full_src(s, 1, 0), out; memset(&out, 0, sizeof out);
        for (int i = 0; i < n; i++)
            out.d[i] = m == ZYDIS_MNEMONIC_CVTPD2PS ? ({ float f = (float)src.fd[i]; u32 u; memcpy(&u, &f, 4); u; })
                     : m == ZYDIS_MNEMONIC_CVTPD2DQ ? (u32)CVTD2I32(src.fd[i]) : (u32)CVTTD2I32(src.fd[i]);
        int r = vreg(s, 0, NULL);
        c->x[r] = out.h[0];
        if (vex) c->xh[r] = (V128){0};
        return X_NEXT; }

    case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_UCOMISS: { V128 a = lane_src(s, 0, 0), b = lane_src(s, 1, 0); comis(s, a.f[0], b.f[0]); return X_NEXT; }
    case ZYDIS_MNEMONIC_COMISD: case ZYDIS_MNEMONIC_UCOMISD: { V128 a = lane_src(s, 0, 0), b = lane_src(s, 1, 0); comis(s, a.fd[0], b.fd[0]); return X_NEXT; }
    case ZYDIS_MNEMONIC_PTEST: case ZYDIS_MNEMONIC_VTESTPS: case ZYDIS_MNEMONIC_VTESTPD: {
        int lanes = vwidth(s) == 256 ? 2 : 1; u64 andv = 0, andn = 0;
        u64 msk = m == ZYDIS_MNEMONIC_VTESTPS ? 0x8000000080000000ull : m == ZYDIS_MNEMONIC_VTESTPD ? 0x8000000000000000ull : ~0ull;
        for (int L = 0; L < lanes; L++) {
            V128 a = lane_src(s, 0, L), b = lane_src(s, 1, L);
            for (int i = 0; i < 2; i++) { andv |= a.q[i] & b.q[i] & msk; andn |= ~a.q[i] & b.q[i] & msk; }
        }
        s->zf = andv == 0; s->cf = andn == 0; s->of = s->sf = s->af = s->pf = 0;
        return X_NEXT; }

    case ZYDIS_MNEMONIC_PCMPISTRI: case ZYDIS_MNEMONIC_PCMPESTRI: case ZYDIS_MNEMONIC_PCMPISTRM: case ZYDIS_MNEMONIC_PCMPESTRM: {
        int expl = m == ZYDIS_MNEMONIC_PCMPESTRI || m == ZYDIS_MNEMONIC_PCMPESTRM;
        int idx = m == ZYDIS_MNEMONIC_PCMPISTRI || m == ZYDIS_MNEMONIC_PCMPESTRI, imm = last_imm(s);
        V128 a = lane_src(s, 0, 0), b = lane_src(s, 1, 0);
        i64 la = -1, lb = -1;
        if (expl) {
            int n = imm & 1 ? 8 : 16;
            i64 ra = (i64)c->r[RAX], rd = (i64)c->r[RDX];
            int w64 = 0;   // REX.W: lengths come from rax/rdx instead of eax/edx
            for (int i = 0; i < in->operand_count; i++) if (IOP(s, i)->type == ZYDIS_OPERAND_TYPE_REGISTER && IOP(s, i)->reg.value == ZYDIS_REGISTER_RAX) w64 = 1;
            if (!w64) { ra = (i32)ra; rd = (i32)rd; }
            la = ra < 0 ? -ra : ra; lb = rd < 0 ? -rd : rd;
            if (la > n) la = n;
            if (lb > n) lb = n;
        }
        u8 fl[4];
        V128 r = rt_pcmpstr(a, b, la, lb, imm, idx, fl);
        s->cf = fl[0]; s->zf = fl[1]; s->sf = fl[2]; s->of = fl[3]; s->af = s->pf = 0;
        if (idx) c->r[RCX] = r.d[0];
        else { c->x[0] = r; if (vex) c->xh[0] = (V128){0}; }
        return X_NEXT; }

    case ZYDIS_MNEMONIC_LDMXCSR: c->mxcsr = LD32(iea(s, IOP(s, 0))); return X_NEXT;
    case ZYDIS_MNEMONIC_STMXCSR: ST32(iea(s, IOP(s, 0)), c->mxcsr); return X_NEXT;

    // ---- AVX cross-lane ----
    case ZYDIS_MNEMONIC_VZEROUPPER: memset(c->xh, 0, sizeof c->xh); return X_NEXT;
    case ZYDIS_MNEMONIC_VZEROALL: memset(c->xh, 0, sizeof c->xh); memset(c->x, 0, sizeof c->x); return X_NEXT;
    case ZYDIS_MNEMONIC_VBROADCASTSS: case ZYDIS_MNEMONIC_VBROADCASTSD: case ZYDIS_MNEMONIC_VBROADCASTF128: case ZYDIS_MNEMONIC_VBROADCASTI128:
    case ZYDIS_MNEMONIC_VPBROADCASTB: case ZYDIS_MNEMONIC_VPBROADCASTW: case ZYDIS_MNEMONIC_VPBROADCASTD: case ZYDIS_MNEMONIC_VPBROADCASTQ: {
        int n = m == ZYDIS_MNEMONIC_VPBROADCASTB ? 1 : m == ZYDIS_MNEMONIC_VPBROADCASTW ? 2 :
                (m == ZYDIS_MNEMONIC_VBROADCASTSS || m == ZYDIS_MNEMONIC_VPBROADCASTD) ? 4 :
                (m == ZYDIS_MNEMONIC_VBROADCASTSD || m == ZYDIS_MNEMONIC_VPBROADCASTQ) ? 8 : 16;
        V256 src = full_src(s, 1, is_mem(s, 1) ? n : 0), out;
        int dbits; vreg(s, 0, &dbits);
        for (int i = 0; i < dbits / 8; i += n) memcpy(&out.b[i], &src.b[0], n);
        full_dst(s, 0, out);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_VINSERTF128: case ZYDIS_MNEMONIC_VINSERTI128: {
        int r = vreg(s, 0, NULL), sr = vreg(s, 1, NULL), imm = last_imm(s);
        V128 lo = c->x[sr], hi = c->xh[sr], v = lane_src(s, 2, 0);
        if (imm & 1) hi = v; else lo = v;
        c->x[r] = lo; c->xh[r] = hi;
        return X_NEXT; }
    case ZYDIS_MNEMONIC_VEXTRACTF128: case ZYDIS_MNEMONIC_VEXTRACTI128: {
        int sr = vreg(s, 1, NULL), imm = last_imm(s);
        V128 v = (imm & 1) ? c->xh[sr] : c->x[sr];
        if (is_mem(s, 0)) STX(iea(s, IOP(s, 0)), v);
        else { int r = vreg(s, 0, NULL); c->x[r] = v; c->xh[r] = (V128){0}; }
        return X_NEXT; }
    case ZYDIS_MNEMONIC_VPERM2F128: case ZYDIS_MNEMONIC_VPERM2I128: {
        int r = vreg(s, 0, NULL), imm = last_imm(s);
        V128 sel[4] = { lane_src(s, 1, 0), lane_src(s, 1, 1), lane_src(s, 2, 0), lane_src(s, 2, 1) };
        V128 lo = (imm & 0x08) ? (V128){0} : sel[imm & 3], hi = (imm & 0x80) ? (V128){0} : sel[(imm >> 4) & 3];
        c->x[r] = lo; c->xh[r] = hi;
        return X_NEXT; }
    case ZYDIS_MNEMONIC_VPERMQ: case ZYDIS_MNEMONIC_VPERMPD: {
        V256 src = full_src(s, 1, 0), out; int imm = last_imm(s);
        for (int i = 0; i < 4; i++) out.q[i] = src.q[imm >> (2 * i) & 3];
        full_dst(s, 0, out);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_VPERMD: case ZYDIS_MNEMONIC_VPERMPS: {
        V256 idx = full_src(s, 1, 0), src = full_src(s, 2, 0), out;
        for (int i = 0; i < 8; i++) out.d[i] = src.d[idx.d[i] & 7];
        full_dst(s, 0, out);
        return X_NEXT; }
    // F16C (PS5 code, docs/ps5.md): half <-> single; vcvtps2ph imm8[1:0] = RN/RD/RU/RZ, imm8[2] = MXCSR (nearest here)
    case ZYDIS_MNEMONIC_VCVTPH2PS: {
        int dbits; vreg(s, 0, &dbits); int n = dbits / 32;
        V256 src = full_src(s, 1, is_mem(s, 1) ? n * 2 : 0), out = { 0 };
        for (int i = 0; i < n; i++) { __fp16 h; memcpy(&h, &src.b[2 * i], 2); float f = (float)h; memcpy(&out.b[4 * i], &f, 4); }
        full_dst(s, 0, out);
        return X_NEXT; }
    case ZYDIS_MNEMONIC_VCVTPS2PH: {
        int sbits; vreg(s, 1, &sbits); int n = sbits / 32, imm = last_imm(s), rc = (imm & 4) ? 0 : imm & 3;
        V256 src = full_src(s, 1, 0); V128 out = { 0 };
        for (int i = 0; i < n; i++) {
            float x; memcpy(&x, &src.b[4 * i], 4);
            __fp16 h = (__fp16)x; u16 u; memcpy(&u, &h, 2); float b = (float)h;
            int neg = (u & 0x8000) != 0;
            if ((rc == 1 && b > x) || (rc == 2 && b < x)) {   // one step toward -inf / +inf
                int up = rc == 2;
                if ((u & 0x7fff) == 0) u = up ? 0x0001 : 0x8001;
                else u = (u16)(neg == up ? u - 1 : u + 1);
            } else if (rc == 3 && __builtin_fabsf(b) > __builtin_fabsf(x)) u = (u16)(u - 1);
            memcpy((u8 *)&out + 2 * i, &u, 2);
        }
        if (is_mem(s, 0)) { u64 a = iea(s, IOP(s, 0)); memcpy((void *)a, &out, n * 2); }
        else { int r = vreg(s, 0, NULL); c->x[r] = out; c->xh[r] = (V128){0}; }
        return X_NEXT; }
    default: return X_UNIMPL;
    }
}
