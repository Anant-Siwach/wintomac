// wintomac runtime: maps the original PE image, wires imports to shims, dispatches guest calls.
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <pthread.h>

#include "win32.h"
#include "interp.h"

#define STACK_SIZE (8ull << 20)
#define THUNK_SLOTS 8192

u64 rt_teb, rt_peb;
int rt_argc;
char **rt_argv;

static u64 thunk_base;                // fake addresses written into the IAT
static ShimFn thunk_fn[THUNK_SLOTS];
static const char *thunk_name[THUNK_SLOTS];
static int nthunks;
static int trace = -1;

u64 rt_trace_enabled(void) {
    if (trace < 0) trace = getenv("WINTOMAC_TRACE") != NULL;
    return trace;
}

void rt_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[rt] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

static void dump_ctx(Ctx *c) {
    static const char *n[] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
    for (int i = 0; i < 16; i++) fprintf(stderr, "  %-3s=%016llx%s", n[i], (unsigned long long)c->r[i], i % 4 == 3 ? "\n" : "");
}

// ---- dispatch ------------------------------------------------------------
// Address -> recompiled function: binary search over rt_funcs[], fronted by a direct-mapped cache of
// pointers to (immutable) table entries, so a racing reader sees either NULL or a complete entry.
#define LOOKUP_CACHE_BITS 14
static const RtFunc *lookup_cache[1u << LOOKUP_CACHE_BITS];
static inline u32 lookup_hash(u64 a) { return (u32)((a * 0x9E3779B97F4A7C15ull) >> (64 - LOOKUP_CACHE_BITS)); }
static const RtFunc *lookup_entry(u64 addr) {
    u32 h = lookup_hash(addr);
    const RtFunc *e = __atomic_load_n(&lookup_cache[h], __ATOMIC_RELAXED);
    if (e && e->addr == addr) return e;
    u64 lo = 0, hi = rt_nfuncs;
    while (lo < hi) {
        u64 mid = (lo + hi) / 2;
        if (rt_funcs[mid].addr == addr) { __atomic_store_n(&lookup_cache[h], &rt_funcs[mid], __ATOMIC_RELAXED); return &rt_funcs[mid]; }
        if (rt_funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
static GuestFn lookup(u64 addr) { const RtFunc *e = lookup_entry(addr); return e ? e->fn : NULL; }

GuestFn rt_lookup_fn(u64 addr) { return lookup(addr); }
u64 rt_seh_lookup(u64 pc, u64 *image_base);

static void miss(Ctx *c, u64 target) {
    // Log for the next recompile pass (`--seeds-file`), then run the code in the interpreter (runtime/interp.c).
    rt_note_miss(target, 0);
    if (!rt_interp_disabled()) { rt_interp_run(c, target); return; }
    rt_log("call to unrecompiled address 0x%llx (logged to wintomac_misses.txt; re-run recomp with --seeds-file)",
           (unsigned long long)target);
    dump_ctx(c);
    exit(70);
}

static inline int is_thunk(u64 t) { return t >= thunk_base && t < thunk_base + 16ull * THUNK_SLOTS; }

static __thread int cur_thunk;
__thread Ctx *rt_cur_ctx;
static void missing_import(Ctx *c);
// Every import call lands here. macOS thread-locals cost a call each, so they are only written when needed.
static void call_thunk(Ctx *c, u64 t) {
    int i = (int)((t - thunk_base) / 16);
    ShimFn fn = thunk_fn[i];
    if (__builtin_expect(fn == missing_import, 0)) cur_thunk = i;
    if (__builtin_expect(rt_cur_ctx != c, 0)) rt_cur_ctx = c;
    if (__builtin_expect(trace > 0, 0)) rt_log("-> %s(%llx, %llx, %llx, %llx)", thunk_name[i],
        (unsigned long long)c->r[RCX], (unsigned long long)c->r[RDX], (unsigned long long)c->r[R8], (unsigned long long)c->r[R9]);
    fn(c);
    if (__builtin_expect(trace > 0, 0)) rt_log("<- %s = %llx", thunk_name[i], (unsigned long long)c->r[RAX]);
    c->r[RSP] += 8;  // emulate the callee's `ret`
}

void rt_call(Ctx *c, u64 target) {
    if (is_thunk(target)) { call_thunk(c, target); return; }
    // Executable memory from VirtualAlloc/VirtualProtect (JIT output, patched code) is always interpreted.
    GuestFn fn = (rt_interp_exec_in_images && rt_interp_is_exec(target)) ? NULL : lookup(target);
    if (!fn) {
        if (rt_interp_is_exec(target) && !rt_interp_disabled()) { rt_interp_run(c, target); return; }
        // An Itanium-ABI unwinder jumping to a landing pad: longjmps to the owning frame (runtime/seh.c)
        extern void rt_landing_pad(Ctx *c, u64 target);
        rt_landing_pad(c, target);
        // Not a function start: a secondary entry (e.g. a jump back from a .cold chunk into the middle of its
        // hot function). Enter the function that owns the address at that label.
        u64 base, fe = rt_seh_lookup(target, &base);
        extern u64 rt_func_root(u64 base, u64 fe);
        GuestFn owner = fe ? lookup(rt_func_root(base, fe)) : NULL;
        if (!owner) { miss(c, target); return; }
        c->resume_target = target;
        owner(c);
        if (c->resume_target) { c->resume_target = 0; miss(c, target); }
        return;
    }
    fn(c);
}

// Indirect call site with an inline cache (rt_icall in recomp.h): the fast path missed. Same as rt_call, and
// a plain recompiled-function target is remembered for the site.
void rt_call_ic(Ctx *c, u64 target, const RtFunc **ic) {
    if (is_thunk(target)) { call_thunk(c, target); return; }
    if (!rt_interp_exec_in_images) {
        const RtFunc *e = lookup_entry(target);
        if (e) { __atomic_store_n(ic, e, __ATOMIC_RELAXED); e->fn(c); return; }
    }
    rt_call(c, target);
}

// For the interpreter: run target natively if it is an import thunk or a recompiled function entry.
int rt_try_native(Ctx *c, u64 target) {
    if (is_thunk(target)) { call_thunk(c, target); return 1; }
    if (rt_interp_exec_in_images && rt_interp_is_exec(target)) return 0;
    GuestFn fn = lookup(target);
    if (!fn) return 0;
    fn(c);
    return 1;
}

void rt_jump(Ctx *c, u64 target) { rt_call(c, target); }

// For the JIT (runtime/jit*.c): would rt_try_native run this target natively?
int rt_jit_is_native(u64 target) { return is_thunk(target) || lookup(target) != NULL; }

// The lifter couldn't translate the instruction at ip. Generated code syncs Ctx before calling this, so the
// interpreter can run the rest of the function from ip; its `ret` returns here and the function returns.
// Flags live in C locals in generated code, so they are passed in (RFLAGS layout) and the interpreter starts with them.
void rt_unimplemented(Ctx *c, u64 ip, const char *what, u64 rflags) {
    if (!rt_interp_disabled()) { rt_note_fallback(ip, what); rt_interp_run_ex(c, ip, &rflags); return; }
    rt_log("unimplemented instruction at 0x%llx: %s", (unsigned long long)ip, what);
    dump_ctx(c); exit(71);
}

void rt_raise_hw(Ctx *c, u64 ip, u32 code);
void rt_trap(Ctx *c, u64 ip, const char *what) {
    u32 code = !strcmp(what, "#DE") ? 0xC0000094u      /* EXCEPTION_INT_DIVIDE_BY_ZERO */
             : !strcmp(what, "int3") ? 0x80000003u     /* EXCEPTION_BREAKPOINT */
             : !strcmp(what, "ud2") ? 0xC000001Du      /* EXCEPTION_ILLEGAL_INSTRUCTION */
             : 0;
    if (code && !getenv("WINTOMAC_NO_HW_SEH")) rt_raise_hw(c, ip, code);   // a guest __try/__except may handle it
    rt_log("guest trap at 0x%llx: %s", (unsigned long long)ip, what);
    dump_ctx(c); exit(72);
}

void rt_fell_off(Ctx *c, u64 ip, u64 rflags) {   // e.g. a call the analysis thought was noreturn returned: keep going, interpreted
    if (!rt_interp_disabled()) { rt_note_fallback(ip, "fell off decoded code"); rt_interp_run_ex(c, ip, &rflags); return; }
    rt_log("control fell off decoded code at 0x%llx (noreturn call returned?)", (unsigned long long)ip);
    dump_ctx(c); exit(73);
}

void rt_cpuid(Ctx *c) {
    // Advertise a Zen 2 / Haswell-class feature set: everything the lifter implements, nothing it doesn't
    // (no AVX-512, no AES/PCLMUL/SHA yet), so feature-dispatching code picks paths we can run.
    u32 leaf = (u32)c->r[RAX], sub = (u32)c->r[RCX];
    u32 a = 0, b = 0, cc = 0, d = 0;
    switch (leaf) {
    case 0: a = 0xd; b = 0x756e6547; d = 0x49656e69; cc = 0x6c65746e; break;  // "GenuineIntel"
    case 1: a = 0x000906ea; b = 0x00100800;
            d = (1u<<0)|(1u<<4)|(1u<<8)|(1u<<15)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26);   // fpu tsc cx8 cmov mmx fxsr sse sse2
            cc = (1u<<0)|(1u<<9)|(1u<<12)|(1u<<13)|(1u<<19)|(1u<<20)|(1u<<22)|(1u<<23)   // sse3 ssse3 fma cx16 sse4.1 sse4.2 movbe popcnt
               | (1u<<27)|(1u<<28)|(1u<<29);                                             // osxsave avx f16c
            break;
    case 7: if (sub == 0) b = (1u<<3)|(1u<<5)|(1u<<8)|(1u<<19); break;                  // bmi1 avx2 bmi2 adx
    case 0xd: if (sub == 0) { a = 7; b = 0x340; cc = 0x340; } break;                     // xcr0 x87|sse|avx, 832-byte area
    case 0x80000000: a = 0x80000004; break;
    case 0x80000001: cc = (1u<<0)|(1u<<5); d = (1u<<29)|(1u<<11)|(1u<<27); break;       // lahf, lzcnt / lm, syscall, rdtscp
    default: break;
    }
    c->r[RAX] = a; c->r[RBX] = b; c->r[RCX] = cc; c->r[RDX] = d;
}

u32 rt_crc32c(u32 crc, u64 v, int n) {
    for (int i = 0; i < n; i++) {
        crc ^= (u8)(v >> (8 * i));
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0x82F63B78u & -(crc & 1));
    }
    return crc;
}

u64 rt_rdtsc(void) { return mach_absolute_time(); }
u64 rt_random64(void) { u64 v; arc4random_buf(&v, 8); return v; }

// ---- host -> guest calls ----------------------------------------------------
// Pushes a sentinel return address; the guest function's `ret` pops it and returns host-side.
u64 rt_guest_call(Ctx *c, u64 fn, int nargs, const u64 *args) {
    Ctx saved = *c;
    int nstack = nargs > 4 ? nargs - 4 : 0;
    // Entry layout: [sp]=ret, [sp+8..0x28)=home space, [sp+0x28+8k]=arg(5+k); sp % 16 == 8.
    u64 a = (c->r[RSP] - 0x100 - 8ull * nstack) & ~0xFull;
    for (int k = 0; k < nstack; k++) ST64(a + 8ull * k, args[4 + k]);
    u64 sp = a - 0x28;
    ST64(sp, 0);
    static const int regs[4] = {RCX, RDX, R8, R9};
    for (int i = 0; i < nargs && i < 4; i++) c->r[regs[i]] = args[i];
    c->r[RSP] = sp;
    rt_call(c, fn);
    u64 ret = c->r[RAX];
    *c = saved;
    c->r[RAX] = ret;
    return ret;
}

// ---- handles ----------------------------------------------------------------
#define MAX_HANDLES 4096
static struct { int kind; intptr_t host; } handles[MAX_HANDLES];

static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;
u64 rt_handle_new(int kind, intptr_t host) {
    pthread_mutex_lock(&handle_lock);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!handles[i].kind) { handles[i].kind = kind; handles[i].host = host; pthread_mutex_unlock(&handle_lock); return (u64)i * 4 + 0x1000; }
    pthread_mutex_unlock(&handle_lock);
    return 0;
}
int rt_handle_get(u64 h, int kind, intptr_t *host) {
    u64 i = (h - 0x1000) / 4;
    if (h < 0x1000 || i >= MAX_HANDLES || handles[i].kind != kind) return 0;
    *host = handles[i].host; return 1;
}
void rt_handle_close(u64 h) {
    u64 i = (h - 0x1000) / 4;
    if (h >= 0x1000 && i < MAX_HANDLES) handles[i].kind = H_NONE;
}

