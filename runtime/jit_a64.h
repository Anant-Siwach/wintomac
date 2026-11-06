// Minimal AArch64 machine-code emitter for the runtime JIT (runtime/jit*.c).
//
// Encodings follow the Arm Architecture Reference Manual (A64); the set is what the x86-64 translator
// needs: integer data processing, loads/stores incl. RCpc (LDAPUR/STLUR) and LSE atomics, branches, and a
// NEON subset for SSE. tests: `make jit-a64-selftest` compares every encoder against clang's assembler.
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct A64 {
    uint32_t *p, *start, *end;   // write cursor, buffer start, buffer end (overflow is checked by callers)
    int overflow;
} A64;

static inline void a64_put(A64 *a, uint32_t w) {
    if (a->p >= a->end) { a->overflow = 1; return; }
    *a->p++ = w;
}
static inline uint32_t *a64_here(A64 *a) { return a->p; }

enum { XZR = 31, WZR = 31, XSP = 31 };
enum { C_EQ, C_NE, C_HS, C_LO, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL, C_NV };
#define C_CS C_HS
#define C_CC C_LO
enum { SH_LSL, SH_LSR, SH_ASR, SH_ROR };

#define SF(x) ((uint32_t)(x) << 31)

// ---- move wide / immediates --------------------------------------------------------------------------------
static inline void a64_movz(A64 *a, int sf, int rd, uint32_t imm16, int hw) { a64_put(a, SF(sf) | 0x52800000u | (uint32_t)hw << 21 | (imm16 & 0xFFFF) << 5 | rd); }
static inline void a64_movk(A64 *a, int sf, int rd, uint32_t imm16, int hw) { a64_put(a, SF(sf) | 0x72800000u | (uint32_t)hw << 21 | (imm16 & 0xFFFF) << 5 | rd); }
static inline void a64_movn(A64 *a, int sf, int rd, uint32_t imm16, int hw) { a64_put(a, SF(sf) | 0x12800000u | (uint32_t)hw << 21 | (imm16 & 0xFFFF) << 5 | rd); }

// Bitmask-immediate encoding (logical instructions). Returns -1 when v is not encodable.
static inline int a64_logimm(uint64_t v, int sf, uint32_t *nrs) {
    if (!sf) { v &= 0xFFFFFFFFull; v |= v << 32; }
    if (v == 0 || v == ~0ull) return -1;
    int size = 64;
    for (;;) {   // smallest repeating element
        int h = size / 2;
        uint64_t m = (1ull << h) - 1;
        if (h < 2 || ((v >> h) & m) != (v & m)) break;
        size = h;
    }
    uint64_t emask = size == 64 ? ~0ull : (1ull << size) - 1;
    uint64_t e = v & emask;
    // rotate e right until it is of the form 0..01..1
    int rot = -1, ones = __builtin_popcountll(e);
    for (int r = 0; r < size; r++) {
        uint64_t x = r ? ((e >> r) | (e << (size - r))) & emask : e;
        if (x == ((ones == 64) ? ~0ull : (1ull << ones) - 1)) { rot = r; break; }
    }
    if (rot < 0) return -1;
    uint32_t immr = (uint32_t)((size - rot) % size);
    uint32_t imms = (uint32_t)((~(size - 1) << 1) | (ones - 1)) & 0x3F;
    uint32_t n = size == 64;
    if (!sf && n) return -1;
    *nrs = n << 12 | immr << 6 | imms;
    return 0;
}

// Load an arbitrary 64-bit constant.
static inline void a64_mov_imm(A64 *a, int rd, uint64_t v) {
    if (v == 0) { a64_movz(a, 1, rd, 0, 0); return; }
    int nz = 0, nf = 0;
    for (int i = 0; i < 4; i++) { uint32_t h = (v >> (16 * i)) & 0xFFFF; nz += h != 0; nf += h != 0xFFFF; }
    if (nf < nz) {   // mostly ones: movn
        int first = 1;
        for (int i = 0; i < 4; i++) {
            uint32_t h = (v >> (16 * i)) & 0xFFFF;
            if (h == 0xFFFF) continue;
            if (first) { a64_movn(a, 1, rd, ~h & 0xFFFF, i); first = 0; }
            else a64_movk(a, 1, rd, h, i);
        }
        if (first) a64_movn(a, 1, rd, 0, 0);
        return;
    }
    uint32_t nrs;
    if (nz > 2 && a64_logimm(v, 1, &nrs) == 0) { a64_put(a, 0xB2000000u | nrs << 10 | XZR << 5 | rd); return; }   // orr rd, xzr, #imm
    int first = 1;
    for (int i = 0; i < 4; i++) {
        uint32_t h = (v >> (16 * i)) & 0xFFFF;
        if (!h) continue;
        if (first) { a64_movz(a, 1, rd, h, i); first = 0; }
        else a64_movk(a, 1, rd, h, i);
    }
}
// Fixed-length (4 instruction) 64-bit constant: patchable / position independent sizes.
static inline void a64_mov_imm64_fixed(A64 *a, int rd, uint64_t v) {
    a64_movz(a, 1, rd, v & 0xFFFF, 0);
    a64_movk(a, 1, rd, (v >> 16) & 0xFFFF, 1);
    a64_movk(a, 1, rd, (v >> 32) & 0xFFFF, 2);
    a64_movk(a, 1, rd, (v >> 48) & 0xFFFF, 3);
}

