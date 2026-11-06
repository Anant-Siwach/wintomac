"""Generate the instruction cases for the interpreter differential test (tests/interp_fuzz).

Each case is a short x86-64 snippet; both runners execute it from the same pseudo-random states (registers,
flags, xmm, a scratch page) and print the resulting state. interp_runner uses runtime/interp*.c,
x86_runner runs natively under Rosetta 2. Flags that the SDM leaves undefined are masked per case.

    python3 gen.py <out-dir>      writes cases.s, assembles it (mingw as) and writes cases.txt
"""
import os
import subprocess
import sys

CF, PF, AF, ZF, SF, OF = 1, 4, 16, 64, 128, 2048
ALL = CF | PF | AF | ZF | SF | OF
cases = []   # (name, asm, flagmask)


def case(asm, mask=ALL, name=None):
    cases.append((name or asm.split(";")[-1].strip(), asm, mask))


# registers: rsi/rbx/rdi point into the scratch page; everything else is random
R = {64: ["rax", "rcx", "rdx", "r8", "r13"], 32: ["eax", "ecx", "edx", "r9d", "r14d"],
     16: ["ax", "cx", "dx", "r10w"], 8: ["al", "cl", "dl", "ah", "r11b", "bpl"]}
PTR = {8: "byte", 16: "word", 32: "dword", 64: "qword"}
IMM = {8: ["0x7f", "-1", "1"], 16: ["0x7fff", "-2", "0x1234"], 32: ["0x7fffffff", "-5", "0x80000000"], 64: ["-1", "0x7fffffff", "-0x80000000", "3"]}

for op in ["add", "adc", "sub", "sbb", "cmp", "and", "or", "xor", "test"]:
    m = ALL & ~AF if op in ("and", "or", "xor", "test") else ALL
    for w in (8, 16, 32, 64):
        r = R[w]
        case(f"{op} {r[0]}, {r[1]}", m); case(f"{op} {r[2]}, {r[3]}", m); case(f"{op} {r[1]}, {r[1]}", m)
        for i in IMM[w]:
            if w == 64 and i.startswith("0x8"):
                continue
            case(f"{op} {r[0]}, {i}", m)
        case(f"{op} {PTR[w]} ptr [rsi+8], {r[1]}", m)
        if op != "test":
            case(f"{op} {r[2]}, {PTR[w]} ptr [rbx+16]", m)
            case(f"lock {op} {PTR[w]} ptr [rsi+24], {r[0]}", m) if op not in ("cmp",) else None
for op in ["inc", "dec", "neg", "not"]:
    for w in (8, 16, 32, 64):
        case(f"{op} {R[w][0]}", 0 if op == "not" else ALL)
        case(f"{op} {PTR[w]} ptr [rsi+32]", 0 if op == "not" else ALL)
    case(f"lock {op} qword ptr [rsi+40]", 0 if op == "not" else ALL)

for op in ["shl", "shr", "sar"]:
    for w in (8, 16, 32, 64):
        r = R[w][0]
        case(f"{op} {r}, 1", ALL & ~AF)
        for n in (3, w - 1):
            case(f"{op} {r}, {n}", CF | PF | ZF | SF)
        case(f"and ecx, 0x3f; {op} {r}, cl", (PF | ZF | SF | CF) if w >= 32 else (PF | ZF | SF), name=f"{op} {r}, cl")
        case(f"and ecx, 7; {op} {r}, cl", PF | ZF | SF | CF, name=f"{op} {r}, cl(small)")
for op in ["rol", "ror", "rcl", "rcr"]:
    for w in (8, 16, 32, 64):
        r = R[w][0]
        case(f"{op} {r}, 1", CF | OF)
        case(f"{op} {r}, 5", CF)
        case(f"and ecx, 0x3f; {op} {r}, cl", CF if op in ("rol", "ror") else 0, name=f"{op} {r}, cl")
for op in ["shld", "shrd"]:
    for w, a, b in ((16, "ax", "dx"), (32, "eax", "edx"), (64, "rax", "rdx")):
        case(f"{op} {a}, {b}, 1", CF | PF | ZF | SF | OF)
        case(f"{op} {a}, {b}, 7", CF | PF | ZF | SF)
        if w > 16:
            case(f"and ecx, 0x1f; {op} {a}, {b}, cl", 0, name=f"{op} {a},{b},cl")