// ---- loader ---------------------------------------------------------------
static void missing_import(Ctx *c) {
    rt_log("called import with no shim: %s (ret addr %llx)", thunk_name[cur_thunk], (unsigned long long)LD64(c->r[RSP]));
    dump_ctx(c);
    exit(74);
}

extern const Shim shims_extra[];
extern const Shim shims_extra2[];
extern const Shim shims_opengl[], shims_gl_gen[];
extern const Shim shims_interp[];
extern const Shim shims_ps5[], shims_ps5_vmem[];   // ps5_shims.c: imports of AnyPS5's Windows libraries (docs/ps5.md)
const char *rt_extra_ordinal(const char *dll, u16 ord);
static const Shim *const shim_tables[] = { shims_seh, shims_kernel32, shims_files, shims_msvcrt, shims_threads, shims_user32,
                                           shims_misc, shims_d3dcompiler, shims_d3dreflect, shims_com, shims_xaudio2, shims_xinput, shims_dsound, shims_wasapi, shims_winsock, shims_extra, shims_extra2, shims_vulkan, shims_vulkan_gen, shims_opengl, shims_gl_gen, shims_d3d12, shims_d3d12_gen, shims_d3d12_ext, shims_d3d12_pso, shims_dxgi12, shims_interp, shims_advapi32, shims_kernel32x, shims_crt2, shims_win32x, shims_steam, shims_ps5, shims_ps5_vmem, NULL };

