"""SSE / SSE2-4.2 / AVX / AVX2 / FMA3 / F16C / BMI1-2 lifting.

Register model: xmm/ymm n = two C locals, x{n} (bits 0-127) and h{n} (bits 128-255), both V128.

Encoding rules that matter:
  * legacy SSE writes to an xmm leave bits 128-255 untouched;
  * VEX.128 writes zero bits 128-255;
  * VEX.256 writes all 256 bits.

Most AVX/AVX2 ops act independently on each 128-bit lane, so they are written once as a *lane
kernel* `k(d, a, b, c, imm, L) -> C` and `vop()` runs the kernel per lane. `d` starts as a copy of `a`,
which yields both legacy "dst is also src1" semantics and VEX "upper elements come from src1"
semantics for scalar ops. Cross-lane ops (permutes, broadcasts, widening converts, gathers) use the
V256 path (`full_src` / `full_dst`).
"""
from __future__ import annotations

import iced_x86 as X

R = X.Register
K = X.OpKind
M = X.Mnemonic
IMM_KINDS = {K.IMMEDIATE8, K.IMMEDIATE8_2ND, K.IMMEDIATE16, K.IMMEDIATE32, K.IMMEDIATE64,
             K.IMMEDIATE8TO16, K.IMMEDIATE8TO32, K.IMMEDIATE8TO64, K.IMMEDIATE32TO64}
VEXLIKE = {X.EncodingKind.VEX, X.EncodingKind.EVEX, X.EncodingKind.XOP}

U = {8: "u8", 16: "u16", 32: "u32", 64: "u64"}
S = {8: "i8", 16: "i16", 32: "i32", 64: "i64"}
UF = {8: "b", 16: "w", 32: "d", 64: "q"}      # unsigned V128 field by element width
SF = {8: "sb", 16: "sw", 32: "sd", 64: "sq"}  # signed field


class Unsupported(Exception):
    pass


# ---- kernel builders ----------------------------------------------------------
def EW(field, n, expr):
    """Element-wise: d.F[i] = expr(a.F[i], b.F[i], c.F[i])."""
    def k(d, a, b, c, imm, L):
        e = expr.format(a=f"{a}.{field}[i_]", b=f"{b}.{field}[i_]", c=f"{c}.{field}[i_]" if c else "0", i="i_")
        return f"for (int i_ = 0; i_ < {n}; i_++) {d}.{field}[i_] = {e};"
    return k


def EW2(dfield, sfield, n, expr):
    """Element-wise with different dest/source views (e.g. compare results into an unsigned mask)."""
    def k(d, a, b, c, imm, L):
        e = expr.format(a=f"{a}.{sfield}[i_]", b=f"{b}.{sfield}[i_]", c=f"{c}.{sfield}[i_]" if c else "0", i="i_")
        return f"for (int i_ = 0; i_ < {n}; i_++) {d}.{dfield}[i_] = {e};"
    return k


def SC(field, expr):
    """Scalar: low element only; the rest come from `a` (dst for legacy, src1 for VEX)."""
    def k(d, a, b, c, imm, L):
        return f"{d}.{field}[0] = " + expr.format(a=f"{a}.{field}[0]", b=f"{b}.{field}[0]", c=f"{c}.{field}[0]" if c else "0") + ";"
    return k


def RAW(fmt):
    """Free-form kernel: fmt uses {d} {a} {b} {c} {imm} {L}."""
    return lambda d, a, b, c, imm, L: fmt.format(d=d, a=a, b=b, c=c, imm=imm, L=L)


def UNPCK(field, n, high):
    half = n // 2
    off = half if high else 0
    def k(d, a, b, c, imm, L):
        return (f"for (int i_ = 0; i_ < {half}; i_++) {{ {d}.{field}[2*i_] = {a}.{field}[i_+{off}]; "
                f"{d}.{field}[2*i_+1] = {b}.{field}[i_+{off}]; }}")
    return k


# Whole-register kernels on the V128 vector views (runtime/recomp.h): clang turns each into one NEON
# instruction, where the per-element loops above came out as scalar code. Same results element for element.
VT = {"b": "v16u8", "sb": "v16i8", "w": "v8u16", "sw": "v8i16", "d": "v4u32", "sd": "v4i32", "q": "v2u64", "sq": "v2i64",
      "f": "v4f32", "fd": "v2f64"}
VF = {"b": "vb", "sb": "vsb", "w": "vw", "sw": "vsw", "d": "vd32", "sd": "vsd", "q": "vq", "sq": "vsq", "f": "vf", "fd": "vd"}
NEL = {"b": 16, "sb": 16, "w": 8, "sw": 8, "d": 4, "sd": 4, "q": 2, "sq": 2, "f": 4, "fd": 2}


def VEC(field, expr):
    """d.V = expr over whole vectors {a} {b} (field's vector view)."""
    v = VF[field]
    return lambda d, a, b, c, imm, L: f"{d}.{v} = " + expr.format(a=f"{a}.{v}", b=f"{b}.{v}", c=f"{c}.{v}" if c else "0") + ";"


def VCMP(dfield, sfield, op):
    """Lane compare -> all-ones / zero mask (vector compares yield signed -1/0 lanes)."""
    dv, sv = VF[dfield], VF[sfield]
    return lambda d, a, b, c, imm, L: f"{d}.{dv} = ({VT[dfield]})({a}.{sv} {op} {b}.{sv});"


def VSEL(field, ifield, cond):
    """Bit-select: d = cond(a, b) ? a : b per lane (x86 min/max: second operand on NaN / equal)."""
    iv, fv, it = VF[ifield], VF[field], VT[ifield]
    def k(d, a, b, c, imm, L):
        m = cond.format(a=f"{a}.{fv}", b=f"{b}.{fv}")
        return f"{{ {it} m_ = ({it})({m}); {d}.{iv} = ({a}.{iv} & m_) | ({b}.{iv} & ~m_); }}"
    return k


def VUNPCK(field, high):
    n = NEL[field]; half = n // 2; off = half if high else 0
    idx = ", ".join(f"{i + off}, {n + i + off}" for i in range(half))
    v = VF[field]
    return lambda d, a, b, c, imm, L: f"{d}.{v} = __builtin_shufflevector({a}.{v}, {b}.{v}, {idx});"


def HOP(field, n, op):
    """Horizontal add/sub: pairs from a, then pairs from b."""
    half = n // 2
    def k(d, a, b, c, imm, L):
        return (f"for (int i_ = 0; i_ < {half}; i_++) {{ {d}.{field}[i_] = {a}.{field}[2*i_] {op} {a}.{field}[2*i_+1]; "
                f"{d}.{field}[i_+{half}] = {b}.{field}[2*i_] {op} {b}.{field}[2*i_+1]; }}")
    return k


CMP_PRED = ["{a} == {b}", "{a} < {b}", "{a} <= {b}", "({a} != {a} || {b} != {b})",
            "!({a} == {b})", "!({a} < {b})", "!({a} <= {b})", "({a} == {a} && {b} == {b})",
            "({a} == {b} || {a} != {a} || {b} != {b})", "!({a} >= {b})", "!({a} > {b})", "0",
            "({a} < {b} || {a} > {b})", "{a} >= {b}", "{a} > {b}", "1"]


# packed compare predicates as whole-vector expressions (mask lanes), for the predicates that have one
VCMP_PRED = {0: "({it})({a} == {b})", 1: "({it})({a} < {b})", 2: "({it})({a} <= {b})",
             3: "(({it})({a} != {a}) | ({it})({b} != {b}))", 4: "~({it})({a} == {b})", 5: "~({it})({a} < {b})",
             6: "~({it})({a} <= {b})", 7: "(({it})({a} == {a}) & ({it})({b} == {b}))",
             13: "({it})({a} >= {b})", 14: "({it})({a} > {b})"}


def CMPK(fl, n, scalar):
    ilane = "d" if fl == "f" else "q"
    ones = "0xFFFFFFFFu" if fl == "f" else "0xFFFFFFFFFFFFFFFFull"
    def k(d, a, b, c, imm, L):
        if not scalar and (imm & 15) in VCMP_PRED:
            it, iv, fv = ("v4u32", "vd32", "vf") if fl == "f" else ("v2u64", "vq", "vd")
            return f"{d}.{iv} = " + VCMP_PRED[imm & 15].format(it=it, a=f"{a}.{fv}", b=f"{b}.{fv}") + ";"
        pred = CMP_PRED[imm & 15].format(a=f"{a}.{fl}[i_]", b=f"{b}.{fl}[i_]")
        return f"for (int i_ = 0; i_ < {1 if scalar else n}; i_++) {d}.{ilane}[i_] = ({pred}) ? {ones} : 0;"
    return k


