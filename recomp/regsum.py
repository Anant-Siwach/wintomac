"""Interprocedural register summaries: which guest registers a direct call really has to pass through Ctx.

Generated functions keep guest registers in C locals and exchange them with callers through `Ctx`. Syncing
every register around every call is correct but costs ~50 memory operations per call. With a summary per
function we only sync what matters (all sets are bitmasks over 48 "register slots": GPR 0-15, xmm 16-31,
ymm upper halves 32-47):

  NEED(f)  registers whose value in Ctx must be correct when f is entered (upward-exposed reads, including
           those of its callees). The caller stores these before the call.
  MOD(f)   registers whose Ctx value f may change. The caller reloads these after the call; every other
           local stays valid (the callee provably left that register alone).
  MUST(f)  registers f definitely writes (fresh value) on every return path. A caller needs to pre-store a
           MOD register only if it is not in MUST: otherwise an unchanged register would be reloaded stale.

So a direct call to g reads NEED(g) | (MOD(g) & ~MUST(g)) and defines MOD(g). Calls the analysis can't see
(indirect calls, imports) and hand-offs to the interpreter keep the full sync of every local.

The per-instruction facts come from iced-x86's register-usage info, cross-checked with the registers the
lifter actually referenced while emitting the instruction (any register the lifter touched that iced doesn't
report is treated as read and written). Locals that are not loaded at entry are zero-initialised, so the
value-independent idioms iced counts as pure writes (`xor eax,eax`, `pxor x,x`, `pcmpeqd x,x`) stay exact.

Win64 ABI (ABI_SAVES): callee-saved registers (rbx rbp rsi rdi r12-r15 xmm6-15) that a function saves in its
prologue and reloads in every epilogue are not in its MOD, and code we can't see (indirect callees, the
interpreter) is assumed to preserve them -- which the guest's own caller code relies on anyway.
"""
from __future__ import annotations

import pickle
import re

import iced_x86 as X

NSLOT = 48
ALL = (1 << NSLOT) - 1
RSP = 1 << 4
NONVOL = sum(1 << i for i in (3, 5, 6, 7, 12, 13, 14, 15)) | sum(1 << (16 + i) for i in range(6, 16))
VOLATILE = ALL & ~NONVOL
# Calls we can't see may also enter System V ABI code (AnyPS5's relinked PS5 programs and the sysv_abi functions of
# its libraries, docs/ps5.md): there rsi/rdi carry the first two arguments (xmm6/xmm7 float arguments 7-8) and are
# not preserved. So unknown calls also store rsi, rdi, xmm6, xmm7 and reload rsi, rdi.
UNK_STORE = VOLATILE | (1 << 6) | (1 << 7) | (1 << (16 + 6)) | (1 << (16 + 7))
UNK_LOAD = VOLATILE | (1 << 6) | (1 << 7)

# See the module docstring and _preserved(). False: pushes are reads and pops writes like any other access,
# and anything the analysis can't see may change any register.
ABI_SAVES = True

_OA = X.OpAccess
_INFO = X.InstructionInfoFactory()

# iced register -> (slot mask, kills-on-write): kills-on-write means a plain WRITE replaces the whole slot(s)
_REGMAP: dict[int, tuple[int, bool]] = {}


def _init_regmap(REG):
    for reg, (kind, idx, bits, hi) in REG.items():
        if kind == "g":
            _REGMAP[reg] = (1 << idx, bits >= 32 and not hi)
    for i in range(16):
        _REGMAP[getattr(X.Register, f"XMM{i}")] = (1 << (16 + i), True)
        _REGMAP[getattr(X.Register, f"YMM{i}")] = ((1 << (16 + i)) | (1 << (32 + i)), True)
        _REGMAP[getattr(X.Register, f"ZMM{i}")] = ((1 << (16 + i)) | (1 << (32 + i)), True)


def insn_effects(ins: X.Instruction):
    """(reads, may_writes, kills) of one instruction per iced."""
    r = w = k = 0
    for u in _INFO.info(ins).used_registers():
        m = _REGMAP.get(u.register)
        if not m:
            continue
        mask, full = m
        a = u.access
        if a == _OA.READ or a == _OA.COND_READ:
            r |= mask
        elif a == _OA.WRITE and full:
            w |= mask; k |= mask
        elif a == _OA.NONE:
            continue
        else:   # partial write, read-write, conditional write
            r |= mask; w |= mask
    return r, w, k & ~RSP


