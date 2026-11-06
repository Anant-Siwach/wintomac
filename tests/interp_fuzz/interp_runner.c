// Interpreter side of the differential test: runs each case with runtime/interp*.c (arm64), and with
// WINTOMAC_JIT_THRESHOLD=0 through the JIT (runtime/jit*.c), every block translated on first execution.
#include "../../runtime/interp_impl.h"
#include "fuzz_common.h"
#include <stdarg.h>

// Minimal runtime services the interpreter links against.
int rt_try_native(Ctx *c, u64 t) { (void)c; (void)t; return 0; }
int rt_jit_is_native(u64 t) { (void)t; return 0; }
GuestFn rt_lookup_fn(u64 t) { (void)t; return 0; }
void rt_trap(Ctx *c, u64 ip, const char *w) { (void)c; printf("TRAP %s at %llx\n", w, (unsigned long long)ip); exit(3); }
void rt_cpuid(Ctx *c) { (void)c; }
u64 rt_rdtsc(void) { return 0; }
u64 rt_random64(void) { return 4; }
u32 rt_crc32c(u32 crc, u64 v, int n) {
    for (int i = 0; i < n; i++) { crc ^= (u8)(v >> (8 * i)); for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0x82F63B78u & -(crc & 1)); }
    return crc;
}
u64 rt_module_base_for(u64 a, u64 *s) { (void)a; (void)s; return 0; }
u64 rt_trace_enabled(void) { return 0; }
void rt_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
void rt_raise_hw(Ctx *c, u64 ip, u32 code) { (void)c; (void)ip; (void)code; }

int main(int argc, char **argv) {
    Case *cs; int n = load_cases(argv[1], &cs);
    uint8_t *scratch = map_scratch();
    uint8_t *page = map_code(PROT_READ | PROT_WRITE);
    static uint8_t stack[65536];
    int only = argc > 2 ? atoi(argv[2]) : -1;
    for (int ci = 0; ci < n; ci++) {
        if (only >= 0 && ci != only) continue;
        for (int si = 0; si < NSTATES; si++) {
            FState s;
            make_state(&s, ci, si, scratch);
            if (getenv("FUZZ_INIT")) print_state(ci, si, "init", &s, s.flags, 0x8D5, scratch);
            memset(page, 0xCC, 4096);
            build_snippet(page, cs[ci].code, cs[ci].n);
            Ctx c; memset(&c, 0, sizeof c);
            memcpy(c.r, s.r, sizeof s.r);
            for (int i = 0; i < 16; i++) { memcpy(&c.x[i], s.x[i], 16); memcpy(&c.xh[i], s.xh[i], 16); }
            c.mxcsr = 0x1f80; c.fcw = 0x37f;
            c.r[RSP] = (u64)(stack + sizeof stack - 64);
            ST64(c.r[RSP], 0);                       // sentinel return address
            u64 fl = s.flags;
            rt_interp_run_ex(&c, (u64)page, &fl);
            memcpy(s.r, c.r, sizeof s.r); s.r[4] = 0;
            for (int i = 0; i < 16; i++) { memcpy(s.x[i], &c.x[i], 16); memcpy(s.xh[i], &c.xh[i], 16); }
            memcpy(&fl, page + 0xF00, 8);
            print_state(ci, si, cs[ci].name, &s, fl, cs[ci].mask, scratch);
        }
    }
    return 0;
}
