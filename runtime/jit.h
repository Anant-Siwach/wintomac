// wintomac runtime JIT: translates hot guest x86-64 code (runtime-generated code, code the static recompiler
// missed) to arm64 at runtime. It sits under the interpreter (runtime/interp.c): cold code is interpreted,
// a block that keeps being entered is translated, and anything the translator doesn't cover runs through the
// interpreter one instruction at a time. Design notes at the top of runtime/jit.c.
#pragma once
#include "interp_impl.h"
#include "jit_a64.h"
#include <signal.h>

// ---- public (interp.c, runtime.c, kernel32.c) ------------------------------------------------------------
int  rt_jit_enabled(void);                        // WINTOMAC_JIT (default on); 0 = interpreter only
int  rt_jit_run(IState *s);                       // at a block boundary: X_NEXT (interpret on at s->pc) / X_HOST_RET
void rt_jit_invalidate(u64 addr, u64 size);       // code changed or unmapped (FlushInstructionCache, VirtualFree)
int  rt_jit_handle_fault(int sig, siginfo_t *si, void *uc);   // 1: our fault (write to translated code, misaligned RCpc access), handled
void rt_jit_dump_stats(void);                     // WINTOMAC_JIT_TRACE
int  rt_jit_is_native(u64 target);                // runtime.c: thunk or recompiled function entry (call it natively)
int  rt_interp_step(IState *s);                   // interp.c: run the one instruction at s->pc (s->in/op/next set up by it)
GuestFn rt_lookup_fn(u64 addr);                   // runtime.c: recompiled function entry, or NULL

// ---- internals shared by jit.c / jit_x86.c / jit_sse.c ------------------------------------------------------
// Host register assignment inside translated code (the "JIT world"):
//   x6..x15  guest rax..r9        x19..x24  guest r10..r15
//   x25      Ctx *                 x26 x27 x28  lazy flags: op, operand a, result (see JF_*)
//   x0..x5, x16, x17 scratch       x29 frame pointer, sp host stack: [sp] JState*, [sp+8] fast table, [sp+16] entry rsp
// Guest xmm registers stay in Ctx (memory); v0..v7 are scratch.
static inline int GR(int i) { return i < 10 ? 6 + i : 19 + (i - 10); }
enum { RCTX = 25, RFOP = 26, RFA = 27, RFRES = 28 };
enum { T0 = 0, T1 = 1, T2 = 2, T3 = 3, T4 = 4, T5 = 5, T16 = 16, T17 = 17 };
#define CTX_R(i) ((int)(8 * (i)))
#define CTX_X(i) ((int)(__builtin_offsetof(Ctx, x) + 16 * (i)))
#define CTX_GS ((int)__builtin_offsetof(Ctx, gs_base))
#define CTX_FS ((int)__builtin_offsetof(Ctx, fs_base))
enum { FR_JS = 0, FR_FAST = 8, FR_ENTRY_RSP = 16 };

// Lazy flags: x26 = op = kind << 2 | size code (0 b, 1 w, 2 d, 3 q) | aux << 8; x27 = first operand, x28 = result.
// kind MAT (op == 0): x28 holds the flags in RFLAGS layout (CF PF AF ZF SF OF).
enum { JF_MAT, JF_ADD, JF_SUB, JF_LOGIC, JF_INC, JF_DEC, JF_ADC, JF_SBB, JF_SHL, JF_SHR, JF_SAR, JF_MUL };
#define JF_OP(kind, sz, aux) ((u64)(kind) << 2 | (u64)(sz) | (u64)(aux) << 8)
u64 rt_jit_flags(u64 op, u64 a, u64 res);   // materialize lazy flags to RFLAGS bits

// Per-invocation state of the JIT world (on the host stack of rt_jit_run).
typedef struct JState {
    Ctx *c;
    u64 entry_rsp;
    u64 rflags;          // in: flags at entry; out (helpers): flags after an interpreted instruction
    u64 fop, fa, fres;   // lazy flags at exit
    u64 exit_pc, exit_aux;
    u8 df;
} JState;

// World exits (x2 at the common exit stub)
enum { EXIT_LINK = 1, EXIT_JMP, EXIT_CALL, EXIT_RET, EXIT_HOSTRET, EXIT_INTERP, EXIT_STALE };

typedef struct JLink { u32 *site; u32 orig; struct JLink *next; } JLink;
typedef struct JBlock {
    u64 pc, end;                 // guest bytes [pc, end)
    u32 *code;                   // host entry
    u32 *stale;                  // exit stub used when the block is invalidated
    u32 *gate, *gate_stale;      // loop traces: back edges enter at `gate` (patched to gate_stale when invalidated)
    u32 ninsn, nfallback, size;
    u8 dead, checked;            // checked: block verifies its guest bytes on entry (volatile page)
    struct JBlock *hnext;        // pc hash chain
    struct JBlock *pnext[2];     // per-page lists
    u64 page[2];
    JLink *in;                   // direct branches linked to this block
    u64 *counter;                // execution counter (WINTOMAC_JIT_TRACE)
    struct JBlock *all;          // every block ever made (statistics)
} JBlock;