// ---- add/sub ---------------------------------------------------------------------------------------------
// op: 0 add, 1 sub; s: set flags
static inline void a64_addsub_imm(A64 *a, int sf, int op, int s, int rd, int rn, uint32_t imm12, int sh12) {
    a64_put(a, SF(sf) | (uint32_t)op << 30 | (uint32_t)s << 29 | 0x11000000u | (uint32_t)sh12 << 22 | (imm12 & 0xFFF) << 10 | rn << 5 | rd);
}
static inline void a64_addsub_reg(A64 *a, int sf, int op, int s, int rd, int rn, int rm, int shift, int amt) {
    a64_put(a, SF(sf) | (uint32_t)op << 30 | (uint32_t)s << 29 | 0x0B000000u | (uint32_t)shift << 22 | (uint32_t)rm << 16 | (uint32_t)(amt & 63) << 10 | rn << 5 | rd);
}
// extended register form (needed when rn/rd is sp): option 3 = UXTX/LSL
static inline void a64_addsub_ext(A64 *a, int sf, int op, int s, int rd, int rn, int rm, int option, int amt) {
    a64_put(a, SF(sf) | (uint32_t)op << 30 | (uint32_t)s << 29 | 0x0B200000u | (uint32_t)rm << 16 | (uint32_t)option << 13 | (uint32_t)amt << 10 | rn << 5 | rd);
}
static inline void a64_add(A64 *a, int sf, int rd, int rn, int rm) { a64_addsub_reg(a, sf, 0, 0, rd, rn, rm, 0, 0); }
static inline void a64_sub(A64 *a, int sf, int rd, int rn, int rm) { a64_addsub_reg(a, sf, 1, 0, rd, rn, rm, 0, 0); }
static inline void a64_adds(A64 *a, int sf, int rd, int rn, int rm) { a64_addsub_reg(a, sf, 0, 1, rd, rn, rm, 0, 0); }
static inline void a64_subs(A64 *a, int sf, int rd, int rn, int rm) { a64_addsub_reg(a, sf, 1, 1, rd, rn, rm, 0, 0); }
static inline void a64_cmp(A64 *a, int sf, int rn, int rm) { a64_subs(a, sf, XZR, rn, rm); }
static inline void a64_cmn(A64 *a, int sf, int rn, int rm) { a64_adds(a, sf, XZR, rn, rm); }
static inline void a64_adc(A64 *a, int sf, int rd, int rn, int rm) { a64_put(a, SF(sf) | 0x1A000000u | rm << 16 | rn << 5 | rd); }
static inline void a64_sbc(A64 *a, int sf, int rd, int rn, int rm) { a64_put(a, SF(sf) | 0x5A000000u | rm << 16 | rn << 5 | rd); }

// ---- logical -----------------------------------------------------------------------------------------------
// opc: 0 and, 1 orr, 2 eor, 3 ands; n: invert rm (bic/orn/eon/bics)
static inline void a64_log_reg(A64 *a, int sf, int opc, int n, int rd, int rn, int rm, int shift, int amt) {
    a64_put(a, SF(sf) | (uint32_t)opc << 29 | 0x0A000000u | (uint32_t)shift << 22 | (uint32_t)n << 21 | (uint32_t)rm << 16 | (uint32_t)(amt & 63) << 10 | rn << 5 | rd);
}
static inline void a64_and(A64 *a, int sf, int rd, int rn, int rm) { a64_log_reg(a, sf, 0, 0, rd, rn, rm, 0, 0); }
static inline void a64_orr(A64 *a, int sf, int rd, int rn, int rm) { a64_log_reg(a, sf, 1, 0, rd, rn, rm, 0, 0); }
static inline void a64_eor(A64 *a, int sf, int rd, int rn, int rm) { a64_log_reg(a, sf, 2, 0, rd, rn, rm, 0, 0); }
static inline void a64_ands(A64 *a, int sf, int rd, int rn, int rm) { a64_log_reg(a, sf, 3, 0, rd, rn, rm, 0, 0); }
static inline void a64_bic(A64 *a, int sf, int rd, int rn, int rm) { a64_log_reg(a, sf, 0, 1, rd, rn, rm, 0, 0); }
static inline void a64_tst(A64 *a, int sf, int rn, int rm) { a64_ands(a, sf, XZR, rn, rm); }
static inline void a64_mov(A64 *a, int sf, int rd, int rm) { a64_orr(a, sf, rd, XZR, rm); }
static inline void a64_mvn(A64 *a, int sf, int rd, int rm) { a64_log_reg(a, sf, 1, 1, rd, XZR, rm, 0, 0); }
// logical immediate; returns 0 if not encodable (caller materializes the constant)
static inline int a64_log_imm(A64 *a, int sf, int opc, int rd, int rn, uint64_t imm) {
    uint32_t nrs;
    if (a64_logimm(imm, sf, &nrs)) return 0;
    a64_put(a, SF(sf) | (uint32_t)opc << 29 | 0x12000000u | nrs << 10 | rn << 5 | rd);
    return 1;
}
// mov sp <-> reg
static inline void a64_mov_sp(A64 *a, int rd, int rn) { a64_addsub_imm(a, 1, 0, 0, rd, rn, 0, 0); }