for w in (8, 16, 32, 64):
    r = R[w]
    case(f"mul {r[1]}", CF | OF); case(f"imul {r[1]}", CF | OF)
    if w > 8:
        case(f"imul {r[0]}, {r[2]}", CF | OF); case(f"imul {r[0]}, {r[2]}, 77", CF | OF); case(f"imul {r[0]}, {r[2]}, -3", CF | OF)
        case(f"imul {r[0]}, {PTR[w]} ptr [rsi]", CF | OF)
case("and r8, 0xffff; or r8, 1; xor edx, edx; div r8", 0, name="div r64")
case("and r8d, 0xffff; or r8d, 1; xor edx, edx; div r8d", 0, name="div r32")
case("and r8, 0xffff; or r8, 3; cqo; idiv r8", 0, name="idiv r64")
case("and r8d, 0xff; or r8d, 3; cdq; idiv r8d", 0, name="idiv r32")
case("and r8d, 0x7f; or r8d, 0x40; and eax, 0x1fff; div r8b", 0, name="div r8")
case("and r10d, 0x7f; or r10d, 0x41; cwd; and edx, 0x1f; idiv r10w", 0, name="idiv r16")
case("mov rax, -9; mov r9, 4; cqo; idiv r9", 0, name="idiv neg")

for op in ["bt", "bts", "btr", "btc"]:
    for w in (16, 32, 64):
        r = R[w]
        case(f"{op} {r[0]}, {r[1]}", CF); case(f"{op} {r[0]}, 13", CF)
        case(f"{op} {PTR[w]} ptr [rsi+64], 9", CF)
    case(f"and rcx, 0x3ff; {op} qword ptr [rsi+256], rcx", CF, name=f"{op} mem, reg (bitstring)")
    case(f"and rcx, 0x3ff; neg rcx; {op} dword ptr [rsi+256], ecx", CF, name=f"{op} mem, -reg (bitstring)")
    if op != "bt":
        case(f"lock {op} qword ptr [rsi+72], 33", CF)
for op in ["bsf", "bsr"]:
    for w in (16, 32, 64):
        case(f"or {R[w][1]}, 0x100; {op} {R[w][0]}, {R[w][1]}", ZF, name=f"{op} {w}")
for op in ["tzcnt", "lzcnt", "popcnt"]:
    for w in (16, 32, 64):
        case(f"{op} {R[w][0]}, {R[w][1]}", CF | ZF if op != "popcnt" else ALL)
        case(f"{op} {R[w][0]}, {PTR[w]} ptr [rsi+80]", CF | ZF if op != "popcnt" else ALL)
case("xor ecx, ecx; tzcnt rax, rcx", CF | ZF, name="tzcnt zero"); case("xor ecx, ecx; lzcnt eax, ecx", CF | ZF, name="lzcnt zero")

CC = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"]
for cc in CC:
    case(f"set{cc} al"); case(f"set{cc} byte ptr [rsi+3]")
    case(f"cmov{cc} rax, rcx"); case(f"cmov{cc} eax, ecx"); case(f"cmov{cc} ax, word ptr [rsi]")
