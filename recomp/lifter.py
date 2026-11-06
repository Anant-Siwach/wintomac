"""x86-64 instruction -> C statement lifter.

Model
-----
* Guest pointers == host pointers (both 64-bit); the runtime maps the PE image at its preferred base.
* Each guest function becomes `void f_<addr>(Ctx *c)`. Guest registers the function touches are held
  in C locals (so clang can keep them in ARM registers) and synced to/from `c` around calls: for direct
  calls only what the callee reads or may change (recomp/regsum.py); lifting is two-phase per program
  (lift with sync placeholders + collect register facts, summarise, then expand the placeholders).
* Flags are C locals. They are computed eagerly; clang's DCE removes the ones nothing reads.
  (Win64 ABI: flags are not preserved across calls, so they never need to live in Ctx.)
* `call` pushes the real guest return address so stack layout (args at [rsp+28h], etc.) is exact;
  the callee's `ret` pops it and does a host `return`.
"""
from __future__ import annotations

import re
import zlib

import iced_x86 as X

from . import regsum
from .analysis import Function, Program
from .vector import Unsupported, VectorMixin

R = X.Register
M = X.Mnemonic
K = X.OpKind
FC = X.FlowControl

MNEMONIC_NAME = {getattr(M, n): n for n in dir(M) if n.isupper()}

# Arithmetic flags packed in RFLAGS layout (IF and reserved bit 1 set), for pushfq and interpreter hand-offs.
FLAGS_PACKED = "((u64)cf | (u64)pf<<2 | (u64)af<<4 | (u64)zf<<6 | (u64)sf<<7 | (u64)of<<11 | 0x202)"
# Instructions whose 32/64-bit immediates may be absolute addresses (pointer loads, pushes, pointer compares).
ABS_IMM_MNEMONICS = {X.Mnemonic.MOV, X.Mnemonic.PUSH, X.Mnemonic.CMP, X.Mnemonic.ADD, X.Mnemonic.SUB}
GPR64 = ["RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI"] + [f"R{i}" for i in range(8, 16)]
GPR32 = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"] + [f"R{i}D" for i in range(8, 16)]
GPR16 = ["AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"] + [f"R{i}W" for i in range(8, 16)]
GPR8 = ["AL", "CL", "DL", "BL", "SPL", "BPL", "SIL", "DIL"] + [f"R{i}L" for i in range(8, 16)]
GPR8H = ["AH", "CH", "DH", "BH"]
LOCAL = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"] + [f"r{i}" for i in range(8, 16)]

REG = {}  # iced Register -> (kind, index, bits, high8)
for i in range(16):
    REG[getattr(R, GPR64[i])] = ("g", i, 64, False)
    REG[getattr(R, GPR32[i])] = ("g", i, 32, False)
    REG[getattr(R, GPR16[i])] = ("g", i, 16, False)
    REG[getattr(R, GPR8[i])] = ("g", i, 8, False)
    REG[getattr(R, f"XMM{i}")] = ("x", i, 128, False)
    REG[getattr(R, f"YMM{i}")] = ("y", i, 256, False)
for i in range(4):
    REG[getattr(R, GPR8H[i])] = ("g", i, 8, True)
regsum._init_regmap(REG)

# Interprocedural call-site sync (recomp/regsum.py). False: every call syncs every register (the old scheme).
CALLOPT = True
# True: functions with an EH frame record (landing pads, __try) keep the full sync (the pre-regsum behaviour).
EH_CONSERVATIVE = False

U = {8: "u8", 16: "u16", 32: "u32", 64: "u64", 128: "u128"}
S = {8: "i8", 16: "i16", 32: "i32", 64: "i64", 128: "i128"}
MASK = {8: "0xFFull", 16: "0xFFFFull", 32: "0xFFFFFFFFull", 64: "0xFFFFFFFFFFFFFFFFull"}

CC = {  # iced ConditionCode -> C expression over flag locals
    X.ConditionCode.O: "of", X.ConditionCode.NO: "!of",
    X.ConditionCode.B: "cf", X.ConditionCode.AE: "!cf",
    X.ConditionCode.E: "zf", X.ConditionCode.NE: "!zf",
    X.ConditionCode.BE: "(cf|zf)", X.ConditionCode.A: "(!cf&&!zf)",
    X.ConditionCode.S: "sf", X.ConditionCode.NS: "!sf",
    X.ConditionCode.P: "pf", X.ConditionCode.NP: "!pf",
    X.ConditionCode.L: "(sf!=of)", X.ConditionCode.GE: "(sf==of)",
    X.ConditionCode.LE: "(zf||sf!=of)", X.ConditionCode.G: "(!zf&&sf==of)",
}


def hx(v: int) -> str:
    return f"0x{v & 0xFFFFFFFFFFFFFFFF:x}ull"