// ---- bitfield / shifts ------------------------------------------------------------------------------------
// opc: 0 sbfm, 1 bfm, 2 ubfm
static inline void a64_bfm(A64 *a, int sf, int opc, int rd, int rn, int immr, int imms) {
    a64_put(a, SF(sf) | (uint32_t)opc << 29 | 0x13000000u | (uint32_t)sf << 22 | (uint32_t)immr << 16 | (uint32_t)imms << 10 | rn << 5 | rd);
}
static inline void a64_lsl_imm(A64 *a, int sf, int rd, int rn, int sh) { int w = sf ? 64 : 32; a64_bfm(a, sf, 2, rd, rn, (w - sh) % w, w - 1 - sh); }
static inline void a64_lsr_imm(A64 *a, int sf, int rd, int rn, int sh) { a64_bfm(a, sf, 2, rd, rn, sh, sf ? 63 : 31); }
static inline void a64_asr_imm(A64 *a, int sf, int rd, int rn, int sh) { a64_bfm(a, sf, 0, rd, rn, sh, sf ? 63 : 31); }
static inline void a64_ubfx(A64 *a, int sf, int rd, int rn, int lsb, int width) { a64_bfm(a, sf, 2, rd, rn, lsb, lsb + width - 1); }
static inline void a64_sbfx(A64 *a, int sf, int rd, int rn, int lsb, int width) { a64_bfm(a, sf, 0, rd, rn, lsb, lsb + width - 1); }
static inline void a64_bfi(A64 *a, int sf, int rd, int rn, int lsb, int width) { int w = sf ? 64 : 32; a64_bfm(a, sf, 1, rd, rn, (w - lsb) % w, width - 1); }
static inline void a64_bfxil(A64 *a, int sf, int rd, int rn, int lsb, int width) { a64_bfm(a, sf, 1, rd, rn, lsb, lsb + width - 1); }
static inline void a64_ubfiz(A64 *a, int sf, int rd, int rn, int lsb, int width) { int w = sf ? 64 : 32; a64_bfm(a, sf, 2, rd, rn, (w - lsb) % w, width - 1); }
static inline void a64_sxtb(A64 *a, int sf, int rd, int rn) { a64_bfm(a, sf, 0, rd, rn, 0, 7); }
static inline void a64_sxth(A64 *a, int sf, int rd, int rn) { a64_bfm(a, sf, 0, rd, rn, 0, 15); }
static inline void a64_sxtw(A64 *a, int rd, int rn) { a64_bfm(a, 1, 0, rd, rn, 0, 31); }
static inline void a64_uxtb(A64 *a, int rd, int rn) { a64_bfm(a, 0, 2, rd, rn, 0, 7); }
static inline void a64_uxth(A64 *a, int rd, int rn) { a64_bfm(a, 0, 2, rd, rn, 0, 15); }
static inline void a64_extr(A64 *a, int sf, int rd, int rn, int rm, int lsb) { a64_put(a, SF(sf) | 0x13800000u | (uint32_t)sf << 22 | (uint32_t)rm << 16 | (uint32_t)lsb << 10 | rn << 5 | rd); }
static inline void a64_ror_imm(A64 *a, int sf, int rd, int rn, int sh) { a64_extr(a, sf, rd, rn, rn, sh); }

// ---- 2-source / 1-source / 3-source -------------------------------------------------------------------------
static inline void a64_dp2(A64 *a, int sf, uint32_t opc, int rd, int rn, int rm) { a64_put(a, SF(sf) | 0x1AC00000u | (uint32_t)rm << 16 | opc << 10 | rn << 5 | rd); }
static inline void a64_udiv(A64 *a, int sf, int rd, int rn, int rm) { a64_dp2(a, sf, 0x02, rd, rn, rm); }
static inline void a64_sdiv(A64 *a, int sf, int rd, int rn, int rm) { a64_dp2(a, sf, 0x03, rd, rn, rm); }
static inline void a64_lslv(A64 *a, int sf, int rd, int rn, int rm) { a64_dp2(a, sf, 0x08, rd, rn, rm); }
static inline void a64_lsrv(A64 *a, int sf, int rd, int rn, int rm) { a64_dp2(a, sf, 0x09, rd, rn, rm); }
static inline void a64_asrv(A64 *a, int sf, int rd, int rn, int rm) { a64_dp2(a, sf, 0x0A, rd, rn, rm); }
static inline void a64_rorv(A64 *a, int sf, int rd, int rn, int rm) { a64_dp2(a, sf, 0x0B, rd, rn, rm); }
static inline void a64_dp1(A64 *a, int sf, uint32_t opc, int rd, int rn) { a64_put(a, SF(sf) | 0x5AC00000u | opc << 10 | rn << 5 | rd); }
static inline void a64_rbit(A64 *a, int sf, int rd, int rn) { a64_dp1(a, sf, 0, rd, rn); }
static inline void a64_rev16(A64 *a, int sf, int rd, int rn) { a64_dp1(a, sf, 1, rd, rn); }
static inline void a64_rev(A64 *a, int sf, int rd, int rn) { a64_dp1(a, sf, sf ? 3 : 2, rd, rn); }
static inline void a64_clz(A64 *a, int sf, int rd, int rn) { a64_dp1(a, sf, 4, rd, rn); }
static inline void a64_madd(A64 *a, int sf, int rd, int rn, int rm, int ra) { a64_put(a, SF(sf) | 0x1B000000u | (uint32_t)rm << 16 | (uint32_t)ra << 10 | rn << 5 | rd); }
static inline void a64_msub(A64 *a, int sf, int rd, int rn, int rm, int ra) { a64_put(a, SF(sf) | 0x1B008000u | (uint32_t)rm << 16 | (uint32_t)ra << 10 | rn << 5 | rd); }
static inline void a64_mul(A64 *a, int sf, int rd, int rn, int rm) { a64_madd(a, sf, rd, rn, rm, XZR); }
static inline void a64_smull(A64 *a, int rd, int rn, int rm) { a64_put(a, 0x9B200000u | (uint32_t)rm << 16 | XZR << 10 | rn << 5 | rd); }
static inline void a64_umull(A64 *a, int rd, int rn, int rm) { a64_put(a, 0x9BA00000u | (uint32_t)rm << 16 | XZR << 10 | rn << 5 | rd); }
static inline void a64_smulh(A64 *a, int rd, int rn, int rm) { a64_put(a, 0x9B400000u | (uint32_t)rm << 16 | XZR << 10 | rn << 5 | rd); }
static inline void a64_umulh(A64 *a, int rd, int rn, int rm) { a64_put(a, 0x9BC00000u | (uint32_t)rm << 16 | XZR << 10 | rn << 5 | rd); }

