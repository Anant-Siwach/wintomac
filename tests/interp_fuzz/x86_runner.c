// Reference side of the interpreter differential test: runs each case natively (x86_64, under Rosetta 2).
#include "fuzz_common.h"

FState *g_s; void *g_code;
__attribute__((naked)) static void run_stub(void) {
    __asm__ volatile(
        "push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
        "mov _g_s(%rip), %rax\n"
        "vmovdqu 128(%rax), %xmm0\n vinsertf128 $1, 384(%rax), %ymm0, %ymm0\n"
        "vmovdqu 144(%rax), %xmm1\n vinsertf128 $1, 400(%rax), %ymm1, %ymm1\n"
        "vmovdqu 160(%rax), %xmm2\n vinsertf128 $1, 416(%rax), %ymm2, %ymm2\n"
        "vmovdqu 176(%rax), %xmm3\n vinsertf128 $1, 432(%rax), %ymm3, %ymm3\n"
        "vmovdqu 192(%rax), %xmm4\n vinsertf128 $1, 448(%rax), %ymm4, %ymm4\n"
        "vmovdqu 208(%rax), %xmm5\n vinsertf128 $1, 464(%rax), %ymm5, %ymm5\n"
        "vmovdqu 224(%rax), %xmm6\n vinsertf128 $1, 480(%rax), %ymm6, %ymm6\n"
        "vmovdqu 240(%rax), %xmm7\n vinsertf128 $1, 496(%rax), %ymm7, %ymm7\n"
        "vmovdqu 256(%rax), %xmm8\n vinsertf128 $1, 512(%rax), %ymm8, %ymm8\n"
        "vmovdqu 272(%rax), %xmm9\n vinsertf128 $1, 528(%rax), %ymm9, %ymm9\n"
        "vmovdqu 288(%rax), %xmm10\n vinsertf128 $1, 544(%rax), %ymm10, %ymm10\n"
        "vmovdqu 304(%rax), %xmm11\n vinsertf128 $1, 560(%rax), %ymm11, %ymm11\n"
        "vmovdqu 320(%rax), %xmm12\n vinsertf128 $1, 576(%rax), %ymm12, %ymm12\n"
        "vmovdqu 336(%rax), %xmm13\n vinsertf128 $1, 592(%rax), %ymm13, %ymm13\n"
        "vmovdqu 352(%rax), %xmm14\n vinsertf128 $1, 608(%rax), %ymm14, %ymm14\n"
        "vmovdqu 368(%rax), %xmm15\n vinsertf128 $1, 624(%rax), %ymm15, %ymm15\n"
        "pushq 640(%rax)\n popfq\n"
        "mov 8(%rax), %rcx\n mov 16(%rax), %rdx\n mov 24(%rax), %rbx\n mov 40(%rax), %rbp\n mov 48(%rax), %rsi\n"
        "mov 56(%rax), %rdi\n mov 64(%rax), %r8\n mov 72(%rax), %r9\n mov 80(%rax), %r10\n mov 88(%rax), %r11\n"
        "mov 96(%rax), %r12\n mov 104(%rax), %r13\n mov 112(%rax), %r14\n mov 120(%rax), %r15\n mov 0(%rax), %rax\n"
        "call *_g_code(%rip)\n"
        "cld\n push %rax\n mov _g_s(%rip), %rax\n"
        "mov %rcx, 8(%rax)\n mov %rdx, 16(%rax)\n mov %rbx, 24(%rax)\n mov %rbp, 40(%rax)\n mov %rsi, 48(%rax)\n"
        "mov %rdi, 56(%rax)\n mov %r8, 64(%rax)\n mov %r9, 72(%rax)\n mov %r10, 80(%rax)\n mov %r11, 88(%rax)\n"
        "mov %r12, 96(%rax)\n mov %r13, 104(%rax)\n mov %r14, 112(%rax)\n mov %r15, 120(%rax)\n pop %rcx\n mov %rcx, 0(%rax)\n"
        "vmovdqu %xmm0, 128(%rax)\n vextractf128 $1, %ymm0, 384(%rax)\n"
        "vmovdqu %xmm1, 144(%rax)\n vextractf128 $1, %ymm1, 400(%rax)\n"
        "vmovdqu %xmm2, 160(%rax)\n vextractf128 $1, %ymm2, 416(%rax)\n"
        "vmovdqu %xmm3, 176(%rax)\n vextractf128 $1, %ymm3, 432(%rax)\n"
        "vmovdqu %xmm4, 192(%rax)\n vextractf128 $1, %ymm4, 448(%rax)\n"
        "vmovdqu %xmm5, 208(%rax)\n vextractf128 $1, %ymm5, 464(%rax)\n"
        "vmovdqu %xmm6, 224(%rax)\n vextractf128 $1, %ymm6, 480(%rax)\n"
        "vmovdqu %xmm7, 240(%rax)\n vextractf128 $1, %ymm7, 496(%rax)\n"
        "vmovdqu %xmm8, 256(%rax)\n vextractf128 $1, %ymm8, 512(%rax)\n"
        "vmovdqu %xmm9, 272(%rax)\n vextractf128 $1, %ymm9, 528(%rax)\n"
        "vmovdqu %xmm10, 288(%rax)\n vextractf128 $1, %ymm10, 544(%rax)\n"
        "vmovdqu %xmm11, 304(%rax)\n vextractf128 $1, %ymm11, 560(%rax)\n"
        "vmovdqu %xmm12, 320(%rax)\n vextractf128 $1, %ymm12, 576(%rax)\n"
        "vmovdqu %xmm13, 336(%rax)\n vextractf128 $1, %ymm13, 592(%rax)\n"
        "vmovdqu %xmm14, 352(%rax)\n vextractf128 $1, %ymm14, 608(%rax)\n"
        "vmovdqu %xmm15, 368(%rax)\n vextractf128 $1, %ymm15, 624(%rax)\n"
        "vzeroupper\n"
        "pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n");
}

int main(int argc, char **argv) {
    Case *cs; int n = load_cases(argv[1], &cs);
    uint8_t *scratch = map_scratch();
    uint8_t *page = map_code(PROT_READ | PROT_WRITE | PROT_EXEC);
    for (int ci = 0; ci < n; ci++) {
        for (int si = 0; si < NSTATES; si++) {
            FState s;
            make_state(&s, ci, si, scratch);
            if (getenv("FUZZ_INIT")) print_state(ci, si, "init", &s, s.flags, 0x8D5, scratch);
            memset(page, 0xCC, 4096);
            build_snippet(page, cs[ci].code, cs[ci].n);
            g_s = &s; g_code = page;
            run_stub();
            uint64_t fl; memcpy(&fl, page + 0xF00, 8);
            print_state(ci, si, cs[ci].name, &s, fl, cs[ci].mask, scratch);
        }
    }
    (void)argc;
    return 0;
}