_GOTO = re.compile(r"goto L_([0-9a-f]+);")
_CS = re.compile(r"@CS(\d+)@")
_TS = re.compile(r"@TS(\d+)@")


class Rec:
    """Facts about one lifted instruction (plus any block-ending text attached to it)."""
    __slots__ = ("R", "W", "K", "S", "E", "ins", "gotos", "calls", "tails", "ret", "handoff", "unknown", "fullcall", "trap")

    def __init__(self, ins=None):
        self.ins = ins
        self.R = self.W = self.K = 0
        self.S = 0   # prologue save of a callee-saved register: this read only feeds the unwinder / epilogue
        self.E = 0   # extra stores before a call in a function with an EH frame (see FuncFacts eh=True)
        self.gotos: list[int] = []
        self.calls: list[int] = []   # direct-call targets
        self.tails: list[int] = []   # tail-call targets
        self.ret = False             # `ret`: writes back and returns
        self.handoff = False         # full sync, then the interpreter / rt_jump runs the rest (an exit)
        self.unknown = False         # a call the analysis can't see (rt_call / rt_icall)
        self.fullcall = False        # full sync both ways around a runtime helper (cpuid)
        self.trap = False            # full sync, then rt_trap (does not return)

    def add_text(self, text: str, callsites: list[int]):
        if "@" not in text and "goto" not in text:
            return
        self.gotos += [int(t, 16) for t in _GOTO.findall(text)]
        self.calls += [callsites[int(n)] for n in _CS.findall(text)]
        self.tails += [callsites[int(n)] for n in _TS.findall(text)]
        self.ret |= "@RET@" in text
        if "@SIC@" in text:
            self.unknown = True       # unknown callee: Win64 ABI call (volatile registers synced)
        if "@SI@" in text:
            self.fullcall = True      # full sync both ways around a runtime helper (cpuid)
        elif "@SO@" in text:
            self.handoff = True
        self.trap |= "@SOT@" in text