class FuncLifter(VectorMixin):
    REG = REG

    def __init__(self, prog: Program, fn: Function):
        self.prog, self.fn = prog, fn
        self.out: list[str] = []
        self.used_g: set[int] = {4}  # rsp always
        self.used_x: set[int] = set()
        self.used_h: set[int] = set()
        self.unsupported: dict[str, int] = {}
        self.tmp = 0
        self.refs = 0                     # register slots referenced while lifting the current instruction
        self.cc_src = self.prev_cc = None
        self.callsites: list[int] = []    # @CS<n>@ / @TS<n>@ placeholder index -> callee
        self.recblocks: list[tuple[int, list[regsum.Rec]]] = []
        self.stub = None                  # whole body when the entry isn't decodable
        self.fp_frame = False             # sets up rbp as a frame pointer

    # --- small helpers ---------------------------------------------------
    def t(self) -> str:
        self.tmp += 1
        return f"t{self.tmp}"

    def emit(self, s: str):
        self.out.append("    " + s)

    def g(self, i: int) -> str:
        self.used_g.add(i)
        self.refs |= 1 << i
        return LOCAL[i]

    # Placeholders: the full register set is only known once the whole function is lifted.
    def sync_out(self) -> str:
        return "@SO@"

    def sync_in(self) -> str:
        return "@SI@"

    def _sync_text(self):
        so = " ".join(f"c->r[{i}]={LOCAL[i]};" for i in sorted(self.used_g)) + \
            "".join(f" c->x[{i}]=x{i};" for i in sorted(self.used_x)) + \
            "".join(f" c->xh[{i}]=h{i};" for i in sorted(self.used_h))
        si = " ".join(f"{LOCAL[i]}=c->r[{i}];" for i in sorted(self.used_g)) + \
            "".join(f" x{i}=c->x[{i}];" for i in sorted(self.used_x)) + \
            "".join(f" h{i}=c->xh[{i}];" for i in sorted(self.used_h))
        return so, si

    # --- operands --------------------------------------------------------
    def addr(self, ins: X.Instruction, with_seg=True) -> str:
        if ins.is_ip_rel_memory_operand:
            a = hx(ins.ip_rel_memory_address)
        else:
            parts = []
            if ins.memory_base != R.NONE:
                k, i, bits, _ = REG[ins.memory_base]
                parts.append(self.g(i) if bits == 64 else f"(u32){self.g(i)}")
            if ins.memory_index != R.NONE:
                k, i, bits, _ = REG[ins.memory_index]
                r = self.g(i) if bits == 64 else f"(u64)(u32){self.g(i)}"
                sc = ins.memory_index_scale
                parts.append(r if sc == 1 else f"{r}*{sc}")
            d = ins.memory_displacement
            f = self.absfix(d)
            if f is not None:
                d = f   # absolute address into an image that moved (synthesized relocations)
            if d or not parts:
                parts.append(hx(d))
            a = "(" + "+".join(parts) + ")"
        seg = ins.memory_segment
        if with_seg and seg == R.GS:
            a = f"(c->gs_base+{a})"
        elif with_seg and seg == R.FS:
            a = f"(c->fs_base+{a})"
        return a

    def _sfx(self, ins) -> str:
        """'S' (stack: plain, unordered access) when the address is rsp-based; '' otherwise (TSO-ordered)."""
        return "S" if ins.memory_base == R.RSP and ins.memory_segment not in (R.GS, R.FS) else ""

    def width(self, ins: X.Instruction, i: int) -> int:
        k = ins.op_kind(i)
        if k == K.REGISTER:
            return REG[ins.op_register(i)][2] if ins.op_register(i) in REG else 0
        if k == K.MEMORY:
            return X.MemorySizeExt.size(ins.memory_size) * 8
        return {K.IMMEDIATE8: 8, K.IMMEDIATE16: 16, K.IMMEDIATE32: 32, K.IMMEDIATE64: 64,
                K.IMMEDIATE8TO16: 16, K.IMMEDIATE8TO32: 32, K.IMMEDIATE8TO64: 64,
                K.IMMEDIATE32TO64: 64, K.IMMEDIATE8_2ND: 8}.get(k, 0)

    def rd(self, ins: X.Instruction, i: int, w: int | None = None) -> str:
        """Read operand i as an unsigned C expression of width w (default: operand's own width)."""
        k = ins.op_kind(i)
        w = w or self.width(ins, i)
        if k == K.REGISTER:
            reg = ins.op_register(i)
            if reg not in REG:
                raise Unsupported(f"reg {reg}")
            kind, idx, bits, hi = REG[reg]
            if kind == "x":
                return f"{self.x(idx)}.q[0]" if w == 64 else (f"{self.x(idx)}.d[0]" if w == 32 else self.x(idx))
            v = self.g(idx)
            if hi:
                return f"(u8)({v}>>8)"
            return v if w == 64 else f"({U[w]}){v}"
        if k == K.MEMORY:
            if w == 128:
                return f"LDX({self.addr(ins)})"
            return f"LD{self._sfx(ins)}{w}({self.addr(ins)})"
        v = ins.immediate(i)
        if w == 64 and ins.mnemonic in ABS_IMM_MNEMONICS:
            f = self.absfix_imm(v)
            if f is not None:
                v = f
        return f"({U[w]}){hx(v)}" if w < 64 else hx(v)

    def absfix_imm(self, v: int) -> int | None:
        """absfix for immediates, which are far more often plain numbers: only values pointing into code or
        initialized data count as addresses (a huge zero-filled .bss makes ordinary constants such as
        0x550000 land inside the image range)."""
        pe = self.prog.pe
        if not pe.abs_delta:
            return None
        u = v & 0xFFFFFFFFFFFFFFFF
        if not pe.abs_lo <= u < pe.abs_hi:
            return None
        rva = u - pe.abs_lo
        for sec in pe.sections:
            if sec.rva <= rva < sec.rva + min(sec.raw_size, sec.vsize or sec.raw_size):
                return self.absfix(v)
        return None

    def absfix(self, v: int) -> int | None:
        """An absolute address into this image as originally linked -> where the image now lives, or None.
        Only images moved with synthesized relocations (no .reloc) need this: their 32-bit absolute
        addresses in code can't be patched in place, so the generated C uses the 64-bit address."""
        pe = self.prog.pe
        if pe.abs_delta:
            u = v & 0xFFFFFFFFFFFFFFFF
            if pe.abs_lo <= u < pe.abs_hi:
                return (u + pe.abs_delta) & 0xFFFFFFFFFFFFFFFF
        return None

    def wr(self, ins: X.Instruction, i: int, val: str, w: int | None = None) -> str:
        k = ins.op_kind(i)
        w = w or self.width(ins, i)
        if k == K.REGISTER:
            reg = ins.op_register(i)
            if reg not in REG:
                raise Unsupported(f"reg {reg}")
            kind, idx, bits, hi = REG[reg]
            if kind == "x":
                if w == 128:
                    return f"{self.x(idx)} = {val};"
                raise Unsupported("partial xmm write via wr")
            v = self.g(idx)
            if bits == 64:
                return f"{v} = (u64)({val});"
            if bits == 32:
                return f"{v} = (u32)({val});"
            if bits == 16:
                return f"{v} = ({v} & ~0xFFFFull) | (u16)({val});"
            if hi:
                return f"{v} = ({v} & ~0xFF00ull) | ((u64)(u8)({val}) << 8);"
            return f"{v} = ({v} & ~0xFFull) | (u8)({val});"
        if k == K.MEMORY:
            if w == 128:
                return f"STX({self.addr(ins)}, {val});"
            return f"ST{self._sfx(ins)}{w}({self.addr(ins)}, ({U[w]})({val}));"
        raise Unsupported("write to immediate")

    # --- flag helpers ------------------------------------------------------
    def szp(self, res: str, w: int) -> str:
        return f"zf = ({res})==0; sf = ({res})>>{w - 1}&1; pf = PAR({res});"

    # --- function --------------------------------------------------------
    def lift(self):
        """Phase 1: lift every instruction (text with sync placeholders) and record its register facts."""
        if self.fn.addr not in self.fn.insns:  # seed that isn't decodable code (e.g. a data pointer into .text)
            self.stub = [f"void f_{self.fn.addr:x}(Ctx *c) {{ rt_trap(c, {hx(self.fn.addr)}, \"entry is not decodable code\"); }}", ""]
            return
        for start, insns in self.fn.blocks():
            self.out.append(f"L_{start:x}:;")
            self.cc_src = None
            recs = []
            for ins in insns:
                self.out.append(f"    /* {ins.ip:x}: {ins} */")
                if ins.op_count == 2 and ins.op0_kind == X.OpKind.REGISTER and ins.op0_register == X.Register.RBP and (
                        (ins.mnemonic == X.Mnemonic.MOV and ins.op1_kind == X.OpKind.REGISTER and ins.op1_register == X.Register.RSP)
                        or (ins.mnemonic == X.Mnemonic.LEA and ins.memory_base == X.Register.RSP)):
                    self.fp_frame = True   # rbp = frame pointer (see finalize)
                mark = len(self.out)
                self.refs = 0
                try:
                    self.lift_insn(ins)
                except Exception as e:  # Unsupported, or a lifter bug: trap at runtime instead of failing the build
                    name = MNEMONIC_NAME.get(ins.mnemonic, str(ins.mnemonic))
                    if not isinstance(e, Unsupported):
                        name = f"{name}(lifter error: {type(e).__name__} {e})"
                    self.unsupported[name] = self.unsupported.get(name, 0) + 1
                    del self.out[mark:]
                    self.cc_src = None
                    self.emit(f"{self.sync_out()} rt_unimplemented(c, {hx(ins.ip)}, \"{ins}\", {FLAGS_PACKED}); return;")
                rec = regsum.Rec(ins)
                r, w, k = regsum.insn_effects(ins)
                extra = self.refs & ~(r | w | k)   # touched by the lifter but not reported by iced: read + written
                rec.R, rec.W, rec.K = r | extra, w | extra, k
                rec.add_text("\n".join(self.out[mark:]), self.callsites)
                recs.append(rec)
            last = insns[-1]
            mark = len(self.out)
            if last.flow_control in (FC.NEXT, FC.CALL, FC.INDIRECT_CALL, FC.CONDITIONAL_BRANCH,
                                     FC.XBEGIN_XABORT_XEND):
                nxt = last.next_ip
                if nxt in self.fn.insns:
                    self.emit(f"goto L_{nxt:x};")
                elif nxt in self.fn.tail_calls:   # falls through into the next function (analysis stopped there)
                    self.emit(self.tail(nxt))
                else:
                    self.emit(f"{self.sync_out()} rt_fell_off(c, {hx(nxt)}, {FLAGS_PACKED}); return;")
            recs[-1].add_text("\n".join(self.out[mark:]), self.callsites)
            self.recblocks.append((start, recs))

    def pack(self):
        """Between the phases: keep the lifted text compressed (big programs lift to gigabytes of C)."""
        self.out_z = zlib.compress("\n".join(self.out).encode(), 1)
        self.out = None
        self.recblocks = None

    def facts(self) -> regsum.FuncFacts:
        roots = sorted(l for l in self.fn.leaders if l in self.fn.insns) if self.fn.eh else ()
        return regsum.FuncFacts(self.fn.addr, self.recblocks, conservative=self.conservative(), eh_roots=roots)

    def conservative(self) -> bool:
        """Full sync everywhere: undecodable entry, --no-callopt, or (EH_CONSERVATIVE) an EH frame record."""
        return self.stub is not None or not CALLOPT or (EH_CONSERVATIVE and self.fn.eh)

    # sync text for a register-slot mask, restricted to the locals this function has
    def _slots(self, mask: int):
        g = [i for i in sorted(self.used_g) if mask >> i & 1]
        x = [i for i in sorted(self.used_x) if mask >> (16 + i) & 1]
        h = [i for i in sorted(self.used_h) if mask >> (32 + i) & 1]
        return g, x, h

    def _store(self, mask: int) -> str:
        g, x, h = self._slots(mask)
        return " ".join([f"c->r[{i}]={LOCAL[i]};" for i in g] + [f"c->x[{i}]=x{i};" for i in x] + [f"c->xh[{i}]=h{i};" for i in h])

    def _load(self, mask: int) -> str:
        g, x, h = self._slots(mask)
        return " ".join([f"{LOCAL[i]}=c->r[{i}];" for i in g] + [f"x{i}=c->x[{i}];" for i in x] + [f"h{i}=c->xh[{i}];" for i in h])

    def finalize(self, NEED, MOD, MUST, ff: regsum.FuncFacts) -> list[str]:
        """Phase 2: header + body with every sync placeholder expanded."""
        if self.stub is not None:
            return self.stub
        if self.out is None:
            self.out = zlib.decompress(self.out_z).decode().split("\n")
        fn = self.fn
        cons = self.conservative()
        ALL = regsum.ALL
        # registers loaded at entry: everything live there, including the prologue's saves of callee-saved
        # registers (NEED, what callers provide, leaves those out: see regsum._preserved)
        entry = ALL if cons else ff.live_in(NEED, MOD, MUST)
        wb = ALL if cons else ff.WB
        head = [f"void f_{fn.addr:x}(Ctx *c) {{"]
        head.append("    u64 " + ", ".join(f"{LOCAL[i]}=c->r[{i}]" if entry >> i & 1 else f"{LOCAL[i]}=0"
                                         for i in sorted(self.used_g)) + ";")
        if self.used_x:
            head.append("    V128 " + ", ".join(f"x{i}=c->x[{i}]" if entry >> (16 + i) & 1 else f"x{i}={{0}}"
                                              for i in sorted(self.used_x)) + ";")
        if self.used_h:
            head.append("    V128 " + ", ".join(f"h{i}=c->xh[{i}]" if entry >> (32 + i) & 1 else f"h{i}={{0}}"
                                              for i in sorted(self.used_h)) + ";")
        head.append("    u8 cf=0, pf=0, af=0, zf=0, sf=0, of=0; (void)cf; (void)pf; (void)af; (void)zf; (void)sf; (void)of;")
        if fn.eh:
            # Resumable frame: unwinding into this frame longjmps here (registers come from Ctx).
            head.append("    RtEhFrame eh_ __attribute__((cleanup(rt_eh_pop)));")
            head.append(f"    rt_eh_push(&eh_, c, {hx(fn.addr)}, rsp);")
            head.append("    if (__builtin_expect(_setjmp(eh_.jb) != 0, 0)) { @SI@ c->resume_target = eh_.target; }")
        resumable = sorted(l for l in (fn.leaders if fn.eh else fn.entries) if l in fn.insns)
        # Entering at a label: the caller went through rt_call (full sync), so every register comes from Ctx.
        head.append("    if (__builtin_expect(c->resume_target != 0, 0)) {")
        head.append("        @SI@ u64 t_ = c->resume_target; c->resume_target = 0;")
        if resumable:
            cases = " ".join(f"case {hx(l)}: goto L_{l:x};" for l in resumable)
            head.append(f"        switch (t_) {{ {cases} default: break; }}")
        # No matching label: a resume request here would otherwise silently run the function from the top.
        head.append(f"        @SO@ rt_resume_elsewhere(c, t_, {hx(fn.addr)}); return;")
        head.append("    }")
        head.append(f"    goto L_{fn.addr:x};")
        so, si = self._sync_text()
        zu = " ".join(f"h{i}=(V128){{0}};" for i in sorted(self.used_h))
        za = " ".join(f"x{i}=(V128){{0}};" for i in sorted(self.used_x))
        body = "\n".join(head + self.out)
        # An exception may unwind into this frame: callee-saved registers this function changes must be in Ctx
        # at every call (see regsum.FuncFacts; the ones it doesn't change are dead at its landing pads).
        ehx = regsum.NONVOL & wb if fn.eh else 0
        # A frame-pointer function: unwinders find its frame through rbp (SEH UWOP_SET_FPREG, DWARF CFA = rbp + n, e.g.
        # after `and rsp,-16`), so rbp must be in Ctx at every call. Without this the guest unwinder (AnyPS5 libc.prx)
        # reads a stale rbp and loses the frame chain.
        if self.fp_frame:
            ehx |= 1 << 5
        # Registers whose Ctx copy this function (or a direct callee) may change before it returns normally (full
        # syncs that hand off to the interpreter or trap don't count: the interpreter runs the epilogue itself,
        # a trap doesn't come back).
        stored = ALL if cons or any("@SI@" in l for l in self.out) else regsum.RSP

        def cs_mask(n, tail):
            g = self.callsites[int(n)]
            return NEED.get(g, ALL) | (MOD.get(g, ALL) & ~MUST.get(g, 0)) | regsum.RSP | ehx | (wb if tail else 0)

        if not cons:
            for m in _CS_RE.finditer(body):   # what we store, and what the callee may change in Ctx
                stored |= cs_mask(m.group(1), False) | MOD.get(self.callsites[int(m.group(1))], ALL)
            for m in _TS_RE.finditer(body):
                stored |= cs_mask(m.group(1), True)
            if "@SOC@" in body:
                stored |= ALL if not regsum.ABI_SAVES else regsum.UNK_STORE | ehx
        body = body.replace("@SO@", so).replace("@SOT@", so).replace("@SI@", si).replace("@ZU@", zu).replace("@ZA@", za)
        # `ret` writes back what this function changed. A callee-saved register its epilogue restored (regsum P)
        # holds its entry value again, which is still what Ctx has unless this function stored it somewhere.
        ret = wb if cons else wb & ~(ff.P & ~stored)
        body = body.replace("@RET@", self._store(ret | regsum.RSP))
        # Calls the analysis can't see. Under the Win64 ABI the callee preserves callee-saved registers, so only
        # volatile ones go through Ctx (regsum.py explains why the unwinder doesn't need the others).
        if cons or not regsum.ABI_SAVES:
            body = body.replace("@SOC@", so).replace("@SIC@", si)
        else:
            body = body.replace("@SOC@", self._store(regsum.UNK_STORE | ehx)).replace("@SIC@", self._load(regsum.UNK_LOAD))

        def cr(m):
            g = self.callsites[int(m.group(1))]
            return si if cons else self._load(MOD.get(g, ALL) | regsum.RSP)

        body = _CS_RE.sub(lambda m: so if cons else self._store(cs_mask(m.group(1), False)), body)
        body = _CR_RE.sub(cr, body)
        body = _TS_RE.sub(lambda m: so if cons else self._store(cs_mask(m.group(1), True)), body)
        tail = ["}", ""]
        # DWARF landing pads (Itanium C++ ABI unwinders resume by jumping there): the runtime looks a jump target up
        # in this linker-collected table to find the frame record to longjmp to (runtime/seh.c rt_landing_pad).
        pads = sorted(l for l in resumable if l in getattr(self.prog, "dwarf_pads", ())) if fn.eh else []
        if pads:
            ents = ", ".join(f"{{{hx(l)}, {hx(fn.addr)}}}" for l in pads)
            tail.append(f'static u64 ehpad_{fn.addr:x}[][2] __attribute__((used, section("__DATA,__wtm_ehpad"))) = {{ {ents} }};')
            tail.append("")
        return body.split("\n") + tail

    # --- control flow ----------------------------------------------------
    def call_direct(self, target: int, ret: int) -> str:
        n = len(self.callsites); self.callsites.append(target)
        return f"rsp -= 8; STS64(rsp, {hx(ret)}); @CS{n}@ f_{target:x}(c); @CR{n}@"

    def tail(self, target: int) -> str:
        if target in self.prog.functions:
            n = len(self.callsites); self.callsites.append(target)
            return f"@TS{n}@ f_{target:x}(c); return;"
        return f"{self.sync_out()} rt_jump(c, {hx(target)}); return;"

    def lift_insn(self, ins: X.Instruction):
        m = ins.mnemonic
        name = MNEMONIC_NAME.get(m, "")
        # last flag producer in this block (cmp/sub/add/test/and/...), for fusing it with a following
        # jcc/setcc/cmovcc into one C comparison (see cond()); any other flag write forgets it
        self.prev_cc = self.cc_src
        if ins.rflags_modified:
            self.cc_src = None
        self.g(4)
        h = getattr(self, "i_" + name, None)
        if h is not None:
            h(ins)
            return
        if not self.try_vector(ins, name):
            raise Unsupported(name)

    # Branches
    def i_JMP(self, ins):
        if ins.op0_kind == K.NEAR_BRANCH64:
            t = ins.near_branch_target
            self.emit(f"goto L_{t:x};" if t in self.fn.insns and t not in self.fn.tail_calls else self.tail(t))
            return
        tv = self.rd(ins, 0, 64)
        jt = self.fn.jump_tables.get(ins.ip)
        tmp = self.t()
        self.emit(f"u64 {tmp} = {tv};")
        if jt:
            cases = []
            for tgt in sorted(set(jt.targets)):
                if tgt in self.fn.leaders and tgt in self.fn.insns and tgt not in self.fn.tail_calls:  # decoded block leaders only
                    cases.append(f"case {hx(tgt)}: goto L_{tgt:x};")
            self.emit(f"switch ({tmp}) {{ " + " ".join(cases) + " default: break; }")
        self.emit(f"{self.sync_out()} rt_jump(c, {tmp}); return;")

    def cond(self, ins) -> str:
        """C condition for a jcc/setcc/cmovcc. When the previous instruction is the flag producer (cmp, sub,
        test, and, ...), compare its operands directly (`(i32)a < (i32)b` instead of `sf != of` rebuilt from
        bits): clang emits one cmp + b.cond and the flag locals die. Otherwise read the flag locals."""
        cc = ins.condition_code
        src = self.prev_cc
        if src:
            kind, a, b, r, w = src
            C = X.ConditionCode
            sx = lambda v: f"({S[w]}){v}"
            if kind == "sub":
                e = {C.E: f"{a} == {b}", C.NE: f"{a} != {b}", C.B: f"{a} < {b}", C.AE: f"{a} >= {b}",
                     C.BE: f"{a} <= {b}", C.A: f"{a} > {b}", C.S: f"{sx(r)} < 0", C.NS: f"{sx(r)} >= 0",
                     C.L: f"{sx(a)} < {sx(b)}", C.GE: f"{sx(a)} >= {sx(b)}", C.LE: f"{sx(a)} <= {sx(b)}",
                     C.G: f"{sx(a)} > {sx(b)}"}.get(cc)
            elif kind == "logic":   # cf = of = 0
                e = {C.E: f"{r} == 0", C.NE: f"{r} != 0", C.S: f"{sx(r)} < 0", C.NS: f"{sx(r)} >= 0",
                     C.L: f"{sx(r)} < 0", C.GE: f"{sx(r)} >= 0", C.LE: f"{sx(r)} <= 0", C.G: f"{sx(r)} > 0",
                     C.BE: f"{r} == 0", C.A: f"{r} != 0", C.B: "0", C.AE: "1"}.get(cc)
            else:                   # add: only the result-based conditions
                e = {C.E: f"{r} == 0", C.NE: f"{r} != 0", C.S: f"{sx(r)} < 0", C.NS: f"{sx(r)} >= 0"}.get(cc)
            if e:
                return f"({e})"
        return CC[cc]

    def i_Jcc(self, ins):
        t = ins.near_branch_target
        cond = self.cond(ins)
        if t in self.fn.insns and t not in self.fn.tail_calls:
            self.emit(f"if ({cond}) goto L_{t:x};")
        else:
            self.emit(f"if ({cond}) {{ {self.tail(t)} }}")

    def _jcxz(self, ins, cond):
        self.g(1); t = ins.near_branch_target
        if t in self.fn.insns and t not in self.fn.tail_calls:
            self.emit(f"if ({cond}) goto L_{t:x};")
        else:   # outside this function, or not on an instruction boundary in it
            self.emit(f"if ({cond}) {{ {self.tail(t)} }}")

    def i_JRCXZ(self, ins): self._jcxz(ins, "rcx == 0")
    def i_JECXZ(self, ins): self._jcxz(ins, "(u32)rcx == 0")

    def i_CALL(self, ins):
        ret = ins.next_ip
        if ins.op0_kind == K.NEAR_BRANCH64:
            t = ins.near_branch_target
            if t in self.prog.functions:
                self.emit(self.call_direct(t, ret))
            else:
                self.emit(f"rsp -= 8; STS64(rsp, {hx(ret)}); @SOC@ rt_call(c, {hx(t)}); @SIC@")
            return
        tv = self.rd(ins, 0, 64)
        tmp = self.t()
        imp = self.prog.iat.get(ins.ip_rel_memory_address) if ins.is_ip_rel_memory_operand else None
        note = f" /* {imp[0]}!{imp[1]} */" if imp else ""
        # unknown callee: per-site inline cache (rt_icall in recomp.h); ABI-aware sync (@SOC@/@SIC@, see finalize)
        self.emit(f"{{ u64 {tmp} = {tv};{note} rsp -= 8; STS64(rsp, {hx(ret)}); @SOC@ {{ static const RtFunc *ic_; rt_icall(c, {tmp}, &ic_); }} @SIC@ }}")

    def i_RET(self, ins):
        extra = ins.immediate(0) if ins.op_count else 0
        self.emit(f"rsp += {8 + extra}; @RET@ return;")

    def i_INT3(self, ins):
        self.emit(f"@SOT@ rt_trap(c, {hx(ins.ip)}, \"int3\"); return;")

    def i_UD2(self, ins):
        self.emit(f"@SOT@ rt_trap(c, {hx(ins.ip)}, \"ud2\"); return;")

    def i_HLT(self, ins):
        self.emit(f"@SOT@ rt_trap(c, {hx(ins.ip)}, \"hlt\"); return;")

    def i_INT(self, ins):
        self.emit(f"@SOT@ rt_trap(c, {hx(ins.ip)}, \"int {ins.immediate(0)}\"); return;")

    def i_NOP(self, ins): pass
    # No CET shadow stack: with shadow stacks disabled RDSSP leaves its destination unchanged (MSVC's
    # vcruntime probes it this way). x87 exception flags aren't modelled, so FNCLEX/FCLEX have nothing to clear.
    def i_RDSSPQ(self, ins): pass
    def i_RDSSPD(self, ins): pass
    def i_FNCLEX(self, ins): pass
    def i_FCLEX(self, ins): pass
    def i_CLC(self, ins): self.emit("cf = 0;")
    def i_STC(self, ins): self.emit("cf = 1;")
    def i_CMC(self, ins): self.emit("cf = !cf;")
    def i_SAHF(self, ins):
        self.g(0); self.emit("{ u8 ah_ = (u8)(rax >> 8); sf = ah_ >> 7 & 1; zf = ah_ >> 6 & 1; af = ah_ >> 4 & 1; pf = ah_ >> 2 & 1; cf = ah_ & 1; }")
    def i_LAHF(self, ins):
        self.g(0); self.emit("rax = (rax & ~0xFF00ull) | ((u64)((sf << 7) | (zf << 6) | (af << 4) | (pf << 2) | 2 | cf) << 8);")
    def _loop(self, ins, cond):
        self.g(1); t = ins.near_branch_target
        go = f"goto L_{t:x};" if t in self.fn.insns and t not in self.fn.tail_calls else f"{{ {self.tail(t)} }}"
        self.emit(f"rcx--; if (rcx{cond}) {go}")
    def i_LOOP(self, ins): self._loop(ins, " != 0")
    def i_LOOPE(self, ins): self._loop(ins, " != 0 && zf")
    def i_LOOPNE(self, ins): self._loop(ins, " != 0 && !zf")
    def i_ENTER(self, ins):
        if ins.immediate(1) & 31:
            raise Unsupported("enter with nesting level")
        self.g(5); self.emit(f"rsp -= 8; STS64(rsp, rbp); rbp = rsp; rsp -= {ins.immediate(0) & 0xFFFF};")
    def i_PAUSE(self, ins): self.emit("__asm__ volatile(\"yield\");")
    def i_PREFETCHT0(self, ins): pass
    def i_PREFETCHT1(self, ins): pass
    def i_PREFETCHT2(self, ins): pass
    def i_PREFETCHNTA(self, ins): pass
    def i_PREFETCHW(self, ins): pass
    def i_LFENCE(self, ins): self.emit("__atomic_thread_fence(__ATOMIC_SEQ_CST);")
    def i_MFENCE(self, ins): self.emit("__atomic_thread_fence(__ATOMIC_SEQ_CST);")
    def i_SFENCE(self, ins): self.emit("__atomic_thread_fence(__ATOMIC_SEQ_CST);")
    def i_CLD(self, ins): pass  # DF assumed 0 (Win64 ABI requires it on call/return)
    def i_ENDBR64(self, ins): pass

    # --- data movement ---------------------------------------------------
    def i_MOV(self, ins):
        w = self.width(ins, 0)
        if ins.op0_kind == K.REGISTER and ins.op0_register in (R.ES, R.CS, R.SS, R.DS, R.FS, R.GS):
            raise Unsupported("segment mov")
        if w == 32 and ins.op0_kind == K.REGISTER and ins.op1_kind in (K.IMMEDIATE32,):
            f = self.absfix_imm(ins.immediate(1))
            if f is not None:   # `mov eax, offset sym` in a moved image: the pointer needs all 64 bits
                self.emit(f"{self.g(REG[ins.op0_register][1])} = {hx(f)};")
                return
        self.emit(self.wr(ins, 0, self.rd(ins, 1, w), w))

    def i_MOVABS(self, ins): self.i_MOV(ins)

    def i_MOVZX(self, ins):
        self.emit(self.wr(ins, 0, f"(u64){self.rd(ins, 1)}"))

    def i_MOVSX(self, ins):
        ws = self.width(ins, 1)
        self.emit(self.wr(ins, 0, f"(u64)(i64)({S[ws]}){self.rd(ins, 1)}"))

    def i_MOVSXD(self, ins): self.i_MOVSX(ins)

    def i_LEA(self, ins):
        self.emit(self.wr(ins, 0, self.addr(ins, with_seg=False)))

    def i_XCHG(self, ins):
        w = self.width(ins, 0)
        a, b = self.t(), self.t()
        mem, reg = (0, 1) if ins.op0_kind == K.MEMORY else (1, 0) if ins.op1_kind == K.MEMORY else (None, None)
        if mem is not None:  # implicitly locked
            self.emit(f"{U[w]} {a} = __atomic_exchange_n(({U[w]}*){self.addr(ins)}, {self.rd(ins, reg)}, __ATOMIC_SEQ_CST);")
            self.emit(self.wr(ins, reg, a))
            return
        self.emit(f"{U[w]} {a} = {self.rd(ins, 0)}; {U[w]} {b} = {self.rd(ins, 1)};")
        self.emit(self.wr(ins, 0, b)); self.emit(self.wr(ins, 1, a))

    def i_CMOVcc(self, ins):
        w = self.width(ins, 0)
        cond = self.cond(ins)
        cur = self.rd(ins, 0)
        src = self.rd(ins, 1)
        self.emit(self.wr(ins, 0, f"{cond} ? {src} : {cur}", w))

    def i_SETcc(self, ins):
        self.emit(self.wr(ins, 0, f"(u8)({self.cond(ins)})", 8))

    def i_PUSH(self, ins):
        v = self.rd(ins, 0, 64) if ins.op0_kind != K.REGISTER or self.width(ins, 0) == 64 else self.rd(ins, 0)
        tmp = self.t()
        w = self.width(ins, 0) if ins.op0_kind == K.REGISTER else 64
        sz = w // 8 if w in (16, 64) else 8
        self.emit(f"{{ u64 {tmp} = {v}; rsp -= {sz}; STS{sz * 8}(rsp, {tmp}); }}")

    def i_POP(self, ins):
        w = self.width(ins, 0)
        tmp = self.t()
        self.emit(f"{{ {U[w]} {tmp} = LDS{w}(rsp); rsp += {w // 8}; {self.wr(ins, 0, tmp)} }}")

    def i_PUSHFQ(self, ins):
        self.emit(f"rsp -= 8; STS64(rsp, {FLAGS_PACKED});")

    def i_POPFQ(self, ins):
        t = self.t()
        self.emit(f"{{ u64 {t} = LDS64(rsp); rsp += 8; cf={t}&1; pf={t}>>2&1; af={t}>>4&1; zf={t}>>6&1; sf={t}>>7&1; of={t}>>11&1; }}")

    def i_LEAVE(self, ins):
        self.emit("rsp = rbp; rbp = LDS64(rsp); rsp += 8;"); self.g(5)

    def i_BSWAP(self, ins):
        w = self.width(ins, 0)
        self.emit(self.wr(ins, 0, f"__builtin_bswap{w}({self.rd(ins, 0)})"))

    def i_CBW(self, ins): self.g(0); self.emit("rax = (rax & ~0xFFFFull) | (u16)(i16)(i8)rax;")
    def i_CWDE(self, ins): self.g(0); self.emit("rax = (u32)(i32)(i16)rax;")
    def i_CDQE(self, ins): self.g(0); self.emit("rax = (u64)(i64)(i32)rax;")
    def i_CWD(self, ins): self.g(0); self.g(2); self.emit("rdx = (rdx & ~0xFFFFull) | (u16)((i16)rax >> 15);")
    def i_CDQ(self, ins): self.g(0); self.g(2); self.emit("rdx = (u32)((i32)rax >> 31);")
    def i_CQO(self, ins): self.g(0); self.g(2); self.emit("rdx = (u64)((i64)rax >> 63);")

    # --- ALU --------------------------------------------------------------
    # Read-modify-write plumbing. For `lock`-prefixed memory destinations the update is a CAS loop, so the
    # RMW is atomic like on x86; `a` ends up holding the value the CAS actually replaced, and flags are
    # computed from it after the loop.
    # `lock add/sub/and/or/xor/inc/dec/not [mem]` map onto one LSE atomic (ldaddal/ldclral/ldsetal/ldeoral):
    # a CAS retry loop collapses under contention. `a` gets the old value, r = a op b as before.
    _FETCH = {"+": "__atomic_fetch_add", "-": "__atomic_fetch_sub", "&": "__atomic_fetch_and",
              "|": "__atomic_fetch_or", "^": "__atomic_fetch_xor"}

    def locked_fetch(self, ins, w, a, b, r, op) -> bool:
        if not (ins.has_lock_prefix and ins.op0_kind == K.MEMORY and op in self._FETCH):
            return False
        ut = U[w]
        self.emit(f"{ut} {a} = {self._FETCH[op]}(({ut}*){self.addr(ins)}, {b}, __ATOMIC_SEQ_CST), {r} = {a} {op} {b};")
        return True

    def rmw_begin(self, ins, w, a, r):
        ut = U[w]
        self.locked_p = None
        if ins.has_lock_prefix and ins.op0_kind == K.MEMORY:
            p = self.t()
            self.locked_p = p
            self.emit(f"u64 {p} = {self.addr(ins)}; {ut} {a} = __atomic_load_n(({ut}*){p}, __ATOMIC_RELAXED), {r};")
            self.emit("do {")
        else:
            self.emit(f"{ut} {a} = {self.rd(ins, 0)}, {r};")

    def rmw_end(self, ins, w, a, r):
        if self.locked_p:
            self.emit(f"}} while (!__atomic_compare_exchange_n(({U[w]}*){self.locked_p}, &{a}, {r}, 1, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));")
            self.locked_p = None
        else:
            self.emit(self.wr(ins, 0, r))

    def _arith(self, ins, op: str, write=True, carry_in=False):
        w = self.width(ins, 0)
        a, b, r = self.t(), self.t(), self.t()
        ut = U[w]
        ci = "cf" if carry_in else "0"
        self.emit(f"{ut} {b} = {self.rd(ins, 1, w)}; u8 ci_{r} = {ci};")
        if write and not carry_in and self.locked_fetch(ins, w, a, b, r, op):
            pass
        else:
            if write:
                self.rmw_begin(ins, w, a, r)
            else:
                self.emit(f"{ut} {a} = {self.rd(ins, 0)}, {r};")
            self.emit(f"{r} = {a} {op} {b} {op} ci_{r};")
            if write:
                self.rmw_end(ins, w, a, r)
        if not carry_in:
            self.cc_src = ("sub" if op == "-" else "add", a, b, r, w)
        if op == "+":
            self.emit(f"cf = ({r} < {a}) || (ci_{r} && {r} == {a});")
            self.emit(f"of = ((({a} ^ {r}) & ({b} ^ {r})) >> {w - 1}) & 1;")
        else:
            self.emit(f"cf = ({a} < {b}) || (ci_{r} && {a} == {b});")
            self.emit(f"of = ((({a} ^ {b}) & ({a} ^ {r})) >> {w - 1}) & 1;")
        self.emit(f"af = (({a} ^ {b} ^ {r}) >> 4) & 1; {self.szp(r, w)}")

    def i_ADD(self, ins): self._arith(ins, "+")
    def i_SUB(self, ins): self._arith(ins, "-")
    def i_ADC(self, ins): self._arith(ins, "+", carry_in=True)
    def i_SBB(self, ins): self._arith(ins, "-", carry_in=True)
    def i_CMP(self, ins): self._arith(ins, "-", write=False)

    def _logic(self, ins, op, write=True):
        w = self.width(ins, 0)
        a, b, r = self.t(), self.t(), self.t()
        self.emit(f"{U[w]} {b} = {self.rd(ins, 1, w)};")
        if write and self.locked_fetch(ins, w, a, b, r, op):
            pass
        elif write:
            self.rmw_begin(ins, w, a, r)
            self.emit(f"{r} = {a} {op} {b};")
            self.rmw_end(ins, w, a, r)
        else:
            self.emit(f"{U[w]} {r} = {self.rd(ins, 0)} {op} {b};")
        self.emit(f"cf = 0; of = 0; af = 0; {self.szp(r, w)}")
        self.cc_src = ("logic", a, b, r, w)

    def i_AND(self, ins): self._logic(ins, "&")
    def i_OR(self, ins): self._logic(ins, "|")
    def i_XOR(self, ins):
        if ins.op_kind(0) == K.REGISTER and ins.op_kind(1) == K.REGISTER and ins.op0_register == ins.op1_register:
            self.emit(self.wr(ins, 0, "0") + " cf = 0; of = 0; af = 0; zf = 1; sf = 0; pf = 1;")
            return
        self._logic(ins, "^")
    def i_TEST(self, ins): self._logic(ins, "&", write=False)

    def _unary(self, ins, expr, flags):
        w = self.width(ins, 0); a, r = self.t(), self.t()
        fetch = {"{a} + 1": ("+", "1"), "{a} - 1": ("-", "1"), "~{a}": ("^", f"({U[w]})~0ull")}.get(expr)
        if not (fetch and self.locked_fetch(ins, w, a, fetch[1], r, fetch[0])):
            self.rmw_begin(ins, w, a, r)
            self.emit(f"{r} = {expr.format(a=a, w=w, U=U[w])};")
            self.rmw_end(ins, w, a, r)
        if flags:
            self.emit(flags.format(a=a, r=r, w=w, w1=w - 1, U=U[w]) + " " + self.szp(r, w))

    def i_INC(self, ins): self._unary(ins, "{a} + 1", "of = {r} == ({U})(1ull << {w1}); af = ({r} & 0xF) == 0;")
    def i_DEC(self, ins): self._unary(ins, "{a} - 1", "of = {a} == ({U})(1ull << {w1}); af = ({a} & 0xF) == 0;")
    def i_NEG(self, ins): self._unary(ins, "-{a}", "cf = {a} != 0; of = {a} == ({U})(1ull << {w1}); af = ({a} & 0xF) != 0;")
    def i_NOT(self, ins): self._unary(ins, "~{a}", "")

    # --- multiply / divide --------------------------------------------------
    def i_IMUL(self, ins):
        if ins.op_count == 1:
            w = self.width(ins, 0)
            src = self.rd(ins, 0)
            self.g(0); self.g(2)
            p = self.t()
            if w == 8:
                self.emit(f"i16 {p} = (i16)(i8)rax * (i16)(i8){src}; rax = (rax & ~0xFFFFull) | (u16){p};")
                self.emit(f"cf = of = {p} != (i16)(i8){p};")
            else:
                wide = {16: "i32", 32: "i64", 64: "i128"}[w]
                self.emit(f"{wide} {p} = ({wide})({S[w]})rax * ({wide})({S[w]}){src};")
                lo = f"({U[w]}){p}"
                hi = f"({U[w]})({p} >> {w})"
                self.emit(self._set_part(0, w, lo) + " " + self._set_part(2, w, hi))
                self.emit(f"cf = of = {p} != ({wide})({S[w]}){p};")
            return
        w = self.width(ins, 0)
        a = self.rd(ins, 1) if ins.op_count == 3 else self.rd(ins, 0)
        b = self.rd(ins, 2, w) if ins.op_count == 3 else self.rd(ins, 1, w)
        wide = {16: "i32", 32: "i64", 64: "i128"}[w]
        p = self.t()
        self.emit(f"{wide} {p} = ({wide})({S[w]}){a} * ({wide})({S[w]}){b};")
        self.emit(f"cf = of = {p} != ({wide})({S[w]}){p}; {self.szp(f'({U[w]}){p}', w)}")
        self.emit(self.wr(ins, 0, f"({U[w]}){p}"))

    def _set_part(self, reg: int, w: int, val: str) -> str:
        v = self.g(reg)
        if w == 64:
            return f"{v} = {val};"
        if w == 32:
            return f"{v} = (u32)({val});"
        return f"{v} = ({v} & ~{MASK[w]}) | ({U[w]})({val});"

    def i_MUL(self, ins):
        w = self.width(ins, 0); src = self.rd(ins, 0); p = self.t()
        self.g(0); self.g(2)
        if w == 8:
            self.emit(f"u16 {p} = (u16)(u8)rax * (u16){src}; rax = (rax & ~0xFFFFull) | {p}; cf = of = ({p} >> 8) != 0;")
            return
        wide = {16: "u32", 32: "u64", 64: "u128"}[w]
        self.emit(f"{wide} {p} = ({wide})({U[w]})rax * ({wide}){src};")
        self.emit(self._set_part(0, w, f"({U[w]}){p}") + " " + self._set_part(2, w, f"({U[w]})({p} >> {w})"))
        self.emit(f"cf = of = ({p} >> {w}) != 0;")

    def _div(self, ins, signed: bool):
        w = self.width(ins, 0); d = self.t(); n = self.t(); q = self.t()
        self.g(0); self.g(2)
        T = (S if signed else U)
        if w == 8:
            self.emit(f"{T[8]} {d} = ({T[8]}){self.rd(ins, 0)}; if (!{d}) {{ @SOT@ rt_trap(c, {hx(ins.ip)}, \"#DE\"); return; }}")
            self.emit(f"{T[16]} {n} = ({T[16]})(u16)rax; {T[16]} {q} = {n} / {d};")
            self.emit(f"rax = (rax & ~0xFFFFull) | (u8){q} | ((u16)(u8)({n} % {d}) << 8);")
            return
        wide = {16: T[32], 32: T[64], 64: T[128]}[w]
        uwide = {16: "u32", 32: "u64", 64: "u128"}[w]
        self.emit(f"{T[w]} {d} = ({T[w]}){self.rd(ins, 0)}; if (!{d}) {{ @SOT@ rt_trap(c, {hx(ins.ip)}, \"#DE\"); return; }}")
        self.emit(f"{wide} {n} = ({wide})((({uwide})({U[w]})rdx << {w}) | ({uwide})({U[w]})rax);")
        self.emit(f"{wide} {q} = {n} / {d};")
        self.emit(self._set_part(2, w, f"({U[w]})({n} % {d})") + " " + self._set_part(0, w, f"({U[w]}){q}"))

    def i_DIV(self, ins): self._div(ins, False)
    def i_IDIV(self, ins): self._div(ins, True)

    # --- shifts / rotates -------------------------------------------------
    def _shift(self, ins, kind):
        w = self.width(ins, 0)
        a, n, r = self.t(), self.t(), self.t()
        cnt = self.rd(ins, 1, 8) if ins.op_count > 1 else "1"
        mask = 63 if w == 64 else 31
        self.emit(f"{U[w]} {a} = {self.rd(ins, 0)}; unsigned {n} = ({cnt}) & {mask};")
        self.emit(f"if ({n}) {{")
        if kind == "shl":
            self.emit(f"  {U[w]} {r} = ({U[w]})((u64){a} << {n}); cf = ({n} <= {w}) ? (({a} >> ({w} - {n})) & 1) : 0;")
            self.emit(f"  of = (({r} >> {w - 1}) & 1) ^ cf;")
        elif kind == "shr":
            self.emit(f"  {U[w]} {r} = ({n} < {w}) ? ({U[w]})({a} >> {n}) : 0; cf = ({a} >> ({n} - 1)) & 1;")
            self.emit(f"  of = ({a} >> {w - 1}) & 1;")
        elif kind == "sar":
            self.emit(f"  {U[w]} {r} = ({U[w]})(({S[w]}){a} >> ({n} < {w} ? {n} : {w - 1})); cf = (({S[w]}){a} >> ({n} - 1 < {w} ? {n} - 1 : {w - 1})) & 1;")
            self.emit("  of = 0;")
        elif kind in ("rol", "ror"):
            self.emit(f"  unsigned m = {n} % {w};")
            if kind == "rol":
                self.emit(f"  {U[w]} {r} = m ? ({U[w]})(({a} << m) | ({a} >> ({w} - m))) : {a}; cf = {r} & 1; of = (({r} >> {w - 1}) & 1) ^ cf;")
            else:
                self.emit(f"  {U[w]} {r} = m ? ({U[w]})(({a} >> m) | ({a} << ({w} - m))) : {a}; cf = ({r} >> {w - 1}) & 1; of = cf ^ (({r} >> {w - 2}) & 1);")
            self.emit(f"  {self.wr(ins, 0, r)}")
            self.emit("}")
            return
        self.emit(f"  {self.szp(r, w)}")
        self.emit(f"  {self.wr(ins, 0, r)}")
        self.emit("}" + (f" else {{ {self.wr(ins, 0, a)} }}" if w == 32 and ins.op0_kind == K.REGISTER else ""))

    def i_SHL(self, ins): self._shift(ins, "shl")
    def i_SAL(self, ins): self._shift(ins, "shl")
    def i_SHR(self, ins): self._shift(ins, "shr")
    def i_SAR(self, ins): self._shift(ins, "sar")
    def i_ROL(self, ins): self._shift(ins, "rol")
    def i_ROR(self, ins): self._shift(ins, "ror")

    def _shxd(self, ins, left):
        w = self.width(ins, 0)
        if w == 16:
            raise Unsupported("shld/shrd 16")
        a, b, n, r = self.t(), self.t(), self.t(), self.t()
        mask = 63 if w == 64 else 31
        self.emit(f"{U[w]} {a} = {self.rd(ins, 0)}; {U[w]} {b} = {self.rd(ins, 1)}; unsigned {n} = ({self.rd(ins, 2, 8)}) & {mask};")
        if left:
            self.emit(f"if ({n}) {{ {U[w]} {r} = ({a} << {n}) | ({b} >> ({w} - {n})); cf = ({a} >> ({w} - {n})) & 1; of = (({r} ^ {a}) >> {w - 1}) & 1; {self.szp(r, w)} {self.wr(ins, 0, r)} }}")
        else:
            self.emit(f"if ({n}) {{ {U[w]} {r} = ({a} >> {n}) | ({b} << ({w} - {n})); cf = ({a} >> ({n} - 1)) & 1; of = (({r} ^ {a}) >> {w - 1}) & 1; {self.szp(r, w)} {self.wr(ins, 0, r)} }}")

    def i_SHLD(self, ins): self._shxd(ins, True)
    def i_SHRD(self, ins): self._shxd(ins, False)

    # --- BMI-ish / bit ops -------------------------------------------------
    def _bt(self, ins, op):
        w = self.width(ins, 0)
        ut = U[w]
        a, n, p = self.t(), self.t(), self.t()
        if ins.op0_kind == K.MEMORY:
            if ins.op1_kind == K.REGISTER:  # bit offset is signed and may reach outside the operand
                off = self.t()
                self.emit(f"i64 {off} = ({S[w]}){self.rd(ins, 1, w)}; u64 {p} = {self.addr(ins)} + (u64)(({off} >> {w.bit_length() - 1}) * {w // 8}); unsigned {n} = (unsigned)({off} & {w - 1});")
            else:
                self.emit(f"u64 {p} = {self.addr(ins)}; unsigned {n} = (unsigned)({self.rd(ins, 1, 8)}) & {w - 1};")
            if op and ins.has_lock_prefix:
                fn = {"s": "__atomic_fetch_or", "r": "__atomic_fetch_and", "c": "__atomic_fetch_xor"}[op]
                m = f"(({ut})1 << {n})" if op != "r" else f"~(({ut})1 << {n})"
                self.emit(f"{ut} {a} = {fn}(({ut}*){p}, {m}, __ATOMIC_SEQ_CST); cf = ({a} >> {n}) & 1;")
                return
            self.emit(f"{ut} {a} = LD{w}({p}); cf = ({a} >> {n}) & 1;")
            if op:
                expr = {"s": f"{a} | (({ut})1 << {n})", "r": f"{a} & ~(({ut})1 << {n})", "c": f"{a} ^ (({ut})1 << {n})"}[op]
                self.emit(f"ST{w}({p}, ({ut})({expr}));")
            return
        self.emit(f"{ut} {a} = {self.rd(ins, 0)}; unsigned {n} = ({self.rd(ins, 1, w)}) & {w - 1}; cf = ({a} >> {n}) & 1;")
        if op:
            expr = {"s": f"{a} | (({ut})1 << {n})", "r": f"{a} & ~(({ut})1 << {n})", "c": f"{a} ^ (({ut})1 << {n})"}[op]
            self.emit(self.wr(ins, 0, expr))

    def i_BT(self, ins): self._bt(ins, None)
    def i_BTS(self, ins): self._bt(ins, "s")
    def i_BTR(self, ins): self._bt(ins, "r")
    def i_BTC(self, ins): self._bt(ins, "c")

    def i_BSF(self, ins):
        w = self.width(ins, 0); s = self.t()
        self.emit(f"{U[w]} {s} = {self.rd(ins, 1)}; zf = {s} == 0; if ({s}) {{ {self.wr(ins, 0, f'__builtin_ctzll({s})')} }}")

    def i_BSR(self, ins):
        w = self.width(ins, 0); s = self.t()
        self.emit(f"{U[w]} {s} = {self.rd(ins, 1)}; zf = {s} == 0; if ({s}) {{ {self.wr(ins, 0, f'63 - __builtin_clzll((u64){s})')} }}")

    def i_TZCNT(self, ins):
        w = self.width(ins, 0); s = self.t()
        self.emit(f"{U[w]} {s} = {self.rd(ins, 1)}; cf = {s} == 0; {self.wr(ins, 0, f'{s} ? __builtin_ctzll({s}) : {w}')} zf = {s} & 1;")

    def i_LZCNT(self, ins):
        w = self.width(ins, 0); s, r = self.t(), self.t()
        self.emit(f"{U[w]} {s} = {self.rd(ins, 1)}; {U[w]} {r} = {s} ? __builtin_clzll((u64){s}) - {64 - w} : {w}; cf = {s} == 0; zf = {r} == 0; {self.wr(ins, 0, r)}")

    def i_POPCNT(self, ins):
        w = self.width(ins, 0); s = self.t()
        self.emit(f"{U[w]} {s} = {self.rd(ins, 1)}; {self.wr(ins, 0, f'__builtin_popcountll({s})')} zf = {s} == 0; cf = of = sf = pf = af = 0;")

    # --- atomics ----------------------------------------------------------
    def i_CMPXCHG(self, ins):
        w = self.width(ins, 0); cur, exp = self.t(), self.t()
        self.g(0)
        if ins.op0_kind == K.MEMORY:
            self.emit(f"{U[w]} {exp} = ({U[w]})rax; {U[w]} {cur} = {exp};")
            self.emit(f"zf = __atomic_compare_exchange_n(({U[w]}*){self.addr(ins)}, &{cur}, {self.rd(ins, 1)}, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);")
        else:
            self.emit(f"{U[w]} {exp} = ({U[w]})rax; {U[w]} {cur} = {self.rd(ins, 0)}; zf = {cur} == {exp};")
            self.emit(f"if (zf) {{ {self.wr(ins, 0, self.rd(ins, 1))} }}")
        self.emit(f"if (!zf) {{ {self._set_part(0, w, cur)} }}")
        self.emit(f"cf = {exp} < {cur}; sf = (({U[w]})({exp} - {cur})) >> {w - 1} & 1;")

    def i_CMPXCHG16B(self, ins):
        self.g(0); self.g(1); self.g(2); self.g(3)
        e = self.t()
        self.emit(f"{{ u128 {e} = ((u128)rdx << 64) | rax; zf = __atomic_compare_exchange_n((u128*){self.addr(ins)}, &{e}, ((u128)rcx << 64) | rbx, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);")
        self.emit(f"  if (!zf) {{ rax = (u64){e}; rdx = (u64)({e} >> 64); }} }}")

    def i_XADD(self, ins):
        w = self.width(ins, 0); old, src, r = self.t(), self.t(), self.t()
        self.emit(f"{U[w]} {src} = {self.rd(ins, 1)};")
        if ins.op0_kind == K.MEMORY:
            self.emit(f"{U[w]} {old} = __atomic_fetch_add(({U[w]}*){self.addr(ins)}, {src}, __ATOMIC_SEQ_CST);")
        else:
            self.emit(f"{U[w]} {old} = {self.rd(ins, 0)}; {self.wr(ins, 0, f'{old} + {src}')}")
        self.emit(f"{U[w]} {r} = {old} + {src}; cf = {r} < {old}; of = ((({old} ^ {r}) & ({src} ^ {r})) >> {w - 1}) & 1; {self.szp(r, w)}")
        self.emit(self.wr(ins, 1, old))

    # --- string ops -------------------------------------------------------
    def _rep(self, ins, body: str):
        self.g(1); self.g(6); self.g(7)
        if ins.has_rep_prefix:
            self.emit(f"while (rcx) {{ {body} rcx--; }}")
        else:
            self.emit(body)

    def i_MOVSB(self, ins): self._movs(ins, 1)
    def i_MOVSW(self, ins): self._movs(ins, 2)
    def i_MOVSQ(self, ins): self._movs(ins, 8)
    def _movs(self, ins, n):
        if ins.has_rep_prefix:
            self.g(1); self.g(6); self.g(7)
            self.emit(f"memmove((void*)rdi, (void*)rsi, rcx * {n}); rdi += rcx * {n}; rsi += rcx * {n}; rcx = 0;")
        else:
            self._rep(ins, f"ST{n * 8}(rdi, LD{n * 8}(rsi)); rdi += {n}; rsi += {n};")

    def i_MOVSD(self, ins):
        if ins.op_count == 0 or ins.op0_kind in (K.MEMORY_ESRDI,):
            return self._movs(ins, 4)
        self.v_MOVSD(ins)  # SSE scalar double move

    def _stos(self, ins, n):
        self.g(0); self.g(1); self.g(7)
        v = f"({U[n * 8]})rax"
        if ins.has_rep_prefix:
            if n == 1:
                self.emit("memset((void*)rdi, (u8)rax, rcx); rdi += rcx; rcx = 0;")
            else:
                self.emit(f"while (rcx) {{ ST{n * 8}(rdi, {v}); rdi += {n}; rcx--; }}")
        else:
            self.emit(f"ST{n * 8}(rdi, {v}); rdi += {n};")

    def i_STOSB(self, ins): self._stos(ins, 1)
    def i_STOSW(self, ins): self._stos(ins, 2)
    def i_STOSD(self, ins): self._stos(ins, 4)
    def i_STOSQ(self, ins): self._stos(ins, 8)

    # cmps / scas: flags as for CMP of [rsi] - [rdi] (cmps) or acc - [rdi] (scas); DF assumed 0.
    def _strcmp_flags(self, a, b, w):
        r = self.t()
        return (f"{U[w]} {r} = ({U[w]})({a} - {b}); cf = {a} < {b}; of = (((({a}) ^ ({b})) & (({a}) ^ {r})) >> {w - 1}) & 1; "
                f"af = ((({a}) ^ ({b}) ^ {r}) >> 4) & 1; {self.szp(r, w)}")

    def _cmps(self, ins, n):
        self.g(1); self.g(6); self.g(7)
        w = n * 8; a, b = self.t(), self.t()
        body = (f"{U[w]} {a} = LD{w}(rsi), {b} = LD{w}(rdi); " + self._strcmp_flags(a, b, w) + f" rsi += {n}; rdi += {n};")
        if ins.has_repe_prefix:
            self.emit(f"while (rcx) {{ {body} rcx--; if (!zf) break; }}")
        elif ins.has_repne_prefix:
            self.emit(f"while (rcx) {{ {body} rcx--; if (zf) break; }}")
        else:
            self.emit(f"{{ {body} }}")

    def _scas(self, ins, n):
        self.g(0); self.g(1); self.g(7)
        w = n * 8; a, b = self.t(), self.t()
        body = f"{U[w]} {a} = ({U[w]})rax, {b} = LD{w}(rdi); " + self._strcmp_flags(a, b, w) + f" rdi += {n};"
        if ins.has_repe_prefix:
            self.emit(f"while (rcx) {{ {body} rcx--; if (!zf) break; }}")
        elif ins.has_repne_prefix:
            self.emit(f"while (rcx) {{ {body} rcx--; if (zf) break; }}")
        else:
            self.emit(f"{{ {body} }}")

    def i_CMPSB(self, ins): self._cmps(ins, 1)
    def i_CMPSW(self, ins): self._cmps(ins, 2)
    def i_CMPSQ(self, ins): self._cmps(ins, 8)
    def i_CMPSD(self, ins):   # string cmpsd vs SSE2 cmpsd (scalar double compare)
        if ins.op_count == 0 or ins.op0_kind in (K.MEMORY_SEG_RSI, K.MEMORY_SEG_ESI, K.MEMORY_ESRDI):
            return self._cmps(ins, 4)
        if not self.try_vector(ins, "CMPSD"):
            raise Unsupported("cmpsd")
    def i_SCASB(self, ins): self._scas(ins, 1)
    def i_SCASW(self, ins): self._scas(ins, 2)
    def i_SCASD(self, ins): self._scas(ins, 4)
    def i_SCASQ(self, ins): self._scas(ins, 8)

    # rotate through carry: done bit-by-bit on a (w+1)-bit value; counts are tiny in practice
    def _rcx(self, ins, left):
        w = self.width(ins, 0)
        a, n = self.t(), self.t()
        cnt = self.rd(ins, 1, 8) if ins.op_count > 1 else "1"
        mod = {8: 9, 16: 17}.get(w)
        self.emit(f"{U[w]} {a} = {self.rd(ins, 0)}; unsigned {n} = ({cnt}) & {63 if w == 64 else 31};" + (f" {n} %= {mod};" if mod else ""))
        if left:
            self.emit(f"for (unsigned i_ = 0; i_ < {n}; i_++) {{ u8 c_ = ({a} >> {w - 1}) & 1; {a} = ({U[w]})(({a} << 1) | cf); cf = c_; }}")
            self.emit(f"if ({n} == 1) of = (({a} >> {w - 1}) & 1) ^ cf;")
        else:
            self.emit(f"if ({n} == 1) of = (({a} >> {w - 1}) & 1) ^ cf;")
            self.emit(f"for (unsigned i_ = 0; i_ < {n}; i_++) {{ u8 c_ = {a} & 1; {a} = ({U[w]})(({a} >> 1) | ((({U[w]})cf) << {w - 1})); cf = c_; }}")
        self.emit(self.wr(ins, 0, a))

    def i_RCL(self, ins): self._rcx(ins, True)
    def i_RCR(self, ins): self._rcx(ins, False)
    def i_WAIT(self, ins): pass   # fwait: x87 exceptions are masked

    # SSE4.2 packed string compares (implemented by rt_pcmpstr in recomp.h)
    def _pcmpstr(self, ins, explicit, index):
        a = self.x(self.vreg(ins, 0)[0])
        bsrc = self.lane_src(ins, 1, 0)
        imm = self.last_imm(ins)
        b = self.t(); res = self.t(); f = self.t()
        self.g(0); self.g(2)
        la = "(i64)(i32)rax" if explicit else "-1"
        lb = "(i64)(i32)rdx" if explicit else "-1"
        self.emit(f"V128 {b} = {bsrc}; u8 {f}[4]; V128 {res} = rt_pcmpstr({a}, {b}, {la}, {lb}, {imm}, {int(index)}, {f});")
        self.emit(f"cf = {f}[0]; zf = {f}[1]; sf = {f}[2]; of = {f}[3]; af = pf = 0;")
        if index:
            self.g(1); self.emit(f"rcx = {res}.d[0];")
        else:
            self.emit(f"{self.x(0)} = {res};")
            if self.is_vex(ins):
                self.emit(f"{self.hh(0)} = (V128){{0}};")

    def i_PCMPISTRI(self, ins): self._pcmpstr(ins, False, True)
    def i_PCMPESTRI(self, ins): self._pcmpstr(ins, True, True)
    def i_PCMPISTRM(self, ins): self._pcmpstr(ins, False, False)
    def i_PCMPESTRM(self, ins): self._pcmpstr(ins, True, False)
    i_VPCMPISTRI = i_PCMPISTRI
    i_VPCMPESTRI = i_PCMPESTRI
    i_VPCMPISTRM = i_PCMPISTRM
    i_VPCMPESTRM = i_PCMPESTRM

    # --- x87 (values computed as doubles; 80-bit memory format honoured) -----------
    def _fmem_load(self, ins):
        w = X.MemorySizeExt.size(ins.memory_size) * 8
        a = self.addr(ins)
        if ins.mnemonic in (M.FILD, M.FIADD, M.FIMUL, M.FISUB, M.FISUBR, M.FIDIV, M.FIDIVR, M.FICOM, M.FICOMP):
            return f"(double)({S[w]})LD{w}({a})"
        return {32: f"(double)LDF32({a})", 64: f"LDF64({a})", 80: f"LDF80({a})"}[w]

    def _sti(self, ins, i):
        return f"ST_(c, {ins.op_register(i) - R.ST0})"

    def i_FLD(self, ins):
        if ins.op0_kind == K.MEMORY:
            self.emit(f"FPUSH(c, {self._fmem_load(ins)});")
        else:
            t = self.t(); self.emit(f"{{ double {t} = {self._sti(ins, 0)}; FPUSH(c, {t}); }}")
    i_FILD = i_FLD

    def _fstore(self, ins, pop, integer=False, trunc=False):
        if ins.op0_kind == K.MEMORY:
            w = X.MemorySizeExt.size(ins.memory_size) * 8
            a = self.addr(ins)
            v = "ST_(c, 0)"
            if integer:
                r = f"__builtin_trunc({v})" if trunc else f"__builtin_rint({v})"
                self.emit(f"ST{w}({a}, ({U[w]})({S[w]}){r});")
            else:
                self.emit({32: f"{{ float f_ = (float){v}; ST32({a}, *(u32*)&f_); }}",
                           64: f"{{ double d_ = {v}; ST64({a}, *(u64*)&d_); }}",
                           80: f"STF80({a}, {v});"}[w])
        else:
            self.emit(f"{self._sti(ins, 0)} = ST_(c, 0);")
        if pop:
            self.emit("(void)FPOP(c);")

    def i_FST(self, ins): self._fstore(ins, False)
    def i_FSTP(self, ins): self._fstore(ins, True)
    def i_FIST(self, ins): self._fstore(ins, False, integer=True)
    def i_FISTP(self, ins): self._fstore(ins, True, integer=True)
    def i_FISTTP(self, ins): self._fstore(ins, True, integer=True, trunc=True)
    def i_FLDZ(self, ins): self.emit("FPUSH(c, 0.0);")
    def i_FLD1(self, ins): self.emit("FPUSH(c, 1.0);")
    def i_FLDPI(self, ins): self.emit("FPUSH(c, 3.14159265358979323846);")
    def i_FLDL2E(self, ins): self.emit("FPUSH(c, 1.44269504088896340736);")
    def i_FLDLN2(self, ins): self.emit("FPUSH(c, 0.69314718055994530942);")
    def i_FCHS(self, ins): self.emit("ST_(c, 0) = -ST_(c, 0);")
    def i_FABS(self, ins): self.emit("ST_(c, 0) = __builtin_fabs(ST_(c, 0));")
    def i_FSQRT(self, ins): self.emit("ST_(c, 0) = __builtin_sqrt(ST_(c, 0));")
    def i_FRNDINT(self, ins): self.emit("ST_(c, 0) = __builtin_rint(ST_(c, 0));")
    def i_FXCH(self, ins):
        i = self._sti(ins, 0) if ins.op_count else "ST_(c, 1)"
        self.emit(f"{{ double t_ = ST_(c, 0); ST_(c, 0) = {i}; {i} = t_; }}")
    def i_FFREE(self, ins): pass
    def i_FINCSTP(self, ins): self.emit("c->ftop = (c->ftop + 1) & 7;")
    def i_FDECSTP(self, ins): self.emit("c->ftop = (c->ftop - 1) & 7;")

    def _farith(self, ins, op, rev=False, pop=False):
        ops = {"+": "{a} + {b}", "-": "{a} - {b}", "*": "{a} * {b}", "/": "{a} / {b}"}
        if ins.op_count == 0:      # fxxxp with implied st(1), st(0)
            dst, src = "ST_(c, 1)", "ST_(c, 0)"
        elif ins.op_count == 1:    # memory operand: st0 = st0 op m
            dst, src = "ST_(c, 0)", self._fmem_load(ins) if ins.op0_kind == K.MEMORY else self._sti(ins, 0)
        else:
            dst, src = self._sti(ins, 0), self._sti(ins, 1)
        a, b = (src, dst) if rev else (dst, src)
        self.emit(f"{dst} = " + ops[op].format(a=a, b=b) + ";")
        if pop:
            self.emit("(void)FPOP(c);")

    def i_FADD(self, ins): self._farith(ins, "+")
    def i_FADDP(self, ins): self._farith(ins, "+", pop=True)
    def i_FIADD(self, ins): self._farith(ins, "+")
    def i_FISUB(self, ins): self._farith(ins, "-")
    def i_FISUBR(self, ins): self._farith(ins, "-", rev=True)
    def i_FIDIV(self, ins): self._farith(ins, "/")
    def i_FIDIVR(self, ins): self._farith(ins, "/", rev=True)
    def i_FMUL(self, ins): self._farith(ins, "*")
    def i_FMULP(self, ins): self._farith(ins, "*", pop=True)
    def i_FIMUL(self, ins): self._farith(ins, "*")
    def i_FSUB(self, ins): self._farith(ins, "-")
    def i_FSUBP(self, ins): self._farith(ins, "-", pop=True)
    def i_FSUBR(self, ins): self._farith(ins, "-", rev=True)
    def i_FSUBRP(self, ins): self._farith(ins, "-", rev=True, pop=True)
    def i_FDIV(self, ins): self._farith(ins, "/")
    def i_FDIVP(self, ins): self._farith(ins, "/", pop=True)
    def i_FDIVR(self, ins): self._farith(ins, "/", rev=True)
    def i_FDIVRP(self, ins): self._farith(ins, "/", rev=True, pop=True)

    def _fcomi(self, ins, pop):
        b = self._sti(ins, 1) if ins.op_count == 2 else self._sti(ins, 0)
        self.emit(f"{{ double a_ = ST_(c, 0), b_ = {b}; if (a_ != a_ || b_ != b_) {{ zf = pf = cf = 1; }} else {{ zf = a_ == b_; pf = 0; cf = a_ < b_; }} of = sf = af = 0; }}")
        if pop:
            self.emit("(void)FPOP(c);")

    def i_FCOMI(self, ins): self._fcomi(ins, False)
    def i_FUCOMI(self, ins): self._fcomi(ins, False)
    def i_FCOMIP(self, ins): self._fcomi(ins, True)
    def i_FUCOMIP(self, ins): self._fcomi(ins, True)

    def i_FNSTSW(self, ins):
        self.emit(self.wr(ins, 0, "(u16)(c->fsw & ~0x3800) | (u16)(c->ftop << 11)", 16))
    def i_FNSTENV(self, ins): pass
    def i_FNINIT(self, ins): self.emit("c->ftop = 0; c->fcw = 0x27f; c->fsw = 0;")
    def i_FWAIT(self, ins): pass
    def i_FNSTCW(self, ins): self.emit(f"ST16({self.addr(ins)}, c->fcw);")
    def i_FLDCW(self, ins): self.emit(f"c->fcw = LD16({self.addr(ins)});")
    def i_FLDENV(self, ins): pass

    # x87 compares that report through the status word (C0 = bit 8, C2 = bit 10, C3 = bit 14)
    def _fcom(self, ins, pops):
        if ins.op_count == 0:
            b = "ST_(c, 1)"
        elif ins.op0_kind == K.MEMORY:
            b = self._fmem_load(ins)
        else:
            b = self._sti(ins, 1) if ins.op_count == 2 else self._sti(ins, 0)
        self.emit(f"{{ double a_ = ST_(c, 0), b_ = {b}; u16 cc_ = (a_ != a_ || b_ != b_) ? 0x4500 : a_ < b_ ? 0x0100 : a_ == b_ ? 0x4000 : 0; c->fsw = (c->fsw & ~0x4700) | cc_; }}")
        for _ in range(pops):
            self.emit("(void)FPOP(c);")

    def i_FCOM(self, ins): self._fcom(ins, 0)
    def i_FCOMP(self, ins): self._fcom(ins, 1)
    def i_FCOMPP(self, ins): self._fcom(ins, 2)
    def i_FUCOM(self, ins): self._fcom(ins, 0)
    def i_FUCOMP(self, ins): self._fcom(ins, 1)
    def i_FUCOMPP(self, ins): self._fcom(ins, 2)
    def i_FICOM(self, ins): self._fcom(ins, 0)
    def i_FICOMP(self, ins): self._fcom(ins, 1)
    def i_FTST(self, ins):
        self.emit("{ double a_ = ST_(c, 0); u16 cc_ = a_ != a_ ? 0x4500 : a_ < 0 ? 0x0100 : a_ == 0 ? 0x4000 : 0; c->fsw = (c->fsw & ~0x4700) | cc_; }")
    def i_FXAM(self, ins):
        self.emit("{ double a_ = ST_(c, 0); u16 cc_ = a_ != a_ ? 0x0100 : __builtin_isinf(a_) ? 0x0500 : a_ == 0 ? 0x4000 : "
                  "__builtin_fpclassify(0, 0, 0, 0x4400, 0x4000, a_) ? 0x4400 : 0x0400; if (__builtin_signbit(a_)) cc_ |= 0x0200; c->fsw = (c->fsw & ~0x4700) | cc_; }")

    # x87 transcendental / remainder ops (mingw's libm implements fmod, sin, pow, ... with these)
    def i_FPREM(self, ins): self.emit("ST_(c, 0) = __builtin_fmod(ST_(c, 0), ST_(c, 1)); c->fsw &= ~0x4700;")    # C2=0: reduction complete
    def i_FPREM1(self, ins): self.emit("ST_(c, 0) = __builtin_remainder(ST_(c, 0), ST_(c, 1)); c->fsw &= ~0x4700;")
    def i_FSIN(self, ins): self.emit("ST_(c, 0) = __builtin_sin(ST_(c, 0)); c->fsw &= ~0x0400;")
    def i_FCOS(self, ins): self.emit("ST_(c, 0) = __builtin_cos(ST_(c, 0)); c->fsw &= ~0x0400;")
    def i_FSINCOS(self, ins): self.emit("{ double x_ = ST_(c, 0); ST_(c, 0) = __builtin_sin(x_); FPUSH(c, __builtin_cos(x_)); c->fsw &= ~0x0400; }")
    def i_FPTAN(self, ins): self.emit("ST_(c, 0) = __builtin_tan(ST_(c, 0)); FPUSH(c, 1.0); c->fsw &= ~0x0400;")
    def i_FPATAN(self, ins): self.emit("ST_(c, 1) = __builtin_atan2(ST_(c, 1), ST_(c, 0)); (void)FPOP(c);")
    def i_F2XM1(self, ins): self.emit("ST_(c, 0) = __builtin_exp2(ST_(c, 0)) - 1.0;")
    def i_FYL2X(self, ins): self.emit("ST_(c, 1) = ST_(c, 1) * __builtin_log2(ST_(c, 0)); (void)FPOP(c);")
    def i_FYL2XP1(self, ins): self.emit("ST_(c, 1) = ST_(c, 1) * __builtin_log2(ST_(c, 0) + 1.0); (void)FPOP(c);")
    def i_FSCALE(self, ins): self.emit("ST_(c, 0) = __builtin_ldexp(ST_(c, 0), (int)__builtin_trunc(ST_(c, 1)));")
    def i_FXTRACT(self, ins): self.emit("{ double x_ = ST_(c, 0); int e_; double m_ = __builtin_frexp(x_, &e_); ST_(c, 0) = (double)(e_ - 1); FPUSH(c, m_ * 2.0); }")

    def i_RDRAND(self, ins):
        self.emit(self.wr(ins, 0, "rt_random64()") + " cf = 1; of = sf = zf = af = pf = 0;")
    i_RDSEED = i_RDRAND

    def _lods(self, ins, n):
        self.g(0); self.g(6)
        if ins.has_rep_prefix:
            raise Unsupported("rep lods")
        self.emit(self._set_part(0, n * 8, f"LD{n * 8}(rsi)") + f" rsi += {n};")
    def i_LODSB(self, ins): self._lods(ins, 1)
    def i_LODSW(self, ins): self._lods(ins, 2)
    def i_LODSD(self, ins): self._lods(ins, 4)
    def i_LODSQ(self, ins): self._lods(ins, 8)

    # --- misc -------------------------------------------------------------
    def i_CPUID(self, ins):
        self.g(0); self.g(1); self.g(2); self.g(3)
        self.emit(f"{self.sync_out()} rt_cpuid(c); {self.sync_in()}")

    def i_RDTSC(self, ins):
        self.g(0); self.g(2)
        self.emit("{ u64 t = rt_rdtsc(); rax = (u32)t; rdx = t >> 32; }")

    def i_XGETBV(self, ins):
        self.g(0); self.g(2)
        self.emit("rax = 7; rdx = 0;")  # x87|SSE|AVX state enabled

    def i_STMXCSR(self, ins):
        self.emit(f"ST32({self.addr(ins)}, c->mxcsr);")

    def i_LDMXCSR(self, ins):
        self.emit(f"c->mxcsr = LD32({self.addr(ins)});")