def FMA(fl, n, form, kind, scalar):
    """form: 132 (d=a*c+b), 213 (d=b*a+c), 231 (d=b*c+a). kind: madd/msub/nmadd/nmsub/maddsub/msubadd."""
    fn = "__builtin_fmaf" if fl == "f" else "__builtin_fma"
    def k(d, a, b, c, imm, L):
        A, B, C = (f"{a}.{fl}[i_]", f"{b}.{fl}[i_]", f"{c}.{fl}[i_]")
        x, y, z = {132: (A, C, B), 213: (B, A, C), 231: (B, C, A)}[form]
        if kind == "madd": e = f"{fn}({x}, {y}, {z})"
        elif kind == "msub": e = f"{fn}({x}, {y}, -{z})"
        elif kind == "nmadd": e = f"{fn}(-{x}, {y}, {z})"
        elif kind == "nmsub": e = f"{fn}(-{x}, {y}, -{z})"
        elif kind == "maddsub": e = f"((i_ & 1) ? {fn}({x}, {y}, {z}) : {fn}({x}, {y}, -{z}))"
        else: e = f"((i_ & 1) ? {fn}({x}, {y}, -{z}) : {fn}({x}, {y}, {z}))"
        return f"for (int i_ = 0; i_ < {1 if scalar else n}; i_++) {d}.{fl}[i_] = {e};"
    return k


# name -> (arity, kernel). arity 1: unary (src = last non-imm operand); 2: binary; 3: ternary (blendv / FMA)
LANE_OPS: dict[str, tuple[int, object]] = {}


def _def(names, arity, k):
    for n in names.split():
        LANE_OPS[n] = (arity, k)
        LANE_OPS["V" + n] = (arity, k)


for op, sym in (("ADD", "+"), ("SUB", "-"), ("MUL", "*"), ("DIV", "/")):
    _def(f"{op}PS", 2, VEC("f", "{a} " + sym + " {b}"))
    _def(f"{op}PD", 2, VEC("fd", "{a} " + sym + " {b}"))
    _def(f"{op}SS", 2, SC("f", "{a} " + sym + " {b}"))
    _def(f"{op}SD", 2, SC("fd", "{a} " + sym + " {b}"))
# x86 min/max: return the second operand if either is NaN or both are zero.
_def("MINPS", 2, VSEL("f", "d", "{a} < {b}")); _def("MAXPS", 2, VSEL("f", "d", "{a} > {b}"))
_def("MINPD", 2, VSEL("fd", "q", "{a} < {b}")); _def("MAXPD", 2, VSEL("fd", "q", "{a} > {b}"))
_def("MINSS", 2, SC("f", "{a} < {b} ? {a} : {b}")); _def("MAXSS", 2, SC("f", "{a} > {b} ? {a} : {b}"))
_def("MINSD", 2, SC("fd", "{a} < {b} ? {a} : {b}")); _def("MAXSD", 2, SC("fd", "{a} > {b} ? {a} : {b}"))
_def("SQRTPS", 1, VEC("f", "__builtin_elementwise_sqrt({b})")); _def("SQRTPD", 1, VEC("fd", "__builtin_elementwise_sqrt({b})"))
_def("SQRTSS", 2, SC("f", "__builtin_sqrtf({b})")); _def("SQRTSD", 2, SC("fd", "__builtin_sqrt({b})"))
_def("RCPPS", 1, VEC("f", "1.0f / {b}")); _def("RSQRTPS", 1, VEC("f", "1.0f / __builtin_elementwise_sqrt({b})"))
_def("RCPSS", 2, SC("f", "1.0f / {b}")); _def("RSQRTSS", 2, SC("f", "1.0f / __builtin_sqrtf({b})"))
_def("ADDSUBPS", 2, EW("f", 4, "(i_ & 1) ? {a} + {b} : {a} - {b}"))
_def("ADDSUBPD", 2, EW("fd", 2, "(i_ & 1) ? {a} + {b} : {a} - {b}"))
_def("HADDPS", 2, HOP("f", 4, "+")); _def("HSUBPS", 2, HOP("f", 4, "-"))
_def("HADDPD", 2, HOP("fd", 2, "+")); _def("HSUBPD", 2, HOP("fd", 2, "-"))
_def("PHADDW", 2, HOP("w", 8, "+")); _def("PHSUBW", 2, HOP("w", 8, "-"))
_def("PHADDD", 2, HOP("d", 4, "+")); _def("PHSUBD", 2, HOP("d", 4, "-"))
for nm, op in (("AND", "{a} & {b}"), ("OR", "{a} | {b}"), ("XOR", "{a} ^ {b}"), ("ANDN", "~{a} & {b}")):
    _def(f"{nm}PS {nm}PD P{nm}", 2, VEC("q", op))
_def("UNPCKLPS", 2, VUNPCK("d", False)); _def("UNPCKHPS", 2, VUNPCK("d", True))
_def("UNPCKLPD", 2, VUNPCK("q", False)); _def("UNPCKHPD", 2, VUNPCK("q", True))
_def("PUNPCKLBW", 2, VUNPCK("b", False)); _def("PUNPCKHBW", 2, VUNPCK("b", True))
_def("PUNPCKLWD", 2, VUNPCK("w", False)); _def("PUNPCKHWD", 2, VUNPCK("w", True))
_def("PUNPCKLDQ", 2, VUNPCK("d", False)); _def("PUNPCKHDQ", 2, VUNPCK("d", True))
_def("PUNPCKLQDQ", 2, VUNPCK("q", False)); _def("PUNPCKHQDQ", 2, VUNPCK("q", True))
_def("MOVHLPS", 2, RAW("{d}.vq = __builtin_shufflevector({a}.vq, {b}.vq, 3, 1);"))
_def("MOVLHPS", 2, RAW("{d}.vq = __builtin_shufflevector({a}.vq, {b}.vq, 0, 2);"))
_def("MOVSHDUP", 1, RAW("{d}.d[0] = {d}.d[1] = {b}.d[1]; {d}.d[2] = {d}.d[3] = {b}.d[3];"))
_def("MOVSLDUP", 1, RAW("{d}.d[0] = {d}.d[1] = {b}.d[0]; {d}.d[2] = {d}.d[3] = {b}.d[2];"))
_def("MOVDDUP", 1, RAW("{d}.q[0] = {d}.q[1] = {b}.q[0];"))

# integer arithmetic
for w, sfx in ((8, "B"), (16, "W"), (32, "D"), (64, "Q")):
    n = 128 // w; f = UF[w]; sf = SF[w]
    _def(f"PADD{sfx}", 2, VEC(f, "{a} + {b}"))
    _def(f"PSUB{sfx}", 2, VEC(f, "{a} - {b}"))
    _def(f"PCMPEQ{sfx}", 2, VCMP(f, f, "=="))
    _def(f"PCMPGT{sfx}", 2, VCMP(f, sf, ">"))
    if w <= 32:
        _def(f"PMINS{sfx}", 2, VEC(sf, "__builtin_elementwise_min({a}, {b})")); _def(f"PMAXS{sfx}", 2, VEC(sf, "__builtin_elementwise_max({a}, {b})"))
        _def(f"PMINU{sfx}", 2, VEC(f, "__builtin_elementwise_min({a}, {b})")); _def(f"PMAXU{sfx}", 2, VEC(f, "__builtin_elementwise_max({a}, {b})"))
        _def(f"PABS{sfx}", 1, EW2(f, sf, n, f"({U[w]})({{b}} < 0 ? -{{b}} : {{b}})"))
        _def(f"PSIGN{sfx}", 2, EW(sf, n, f"({S[w]})({{b}} < 0 ? -{{a}} : {{b}} == 0 ? 0 : {{a}})"))
for sfx, f in (("B", "sb"), ("W", "sw")):
    _def(f"PADDS{sfx}", 2, VEC(f, "__builtin_elementwise_add_sat({a}, {b})"))
    _def(f"PSUBS{sfx}", 2, VEC(f, "__builtin_elementwise_sub_sat({a}, {b})"))
for sfx, f in (("B", "b"), ("W", "w")):
    _def(f"PADDUS{sfx}", 2, VEC(f, "__builtin_elementwise_add_sat({a}, {b})"))
    _def(f"PSUBUS{sfx}", 2, VEC(f, "__builtin_elementwise_sub_sat({a}, {b})"))