for i in ["movzx eax, cl", "movzx rax, word ptr [rsi+2]", "movsx rax, cl", "movsx eax, word ptr [rsi+6]", "movsxd rax, ecx",
          "movsx ax, dl", "mov ah, cl", "mov al, ah", "mov eax, ecx", "mov ax, cx", "mov rax, 0x123456789abcdef0", "mov eax, -1",
          "lea rax, [rcx + rdx*4 + 100]", "lea eax, [rcx + rdx*8 - 7]", "lea ax, [rcx + rdx]", "lea rax, [rip + 16]",
          "bswap eax", "bswap rcx", "xchg rax, rcx", "xchg eax, edx", "xchg ax, r10w", "xchg al, ah", "xchg qword ptr [rsi], rdx",
          "xadd rax, rcx", "xadd dword ptr [rsi+12], edx", "lock xadd qword ptr [rsi+16], rcx",
          "cmpxchg rcx, rdx", "cmpxchg ecx, edx", "mov rax, qword ptr [rsi]; cmpxchg qword ptr [rsi], rdx", "cmpxchg qword ptr [rsi], rdx",
          "lock cmpxchg dword ptr [rsi+4], ecx", "cmpxchg al, cl",
          "mov rax, qword ptr [rsi]; mov rdx, qword ptr [rsi+8]; lock cmpxchg16b xmmword ptr [rsi]", "lock cmpxchg16b xmmword ptr [rsi]",
          "cmpxchg8b qword ptr [rsi+8]",
          "cbw", "cwde", "cdqe", "cwd", "cdq", "cqo", "lahf", "sahf", "clc", "stc", "cmc",
          "movbe eax, dword ptr [rsi]", "movbe qword ptr [rsi+8], rcx",
          "andn rax, rcx, rdx", "andn eax, ecx, dword ptr [rsi]", "blsi rax, rcx", "blsr eax, ecx", "blsmsk rax, rdx",
          "bzhi rax, rcx, rdx", "bextr rax, rcx, rdx", "pdep rax, rcx, rdx", "pext rax, rcx, rdx", "pdep eax, ecx, edx",
          "mulx r8, r9, rcx", "mulx r8d, r9d, ecx", "sarx rax, rcx, rdx", "shlx eax, ecx, edx", "shrx rax, rcx, rdx", "rorx rax, rcx, 13", "rorx eax, ecx, 31",
          # adcx/adox: not supported by Rosetta 2, so not testable here
          "crc32 eax, cl", "crc32 eax, cx", "crc32 eax, ecx", "crc32 rax, rcx", "crc32 eax, byte ptr [rsi]"]:
    m = ALL
    if i.startswith(("andn", "bextr", "bzhi")):
        m = CF | ZF | OF if not i.startswith("bextr") else ZF | CF | OF
    if i.startswith(("blsi", "blsr", "blsmsk")):
        m = CF | ZF | SF | OF
    if i.startswith("adcx"):
        m = CF
    if i.startswith("adox"):
        m = OF
    if "cmpxchg" in i and "8b" not in i and "16b" not in i:
        m = ZF   # SDM/QEMU/box64/the lifter: flags of (accumulator - dest); Rosetta 2 computes (dest - accumulator)
    case(i, m)
case("std; cld", ALL, name="std;cld")

# string ops (rdi -> scratch+0x200)
for sfx in "bwdq":
    case(f"lea rdi, [rbx+0x200]; and ecx, 15; rep movs{sfx}", ALL, name=f"rep movs{sfx}")
    case(f"lea rdi, [rbx+0x200]; and ecx, 15; rep stos{sfx}", ALL, name=f"rep stos{sfx}")
    case(f"lea rdi, [rbx+0x200]; movs{sfx}", ALL, name=f"movs{sfx}")
    case(f"lods{sfx}", ALL)
    case(f"lea rdi, [rsi+1]; and ecx, 31; repe cmps{sfx}", ALL, name=f"repe cmps{sfx}")
    case(f"mov rdi, rsi; and ecx, 31; repne scas{sfx}", ALL, name=f"repne scas{sfx}")
case("lea rdi, [rsi+5]; mov ecx, 40; rep movsb", ALL, name="rep movsb overlap")
case("lea rdi, [rbx+0x300]; mov ecx, 9; std; rep movsb; cld", ALL, name="rep movsb backwards")

# ---- SSE family (xmm0..xmm15 random; [rsi] memory operands) ----
def sse(i, m=0):
    case(i, m)

