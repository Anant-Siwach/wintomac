// wintomac runtime JIT: x86-64 -> arm64 translation of traces (integer ISA, control flow, lazy flags).
//
// Semantics follow the interpreter (runtime/interp.c) instruction for instruction, so `make interp-fuzz` with
// WINTOMAC_JIT_THRESHOLD=0 checks the translator against Rosetta 2 through the same cases. Techniques borrowed
// from box64's dynarec (MIT; flag deferral per instruction, native-flag fusion for cmp+jcc, LSE for lock ops),
// QEMU TCG (lazy condition codes: op + operands, compute on demand; chaining by patching jumps) and FEX
// (fixed guest->host register mapping across blocks).
#include "jit.h"
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

static ZydisDecoder dec;
static pthread_once_t dec_once = PTHREAD_ONCE_INIT;
static void dec_init(void) { ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64); }

#define A (&jc->a)
#define OPN(j, i) (&(j)->op[i])
#define ISREG(o) ((o)->type == ZYDIS_OPERAND_TYPE_REGISTER)
#define ISMEM(o) ((o)->type == ZYDIS_OPERAND_TYPE_MEMORY)
#define ISIMM(o) ((o)->type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
#define NV(j) ((j)->in.operand_count_visible)
#define MN(j) ((j)->in.mnemonic)
#define NEXT(j) ((j)->pc + (j)->in.length)

static inline int szc(int w) { return w == 8 ? 0 : w == 16 ? 1 : w == 32 ? 2 : 3; }
static inline int is_gpr(const ZydisDecodedOperand *o) { return ISREG(o) && rt_ireg[o->reg.value].kind == RK_GPR; }
static inline int gidx(const ZydisDecodedOperand *o) { return rt_ireg[o->reg.value].idx; }
static inline int ghi(const ZydisDecodedOperand *o) { return rt_ireg[o->reg.value].hi; }
static inline int gpr_mem_imm(const ZydisDecodedOperand *o) { return is_gpr(o) || ISMEM(o) || ISIMM(o); }

// ---- exits -------------------------------------------------------------------------------------------------
static JExit *add_exit(JComp *jc, int kind, u64 target, u32 *branch) {
    if (jc->nexit >= JMAX_EXIT) { jc->fail = 1; return &jc->exits[0]; }
    JExit *e = &jc->exits[jc->nexit++];
    memset(e, 0, sizeof *e);
    e->kind = kind; e->target = target; e->branch = branch;
    e->xdirty = jc->xdirty;
    return e;
}
void jit_xflush(JComp *jc) {
    for (int i = 0; i < 16; i++) if (jc->xdirty >> i & 1) a64_fldst_uimm(A, 4, 0, 16 + i, RCTX, (u32)CTX_X(i));
    jc->xdirty = 0;
}
void jit_xload(JComp *jc, u16 mask) {
    for (int i = 0; i < 16; i++) if (mask >> i & 1) a64_fldst_uimm(A, 4, 1, 16 + i, RCTX, (u32)CTX_X(i));
}
// conditional branch back to the trace start (a loop): straight there unless xmm registers must be written back
static void loop_branch(JComp *jc, u32 insn) {
    u32 *at = a64_here(A);
    a64_put(A, insn);
    if (jc->loopL) {   // loop trace: the gate expects loopL cached
        u16 need = jc->loopL & ~jc->xcached;
        if (!need) { a64_patch_cbz(at, jc->gate); return; }
        JExit *x = add_exit(jc, XS_LOOP, 0, at);
        x->xreload = need;
        return;
    }
    if (!jc->xdirty) { a64_patch_cbz(at, jc->entry); return; }
    add_exit(jc, XS_LOOP, 0, at);
}
// placeholder conditional branch (b.cond / cbz / cbnz) to a stub; patched when the stub is emitted
static void bcond_exit(JComp *jc, int cond, int kind, u64 target) {
    u32 *at = a64_here(A);
    a64_put(A, 0x54000000u | (u32)cond);
    add_exit(jc, kind, target, at);
}
static void cbz_exit(JComp *jc, int sf, int nz, int rt, int kind, u64 target) {
    u32 *at = a64_here(A);
    a64_put(A, (u32)sf << 31 | 0x34000000u | (u32)nz << 24 | (u32)rt);
    add_exit(jc, kind, target, at);
}
// The patchable sequence for a direct exit: its first instruction is the link site.
static void emit_link_exit_seq(JComp *jc, u64 target) {
    // The link site is a `b .+4` that chaining turns into `b <target block>`: B-for-B replacement is one of the
    // modifications Arm allows while other threads may be executing the instruction (CMODX).
    u32 *site = a64_here(A);
    a64_put(A, a64_b_enc(site, site + 1));
    a64_mov_imm64_fixed(A, 0, target);
    a64_adr(A, 1, site);
    a64_movz(A, 1, 2, EXIT_LINK, 0);
    a64_b(A, rt_jit_stubs.exit);
}
void jit_emit_exit_direct(JComp *jc, u64 target, int kind) {
    (void)kind;
    if (target == jc->pc && jc->loopL) { jit_xload(jc, jc->loopL & ~jc->xcached); a64_b(A, jc->gate); return; }
    jit_xflush(jc);
    if (target == jc->pc) { a64_b(A, jc->entry); return; }
    emit_link_exit_seq(jc, target);
}
static void emit_helper_exit_seq(JComp *jc) {   // x0 = exit reason from a helper, target in JState.exit_pc
    a64_mov(A, 1, 2, 0);
    a64_ldr(A, 1, T3, XSP, FR_JS);
    a64_ldr(A, 1, 0, T3, __builtin_offsetof(JState, exit_pc));
    a64_movz(A, 1, 1, 0, 0);
    a64_b(A, rt_jit_stubs.exit);
}

// ---- flags ---------------------------------------------------------------------------------------------------
static void call_mat(JComp *jc) {
    if (jc->fk == JF_MAT) return;
    a64_bl(A, rt_jit_stubs.mat);
    jc->fk = JF_MAT; jc->nzcv = 0;
}
// Recreate the host NZCV for the known op (kinds whose flags map onto ARM's N Z C V).
static void ensure_nzcv(JComp *jc) {
    if (jc->nzcv) return;
    int s = 64 - (8 << jc->fsz);
    switch (jc->fk) {
    case JF_SUB:
        if (s) { a64_lsl_imm(A, 1, T4, RFA, s); a64_lsl_imm(A, 1, T5, RFRES, s); a64_sub(A, 1, T3, T4, T5); a64_cmp(A, 1, T4, T3); }
        else { a64_sub(A, 1, T3, RFA, RFRES); a64_cmp(A, 1, RFA, T3); }
        break;
    case JF_ADD:
        if (s) { a64_lsl_imm(A, 1, T4, RFA, s); a64_lsl_imm(A, 1, T5, RFRES, s); a64_sub(A, 1, T3, T5, T4); a64_cmn(A, 1, T4, T3); }
        else { a64_sub(A, 1, T3, RFRES, RFA); a64_cmn(A, 1, RFA, T3); }
        break;
    case JF_LOGIC:
        if (s) { a64_lsl_imm(A, 1, T5, RFRES, s); a64_tst(A, 1, T5, T5); }
        else a64_tst(A, 1, RFRES, RFRES);
        break;
    case JF_INC: case JF_DEC:
        if (s) a64_lsl_imm(A, 1, T4, RFA, s); else a64_mov(A, 1, T4, RFA);
        a64_mov_imm(A, T3, 1ull << s);
        if (jc->fk == JF_INC) a64_cmn(A, 1, T4, T3); else a64_cmp(A, 1, T4, T3);
        break;
    default: return;
    }
    jc->nzcv = 1;
}
// ARM condition for x86 condition cc (0..15, opcode order). May emit code; clobbers T3..T5 (and x0-x5, x16, x17
// when the flags have to be materialized), so call it before loading operands.
static int cond_arm(JComp *jc, int cc) {
    static const signed char tab[5][8] = {
        /* ADD   */ {C_VS, C_HS, C_EQ, -1, C_MI, -1, C_LT, C_LE},
        /* SUB   */ {C_VS, C_LO, C_EQ, C_LS, C_MI, -1, C_LT, C_LE},
        /* LOGIC */ {C_VS, C_HS, C_EQ, C_EQ, C_MI, -1, C_LT, C_LE},
        /* INC   */ {C_VS, -1, C_EQ, -1, C_MI, -1, C_LT, C_LE},
        /* DEC   */ {C_VS, -1, C_EQ, -1, C_MI, -1, C_LT, C_LE},
    };
    int base = cc >> 1, neg = cc & 1, c = -1;
    if (jc->fk > 0) {
        int w = 8 << jc->fsz;
        if (jc->fk >= JF_ADD && jc->fk <= JF_DEC && tab[jc->fk - 1][base] >= 0 && (jc->nzcv || (base != 2 && base != 4))) {
            ensure_nzcv(jc);
            c = tab[jc->fk - 1][base];
        } else if (base == 2) {   // ZF from the result
            if (w == 64) a64_tst(A, 1, RFRES, RFRES); else a64_log_imm(A, 1, 3, XZR, RFRES, imask(w));
            jc->nzcv = 0; c = C_EQ;
        } else if (base == 4) {   // SF
            a64_log_imm(A, 1, 3, XZR, RFRES, 1ull << (w - 1));
            jc->nzcv = 0; c = C_NE;
        } else if (base == 5) {   // PF: even parity of the low byte
            a64_log_reg(A, 1, 2, 0, T5, RFRES, RFRES, SH_LSR, 4);
            a64_log_reg(A, 1, 2, 0, T5, T5, T5, SH_LSR, 2);
            a64_log_reg(A, 1, 2, 0, T5, T5, T5, SH_LSR, 1);
            a64_log_imm(A, 1, 3, XZR, T5, 1);
            jc->nzcv = 0; c = C_EQ;
        }
    }
    if (c < 0 && jc->fk == JF_MAT && jc->nzcv) {   // NZCV from (u)comis: CF = lt|unordered, ZF = eq|unordered, PF = unordered
        static const signed char fc[8] = {-1, C_LT, -1, C_LE, -1, C_VS, -1, -1};
        c = fc[base];
    }
    if (c < 0) {
        call_mat(jc);
        jc->nzcv = 0;
        switch (base) {
        case 0: a64_log_imm(A, 1, 3, XZR, RFRES, FL_OF); break;
        case 1: a64_log_imm(A, 1, 3, XZR, RFRES, FL_CF); break;
        case 2: a64_log_imm(A, 1, 3, XZR, RFRES, FL_ZF); break;
        case 3: a64_ubfx(A, 1, T5, RFRES, 6, 1); a64_orr(A, 1, T5, T5, RFRES); a64_log_imm(A, 1, 3, XZR, T5, 1); break;
        case 4: a64_log_imm(A, 1, 3, XZR, RFRES, FL_SF); break;
        case 5: a64_log_imm(A, 1, 3, XZR, RFRES, FL_PF); break;
        case 6: a64_log_reg(A, 1, 2, 0, T5, RFRES, RFRES, SH_LSR, 4); a64_log_imm(A, 1, 3, XZR, T5, 0x80); break;
        default: a64_log_reg(A, 1, 2, 0, T5, RFRES, RFRES, SH_LSR, 4); a64_log_imm(A, 1, 3, XZR, T5, 0xC0); break;
        }
        c = C_NE;
    }
    return neg ? (c ^ 1) : c;
}
// CF (0/1) into dst. Same clobber rules as cond_arm.
static void get_cf(JComp *jc, int dst) {
    switch (jc->fk) {
    case JF_SUB: ensure_nzcv(jc); a64_cset(A, 1, dst, C_LO); return;
    case JF_ADD: ensure_nzcv(jc); a64_cset(A, 1, dst, C_HS); return;
    case JF_LOGIC: a64_movz(A, 1, dst, 0, 0); return;
    case JF_INC: case JF_DEC: a64_ubfx(A, 1, dst, RFOP, 8, 1); return;
    default: call_mat(jc); a64_ubfx(A, 1, dst, RFRES, 0, 1); return;
    }
}
static void set_op(JComp *jc, int kind, int sz, int nzcv) {
    a64_movz(A, 1, RFOP, (u32)JF_OP(kind, sz, 0), 0);
    jc->fk = kind; jc->fsz = sz; jc->nzcv = nzcv;
}
static void set_op_aux(JComp *jc, int kind, int sz, int auxreg) {   // aux (cin / overflow / count) from a register
    a64_movz(A, 1, RFOP, (u32)JF_OP(kind, sz, 0), 0);
    a64_log_reg(A, 1, 1, 0, RFOP, RFOP, auxreg, SH_LSL, 8);
    jc->fk = kind; jc->fsz = sz; jc->nzcv = 0;
}
// Instructions that write only some flags: if other flags are live, materialize first and merge.
static int partial_prepare(JComp *jc, JInsn *j, u16 mask) {
    if (j->live_after & F6 & ~mask) { call_mat(jc); return 1; }
    return 0;
}
static void partial_commit(JComp *jc, int merge, u16 mask, int bits) {
    if (merge) {
        a64_mov_imm(A, T5, mask);
        a64_bic(A, 1, RFRES, RFRES, T5);
        a64_orr(A, 1, RFRES, RFRES, bits);
    } else a64_mov(A, 1, RFRES, bits);
    a64_movz(A, 1, RFOP, 0, 0);
    jc->fk = JF_MAT; jc->nzcv = 0;
}

// ---- operands -----------------------------------------------------------------------------------------------
typedef struct { int base; int disp; int stack; } EA;
// Effective address. Uses T0 (and T5 for segment bases, T17 for large displacements).
static EA ea_of(JComp *jc, JInsn *j, const ZydisDecodedOperand *o) {
    EA e = { T0, 0, 0 };
    ZydisRegister b = o->mem.base, x = o->mem.index;
    i64 disp = o->mem.disp.value;
    int seg = o->mem.segment == ZYDIS_REGISTER_FS ? CTX_FS : o->mem.segment == ZYDIS_REGISTER_GS ? CTX_GS : 0;
    int a32 = j->in.address_width == 32;
    if (b == ZYDIS_REGISTER_RIP || b == ZYDIS_REGISTER_EIP) {
        u64 abs = NEXT(j) + (u64)disp;
        if (a32) abs = (u32)abs;
        a64_mov_imm(A, T0, abs);
        if (seg) { a64_ldr(A, 1, T5, RCTX, seg); a64_add(A, 1, T0, T0, T5); }
        return e;
    }
    if (b != ZYDIS_REGISTER_NONE && x == ZYDIS_REGISTER_NONE && !seg && !a32) {
        int r = GR(rt_ireg[b].idx);
        if (rt_ireg[b].idx == RSP) { e.base = r; e.disp = (int)disp; e.stack = 1; if (disp == (int)disp) return e; }
        else if (disp >= -256 && disp < 256) { e.base = r; e.disp = (int)disp; return e; }
    }
    int have = 0;
    if (b != ZYDIS_REGISTER_NONE) {
        int rb = GR(rt_ireg[b].idx);
        if (x != ZYDIS_REGISTER_NONE) a64_addsub_reg(A, 1, 0, 0, T0, rb, GR(rt_ireg[x].idx), SH_LSL, __builtin_ctz(o->mem.scale ? o->mem.scale : 1));
        else a64_mov(A, 1, T0, rb);
        have = 1;
    } else if (x != ZYDIS_REGISTER_NONE) {
        a64_lsl_imm(A, 1, T0, GR(rt_ireg[x].idx), __builtin_ctz(o->mem.scale ? o->mem.scale : 1));
        have = 1;
    }
    if (!have) a64_mov_imm(A, T0, (u64)disp);
    else if (disp) {
        if (disp > 0 && disp < 4096) a64_addsub_imm(A, 1, 0, 0, T0, T0, (u32)disp, 0);
        else if (disp < 0 && -disp < 4096) a64_addsub_imm(A, 1, 1, 0, T0, T0, (u32)-disp, 0);
        else { a64_mov_imm(A, T17, (u64)disp); a64_add(A, 1, T0, T0, T17); }
    }
    if (a32) a64_mov(A, 0, T0, T0);
    if (seg) { a64_ldr(A, 1, T5, RCTX, seg); a64_add(A, 1, T0, T0, T5); }
    return e;
}
// For jit_sse.c: base register + displacement of a memory operand (plain accesses; any displacement)
void jit_ea(JComp *jc, JInsn *j, const ZydisDecodedOperand *o, int *base, int *disp) { EA e = ea_of(jc, j, o); *base = e.base; *disp = e.disp; }
int jit_rd_int(JComp *jc, JInsn *j, int i, int scratch);
void jit_wr_int(JComp *jc, JInsn *j, int i, int src);
void jit_slow_path_cbz(JComp *jc, JInsn *j, int sf, int nz, int rt);
void jit_slow_path_cond(JComp *jc, JInsn *j, int cond);
void jit_slow_resume(JComp *jc, int first);
// Address fully in a register (atomics need [Xn] without offset)
static int ea_reg(JComp *jc, EA e) {
    if (e.base == T0 && e.disp == 0) return T0;
    if (e.disp == 0) return e.base;
    if (e.disp > 0 && e.disp < 4096) a64_addsub_imm(A, 1, 0, 0, T0, e.base, (u32)e.disp, 0);
    else if (e.disp < 0 && -e.disp < 4096) a64_addsub_imm(A, 1, 1, 0, T0, e.base, (u32)-e.disp, 0);
    else { a64_mov_imm(A, T17, (u64)(i64)e.disp); a64_add(A, 1, T0, e.base, T17); }
    return T0;
}
// Guest memory access: stack (rsp-based) accesses are thread private -> plain; others acquire/release (TSO).
static void ea_load(JComp *jc, EA e, int size, int dst) {
    if (e.stack) a64_ldst_any(A, size, 1, dst, e.base, e.disp, T17);
    else if (e.disp >= -256 && e.disp < 256) a64_ldapur(A, size, dst, e.base, e.disp);
    else a64_ldapur(A, size, dst, ea_reg(jc, e), 0);
}
static void ea_store(JComp *jc, EA e, int size, int src) {
    if (e.stack) a64_ldst_any(A, size, 0, src, e.base, e.disp, T17);
    else if (e.disp >= -256 && e.disp < 256) a64_stlur(A, size, src, e.base, e.disp);
    else a64_stlur(A, size, src, ea_reg(jc, e), 0);
}

// Value of integer operand i (width = operand size) in a host register (its low `size` bits). For 32/64-bit
// GPR operands that is the guest's own host register (don't write to it). zext: bits above the size are 0.
static int rd_val(JComp *jc, JInsn *j, int i, int scratch, EA *ea, int zext) {
    const ZydisDecodedOperand *o = OPN(j, i);
    int w = o->size;
    if (ISREG(o)) {
        int r = GR(gidx(o));
        if (ghi(o)) { a64_ubfx(A, 1, scratch, r, 8, 8); return scratch; }
        if (!zext || w == 64) return r;
        if (w == 32) a64_mov(A, 0, scratch, r);
        else if (w == 16) a64_uxth(A, scratch, r);
        else a64_uxtb(A, scratch, r);
        return scratch;
    }
    if (ISMEM(o)) { ea_load(jc, *ea, szc(w), scratch); return scratch; }
    u64 v = o->imm.value.u;
    if (zext) v &= imask(w);
    if (v == 0) return XZR;
    a64_mov_imm(A, scratch, v);
    return scratch;
}
// Write the low `w` bits of src to integer operand i (GPR merge rules: 8/16 merge, 32 zero-extends).
static void wr_val(JComp *jc, JInsn *j, int i, int src, EA *ea) {
    const ZydisDecodedOperand *o = OPN(j, i);
    int w = o->size;
    if (ISMEM(o)) { ea_store(jc, *ea, szc(w), src); return; }
    int r = GR(gidx(o));
    if (ghi(o)) { a64_bfi(A, 1, r, src, 8, 8); return; }
    if (w == 64) { if (src != r) a64_mov(A, 1, r, src); }
    else if (w == 32) a64_mov(A, 0, r, src);
    else a64_bfi(A, 1, r, src, 0, w);
}
static inline int wreg_direct(const ZydisDecodedOperand *o) { return is_gpr(o) && !ghi(o) && o->size >= 32; }

// ---- classification & flag usage (must agree with the translators below) --------------------------------------
static int is_cc_mn(ZydisMnemonic m, ZydisMnemonic first, ZydisMnemonic last) { return m >= first && m <= last; }