// ---- conditional ------------------------------------------------------------------------------------------
static inline void a64_csel(A64 *a, int sf, int rd, int rn, int rm, int cond) { a64_put(a, SF(sf) | 0x1A800000u | (uint32_t)rm << 16 | (uint32_t)cond << 12 | rn << 5 | rd); }
static inline void a64_csinc(A64 *a, int sf, int rd, int rn, int rm, int cond) { a64_put(a, SF(sf) | 0x1A800400u | (uint32_t)rm << 16 | (uint32_t)cond << 12 | rn << 5 | rd); }
static inline void a64_csinv(A64 *a, int sf, int rd, int rn, int rm, int cond) { a64_put(a, SF(sf) | 0x5A800000u | (uint32_t)rm << 16 | (uint32_t)cond << 12 | rn << 5 | rd); }
static inline void a64_csneg(A64 *a, int sf, int rd, int rn, int rm, int cond) { a64_put(a, SF(sf) | 0x5A800400u | (uint32_t)rm << 16 | (uint32_t)cond << 12 | rn << 5 | rd); }
static inline void a64_cset(A64 *a, int sf, int rd, int cond) { a64_csinc(a, sf, rd, XZR, XZR, cond ^ 1); }
static inline void a64_csetm(A64 *a, int sf, int rd, int cond) { a64_csinv(a, sf, rd, XZR, XZR, cond ^ 1); }
// ccmp rn, rm/imm5, #nzcv, cond
static inline void a64_ccmp(A64 *a, int sf, int rn, int rm, int nzcv, int cond) { a64_put(a, SF(sf) | 0x7A400000u | (uint32_t)rm << 16 | (uint32_t)cond << 12 | rn << 5 | nzcv); }
static inline void a64_ccmp_imm(A64 *a, int sf, int rn, int imm5, int nzcv, int cond) { a64_put(a, SF(sf) | 0x7A400800u | (uint32_t)imm5 << 16 | (uint32_t)cond << 12 | rn << 5 | nzcv); }
static inline void a64_mrs_nzcv(A64 *a, int rt) { a64_put(a, 0xD53B4200u | rt); }
static inline void a64_msr_nzcv(A64 *a, int rt) { a64_put(a, 0xD51B4200u | rt); }

// ---- loads / stores ---------------------------------------------------------------------------------------
// size: 0 byte, 1 half, 2 word, 3 dword. opc: 0 store, 1 load (zero-extend), 2 load signed to 64, 3 load signed to 32
static inline void a64_ldst_uimm(A64 *a, int size, int opc, int rt, int rn, uint32_t byteoff) {
    a64_put(a, 0x39000000u | (uint32_t)size << 30 | (uint32_t)opc << 22 | ((byteoff >> size) & 0xFFF) << 10 | rn << 5 | rt);
}
static inline void a64_ldst_simm(A64 *a, int size, int opc, int rt, int rn, int imm9, int mode) {   // mode: 0 unscaled, 1 post, 3 pre
    a64_put(a, 0x38000000u | (uint32_t)size << 30 | (uint32_t)opc << 22 | ((uint32_t)imm9 & 0x1FF) << 12 | (uint32_t)mode << 10 | rn << 5 | rt);
}
static inline void a64_ldst_reg(A64 *a, int size, int opc, int rt, int rn, int rm, int scaled) {   // [rn, rm, lsl #size?]
    a64_put(a, 0x38200800u | (uint32_t)size << 30 | (uint32_t)opc << 22 | (uint32_t)rm << 16 | 3u << 13 | (uint32_t)scaled << 12 | rn << 5 | rt);
}
// Plain load/store with any offset (picks uimm / simm9 / via scratch register).
static inline void a64_ldst_any(A64 *a, int size, int opc, int rt, int rn, int64_t off, int scratch) {
    if (off >= 0 && !(off & ((1 << size) - 1)) && (off >> size) < 4096) a64_ldst_uimm(a, size, opc, rt, rn, (uint32_t)off);
    else if (off >= -256 && off < 256) a64_ldst_simm(a, size, opc, rt, rn, (int)off, 0);
    else { a64_mov_imm(a, scratch, (uint64_t)off); a64_ldst_reg(a, size, opc, rt, rn, scratch, 0); }
}
static inline void a64_ldr(A64 *a, int sf, int rt, int rn, int64_t off) { a64_ldst_any(a, sf ? 3 : 2, 1, rt, rn, off, 17); }
static inline void a64_str(A64 *a, int sf, int rt, int rn, int64_t off) { a64_ldst_any(a, sf ? 3 : 2, 0, rt, rn, off, 17); }
// ldp/stp 64-bit, signed offset (mode 2), post-index (1), pre-index (3)
static inline void a64_ldstp(A64 *a, int load, int mode, int rt, int rt2, int rn, int off) {
    a64_put(a, 0xA8000000u | (uint32_t)mode << 23 | (uint32_t)load << 22 | (((uint32_t)(off / 8)) & 0x7F) << 15 | (uint32_t)rt2 << 10 | rn << 5 | rt);
}
static inline void a64_stp(A64 *a, int rt, int rt2, int rn, int off) { a64_ldstp(a, 0, 2, rt, rt2, rn, off); }
static inline void a64_ldp(A64 *a, int rt, int rt2, int rn, int off) { a64_ldstp(a, 1, 2, rt, rt2, rn, off); }
// RCpc: ldapur/stlur [rn, #simm9] (size 0..3; opc 1 load, 0 store). LDAPR/STLR without offset.
static inline void a64_ldapur(A64 *a, int size, int rt, int rn, int imm9) { a64_put(a, 0x19400000u | (uint32_t)size << 30 | ((uint32_t)imm9 & 0x1FF) << 12 | rn << 5 | rt); }
static inline void a64_stlur(A64 *a, int size, int rt, int rn, int imm9) { a64_put(a, 0x19000000u | (uint32_t)size << 30 | ((uint32_t)imm9 & 0x1FF) << 12 | rn << 5 | rt); }
static inline void a64_ldapr(A64 *a, int size, int rt, int rn) { a64_put(a, 0x38BFC000u | (uint32_t)size << 30 | rn << 5 | rt); }
static inline void a64_stlr(A64 *a, int size, int rt, int rn) { a64_put(a, 0x089FFC00u | (uint32_t)size << 30 | rn << 5 | rt); }
// LSE atomics, acquire+release. op: 0 ldadd, 1 ldclr, 2 ldeor, 3 ldset, 8 swp
static inline void a64_ldop_al(A64 *a, int size, int op, int rs, int rt, int rn) {
    a64_put(a, 0x38E00000u | (uint32_t)size << 30 | (uint32_t)rs << 16 | (uint32_t)op << 12 | rn << 5 | rt);
}
static inline void a64_casal(A64 *a, int size, int rs, int rt, int rn) { a64_put(a, 0x08E0FC00u | (uint32_t)size << 30 | (uint32_t)rs << 16 | rn << 5 | rt); }
static inline void a64_caspal(A64 *a, int rs, int rt, int rn) { a64_put(a, 0x4860FC00u | (uint32_t)rs << 16 | rn << 5 | rt); }
static inline void a64_dmb_ish(A64 *a) { a64_put(a, 0xD5033BBFu); }
static inline void a64_dmb_ishld(A64 *a) { a64_put(a, 0xD50339BFu); }

// SIMD&FP register load/store. size: 0 b, 1 h, 2 s, 3 d, 4 q
static inline uint32_t a64_fp_szbits(int size) { return size == 4 ? 0x00800000u : (uint32_t)size << 30; }
static inline void a64_fldst_uimm(A64 *a, int size, int load, int vt, int rn, uint32_t byteoff) {
    a64_put(a, 0x3D000000u | a64_fp_szbits(size) | (uint32_t)load << 22 | ((byteoff >> size) & 0xFFF) << 10 | rn << 5 | vt);
}
static inline void a64_fldst_simm(A64 *a, int size, int load, int vt, int rn, int imm9) {
    a64_put(a, 0x3C000000u | a64_fp_szbits(size) | (uint32_t)load << 22 | ((uint32_t)imm9 & 0x1FF) << 12 | rn << 5 | vt);
}
static inline void a64_ldstp_q(A64 *a, int load, int vt, int vt2, int rn, int off) {   // ldp/stp q, signed offset
    a64_put(a, 0xAD000000u | (uint32_t)load << 22 | (((uint32_t)(off / 16)) & 0x7F) << 15 | (uint32_t)vt2 << 10 | rn << 5 | vt);
}
static inline void a64_fldst_reg(A64 *a, int size, int load, int vt, int rn, int rm) {
    a64_put(a, 0x3C200800u | a64_fp_szbits(size) | (uint32_t)load << 22 | (uint32_t)rm << 16 | 3u << 13 | rn << 5 | vt);
}
static inline void a64_fldst_any(A64 *a, int size, int load, int vt, int rn, int64_t off, int scratch) {
    if (off >= 0 && !(off & ((1 << size) - 1)) && (off >> size) < 4096) a64_fldst_uimm(a, size, load, vt, rn, (uint32_t)off);
    else if (off >= -256 && off < 256) a64_fldst_simm(a, size, load, vt, rn, (int)off);
    else { a64_mov_imm(a, scratch, (uint64_t)off); a64_fldst_reg(a, size, load, vt, rn, scratch); }
}

