# Guest code for tests/src/jit_hot.c (Win64 calling convention). Assembled by tests/jit/gen_code.py, which
# writes the bytes and function offsets into tests/src/jit_hot_code.h.
.intel_syntax noprefix
.text
.globl int_loop
int_loop:                  # u64 int_loop(u64 n): hashing loop (imul, rotates, shifts, cmov, setcc, adc, sar)
  push rbx
  push rsi
  mov r8, rcx
  mov rax, 0x9E3779B97F4A7C15
  xor edx, edx
  xor esi, esi
  mov r9, 1469598103934665603
1:
  mov r10, rdx
  imul r10, rax
  xor r9, r10
  rol r9, 13
  mov r11, r9
  shr r11, 7
  add r9, r11
  cmp r11, r10
  cmovb r10, r11
  setnz bl
  movzx ebx, bl
  add rsi, rbx
  add rsi, r10
  adc rsi, 0
  test dl, 3
  jnz 2f
  sub rsi, r9
  sar rsi, 1
2:
  lea rdx, [rdx + 1]
  cmp rdx, r8
  jb 1b
  mov rax, r9
  xor rax, rsi
  pop rsi
  pop rbx
  ret

.globl mem_loop
mem_loop:                  # u64 mem_loop(u8 *buf, u64 n): loads/stores, misaligned (crossing 16 bytes), RMW
  xor eax, eax
  xor r9d, r9d
1:
  mov r10, r9
  and r10d, 511
  mov r11, qword ptr [rcx + r10 + 1]
  add rax, r11
  ror rax, 3
  movzx r11d, byte ptr [rcx + r10]
  xor rax, r11
  mov dword ptr [rcx + r10 + 520], eax
  add word ptr [rcx + r10 + 3], 7
  inc r9
  cmp r9, rdx
  jne 1b
  ret

.globl fp_loop
fp_loop:                   # i64 fp_loop(const float *v, u64 n): SSE arithmetic, min, sqrt, conversions, comiss
  movups xmm3, [rcx]
  mov eax, 0x3f000000
  movd xmm1, eax
  shufps xmm1, xmm1, 0
  movaps xmm2, xmm3
  xorps xmm0, xmm0
  xor r8d, r8d
  xor r9d, r9d
1:
  mulps xmm2, xmm1
  addps xmm2, xmm3
  addps xmm0, xmm2
  movaps xmm4, xmm2
  shufps xmm4, xmm4, 0x1b
  minps xmm4, xmm2
  sqrtss xmm5, xmm4
  cvttss2si r10d, xmm5
  add r8, r10
  comiss xmm5, xmm1
  jbe 2f
  inc r8
2:
  inc r9
  cmp r9, rdx
  jb 1b
  movhlps xmm4, xmm0
  addps xmm0, xmm4
  movaps xmm4, xmm0
  shufps xmm4, xmm4, 0x55
  addss xmm0, xmm4
  cvttss2si rax, xmm0
  add rax, r8
  ret

.globl cb_loop
cb_loop:                   # u64 cb_loop(u64 (*fn)(u64), u64 n): calls back into C n times
  push rbx
  push rsi
  push rdi
  sub rsp, 32
  mov rbx, rcx
  mov rsi, rdx
  xor edi, edi
1:
  mov rcx, rsi
  call rbx
  add rdi, rax
  dec rsi
  jnz 1b
  mov rax, rdi
  add rsp, 32
  pop rdi
  pop rsi
  pop rbx
  ret

.globl square
square:                    # u64 square(u64 x): called from the C callback (C -> JIT -> C -> JIT)
  mov rax, rcx
  imul rax, rcx
  ret

.globl switch_loop
switch_loop:               # u64 switch_loop(u64 n): jump-table dispatch (indirect jmp)
  xor eax, eax
  xor edx, edx
  lea r8, [rip + jt]
1:
  mov r9, rdx
  and r9d, 3
  movsxd r10, dword ptr [r8 + r9*4]
  add r10, r8
  jmp r10
c0:
  add rax, rdx
  jmp 2f
c1:
  xor rax, rdx
  jmp 2f
c2:
  imul rax, rax, 3
  jmp 2f
c3:
  rol rax, 5
2:
  inc rdx
  cmp rdx, rcx
  jb 1b
  ret
.p2align 2
jt:
  .long c0 - jt, c1 - jt, c2 - jt, c3 - jt

.globl fib
fib:                       # i64 fib(i64 n): recursion inside the buffer (direct calls, returns via lookup)
  cmp rcx, 2
  jl 3f
  push rbx
  push rdi
  sub rsp, 40
  mov rbx, rcx
  lea rcx, [rbx - 1]
  call fib
  mov rdi, rax
  lea rcx, [rbx - 2]
  call fib
  add rax, rdi
  add rsp, 40
  pop rdi
  pop rbx
  ret
3:
  mov rax, rcx
  ret

.globl atomic_loop
atomic_loop:               # u64 atomic_loop(u64 *cells, u64 n): lock xadd / lock inc / lock cmpxchg loop
  xor r9d, r9d
1:
  mov eax, 1
  lock xadd qword ptr [rcx], rax
  lock inc qword ptr [rcx + 8]
  mov rax, qword ptr [rcx + 16]
2:
  lea r10, [rax + 3]
  lock cmpxchg qword ptr [rcx + 16], r10
  jnz 2b
  inc r9
  cmp r9, rdx
  jne 1b
  mov rax, r9
  ret

.globl get_const
get_const:                 # u32 get_const(void): its immediate gets patched
  mov eax, 0x11111111
  ret

.globl smc_loop
smc_loop:                  # u64 smc_loop(u32 *imm, u64 n): sum += get_const(); *imm = n-i (the code patches code it runs)
  push rbx
  push rsi
  push rdi
  sub rsp, 32
  mov rbx, rcx
  mov rsi, rdx
  xor edi, edi
1:
  call get_const
  add rdi, rax
  mov dword ptr [rbx], esi
  dec rsi
  jnz 1b
  mov rax, rdi
  add rsp, 32
  pop rdi
  pop rsi
  pop rbx
  ret

.globl str_loop
str_loop:                  # u64 str_loop(u8 *dst, u64 n): rep stosb / rep movsb / repne scasb (interpreter fallback inside JIT code)
  push rdi
  push rsi
  mov r8, rcx
  mov r9, rdx
  xor r10d, r10d
1:
  mov rdi, r8
  mov al, r9b
  or al, 1
  mov ecx, 64
  rep stosb
  lea rsi, [r8]
  lea rdi, [r8 + 64]
  mov ecx, 64
  rep movsb
  mov byte ptr [r8 + 100], 0
  mov rdi, r8
  xor eax, eax
  mov rcx, -1
  repne scasb
  not rcx
  add r10, rcx
  dec r9
  jnz 1b
  mov rax, r10
  pop rsi
  pop rdi
  ret
.globl end_of_code
end_of_code:
