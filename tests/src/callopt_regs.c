// Register values across calls, for the interprocedural call-site sync (recomp/regsum.py):
//  * GCC -O2 uses IPA-RA: a caller keeps values in volatile registers across a call to a static function it
//    knows doesn't clobber them, so the recompiled caller must not reload those from a stale Ctx.
//  * Hand-written x86 with a private convention (r10 in, r11 preserved, rbx carried through), which only works
//    if MOD sets are exact.
//  * Callee-saved GPR / xmm6-15 pressure across direct, recursive, indirect and import calls.
// Native builds use the C fallbacks, which compute the same values.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __clang__
#pragma clang fp contract(off)   /* the native reference must not fuse multiply-adds */
#endif

#if defined(_WIN32) && defined(__x86_64__)
// asm_add_r10: rax = r10 + 1000, touches nothing else. asm_chain: keeps r11 and rbx live across the call.
__asm__(".text\n.globl asm_add_r10\nasm_add_r10:\n  lea 1000(%r10), %rax\n  ret\n"
        ".globl asm_chain\nasm_chain:\n"
        "  push %rbx\n  mov %rcx, %r11\n  mov %rdx, %rbx\n  mov %rcx, %r10\n  shl $1, %r10\n"
        "  call asm_add_r10\n"            // relies on r11 and rbx surviving
        "  add %r11, %rax\n  add %rbx, %rax\n"
        "  mov %rax, %r10\n  call asm_add_r10\n  add %r11, %rax\n"
        "  pop %rbx\n  ret\n");
long long asm_chain(long long a, long long b);
// sum of xmm0..xmm3 into xmm0, clobbers xmm4/xmm5 (volatile) but not xmm6+
__asm__(".globl asm_fsum\nasm_fsum:\n  movaps %xmm0, %xmm4\n  addsd %xmm1, %xmm4\n  movaps %xmm2, %xmm5\n"
        "  addsd %xmm3, %xmm5\n  addsd %xmm5, %xmm4\n  movaps %xmm4, %xmm0\n  ret\n");
double asm_fsum(double a, double b, double c, double d);
// MSVC-style callee-saved register handling (found in SDL3.dll): asm_mid saves rbx, restores it on both exits,
// and one exit tail-calls asm_t2, which changes rbx in a way the epilogue check doesn't recognise. A caller
// keeping a value in rbx across asm_mid's plain-return path must still see it intact.
__asm__(".globl asm_t2\nasm_t2:\n  push %rbx\n  mov %rcx, %rbx\n  lea 7(%rbx), %rax\n  mov (%rsp), %rbx\n"
        "  add $8, %rsp\n  xor %ecx, %ecx\n  ret\n"
        ".globl asm_mid\nasm_mid:\n  mov %rbx, 8(%rsp)\n  push %rsi\n  sub $0x20, %rsp\n  mov %rdx, %rbx\n  mov %rdx, %rsi\n"
        "  test %rcx, %rcx\n  jnz 1f\n  lea 1(%rbx,%rsi), %rax\n  mov 0x30(%rsp), %rbx\n  add $0x20, %rsp\n  pop %rsi\n  ret\n"
        "1:\n  mov 0x30(%rsp), %rbx\n  add $0x20, %rsp\n  pop %rsi\n  jmp asm_t2\n"
        ".globl asm_outer\nasm_outer:\n  push %rbx\n  push %rsi\n  sub $0x28, %rsp\n  mov %rcx, %rbx\n  mov %rcx, %rsi\n"
        "  xor %ecx, %ecx\n  mov $100, %edx\n  call asm_mid\n  add %rbx, %rax\n  add %rsi, %rax\n  mov %rax, %rsi\n"
        "  mov $1, %ecx\n  mov $5, %edx\n  call asm_mid\n  add %rbx, %rax\n  add %rsi, %rax\n"
        "  add $0x28, %rsp\n  pop %rsi\n  pop %rbx\n  ret\n");
long long asm_outer(long long v);
long long asm_t2(long long v);   // also called directly, so the analysis sees it as a function of its own
#else
static long long asm_t2(long long v) { return v + 7; }
static long long asm_outer(long long v) { return 209 + 3 * v; }
static long long asm_chain(long long a, long long b) { long long r = 2 * a + 1000 + a + b; return r + 1000 + a; }
static double asm_fsum(double a, double b, double c, double d) { return (a + b) + (c + d); }
#endif

static int depth_sum;
__attribute__((noinline)) static int leaf(int x) { depth_sum += x; return x * 3 + 1; }
__attribute__((noinline)) static int twice(int x) { return leaf(x) + leaf(x + 1); }

// many values live across calls: forces callee-saved registers (and IPA-RA volatile reuse)
__attribute__((noinline)) static unsigned long long pressure(int n) {
    unsigned long long a = n, b = n * 3, c = n ^ 0x55, d = n + 7, e = n * n, f = n - 9, g = n << 3, h = n | 1;
    for (int i = 0; i < 50; i++) {
        a += twice(i); b ^= leaf(i + 2); c += a >> 3; d -= b & 7; e += c * 3; f ^= d; g += e; h *= 3;
        h ^= g >> 7;
    }
    return a + b + c + d + e + f + g + h;
}

__attribute__((noinline)) static double fpressure(int n) {
    double a = n, b = n * 0.5, c = n + 0.25, d = n * 1.5, e = 3.0, f = 7.5, g = n * 0.125, h = 2.0;
    for (int i = 0; i < 40; i++) {
        double s = asm_fsum(a, b, c, d);
        a = s * 0.5; b += e; c -= f * 0.1; d = g + h; e += 0.5; f *= 0.99; g += s * 0.001; h += a * 0.01;
        leaf(i);
    }
    return a + b + c + d + e + f + g + h;
}

__attribute__((noinline)) static unsigned fib(unsigned n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

typedef unsigned long long (*binop)(unsigned long long, unsigned long long);
__attribute__((noinline)) static unsigned long long op_add(unsigned long long a, unsigned long long b) { return a + b; }
__attribute__((noinline)) static unsigned long long op_mul(unsigned long long a, unsigned long long b) { return a * b; }
__attribute__((noinline)) static unsigned long long op_xor(unsigned long long a, unsigned long long b) { return a ^ (b << 1); }
static binop volatile ops[3] = { op_add, op_mul, op_xor };

__attribute__((noinline)) static unsigned long long indirect_mix(int n) {
    unsigned long long acc = 1, k1 = n, k2 = n * 7, k3 = n * 13;
    for (int i = 0; i < 300; i++) { acc = ops[i % 3](acc, i) & 0xffffffffffll; k1 += acc; k2 ^= k1; k3 += k2 >> 2; }
    return acc + k1 + k2 + k3;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(void) {
    printf("asm_chain %lld\n", asm_chain(5, 11));
    printf("asm_fsum %.3f\n", asm_fsum(1.5, 2.25, 3.0, 4.125));
    printf("asm_outer %lld %lld\n", asm_outer(1000), asm_t2(35));
    printf("pressure %llu\n", pressure(17));
    printf("fpressure %.6f\n", fpressure(9));
    printf("fib %u\n", fib(22));
    printf("indirect %llu\n", indirect_mix(5));
    int v[64]; for (int i = 0; i < 64; i++) v[i] = (i * 37) % 64;
    unsigned long long before = pressure(3);
    qsort(v, 64, sizeof v[0], cmp_int);   // guest callback from the runtime's qsort
    unsigned long long after = pressure(3);
    printf("qsort %d %d %d same=%d\n", v[0], v[31], v[63], before == after);
    printf("depth_sum %d\n", depth_sum);
    return 0;
}