// ---- branches ---------------------------------------------------------------------------------------------
static inline uint32_t a64_b_enc(const uint32_t *from, const void *to) { return 0x14000000u | ((uint32_t)(((const uint8_t *)to - (const uint8_t *)from) >> 2) & 0x3FFFFFF); }
static inline void a64_b(A64 *a, const void *to) { a64_put(a, a->p < a->end ? a64_b_enc(a->p, to) : 0); }
static inline void a64_bl(A64 *a, const void *to) { a64_put(a, a->p < a->end ? (a64_b_enc(a->p, to) | 0x80000000u) : 0); }
static inline void a64_bcond(A64 *a, int cond, const void *to) {
    int64_t d = ((const uint8_t *)to - (const uint8_t *)a->p) >> 2;
    a64_put(a, 0x54000000u | ((uint32_t)d & 0x7FFFF) << 5 | (uint32_t)cond);
}
static inline void a64_cbz(A64 *a, int sf, int nz, int rt, const void *to) {
    int64_t d = ((const uint8_t *)to - (const uint8_t *)a->p) >> 2;
    a64_put(a, SF(sf) | 0x34000000u | (uint32_t)nz << 24 | ((uint32_t)d & 0x7FFFF) << 5 | rt);
}
static inline void a64_tbz(A64 *a, int nz, int rt, int bit, const void *to) {
    int64_t d = ((const uint8_t *)to - (const uint8_t *)a->p) >> 2;
    a64_put(a, (uint32_t)(bit >> 5) << 31 | 0x36000000u | (uint32_t)nz << 24 | (uint32_t)(bit & 31) << 19 | ((uint32_t)d & 0x3FFF) << 5 | rt);
}
static inline void a64_br(A64 *a, int rn) { a64_put(a, 0xD61F0000u | rn << 5); }
static inline void a64_blr(A64 *a, int rn) { a64_put(a, 0xD63F0000u | rn << 5); }
static inline void a64_ret(A64 *a) { a64_put(a, 0xD65F03C0u); }
static inline void a64_nop(A64 *a) { a64_put(a, 0xD503201Fu); }
static inline void a64_brk(A64 *a, int imm) { a64_put(a, 0xD4200000u | (uint32_t)(imm & 0xFFFF) << 5); }
static inline void a64_adr(A64 *a, int rd, const void *to) {
    int64_t d = (const uint8_t *)to - (const uint8_t *)a->p;
    a64_put(a, 0x10000000u | ((uint32_t)d & 3) << 29 | (((uint32_t)(d >> 2)) & 0x7FFFF) << 5 | rd);
}

// Forward-branch fixups within one block: emit a placeholder, patch when the target is known.
static inline void a64_patch_bcond(uint32_t *at, int cond, const void *to) { int64_t d = ((const uint8_t *)to - (const uint8_t *)at) >> 2; *at = 0x54000000u | ((uint32_t)d & 0x7FFFF) << 5 | (uint32_t)cond; }
static inline void a64_patch_b(uint32_t *at, const void *to) { *at = a64_b_enc(at, to); }
static inline void a64_patch_cbz(uint32_t *at, const void *to) { int64_t d = ((const uint8_t *)to - (const uint8_t *)at) >> 2; *at = (*at & ~(0x7FFFFu << 5)) | ((uint32_t)d & 0x7FFFF) << 5; }
static inline void a64_patch_tbz(uint32_t *at, const void *to) { int64_t d = ((const uint8_t *)to - (const uint8_t *)at) >> 2; *at = (*at & ~(0x3FFFu << 5)) | ((uint32_t)d & 0x3FFF) << 5; }

// ---- scalar floating point ----------------------------------------------------------------------------------
// type: 0 single, 1 double
static inline void a64_fp2(A64 *a, int type, uint32_t opc, int rd, int rn, int rm) { a64_put(a, 0x1E200800u | (uint32_t)type << 22 | (uint32_t)rm << 16 | opc << 12 | rn << 5 | rd); }
enum { FP2_MUL = 0, FP2_DIV = 1, FP2_ADD = 2, FP2_SUB = 3, FP2_MAX = 4, FP2_MIN = 5 };
static inline void a64_fsqrt(A64 *a, int type, int rd, int rn) { a64_put(a, 0x1E21C000u | (uint32_t)type << 22 | rn << 5 | rd); }
static inline void a64_fcmp(A64 *a, int type, int rn, int rm) { a64_put(a, 0x1E202000u | (uint32_t)type << 22 | (uint32_t)rm << 16 | rn << 5); }
static inline void a64_fcvt_s2d(A64 *a, int rd, int rn) { a64_put(a, 0x1E22C000u | rn << 5 | rd); }
static inline void a64_fcvt_d2s(A64 *a, int rd, int rn) { a64_put(a, 0x1E624000u | rn << 5 | rd); }
static inline void a64_scvtf(A64 *a, int sf, int type, int rd, int rn) { a64_put(a, SF(sf) | 0x1E220000u | (uint32_t)type << 22 | rn << 5 | rd); }
static inline void a64_fcvtzs(A64 *a, int sf, int type, int rd, int rn) { a64_put(a, SF(sf) | 0x1E380000u | (uint32_t)type << 22 | rn << 5 | rd); }
static inline void a64_fcvtns(A64 *a, int sf, int type, int rd, int rn) { a64_put(a, SF(sf) | 0x1E200000u | (uint32_t)type << 22 | rn << 5 | rd); }
static inline void a64_frintx(A64 *a, int type, int rd, int rn) { a64_put(a, 0x1E274000u | (uint32_t)type << 22 | rn << 5 | rd); }
static inline void a64_fmov_to_gp(A64 *a, int sf, int rd, int vn) { a64_put(a, sf ? (0x9E660000u | vn << 5 | rd) : (0x1E260000u | vn << 5 | rd)); }
static inline void a64_fmov_from_gp(A64 *a, int sf, int vd, int rn) { a64_put(a, sf ? (0x9E670000u | rn << 5 | vd) : (0x1E270000u | rn << 5 | vd)); }