_def("PMULLW", 2, VEC("w", "{a} * {b}"))
_def("PMULLD", 2, VEC("d", "{a} * {b}"))
_def("PMULHW", 2, EW2("w", "sw", 8, "(u16)(((i32){a} * {b}) >> 16)"))
_def("PMULHUW", 2, EW("w", 8, "(u16)(((u32){a} * {b}) >> 16)"))
_def("PMULHRSW", 2, EW2("w", "sw", 8, "(u16)(((((i32){a} * {b}) >> 14) + 1) >> 1)"))
_def("PMULUDQ", 2, RAW("{d}.vq = ({a}.vq & 0xFFFFFFFFull) * ({b}.vq & 0xFFFFFFFFull);"))
_def("PMULDQ", 2, RAW("{d}.sq[0] = (i64){a}.sd[0] * {b}.sd[0]; {d}.sq[1] = (i64){a}.sd[2] * {b}.sd[2];"))
_def("PMADDWD", 2, RAW("for (int i_ = 0; i_ < 4; i_++) {d}.sd[i_] = (i32)((u32)((i32){a}.sw[2*i_] * {b}.sw[2*i_]) + (u32)((i32){a}.sw[2*i_+1] * {b}.sw[2*i_+1]));"))
_def("PMADDUBSW", 2, RAW("for (int i_ = 0; i_ < 8; i_++) {d}.sw[i_] = SAT_I16((i32){a}.b[2*i_] * {b}.sb[2*i_] + (i32){a}.b[2*i_+1] * {b}.sb[2*i_+1]);"))
_def("PAVGB", 2, EW("b", 16, "(u8)(((u32){a} + {b} + 1) >> 1)"))
_def("PAVGW", 2, EW("w", 8, "(u16)(((u32){a} + {b} + 1) >> 1)"))
_def("PSADBW", 2, RAW("for (int j_ = 0; j_ < 2; j_++) {{ u32 s_ = 0; for (int i_ = 0; i_ < 8; i_++) {{ int t_ = (int){a}.b[8*j_+i_] - {b}.b[8*j_+i_]; s_ += t_ < 0 ? -t_ : t_; }} {d}.q[j_] = s_; }}"))
_def("PSHUFB", 2, RAW("for (int i_ = 0; i_ < 16; i_++) {d}.b[i_] = ({b}.b[i_] & 0x80) ? 0 : {a}.b[{b}.b[i_] & 15];"))
_def("PACKSSWB", 2, RAW("for (int i_ = 0; i_ < 8; i_++) {{ {d}.sb[i_] = SAT_I8({a}.sw[i_]); {d}.sb[8+i_] = SAT_I8({b}.sw[i_]); }}"))
_def("PACKUSWB", 2, RAW("for (int i_ = 0; i_ < 8; i_++) {{ {d}.b[i_] = SAT_U8({a}.sw[i_]); {d}.b[8+i_] = SAT_U8({b}.sw[i_]); }}"))
_def("PACKSSDW", 2, RAW("for (int i_ = 0; i_ < 4; i_++) {{ {d}.sw[i_] = SAT_I16({a}.sd[i_]); {d}.sw[4+i_] = SAT_I16({b}.sd[i_]); }}"))
_def("PACKUSDW", 2, RAW("for (int i_ = 0; i_ < 4; i_++) {{ {d}.w[i_] = SAT_U16({a}.sd[i_]); {d}.w[4+i_] = SAT_U16({b}.sd[i_]); }}"))
# variable shifts (AVX2)
_def("PSLLVD", 2, EW("d", 4, "{b} > 31 ? 0 : {a} << {b}")); _def("PSLLVQ", 2, EW("q", 2, "{b} > 63 ? 0 : {a} << {b}"))
_def("PSRLVD", 2, EW("d", 4, "{b} > 31 ? 0 : {a} >> {b}")); _def("PSRLVQ", 2, EW("q", 2, "{b} > 63 ? 0 : {a} >> {b}"))
_def("PSRAVD", 2, EW2("d", "sd", 4, "(u32)({a} >> ({b} > 31 ? 31 : {b}))"))
# immediate-controlled
_def("SHUFPS", 2, lambda d, a, b, c, imm, L: f"{d}.vd32 = __builtin_shufflevector({a}.vd32, {b}.vd32, {imm & 3}, {imm >> 2 & 3}, {4 + (imm >> 4 & 3)}, {4 + (imm >> 6 & 3)});")
_def("SHUFPD", 2, lambda d, a, b, c, imm, L: f"{d}.vq = __builtin_shufflevector({a}.vq, {b}.vq, {imm >> (2 * L) & 1}, {2 + (imm >> (2 * L + 1) & 1)});")
_def("PSHUFD", 1, lambda d, a, b, c, imm, L: f"{d}.vd32 = __builtin_shufflevector({b}.vd32, {b}.vd32, " + ", ".join(str(imm >> (2 * i) & 3) for i in range(4)) + ");")
_def("PSHUFLW", 1, lambda d, a, b, c, imm, L: f"{d} = {b}; " + "".join(f"{d}.w[{i}] = {b}.w[{imm >> (2 * i) & 3}]; " for i in range(4)))
_def("PSHUFHW", 1, lambda d, a, b, c, imm, L: f"{d} = {b}; " + "".join(f"{d}.w[{4 + i}] = {b}.w[{4 + (imm >> (2 * i) & 3)}]; " for i in range(4)))
_def("BLENDPS", 2, lambda d, a, b, c, imm, L: f"{d}.vd32 = __builtin_shufflevector({a}.vd32, {b}.vd32, " + ", ".join(str(4 + i if imm >> (4 * L + i) & 1 else i) for i in range(4)) + ");")
_def("BLENDPD", 2, lambda d, a, b, c, imm, L: "".join(f"{d}.q[{i}] = {b}.q[{i}]; " for i in range(2) if imm >> (2 * L + i) & 1))
_def("PBLENDW", 2, lambda d, a, b, c, imm, L: "".join(f"{d}.w[{i}] = {b}.w[{i}]; " for i in range(8) if imm >> i & 1))
_def("PBLENDD", 2, lambda d, a, b, c, imm, L: "".join(f"{d}.d[{i}] = {b}.d[{i}]; " for i in range(4) if imm >> (4 * L + i) & 1))
_def("BLENDVPS", 3, RAW("{{ v4u32 m_ = (v4u32)({c}.vsd < 0); {d}.vd32 = ({b}.vd32 & m_) | ({a}.vd32 & ~m_); }}"))
_def("BLENDVPD", 3, RAW("{{ v2u64 m_ = (v2u64)({c}.vsq < 0); {d}.vq = ({b}.vq & m_) | ({a}.vq & ~m_); }}"))
_def("PHMINPOSUW", 1, RAW("{{ u16 m_ = {b}.w[0], k_ = 0; for (int i_ = 1; i_ < 8; i_++) if ({b}.w[i_] < m_) {{ m_ = {b}.w[i_]; k_ = (u16)i_; }} {d} = (V128){{0}}; {d}.w[0] = m_; {d}.w[1] = k_; }}"))
_def("PBLENDVB", 3, RAW("{{ v16u8 m_ = (v16u8)({c}.vsb < 0); {d}.vb = ({b}.vb & m_) | ({a}.vb & ~m_); }}"))
_def("PALIGNR", 2, lambda d, a, b, c, imm, L: f"{{ u8 t_[32]; memcpy(t_, &{b}, 16); memcpy(t_ + 16, &{a}, 16); for (int i_ = 0; i_ < 16; i_++) {d}.b[i_] = i_ + {imm} < 32 ? t_[i_ + {imm}] : 0; }}")
_def("PSLLDQ", 1, lambda d, a, b, c, imm, L: f"{d} = (V128){{0}}; " + (f"memcpy(&{d}.b[{imm}], &{b}.b[0], {16 - imm});" if imm < 16 else ""))
_def("PSRLDQ", 1, lambda d, a, b, c, imm, L: f"{d} = (V128){{0}}; " + (f"memcpy(&{d}.b[0], &{b}.b[{imm}], {16 - imm});" if imm < 16 else ""))
_def("ROUNDPS", 1, lambda d, a, b, c, imm, L: f"for (int i_ = 0; i_ < 4; i_++) {d}.f[i_] = RNDF({b}.f[i_], {imm & 3 if not imm & 4 else 0});")
_def("ROUNDPD", 1, lambda d, a, b, c, imm, L: f"for (int i_ = 0; i_ < 2; i_++) {d}.fd[i_] = RNDD({b}.fd[i_], {imm & 3 if not imm & 4 else 0});")
_def("ROUNDSS", 2, lambda d, a, b, c, imm, L: f"{d}.f[0] = RNDF({b}.f[0], {imm & 3 if not imm & 4 else 0});")
_def("ROUNDSD", 2, lambda d, a, b, c, imm, L: f"{d}.fd[0] = RNDD({b}.fd[0], {imm & 3 if not imm & 4 else 0});")
def _dpps(d, a, b, c, imm, L):
    # Intel order: (p0 + p1) + (p2 + p3), masked-out products are +0.0
    p = [f"({a}.f[{i}] * {b}.f[{i}])" if imm >> (4 + i) & 1 else "0.0f" for i in range(4)]
    return (f"{{ float s_ = ({p[0]} + {p[1]}) + ({p[2]} + {p[3]}); "
            + "".join(f"{d}.f[{i}] = {'s_' if imm >> i & 1 else '0.0f'}; " for i in range(4)) + "}")


