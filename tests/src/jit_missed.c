// Code the static recompiler cannot see. The functions in the asm block below live in the program's own .text
// but have no .pdata entry, no relocation, no symbol and no `lea` pointing at them; they are reached through a
// function pointer computed at run time as anchor_fn + an assembler-time constant read from memory. The
// recompiler never decodes them, so calls land on unrecompiled addresses and the runtime interprets them
// (runtime/interp.c). Covered here: interpreted -> recompiled callbacks, interpreted -> interpreted calls,
// a jump table (indirect jmp) inside interpreted code, a tail jump into C, a thread whose start routine is
// unrecompiled code, and recompiled functions that continue in the interpreter at an instruction the lifter
// can't translate. Reference output: x86_64 macOS build under Rosetta 2.
#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef __APPLE__
#define SYM(x) "_" #x
#define LBL(x) "L" #x
#else
#define SYM(x) #x
#define LBL(x) ".L" #x
#endif

// Win64 calling convention throughout (ms_abi on the native reference build).
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl " SYM(anchor_fn) "\n"
    SYM(anchor_fn) ":\n"
    "  movl $42, %eax\n"
    "  ret\n"
    ".intel_syntax noprefix\n"
    ".p2align 4\n"
    // i64 hidden_sum(i64 (*cb)(i64), i64 n) = sum_{k=1..n} cb(k) + helper(k)
    LBL(hidden_sum) ":\n"
    "  push rbx\n  push rsi\n  push rdi\n  sub rsp, 32\n"
    "  mov rbx, rcx\n  mov rsi, rdx\n  xor edi, edi\n"
    LBL(hs_loop) ":\n"
    "  test rsi, rsi\n  jz " LBL(hs_done) "\n"
    "  mov rcx, rsi\n  call rbx\n  add rdi, rax\n"            // native callback
    "  mov rcx, rsi\n  call " LBL(helper) "\n  add rdi, rax\n" // interpreted -> interpreted
    "  dec rsi\n  jmp " LBL(hs_loop) "\n"
    LBL(hs_done) ":\n"
    "  mov rax, rdi\n  add rsp, 32\n  pop rdi\n  pop rsi\n  pop rbx\n  ret\n"
    // i64 helper(i64 k): switch (k & 3) through a jump table of relative offsets
    LBL(helper) ":\n"
    "  mov eax, ecx\n  and eax, 3\n"
    "  lea rdx, [rip + " LBL(table) "]\n"
    "  movsxd rax, dword ptr [rdx + rax*4]\n"
    "  add rax, rdx\n  jmp rax\n"
    LBL(case0) ":\n  lea rax, [rcx + rcx]\n  ret\n"
    LBL(case1) ":\n  imul rax, rcx, 7\n  ret\n"
    LBL(case2) ":\n  mov rax, rcx\n  neg rax\n  ret\n"
    LBL(case3) ":\n  mov rax, rcx\n  shl rax, 4\n  ret\n"
    ".p2align 2\n"
    LBL(table) ":\n"
    "  .long " LBL(case0) " - " LBL(table) ", " LBL(case1) " - " LBL(table) ", "
               LBL(case2) " - " LBL(table) ", " LBL(case3) " - " LBL(table) "\n"
    // i64 hidden_tail(i64 (*cb)(i64), i64 x) = cb(x * 3 + 1000), as a tail jump into C
    ".p2align 4\n"
    LBL(hidden_tail) ":\n"
    "  mov rax, rcx\n  lea rcx, [rdx + rdx*2 + 1000]\n  jmp rax\n"
    // u32 hidden_thread(u64 *p): p[1] = p[0]^2 + popcount(p[0]); returns p[0] & 0xff
    ".p2align 4\n"
    LBL(hidden_thread) ":\n"
    "  mov rax, [rcx]\n  mov rdx, rax\n  imul rdx, rax\n  popcnt r8, rax\n  add rdx, r8\n"
    "  mov [rcx + 8], rdx\n  movzx eax, al\n  ret\n"
    ".att_syntax prefix\n"
    ".data\n"
    ".p2align 2\n"
    ".globl " SYM(hidden_offsets) "\n"
    SYM(hidden_offsets) ":\n"
    "  .long " LBL(hidden_sum) " - " SYM(anchor_fn) ", " LBL(hidden_tail) " - " SYM(anchor_fn) ", "
               LBL(hidden_thread) " - " SYM(anchor_fn) "\n"
    ".text\n");