static ShimFn find_shim(const char *dll, const char *name) {
    for (int t = 0; shim_tables[t]; t++)
        for (const Shim *s = shim_tables[t]; s->name; s++)
            if (!strcmp(s->name, name) && (!s->dll || !strcasecmp(s->dll, dll))) return s->fn;
    return NULL;
}

// GetProcAddress support: a thunk for a shim looked up by name at runtime (0 if no shim exists).
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;
u64 rt_thunk_for(const char *dll, const char *name) {
    ShimFn fn = find_shim(dll ? dll : "", name);
    if (!fn) {
        for (int t = 0; shim_tables[t] && !fn; t++)   // GetProcAddress often passes a module we don't track
            for (const Shim *s = shim_tables[t]; s->name; s++) if (!strcmp(s->name, name)) { fn = s->fn; break; }
        if (!fn) return 0;
    }
    pthread_mutex_lock(&thunk_lock);
    for (int i = 0; i < nthunks; i++)
        if (thunk_fn[i] == fn) { pthread_mutex_unlock(&thunk_lock); return thunk_base + 16ull * i; }
    u64 r = 0;
    if (nthunks < THUNK_SLOTS) { thunk_fn[nthunks] = fn; thunk_name[nthunks] = strdup(name); r = thunk_base + 16ull * nthunks++; }
    pthread_mutex_unlock(&thunk_lock);
    return r;
}