binops = ("addps addpd addss addsd subps subpd subss subsd mulps mulpd mulss mulsd divps divpd divss divsd minps minpd minss minsd "
          "maxps maxpd maxss maxsd andps andpd andnps andnpd orps orpd xorps xorpd unpcklps unpckhps unpcklpd unpckhpd "
          "haddps haddpd hsubps hsubpd addsubps addsubpd "
          "paddb paddw paddd paddq psubb psubw psubd psubq paddsb paddsw psubsb psubsw paddusb paddusw psubusb psubusw "
          "pmullw pmulhw pmulhuw pmuludq pmaddwd pavgb pavgw psadbw pand pandn por pxor pcmpeqb pcmpeqw pcmpeqd pcmpgtb pcmpgtw "
          "pcmpgtd pminub pmaxub pminsw pmaxsw punpcklbw punpckhbw punpcklwd punpckhwd punpckldq punpckhdq punpcklqdq punpckhqdq "
          "packsswb packssdw packuswb pshufb phaddw phaddd phaddsw phsubw phsubd phsubsw pmaddubsw pmulhrsw psignb psignw psignd "
          "pmulld pmuldq pminsb pminsd pminuw pminud pmaxsb pmaxsd pmaxuw pmaxud pcmpeqq packusdw pcmpgtq "
          "sqrtps sqrtpd sqrtss sqrtsd pabsb pabsw pabsd movshdup movsldup movddup cvtdq2ps cvtps2dq cvttps2dq "
          "cvtss2sd cvtsd2ss cvtdq2pd cvtps2pd cvtpd2ps cvtpd2dq cvttpd2dq movhlps movlhps phminposuw").split()
for op in binops:
    sse(f"{op} xmm1, xmm2")
    if op not in ("movhlps", "movlhps"):
        dw = {"addss", "subss", "mulss", "divss", "minss", "maxss", "sqrtss", "cvtss2sd"}
        qw = {"addsd", "subsd", "mulsd", "divsd", "minsd", "maxsd", "sqrtsd", "cvtsd2ss", "movddup", "cvtdq2pd", "cvtps2pd"}
        sse(f"{op} xmm3, {'dword' if op in dw else 'qword' if op in qw else 'xmmword'} ptr [rsi]")
for op, imms in (("shufps", (0x1b, 0xe4, 0x4e)), ("shufpd", (0, 1, 2, 3)), ("pshufd", (0x1b, 0xaa)), ("pshuflw", (0x1b,)),
                 ("pshufhw", (0x93,)), ("palignr", (0, 3, 8, 15, 20)), ("blendps", (5, 10)), ("blendpd", (1, 2)), ("pblendw", (0x5a,)),
                 ("roundps", (0, 1, 2, 3)), ("roundpd", (0, 1, 2, 3)), ("roundss", (1, 2)), ("roundsd", (0, 3)), ("dpps", (0xff, 0x71, 0x3c)),
                 ("dppd", (0x33, 0x11)), ("cmpps", range(8)), ("cmppd", (0, 1, 3, 4, 7)), ("cmpss", (1, 2, 5)), ("cmpsd", (0, 6)),
                 ("insertps", (0x10, 0x9c, 0x35))):
    for imm in imms:
        sse(f"{op} xmm1, xmm2, {imm}")
for sh in ("psllw psllq pslld psrlw psrld psrlq psraw psrad").split():
    for n in (0, 3, 15, 31, 40):
        sse(f"{sh} xmm4, {n}")
    sse(f"{sh} xmm4, xmm5")
for sh in ("pslldq", "psrldq"):
    for n in (0, 1, 7, 15, 16):
        sse(f"{sh} xmm4, {n}")