def _dppd(d, a, b, c, imm, L):
    p = [f"({a}.fd[{i}] * {b}.fd[{i}])" if imm >> (4 + i) & 1 else "0.0" for i in range(2)]
    return (f"{{ double s_ = {p[0]} + {p[1]}; "
            + "".join(f"{d}.fd[{i}] = {'s_' if imm >> i & 1 else '0.0'}; " for i in range(2)) + "}")


_def("DPPS", 2, _dpps)
_def("DPPD", 2, _dppd)
_def("CMPPS", 2, CMPK("f", 4, False)); _def("CMPPD", 2, CMPK("fd", 2, False))
_def("CMPSS", 2, CMPK("f", 4, True)); _def("CMPSD", 2, CMPK("fd", 2, True))
_def("CVTDQ2PS", 1, lambda d, a, b, c, imm, L: f"{d}.vf = __builtin_convertvector({b}.vsd, v4f32);")
_def("CVTPS2DQ", 1, EW2("d", "f", 4, "(u32)CVTS2I32({b})"))
_def("CVTTPS2DQ", 1, EW2("d", "f", 4, "(u32)CVTTS2I32({b})"))
_def("CVTSS2SD", 2, lambda d, a, b, c, imm, L: f"{d}.fd[0] = (double){b}.f[0];")
_def("CVTSD2SS", 2, lambda d, a, b, c, imm, L: f"{d}.f[0] = (float){b}.fd[0];")
_def("VPERMILPS_IMM", 1, lambda d, a, b, c, imm, L: "".join(f"{d}.d[{i}] = {b}.d[{imm >> (2 * i) & 3}]; " for i in range(4)))
_def("VPERMILPD_IMM", 1, lambda d, a, b, c, imm, L: "".join(f"{d}.q[{i}] = {b}.q[{imm >> (2 * L + i) & 1}]; " for i in range(2)))
LANE_OPS["VPERMILPS"] = (2, RAW("for (int i_ = 0; i_ < 4; i_++) {d}.d[i_] = {a}.d[{b}.d[i_] & 3];"))
LANE_OPS["VPERMILPD"] = (2, RAW("for (int i_ = 0; i_ < 2; i_++) {d}.q[i_] = {a}.q[({b}.q[i_] >> 1) & 1];"))

for kind, nm in (("madd", "FMADD"), ("msub", "FMSUB"), ("nmadd", "FNMADD"), ("nmsub", "FNMSUB"),
                 ("maddsub", "FMADDSUB"), ("msubadd", "FMSUBADD")):
    for form in (132, 213, 231):
        LANE_OPS[f"V{nm}{form}PS"] = (3, FMA("f", 4, form, kind, False))
        LANE_OPS[f"V{nm}{form}PD"] = (3, FMA("fd", 2, form, kind, False))
        if kind in ("madd", "msub", "nmadd", "nmsub"):
            LANE_OPS[f"V{nm}{form}SS"] = (3, FMA("f", 4, form, kind, True))
            LANE_OPS[f"V{nm}{form}SD"] = (3, FMA("fd", 2, form, kind, True))


ZERO_IDIOMS = {getattr(M, n) for n in (
    "XORPS XORPD PXOR VXORPS VXORPD VPXOR ANDNPS ANDNPD PANDN VANDNPS VANDNPD VPANDN "
    "PSUBB PSUBW PSUBD PSUBQ VPSUBB VPSUBW VPSUBD VPSUBQ").split()}