_CS_RE = re.compile(r"@CS(\d+)@")
_CR_RE = re.compile(r"@CR(\d+)@")
_TS_RE = re.compile(r"@TS(\d+)@")
_PROGS: dict[int, dict] = {}   # id(prog) -> {"lifters", "facts", "NEED", "MOD", "MUST"}


# Jcc / CMOVcc / SETcc all route through generic handlers keyed off condition codes. Class attributes, not
# per-instance ones: 48 bound methods per lifter cost >1 GB across Godot's 259K functions kept between phases.
for _cc in ("JO JNO JB JAE JE JNE JBE JA JS JNS JP JNP JL JGE JLE JG").split():
    setattr(FuncLifter, "i_" + _cc, FuncLifter.i_Jcc)
for _cc in ("O NO B AE E NE BE A S NS P NP L GE LE G").split():
    setattr(FuncLifter, "i_CMOV" + _cc, FuncLifter.i_CMOVcc)
    setattr(FuncLifter, "i_SET" + _cc, FuncLifter.i_SETcc)


def _new_lifter(prog: Program, fn: Function) -> FuncLifter:
    fl = FuncLifter(prog, fn)
    dec = fn.materialize()          # decoded instructions only while lifting this function (analysis.CompactInsns)
    try:
        fl.lift()
    finally:
        if dec is not None:
            fn.compact(dec)
    return fl


def lift_function(prog: Program, fn: Function):
    """Lift one function. The first call for a program lifts all of its functions and computes the
    interprocedural register summaries (regsum.py) that decide what each direct call syncs."""
    st = _PROGS.get(id(prog))
    if st is None or st["prog"] is not prog:
        lifters, facts = {}, {}
        for a, f in prog.functions.items():   # phase 1: lift, keep compact facts + compressed text
            fl = lifters[a] = _new_lifter(prog, f)
            facts[a] = fl.facts()
            fl.pack()
        NEED, MOD, MUST = regsum.summarize(facts)
        st = _PROGS[id(prog)] = {"prog": prog, "lifters": lifters, "facts": facts, "NEED": NEED, "MOD": MOD, "MUST": MUST}
    fl = st["lifters"].pop(fn.addr, None) or _new_lifter(prog, fn)
    lines = fl.finalize(st["NEED"], st["MOD"], st["MUST"], st["facts"].get(fn.addr) or fl.facts())
    if not st["lifters"]:
        del _PROGS[id(prog)]
    return lines, fl.unsupported
