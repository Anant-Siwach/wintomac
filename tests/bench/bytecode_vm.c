// A small stack/register bytecode VM, the indirect-branch-heavy core of every scripting language in a game.
// Two dispatch styles: a switch (compiled to a jump table) and a table of handler function pointers
// (indirect calls), plus a recursive tree-walking evaluator (lots of small calls).
#include "bench.h"

enum { OP_PUSH, OP_LOAD, OP_STORE, OP_ADD, OP_SUB, OP_MUL, OP_XOR, OP_SHR, OP_AND, OP_LT, OP_JNZ, OP_JMP, OP_DUP, OP_POP,
       OP_INC, OP_HALT };
typedef struct { uint8_t op; int32_t arg; } Insn;

// Program: for (i = 0; i < n; i++) { acc = (acc * 31 + (i ^ (acc >> 7))) & 0xffffff; t += acc & 255; }  locals: 0=i 1=n 2=acc 3=t
static Insn prog[] = {
    /*0*/ {OP_LOAD, 0}, {OP_LOAD, 1}, {OP_LT, 0}, {OP_JNZ, 5}, {OP_HALT, 0},
    /*5*/ {OP_LOAD, 2}, {OP_PUSH, 31}, {OP_MUL, 0}, {OP_LOAD, 0}, {OP_LOAD, 2}, {OP_PUSH, 7}, {OP_SHR, 0}, {OP_XOR, 0},
    /*13*/ {OP_ADD, 0}, {OP_PUSH, 0xffffff}, {OP_AND, 0}, {OP_DUP, 0}, {OP_STORE, 2},
    /*18*/ {OP_PUSH, 255}, {OP_AND, 0}, {OP_LOAD, 3}, {OP_ADD, 0}, {OP_STORE, 3},
    /*23*/ {OP_INC, 0}, {OP_JMP, 0},
};

BENCH_NOINLINE static int64_t run_switch(const Insn *code, int64_t *loc) {
    int64_t stack[64]; int sp = 0; int pc = 0;
    for (;;) {
        const Insn in = code[pc++];
        switch (in.op) {
        case OP_PUSH: stack[sp++] = in.arg; break;
        case OP_LOAD: stack[sp++] = loc[in.arg]; break;
        case OP_STORE: loc[in.arg] = stack[--sp]; break;
        case OP_ADD: sp--; stack[sp - 1] += stack[sp]; break;
        case OP_SUB: sp--; stack[sp - 1] -= stack[sp]; break;
        case OP_MUL: sp--; stack[sp - 1] *= stack[sp]; break;
        case OP_XOR: sp--; stack[sp - 1] ^= stack[sp]; break;
        case OP_SHR: sp--; stack[sp - 1] >>= stack[sp]; break;
        case OP_AND: sp--; stack[sp - 1] &= stack[sp]; break;
        case OP_LT: sp--; stack[sp - 1] = stack[sp - 1] < stack[sp]; break;
        case OP_JNZ: if (stack[--sp]) pc = in.arg; break;
        case OP_JMP: pc = in.arg; break;
        case OP_DUP: stack[sp] = stack[sp - 1]; sp++; break;
        case OP_POP: sp--; break;
        case OP_INC: loc[in.arg]++; break;
        case OP_HALT: return loc[3];
        }
    }
}