class VectorMixin:
    """Mixed into FuncLifter. Relies on: self.t(), self.emit(), self.g(), self.addr(), self.rd(), self.wr(),
    self.used_x, self.used_h, REG (via self.REG)."""

    # ---- register / operand plumbing ----------------------------------------
    def x(self, i):
        self.used_x.add(i); self.refs |= 1 << (16 + i); return f"x{i}"

    def hh(self, i):
        self.used_h.add(i); self.refs |= 1 << (32 + i); return f"h{i}"

    def vreg(self, ins, i):
        """(index, bits) of an xmm/ymm register operand, else None."""
        if ins.op_kind(i) != K.REGISTER:
            return None
        info = self.REG.get(ins.op_register(i))
        if not info or info[0] not in ("x", "y"):
            return None
        return info[1], info[2]

    def is_vex(self, ins):
        return ins.encoding in VEXLIKE

    def msize(self, ins):
        return X.MemorySizeExt.size(ins.memory_size)

    def vwidth(self, ins):
        for i in range(ins.op_count):
            r = self.vreg(ins, i)
            if r and r[1] == 256:
                return 256
        if any(ins.op_kind(i) == K.MEMORY for i in range(ins.op_count)) and self.msize(ins) == 32:
            return 256
        return 128

    def lane_src(self, ins, i, lane):
        k = ins.op_kind(i)
        if k == K.REGISTER:
            r = self.vreg(ins, i)
            if not r:
                raise Unsupported("non-vector register in vector op")
            return self.x(r[0]) if lane == 0 else self.hh(r[0])
        if k == K.MEMORY:
            n = self.msize(ins)
            a = self.addr(ins)
            if n >= 16:
                return f"LDX({a}+{16 * lane})"
            if lane:
                return "(V128){0}"
            return {1: f"LDV8({a})", 2: f"LDV16({a})", 4: f"LDV32({a})", 8: f"LDV64({a})"}[n]
        raise Unsupported("immediate as vector source")

    def zero_upper(self, ins, n):
        if self.is_vex(ins):
            self.emit(f"{self.hh(n)} = (V128){{0}};")

    def last_imm(self, ins):
        if ins.op_count and ins.op_kind(ins.op_count - 1) in IMM_KINDS:
            return ins.immediate(ins.op_count - 1) & 0xFF
        return 0

    # ---- generic per-lane engine -----------------------------------------------
    def vop(self, ins, arity, kern):
        vex = self.is_vex(ins)
        nops = ins.op_count - (1 if ins.op_count and ins.op_kind(ins.op_count - 1) in IMM_KINDS else 0)
        if arity == 1:
            D, A, B, C = 0, 0, (1 if nops > 1 else 0), None
        elif arity == 2:
            D, A, B, C = (0, 1, 2, None) if vex else (0, 0, 1, None)
        else:
            if ins.mnemonic in FMA_MNEMONICS:
                D, A, B, C = 0, 0, 1, 2
            else:  # blendv: legacy has implicit xmm0 as op2; VEX has explicit op3
                D, A, B, C = (0, 1, 2, 3) if vex else (0, 0, 1, 2)
        dreg = self.vreg(ins, D)
        if not dreg:
            raise Unsupported("vector op with memory destination")
        lanes = 2 if self.vwidth(ins) == 256 else 1
        # Zeroing idioms (xorps x,x / pxor / psub* / andnps with both sources the same register) as a plain zero
        # store: a whole-vector xor would keep clang from splitting the register into scalar lanes, which costs
        # scalar code that later uses the register (an `addsd` accumulator carried its upper lane around a loop).
        if arity == 2 and ins.mnemonic in ZERO_IDIOMS and ins.op_kind(A) == K.REGISTER and ins.op_kind(B) == K.REGISTER \
                and ins.op_register(A) == ins.op_register(B):
            self.emit(f"{self.x(dreg[0])} = (V128){{0}};")
            if lanes == 2:
                self.emit(f"{self.hh(dreg[0])} = (V128){{0}};")
            else:
                self.zero_upper(ins, dreg[0])
            return
        imm = self.last_imm(ins)
        outs = []
        for L in range(lanes):
            a, b, d = self.t(), self.t(), self.t()
            c = self.t() if C is not None else None
            stm = f"V128 {a} = {self.lane_src(ins, A, L)}; V128 {b} = {self.lane_src(ins, B, L)};"
            if c:
                # legacy blendv: iced may leave the implicit xmm0 out of the operand list
                src = self.lane_src(ins, C, L) if C < ins.op_count else (self.x(0) if L == 0 else self.hh(0))
                stm += f" V128 {c} = {src};"
            stm += f" V128 {d} = {a};"
            self.emit(stm)
            self.emit(kern(d, a, b, c, imm, L))
            outs.append(d)
        self.emit(f"{self.x(dreg[0])} = {outs[0]};")
        if lanes == 2:
            self.emit(f"{self.hh(dreg[0])} = {outs[1]};")
        else:
            self.zero_upper(ins, dreg[0])

    # ---- full-width (V256) plumbing ------------------------------------------
    def full_src(self, ins, i, nbytes=None):
        t = self.t()
        k = ins.op_kind(i)
        if k == K.REGISTER:
            r = self.vreg(ins, i)
            if r:
                hi = self.hh(r[0]) if r[1] == 256 else "(V128){0}"
                self.emit(f"V256 {t}; {t}.h[0] = {self.x(r[0])}; {t}.h[1] = {hi};")
            else:  # GPR source (vpbroadcastd ymm, eax style is EVEX-only; keep for movd-like uses)
                self.emit(f"V256 {t} = {{0}}; {t}.q[0] = {self.rd(ins, i, 64)};")
            return t
        if k == K.MEMORY:
            n = nbytes or self.msize(ins)
            self.emit(f"V256 {t} = {{0}}; memcpy(&{t}, (void*){self.addr(ins)}, {n});")
            return t
        raise Unsupported("full_src imm")

    def full_dst(self, ins, i, t, width):
        """Write V256 temp t to operand i. width 128/256 = how much of t is meaningful."""
        k = ins.op_kind(i)
        if k == K.MEMORY:
            self.emit(f"memcpy((void*){self.addr(ins)}, &{t}, {self.msize(ins)});")
            return
        r = self.vreg(ins, i)
        self.emit(f"{self.x(r[0])} = {t}.h[0];")
        if r[1] == 256:
            self.emit(f"{self.hh(r[0])} = {t}.h[1];")
        else:
            self.zero_upper(ins, r[0])

    # ---- dispatch hook used by FuncLifter.lift_insn --------------------------------
    def try_vector(self, ins, name) -> bool:
        for k in range(ins.op_count):   # MMX (mm0-7) has no state in Ctx: report it, don't crash on the lookup
            if ins.op_kind(k) == X.OpKind.REGISTER and X.Register.MM0 <= ins.op_register(k) <= X.Register.MM7:
                raise Unsupported("MMX")
        if name in ("VPERMILPS", "VPERMILPD") and ins.op_kind(ins.op_count - 1) in IMM_KINDS:
            name = name + "_IMM"
        if name in LANE_OPS:
            arity, kern = LANE_OPS[name]
            if name.endswith("_IMM"):
                arity = 1
            self.vop(ins, arity, kern)
            return True
        h = getattr(self, "v_" + name, None) or getattr(self, "v_" + name[1:] if name.startswith("V") else "", None)
        if h is None:
            return False
        h(ins)
        return True

    # ---- moves -----------------------------------------------------------
    def _vmov_full(self, ins):
        if ins.op0_kind == K.MEMORY:
            r = self.vreg(ins, 1)
            self.emit(f"STX({self.addr(ins)}, {self.x(r[0])});")
            if r[1] == 256:
                self.emit(f"STX({self.addr(ins)}+16, {self.hh(r[0])});")
            return
        d = self.vreg(ins, 0)
        src0 = self.lane_src(ins, 1, 0)
        if d[1] == 256:
            t0, t1 = self.t(), self.t()
            self.emit(f"V128 {t0} = {src0}; V128 {t1} = {self.lane_src(ins, 1, 1)}; {self.x(d[0])} = {t0}; {self.hh(d[0])} = {t1};")
        else:
            self.emit(f"{self.x(d[0])} = {src0};")
            self.zero_upper(ins, d[0])

    v_MOVAPS = v_MOVUPS = v_MOVAPD = v_MOVUPD = v_MOVDQA = v_MOVDQU = v_LDDQU = _vmov_full
    v_MOVNTDQ = v_MOVNTPS = v_MOVNTPD = v_MOVNTDQA = _vmov_full

    def _vmov_scalar(self, ins, bits):
        f = "d" if bits == 32 else "q"
        if ins.op0_kind == K.MEMORY:
            self.emit(f"ST{bits}({self.addr(ins)}, {self.x(self.vreg(ins, 1)[0])}.{f}[0]);")
            return
        d = self.vreg(ins, 0)[0]
        if ins.op_count == 2 and ins.op1_kind == K.MEMORY:          # load: zero the rest
            self.emit(f"{self.x(d)} = LDV{bits}({self.addr(ins)});")
            self.zero_upper(ins, d)
            return
        if ins.op_count == 2:                                        # legacy reg,reg: merge low element
            self.emit(f"{self.x(d)}.{f}[0] = {self.x(self.vreg(ins, 1)[0])}.{f}[0];")
            return
        a, b = self.vreg(ins, 1)[0], self.vreg(ins, 2)[0]            # VEX: d = src1 with low from src2
        t = self.t()
        self.emit(f"V128 {t} = {self.x(a)}; {t}.{f}[0] = {self.x(b)}.{f}[0]; {self.x(d)} = {t};")
        self.zero_upper(ins, d)

    def v_MOVSS(self, ins): self._vmov_scalar(ins, 32)
    def v_MOVSD(self, ins): self._vmov_scalar(ins, 64)

    def _vmovdq(self, ins, bits):
        d = self.vreg(ins, 0)
        if d:  # xmm <- gpr / mem / xmm
            s = self.vreg(ins, 1)
            src = f"{self.x(s[0])}.q[0]" if s else self.rd(ins, 1, bits)
            t = self.t()
            self.emit(f"u64 {t} = (u64)({U[bits]})({src}); {self.x(d[0])} = (V128){{0}}; {self.x(d[0])}.q[0] = {t};")
            self.zero_upper(ins, d[0])
        else:  # gpr / mem <- xmm
            s = self.vreg(ins, 1)
            self.emit(self.wr(ins, 0, f"({U[bits]}){self.x(s[0])}.q[0]", bits))

    def v_MOVD(self, ins): self._vmovdq(ins, 32)
    def v_MOVQ(self, ins): self._vmovdq(ins, 64)

    def _vmov_half(self, ins, hi):
        q = 1 if hi else 0
        if ins.op0_kind == K.MEMORY:
            self.emit(f"ST64({self.addr(ins)}, {self.x(self.vreg(ins, 1)[0])}.q[{q}]);")
            return
        self.vop(ins, 2, RAW(f"{{d}}.q[{q}] = {{b}}.q[0];"))

    def v_MOVHPS(self, ins): self._vmov_half(ins, True)
    def v_MOVHPD(self, ins): self._vmov_half(ins, True)
    def v_MOVLPS(self, ins): self._vmov_half(ins, False)
    def v_MOVLPD(self, ins): self._vmov_half(ins, False)

    def v_MOVMSKPS(self, ins):
        s = self.full_src(ins, 1); n = 8 if self.vwidth(ins) == 256 else 4; r = self.t()
        self.emit(f"u32 {r} = 0; for (int i_ = 0; i_ < {n}; i_++) {r} |= ({s}.d[i_] >> 31) << i_;")
        self.emit(self.wr(ins, 0, r, 32 if self.width(ins, 0) == 32 else 64))

    def v_MOVMSKPD(self, ins):
        s = self.full_src(ins, 1); n = 4 if self.vwidth(ins) == 256 else 2; r = self.t()
        self.emit(f"u32 {r} = 0; for (int i_ = 0; i_ < {n}; i_++) {r} |= (u32)({s}.q[i_] >> 63) << i_;")
        self.emit(self.wr(ins, 0, r, 32 if self.width(ins, 0) == 32 else 64))

    def v_PMOVMSKB(self, ins):
        s = self.full_src(ins, 1); n = 32 if self.vwidth(ins) == 256 else 16; r = self.t()
        self.emit(f"u32 {r} = 0; for (int i_ = 0; i_ < {n}; i_++) {r} |= (u32)({s}.b[i_] >> 7) << i_;")
        self.emit(self.wr(ins, 0, r, 32 if self.width(ins, 0) == 32 else 64))

    # ---- shifts by count -----------------------------------------------------------
    def _vshift(self, ins, field, bits, op):
        vex = self.is_vex(ins)
        last = ins.op_count - 1
        if ins.op_kind(last) in IMM_KINDS:
            cnt = f"{ins.immediate(last) & 0xFF}u"
        else:
            cnt = f"({self.lane_src(ins, last, 0)}).q[0]"
        src = 1 if vex else 0
        n = self.t()
        self.emit(f"u64 {n} = {cnt};")
        sh = {"sll": "<<", "srl": ">>"}.get(op)
        # whole-vector shifts (one NEON instruction); counts >= the lane width give 0 (or all sign bits)
        uv, sv = {"w": ("vw", "vsw"), "d": ("vd32", "vsd"), "q": ("vq", "vsq")}[field]
        if op == "sra":
            k = lambda d, a, b, c, imm, L: f"{d}.{sv} = {b}.{sv} >> (int)({n} > {bits - 1} ? {bits - 1} : {n});"
        else:
            k = lambda d, a, b, c, imm, L: f"if ({n} > {bits - 1}) {d} = (V128){{0}}; else {d}.{uv} = {b}.{uv} {sh} (int){n};"
        # run as unary over the data operand
        dreg = self.vreg(ins, 0)
        lanes = 2 if self.vwidth(ins) == 256 else 1
        outs = []
        for L in range(lanes):
            b, d = self.t(), self.t()
            self.emit(f"V128 {b} = {self.lane_src(ins, src, L)}; V128 {d};")
            self.emit(k(d, None, b, None, 0, L))
            outs.append(d)
        self.emit(f"{self.x(dreg[0])} = {outs[0]};")
        if lanes == 2:
            self.emit(f"{self.hh(dreg[0])} = {outs[1]};")
        else:
            self.zero_upper(ins, dreg[0])

    def v_PSLLW(self, ins): self._vshift(ins, "w", 16, "sll")
    def v_PSLLD(self, ins): self._vshift(ins, "d", 32, "sll")
    def v_PSLLQ(self, ins): self._vshift(ins, "q", 64, "sll")
    def v_PSRLW(self, ins): self._vshift(ins, "w", 16, "srl")
    def v_PSRLD(self, ins): self._vshift(ins, "d", 32, "srl")
    def v_PSRLQ(self, ins): self._vshift(ins, "q", 64, "srl")
    def v_PSRAW(self, ins): self._vshift(ins, "w", 16, "sra")
    def v_PSRAD(self, ins): self._vshift(ins, "d", 32, "sra")

    # ---- insert / extract -----------------------------------------------------------
    def _pinsr(self, ins, bits):
        vex = self.is_vex(ins)
        d = self.vreg(ins, 0)[0]
        A, S_ = (1, 2) if vex else (0, 1)
        imm = self.last_imm(ins) & (128 // bits - 1)
        v = self.rd(ins, S_, bits if ins.op_kind(S_) == K.MEMORY else max(bits, 32))
        t = self.t()
        self.emit(f"V128 {t} = {self.lane_src(ins, A, 0)}; {t}.{UF[bits]}[{imm}] = ({U[bits]})({v}); {self.x(d)} = {t};")
        self.zero_upper(ins, d)

    def v_PINSRB(self, ins): self._pinsr(ins, 8)
    def v_PINSRW(self, ins): self._pinsr(ins, 16)
    def v_PINSRD(self, ins): self._pinsr(ins, 32)
    def v_PINSRQ(self, ins): self._pinsr(ins, 64)

    def _pextr(self, ins, bits):
        s = self.vreg(ins, 1)[0]
        imm = self.last_imm(ins) & (128 // bits - 1)
        w = bits if ins.op0_kind == K.MEMORY else max(32, self.width(ins, 0))
        self.emit(self.wr(ins, 0, f"(u64){self.x(s)}.{UF[bits]}[{imm}]", w))

    def v_PEXTRB(self, ins): self._pextr(ins, 8)
    def v_PEXTRW(self, ins): self._pextr(ins, 16)
    def v_PEXTRD(self, ins): self._pextr(ins, 32)
    def v_PEXTRQ(self, ins): self._pextr(ins, 64)
    def v_EXTRACTPS(self, ins): self._pextr(ins, 32)

    def v_INSERTPS(self, ins):
        vex = self.is_vex(ins)
        d = self.vreg(ins, 0)[0]
        A, S_ = (1, 2) if vex else (0, 1)
        imm = self.last_imm(ins)
        src = f"LD32({self.addr(ins)})" if ins.op_kind(S_) == K.MEMORY else f"{self.x(self.vreg(ins, S_)[0])}.d[{imm >> 6}]"
        t = self.t()
        self.emit(f"V128 {t} = {self.lane_src(ins, A, 0)}; {t}.d[{(imm >> 4) & 3}] = {src};" +
                  "".join(f" {t}.d[{i}] = 0;" for i in range(4) if imm >> i & 1) + f" {self.x(d)} = {t};")
        self.zero_upper(ins, d)

    # ---- cross-lane AVX/AVX2 -----------------------------------------------------
    def _bcast(self, ins, ebytes):
        s = self.full_src(ins, 1, nbytes=min(ebytes, 16) if ins.op1_kind == K.MEMORY else None)
        width = self.vwidth(ins) if self.vreg(ins, 0)[1] == 256 else 128
        n = (32 if self.vreg(ins, 0)[1] == 256 else 16) // ebytes
        t = self.t()
        self.emit(f"V256 {t} = {{0}}; for (int i_ = 0; i_ < {n}; i_++) memcpy(&{t}.b[i_ * {ebytes}], &{s}.b[0], {ebytes});")
        self.full_dst(ins, 0, t, width)

    def v_VBROADCASTSS(self, ins): self._bcast(ins, 4)
    def v_VBROADCASTSD(self, ins): self._bcast(ins, 8)
    def v_VBROADCASTF128(self, ins): self._bcast(ins, 16)
    def v_VBROADCASTI128(self, ins): self._bcast(ins, 16)
    def v_VPBROADCASTB(self, ins): self._bcast(ins, 1)
    def v_VPBROADCASTW(self, ins): self._bcast(ins, 2)
    def v_VPBROADCASTD(self, ins): self._bcast(ins, 4)
    def v_VPBROADCASTQ(self, ins): self._bcast(ins, 8)

    def v_VINSERTF128(self, ins):
        a = self.full_src(ins, 1); imm = self.last_imm(ins) & 1
        self.emit(f"{a}.h[{imm}] = {self.lane_src(ins, 2, 0)};")
        self.full_dst(ins, 0, a, 256)
    v_VINSERTI128 = v_VINSERTF128

    def v_VEXTRACTF128(self, ins):
        a = self.full_src(ins, 1); imm = self.last_imm(ins) & 1; t = self.t()
        self.emit(f"V256 {t} = {{0}}; {t}.h[0] = {a}.h[{imm}];")
        if ins.op0_kind == K.MEMORY:
            self.emit(f"STX({self.addr(ins)}, {t}.h[0]);")
        else:
            self.full_dst(ins, 0, t, 128)
    v_VEXTRACTI128 = v_VEXTRACTF128

    def v_VPERM2F128(self, ins):
        a, b = self.full_src(ins, 1), self.full_src(ins, 2); imm = self.last_imm(ins); t = self.t()
        parts = []
        for j in range(2):
            sel = imm >> (4 * j) & 3
            src = [f"{a}.h[0]", f"{a}.h[1]", f"{b}.h[0]", f"{b}.h[1]"][sel]
            parts.append(f"{t}.h[{j}] = {'(V128){0}' if imm >> (4 * j + 3) & 1 else src};")
        self.emit(f"V256 {t}; " + " ".join(parts))
        self.full_dst(ins, 0, t, 256)
    v_VPERM2I128 = v_VPERM2F128

    def v_VPERMQ(self, ins):
        s = self.full_src(ins, 1); imm = self.last_imm(ins); t = self.t()
        self.emit(f"V256 {t}; " + " ".join(f"{t}.q[{i}] = {s}.q[{imm >> (2 * i) & 3}];" for i in range(4)))
        self.full_dst(ins, 0, t, 256)
    v_VPERMPD = v_VPERMQ

    def v_VPERMD(self, ins):
        idx, tab = self.full_src(ins, 1), self.full_src(ins, 2); t = self.t()
        self.emit(f"V256 {t}; for (int i_ = 0; i_ < 8; i_++) {t}.d[i_] = {tab}.d[{idx}.d[i_] & 7];")
        self.full_dst(ins, 0, t, 256)
    v_VPERMPS = v_VPERMD

    def _pmovx(self, ins, sb, db, signed):
        dwidth = 256 if self.vreg(ins, 0)[1] == 256 else 128
        n = dwidth // db
        s = self.full_src(ins, 1, nbytes=n * sb // 8 if ins.op1_kind == K.MEMORY else None)
        t = self.t()
        sf = (SF if signed else UF)[sb]
        self.emit(f"V256 {t} = {{0}}; for (int i_ = 0; i_ < {n}; i_++) {t}.{UF[db]}[i_] = ({U[db]})({S[db] if signed else U[db]}){s}.{sf}[i_];")
        self.full_dst(ins, 0, t, dwidth)


    def _cvt_widen(self, ins, expr_fmt, sfield, dfield):
        """xmm/m64 -> xmm, or xmm/m128 -> ymm (2 or 4 elements double width)."""
        dwidth = 256 if self.vreg(ins, 0)[1] == 256 else 128
        n = dwidth // 64
        s = self.full_src(ins, 1, nbytes=n * 4 if ins.op1_kind == K.MEMORY else None)
        t = self.t()
        self.emit(f"V256 {t} = {{0}}; for (int i_ = 0; i_ < {n}; i_++) {t}.{dfield}[i_] = " + expr_fmt.format(s=f"{s}.{sfield}[i_]") + ";")
        self.full_dst(ins, 0, t, dwidth)

    def v_CVTDQ2PD(self, ins): self._cvt_widen(ins, "(double){s}", "sd", "fd")
    def v_CVTPS2PD(self, ins): self._cvt_widen(ins, "(double){s}", "f", "fd")

    def _cvt_narrow(self, ins, expr_fmt, dfield):
        """xmm/m128 -> xmm (2 elements), ymm/m256 -> xmm (4 elements); rest zeroed."""
        n = 4 if self.vwidth(ins) == 256 else 2
        s = self.full_src(ins, 1); t = self.t()
        self.emit(f"V256 {t} = {{0}}; for (int i_ = 0; i_ < {n}; i_++) {t}.{dfield}[i_] = " + expr_fmt.format(s=f"{s}.fd[i_]") + ";")
        d = self.vreg(ins, 0)[0]
        self.emit(f"{self.x(d)} = {t}.h[0];")
        if self.is_vex(ins):
            self.emit(f"{self.hh(d)} = (V128){{0}};")

    def v_CVTPD2PS(self, ins): self._cvt_narrow(ins, "(float){s}", "f")
    def v_CVTPD2DQ(self, ins): self._cvt_narrow(ins, "(u32)CVTD2I32({s})", "d")
    def v_CVTTPD2DQ(self, ins): self._cvt_narrow(ins, "(u32)CVTTD2I32({s})", "d")

    def v_VCVTPH2PS(self, ins):
        dwidth = 256 if self.vreg(ins, 0)[1] == 256 else 128
        n = dwidth // 32
        s = self.full_src(ins, 1, nbytes=n * 2 if ins.op1_kind == K.MEMORY else None); t = self.t()
        self.emit(f"V256 {t} = {{0}}; for (int i_ = 0; i_ < {n}; i_++) {{ __fp16 h_; memcpy(&h_, &{s}.w[i_], 2); {t}.f[i_] = (float)h_; }}")
        self.full_dst(ins, 0, t, dwidth)

    def v_VCVTPS2PH(self, ins):
        n = 8 if self.vreg(ins, 1)[1] == 256 else 4
        s = self.full_src(ins, 1); t = self.t()
        # imm8[1:0]: 0 nearest-even, 1 toward -inf, 2 toward +inf, 3 toward zero; imm8[2] = use MXCSR.RC (nearest here).
        # Directed modes: round to nearest, then step one half-ulp back if that went past the value the wrong way.
        rc = 0 if ins.immediate8 & 4 else ins.immediate8 & 3
        fix = ""
        if rc:
            cond = {1: "b_ > x_", 2: "b_ < x_", 3: "__builtin_fabsf(b_) > __builtin_fabsf(x_)"}[rc]
            # one step toward -inf (1), +inf (2), zero (3) on the sign-magnitude half encoding
            if rc == 3:
                step = "u_ = (u16)(u_ - 1);"
            else:
                tow_pos = rc == 2
                step = (f"if (u_ & 0x8000) u_ = (u_ == 0x8000) ? {'0x0001' if tow_pos else '0x8001'} : (u16)(u_ {'-' if tow_pos else '+'} 1); "
                        f"else u_ = (u_ == 0) ? {'0x0001' if tow_pos else '0x8001'} : (u16)(u_ {'+' if tow_pos else '-'} 1);")
            fix = f" float b_ = (float)h_; if ({cond}) {{ {step} }}"
        self.emit(f"V256 {t} = {{0}}; for (int i_ = 0; i_ < {n}; i_++) {{ float x_ = {s}.f[i_]; __fp16 h_ = (__fp16)x_; u16 u_; memcpy(&u_, &h_, 2);{fix} {t}.w[i_] = u_; }}")
        if ins.op0_kind == K.MEMORY:
            self.emit(f"memcpy((void*){self.addr(ins)}, &{t}, {n * 2});")
        else:
            d = self.vreg(ins, 0)[0]
            self.emit(f"{self.x(d)} = {t}.h[0]; {self.hh(d)} = (V128){{0}};")

    # ---- masked moves and gathers ----------------------------------------------------
    def _maskmov(self, ins, ebits):
        f = UF[ebits]; sf = SF[ebits]
        if ins.op0_kind == K.MEMORY:   # store: [m] = src where mask
            m, s = self.full_src(ins, 1), self.full_src(ins, 2)
            n = (32 if self.vreg(ins, 1)[1] == 256 else 16) * 8 // ebits
            a = self.addr(ins)
            self.emit(f"for (int i_ = 0; i_ < {n}; i_++) if ({m}.{sf}[i_] < 0) ST{ebits}({a} + i_ * {ebits // 8}, {s}.{f}[i_]);")
            return
        m = self.full_src(ins, 1)
        d = self.vreg(ins, 0); n = (32 if d[1] == 256 else 16) * 8 // ebits
        a = self.addr(ins); t = self.t()
        self.emit(f"V256 {t} = {{0}}; for (int i_ = 0; i_ < {n}; i_++) if ({m}.{sf}[i_] < 0) {t}.{f}[i_] = LD{ebits}({a} + i_ * {ebits // 8});")
        self.full_dst(ins, 0, t, d[1])

    def v_VMASKMOVPS(self, ins): self._maskmov(ins, 32)
    def v_VMASKMOVPD(self, ins): self._maskmov(ins, 64)
    def v_VPMASKMOVD(self, ins): self._maskmov(ins, 32)
    def v_VPMASKMOVQ(self, ins): self._maskmov(ins, 64)

    def _gather(self, ins, ibits, ebits):
        # vgather dst, [base + vindex*scale + disp], mask
        d = self.vreg(ins, 0); mreg = self.vreg(ins, 2)
        vidx = self.REG[ins.memory_index]
        idx = self.t()
        hi = self.hh(vidx[1]) if vidx[2] == 256 else "(V128){0}"
        self.emit(f"V256 {idx}; {idx}.h[0] = {self.x(vidx[1])}; {idx}.h[1] = {hi};")
        m = self.t()
        mhi = self.hh(mreg[0]) if mreg[1] == 256 else "(V128){0}"
        self.emit(f"V256 {m}; {m}.h[0] = {self.x(mreg[0])}; {m}.h[1] = {mhi};")
        base = "0"
        if ins.memory_base != R.NONE:
            base = self.g(self.REG[ins.memory_base][1])
        disp = ins.memory_displacement & 0xFFFFFFFFFFFFFFFF
        scale = ins.memory_index_scale
        nidx = (256 if vidx[2] == 256 else 128) // ibits
        nd = (256 if d[1] == 256 else 128) // ebits
        n = min(nidx, nd)
        t = self.t()
        dsrc = self.full_src(ins, 0)
        self.emit(f"V256 {t} = {dsrc}; for (int i_ = 0; i_ < {n}; i_++) if ({m}.{SF[ebits]}[i_] < 0) "
                  f"{t}.{UF[ebits]}[i_] = LD{ebits}((u64)({base}) + (u64)((i64){idx}.{SF[ibits]}[i_] * {scale}) + 0x{disp:x}ull);")
        self.emit(f"{self.x(mreg[0])} = (V128){{0}}; {self.hh(mreg[0])} = (V128){{0}};")
        self.full_dst(ins, 0, t, d[1])

    def v_VGATHERDPS(self, ins): self._gather(ins, 32, 32)
    def v_VGATHERDPD(self, ins): self._gather(ins, 32, 64)
    def v_VGATHERQPS(self, ins): self._gather(ins, 64, 32)
    def v_VGATHERQPD(self, ins): self._gather(ins, 64, 64)
    def v_VPGATHERDD(self, ins): self._gather(ins, 32, 32)
    def v_VPGATHERDQ(self, ins): self._gather(ins, 32, 64)
    def v_VPGATHERQD(self, ins): self._gather(ins, 64, 32)
    def v_VPGATHERQQ(self, ins): self._gather(ins, 64, 64)

    def v_VZEROUPPER(self, ins):
        self.emit("memset(c->xh, 0, sizeof c->xh); @ZU@")

    def v_VZEROALL(self, ins):
        self.emit("memset(c->xh, 0, sizeof c->xh); memset(c->x, 0, sizeof c->x); @ZU@ @ZA@")

    # ---- tests / compares that set EFLAGS --------------------------------------------
    def _ptest(self, ins, mode):
        a, b = self.full_src(ins, 0), self.full_src(ins, 1)
        n = 4 if self.vwidth(ins) == 256 else 2
        if mode == "all":
            self.emit(f"{{ u64 z_ = 0, c_ = 0; for (int i_ = 0; i_ < {n}; i_++) {{ z_ |= {a}.q[i_] & {b}.q[i_]; c_ |= ~{a}.q[i_] & {b}.q[i_]; }} zf = z_ == 0; cf = c_ == 0; of = sf = af = pf = 0; }}")
        else:
            sign = "0x8000000080000000ull" if mode == "ps" else "0x8000000000000000ull"
            self.emit(f"{{ u64 z_ = 0, c_ = 0; for (int i_ = 0; i_ < {n}; i_++) {{ z_ |= {a}.q[i_] & {b}.q[i_] & {sign}; c_ |= ~{a}.q[i_] & {b}.q[i_] & {sign}; }} zf = z_ == 0; cf = c_ == 0; of = sf = af = pf = 0; }}")

    def v_PTEST(self, ins): self._ptest(ins, "all")
    def v_VTESTPS(self, ins): self._ptest(ins, "ps")
    def v_VTESTPD(self, ins): self._ptest(ins, "pd")

    def _comis(self, ins, ty):
        lane = "f" if ty == "f" else "fd"
        a = f"{self.x(self.vreg(ins, 0)[0])}.{lane}[0]"
        b = f"({self.lane_src(ins, 1, 0)}).{lane}[0]"
        ct = "float" if ty == "f" else "double"
        self.emit(f"{{ {ct} a_ = {a}, b_ = {b}; if (a_ != a_ || b_ != b_) {{ zf = pf = cf = 1; }} else {{ zf = a_ == b_; pf = 0; cf = a_ < b_; }} of = sf = af = 0; }}")

    def v_UCOMISS(self, ins): self._comis(ins, "f")
    def v_COMISS(self, ins): self._comis(ins, "f")
    def v_UCOMISD(self, ins): self._comis(ins, "d")
    def v_COMISD(self, ins): self._comis(ins, "d")

    # ---- scalar int <-> float conversions ------------------------------------------
    def _cvt_si2f(self, ins, ty):
        vex = self.is_vex(ins)
        d = self.vreg(ins, 0)[0]
        A, S_ = (1, 2) if vex else (0, 1)
        w = self.width(ins, S_)
        lane = "f" if ty == "f" else "fd"
        cast = "float" if ty == "f" else "double"
        t = self.t()
        self.emit(f"V128 {t} = {self.lane_src(ins, A, 0)}; {t}.{lane}[0] = ({cast})({S[w]}){self.rd(ins, S_)}; {self.x(d)} = {t};")
        self.zero_upper(ins, d)

    def v_CVTSI2SS(self, ins): self._cvt_si2f(ins, "f")
    def v_CVTSI2SD(self, ins): self._cvt_si2f(ins, "d")

    def _cvt_f2si(self, ins, ty, trunc):
        w = self.width(ins, 0)
        lane = "f" if ty == "f" else "fd"
        src = f"({self.lane_src(ins, 1, 0)}).{lane}[0]"
        fn = f"CVT{'T' if trunc else ''}{'S' if ty == 'f' else 'D'}2I{w}"
        self.emit(self.wr(ins, 0, f"{fn}({src})"))

    def v_CVTTSS2SI(self, ins): self._cvt_f2si(ins, "f", True)
    def v_CVTTSD2SI(self, ins): self._cvt_f2si(ins, "d", True)
    def v_CVTSS2SI(self, ins): self._cvt_f2si(ins, "f", False)
    def v_CVTSD2SI(self, ins): self._cvt_f2si(ins, "d", False)

    def v_STMXCSR(self, ins): self.emit(f"ST32({self.addr(ins)}, c->mxcsr);")
    def v_LDMXCSR(self, ins): self.emit(f"c->mxcsr = LD32({self.addr(ins)});")

    # ---- BMI1 / BMI2 / ADX / MOVBE (GPR) --------------------------------------------
    def v_ANDN(self, ins):
        w = self.width(ins, 0); r = self.t()
        self.emit(f"{U[w]} {r} = ~{self.rd(ins, 1)} & {self.rd(ins, 2)}; zf = {r} == 0; sf = {r} >> {w - 1} & 1; cf = of = 0;")
        self.emit(self.wr(ins, 0, r))

    def v_BEXTR(self, ins):
        w = self.width(ins, 0); s, c_, r, st, ln = self.t(), self.t(), self.t(), self.t(), self.t()
        self.emit(f"{U[w]} {s} = {self.rd(ins, 1)}; u32 {c_} = (u32){self.rd(ins, 2)}; unsigned {st} = {c_} & 0xFF, {ln} = ({c_} >> 8) & 0xFF;")
        self.emit(f"{U[w]} {r} = {st} >= {w} ? 0 : ({U[w]})({s} >> {st}); if ({ln} < {w}) {r} &= ({U[w]})((1ull << {ln}) - 1); zf = {r} == 0; cf = of = 0;")
        self.emit(self.wr(ins, 0, r))

    def _blsx(self, ins, expr, cf):
        w = self.width(ins, 0); s, r = self.t(), self.t()
        self.emit(f"{U[w]} {s} = {self.rd(ins, 1)}; {U[w]} {r} = {expr.format(s=s)}; zf = {r} == 0; sf = {r} >> {w - 1} & 1; of = 0; cf = {cf.format(s=s)};")
        self.emit(self.wr(ins, 0, r))

    def v_BLSI(self, ins): self._blsx(ins, "{s} & -{s}", "{s} != 0")
    def v_BLSMSK(self, ins): self._blsx(ins, "{s} ^ ({s} - 1)", "{s} == 0")
    def v_BLSR(self, ins): self._blsx(ins, "{s} & ({s} - 1)", "{s} == 0")

    def v_BZHI(self, ins):
        w = self.width(ins, 0); s, r, n = self.t(), self.t(), self.t()
        self.emit(f"{U[w]} {s} = {self.rd(ins, 1)}; unsigned {n} = (u8){self.rd(ins, 2)}; {U[w]} {r} = {n} < {w} ? ({U[w]})({s} & ((1ull << {n}) - 1)) : {s};")
        self.emit(f"cf = {n} > {w - 1}; zf = {r} == 0; sf = {r} >> {w - 1} & 1; of = 0;")
        self.emit(self.wr(ins, 0, r))

    def v_PDEP(self, ins):
        w = self.width(ins, 0)
        self.emit(self.wr(ins, 0, f"({U[w]})PDEP64({self.rd(ins, 1)}, {self.rd(ins, 2)})"))

    def v_PEXT(self, ins):
        w = self.width(ins, 0)
        self.emit(self.wr(ins, 0, f"({U[w]})PEXT64({self.rd(ins, 1)}, {self.rd(ins, 2)})"))

    def v_MULX(self, ins):  # hi -> op0, lo -> op1, rdx * src
        w = self.width(ins, 0); p = self.t()
        self.g(2)
        wide = "u128" if w == 64 else "u64"
        self.emit(f"{wide} {p} = ({wide})({U[w]})rdx * ({wide}){self.rd(ins, 2)};")
        self.emit(self.wr(ins, 1, f"({U[w]}){p}"))
        self.emit(self.wr(ins, 0, f"({U[w]})({p} >> {w})"))

    def v_RORX(self, ins):
        w = self.width(ins, 0); s = self.t(); n = self.last_imm(ins) % w
        self.emit(f"{U[w]} {s} = {self.rd(ins, 1)};")
        self.emit(self.wr(ins, 0, f"({U[w]})(({s} >> {n}) | ({s} << ({w} - {n})))" if n else s))

    def _shx(self, ins, op):
        w = self.width(ins, 0); s, n = self.t(), self.t()
        self.emit(f"{U[w]} {s} = {self.rd(ins, 1)}; unsigned {n} = (unsigned){self.rd(ins, 2)} & {w - 1};")
        e = {"shl": f"{s} << {n}", "shr": f"{s} >> {n}", "sar": f"({S[w]}){s} >> {n}"}[op]
        self.emit(self.wr(ins, 0, f"({U[w]})({e})"))

    def v_SHLX(self, ins): self._shx(ins, "shl")
    def v_SHRX(self, ins): self._shx(ins, "shr")
    def v_SARX(self, ins): self._shx(ins, "sar")

    def _adx(self, ins, flag):
        w = self.width(ins, 0); a, b, r = self.t(), self.t(), self.t()
        self.emit(f"{U[w]} {a} = {self.rd(ins, 0)}; {U[w]} {b} = {self.rd(ins, 1)}; {U[w]} {r} = {a} + {b} + {flag};")
        self.emit(f"{flag} = ({r} < {a}) || ({flag} && {r} == {a});")
        self.emit(self.wr(ins, 0, r))

    def v_ADCX(self, ins): self._adx(ins, "cf")
    def v_ADOX(self, ins): self._adx(ins, "of")

    def v_MOVBE(self, ins):
        w = self.width(ins, 0)
        self.emit(self.wr(ins, 0, f"__builtin_bswap{w}({self.rd(ins, 1)})"))

    def v_CRC32(self, ins):
        w = self.width(ins, 1); dw = self.width(ins, 0)
        self.emit(self.wr(ins, 0, f"rt_crc32c((u32){self.rd(ins, 0, dw)}, {self.rd(ins, 1)}, {w // 8})", 32 if dw == 32 else 64))


FMA_MNEMONICS = {getattr(M, n) for n in dir(M) if n.startswith(("VFMADD", "VFMSUB", "VFNMADD", "VFNMSUB")) and n.isupper()}


def _mk_pmovx(sb, db, signed):
    return lambda self, ins: self._pmovx(ins, sb, db, signed)


for _n, _sb, _db in (("BW", 8, 16), ("BD", 8, 32), ("BQ", 8, 64), ("WD", 16, 32), ("WQ", 16, 64), ("DQ", 32, 64)):
    setattr(VectorMixin, f"v_PMOVZX{_n}", _mk_pmovx(_sb, _db, False))
    setattr(VectorMixin, f"v_PMOVSX{_n}", _mk_pmovx(_sb, _db, True))