for i in ["blendvps xmm1, xmm2, xmm0", "blendvpd xmm1, xmm2, xmm0", "pblendvb xmm1, xmm2, xmm0",
          "movaps xmm1, xmm2", "movups xmm1, xmmword ptr [rsi+3]", "movups xmmword ptr [rsi+5], xmm2", "movdqu xmm7, xmmword ptr [rsi]",
          "movss xmm1, xmm2", "movss xmm1, dword ptr [rsi]", "movss dword ptr [rsi+4], xmm2", "movsd xmm1, xmm2", "movsd xmm1, qword ptr [rsi]",
          "movsd qword ptr [rsi+8], xmm2", "movd xmm1, eax", "movd eax, xmm2", "movq xmm1, rcx", "movq rax, xmm2", "movq xmm1, xmm2",
          "movq xmm1, qword ptr [rsi]", "movq qword ptr [rsi], xmm3", "movd xmm1, dword ptr [rsi]",
          "movhps xmm1, qword ptr [rsi]", "movlps xmm1, qword ptr [rsi]", "movhpd qword ptr [rsi], xmm2", "movlpd qword ptr [rsi+8], xmm2",
          "movmskps eax, xmm2", "movmskpd eax, xmm2", "pmovmskb eax, xmm2",
          "pinsrb xmm1, eax, 5", "pinsrw xmm1, ecx, 3", "pinsrd xmm1, edx, 2", "pinsrq xmm1, rcx, 1", "pinsrw xmm1, word ptr [rsi], 7",
          "pextrb eax, xmm2, 9", "pextrw eax, xmm2, 5", "pextrd eax, xmm2, 3", "pextrq rax, xmm2, 1", "pextrw word ptr [rsi], xmm2, 2",
          "extractps eax, xmm2, 1", "extractps dword ptr [rsi], xmm2, 3",
          "pmovzxbw xmm1, xmm2", "pmovsxbw xmm1, xmm2", "pmovzxbd xmm1, dword ptr [rsi]", "pmovsxbq xmm1, xmm2", "pmovzxwd xmm1, xmm2",
          "pmovsxwq xmm1, xmm2", "pmovzxdq xmm1, qword ptr [rsi]", "pmovsxdq xmm1, xmm2",
          "cvtsi2ss xmm1, eax", "cvtsi2ss xmm1, rcx", "cvtsi2sd xmm1, eax", "cvtsi2sd xmm1, rdx", "cvtsi2sd xmm1, dword ptr [rsi]",
          "cvttss2si eax, xmm2", "cvttss2si rax, xmm2", "cvtss2si eax, xmm2", "cvttsd2si eax, xmm2", "cvttsd2si rax, xmm2", "cvtsd2si rax, xmm2"]:
    sse(i)
for i in ["comiss xmm1, xmm2", "ucomiss xmm1, xmm2", "comisd xmm1, xmm2", "ucomisd xmm1, qword ptr [rsi]"]:
    sse(i, ALL)
# PTEST clears AF/OF/PF/SF per the SDM; Rosetta 2 leaves them unchanged, so only ZF/CF are compared
for i in ["ptest xmm1, xmm2", "ptest xmm1, xmm1"]:
    sse(i, ZF | CF)
for i in ["pcmpistri xmm1, xmm2, 0x0c", "pcmpistri xmm1, xmm2, 0x00", "pcmpistrm xmm1, xmm2, 0x40", "pcmpestri xmm1, xmm2, 0x18"]:
    case(("and eax, 15; and edx, 15; " if "estr" in i else "") + i, CF | ZF | SF | OF, name=i)