// Function-pointer dispatch
typedef struct VM { int64_t stack[64]; int sp, pc; int64_t *loc; int halted; } VM;
typedef void (*Handler)(VM *, int32_t);
static void h_push(VM *v, int32_t a) { v->stack[v->sp++] = a; }
static void h_load(VM *v, int32_t a) { v->stack[v->sp++] = v->loc[a]; }
static void h_store(VM *v, int32_t a) { v->loc[a] = v->stack[--v->sp]; }
static void h_add(VM *v, int32_t a) { v->sp--; v->stack[v->sp - 1] += v->stack[v->sp]; }
static void h_sub(VM *v, int32_t a) { v->sp--; v->stack[v->sp - 1] -= v->stack[v->sp]; }
static void h_mul(VM *v, int32_t a) { v->sp--; v->stack[v->sp - 1] *= v->stack[v->sp]; }
static void h_xor(VM *v, int32_t a) { v->sp--; v->stack[v->sp - 1] ^= v->stack[v->sp]; }
static void h_shr(VM *v, int32_t a) { v->sp--; v->stack[v->sp - 1] >>= v->stack[v->sp]; }
static void h_and(VM *v, int32_t a) { v->sp--; v->stack[v->sp - 1] &= v->stack[v->sp]; }
static void h_lt(VM *v, int32_t a) { v->sp--; v->stack[v->sp - 1] = v->stack[v->sp - 1] < v->stack[v->sp]; }
static void h_jnz(VM *v, int32_t a) { if (v->stack[--v->sp]) v->pc = a; }
static void h_jmp(VM *v, int32_t a) { v->pc = a; }
static void h_dup(VM *v, int32_t a) { v->stack[v->sp] = v->stack[v->sp - 1]; v->sp++; }
static void h_pop(VM *v, int32_t a) { v->sp--; }
static void h_inc(VM *v, int32_t a) { v->loc[a]++; }
static void h_halt(VM *v, int32_t a) { v->halted = 1; }
static Handler handlers[] = { h_push, h_load, h_store, h_add, h_sub, h_mul, h_xor, h_shr, h_and, h_lt, h_jnz, h_jmp,
                              h_dup, h_pop, h_inc, h_halt };
static volatile int handlers_sel;   // keeps the compiler from devirtualizing the table

BENCH_NOINLINE static int64_t run_fnptr(const Insn *code, int64_t *loc) {
    VM v; v.sp = 0; v.pc = 0; v.loc = loc; v.halted = 0;
    Handler *h = handlers + handlers_sel;
    while (!v.halted) { const Insn in = code[v.pc++]; h[in.op](&v, in.arg); }
    return loc[3];
}

// Tree-walking expression evaluator (recursion, small calls, pointer chasing)
typedef struct Node { int kind; int64_t val; struct Node *l, *r; } Node;
static Node pool[4096]; static int npool;
static Node *mk(int kind, int64_t val, Node *l, Node *r) { Node *n = &pool[npool++]; n->kind = kind; n->val = val; n->l = l; n->r = r; return n; }
static Node *build(int depth, uint64_t *s) {
    if (depth == 0) return (bench_rng(s) & 1) ? mk(0, (int64_t)(bench_rng(s) % 100), 0, 0) : mk(1, 0, 0, 0);
    return mk(2 + (int)(bench_rng(s) % 4), 0, build(depth - 1, s), build(depth - 1, s));
}
BENCH_NOINLINE static int64_t eval(const Node *n, int64_t x) {
    switch (n->kind) {
    case 0: return n->val;
    case 1: return x;
    case 2: return eval(n->l, x) + eval(n->r, x);
    case 3: return eval(n->l, x) - eval(n->r, x);
    case 4: return (eval(n->l, x) * eval(n->r, x)) & 0xffff;
    default: return eval(n->l, x) ^ eval(n->r, x);
    }
}

int main(void) {
    int64_t loc[4];
    double t0 = bench_now_ms();
    uint64_t h = 0;
    for (int r = 0; r < 4; r++) { loc[0] = 0; loc[1] = 1000000 + r; loc[2] = r; loc[3] = 0; h += (uint64_t)run_switch(prog, loc); }
    bench_report("vm_switch", bench_now_ms() - t0, h);

    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 2; r++) { loc[0] = 0; loc[1] = 1000000 + r; loc[2] = r; loc[3] = 0; h += (uint64_t)run_fnptr(prog, loc); }
    bench_report("vm_fnptr", bench_now_ms() - t0, h);

    uint64_t s = 7;
    Node *root = build(10, &s);
    t0 = bench_now_ms(); h = 0;
    for (int64_t x = 0; x < 20000; x++) h = h * 33 + (uint64_t)eval(root, x);
    bench_report("tree_eval", bench_now_ms() - t0, h);
    return 0;
}
