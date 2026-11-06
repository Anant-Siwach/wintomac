/* Minimal application start-up for PS5 homebrew run through AnyPS5 (tools/ps5_build.sh, docs/ps5.md).
   The ps5-payload-sdk's own crt1.o expects an ELF loader's payload_args (sceKernelDlsym pointer); a title instead
   is entered like a FreeBSD program: rdi -> { u64 argc; char *argv[]; }, rsi -> exit callback. libc comes from
   libSceLibcInternal (AnyPS5's HLE libc), resolved by name -> NID by the relinker. */
#include <stdint.h>
int main(int argc, char **argv);
void exit(int status) __attribute__((noreturn));
/* Not at image offset 0: the relinker treats entry point 0 as "no entry" (tools/ps5_build.sh links a pad first). */
__attribute__((used, noreturn)) void _start(uint64_t *args, void (*exit_cb)(void)) {
    (void)exit_cb;
    exit(main((int)args[0], (char **)(args + 1)));
}