int jit_classify(const JInsn *j) {
    const ZydisDecodedInstruction *in = &j->in;
    ZydisMnemonic m = in->mnemonic;
    int nv = in->operand_count_visible;
    const ZydisDecodedOperand *o0 = &j->op[0], *o1 = &j->op[1];
    int lock = (in->attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0;
    int w = nv ? o0->size : in->operand_width;
    if (in->address_width == 16) return 0;
    for (int i = 0; i < nv; i++)
        if (ISMEM(&j->op[i]) && j->op[i].mem.segment != ZYDIS_REGISTER_DS && j->op[i].mem.segment != ZYDIS_REGISTER_SS &&
            j->op[i].mem.segment != ZYDIS_REGISTER_ES && j->op[i].mem.segment != ZYDIS_REGISTER_CS && j->op[i].mem.segment != ZYDIS_REGISTER_FS &&
            j->op[i].mem.segment != ZYDIS_REGISTER_GS && j->op[i].mem.segment != ZYDIS_REGISTER_NONE) return 0;
    switch (m) {
    case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_PAUSE: case ZYDIS_MNEMONIC_ENDBR64: case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1: case ZYDIS_MNEMONIC_PREFETCHT2: case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHW:
    case ZYDIS_MNEMONIC_PREFETCH: case ZYDIS_MNEMONIC_LFENCE: case ZYDIS_MNEMONIC_MFENCE: case ZYDIS_MNEMONIC_SFENCE:
        return 1;
    case ZYDIS_MNEMONIC_MOV:
        return nv == 2 && gpr_mem_imm(o0) && gpr_mem_imm(o1) && !ISIMM(o0);
    case ZYDIS_MNEMONIC_MOVZX: case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD:
        return is_gpr(o0) && (is_gpr(o1) || ISMEM(o1));
    case ZYDIS_MNEMONIC_LEA: return is_gpr(o0) && w >= 16;
    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR:
        if (!gpr_mem_imm(o0) || !gpr_mem_imm(o1)) return 0;
        return !lock || ISMEM(o0);
    case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SBB:
        return !lock && gpr_mem_imm(o0) && gpr_mem_imm(o1);
    case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_TEST:
        return !lock && gpr_mem_imm(o0) && gpr_mem_imm(o1);
    case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC:
        return gpr_mem_imm(o0) && (!lock || ISMEM(o0));
    case ZYDIS_MNEMONIC_NEG: case ZYDIS_MNEMONIC_NOT:
        return !lock && gpr_mem_imm(o0);
    case ZYDIS_MNEMONIC_XCHG: return gpr_mem_imm(o0) && gpr_mem_imm(o1);
    case ZYDIS_MNEMONIC_XADD: return gpr_mem_imm(o0) && is_gpr(o1);
    case ZYDIS_MNEMONIC_CMPXCHG: return ISMEM(o0) && is_gpr(o1);
    case ZYDIS_MNEMONIC_PUSH: return in->operand_width == 64 && gpr_mem_imm(o0);
    case ZYDIS_MNEMONIC_POP: return in->operand_width == 64 && (is_gpr(o0) || ISMEM(o0));
    case ZYDIS_MNEMONIC_LEAVE: return in->operand_width == 64;
    case ZYDIS_MNEMONIC_CBW: case ZYDIS_MNEMONIC_CWDE: case ZYDIS_MNEMONIC_CDQE: case ZYDIS_MNEMONIC_CWD:
    case ZYDIS_MNEMONIC_CDQ: case ZYDIS_MNEMONIC_CQO:
        return 1;
    case ZYDIS_MNEMONIC_IMUL:
        if (nv == 1) return (w == 32 || w == 64) && (is_gpr(o0) || ISMEM(o0));
        return w >= 16 && is_gpr(o0) && gpr_mem_imm(o1);
    case ZYDIS_MNEMONIC_MUL: return (w == 32 || w == 64) && (is_gpr(o0) || ISMEM(o0));
    case ZYDIS_MNEMONIC_DIV: case ZYDIS_MNEMONIC_IDIV: return (w == 32 || w == 64) && (is_gpr(o0) || ISMEM(o0));
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR:
        return (is_gpr(o0) || ISMEM(o0)) && (nv < 2 || ISIMM(o1) || (is_gpr(o1) && o1->reg.value == ZYDIS_REGISTER_CL));
    case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR: case ZYDIS_MNEMONIC_BTC:
        if (w < 16) return 0;
        if (ISMEM(o0)) return ISIMM(o1) && (!lock || m != ZYDIS_MNEMONIC_BT);
        return !lock && is_gpr(o0) && (ISIMM(o1) || is_gpr(o1));
    case ZYDIS_MNEMONIC_BSF: case ZYDIS_MNEMONIC_BSR: case ZYDIS_MNEMONIC_TZCNT: case ZYDIS_MNEMONIC_LZCNT: case ZYDIS_MNEMONIC_POPCNT:
        return w >= 16 && is_gpr(o0) && (is_gpr(o1) || ISMEM(o1));
    case ZYDIS_MNEMONIC_BSWAP: return w >= 32;
    case ZYDIS_MNEMONIC_CLC: case ZYDIS_MNEMONIC_STC: case ZYDIS_MNEMONIC_CMC: case ZYDIS_MNEMONIC_LAHF: case ZYDIS_MNEMONIC_SAHF:
        return 1;
    case ZYDIS_MNEMONIC_JMP:
        return nv == 1 && (ISIMM(o0) || is_gpr(o0) || ISMEM(o0)) && (ISIMM(o0) || w == 64);
    case ZYDIS_MNEMONIC_CALL:
        return nv == 1 && (ISIMM(o0) || is_gpr(o0) || ISMEM(o0)) && (ISIMM(o0) || w == 64) && in->operand_width == 64;
    case ZYDIS_MNEMONIC_RET: return in->operand_width == 64 && !(in->attributes & ZYDIS_ATTRIB_HAS_REPNE) && (!nv || ISIMM(o0));
    case ZYDIS_MNEMONIC_JRCXZ: case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_LOOP: return ISIMM(o0);
    default: break;
    }
    if (is_cc_mn(m, ZYDIS_MNEMONIC_JB, ZYDIS_MNEMONIC_JZ) || m == ZYDIS_MNEMONIC_JO || m == ZYDIS_MNEMONIC_JNO || m == ZYDIS_MNEMONIC_JS ||
        m == ZYDIS_MNEMONIC_JNS || m == ZYDIS_MNEMONIC_JP || m == ZYDIS_MNEMONIC_JNP || m == ZYDIS_MNEMONIC_JL || m == ZYDIS_MNEMONIC_JLE ||
        m == ZYDIS_MNEMONIC_JNL || m == ZYDIS_MNEMONIC_JNLE || m == ZYDIS_MNEMONIC_JNB || m == ZYDIS_MNEMONIC_JNBE || m == ZYDIS_MNEMONIC_JBE ||
        m == ZYDIS_MNEMONIC_JNZ)
        return in->meta.category == ZYDIS_CATEGORY_COND_BR && ISIMM(o0);
    if (in->meta.category == ZYDIS_CATEGORY_SETCC) return is_gpr(o0) || ISMEM(o0);
    if (in->meta.category == ZYDIS_CATEGORY_CMOV) return w >= 16 && is_gpr(o0) && (is_gpr(o1) || ISMEM(o1));
    return jit_sse_classify(j);
}

static u16 cc_reads(int cc) {
    static const u16 r[8] = {FL_OF, FL_CF, FL_ZF, FL_CF | FL_ZF, FL_SF, FL_PF, FL_SF | FL_OF, FL_ZF | FL_SF | FL_OF};
    return r[(cc & 15) >> 1];
}
void jit_flag_use(const JInsn *j, u16 *rd, u16 *wr) {
    *rd = 0; *wr = 0;
    if (j->fallback) { *rd = F6; *wr = F6; return; }
    const ZydisDecodedInstruction *in = &j->in;
    ZydisMnemonic m = in->mnemonic;
    int w = in->operand_count_visible ? j->op[0].size : in->operand_width;
    switch (m) {
    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_TEST: case ZYDIS_MNEMONIC_NEG: case ZYDIS_MNEMONIC_IMUL: case ZYDIS_MNEMONIC_MUL:
    case ZYDIS_MNEMONIC_XADD: case ZYDIS_MNEMONIC_CMPXCHG: case ZYDIS_MNEMONIC_POPCNT:
        *wr = F6; return;
    case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SBB: *wr = F6; *rd = FL_CF; return;
    case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: *wr = F6 & ~FL_CF; return;
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: {
        if (in->operand_count_visible >= 2 && !ISIMM(&j->op[1])) return;   // count may be 0: flags unchanged
        u64 cnt = in->operand_count_visible >= 2 ? j->op[1].imm.value.u : 1;
        if (!(cnt & (w == 64 ? 63 : 31))) return;
        *wr = (m == ZYDIS_MNEMONIC_ROL || m == ZYDIS_MNEMONIC_ROR) ? (FL_CF | FL_OF) : F6;
        return; }
    case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR: case ZYDIS_MNEMONIC_BTC:
    case ZYDIS_MNEMONIC_CLC: case ZYDIS_MNEMONIC_STC:
        *wr = FL_CF; return;
    case ZYDIS_MNEMONIC_CMC: *wr = FL_CF; *rd = FL_CF; return;
    case ZYDIS_MNEMONIC_BSF: case ZYDIS_MNEMONIC_BSR: *wr = FL_ZF; return;
    case ZYDIS_MNEMONIC_TZCNT: case ZYDIS_MNEMONIC_LZCNT: *wr = FL_CF | FL_ZF; return;
    case ZYDIS_MNEMONIC_LAHF: *rd = F6 & ~FL_OF; return;
    case ZYDIS_MNEMONIC_SAHF: *wr = F6 & ~FL_OF; return;
    default: break;
    }
    if (in->meta.category == ZYDIS_CATEGORY_COND_BR || in->meta.category == ZYDIS_CATEGORY_SETCC || in->meta.category == ZYDIS_CATEGORY_CMOV) {
        if (m != ZYDIS_MNEMONIC_JRCXZ && m != ZYDIS_MNEMONIC_JECXZ && m != ZYDIS_MNEMONIC_LOOP) *rd = cc_reads(in->opcode);
        return;
    }
    if (in->meta.isa_ext == ZYDIS_ISA_EXT_SSE || in->meta.isa_ext == ZYDIS_ISA_EXT_SSE2) {
        switch (m) {
        case ZYDIS_MNEMONIC_UCOMISS: case ZYDIS_MNEMONIC_UCOMISD: case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_COMISD: *wr = F6; return;
        default: return;
        }
    }
}

// ---- integer translators -----------------------------------------------------------------------------------
static void x_mov(JComp *jc, JInsn *j) {
    const ZydisDecodedOperand *d = OPN(j, 0), *s = OPN(j, 1);
    int w = d->size;
    if (ISMEM(d)) {
        EA e = ea_of(jc, j, d);
        int v = rd_val(jc, j, 1, T1, NULL, 0);
        ea_store(jc, e, szc(w), v);
        return;
    }
    if (ISMEM(s)) {
        EA e = ea_of(jc, j, s);
        if (wreg_direct(d)) ea_load(jc, e, szc(w), GR(gidx(d)));
        else { ea_load(jc, e, szc(w), T1); wr_val(jc, j, 0, T1, NULL); }
        return;
    }
    if (ISIMM(s) && wreg_direct(d)) { a64_mov_imm(A, GR(gidx(d)), w == 32 ? (u32)s->imm.value.u : s->imm.value.u); return; }
    int v = rd_val(jc, j, 1, T1, NULL, 0);
    wr_val(jc, j, 0, v, NULL);
}

static void x_movx(JComp *jc, JInsn *j) {
    const ZydisDecodedOperand *d = OPN(j, 0), *s = OPN(j, 1);
    int w = d->size, sw = s->size, sgn = MN(j) != ZYDIS_MNEMONIC_MOVZX;
    EA e; if (ISMEM(s)) e = ea_of(jc, j, s);
    int v = rd_val(jc, j, 1, T1, &e, 0);
    int dst = wreg_direct(d) ? GR(gidx(d)) : T1;
    int sf = w == 64;
    if (sw == 8) { if (sgn) a64_sxtb(A, sf, dst, v); else a64_uxtb(A, dst, v); }
    else if (sw == 16) { if (sgn) a64_sxth(A, sf, dst, v); else a64_uxth(A, dst, v); }
    else if (sw == 32) { if (sgn && w == 64) a64_sxtw(A, dst, v); else a64_mov(A, 0, dst, v); }
    else a64_mov(A, 1, dst, v);
    if (dst == T1) wr_val(jc, j, 0, T1, NULL);
}

static void x_lea(JComp *jc, JInsn *j) {
    const ZydisDecodedOperand *d = OPN(j, 0), *o = OPN(j, 1);
    ZydisRegister b = o->mem.base, x = o->mem.index;
    i64 disp = o->mem.disp.value;
    int dst = wreg_direct(d) && d->size == 64 ? GR(gidx(d)) : T0;
    if (b == ZYDIS_REGISTER_RIP || b == ZYDIS_REGISTER_EIP) a64_mov_imm(A, dst, NEXT(j) + (u64)disp);
    else {
        int have = 0, src = T0;
        if (b != ZYDIS_REGISTER_NONE && x != ZYDIS_REGISTER_NONE) {
            a64_addsub_reg(A, 1, 0, 0, T0, GR(rt_ireg[b].idx), GR(rt_ireg[x].idx), SH_LSL, __builtin_ctz(o->mem.scale)); have = 1;
        } else if (b != ZYDIS_REGISTER_NONE) { src = GR(rt_ireg[b].idx); have = 1; }
        else if (x != ZYDIS_REGISTER_NONE) { a64_lsl_imm(A, 1, T0, GR(rt_ireg[x].idx), __builtin_ctz(o->mem.scale)); have = 1; }
        if (!have) a64_mov_imm(A, dst, (u64)disp);
        else if (!disp) { if (src != dst) a64_mov(A, 1, dst, src); }
        else if (disp > 0 && disp < 4096) a64_addsub_imm(A, 1, 0, 0, dst, src, (u32)disp, 0);
        else if (disp < 0 && -disp < 4096) a64_addsub_imm(A, 1, 1, 0, dst, src, (u32)-disp, 0);
        else { a64_mov_imm(A, T17, (u64)disp); a64_add(A, 1, dst, src, T17); }
    }
    if (dst == T0) {
        if (j->in.address_width == 32) a64_mov(A, 0, T0, T0);
        wr_val(jc, j, 0, T0, NULL);
    } else if (j->in.address_width == 32) a64_mov(A, 0, dst, dst);
}

// add/sub/and/or/xor/adc/sbb/cmp/test
static void x_alu2(JComp *jc, JInsn *j) {
    ZydisMnemonic m = MN(j);
    const ZydisDecodedOperand *d = OPN(j, 0), *s = OPN(j, 1);
    int w = d->size, sz = szc(w), sf = w == 64, s32 = w >= 32;
    int need = (j->live_after & F6) != 0;
    int cmp = m == ZYDIS_MNEMONIC_CMP || m == ZYDIS_MNEMONIC_TEST;
    if (cmp && !need) return;
    int lock = (j->in.attributes & ZYDIS_ATTRIB_HAS_LOCK) && ISMEM(d);
    int carry = m == ZYDIS_MNEMONIC_ADC || m == ZYDIS_MNEMONIC_SBB;
    if (carry) get_cf(jc, T4);
    EA e = { 0 };
    if (ISMEM(d)) e = ea_of(jc, j, d);
    else if (ISMEM(s)) e = ea_of(jc, j, s);
    int kind = (m == ZYDIS_MNEMONIC_ADD) ? JF_ADD : (m == ZYDIS_MNEMONIC_SUB || m == ZYDIS_MNEMONIC_CMP) ? JF_SUB :
               m == ZYDIS_MNEMONIC_ADC ? JF_ADC : m == ZYDIS_MNEMONIC_SBB ? JF_SBB : JF_LOGIC;
    if (lock) {
        int b = rd_val(jc, j, 1, T2, NULL, 0);
        int ar = ea_reg(jc, e);
        switch (m) {
        case ZYDIS_MNEMONIC_ADD: a64_ldop_al(A, sz, 0, b, T1, ar); break;
        case ZYDIS_MNEMONIC_SUB: a64_sub(A, 1, T3, XZR, b); a64_ldop_al(A, sz, 0, T3, T1, ar); break;
        case ZYDIS_MNEMONIC_AND: a64_mvn(A, 1, T3, b); a64_ldop_al(A, sz, 1, T3, T1, ar); break;
        case ZYDIS_MNEMONIC_OR: a64_ldop_al(A, sz, 3, b, T1, ar); break;
        default: a64_ldop_al(A, sz, 2, b, T1, ar); break;
        }
        if (!need) return;
        if (kind != JF_LOGIC) a64_mov(A, 1, RFA, T1);
        switch (m) {
        case ZYDIS_MNEMONIC_ADD: a64_addsub_reg(A, s32 ? sf : 0, 0, s32, RFRES, T1, b, 0, 0); break;
        case ZYDIS_MNEMONIC_SUB: a64_addsub_reg(A, s32 ? sf : 0, 1, s32, RFRES, T1, b, 0, 0); break;
        case ZYDIS_MNEMONIC_AND: a64_log_reg(A, s32 ? sf : 0, s32 ? 3 : 0, 0, RFRES, T1, b, 0, 0); break;
        case ZYDIS_MNEMONIC_OR: a64_orr(A, sf, RFRES, T1, b); s32 = 0; break;
        default: a64_eor(A, sf, RFRES, T1, b); s32 = 0; break;
        }
        set_op(jc, kind, sz, s32 && (kind == JF_ADD || kind == JF_SUB || m == ZYDIS_MNEMONIC_AND));
        return;
    }
    int av = rd_val(jc, j, 0, T1, &e, 0);
    // second operand: immediate forms where possible
    int bimm = ISIMM(s), bv = -1;
    u64 imm = bimm ? s->imm.value.u : 0;
    int opsf = s32 ? sf : 0;
    int setf = need && s32 && !carry;
    int res = need ? RFRES : (wreg_direct(d) ? GR(gidx(d)) : T3);
    if (need && (kind == JF_ADD || kind == JF_SUB || kind == JF_ADC || kind == JF_SBB)) a64_mov(A, 1, RFA, av);
    int done = 0;
    if (bimm && !carry) {
        if (kind == JF_ADD || kind == JF_SUB) {
            i64 v = (i64)imm;
            if (w < 64) v = (i64)sext(imm & imask(w), w);
            int op = kind == JF_SUB;
            if (v < 0 && v != INT64_MIN) { v = -v; op ^= 1; }
            if (v == 0) { a64_addsub_reg(A, opsf, kind == JF_SUB, setf, res, av, XZR, 0, 0); done = 1; }
            else if (v < 4096) { a64_addsub_imm(A, opsf, op, setf, res, av, (u32)v, 0); done = 1; }
            else if (!(v & 0xFFF) && v < (1 << 24)) { a64_addsub_imm(A, opsf, op, setf, res, av, (u32)(v >> 12), 1); done = 1; }
        } else {
            int opc = m == ZYDIS_MNEMONIC_OR ? 1 : m == ZYDIS_MNEMONIC_XOR ? 2 : (setf ? 3 : 0);
            u64 v = w == 64 ? imm : imm & imask(w);
            if (!s32) v &= imask(w);
            if (v && v != imask(sf ? 64 : 32) && a64_log_imm(A, opsf, opc, res, av, v)) done = 1;
        }
    }
    if (!done) {
        bv = rd_val(jc, j, 1, T2, &e, 0);
        switch (kind) {
        case JF_ADD: a64_addsub_reg(A, opsf, 0, setf, res, av, bv, 0, 0); break;
        case JF_SUB: a64_addsub_reg(A, opsf, 1, setf, res, av, bv, 0, 0); break;
        case JF_ADC: a64_add(A, opsf, res, av, bv); a64_add(A, opsf, res, res, T4); break;
        case JF_SBB: a64_sub(A, opsf, res, av, bv); a64_sub(A, opsf, res, res, T4); break;
        default: {
            int opc = m == ZYDIS_MNEMONIC_OR ? 1 : m == ZYDIS_MNEMONIC_XOR ? 2 : (setf ? 3 : 0);
            a64_log_reg(A, opsf, opc, 0, res, av, bv, 0, 0);
        } }
    }
    if (!cmp) wr_val(jc, j, 0, res, &e);
    if (need) {
        if (carry) set_op_aux(jc, kind, sz, T4);
        else set_op(jc, kind, sz, setf && (kind == JF_ADD || kind == JF_SUB || m == ZYDIS_MNEMONIC_AND || m == ZYDIS_MNEMONIC_TEST));
    }
}

static void x_alu1(JComp *jc, JInsn *j) {
    ZydisMnemonic m = MN(j);
    const ZydisDecodedOperand *d = OPN(j, 0);
    int w = d->size, sz = szc(w), sf = w == 64, s32 = w >= 32;
    int need = (j->live_after & F6) != 0 && m != ZYDIS_MNEMONIC_NOT;
    int lock = (j->in.attributes & ZYDIS_ATTRIB_HAS_LOCK) && ISMEM(d);
    int keepcf = need && (m == ZYDIS_MNEMONIC_INC || m == ZYDIS_MNEMONIC_DEC) && (j->live_after & FL_CF);
    if (keepcf) get_cf(jc, T4);
    EA e = { 0 };
    if (ISMEM(d)) e = ea_of(jc, j, d);
    if (lock) {   // inc/dec
        int ar = ea_reg(jc, e);
        a64_mov_imm(A, T2, m == ZYDIS_MNEMONIC_INC ? 1 : ~0ull);
        a64_ldop_al(A, sz, 0, T2, T1, ar);
        if (!need) return;
        a64_mov(A, 1, RFA, T1);
        a64_add(A, 1, RFRES, T1, T2);
        if (keepcf) set_op_aux(jc, m == ZYDIS_MNEMONIC_INC ? JF_INC : JF_DEC, sz, T4); else set_op(jc, m == ZYDIS_MNEMONIC_INC ? JF_INC : JF_DEC, sz, 0);
        return;
    }
    int av = rd_val(jc, j, 0, T1, &e, 0);
    int res = need ? RFRES : (wreg_direct(d) ? GR(gidx(d)) : T3);
    int opsf = s32 ? sf : 0;
    switch (m) {
    case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC:
        if (need) a64_mov(A, 1, RFA, av);
        a64_addsub_imm(A, opsf, m == ZYDIS_MNEMONIC_DEC, 0, res, av, 1, 0);
        break;
    case ZYDIS_MNEMONIC_NEG:
        if (need) a64_movz(A, 1, RFA, 0, 0);
        a64_addsub_reg(A, opsf, 1, need && s32, res, XZR, av, 0, 0);
        break;
    default: a64_mvn(A, opsf, res, av); break;
    }
    wr_val(jc, j, 0, res, &e);
    if (!need) return;
    if (m == ZYDIS_MNEMONIC_NEG) set_op(jc, JF_SUB, sz, s32);
    else if (keepcf) set_op_aux(jc, m == ZYDIS_MNEMONIC_INC ? JF_INC : JF_DEC, sz, T4);
    else set_op(jc, m == ZYDIS_MNEMONIC_INC ? JF_INC : JF_DEC, sz, 0);
}

static void x_xchg(JComp *jc, JInsn *j) {
    const ZydisDecodedOperand *o0 = OPN(j, 0), *o1 = OPN(j, 1);
    int w = o0->size;
    if (ISMEM(o0) || ISMEM(o1)) {
        int mi = ISMEM(o0) ? 0 : 1, ri = 1 - mi;
        EA e = ea_of(jc, j, OPN(j, mi));
        int ar = ea_reg(jc, e);
        int v = rd_val(jc, j, ri, T1, NULL, 0);
        a64_ldop_al(A, szc(w), 8, v, T2, ar);
        wr_val(jc, j, ri, T2, NULL);
        return;
    }
    int a = rd_val(jc, j, 0, T1, NULL, 0);
    if (a != T1) { a64_mov(A, 1, T1, a); }
    int b = rd_val(jc, j, 1, T2, NULL, 0);
    wr_val(jc, j, 0, b, NULL);
    wr_val(jc, j, 1, T1, NULL);
}

static void x_xadd(JComp *jc, JInsn *j) {
    const ZydisDecodedOperand *d = OPN(j, 0);
    int w = d->size, sz = szc(w), s32 = w >= 32, sf = w == 64;
    int need = (j->live_after & F6) != 0;
    int b = rd_val(jc, j, 1, T2, NULL, 0);
    if (b != T2) { a64_mov(A, 1, T2, b); b = T2; }
    if (ISMEM(d)) {
        EA e = ea_of(jc, j, d);
        a64_ldop_al(A, sz, 0, b, T1, ea_reg(jc, e));
    } else {
        int a = rd_val(jc, j, 0, T1, NULL, 0);
        if (a != T1) a64_mov(A, 1, T1, a);
        a64_add(A, 1, T3, T1, b);
        wr_val(jc, j, 0, T3, NULL);
    }
    if (need) { a64_mov(A, 1, RFA, T1); a64_addsub_reg(A, s32 ? sf : 0, 0, s32, RFRES, T1, b, 0, 0); }
    wr_val(jc, j, 1, T1, NULL);
    if (need) set_op(jc, JF_ADD, sz, s32);
}

static void x_cmpxchg(JComp *jc, JInsn *j) {
    const ZydisDecodedOperand *d = OPN(j, 0);
    int w = d->size, sz = szc(w), sf = w == 64, s32 = w >= 32;
    int need = (j->live_after & F6) != 0;
    EA e = ea_of(jc, j, d);
    int ar = ea_reg(jc, e);
    int src = rd_val(jc, j, 1, T1, NULL, 0);
    int rax = GR(RAX);
    a64_mov(A, 1, T2, rax);   // casal compares the low `w` bits of T2, returns the old memory value in T2
    a64_casal(A, sz, T2, src, ar);
    if (need) a64_mov(A, 1, RFA, rax);
    // flags = cmp acc, old
    if (s32) a64_addsub_reg(A, sf, 1, 1, need ? RFRES : XZR, rax, T2, 0, 0);
    else {
        a64_lsl_imm(A, 1, T3, rax, 64 - w); a64_lsl_imm(A, 1, T4, T2, 64 - w);
        a64_cmp(A, 1, T3, T4);
        if (need) a64_sub(A, 0, RFRES, rax, T2);
    }
    if (w == 64) a64_csel(A, 1, rax, rax, T2, C_EQ);
    else if (w == 32) a64_csel(A, 1, rax, rax, T2, C_EQ);   // old is zero-extended; on success rax is untouched
    else { a64_csel(A, 1, T3, rax, T2, C_EQ); a64_bfi(A, 1, rax, T3, 0, w); }
    if (need) set_op(jc, JF_SUB, sz, s32);
    else jc->nzcv = 0;
}

static void x_push(JComp *jc, JInsn *j) {
    const ZydisDecodedOperand *o = OPN(j, 0);
    int rsp = GR(RSP);
    int v;
    if (ISMEM(o)) { EA e = ea_of(jc, j, o); ea_load(jc, e, 3, T1); v = T1; }
    else if (ISIMM(o)) { a64_mov_imm(A, T1, o->imm.value.u); v = T1; }
    else { v = GR(gidx(o)); if (v == rsp) { a64_mov(A, 1, T1, rsp); v = T1; } }
    a64_ldst_simm(A, 3, 0, v, rsp, -8, 3);
}
static void x_pop(JComp *jc, JInsn *j) {
    const ZydisDecodedOperand *o = OPN(j, 0);
    int rsp = GR(RSP);
    if (is_gpr(o) && GR(gidx(o)) != rsp) { a64_ldst_simm(A, 3, 1, GR(gidx(o)), rsp, 8, 1); return; }
    a64_ldst_simm(A, 3, 1, T1, rsp, 8, 1);
    if (is_gpr(o)) { a64_mov(A, 1, rsp, T1); return; }
    EA e = ea_of(jc, j, o);   // address computed after the increment, like the hardware
    ea_store(jc, e, 3, T1);
}

static void x_cvt(JComp *jc, JInsn *j) {
    int rax = GR(RAX), rdx = GR(RDX);
    switch (MN(j)) {
    case ZYDIS_MNEMONIC_CBW: a64_sxtb(A, 0, T1, rax); a64_bfi(A, 1, rax, T1, 0, 16); break;
    case ZYDIS_MNEMONIC_CWDE: a64_sxth(A, 0, rax, rax); break;
    case ZYDIS_MNEMONIC_CDQE: a64_sxtw(A, rax, rax); break;
    case ZYDIS_MNEMONIC_CWD: a64_sbfx(A, 1, T1, rax, 15, 1); a64_bfi(A, 1, rdx, T1, 0, 16); break;
    case ZYDIS_MNEMONIC_CDQ: a64_asr_imm(A, 0, rdx, rax, 31); break;
    default: a64_asr_imm(A, 1, rdx, rax, 63); break;
    }
}

static void x_imul(JComp *jc, JInsn *j) {
    int nv = NV(j);
    const ZydisDecodedOperand *d = OPN(j, 0);
    int w = d->size, sz = szc(w);
    int need = (j->live_after & F6) != 0;
    EA e = { 0 };
    for (int i = 0; i < nv; i++) if (ISMEM(OPN(j, i))) e = ea_of(jc, j, OPN(j, i));
    int a, b;
    if (nv == 3) { a = rd_val(jc, j, 1, T1, &e, 0); b = rd_val(jc, j, 2, T2, &e, 0); }
    else { a = rd_val(jc, j, 0, T1, &e, 0); b = rd_val(jc, j, 1, T2, &e, 0); }
    if (w == 64) {
        a64_mul(A, 1, T3, a, b);
        if (need) { a64_smulh(A, T4, a, b); a64_addsub_reg(A, 1, 1, 1, XZR, T4, T3, SH_ASR, 63); a64_cset(A, 1, T4, C_NE); }
    } else if (w == 32) {
        a64_smull(A, T3, a, b);
        if (need) { a64_addsub_ext(A, 1, 1, 1, XZR, T3, T3, 6, 0); a64_cset(A, 1, T4, C_NE); }
    } else {
        a64_sxth(A, 0, T1, a); a64_sxth(A, 0, T2, b);
        a64_mul(A, 0, T3, T1, T2);
        if (need) { a64_sxth(A, 0, T4, T3); a64_cmp(A, 0, T4, T3); a64_cset(A, 1, T4, C_NE); }
    }
    wr_val(jc, j, 0, T3, &e);
    if (need) { a64_mov(A, 1, RFRES, T3); set_op_aux(jc, JF_MUL, sz, T4); }
}

static void x_mul1(JComp *jc, JInsn *j) {   // mul/imul r/m32/64: rdx:rax = rax * src
    const ZydisDecodedOperand *o = OPN(j, 0);
    int w = o->size, sgn = MN(j) == ZYDIS_MNEMONIC_IMUL;
    int need = (j->live_after & F6) != 0;
    EA e = { 0 };
    if (ISMEM(o)) e = ea_of(jc, j, o);
    int b = rd_val(jc, j, 0, T2, &e, 0);
    int rax = GR(RAX), rdx = GR(RDX);
    if (w == 64) {
        a64_mul(A, 1, T3, rax, b);
        if (sgn) a64_smulh(A, T4, rax, b); else a64_umulh(A, T4, rax, b);
        a64_mov(A, 1, rax, T3); a64_mov(A, 1, rdx, T4);
        if (need) {
            if (sgn) a64_addsub_reg(A, 1, 1, 1, XZR, T4, T3, SH_ASR, 63); else a64_cmp(A, 1, T4, XZR);
            a64_cset(A, 1, T4, C_NE);
        }
    } else {
        if (sgn) a64_smull(A, T3, rax, b); else a64_umull(A, T3, rax, b);
        a64_mov(A, 0, rax, T3); a64_lsr_imm(A, 1, rdx, T3, 32);
        if (need) {
            if (sgn) a64_addsub_ext(A, 1, 1, 1, XZR, T3, T3, 6, 0); else a64_cmp(A, 1, rdx, XZR);
            a64_cset(A, 1, T4, C_NE);
        }
    }
    if (need) { a64_mov(A, 1, RFRES, T3); set_op_aux(jc, JF_MUL, szc(w), T4); }
}

static void slow_path(JComp *jc, JInsn *j, int cond) {   // b.cond to an out-of-line "interpret this instruction"
    u32 *at = a64_here(A);
    a64_put(A, 0x54000000u | (u32)cond);
    JExit *x = add_exit(jc, XS_SLOW, 0, at);
    x->insn = j;
}
static void slow_cbz(JComp *jc, JInsn *j, int sf, int nz, int rt) {
    u32 *at = a64_here(A);
    a64_put(A, (u32)sf << 31 | 0x34000000u | (u32)nz << 24 | (u32)rt);
    JExit *x = add_exit(jc, XS_SLOW, 0, at);
    x->insn = j;
}
void jit_slow_path_cbz(JComp *jc, JInsn *j, int sf, int nz, int rt) { slow_cbz(jc, j, sf, nz, rt); }
void jit_slow_path_cond(JComp *jc, JInsn *j, int cond) { slow_path(jc, j, cond); }
void jit_slow_resume(JComp *jc, int first) {   // slow paths since exit index `first` resume here
    u32 *resume = a64_here(A);
    for (int i = first; i < jc->nexit; i++)
        if (jc->exits[i].kind == XS_SLOW && !jc->exits[i].resume) { jc->exits[i].resume = resume; jc->exits[i].xreload = jc->xcached; }
    jc->fk = -1; jc->nzcv = 0;
}
int jit_rd_int(JComp *jc, JInsn *j, int i, int scratch) {
    EA e = { 0 };
    if (ISMEM(OPN(j, i))) e = ea_of(jc, j, OPN(j, i));
    return rd_val(jc, j, i, scratch, &e, 0);
}
void jit_wr_int(JComp *jc, JInsn *j, int i, int src) {
    EA e = { 0 };
    if (ISMEM(OPN(j, i))) e = ea_of(jc, j, OPN(j, i));
    wr_val(jc, j, i, src, &e);
}
static void x_div(JComp *jc, JInsn *j) {
    const ZydisDecodedOperand *o = OPN(j, 0);
    int w = o->size, sgn = MN(j) == ZYDIS_MNEMONIC_IDIV;
    int first = jc->nexit;
    EA e = { 0 };
    if (ISMEM(o)) e = ea_of(jc, j, o);
    int v = rd_val(jc, j, 0, T2, &e, 0);
    int rax = GR(RAX), rdx = GR(RDX);
    if (w == 64) {
        if (v != T2) a64_mov(A, 1, T2, v);
        slow_cbz(jc, j, 1, 0, T2);
        if (!sgn) {
            slow_cbz(jc, j, 1, 1, rdx);
            a64_udiv(A, 1, T3, rax, T2);
        } else {
            a64_addsub_reg(A, 1, 1, 1, XZR, rdx, rax, SH_ASR, 63);
            slow_path(jc, j, C_NE);
            a64_mov_imm(A, T5, 0x8000000000000000ull);
            a64_addsub_imm(A, 1, 0, 1, XZR, T2, 1, 0);   // cmn T2, #1
            a64_ccmp(A, 1, rax, T5, 0, C_EQ);
            slow_path(jc, j, C_EQ);
            a64_sdiv(A, 1, T3, rax, T2);
        }
        a64_msub(A, 1, T4, T3, T2, rax);
        a64_mov(A, 1, rax, T3); a64_mov(A, 1, rdx, T4);
    } else {
        a64_mov(A, 0, T1, rax);
        a64_log_reg(A, 1, 1, 0, T1, T1, rdx, SH_LSL, 32);
        if (sgn) a64_sxtw(A, T2, v); else a64_mov(A, 0, T2, v);
        slow_cbz(jc, j, 1, 0, T2);
        if (!sgn) {
            a64_udiv(A, 1, T3, T1, T2);
            a64_lsr_imm(A, 1, T4, T3, 32);
            slow_cbz(jc, j, 1, 1, T4);
        } else {
            a64_sdiv(A, 1, T3, T1, T2);
            a64_addsub_ext(A, 1, 1, 1, XZR, T3, T3, 6, 0);
            slow_path(jc, j, C_NE);
        }
        a64_msub(A, 1, T4, T3, T2, T1);
        a64_mov(A, 0, rax, T3); a64_mov(A, 0, rdx, T4);
    }
    jit_slow_resume(jc, first);   // flags: unchanged (fast path) or materialized (slow path)
}

static void x_shift(JComp *jc, JInsn *j) {
    ZydisMnemonic m = MN(j);
    const ZydisDecodedOperand *d = OPN(j, 0);
    int w = d->size, sz = szc(w), sf = w == 64, mask = w == 64 ? 63 : 31;
    int byimm = NV(j) < 2 || ISIMM(OPN(j, 1));
    u64 cnt = NV(j) < 2 ? 1 : byimm ? (OPN(j, 1)->imm.value.u & mask) : 0;
    int rot = m == ZYDIS_MNEMONIC_ROL || m == ZYDIS_MNEMONIC_ROR;
    u16 wr = rot ? (FL_CF | FL_OF) : F6;
    int need = (j->live_after & wr) != 0;
    int merge = 0;
    if (rot && need) merge = partial_prepare(jc, j, FL_CF | FL_OF);
    if (!byimm && need && !rot) {
        // count may be 0 (flags unchanged): compute and update flags under a branch
    }
    EA e = { 0 };
    if (ISMEM(d)) e = ea_of(jc, j, d);
    if (byimm && cnt == 0) {   // no-op, but a 32-bit register destination is still written (zero-extended)
        if (w == 32 && is_gpr(d)) a64_mov(A, 0, GR(gidx(d)), GR(gidx(d)));
        return;
    }
    // a: operand value, zero-extended for SHR/rotates, sign-extended for SAR (8/16 bit)
    int a = rd_val(jc, j, 0, T1, &e, m == ZYDIS_MNEMONIC_SHR || rot);
    if (m == ZYDIS_MNEMONIC_SAR && w < 32) { if (w == 8) a64_sxtb(A, 0, T1, a); else a64_sxth(A, 0, T1, a); a = T1; }
    int opsf = sf;
    int creg = -1;
    if (!byimm) { a64_log_imm(A, 0, 0, T4, GR(RCX), (u64)mask); creg = T4; }
    int res = T3;
    if (!rot) {
        if (byimm) {
            int c = (int)cnt;
            if (m == ZYDIS_MNEMONIC_SHL) { if (w < 32 && c >= 32) a64_movz(A, 1, res, 0, 0); else a64_lsl_imm(A, w == 64, res, a, w == 64 ? c : (c > 31 ? 31 : c)); }
            else if (m == ZYDIS_MNEMONIC_SHR) a64_lsr_imm(A, w == 64, res, a, c);
            else a64_asr_imm(A, w == 64, res, a, c);
        } else {
            if (m == ZYDIS_MNEMONIC_SHL) a64_lslv(A, opsf, res, a, creg);
            else if (m == ZYDIS_MNEMONIC_SHR) a64_lsrv(A, opsf, res, a, creg);
            else a64_asrv(A, opsf, res, a, creg);
        }
    } else {
        int left = m == ZYDIS_MNEMONIC_ROL;
        if (w >= 32) {
            if (byimm) a64_ror_imm(A, sf, res, a, left ? (int)((w - cnt % w) % w) : (int)(cnt % w));
            else { if (left) { a64_sub(A, 0, T5, XZR, creg); a64_rorv(A, sf, res, a, T5); } else a64_rorv(A, sf, res, a, creg); }
        } else {
            // replicate the 8/16-bit value across 32 bits, rotate, take the low bits
            if (w == 8) { a64_log_reg(A, 0, 1, 0, T2, a, a, SH_LSL, 8); a64_log_reg(A, 0, 1, 0, T2, T2, T2, SH_LSL, 16); }
            else a64_log_reg(A, 0, 1, 0, T2, a, a, SH_LSL, 16);
            if (byimm) { int k = (int)(cnt % w); a64_ror_imm(A, 0, res, T2, left ? (32 - k) % 32 : k); }
            else { if (left) { a64_sub(A, 0, T5, XZR, creg); a64_rorv(A, 0, res, T2, T5); } else a64_rorv(A, 0, res, T2, creg); }
        }
    }
    if (need && !rot) a64_mov(A, 1, T2, a);   // the operand, before the destination (maybe the same register) is written
    wr_val(jc, j, 0, res, &e);
    if (!need) return;
    u32 *skip = NULL;
    if (!byimm) { skip = a64_here(A); a64_put(A, 0x34000000u | (u32)creg); }   // cbz w4, skip
    if (!rot) {
        a64_mov(A, 1, RFA, T2);
        a64_mov(A, 1, RFRES, res);
        int kind = m == ZYDIS_MNEMONIC_SHL ? JF_SHL : m == ZYDIS_MNEMONIC_SHR ? JF_SHR : JF_SAR;
        if (byimm) { a64_movz(A, 1, RFOP, (u32)JF_OP(kind, sz, cnt), 0); jc->fk = kind; jc->fsz = sz; jc->nzcv = 0; }
        else set_op_aux(jc, kind, sz, creg);
    } else {
        // CF/OF from the result: rol: CF = bit0, OF = msb ^ CF; ror: CF = msb, OF = msb ^ msb-1
        if (m == ZYDIS_MNEMONIC_ROL) { a64_ubfx(A, 1, T2, res, 0, 1); a64_ubfx(A, 1, T1, res, w - 1, 1); }
        else { a64_ubfx(A, 1, T2, res, w - 1, 1); a64_ubfx(A, 1, T1, res, w - 2, 1); }
        a64_eor(A, 1, T1, T1, T2);
        a64_log_reg(A, 1, 1, 0, T2, T2, T1, SH_LSL, 11);
        partial_commit(jc, merge, FL_CF | FL_OF, T2);
    }
    if (skip) { a64_patch_cbz(skip, a64_here(A)); jc->fk = -1; jc->nzcv = 0; }
}

static void x_bt(JComp *jc, JInsn *j) {
    ZydisMnemonic m = MN(j);
    const ZydisDecodedOperand *d = OPN(j, 0), *s = OPN(j, 1);
    int w = d->size, sz = szc(w);
    int need = (j->live_after & FL_CF) != 0;
    int merge = need ? partial_prepare(jc, j, FL_CF) : 0;
    int lock = (j->in.attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0;
    EA e = { 0 };
    if (ISMEM(d)) e = ea_of(jc, j, d);
    int bitreg = -1, bit = 0;
    if (ISIMM(s)) bit = (int)(s->imm.value.u & (w - 1));
    else { a64_log_imm(A, 1, 0, T4, GR(gidx(s)), (u64)(w - 1)); bitreg = T4; }
    int v;
    if (ISMEM(d) && lock && m != ZYDIS_MNEMONIC_BT) {
        int ar = ea_reg(jc, e);
        a64_mov_imm(A, T2, 1ull << bit);
        a64_ldop_al(A, sz, m == ZYDIS_MNEMONIC_BTS ? 3 : m == ZYDIS_MNEMONIC_BTR ? 1 : 2, T2, T1, ar);
        v = T1;
    } else {
        v = rd_val(jc, j, 0, T1, &e, 0);
        if (m != ZYDIS_MNEMONIC_BT) {
            if (bitreg < 0) a64_mov_imm(A, T2, 1ull << bit);
            else { a64_movz(A, 1, T2, 1, 0); a64_lslv(A, 1, T2, T2, bitreg); }
            if (m == ZYDIS_MNEMONIC_BTS) a64_orr(A, 1, T3, v, T2);
            else if (m == ZYDIS_MNEMONIC_BTR) a64_bic(A, 1, T3, v, T2);
            else a64_eor(A, 1, T3, v, T2);
            if (v != T1) { a64_mov(A, 1, T1, v); v = T1; }   // keep the old value for CF
            wr_val(jc, j, 0, T3, &e);
        }
    }
    if (!need) return;
    if (bitreg < 0) a64_ubfx(A, 1, T2, v, bit, 1);
    else { a64_lsrv(A, 1, T2, v, bitreg); a64_log_imm(A, 1, 0, T2, T2, 1); }
    partial_commit(jc, merge, FL_CF, T2);
}

static void x_bitscan(JComp *jc, JInsn *j) {
    ZydisMnemonic m = MN(j);
    const ZydisDecodedOperand *d = OPN(j, 0);
    int w = d->size, sf = w == 64;
    u16 wr = (m == ZYDIS_MNEMONIC_BSF || m == ZYDIS_MNEMONIC_BSR) ? FL_ZF : m == ZYDIS_MNEMONIC_POPCNT ? F6 : (FL_CF | FL_ZF);
    int need = (j->live_after & wr) != 0;
    int merge = (need && m != ZYDIS_MNEMONIC_POPCNT) ? partial_prepare(jc, j, wr) : 0;
    EA e = { 0 };
    if (ISMEM(OPN(j, 1))) e = ea_of(jc, j, OPN(j, 1));
    int v = rd_val(jc, j, 1, T1, &e, 1);
    if (v != T1) { a64_mov(A, 1, T1, v); v = T1; }
    int dst = GR(gidx(d));
    switch (m) {
    case ZYDIS_MNEMONIC_BSF: case ZYDIS_MNEMONIC_BSR:
        if (m == ZYDIS_MNEMONIC_BSF) { a64_rbit(A, 1, T3, v); a64_clz(A, 1, T3, T3); }
        else { a64_clz(A, 1, T3, v); a64_log_imm(A, 1, 2, T3, T3, 63); }
        a64_cmp(A, 1, v, XZR);
        if (w >= 32) a64_csel(A, 1, dst, dst, T3, C_EQ);
        else { a64_csel(A, 1, T3, dst, T3, C_EQ); a64_bfi(A, 1, dst, T3, 0, 16); }
        if (need) { a64_cset(A, 1, T2, C_EQ); a64_lsl_imm(A, 1, T2, T2, 6); partial_commit(jc, merge, FL_ZF, T2); }
        return;
    case ZYDIS_MNEMONIC_TZCNT:
        if (w == 16) { a64_log_imm(A, 0, 1, T3, v, 0x10000); a64_rbit(A, 0, T3, T3); a64_clz(A, 0, T3, T3); }
        else { a64_rbit(A, sf, T3, v); a64_clz(A, sf, T3, T3); }
        break;
    case ZYDIS_MNEMONIC_LZCNT:
        if (w == 16) { a64_lsl_imm(A, 0, T3, v, 16); a64_log_imm(A, 0, 1, T3, T3, 0x8000); a64_clz(A, 0, T3, T3); }
        else a64_clz(A, sf, T3, v);
        break;
    default:   // popcnt
        a64_fmov_from_gp(A, 1, 0, v);
        a64_vcnt(A, 0, 0, 0);
        a64_vaddv_b(A, 0, 0, 0);
        a64_fmov_to_gp(A, 0, T3, 0);
        break;
    }
    wr_val(jc, j, 0, T3, NULL);
    if (!need) return;
    if (m == ZYDIS_MNEMONIC_POPCNT) {
        a64_cmp(A, 1, v, XZR); a64_cset(A, 1, T2, C_EQ); a64_lsl_imm(A, 1, T2, T2, 6);
        partial_commit(jc, 0, F6, T2);
        return;
    }
    a64_cmp(A, 1, v, XZR); a64_cset(A, 1, T2, C_EQ);     // CF = src == 0
    a64_cmp(A, 1, T3, XZR); a64_cset(A, 1, T4, C_EQ);    // ZF = result == 0
    a64_log_reg(A, 1, 1, 0, T2, T2, T4, SH_LSL, 6);
    partial_commit(jc, merge, FL_CF | FL_ZF, T2);
}

static void x_flagops(JComp *jc, JInsn *j) {
    ZydisMnemonic m = MN(j);
    int rax = GR(RAX);
    switch (m) {
    case ZYDIS_MNEMONIC_CLC: case ZYDIS_MNEMONIC_STC: {
        if (!(j->live_after & FL_CF)) return;
        int merge = partial_prepare(jc, j, FL_CF);
        a64_movz(A, 1, T2, m == ZYDIS_MNEMONIC_STC, 0);
        partial_commit(jc, merge, FL_CF, T2);
        return; }
    case ZYDIS_MNEMONIC_CMC: {
        if (!(j->live_after & FL_CF)) return;
        get_cf(jc, T2);
        int merge = partial_prepare(jc, j, FL_CF);
        if (merge) get_cf(jc, T2);   // materialization clobbered T2
        a64_log_imm(A, 1, 2, T2, T2, 1);
        partial_commit(jc, merge, FL_CF, T2);
        return; }
    case ZYDIS_MNEMONIC_LAHF:
        call_mat(jc);
        a64_mov_imm(A, T2, 0xD5);
        a64_and(A, 1, T2, T2, RFRES);
        a64_log_imm(A, 1, 1, T2, T2, 2);
        a64_bfi(A, 1, rax, T2, 8, 8);
        return;
    default: {   // SAHF
        u16 mask = F6 & ~FL_OF;
        if (!(j->live_after & mask)) return;
        int merge = partial_prepare(jc, j, mask);
        a64_ubfx(A, 1, T2, rax, 8, 8);
        a64_mov_imm(A, T3, 0xD5);
        a64_and(A, 1, T2, T2, T3);
        partial_commit(jc, merge, mask, T2);
        return; }
    }
}

static void x_setcc(JComp *jc, JInsn *j) {
    int c = cond_arm(jc, j->in.opcode & 15);
    a64_cset(A, 1, T3, c);
    EA e = { 0 };
    if (ISMEM(OPN(j, 0))) e = ea_of(jc, j, OPN(j, 0));
    wr_val(jc, j, 0, T3, &e);
}
static void x_cmov(JComp *jc, JInsn *j) {
    const ZydisDecodedOperand *d = OPN(j, 0);
    int w = d->size;
    int c = cond_arm(jc, j->in.opcode & 15);
    EA e = { 0 };
    if (ISMEM(OPN(j, 1))) e = ea_of(jc, j, OPN(j, 1));
    int v = rd_val(jc, j, 1, T1, &e, 0);
    int dst = GR(gidx(d));
    if (w == 64) a64_csel(A, 1, dst, v, dst, c);
    else if (w == 32) a64_csel(A, 0, dst, v, dst, c);
    else { a64_csel(A, 0, T3, v, dst, c); a64_bfi(A, 1, dst, T3, 0, 16); }
}

// ---- control flow ------------------------------------------------------------------------------------------------
static u64 branch_imm_target(JInsn *j) { return NEXT(j) + (u64)OPN(j, 0)->imm.value.s; }

// inline fast-table lookup of x0; falls through on a miss
static void emit_fast_lookup(JComp *jc) {
    a64_ldr(A, 1, T3, XSP, FR_FAST);
    a64_log_reg(A, 1, 2, 0, T4, 0, 0, SH_LSR, 12);
    a64_log_imm(A, 1, 0, T4, T4, 0xFFFF);
    a64_addsub_reg(A, 1, 0, 0, T3, T3, T4, SH_LSL, 4);
    a64_ldp(A, T4, T5, T3, 0);
    a64_cmp(A, 1, T4, 0);
    a64_put(A, 0x54000000u | (2u << 5) | C_NE);
    a64_br(A, T5);
    jc->nzcv = 0;
}

static int x_branch(JComp *jc, JInsn *j) {
    ZydisMnemonic m = MN(j);
    const ZydisDecodedOperand *o = OPN(j, 0);
    int rsp = GR(RSP);
    switch (m) {
    case ZYDIS_MNEMONIC_JMP:
        if (ISIMM(o)) { jit_emit_exit_direct(jc, branch_imm_target(j), XS_LINK); return 1; }
        {
            jit_xflush(jc);
            EA e = { 0 };
            if (ISMEM(o)) e = ea_of(jc, j, o);
            int t = rd_val(jc, j, 0, T1, &e, 0);
            a64_mov(A, 1, 0, t);
            emit_fast_lookup(jc);
            a64_mov(A, 1, 1, 0);
            a64_ldr(A, 1, 0, XSP, FR_JS);
            jit_emit_call_c(jc, (void *)rt_jit_h_native_jmp);
            // 0: dispatch to the target, 1: continue at the popped return address, 2: host return
            a64_addsub_imm(A, 1, 1, 1, XZR, 0, 2, 0);
            a64_movz(A, 1, 2, EXIT_RET, 0);
            a64_movz(A, 1, T5, EXIT_HOSTRET, 0);
            a64_csel(A, 1, 2, T5, 2, C_EQ);
            a64_ldr(A, 1, T3, XSP, FR_JS);
            a64_ldr(A, 1, 0, T3, __builtin_offsetof(JState, exit_pc));
            a64_movz(A, 1, 1, 0, 0);
            a64_b(A, rt_jit_stubs.exit);
            return 1;
        }
    case ZYDIS_MNEMONIC_CALL: {
        u64 ra = NEXT(j);
        if (ISIMM(o)) {
            u64 t = branch_imm_target(j);
            a64_mov_imm(A, T1, ra);
            a64_ldst_simm(A, 3, 0, T1, rsp, -8, 3);
            if (!rt_interp_is_exec(t) && rt_jit_is_native(t)) {
                GuestFn fn = rt_lookup_fn(t);
                a64_mov(A, 1, 0, RCTX);
                if (fn) jit_emit_call_c(jc, (void *)fn);           // recompiled function: fn(c), its ret pops [rsp]
                else {                                             // import thunk
                    a64_mov_imm(A, 1, t);
                    jit_emit_call_c(jc, (void *)rt_try_native);
                }
                return 0;
            }
            jit_emit_exit_direct(jc, t, XS_LINK);
            return 1;
        }
        jit_xflush(jc);
        jc->xcached = 0;   // (a translated callee may change any xmm register before we continue here)
        EA e = { 0 };
        if (ISMEM(o)) e = ea_of(jc, j, o);
        int t = rd_val(jc, j, 0, T1, &e, 0);
        a64_mov(A, 1, 0, t);
        a64_mov_imm(A, T1, ra);
        a64_ldst_simm(A, 3, 0, T1, rsp, -8, 3);
        emit_fast_lookup(jc);
        a64_mov(A, 1, 1, 0);
        a64_ldr(A, 1, 0, XSP, FR_JS);
        jit_emit_call_c(jc, (void *)rt_jit_h_call);
        u32 *at = a64_here(A); a64_put(A, 0xB4000000u | 0);   // cbz x0: not native -> dispatch to the target
        JExit *x = add_exit(jc, XS_HELPER, 0, at);
        (void)x;
        return 0; }
    case ZYDIS_MNEMONIC_RET: {
        jit_xflush(jc);
        u64 extra = NV(j) ? (OPN(j, 0)->imm.value.u & 0xFFFF) : 0;
        a64_ldst_simm(A, 3, 1, 0, rsp, 8, 1);
        if (extra) {
            if (extra < 4096) a64_addsub_imm(A, 1, 0, 0, rsp, rsp, (u32)extra, 0);
            else { a64_mov_imm(A, T1, extra); a64_add(A, 1, rsp, rsp, T1); }
        }
        a64_ldr(A, 1, T3, XSP, FR_ENTRY_RSP);
        a64_cmp(A, 1, rsp, T3);
        u32 *at = a64_here(A); a64_put(A, 0x54000000u | C_HI);
        add_exit(jc, XS_HOSTRET, 0, at);
        emit_fast_lookup(jc);
        a64_movz(A, 1, 1, 0, 0);
        a64_movz(A, 1, 2, EXIT_RET, 0);
        a64_b(A, rt_jit_stubs.exit);
        jc->nzcv = 0;
        return 1; }
    case ZYDIS_MNEMONIC_JRCXZ: case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_LOOP: {
        u64 t = branch_imm_target(j);
        int rcx = GR(RCX), a32 = j->in.address_width == 32;
        if (m == ZYDIS_MNEMONIC_LOOP) {
            if (a32) a64_addsub_imm(A, 0, 1, 0, rcx, rcx, 1, 0); else a64_addsub_imm(A, 1, 1, 0, rcx, rcx, 1, 0);
        }
        int nz = m == ZYDIS_MNEMONIC_LOOP;
        int sf = !(m == ZYDIS_MNEMONIC_JECXZ || a32);
        if (t == jc->pc) loop_branch(jc, (u32)sf << 31 | 0x34000000u | (u32)nz << 24 | (u32)rcx);
        else cbz_exit(jc, sf, nz, rcx, XS_LINK, t);
        return 0; }
    default: {   // jcc
        u64 t = branch_imm_target(j);
        int c = cond_arm(jc, j->in.opcode & 15);
        if (t == jc->pc) loop_branch(jc, 0x54000000u | (u32)c);
        else bcond_exit(jc, c, XS_LINK, t);
        return 0; }
    }
}

// ---- dispatch ----------------------------------------------------------------------------------------------------
void jit_emit_fallback(JComp *jc, JInsn *j) {
    a64_ldr(A, 1, 0, XSP, FR_JS);
    a64_mov_imm(A, 1, j->pc);
    a64_mov(A, 1, 2, RFOP); a64_mov(A, 1, 3, RFA); a64_mov(A, 1, 4, RFRES);
    jit_emit_call_c(jc, (void *)rt_jit_h_interp);
    u32 *at = a64_here(A); a64_put(A, 0xB5000000u | 0);   // cbnz x0 -> exit with the helper's reason
    add_exit(jc, XS_HELPER, 0, at);
    a64_ldr(A, 1, T3, XSP, FR_JS);
    a64_ldr(A, 1, RFRES, T3, __builtin_offsetof(JState, rflags));
    a64_movz(A, 1, RFOP, 0, 0);
    jc->fk = JF_MAT; jc->nzcv = 0;
}

int jit_translate_insn(JComp *jc, JInsn *j) {
    ZydisMnemonic m = MN(j);
    switch (m) {
    case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_PAUSE: case ZYDIS_MNEMONIC_ENDBR64: case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1: case ZYDIS_MNEMONIC_PREFETCHT2: case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHW:
    case ZYDIS_MNEMONIC_PREFETCH:
        return 0;
    case ZYDIS_MNEMONIC_LFENCE: case ZYDIS_MNEMONIC_MFENCE: case ZYDIS_MNEMONIC_SFENCE: a64_dmb_ish(A); return 0;
    case ZYDIS_MNEMONIC_MOV: x_mov(jc, j); return 0;
    case ZYDIS_MNEMONIC_MOVZX: case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD: x_movx(jc, j); return 0;
    case ZYDIS_MNEMONIC_LEA: x_lea(jc, j); return 0;
    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_TEST:
        x_alu2(jc, j); return 0;
    case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_NEG: case ZYDIS_MNEMONIC_NOT: x_alu1(jc, j); return 0;
    case ZYDIS_MNEMONIC_XCHG: x_xchg(jc, j); return 0;
    case ZYDIS_MNEMONIC_XADD: x_xadd(jc, j); return 0;
    case ZYDIS_MNEMONIC_CMPXCHG: x_cmpxchg(jc, j); return 0;
    case ZYDIS_MNEMONIC_PUSH: x_push(jc, j); return 0;
    case ZYDIS_MNEMONIC_POP: x_pop(jc, j); return 0;
    case ZYDIS_MNEMONIC_LEAVE: {
        int rsp = GR(RSP), rbp = GR(RBP);
        a64_mov(A, 1, rsp, rbp);
        a64_ldst_simm(A, 3, 1, rbp, rsp, 8, 1);
        return 0; }
    case ZYDIS_MNEMONIC_CBW: case ZYDIS_MNEMONIC_CWDE: case ZYDIS_MNEMONIC_CDQE: case ZYDIS_MNEMONIC_CWD:
    case ZYDIS_MNEMONIC_CDQ: case ZYDIS_MNEMONIC_CQO:
        x_cvt(jc, j); return 0;
    case ZYDIS_MNEMONIC_IMUL: if (NV(j) == 1) x_mul1(jc, j); else x_imul(jc, j); return 0;
    case ZYDIS_MNEMONIC_MUL: x_mul1(jc, j); return 0;
    case ZYDIS_MNEMONIC_DIV: case ZYDIS_MNEMONIC_IDIV: x_div(jc, j); return 0;
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR:
        x_shift(jc, j); return 0;
    case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR: case ZYDIS_MNEMONIC_BTC: x_bt(jc, j); return 0;
    case ZYDIS_MNEMONIC_BSF: case ZYDIS_MNEMONIC_BSR: case ZYDIS_MNEMONIC_TZCNT: case ZYDIS_MNEMONIC_LZCNT: case ZYDIS_MNEMONIC_POPCNT:
        x_bitscan(jc, j); return 0;
    case ZYDIS_MNEMONIC_BSWAP: { int r = GR(gidx(OPN(j, 0))); a64_rev(A, OPN(j, 0)->size == 64, r, r); return 0; }
    case ZYDIS_MNEMONIC_CLC: case ZYDIS_MNEMONIC_STC: case ZYDIS_MNEMONIC_CMC: case ZYDIS_MNEMONIC_LAHF: case ZYDIS_MNEMONIC_SAHF:
        x_flagops(jc, j); return 0;
    case ZYDIS_MNEMONIC_JMP: case ZYDIS_MNEMONIC_CALL: case ZYDIS_MNEMONIC_RET: case ZYDIS_MNEMONIC_JRCXZ: case ZYDIS_MNEMONIC_JECXZ:
    case ZYDIS_MNEMONIC_LOOP:
        return x_branch(jc, j);
    default: break;
    }
    if (j->in.meta.category == ZYDIS_CATEGORY_COND_BR) return x_branch(jc, j);
    if (j->in.meta.category == ZYDIS_CATEGORY_SETCC) { x_setcc(jc, j); return 0; }
    if (j->in.meta.category == ZYDIS_CATEGORY_CMOV) { x_cmov(jc, j); return 0; }
    if (!jit_sse_translate(jc, j)) jc->fail = 1;
    return 0;
}

// ---- trace driver ------------------------------------------------------------------------------------------------
static int ends_trace(const JInsn *j) {
    const ZydisDecodedInstruction *in = &j->in;
    switch (in->meta.category) {
    case ZYDIS_CATEGORY_UNCOND_BR: case ZYDIS_CATEGORY_RET: case ZYDIS_CATEGORY_INTERRUPT: case ZYDIS_CATEGORY_SYSCALL:
    case ZYDIS_CATEGORY_SYSRET:
        return 1;
    case ZYDIS_CATEGORY_CALL:   // a direct call into guest code leaves the trace; native and indirect calls may return to it
        if (j->op[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            u64 t = NEXT(j) + (u64)j->op[0].imm.value.s;
            return rt_interp_is_exec(t) || !rt_jit_is_native(t);
        }
        return j->fallback;
    default: break;
    }
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_UD2: case ZYDIS_MNEMONIC_HLT: case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_IRETQ: case ZYDIS_MNEMONIC_IRETD:
        return 1;
    default: return 0;
    }
}

static void emit_stub(JComp *jc, JExit *x) {
    u32 *here = a64_here(A);
    if (x->branch) {
        u32 w = *x->branch;
        if ((w >> 26) == 0x05) a64_patch_b(x->branch, here);                       // b
        else a64_patch_cbz(x->branch, here);                                        // b.cond / cbz / cbnz: imm19 at [23:5]
    }
    if (x->kind == XS_LOOP && jc->loopL) { jit_xload(jc, x->xreload); a64_b(A, jc->gate); return; }
    jc->xdirty = x->xdirty;
    jit_xflush(jc);
    jc->xcached = 0;
    switch (x->kind) {
    case XS_LINK: emit_link_exit_seq(jc, x->target); break;
    case XS_LOOP: a64_b(A, jc->entry); break;
    case XS_HOSTRET: a64_movz(A, 1, 2, EXIT_HOSTRET, 0); a64_b(A, rt_jit_stubs.exit); break;
    case XS_HELPER:
        // helper results: rt_jit_h_interp gives the exit reason; rt_jit_h_call gives 0 (= dispatch to exit_pc)
        a64_cbz(A, 1, 1, 0, a64_here(A) + 2);
        a64_movz(A, 1, 0, EXIT_RET, 0);
        emit_helper_exit_seq(jc);
        break;
    case XS_SLOW:
        jc->fk = -1;
        jit_emit_fallback(jc, x->insn);
        jit_xload(jc, x->xreload);
        a64_b(A, x->resume);
        break;
    }
}

int jit_compile_trace(JComp *jc, u64 pc, int checked) {
    pthread_once(&dec_once, dec_init);
    u64 p = pc;
    u64 limit = (pc & ~(u64)(getpagesize() - 1)) + 2 * (u64)getpagesize();
    int n = 0;
    while (n < JMAX_INSN) {
        JInsn *j = &jc->ins[n];
        if (p + 15 > limit && n) break;
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, (const void *)p, 15, &j->in, j->op))) break;
        j->pc = p;
        j->fallback = !jit_classify(j);
        n++;
        p += j->in.length;
        if (ends_trace(j)) break;
    }
    jc->n = n;
    if (!n) return 0;
    // An instruction run through the interpreter from translated code costs more than interpreting it outright
    // (guest state sync + helper), so traces dominated by such instructions stay in the interpreter.
    int nfb = 0;
    for (int i = 0; i < n; i++) nfb += jc->ins[i].fallback;
    if (jc->reject_fallback_heavy && nfb * 3 > n) { jc->rejected = 1; jc->fail = 1; return 0; }
    // flag liveness, backwards: at side exits and the end of the trace every flag is live
    u16 live = F6;
    for (int i = n - 1; i >= 0; i--) {
        JInsn *j = &jc->ins[i];
        u16 rd, wr;
        jit_flag_use(j, &rd, &wr);
        if (j->in.meta.category == ZYDIS_CATEGORY_COND_BR || (MN(j) == ZYDIS_MNEMONIC_CALL && !j->fallback) ||
            MN(j) == ZYDIS_MNEMONIC_DIV || MN(j) == ZYDIS_MNEMONIC_IDIV || (!j->fallback && jit_sse_has_slow_path(j)))
            live = F6;   // side exit / helper exit / slow path: everything live afterwards
        j->live_after = live;
        live = (u16)((live & ~wr) | rd);
    }
    // emit
    A64 *a = A;
    jc->entry = a64_here(a);
    a64_put(a, a64_b_enc(jc->entry, jc->entry + 1));   // patch site for invalidation (b .+4 -> b stale)
    jc->fk = -1; jc->nzcv = 0; jc->xcached = jc->xdirty = 0;
    u32 *check_branches[64]; int ncheck = 0;
    if (checked) {
        u64 end = jc->ins[n - 1].pc + jc->ins[n - 1].in.length, len = end - pc;
        a64_mov_imm(a, T16, pc);
        for (u64 off = 0; off < len;) {
            int sz = len - off >= 8 ? 3 : len - off >= 4 ? 2 : len - off >= 2 ? 1 : 0;
            u64 v = 0; memcpy(&v, (const void *)(pc + off), 1u << sz);
            a64_ldst_any(a, sz, 1, T0, T16, (i64)off, T17);
            a64_mov_imm(a, T1, v);
            a64_cmp(a, 1, T0, T1);
            if (ncheck < 64) { check_branches[ncheck++] = a64_here(a); a64_put(a, 0x54000000u | C_NE); }
            else { jc->fail = 1; return 0; }
            off += 1u << sz;
        }
    }
    // loop traces: preload the xmm registers the loop uses, back edges continue at the gate
    jc->loopL = jc->loopD = 0; jc->gate = jc->gate_stale = NULL;
    if (!checked) {
        int loop = 0; u16 L = 0, D = 0;
        for (int i = 0; i < n; i++) {
            JInsn *j = &jc->ins[i];
            if (j->fallback) continue;
            const ZydisDecodedOperand *o = &j->op[0];
            if ((j->in.meta.category == ZYDIS_CATEGORY_COND_BR || j->in.meta.category == ZYDIS_CATEGORY_UNCOND_BR) &&
                o->type == ZYDIS_OPERAND_TYPE_IMMEDIATE && NEXT(j) + (u64)o->imm.value.s == pc) loop = 1;
            for (int k = 0; k < j->in.operand_count; k++) {
                const ZydisDecodedOperand *q = &j->op[k];
                if (q->type != ZYDIS_OPERAND_TYPE_REGISTER || rt_ireg[q->reg.value].kind != RK_XMM) continue;
                L |= (u16)(1u << rt_ireg[q->reg.value].idx);
                if (q->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) D |= (u16)(1u << rt_ireg[q->reg.value].idx);
            }
        }
        if (loop && L) {
            jit_xload(jc, L);
            jc->loopL = L; jc->loopD = D;
            jc->xcached = L; jc->xdirty = D;
            jc->gate = a64_here(a);
            a64_put(a, a64_b_enc(jc->gate, jc->gate + 1));   // b .+4 (patched to the stale path on invalidation)
        }
    }
    if (jc->trace_mode && jc->counter) {
        {
            a64_mov_imm(a, T16, (u64)jc->counter);
            a64_ldr(a, 1, T17, T16, 0);
            a64_addsub_imm(a, 1, 0, 0, T17, T17, 1, 0);
            a64_str(a, 1, T17, T16, 0);
        }
    }
    jc->body = a64_here(a);
    int term = 0;
    for (int i = 0; i < n && !jc->fail; i++) {
        JInsn *j = &jc->ins[i];
        jc->cur = i;
        if (j->fallback) {
            jit_emit_fallback(jc, j);
            if (ends_trace(j)) { jit_emit_exit_direct(jc, NEXT(j), XS_LINK); term = 1; break; }
        } else if (jit_translate_insn(jc, j)) { term = 1; break; }
        if (a->overflow) { jc->fail = 1; break; }
    }
    if (jc->fail) return 0;
    if (!term) jit_emit_exit_direct(jc, NEXT(&jc->ins[n - 1]), XS_LINK);
    for (int i = 0; i < jc->nexit; i++) emit_stub(jc, &jc->exits[i]);   // XS_SLOW may add more exits: loop re-reads nexit
    jc->stale = a64_here(a);
    a64_mov_imm(a, 0, pc);
    a64_movz(a, 1, 1, 0, 0);
    a64_movz(a, 1, 2, EXIT_STALE, 0);
    a64_b(a, rt_jit_stubs.exit);
    if (jc->gate) {   // an invalidated loop trace: write its xmm registers back, then leave like the entry would
        jc->gate_stale = a64_here(a);
        jc->xdirty = jc->loopD;
        jit_xflush(jc);
        a64_b(a, jc->stale);
    }
    for (int i = 0; i < ncheck; i++) a64_patch_bcond(check_branches[i], C_NE, jc->stale);
    return !a->overflow && !jc->fail;
}