# AVX / AVX2 / FMA (VEX)
for i in ["vaddps ymm1, ymm2, ymm3", "vaddps xmm1, xmm2, xmm3", "vmulpd ymm1, ymm2, ymmword ptr [rsi]", "vaddss xmm1, xmm2, xmm3",
          "vxorps ymm1, ymm1, ymm1", "vpaddd ymm1, ymm2, ymm3", "vpshufb ymm1, ymm2, ymm3", "vpshufd ymm1, ymm2, 0x1b",
          "vshufps ymm1, ymm2, ymm3, 0x4e", "vunpcklps ymm1, ymm2, ymm3", "vpunpckhbw ymm4, ymm5, ymm6", "vpermilps ymm1, ymm2, 0x1b",
          "vpermilpd ymm1, ymm2, 5", "vpermilps xmm1, xmm2, xmm3", "vbroadcastss ymm1, xmm2", "vbroadcastss xmm1, dword ptr [rsi]",
          "vbroadcastsd ymm1, xmm2", "vpbroadcastb ymm1, xmm2", "vpbroadcastw xmm1, xmm2", "vpbroadcastd ymm1, dword ptr [rsi]",
          "vpbroadcastq ymm1, xmm2", "vbroadcastf128 ymm1, xmmword ptr [rsi]", "vinsertf128 ymm1, ymm2, xmm3, 1", "vinserti128 ymm1, ymm2, xmm3, 0",
          "vextractf128 xmm1, ymm2, 1", "vextracti128 xmmword ptr [rsi], ymm2, 1", "vperm2f128 ymm1, ymm2, ymm3, 0x31",
          "vperm2i128 ymm1, ymm2, ymm3, 0x08", "vpermq ymm1, ymm2, 0x1b", "vpermd ymm1, ymm2, ymm3", "vpermps ymm1, ymm2, ymm3",
          "vpermpd ymm1, ymm2, 0x4e", "vzeroupper", "vmovaps ymm1, ymm2", "vmovups ymmword ptr [rsi], ymm2", "vmovdqu ymm1, ymmword ptr [rsi]",
          "vmovaps xmm1, xmm2", "vmovss xmm1, xmm2, xmm3", "vmovsd xmm1, qword ptr [rsi]", "vmovq xmm1, xmm2", "vmovd xmm1, eax",
          "vpsllw ymm1, ymm2, 3", "vpsrad ymm1, ymm2, xmm3", "vpslldq ymm1, ymm2, 5", "vpsrlvd ymm1, ymm2, ymm3",
          "vpsravd ymm1, ymm2, ymm3",
          # F16C (PS5 titles): every vcvtps2ph rounding mode, register and memory forms
          "vcvtph2ps ymm1, xmm2", "vcvtph2ps xmm1, qword ptr [rsi]", "vcvtps2ph xmm1, ymm2, 0", "vcvtps2ph xmm1, ymm2, 1",
          "vcvtps2ph xmm1, ymm2, 2", "vcvtps2ph xmm1, ymm2, 3", "vcvtps2ph xmm1, xmm2, 4", "vcvtps2ph qword ptr [rsi], xmm2, 3",
          "movbe eax, dword ptr [rsi+1]", "movbe qword ptr [rsi+3], rcx", "movbe ax, word ptr [rsi+5]",
          # per-element counts > 63 must give 0 (SDM); Rosetta 2 does not do that, so only in-range counts here
          "vpsrlq xmm3, xmm3, 58; vpsllvq xmm1, xmm2, xmm3", "vpblendd ymm1, ymm2, ymm3, 0xa5", "vblendvps ymm1, ymm2, ymm3, ymm4", "vpblendvb xmm1, xmm2, xmm3, xmm4",
          "vpmovzxbw ymm1, xmm2", "vpmovsxwd ymm1, xmm2", "vpmovzxbd ymm1, qword ptr [rsi]", "vcvtps2pd ymm1, xmm2", "vcvtpd2ps xmm1, ymm2",
          "vcvtdq2pd ymm1, xmm2", "vcvttpd2dq xmm1, ymm2", "vcvtdq2ps ymm1, ymm2", "vsqrtps ymm1, ymm2", "vroundps ymm1, ymm2, 1",
          "vdpps ymm1, ymm2, ymm3, 0xf1", "vcmpps ymm1, ymm2, ymm3, 1", "vcmpps ymm1, ymm2, ymm3, 13", "vcmppd xmm1, xmm2, xmm3, 20",
          "vmaxps ymm1, ymm2, ymm3", "vhaddps ymm1, ymm2, ymm3", "vpalignr ymm1, ymm2, ymm3, 7", "vpackusdw ymm1, ymm2, ymm3",
          "vpmaddwd ymm1, ymm2, ymm3", "vpmulld ymm1, ymm2, ymm3", "vpminsd ymm1, ymm2, ymm3", "vpcmpgtq ymm1, ymm2, ymm3",
          "vpinsrd xmm1, xmm2, eax, 1", "vinsertps xmm1, xmm2, xmm3, 0x5c", "vmovhps xmm1, xmm2, qword ptr [rsi]",
          "vfmadd132ps ymm1, ymm2, ymm3", "vfmadd213ps xmm1, xmm2, xmm3", "vfmadd231pd ymm1, ymm2, ymm3", "vfmsub231ps ymm1, ymm2, ymm3",
          "vfnmadd213ss xmm1, xmm2, xmm3", "vfnmsub132sd xmm1, xmm2, xmm3", "vfmadd231sd xmm1, xmm2, qword ptr [rsi]",
          "vfmaddsub213ps ymm1, ymm2, ymm3", "vfmsubadd231pd ymm1, ymm2, ymm3"]:
    sse(i)
for i in ["vptest ymm1, ymm2", "vtestps ymm1, ymm2", "vtestpd xmm1, xmm2"]:
    sse(i, ZF | CF)
for i in ["vucomiss xmm1, xmm2", "vcomisd xmm1, xmm2"]:
    sse(i, ALL)
case("vpmovmskb eax, ymm2", 0); case("vmovmskps eax, ymm2", 0)

