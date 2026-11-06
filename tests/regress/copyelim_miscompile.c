// Regression test for an LLVM AArch64 miscompile hit by recompiled GZDoom (tools/link.sh works around it with
// -mllvm -aarch64-enable-copyelim=false). "AArch64 Redundant Copy Elimination" deletes `mov wN, #1` after
// `cmp wN, #1; b.eq` because it knows wN == 1 there, but a later 64-bit use (`mov xM, xN`) needs the upper half,
// which held garbage (a spilled 64-bit add that the IR had masked with `and 0xffffffff`): x86 `mov edx, 1` came
// out as 0x100000001 and indexed far outside a table. Present in Apple clang 21 and LLVM 23.
//
// The function below is lifted GZDoom code (gzdoom.exe 0x1405d43f0, GPL-3.0), reduced by delta debugging until
// only what triggers the bug is left. `make regress-copyelim` builds it at -O0 and with link.sh's flags and checks
// that both runs end in the same guest state (it also reports whether this compiler still has the bug without
// the workaround). Stubs stand in for the callees; guest memory lives at fixed addresses so states compare.
#include "recomp.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
static u64 guest_stack_top;
static void dump(Ctx *c) { for (int i = 0; i < 16; i++) fprintf(stderr, "r%d=%llx ", i, (unsigned long long)c->r[i]); fprintf(stderr, "\n"); u64 h = 0; for (int i = 0; i < 16; i++) h = h * 31 + c->r[i]; u8 *s = (u8 *)(c->r[4] - 0x400); for (int i = 0; i < 0x800; i++) h = h * 31 + s[i]; printf("state %016llx\n", (unsigned long long)h); fflush(stdout); _exit(0); }
#define RET8 c->r[4] += 8
#define STOP(n) void n(Ctx *c) { dump(c); }
void f_10000654060(Ctx *c) { RET8; }
void f_100005d3d30(Ctx *c) { c->r[0] = 0x10; RET8; }
STOP(f_100001d31f0) STOP(f_100005d1a40) STOP(f_100005d3920) STOP(f_100005d4abd) STOP(f_100005d4ac5) STOP(f_100005d4ae1)
STOP(f_100005fbc80) STOP(f_100005fbd60) STOP(f_100005fbe10) STOP(f_100005fbe40) STOP(f_1000060ef20) STOP(f_10000610ef0)
STOP(f_10000614000) STOP(f_10000654100) STOP(f_100006e4560) STOP(f_10000733e00) STOP(f_10000758d14)
void rt_resume_elsewhere(Ctx *c, u64 t, u64 s) { dump(c); }
void f_100005d43f0(Ctx *c) {
    u64 rax=0, rcx=c->r[1], rdx=c->r[2], rbx=c->r[3], rsp=c->r[4], rbp=c->r[5], rsi=c->r[6], rdi=c->r[7], r8=c->r[8], r9=c->r[9], r10=c->r[10], r11=c->r[11], r12=c->r[12], r13=c->r[13], r14=c->r[14], r15=c->r[15];
    V128 x0={0}, x1={0};
    u8 cf=0, pf=0, af=0, zf=0, sf=0, of=0; (void)cf; (void)pf; (void)af; (void)zf; (void)sf; (void)of;
    RtEhFrame eh_ __attribute__((cleanup(rt_eh_pop)));
    rt_eh_push(&eh_, c, 0x100005d43f0ull, rsp);
    if (__builtin_expect(_setjmp(eh_.jb) != 0, 0)) { rax=c->r[0]; rcx=c->r[1]; rdx=c->r[2]; rbx=c->r[3]; rsp=c->r[4]; rbp=c->r[5]; rsi=c->r[6]; rdi=c->r[7]; r8=c->r[8]; r9=c->r[9]; r10=c->r[10]; r11=c->r[11]; r12=c->r[12]; r13=c->r[13]; r14=c->r[14]; r15=c->r[15]; x0=c->x[0]; x1=c->x[1]; c->resume_target = eh_.target; }
    if (__builtin_expect(c->resume_target != 0, 0)) {
        rax=c->r[0]; rcx=c->r[1]; rdx=c->r[2]; rbx=c->r[3]; rsp=c->r[4]; rbp=c->r[5]; rsi=c->r[6]; rdi=c->r[7]; r8=c->r[8]; r9=c->r[9]; r10=c->r[10]; r11=c->r[11]; r12=c->r[12]; r13=c->r[13]; r14=c->r[14]; r15=c->r[15]; x0=c->x[0]; x1=c->x[1]; u64 t_ = c->resume_target; c->resume_target = 0;
        switch (t_) { case 0x100005d43f0ull: goto L_100005d43f0; case 0x100005d4482ull: goto L_100005d4482; case 0x100005d4489ull: goto L_100005d4489; case 0x100005d4492ull: goto L_100005d4492; case 0x100005d449dull: goto L_100005d449d; case 0x100005d44adull: goto L_100005d44ad; case 0x100005d44b2ull: goto L_100005d44b2; case 0x100005d44b6ull: goto L_100005d44b6; case 0x100005d44bbull: goto L_100005d44bb; case 0x100005d44c0ull: goto L_100005d44c0; case 0x100005d44c5ull: goto L_100005d44c5; case 0x100005d44ccull: goto L_100005d44cc; case 0x100005d44cfull: goto L_100005d44cf; case 0x100005d44dbull: goto L_100005d44db; case 0x100005d44e1ull: goto L_100005d44e1; case 0x100005d44e6ull: goto L_100005d44e6; case 0x100005d44e9ull: goto L_100005d44e9; case 0x100005d4513ull: goto L_100005d4513; case 0x100005d4534ull: goto L_100005d4534; case 0x100005d4549ull: goto L_100005d4549; case 0x100005d454cull: goto L_100005d454c; case 0x100005d4560ull: goto L_100005d4560; case 0x100005d4574ull: goto L_100005d4574; case 0x100005d458full: goto L_100005d458f; case 0x100005d4592ull: goto L_100005d4592; case 0x100005d45a0ull: goto L_100005d45a0; case 0x100005d45a3ull: goto L_100005d45a3; case 0x100005d45b7ull: goto L_100005d45b7; case 0x100005d45d2ull: goto L_100005d45d2; case 0x100005d45d5ull: goto L_100005d45d5; case 0x100005d45e6ull: goto L_100005d45e6; case 0x100005d4630ull: goto L_100005d4630; case 0x100005d4646ull: goto L_100005d4646; case 0x100005d465dull: goto L_100005d465d; case 0x100005d4678ull: goto L_100005d4678; case 0x100005d467bull: goto L_100005d467b; case 0x100005d46a7ull: goto L_100005d46a7; case 0x100005d46b0ull: goto L_100005d46b0; case 0x100005d46d0ull: goto L_100005d46d0; case 0x100005d46ddull: goto L_100005d46dd; case 0x100005d46f0ull: goto L_100005d46f0; case 0x100005d4709ull: goto L_100005d4709; case 0x100005d4720ull: goto L_100005d4720; case 0x100005d473bull: goto L_100005d473b; case 0x100005d473eull: goto L_100005d473e; case 0x100005d4757ull: goto L_100005d4757; case 0x100005d4763ull: goto L_100005d4763; case 0x100005d476aull: goto L_100005d476a; case 0x100005d4788ull: goto L_100005d4788; case 0x100005d4790ull: goto L_100005d4790; case 0x100005d479eull: goto L_100005d479e; case 0x100005d47abull: goto L_100005d47ab; case 0x100005d47b3ull: goto L_100005d47b3; case 0x100005d47cdull: goto L_100005d47cd; case 0x100005d480cull: goto L_100005d480c; case 0x100005d4810ull: goto L_100005d4810; case 0x100005d4830ull: goto L_100005d4830; case 0x100005d4837ull: goto L_100005d4837; case 0x100005d4845ull: goto L_100005d4845; case 0x100005d4850ull: goto L_100005d4850; case 0x100005d485eull: goto L_100005d485e; case 0x100005d4878ull: goto L_100005d4878; case 0x100005d4880ull: goto L_100005d4880; case 0x100005d4889ull: goto L_100005d4889; case 0x100005d4892ull: goto L_100005d4892; case 0x100005d48c9ull: goto L_100005d48c9; case 0x100005d48ceull: goto L_100005d48ce; case 0x100005d48eaull: goto L_100005d48ea; case 0x100005d4903ull: goto L_100005d4903; case 0x100005d4906ull: goto L_100005d4906; case 0x100005d4919ull: goto L_100005d4919; case 0x100005d4926ull: goto L_100005d4926; case 0x100005d4933ull: goto L_100005d4933; case 0x100005d4940ull: goto L_100005d4940; case 0x100005d4951ull: goto L_100005d4951; case 0x100005d496bull: goto L_100005d496b; case 0x100005d4970ull: goto L_100005d4970; case 0x100005d4979ull: goto L_100005d4979; case 0x100005d4982ull: goto L_100005d4982; case 0x100005d49b9ull: goto L_100005d49b9; case 0x100005d49beull: goto L_100005d49be; case 0x100005d49daull: goto L_100005d49da; case 0x100005d49f3ull: goto L_100005d49f3; case 0x100005d49f6ull: goto L_100005d49f6; case 0x100005d4a09ull: goto L_100005d4a09; case 0x100005d4a16ull: goto L_100005d4a16; case 0x100005d4a23ull: goto L_100005d4a23; case 0x100005d4a30ull: goto L_100005d4a30; case 0x100005d4a45ull: goto L_100005d4a45; case 0x100005d4a4full: goto L_100005d4a4f; case 0x100005d4a7aull: goto L_100005d4a7a; case 0x100005d4aacull: goto L_100005d4aac; case 0x100005d4abaull: goto L_100005d4aba; default: break; }
        c->r[0]=rax; c->r[1]=rcx; c->r[2]=rdx; c->r[3]=rbx; c->r[4]=rsp; c->r[5]=rbp; c->r[6]=rsi; c->r[7]=rdi; c->r[8]=r8; c->r[9]=r9; c->r[10]=r10; c->r[11]=r11; c->r[12]=r12; c->r[13]=r13; c->r[14]=r14; c->r[15]=r15; c->x[0]=x0; c->x[1]=x1; rt_resume_elsewhere(c, t_, 0x100005d43f0ull); return;
    }
    goto L_100005d43f0;
L_100005d43f0:;
    /* 100005d4412: mov r15d,edx */
    r15 = (u32)((u32)rdx);
L_100005d4482:;
L_100005d4489:;
L_100005d4492:;
L_100005d449d:;
    /* 100005d44a0: mov eax,r15d */
    rax = (u32)((u32)r15);
    /* 100005d44a8: sub eax,1 */
    u32 t24 = (u32)0x1ull; u8 ci_t25 = 0;
    u32 t23 = (u32)rax, t25;
    t25 = t23 - t24 - ci_t25;
    rax = (u32)(t25);
    cf = (t23 < t24) || (ci_t25 && t23 == t24);
    of = (((t23 ^ t24) & (t23 ^ t25)) >> 31) & 1;
    af = ((t23 ^ t24 ^ t25) >> 4) & 1; zf = (t25)==0; sf = (t25)>>31&1; pf = PAR(t25);
L_100005d44ad:;
    /* 100005d44ad: sub eax,1 */
    u32 t27 = (u32)0x1ull; u8 ci_t28 = 0;
    u32 t26 = (u32)rax, t28;
    t28 = t26 - t27 - ci_t28;
    rax = (u32)(t28);
    cf = (t26 < t27) || (ci_t28 && t26 == t27);
    of = (((t26 ^ t27) & (t26 ^ t28)) >> 31) & 1;
    af = ((t26 ^ t27 ^ t28) >> 4) & 1; zf = (t28)==0; sf = (t28)>>31&1; pf = PAR(t28);
    /* 100005d44b0: je short 00000100005D44C5h */
    if ((t26 == t27)) goto L_100005d44c5;
    goto L_100005d44b2;
L_100005d44b2:;
    /* 100005d44b2: sub eax,ebp */
    u32 t30 = (u32)rbp; u8 ci_t31 = 0;
    u32 t29 = (u32)rax, t31;
    t31 = t29 - t30 - ci_t31;
    rax = (u32)(t31);
    cf = (t29 < t30) || (ci_t31 && t29 == t30);
    of = (((t29 ^ t30) & (t29 ^ t31)) >> 31) & 1;
    af = ((t29 ^ t30 ^ t31) >> 4) & 1; zf = (t31)==0; sf = (t31)>>31&1; pf = PAR(t31);
L_100005d44b6:;
L_100005d44bb:;
L_100005d44c0:;
L_100005d44c5:;
    /* 100005d44c5: mov edx,1 */
    rdx = (u32)((u32)0x1ull);
    /* 100005d44ca: jmp short 00000100005D44CFh */
    goto L_100005d44cf;
L_100005d44cc:;
    /* 100005d44cc: mov rdx,r12 */
    rdx = (u64)(r12);
    goto L_100005d44cf;
L_100005d44cf:;
    /* 100005d44cf: mov r9,r12 */
    r9 = (u64)(r12);
L_100005d44db:;
L_100005d44e1:;
L_100005d44e6:;
L_100005d44e9:;
L_100005d4513:;
L_100005d4534:;
    /* 100005d4542: call 00000100005FBC80h */
    rsp -= 8; STS64(rsp, 0x100005d4547ull); c->r[1]=rcx; c->r[2]=rdx; c->r[3]=rbx; c->r[4]=rsp; c->r[5]=rbp; c->r[6]=rsi; c->r[7]=rdi; c->r[8]=r8; c->r[9]=r9; c->r[10]=r10; c->r[11]=r11; c->r[12]=r12; c->r[13]=r13; c->r[14]=r14; c->r[15]=r15; c->x[0]=x0; c->x[1]=x1; f_100005fbc80(c); rax=c->r[0]; rcx=c->r[1]; rdx=c->r[2]; rsp=c->r[4]; r8=c->r[8]; r9=c->r[9]; r10=c->r[10]; r11=c->r[11]; x0=c->x[0]; x1=c->x[1];
L_100005d4549:;
L_100005d454c:;
L_100005d4560:;
L_100005d4574:;
L_100005d458f:;
L_100005d4592:;
L_100005d45a0:;
L_100005d45a3:;
L_100005d45b7:;
L_100005d45d2:;
L_100005d45d5:;
L_100005d45e6:;
L_100005d4630:;
L_100005d4646:;
L_100005d465d:;
L_100005d4678:;
L_100005d467b:;
L_100005d46a7:;
L_100005d46b0:;
L_100005d46d0:;
L_100005d46dd:;
L_100005d46f0:;
L_100005d4709:;
L_100005d4720:;
L_100005d473b:;
L_100005d473e:;
L_100005d4757:;
L_100005d4763:;
L_100005d476a:;
L_100005d4788:;
L_100005d4790:;
    /* 100005d4798: cmp rdx,[rbx+10h] */
    u64 t129 = LD64((rbx+0x10ull)); u8 ci_t130 = 0;
    u64 t128 = rdx, t130;
    t130 = t128 - t129 - ci_t130;
    cf = (t128 < t129) || (ci_t130 && t128 == t129);
    of = (((t128 ^ t129) & (t128 ^ t130)) >> 63) & 1;
    af = ((t128 ^ t129 ^ t130) >> 4) & 1; zf = (t130)==0; sf = (t130)>>63&1; pf = PAR(t130);
L_100005d479e:;
L_100005d47ab:;
L_100005d47b3:;
L_100005d47cd:;
L_100005d480c:;
L_100005d4810:;
L_100005d4830:;
L_100005d4837:;
L_100005d4845:;
L_100005d4850:;
L_100005d485e:;
L_100005d4878:;
L_100005d4880:;
L_100005d4889:;
L_100005d4892:;
L_100005d48c9:;
L_100005d48ce:;
L_100005d48ea:;
L_100005d4903:;
L_100005d4906:;
L_100005d4919:;
L_100005d4926:;
L_100005d4933:;
L_100005d4940:;
L_100005d4951:;
L_100005d496b:;
L_100005d4970:;
L_100005d4979:;
L_100005d4982:;
L_100005d49b9:;
L_100005d49be:;
    /* 100005d49be: mov rax,[rbx+10h] */
    rax = (u64)(LD64((rbx+0x10ull)));
L_100005d49da:;
L_100005d49f3:;
L_100005d49f6:;
L_100005d4a09:;
L_100005d4a16:;
    /* 100005d4a19: cmp rdi,0Eh */
    u64 t296 = 0xeull; u8 ci_t297 = 0;
    u64 t295 = rdi, t297;
    t297 = t295 - t296 - ci_t297;
    cf = (t295 < t296) || (ci_t297 && t295 == t296);
    of = (((t295 ^ t296) & (t295 ^ t297)) >> 63) & 1;
    af = ((t295 ^ t296 ^ t297) >> 4) & 1; zf = (t297)==0; sf = (t297)>>63&1; pf = PAR(t297);
    /* 100005d4a1d: jl near ptr 00000100005D4940h */
    if (((i64)t295 < (i64)t296)) goto L_100005d4940;
    goto L_100005d4a23;
L_100005d4a23:;
L_100005d4a30:;
L_100005d4a45:;
L_100005d4a4f:;
L_100005d4a7a:;
L_100005d4aac:;
L_100005d4aba:;
    /* 100005d4ac3: jmp short 00000100005D4AE1h */
    c->r[0]=rax; c->r[1]=rcx; c->r[2]=rdx; c->r[3]=rbx; c->r[4]=rsp; c->r[5]=rbp; c->r[6]=rsi; c->r[7]=rdi; c->r[8]=r8; c->r[9]=r9; c->r[10]=r10; c->r[11]=r11; c->r[12]=r12; c->r[13]=r13; c->r[14]=r14; c->r[15]=r15; c->x[0]=x0; c->x[1]=x1; f_100005d4ae1(c); return;
}

static void segv(int s, siginfo_t *si, void *u) { char b[64]; int n = snprintf(b, 64, "SIGSEGV at %p\n", si->si_addr); write(1, b, n); _exit(0); }
int main(void) {
    struct sigaction sa = {0}; sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
    if (mmap((void *)0x10000700000ull, 0x100000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0) == MAP_FAILED) return 3;
    if (mmap((void *)0x10001300000ull, 0x200000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0) == MAP_FAILED) return 3;
    u8 *stk = mmap((void *)0x20000000000ull, 1 << 20, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0); u8 *obj = stk + 0x1000;
    *(u32 *)(obj + 0xc) = 0; *(u32 *)(obj + 8) = 1; obj[0x10] = 0;
    Ctx *c = calloc(1, sizeof *c);
    c->r[4] = (u64)stk + (1 << 19); c->r[4] -= 8;   // return address slot
    c->r[1] = 7; c->r[2] = 2; c->r[8] = (u64)obj; c->r[9] = 1;
    f_100005d43f0(c);
    dump(c); return 0;
}