class FuncFacts:
    """CFG + per-instruction facts of one function, and its liveness solver."""

    def __init__(self, addr: int, blocks: list[tuple[int, list[Rec]]], conservative: bool, eh_roots=()):
        """eh_roots: labels unwinding can resume at (function with an EH frame record). The resume path reloads
        every local from Ctx, so before every call (where an exception may start) the callee-saved locals are
        stored too (Rec.E): volatile registers are dead at a landing pad per the ABI, callee-saved ones come
        back from the unwinder, which takes them from Ctx unless a frame below saved them."""
        self.addr = addr
        self.conservative = conservative
        self.eh_roots = [r for r in eh_roots if r != addr]
        bmap = dict(blocks)
        for _, recs in blocks:
            for rec in recs:
                rec.E = 1 if eh_roots and (rec.calls or rec.unknown or rec.tails) else 0
        # keep only blocks reachable from the entry and the landing pads (other resume labels are entered
        # from rt_call, which syncs everything)
        seen, stack = set(), ([addr] if addr in bmap else []) + self.eh_roots
        while stack:
            b = stack.pop()
            if b in seen or b not in bmap:
                continue
            seen.add(b)
            for rec in bmap[b]:
                stack.extend(rec.gotos)
        blocks = [(s, recs) for s, recs in blocks if s in seen]
        self.WB = RSP          # registers this function's own code may write: written back at `ret`
        self.calls: set[int] = set()
        self.tails: set[int] = set()
        self.opaque = False    # has unknown calls or interpreter hand-offs
        for _, recs in blocks:
            for rec in recs:
                self.WB |= rec.W
                self.calls.update(rec.calls)
                self.tails.update(rec.tails)
                self.opaque |= rec.handoff or rec.unknown or rec.fullcall
        self.callees = self.calls | self.tails
        for _, recs in blocks:   # callee-saved registers this function changes (the lifter stores the same)
            for rec in recs:
                if rec.E:
                    rec.E = NONVOL & self.WB & ~rec.R
        self.P = 0 if conservative or not ABI_SAVES else _preserved(self.addr, blocks)
        # The Rec objects can go now. The compacted ops are kept pickled: as Python tuples and ints they cost ~9x
        # more, which for Godot (33M instructions) meant ~4 GB in the parent plus each lifting worker's share.
        self.nblocks = len(blocks)
        self._packed = pickle.dumps([(s, _compact(recs)) for s, recs in blocks], protocol=5)

    @property
    def blocks(self) -> list[tuple[int, list[tuple]]]:
        return pickle.loads(self._packed)

    def mod_local(self) -> int:
        if self.conservative:
            return ALL
        if self.opaque:
            return (self.WB | UNK_LOAD) & ~self.P if ABI_SAVES else ALL
        return self.WB & ~self.P

    def must(self, MUST) -> int:
        """Registers definitely given a fresh value on every way out (forward must-analysis)."""
        if self.conservative or not self.nblocks:
            return 0
        blocks = self.blocks
        bin_ = {s: ALL for s, _ in blocks}
        bin_[self.addr] = 0
        for r in self.eh_roots:     # a landing pad starts with nothing freshly defined
            if r in bin_:
                bin_[r] = 0
        exits = None
        changed = True
        while changed:
            changed = False
            exits = ALL
            for s, ops in blocks:
                cur = bin_[s]
                for op in ops:     # forward: the call happens first, branches (incl. fallthrough) last
                    if len(op) == 3:
                        cur |= op[0]
                        continue
                    K, _, _, gotos, calls, tails, fl = op
                    for g in calls:
                        cur |= MUST.get(g, 0)
                    cur |= K
                    for t in tails:
                        exits &= cur | MUST.get(t, 0)
                    if fl & F_HANDOFF:
                        exits &= cur
                    for t in gotos:
                        if t in bin_ and bin_[t] & ~cur:
                            bin_[t] &= cur
                            changed = True
                    if fl & F_RET:
                        exits &= cur
        return exits & ~RSP if exits is not None else 0

    def live_in(self, NEED, MOD, MUST, export=False) -> int:
        """Backward liveness; returns the registers live at the entry.
        export=False: what the function itself must load at entry (every local a full sync could store).
        export=True: the NEED summary its callers see. Under ABI_SAVES, prologue saves of callee-saved
        registers and full syncs (whose callees preserve callee-saved registers) don't make those registers
        something the caller has to provide (see _preserved for why the unwinder is still fine)."""
        if self.conservative:
            return ALL
        abi = export and ABI_SAVES
        gi = 2 if abi else 1
        full = VOLATILE if abi else ALL
        unk = UNK_STORE if ABI_SAVES else ALL
        blocks = self.blocks
        bin_ = {s: 0 for s, _ in blocks}
        ret_reads = self.WB | RSP
        changed = True
        while changed:
            changed = False
            for s, ops in reversed(blocks):
                live = 0
                for op in reversed(ops):   # backward: branches (incl. fallthrough), then the call, then the insn
                    if len(op) == 3:
                        live = (live & ~op[0]) | op[gi]
                        continue
                    K, _, _, gotos, calls, tails, fl = op
                    for t in gotos:
                        live |= bin_.get(t, 0)
                    for g in tails:
                        live |= NEED.get(g, ALL) | (MOD.get(g, ALL) & ~MUST.get(g, 0)) | ret_reads
                    if fl & F_RET:
                        live |= ret_reads
                    for g in calls:
                        m = MOD.get(g, ALL)
                        live = (live & ~m) | NEED.get(g, ALL) | (m & ~MUST.get(g, 0)) | RSP
                    if fl & F_UNKNOWN:  # stores the volatile registers (all of them without ABI_SAVES)
                        live |= unk
                    if fl & F_FULL:     # stores every local
                        live |= full
                    live = (live & ~K) | op[gi]
                if live != bin_[s]:
                    bin_[s] = live
                    changed = True
        return bin_.get(self.addr, ALL) | RSP


F_RET, F_UNKNOWN, F_FULL, F_HANDOFF = 1, 2, 4, 8