# x87 (results that are exact in double precision, so a double-based model must match)
for i in ["fld qword ptr [rsi+0x400]; fld qword ptr [rsi+0x408]; faddp st(1), st; fstp qword ptr [rsi+0x410]",
          "fld qword ptr [rsi+0x400]; fmul qword ptr [rsi+0x408]; fstp qword ptr [rsi+0x410]",
          "fild dword ptr [rsi+0x418]; fild qword ptr [rsi+0x420]; fsubp st(1), st; fistp qword ptr [rsi+0x428]",
          "fld1; fldz; fucomip st, st(1); fstp st(0); setb al; sete ah",
          "fld qword ptr [rsi+0x400]; fld qword ptr [rsi+0x408]; fcomi st, st(1); fstp st(0); fstp st(0); setb al; seta ah",
          "fld qword ptr [rsi+0x400]; fchs; fabs; fstp dword ptr [rsi+0x430]",
          "fld qword ptr [rsi+0x400]; fld qword ptr [rsi+0x408]; fxch st(1); fdivrp st(1), st; fstp qword ptr [rsi+0x438]",
          "fnstcw word ptr [rsi+0x440]; fld qword ptr [rsi+0x400]; fistp dword ptr [rsi+0x444]",
          "fld qword ptr [rsi+0x400]; fstp tbyte ptr [rsi+0x450]; fld tbyte ptr [rsi+0x450]; fstp qword ptr [rsi+0x460]"]:
    case(i, CF | ZF | PF, name=i.split(";")[0] + " ... (" + str(len(cases)) + ")")

# control flow inside the snippet
case("test rcx, rcx; jz 1f; inc rax; 1: nop", ALL, name="jz fwd")
case("and ecx, 7; inc ecx; 1: add rax, rcx; loop 1b", ALL & ~AF, name="loop")
case("and ecx, 3; jrcxz 1f; mov rax, 5; 1: nop", ALL, name="jrcxz")
case("call 1f; jmp 2f; 1: add rax, 3; ret; 2: nop", ALL, name="call/ret within")
case("lea rdx, [rip + 1f]; jmp rdx; ud2; 1: mov eax, 9", ALL, name="jmp reg")
case("push rcx; push rdx; pop rax; pop r8", ALL, name="push/pop")
case("push 0x7f; pushfq; pop rdx; pop rax; and edx, 0x8d5", ALL, name="push imm / pushfq")
case("pushfq; or qword ptr [rsp], 0x801; popfq", ALL, name="popfq")

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "cases.s"), "w") as f:
        f.write(".intel_syntax noprefix\n.text\n")
        for k, (name, asm, mask) in enumerate(cases):
            f.write(f".globl case_{k}\ncase_{k}:\n  " + "\n  ".join(a.strip() for a in asm.split(";")) + "\n")
        f.write(f".globl case_{len(cases)}\ncase_{len(cases)}:\n  nop\n")
    o = os.path.join(out, "cases.o")
    subprocess.check_call(["x86_64-w64-mingw32-as", os.path.join(out, "cases.s"), "-o", o])
    if subprocess.check_output(["x86_64-w64-mingw32-objdump", "-r", o]).decode().count("R_X86") or \
       "IMAGE_REL" in subprocess.check_output(["x86_64-w64-mingw32-objdump", "-r", o]).decode():
        sys.exit("cases need relocations: keep snippets position independent")
    subprocess.check_call(["x86_64-w64-mingw32-objcopy", "-O", "binary", "-j", ".text", o, os.path.join(out, "cases.bin")])
    code = open(os.path.join(out, "cases.bin"), "rb").read()
    offs = {}
    for l in subprocess.check_output(["x86_64-w64-mingw32-nm", o]).decode().split("\n"):
        p = l.split()
        if len(p) == 3 and p[2].startswith("case_"):
            offs[int(p[2][5:])] = int(p[0], 16)
    with open(os.path.join(out, "cases.txt"), "w") as f:
        for k, (name, asm, mask) in enumerate(cases):
            f.write(f"{code[offs[k]:offs[k + 1]].hex()} {mask:x} {name}\n")
    print(f"[interp_fuzz] {len(cases)} cases")