// ---- images: map, relocate, bind, initialise --------------------------------------
typedef struct { const RtImage *img; int loaded, inited; char path[1024]; } Module;
static Module mods[64];
static pthread_mutex_t mod_lock = PTHREAD_MUTEX_INITIALIZER;
static char exe_dir[1024];
const char *rt_exe_dir(void) { return exe_dir; }

static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}
// "D3D11.DLL", "d3d11", "C:\x\d3d11.dll" all name the same module
static int same_module(const char *a, const char *b) {
    a = base_name(a); b = base_name(b);
    size_t la = strlen(a), lb = strlen(b);
    if (la > 4 && !strcasecmp(a + la - 4, ".dll")) la -= 4;
    if (lb > 4 && !strcasecmp(b + lb - 4, ".dll")) lb -= 4;
    return la == lb && !strncasecmp(a, b, la);
}

static Module *mod_by_name(const char *name) {
    for (u64 i = 0; i < rt_nimages; i++) if (same_module(rt_images[i].name, name)) return &mods[i];
    return NULL;
}
static Module *mod_by_base(u64 base) {
    for (u64 i = 0; i < rt_nimages; i++) if (rt_images[i].base == base) return &mods[i];
    return NULL;
}
static Module *mod_for_addr(u64 a) {
    for (u64 i = 0; i < rt_nimages; i++) if (a >= rt_images[i].base && a < rt_images[i].base + rt_images[i].size) return &mods[i];
    return NULL;
}

static u32 dir_rva(u64 base, int idx, u32 *size) {
    u32 pe = LD32(base + 0x3c);
    u64 d = base + pe + 24 + 112 + 8 * idx;
    if (size) *size = LD32(d + 4);
    return LD32(d);
}

static u64 export_lookup(Module *m, const char *name, u16 ordinal, int depth);

static u64 resolve_forwarder(const char *fwd, int depth) {  // "OTHERDLL.Func" or "OTHERDLL.#12"
    const char *dot = strrchr(fwd, '.');
    if (!dot || depth > 8) return 0;
    char dll[256]; size_t n = (size_t)(dot - fwd); if (n >= sizeof dll) return 0;
    memcpy(dll, fwd, n); dll[n] = 0;
    Module *m = mod_by_name(dll);
    if (m && m->loaded) return dot[1] == '#' ? export_lookup(m, NULL, (u16)atoi(dot + 2), depth + 1) : export_lookup(m, dot + 1, 0, depth + 1);
    return dot[1] == '#' ? 0 : rt_thunk_for(dll, dot + 1);
}

static u64 export_lookup(Module *m, const char *name, u16 ordinal, int depth) {
    u64 base = m->img->base; u32 esz;
    u32 erva = dir_rva(base, 0, &esz);
    if (!erva) return 0;
    u64 ed = base + erva;
    u32 obase = LD32(ed + 16), nfuncs = LD32(ed + 20), nnames = LD32(ed + 24);
    u64 funcs = base + LD32(ed + 28), names = base + LD32(ed + 32), ords = base + LD32(ed + 36);
    u32 idx = 0xFFFFFFFF;
    if (name) {
        u32 lo = 0, hi = nnames;  // export names are sorted
        while (lo < hi) {
            u32 mid = (lo + hi) / 2; int c = strcmp((const char *)(base + LD32(names + 4ull * mid)), name);
            if (!c) { idx = LD16(ords + 2ull * mid); break; }
            if (c < 0) lo = mid + 1; else hi = mid;
        }
    } else if (ordinal >= obase) idx = ordinal - obase;
    if (idx >= nfuncs) return 0;
    u32 frva = LD32(funcs + 4ull * idx);
    if (!frva) return 0;
    if (frva >= erva && frva < erva + esz) return resolve_forwarder((const char *)(base + frva), depth);
    return base + frva;
}