#define MSABI __attribute__((ms_abi))
typedef int64_t (MSABI *cb_t)(int64_t);
typedef int64_t (MSABI *hidden_t)(cb_t, int64_t);
typedef uint32_t (MSABI *thread_t)(uint64_t *);
extern int MSABI anchor_fn(void);
extern volatile int32_t hidden_offsets[3];

static int calls;
static int64_t MSABI cube(int64_t x) { calls++; return x * x * x; }

static void *at(int i) { return (char *)(uintptr_t)&anchor_fn + hidden_offsets[i]; }

// Recompiled functions containing instructions the static lifter doesn't translate (PHMINPOSUW, CMPXCHG8B):
// the generated code hands the rest of the function to the interpreter at that instruction.
__attribute__((noinline)) static int64_t minpos_then_more(const uint16_t *v, int64_t k) {
    uint32_t r;
    __asm__ volatile("movdqu %1, %%xmm0\n\tphminposuw %%xmm0, %%xmm0\n\tmovd %%xmm0, %0"
                     : "=r"(r) : "m"(*(const uint16_t(*)[8])v) : "xmm0");
    int64_t acc = r;
    for (int i = 0; i < 8; i++) acc = acc * 31 + v[i] + cube(i + k);   // calls back into recompiled code
    return acc;
}
__attribute__((noinline)) static int cas8(volatile uint64_t *p, uint64_t expect, uint64_t desired) {
    uint32_t lo = (uint32_t)expect, hi = (uint32_t)(expect >> 32); unsigned char ok;
    __asm__ volatile("lock cmpxchg8b %1\n\tsete %0" : "=q"(ok), "+m"(*p), "+a"(lo), "+d"(hi)
                     : "b"((uint32_t)desired), "c"((uint32_t)(desired >> 32)) : "cc", "memory");
    return ok * 1000 + (int)(lo & 0xff);
}

// Flags live across the untranslatable instruction: cmp, then PHMINPOSUW (handed to the interpreter), then a
// setcc that must still see the cmp's SF/OF/ZF/CF. The generated code passes its flags along at the hand-off.
__attribute__((noinline)) static int flags_across(int a, int b, const uint16_t *v) {
    int r;
    __asm__ volatile("cmpl %2, %1\n\tmovdqu %3, %%xmm0\n\tphminposuw %%xmm0, %%xmm0\n\t"
                     "setl %b0\n\tsetb %%ah\n\tsete %%al\n\tshl $2, %0\n\tor %%ah, %b0\n\tadd %%al, %%al\n\tor %%al, %b0\n\tmovzbl %b0, %0"
                     : "=&q"(r) : "r"(a), "r"(b), "m"(*(const uint16_t(*)[8])v) : "xmm0", "cc", "rax");
    return r;
}

#ifndef _WIN32
static uint64_t thread_cell[2];
static void *thread_tramp(void *p) { return (void *)(uintptr_t)((thread_t)at(2))((uint64_t *)p); }
#endif

int main(void) {
    printf("anchor_fn() = %d\n", anchor_fn());
    hidden_t sum = (hidden_t)at(0), tail = (hidden_t)at(1);
    for (int n = 0; n <= 12; n += 4) printf("hidden_sum(cube, %d) = %lld\n", n, (long long)sum(cube, n));
    printf("hidden_tail(cube, 5) = %lld\n", (long long)tail(cube, 5));
    printf("callbacks = %d\n", calls);

    uint64_t cell[2] = {0x1234567, 0};
    uint32_t code;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)at(2), cell, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    DWORD ec = 0; GetExitCodeThread(h, &ec); code = (uint32_t)ec;
    CloseHandle(h);
#else
    pthread_t t; void *rv;
    pthread_create(&t, NULL, thread_tramp, cell);
    pthread_join(t, &rv); code = (uint32_t)(uintptr_t)rv;
    (void)thread_cell;
#endif
    printf("hidden_thread: exit=%u cell[1]=%llu\n", code, (unsigned long long)cell[1]);

    uint16_t v[8] = {900, 77, 65535, 12, 400, 12, 3000, 501};
    printf("minpos_then_more = %lld\n", (long long)minpos_then_more(v, 3));
    printf("flags across fallback: %d %d %d %d\n", flags_across(1, 2, v), flags_across(5, 2, v), flags_across(-3, -1, v), flags_across(7, 7, v));
    volatile uint64_t q = 0x1122334455667788ull;
    int a = cas8(&q, 0x1122334455667788ull, 0xdeadbeefcafef00dull);
    int b = cas8(&q, 0x1122334455667788ull, 5);
    printf("cas8: %d %d q=%llx\n", a, b, (unsigned long long)q);
    printf("callbacks = %d\n", calls);
    return 0;
}