def _compact(recs: list[Rec]) -> list[tuple]:
    """A block as a short list of ops: runs of plain instructions composed into one (K, G_load, G_export)
    transfer, and (K, R_load, R_export, gotos, calls, tails, flags) for instructions with control effects.
    Keeps the analysis small for programs with millions of instructions."""
    ops: list[tuple] = []
    run: list[Rec] = []

    def flush():
        if run:
            k = gl = ge = 0
            for r in reversed(run):
                gl = (gl & ~r.K) | r.R | r.E
                ge = (ge & ~r.K) | (r.R & ~r.S)
                k |= r.K
            ops.append((k, gl, ge))
            run.clear()

    for r in recs:
        fl = (F_RET if r.ret else 0) | (F_UNKNOWN if r.unknown else 0) | \
             (F_FULL if r.handoff or r.fullcall or r.trap else 0) | (F_HANDOFF if r.handoff else 0)
        if not (fl or r.gotos or r.calls or r.tails):
            run.append(r)
            continue
        flush()
        ops.append((r.K, r.R | r.E, r.R & ~r.S, tuple(r.gotos), tuple(r.calls), tuple(r.tails), fl))
    flush()
    return ops


def summarize(facts: dict[int, FuncFacts]):
    """Whole-program NEED / MOD / MUST. Functions not in `facts` are unknown (everything)."""
    # MOD: transitive closure of local writes over direct and tail calls
    MOD = {a: f.mod_local() for a, f in facts.items()}
    changed = True
    while changed:
        changed = False
        for a, f in facts.items():
            m = MOD[a]
            for g in f.calls:    # whatever a callee does to a register f restores in its epilogue is undone
                m |= MOD.get(g, ALL) & ~f.P
            for g in f.tails:    # ... but a tail callee runs after the epilogue
                m |= MOD.get(g, ALL)
            if m != MOD[a]:
                MOD[a] = m
                changed = True
    # MUST: grows from nothing; every intermediate result is a sound under-approximation
    MUST: dict[int, int] = {a: 0 for a in facts}
    for _ in range(3):
        changed = False
        for a, f in facts.items():
            # A callee-saved register the epilogue restores (P) counts as written by its restore, but `ret` may
            # skip writing it back (lifter.finalize): Ctx keeps the caller's copy, so it must not be in MUST, or
            # a caller would skip storing it and reload a stale value (SDL3.dll: rbx saved to the home slot,
            # one exit a tail call whose callee is allowed to change rbx -> rbx in MOD).
            m = f.must(MUST) & MOD[a] & ~f.P
            if m != MUST[a]:
                MUST[a] = m
                changed = True
        if not changed:
            break
    # NEED: worklist to a fixpoint (monotone)
    callers: dict[int, set[int]] = {a: set() for a in facts}
    for a, f in facts.items():
        for g in f.callees:
            if g in callers:
                callers[g].add(a)
    NEED = {a: 0 for a in facts}
    # callees before callers (DFS postorder), so most functions are solved once
    order, seen = [], set()
    for root in facts:
        if root in seen:
            continue
        seen.add(root)
        stack = [(root, iter(facts[root].callees))]
        while stack:
            a, it = stack[-1]
            for g in it:
                if g in facts and g not in seen:
                    seen.add(g)
                    stack.append((g, iter(facts[g].callees)))
                    break
            else:
                stack.pop()
                order.append(a)
    work = order[::-1]   # pop() takes from the end: callees first
    inq = set(work)
    while work:
        a = work.pop()
        inq.discard(a)
        n = facts[a].live_in(NEED, MOD, MUST, export=True)
        if n != NEED[a]:
            NEED[a] = n
            for c in callers[a]:
                if c not in inq:
                    inq.add(c)
                    work.append(c)
    return NEED, MOD, MUST