static void apply_relocs(u64 base, u64 delta) {
    u32 size, rva = dir_rva(base, 5, &size);
    u64 p = base + rva, end = p + size;
    while (rva && p < end) {
        u32 page = LD32(p), blk = LD32(p + 4);
        if (blk < 8) break;
        for (u32 j = 0; j < (blk - 8) / 2; j++) {
            u16 e = LD16(p + 8 + 2 * j);
            if (e >> 12 == 10) { u64 a = base + page + (e & 0xFFF); ST64(a, LD64(a) + delta); }
        }
        p += blk;
    }
}

static int find_file(const RtImage *img, int idx, const char *exe_path, char *out, size_t n) {
    if (idx == 0) { snprintf(out, n, "%s", exe_path); return access(out, R_OK) == 0; }
    const char *dirs[3] = { exe_dir, getenv("WINTOMAC_DLLPATH"), NULL };
    for (int i = 0; i < 2; i++) {
        if (!dirs[i] || !*dirs[i]) continue;
        snprintf(out, n, "%s/%s", dirs[i], img->name);
        if (access(out, R_OK) == 0) return 1;
    }
    snprintf(out, n, "%s", img->path);  // where it was when we recompiled it
    return access(out, R_OK) == 0;
}

static void map_image(Module *m, const char *path) {
    const RtImage *img = m->img;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { rt_log("cannot open %s: %s", path, strerror(errno)); exit(2); }
    struct stat st; fstat(fd, &st);
    u8 *file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED || file[0] != 'M' || file[1] != 'Z') { rt_log("%s is not a PE file", path); exit(2); }
    u32 pe = *(u32 *)(file + 0x3c);
    u16 nsec = *(u16 *)(file + pe + 6), optsz = *(u16 *)(file + pe + 20);
    u32 stamp = *(u32 *)(file + pe + 8);
    u8 *opt = file + pe + 24;
    u64 pref = *(u64 *)(opt + 24);
    u32 hdrsz = *(u32 *)(opt + 60), imgsz = *(u32 *)(opt + 56);
    if (pref != img->orig_base || imgsz != img->size || stamp != img->timestamp) {
        rt_log("%s does not match the image that was recompiled (%s)", path, img->path);
        exit(2);
    }
    u64 size = (img->size + 0x3fff) & ~0x3fffull;  // 16K host pages
    // No MAP_FIXED: a hint the kernel honours only if the range is free, so host mappings are never clobbered.
    void *mem = mmap((void *)img->base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem != (void *)img->base) {
        rt_log("could not map %s at %llx (got %p): %s", img->name, (unsigned long long)img->base, mem, strerror(errno));
        exit(2);
    }
    memcpy(mem, file, hdrsz);
    u8 *sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        u32 vsize = *(u32 *)(sec + 8), rva = *(u32 *)(sec + 12), rawsz = *(u32 *)(sec + 16), rawoff = *(u32 *)(sec + 20);
        u32 n = rawsz < vsize || !vsize ? rawsz : vsize;
        memcpy((u8 *)mem + rva, file + rawoff, n);
    }
    munmap(file, st.st_size);
    if (img->base != img->orig_base) {
        apply_relocs(img->base, img->base - img->orig_base);
        for (u64 k = 0; k < img->nsynth; k++) {   // relocations the recompiler recovered (no .reloc in the file)
            u64 a = img->base + img->synth_relocs[k]; ST64(a, LD64(a) + (img->base - img->orig_base));
        }
        ST64(img->base + LD32(img->base + 0x3c) + 24 + 24, img->base);  // OptionalHeader.ImageBase
    }
    snprintf(m->path, sizeof m->path, "%s", path);
    m->loaded = 1;
}

// Exports that only exist by ordinal in the real DLLs.
static const char *ordinal_name(const char *dll, u16 ord) {
    if (!strncasecmp(dll, "xinput", 6)) {
        if (ord == 100) return "XInputGetStateEx";
        if (ord == 103) return "XInputPowerOffController";
    }
    const char *x = rt_extra_ordinal(dll, ord); if (x) return x;
    if (!strncasecmp(dll, "ws2_32", 6) || !strncasecmp(dll, "wsock32", 7)) return rt_winsock_ordinal(ord);
    return NULL;
}

static void bind_imports(Module *m) {
    int missing = 0;
    for (u64 i = 0; i < m->img->nimports; i++) {
        const RtImport *im = &m->img->imports[i];
        Module *dep = mod_by_name(im->dll);
        u64 target = 0;
        if (dep && dep->loaded) {
            target = export_lookup(dep, im->name, im->ordinal, 0);
            if (!target) rt_log("%s: %s has no export %s", m->img->name, im->dll, im->name ? im->name : "(ordinal)");
        }
        if (!target) {
            const char *nm = im->name ? im->name : ordinal_name(im->dll, im->ordinal);
            ShimFn fn = nm ? find_shim(im->dll, nm) : NULL;
            if (!fn) { fn = missing_import; missing++; if (getenv("WINTOMAC_VERBOSE")) rt_log("no shim: %s!%s", im->dll, im->name ? im->name : "(ordinal)"); }
            pthread_mutex_lock(&thunk_lock);
            thunk_fn[nthunks] = fn; thunk_name[nthunks] = im->name ? im->name : "(ordinal)";
            target = thunk_base + 16ull * nthunks++;
            pthread_mutex_unlock(&thunk_lock);
        }
        ST64(im->iat, target);
    }
    if (missing && getenv("WINTOMAC_VERBOSE")) rt_log("%s: %d imports have no shim yet", m->img->name, missing);
    rt_bind_data_imports(m->img->base);   // CRT data exports (_acmdln, _fmode, _iob, ...): IAT slots -> variables
}