// ---- NEON (advanced SIMD), 128-bit unless noted --------------------------------------------------------------
static inline void a64_v3same(A64 *a, int q, int u, int size, uint32_t opc5, int rd, int rn, int rm) {
    a64_put(a, 0x0E200400u | (uint32_t)q << 30 | (uint32_t)u << 29 | (uint32_t)size << 22 | (uint32_t)rm << 16 | opc5 << 11 | rn << 5 | rd);
}
// integer
static inline void a64_vadd(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 0, size, 0x10, rd, rn, rm); }
static inline void a64_vsub(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 1, size, 0x10, rd, rn, rm); }
static inline void a64_vsqadd(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 0, size, 0x01, rd, rn, rm); }
static inline void a64_vuqadd(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 1, size, 0x01, rd, rn, rm); }
static inline void a64_vsqsub(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 0, size, 0x05, rd, rn, rm); }
static inline void a64_vuqsub(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 1, size, 0x05, rd, rn, rm); }
static inline void a64_vcmeq(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 1, size, 0x11, rd, rn, rm); }
static inline void a64_vcmgt(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 0, size, 0x06, rd, rn, rm); }
static inline void a64_vmul(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 0, size, 0x13, rd, rn, rm); }
static inline void a64_vsmax(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 0, size, 0x0C, rd, rn, rm); }
static inline void a64_vsmin(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 0, size, 0x0D, rd, rn, rm); }
static inline void a64_vumax(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 1, size, 0x0C, rd, rn, rm); }
static inline void a64_vumin(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 1, size, 0x0D, rd, rn, rm); }
static inline void a64_vurhadd(A64 *a, int size, int rd, int rn, int rm) { a64_v3same(a, 1, 1, size, 0x02, rd, rn, rm); }
// bitwise (size field selects the op)
static inline void a64_vand(A64 *a, int rd, int rn, int rm) { a64_v3same(a, 1, 0, 0, 0x03, rd, rn, rm); }
static inline void a64_vbic(A64 *a, int rd, int rn, int rm) { a64_v3same(a, 1, 0, 1, 0x03, rd, rn, rm); }
static inline void a64_vorr(A64 *a, int rd, int rn, int rm) { a64_v3same(a, 1, 0, 2, 0x03, rd, rn, rm); }
static inline void a64_veor(A64 *a, int rd, int rn, int rm) { a64_v3same(a, 1, 1, 0, 0x03, rd, rn, rm); }
static inline void a64_vbsl(A64 *a, int rd, int rn, int rm) { a64_v3same(a, 1, 1, 1, 0x03, rd, rn, rm); }
static inline void a64_vmov(A64 *a, int rd, int rn) { a64_vorr(a, rd, rn, rn); }
// float: sz 0 single, 1 double
static inline void a64_vfadd(A64 *a, int sz, int rd, int rn, int rm) { a64_v3same(a, 1, 0, sz, 0x1A, rd, rn, rm); }
static inline void a64_vfsub(A64 *a, int sz, int rd, int rn, int rm) { a64_v3same(a, 1, 0, 2 | sz, 0x1A, rd, rn, rm); }
static inline void a64_vfmul(A64 *a, int sz, int rd, int rn, int rm) { a64_v3same(a, 1, 1, sz, 0x1B, rd, rn, rm); }
static inline void a64_vfdiv(A64 *a, int sz, int rd, int rn, int rm) { a64_v3same(a, 1, 1, sz, 0x1F, rd, rn, rm); }
static inline void a64_vfcmeq(A64 *a, int sz, int rd, int rn, int rm) { a64_v3same(a, 1, 0, sz, 0x1C, rd, rn, rm); }
static inline void a64_vfcmge(A64 *a, int sz, int rd, int rn, int rm) { a64_v3same(a, 1, 1, sz, 0x1C, rd, rn, rm); }
static inline void a64_vfcmgt(A64 *a, int sz, int rd, int rn, int rm) { a64_v3same(a, 1, 1, 2 | sz, 0x1C, rd, rn, rm); }
static inline void a64_vfsqrt(A64 *a, int sz, int rd, int rn) { a64_put(a, 0x6EA1F800u | (uint32_t)sz << 22 | rn << 5 | rd); }
static inline void a64_vnot(A64 *a, int rd, int rn) { a64_put(a, 0x6E205800u | rn << 5 | rd); }
// permutes: op 1 uzp1, 2 trn1, 3 zip1, 5 uzp2, 6 trn2, 7 zip2
static inline void a64_vperm(A64 *a, int op, int size, int rd, int rn, int rm) { a64_put(a, 0x4E000800u | (uint32_t)size << 22 | (uint32_t)rm << 16 | (uint32_t)op << 12 | rn << 5 | rd); }
static inline void a64_vext(A64 *a, int rd, int rn, int rm, int idx) { a64_put(a, 0x6E000000u | (uint32_t)rm << 16 | (uint32_t)idx << 11 | rn << 5 | rd); }
static inline void a64_vmovi0(A64 *a, int rd) { a64_put(a, 0x6F00E400u | rd); }   // movi vd.2d, #0
// element moves. size: 0 b, 1 h, 2 s, 3 d
static inline void a64_vins_elem(A64 *a, int size, int rd, int di, int rn, int si) {
    uint32_t imm5 = ((uint32_t)di << (size + 1)) | (1u << size), imm4 = (uint32_t)si << size;
    a64_put(a, 0x6E000400u | imm5 << 16 | imm4 << 11 | rn << 5 | rd);
}
static inline void a64_vins_gp(A64 *a, int size, int rd, int di, int rn) { uint32_t imm5 = ((uint32_t)di << (size + 1)) | (1u << size); a64_put(a, 0x4E001C00u | imm5 << 16 | rn << 5 | rd); }
static inline void a64_vumov(A64 *a, int size, int rd, int rn, int si) { uint32_t imm5 = ((uint32_t)si << (size + 1)) | (1u << size); a64_put(a, (size == 3 ? 0x4E003C00u : 0x0E003C00u) | imm5 << 16 | rn << 5 | rd); }
static inline void a64_vdup_elem(A64 *a, int size, int rd, int rn, int si) { uint32_t imm5 = ((uint32_t)si << (size + 1)) | (1u << size); a64_put(a, 0x4E000400u | imm5 << 16 | rn << 5 | rd); }
// shifts by immediate. size 0 b .. 3 d
static inline void a64_vshl_imm(A64 *a, int size, int rd, int rn, int sh) { uint32_t es = 8u << size; a64_put(a, 0x4F005400u | (es + (uint32_t)sh) << 16 | rn << 5 | rd); }
static inline void a64_vushr(A64 *a, int size, int rd, int rn, int sh) { uint32_t es = 8u << size; a64_put(a, 0x6F000400u | (2 * es - (uint32_t)sh) << 16 | rn << 5 | rd); }
static inline void a64_vsshr(A64 *a, int size, int rd, int rn, int sh) { uint32_t es = 8u << size; a64_put(a, 0x4F000400u | (2 * es - (uint32_t)sh) << 16 | rn << 5 | rd); }
// narrowing: size = destination element size (0 b, 1 h, 2 s); hi: write upper half (…2 forms)
static inline void a64_vsqxtn(A64 *a, int hi, int size, int rd, int rn) { a64_put(a, 0x0E214800u | (uint32_t)hi << 30 | (uint32_t)size << 22 | rn << 5 | rd); }
static inline void a64_vsqxtun(A64 *a, int hi, int size, int rd, int rn) { a64_put(a, 0x2E212800u | (uint32_t)hi << 30 | (uint32_t)size << 22 | rn << 5 | rd); }
static inline void a64_vxtn(A64 *a, int hi, int size, int rd, int rn) { a64_put(a, 0x0E212800u | (uint32_t)hi << 30 | (uint32_t)size << 22 | rn << 5 | rd); }
// widening multiply: size = source element size; hi: upper halves (…2)
static inline void a64_vsmull(A64 *a, int hi, int size, int rd, int rn, int rm) { a64_put(a, 0x0E20C000u | (uint32_t)hi << 30 | (uint32_t)size << 22 | (uint32_t)rm << 16 | rn << 5 | rd); }
static inline void a64_vumull(A64 *a, int hi, int size, int rd, int rn, int rm) { a64_put(a, 0x2E20C000u | (uint32_t)hi << 30 | (uint32_t)size << 22 | (uint32_t)rm << 16 | rn << 5 | rd); }
// conversions (vector)
static inline void a64_vscvtf(A64 *a, int sz, int rd, int rn) { a64_put(a, 0x4E21D800u | (uint32_t)sz << 22 | rn << 5 | rd); }
static inline void a64_vfcvtl(A64 *a, int rd, int rn) { a64_put(a, 0x0E617800u | rn << 5 | rd); }    // 2s -> 2d (lower)
static inline void a64_vfcvtn(A64 *a, int rd, int rn) { a64_put(a, 0x0E616800u | rn << 5 | rd); }    // 2d -> 2s (lower, upper zeroed)
static inline void a64_vcnt(A64 *a, int q, int rd, int rn) { a64_put(a, 0x0E205800u | (uint32_t)q << 30 | rn << 5 | rd); }
static inline void a64_vaddv_b(A64 *a, int q, int rd, int rn) { a64_put(a, 0x0E31B800u | (uint32_t)q << 30 | rn << 5 | rd); }
static inline void a64_vtbl1(A64 *a, int rd, int rn, int rm) { a64_put(a, 0x4E000000u | (uint32_t)rm << 16 | rn << 5 | rd); }