// Stubs at the start of every code chunk
typedef struct JStubs {
    u32 (*enter)(Ctx *c, JState *js, void *code);
    u32 *exit;        // x0 = guest pc, x1 = aux, x2 = reason
    u32 *lookup;      // x0 = guest target, x2 = reason on miss: fast-table lookup, br or exit
    u32 *mat;         // materialize lazy flags in place (x26 = 0, x28 = RFLAGS)
    u32 *call_c;      // x16 = C function, x0.. args: sync guest regs to Ctx, call, reload
} JStubs;
extern JStubs rt_jit_stubs;

// Helpers called from translated code (through call_c)
u64 rt_jit_h_interp(JState *js, u64 pc, u64 fop, u64 fa, u64 fres);   // run one instruction in the interpreter
u64 rt_jit_h_call(JState *js, u64 target);    // call/jmp to a native target: 1 handled (continue), 0 not native
u64 rt_jit_h_native_jmp(JState *js, u64 target);

// Translation context
typedef struct JInsn {
    ZydisDecodedInstruction in;
    ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];
    u64 pc;
    u16 live_after;     // flags live after this instruction (CF PF AF ZF SF OF bits)
    u8 fallback;        // translated as a call into the interpreter
} JInsn;

// Out-of-line stubs emitted after the trace body. kind: XS_LINK (direct branch exit to target), XS_HOSTRET,
// XS_HELPER (helper returned an exit reason in x0, target in JState.exit_pc), XS_SLOW (interpret insn, resume)
enum { XS_LINK, XS_HOSTRET, XS_HELPER, XS_SLOW, XS_LOOP };
typedef struct JExit {
    u32 *branch; int kind; u64 target; JInsn *insn; u32 *resume;
    u16 xdirty;     // cached xmm registers to write back to Ctx before leaving through this exit
    u16 xreload;    // XS_SLOW: xmm registers cached at the resume point (reloaded after the interpreter ran)
} JExit;
#define JMAX_INSN 96
#define JMAX_EXIT 128
typedef struct JComp {
    A64 a;
    JInsn *ins; int n;
    u64 pc;                 // trace start
    u32 *entry;             // host address of the trace start (self loops)
    u32 *body;              // after the entry check (if any)
    u32 *stale;             // stub that leaves through EXIT_STALE (entry check failed / block invalidated)
    JExit exits[JMAX_EXIT]; int nexit;
    // compile-time flag state
    int fk;                 // known kind of the lazy flags in x26..x28, or -1 (runtime)
    int fsz;                // size code of the known kind
    int nzcv;               // host NZCV currently holds the known op's flags in ARM form
    int cur;                // index of the instruction being translated
    int trace_mode;
    u64 *counter;           // execution counter of this block (trace mode)
    int fail;               // translation must be abandoned (falls back to interpretation)
    int reject_fallback_heavy, rejected;   // don't translate traces that mostly need the interpreter
    // guest xmm i lives in host v(16+i) while cached (straight-line code inside one trace); dirty ones are written
    // back to Ctx before any exit, helper call or back edge
    u16 xcached, xdirty;
    // loop traces (a back edge to the trace start) keep the xmm registers they use (loopL, written: loopD) in host
    // registers across iterations: loaded once at entry, back edges jump to `gate` after the entry code
    u16 loopL, loopD;
    u32 *gate, *gate_stale;
} JComp;

#define F6 (ZYDIS_CPUFLAG_CF | ZYDIS_CPUFLAG_PF | ZYDIS_CPUFLAG_AF | ZYDIS_CPUFLAG_ZF | ZYDIS_CPUFLAG_SF | ZYDIS_CPUFLAG_OF)
enum { FL_CF = 1, FL_PF = 4, FL_AF = 16, FL_ZF = 64, FL_SF = 128, FL_OF = 2048 };

// jit_x86.c
int  jit_classify(const JInsn *j);           // 1: translated natively, 0: interpreter fallback
void jit_flag_use(const JInsn *j, u16 *rd, u16 *wr);   // flags read / (unconditionally) written, for liveness
int  jit_translate_insn(JComp *jc, JInsn *j);   // 0: done, 1: ends the trace (terminator emitted)
void jit_emit_fallback(JComp *jc, JInsn *j);
void jit_emit_exit_direct(JComp *jc, u64 target, int kind);   // unconditional exit to a guest pc
// jit_sse.c
int  jit_sse_classify(const JInsn *j);
int  jit_sse_translate(JComp *jc, JInsn *j);   // 1 translated
int  jit_sse_has_slow_path(const JInsn *j);    // may fall back to the interpreter at run time (NaN results)
void jit_ea(JComp *jc, JInsn *j, const ZydisDecodedOperand *o, int *base, int *disp);
int  jit_rd_int(JComp *jc, JInsn *j, int i, int scratch);
void jit_wr_int(JComp *jc, JInsn *j, int i, int src);
void jit_slow_path_cbz(JComp *jc, JInsn *j, int sf, int nz, int rt);
void jit_slow_path_cond(JComp *jc, JInsn *j, int cond);
void jit_slow_resume(JComp *jc, int first);
void jit_xflush(JComp *jc);                    // write dirty cached xmm registers back to Ctx
void jit_xload(JComp *jc, u16 mask);          // (re)load these xmm registers from Ctx
// jit.c
extern int rt_jit_stat_trace;
void jit_emit_call_c(JComp *jc, void *fn);