# ---- Win64 callee-saved registers ---------------------------------------------------------------------
# A register the prologue saves to the stack and every epilogue reloads leaves the function with the value it
# came in with, so it is not in MOD, and the save is not a read the caller has to provide: the saved copy only
# matters to the unwinder, which restores a callee-saved register for the catching frame from the outermost
# frame below it that saved it. That frame read it from Ctx, where the catching frame (full sync: it has an
# EH frame record) left the right value, and the frames in between don't touch it, per the ABI. The epilogue
# shape is checked on every exit; the stack slot holding the saved value is trusted (it is on any Windows
# compiler's output).
_M = X.Mnemonic
_VMOV = {_M.MOVAPS, _M.MOVUPS, _M.MOVDQA, _M.MOVDQU, _M.MOVAPD, _M.MOVUPD,
         _M.VMOVAPS, _M.VMOVUPS, _M.VMOVDQA, _M.VMOVDQU, _M.VMOVAPD, _M.VMOVUPD}
_FRAME_BASES = {X.Register.RSP, X.Register.RBP, X.Register.R11, X.Register.RAX}


def _reg_slot(reg) -> int:
    m = _REGMAP.get(reg)
    return m[0] if m else 0


def _stack_mem(ins) -> bool:
    return ins.memory_base in _FRAME_BASES and ins.memory_index == X.Register.NONE


def _save_of(ins) -> int:
    """Slot saved by a prologue store of a callee-saved register, else 0."""
    K = X.OpKind
    m = ins.mnemonic
    if m == _M.PUSH and ins.op0_kind == K.REGISTER and X.RegisterExt.size(ins.op0_register) == 8:
        return _reg_slot(ins.op0_register) & NONVOL
    if ins.op_count == 2 and ins.op0_kind == K.MEMORY and ins.op1_kind == K.REGISTER and _stack_mem(ins):
        if m == _M.MOV and X.RegisterExt.size(ins.op1_register) == 8:
            return _reg_slot(ins.op1_register) & NONVOL
        if m in _VMOV and X.RegisterExt.is_xmm(ins.op1_register):
            return _reg_slot(ins.op1_register) & NONVOL
    return 0


def _restore_of(ins):
    """(slot restored, is epilogue-shaped) for an instruction scanned backwards from a function exit."""
    K = X.OpKind
    m = ins.mnemonic
    if m in (_M.RET, _M.JMP):
        return 0, True
    if m == _M.POP and ins.op0_kind == K.REGISTER and X.RegisterExt.size(ins.op0_register) == 8:
        return _reg_slot(ins.op0_register) & NONVOL, True
    if m == _M.LEAVE:
        return 1 << 5, True
    if ins.op_count == 2 and ins.op0_kind == K.REGISTER and ins.op1_kind == K.MEMORY and _stack_mem(ins):
        if m == _M.MOV and X.RegisterExt.size(ins.op0_register) == 8:
            return _reg_slot(ins.op0_register) & NONVOL, True
        if m in _VMOV and X.RegisterExt.is_xmm(ins.op0_register):
            return _reg_slot(ins.op0_register) & NONVOL, True
    if ins.op_count >= 1 and ins.op0_kind == K.REGISTER and ins.op0_register == X.Register.RSP and \
            m in (_M.ADD, _M.LEA, _M.MOV, _M.SUB):
        return 0, True
    return 0, False


def _preserved(entry: int, blocks) -> int:
    """Callee-saved registers saved in the entry block and restored by the epilogue of every ret / tail-call
    exit. Marks the saving instructions (Rec.S). Interpreter hand-offs run this same code to one of these exits."""
    bmap = dict(blocks)
    if entry not in bmap:
        return 0
    saved = written = 0
    for rec in bmap[entry]:
        if rec.ins is None:
            return 0
        sv = _save_of(rec.ins) & ~written
        if sv:
            saved |= sv
            rec.S = sv
        written |= rec.W & ~sv
    if not saved:
        return 0
    restored_all, nexits = saved, 0
    for _, recs in blocks:
        if not any(r.ret or r.tails for r in recs):
            continue
        nexits += 1
        last = recs[-1]
        got = 0
        if (last.ret or last.tails) and not any(r.ret or r.tails for r in recs[:-1]):
            for rec in reversed(recs):
                if rec.ins is None:
                    break
                rs, ok = _restore_of(rec.ins)
                if not ok:
                    break
                got |= rs
        restored_all &= got      # an exit in the middle of a block, or no epilogue: nothing restored
    if not nexits:
        restored_all = 0
    for rec in bmap[entry]:
        rec.S &= restored_all    # saves of registers some exit doesn't restore are ordinary reads
    return restored_all