static Ctx *main_ctx;
// DLL_PROCESS_ATTACH in dependency order (a DLL's imports are initialised before it), like the NT loader.
static int init_module(Module *m) {
    if (m->inited) return 1;
    m->inited = 1;
    for (u64 i = 0; i < m->img->nimports; i++) {
        Module *dep = mod_by_name(m->img->imports[i].dll);
        if (dep && dep != m) init_module(dep);
    }
    if (!m->img->is_dll) return 1;
    for (u64 i = 0; i < m->img->ntls; i++) {
        u64 args[3] = { m->img->base, 1, 0 };
        rt_guest_call(rt_cur_ctx ? rt_cur_ctx : main_ctx, m->img->tls[i], 3, args);
    }
    if (m->img->entry) {
        if (getenv("WINTOMAC_VERBOSE")) rt_log("DllMain(%s)", m->img->name);
        u64 args[3] = { m->img->base, 1 /* DLL_PROCESS_ATTACH */, 0 };
        if (!(u32)rt_guest_call(rt_cur_ctx ? rt_cur_ctx : main_ctx, m->img->entry, 3, args)) {
            rt_log("DllMain(%s) returned FALSE", m->img->name);
            return 0;
        }
    }
    return 1;
}

// ---- module API for kernel32 (GetModuleHandle / LoadLibrary / GetProcAddress / ...) ------------
// A recompiled DLL is mapped at startup but only "loaded" in the Windows sense once it is initialised (imported by
// a loaded module, or LoadLibrary'd). Until then GetModuleHandle must fail like on Windows: MSVC's delay-load helper
// (GZDoom delay-loads openal32.dll) asks GetModuleHandle first and, given a handle, skips LoadLibrary, so DllMain
// and the DLL's C++ static constructors would never run.
u64 rt_module_handle(const char *name) {  // loaded recompiled image, or 0
    Module *m = name ? mod_by_name(name) : &mods[0];
    return m && m->loaded && (m->inited || !m->img->is_dll) ? m->img->base : 0;
}
int rt_module_is_recompiled(const char *name) { Module *m = mod_by_name(name); return m && m->loaded; }
u64 rt_module_load(const char *name) {    // LoadLibrary: initialise on first use
    Module *m = mod_by_name(name);
    if (!m || !m->loaded) return 0;
    pthread_mutex_lock(&mod_lock);
    int ok = init_module(m);
    pthread_mutex_unlock(&mod_lock);
    return ok ? m->img->base : 0;
}
u64 rt_module_export(u64 base, const char *name, u16 ordinal) {
    Module *m = mod_by_base(base);
    return m && m->loaded ? export_lookup(m, name, ordinal, 0) : 0;
}
const char *rt_module_path(u64 base) { Module *m = mod_by_base(base); return m && m->loaded ? m->path : NULL; }
u64 rt_module_base_for(u64 addr, u64 *size) {
    Module *m = mod_for_addr(addr);
    if (!m) return 0;
    if (size) *size = m->img->size;
    return m->img->base;
}

// Static TLS (IMAGE_TLS_DIRECTORY64): each image with a TLS directory gets an index; every thread gets a copy
// of the template, reachable through TEB.ThreadLocalStoragePointer (gs:[0x58]).
void rt_setup_thread_tls(u64 teb) {
    u64 *slots = calloc(rt_nimages + 1, sizeof(u64));
    for (u64 i = 0; i < rt_nimages; i++) {
        const RtImage *im = &rt_images[i];
        if (!im->tls_index) continue;
        u64 n = im->tls_end - im->tls_start, total = n + im->tls_zero;
        u8 *blk = aligned_alloc(64, (total + 63) & ~63ull);
        memset(blk, 0, (total + 63) & ~63ull);
        if (n) memcpy(blk, (void *)im->tls_start, n);
        slots[i] = (u64)blk;
        ST32(im->tls_index, (u32)i);
    }
    ST64(teb + 0x58, (u64)slots);
}

// Fields of the PEB that Windows code reads *directly* (statically linked CRTs, engines, anti-debug checks),
// bypassing any API. Offsets are the documented x64 layouts.
static u64 wstr_dup(const char *a) { size_t n = strlen(a); u16 *w = calloc(n + 1, 2); for (size_t i = 0; i < n; i++) w[i] = (u8)a[i]; return (u64)w; }
static void unicode_string(u64 at, const char *s) {   // UNICODE_STRING { u16 Length, MaximumLength; PWSTR Buffer }
    u64 w = wstr_dup(s); u16 n = (u16)(2 * strlen(s));
    ST16(at, n); ST16(at + 2, (u16)(n + 2)); ST64(at + 8, w);
}
static void populate_peb(u64 peb) {
    // RTL_USER_PROCESS_PARAMETERS
    u64 pp = (u64)calloc(1, 0x440);
    ST32(pp + 0x00, 0x440); ST32(pp + 0x04, 0x440); ST32(pp + 0x08, 1 /* PROCESS_PARAMETERS_NORMALIZED */);
    unicode_string(pp + 0x38, "C:\\");                              // CurrentDirectory.DosPath
    unicode_string(pp + 0x50, "C:\\windows\\system32");          // DllPath
    unicode_string(pp + 0x60, "C:\\wintomac\\game.exe");          // ImagePathName
    { size_t n = 16; for (int i = 1; i < rt_argc; i++) n += strlen(rt_argv[i]) + 3;
      char *cl = calloc(1, n); strcpy(cl, "game.exe");
      for (int i = 1; i < rt_argc; i++) { strcat(cl, " "); strcat(cl, rt_argv[i]); }
      unicode_string(pp + 0x70, cl); free(cl); }                     // CommandLine
    { extern char **environ; size_t n = 1; for (char **e = environ; *e; e++) n += strlen(*e) + 1;
      u16 *env = calloc(n, 2), *q = env;
      for (char **e = environ; *e; e++) { for (const char *t = *e; *t; t++) *q++ = (u8)*t; *q++ = 0; }
      ST64(pp + 0x80, (u64)env); ST64(pp + 0x3F0, 2 * n); }          // Environment, EnvironmentSize
    ST32(pp + 0xA8, 1);                                              // ShowWindowFlags = SW_SHOWNORMAL
    unicode_string(pp + 0xB0, "game.exe");                          // WindowTitle
    ST64(peb + 0x20, pp);                                            // ProcessParameters
    // Loader data with empty (self-linked) module lists, so code walking them terminates.
    u64 ldr = (u64)calloc(1, 0x58);
    ST32(ldr, 0x58); ST8(ldr + 4, 1);                                // Length, Initialized
    for (int k = 0; k < 3; k++) { u64 l = ldr + 0x10 + 0x10ull * k; ST64(l, l); ST64(l + 8, l); }
    ST64(peb + 0x18, ldr);
    ST8(peb + 0x02, 0);                                              // BeingDebugged
    ST64(peb + 0x30, 0x7e57);                                        // ProcessHeap (== GetProcessHeap())
    ST32(peb + 0xB8, (u32)sysconf(_SC_NPROCESSORS_ONLN));           // NumberOfProcessors
    ST32(peb + 0x118, 10); ST32(peb + 0x11C, 0); ST16(peb + 0x120, 19045); ST32(peb + 0x124, 2);   // OS version
    ST32(peb + 0xBC, 0);                                             // NtGlobalFlag
}

static void setup_teb(Ctx *c, u64 stack_lo, u64 stack_hi) {
    u8 *teb = calloc(1, 0x2000), *peb = calloc(1, 0x1000);
    rt_teb = (u64)teb; rt_peb = (u64)peb;
    ST64(rt_teb + 0x08, stack_hi);     // NT_TIB.StackBase
    ST64(rt_teb + 0x10, stack_lo);     // NT_TIB.StackLimit
    ST64(rt_teb + 0x30, rt_teb);       // NT_TIB.Self
    ST64(rt_teb + 0x60, rt_peb);       // ProcessEnvironmentBlock
    ST32(rt_teb + 0x40, (u32)getpid()); // ClientId.UniqueProcess
    ST32(rt_teb + 0x48, 1);            // ClientId.UniqueThread
    ST64(rt_peb + 0x10, rt_image_base); // ImageBaseAddress
    populate_peb(rt_peb);
    c->gs_base = rt_teb;
    rt_setup_thread_tls(rt_teb);
}

// ---- crash reporting: guest backtrace on SIGSEGV/SIGBUS --------------------------------------------
#include <signal.h>
static const char *nearest_export(u64 ib, u64 addr, u64 *off) {
    u32 esz, erva = dir_rva(ib, 0, &esz); if (!erva) return NULL;
    u64 ed = ib + erva; u32 nnames = LD32(ed + 24);
    u64 funcs = ib + LD32(ed + 28), names = ib + LD32(ed + 32), ords = ib + LD32(ed + 36);
    const char *best = NULL; u64 best_rva = 0, rva = addr - ib;
    for (u32 i = 0; i < nnames; i++) {
        u32 f = LD32(funcs + 4ull * LD16(ords + 2ull * i));
        if (f <= rva && f >= best_rva) { best_rva = f; best = (const char *)(ib + LD32(names + 4ull * i)); }
    }
    if (best) *off = rva - best_rva;
    return best;
}
static int follows_call(u64 ra) {   // is the instruction before `ra` a call? (E8 rel32, FF /2 with common encodings)
    u64 isz, ib = rt_module_base_for(ra, &isz);
    if (!ib || ra < ib + 0x1000 + 7) return 0;
    if (LD8(ra - 5) == 0xE8) return 1;
    for (int n = 2; n <= 7; n++) if (LD8(ra - n) == 0xFF && ((LD8(ra - n + 1) >> 3) & 7) == 2) return 1;
    if (LD8(ra - 3) == 0x41 && LD8(ra - 2) == 0xFF) return 1;
    return 0;
}
static void print_guest_addr(const char *what, u64 a) {
    u64 isz, ib = rt_module_base_for(a, &isz);
    if (!ib) { fprintf(stderr, "  %s 0x%llx\n", what, (unsigned long long)a); return; }
    Module *m = mod_for_addr(a); u64 off = 0;
    const char *ex = nearest_export(ib, a, &off);
    fprintf(stderr, "  %s 0x%llx  %s+0x%llx (preferred 0x%llx)", what, (unsigned long long)a, m->img->name,
            (unsigned long long)(a - ib), (unsigned long long)(a - ib + m->img->orig_base));
    if (ex && off < 0x10000) fprintf(stderr, "  [%s+0x%llx]", ex, (unsigned long long)off);
    fputc('\n', stderr);
}
// Print the return addresses found on the guest stack (innermost first): a cheap backtrace for diagnostics.
void rt_guest_stack(Ctx *c) {
    fprintf(stderr, "[rt] guest call stack (return addresses found on the guest stack, innermost first):\n");
    u64 lo = LD64(c->gs_base + 16), hi = LD64(c->gs_base + 8), sp = c->r[RSP] & ~7ull;
    int n = 0;
    for (u64 p = sp; p + 8 <= hi && p >= lo && n < 24; p += 8) {
        u64 v = LDS64(p);
        if (follows_call(v)) { print_guest_addr("ret", v); n++; }
    }
}
int rt_jit_handle_fault(int sig, siginfo_t *si, void *uc);
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    if (rt_jit_handle_fault(sig, si, uc)) return;   // write to a page holding JIT translations (runtime/jit.c)
    fprintf(stderr, "[rt] %s at host address %p\n", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", si->si_addr);
    Ctx *c = rt_cur_ctx;
    if (c) {
        dump_ctx(c);
        rt_guest_stack(c);
    }
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void install_crash_handler(void) {
    static u8 altstack[64 * 1024];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
}

int main(int argc, char **argv) {
    const char *exe = getenv("WINTOMAC_EXE");
    if (!exe) {
        if (argc < 2) { fprintf(stderr, "usage: %s <original.exe> [args...]\n", argv[0]); return 2; }
        exe = argv[1]; argc--; argv++;
    }
    rt_argc = argc; rt_argv = argv;
    rt_trace_enabled();   // reads WINTOMAC_TRACE once (call_thunk tests the cached flag)
    if (!getenv("WINTOMAC_NO_CRASH_HANDLER")) install_crash_handler();
    setenv("VKD3D_DEBUG", "err", 0); setenv("VKD3D_SHADER_DEBUG", "err", 0);  // quiet vkd3d FIXMEs unless asked
    snprintf(exe_dir, sizeof exe_dir, "%s", exe);
    { char *sl = strrchr(exe_dir, '/'); if (sl) *sl = 0; else strcpy(exe_dir, "."); }
    { char real[1024]; if (realpath(exe_dir, real)) snprintf(exe_dir, sizeof exe_dir, "%s", real); }
    rt_files_init(exe_dir);

    thunk_base = (u64)mmap(NULL, 16ull * THUNK_SLOTS, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    for (u64 i = 0; i < rt_nimages; i++) {
        mods[i].img = &rt_images[i];
        char path[1024];
        if (!find_file(&rt_images[i], (int)i, exe, path, sizeof path)) {
            rt_log("cannot find %s (put it next to the exe or set WINTOMAC_DLLPATH)", rt_images[i].name);
            exit(2);
        }
        map_image(&mods[i], path);
    }
    for (u64 i = 0; i < rt_nimages; i++) bind_imports(&mods[i]);

    static Ctx ctx;  // main thread context
    Ctx *c = main_ctx = &ctx;
    rt_cur_ctx = c;
    u8 *stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    u64 top = (u64)stack + STACK_SIZE;
    setup_teb(c, (u64)stack, top);
    c->mxcsr = 0x1f80; c->fcw = 0x27f;
    c->r[RSP] = top - 0x100;           // 16-aligned

    if (!init_module(&mods[0])) return 1;   // DLLs the exe imports (transitively)
    const RtImage *exe_img = &rt_images[0];
    for (u64 i = 0; i < exe_img->ntls; i++) {
        u64 args[3] = { exe_img->base, 1 /* DLL_PROCESS_ATTACH */, 0 };
        rt_guest_call(c, exe_img->tls[i], 3, args);
    }
    u64 args[1] = { rt_peb };
    u64 r = rt_guest_call(c, exe_img->entry, 1, args);
    return (int)r;
}
