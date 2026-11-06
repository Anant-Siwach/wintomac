// kernel32.dll shims (growing as games need them).
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#include "win32.h"

#define STD_INPUT_HANDLE  ((u32)-10)
#define STD_OUTPUT_HANDLE ((u32)-11)
#define STD_ERROR_HANDLE  ((u32)-12)

static u64 std_handle[3];
static __thread u32 last_error;
void rt_set_last_error(u32 e) { last_error = e; }

static u64 std_handle_for(u32 which) {
    int idx = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : which == STD_ERROR_HANDLE ? 2 : -1;
    if (idx < 0) return (u64)-1;
    if (!std_handle[idx]) std_handle[idx] = rt_handle_new(H_FILE, idx);
    return std_handle[idx];
}

SHIM(GetStdHandle) { RET(std_handle_for((u32)A1)); }

// OVERLAPPED { u64 Internal (status); u64 InternalHigh (bytes); u32 Offset, OffsetHigh; HANDLE hEvent }.
// Overlapped requests are performed immediately at the given offset (Windows may complete them synchronously,
// and then reports TRUE); the OVERLAPPED is filled in and its event signalled as on completion.
static int file_io(u64 h, u64 buf, u32 n, u64 done, u64 ov, int writing) {
    intptr_t fd;
    if (!rt_handle_get(h, H_FILE, &fd)) { last_error = 6; return 0; }   // ERROR_INVALID_HANDLE
    ssize_t r;
    if (ov) {
        off_t off = (off_t)((u64)LD32(ov + 20) << 32 | LD32(ov + 16));
        r = writing ? pwrite((int)fd, (void *)buf, n, off) : pread((int)fd, (void *)buf, n, off);
        ST64(ov, r < 0 ? 0xC0000185ull /* STATUS_IO_DEVICE_ERROR */ : 0); ST64(ov + 8, r < 0 ? 0 : (u64)r);
        if (LD64(ov + 24)) rt_signal_event(LD64(ov + 24) & ~1ull);
        if (!writing && r == 0 && n) { ST64(ov, 0xC0000011ull); last_error = 38; if (done) ST32(done, 0); return 0; }  // ERROR_HANDLE_EOF
    } else {
        r = writing ? write((int)fd, (void *)buf, n) : read((int)fd, (void *)buf, n);
    }
    if (done) ST32(done, r < 0 ? 0 : (u32)r);
    if (r < 0) { last_error = 30; return 0; }   // ERROR_READ_FAULT
    return 1;
}
SHIM(WriteFile) { RET(file_io(A1, A2, (u32)A3, A4, AN(5), 1)); }   // (h, buf, n, *written, overlapped)
SHIM(ReadFile) { RET(file_io(A1, A2, (u32)A3, A4, AN(5), 0)); }
SHIM(GetOverlappedResult) {  // (h, ov, *bytes, wait)
    u64 st = LD64(A2);
    if (A3) ST32(A3, (u32)LD64(A2 + 8));
    if (st == 0xC0000011ull) { last_error = 38; RET(0); }
    if (st) { last_error = 30; RET(0); }
    RET(1);
}
SHIM(GetOverlappedResultEx) { sh_GetOverlappedResult(c); }
SHIM(CancelIo) { RET(1); }        // nothing is ever pending
SHIM(CancelIoEx) { last_error = 1168; RET(0); }   // ERROR_NOT_FOUND: no pending request

SHIM(CloseHandle) { rt_handle_close(A1); RET(1); }
void rt_jit_dump_stats(void);
SHIM(ExitProcess) { rt_jit_dump_stats(); fflush(NULL); _exit((int)(u32)A1); }
SHIM(GetLastError) { RET(last_error); }
SHIM(SetLastError) { last_error = (u32)A1; RET(0); }
SHIM(GetCurrentProcessId) { RET(getpid()); }
SHIM(GetCurrentThreadId) { RET(LD32(c->gs_base + 0x48)); }
SHIM(GetCurrentProcess) { RET((u64)-1); }
SHIM(GetCurrentThread) { RET((u64)-2); }

// Modules: the exe is the only real image. Any named DLL gets a pseudo module handle so that
// GetProcAddress(GetModuleHandle("kernel32"), "Foo") can resolve to our shim for Foo.
#define PSEUDO_MODULE 0x7ffe00000000ull
static u64 wide_to_ascii(u64 w, char *out, size_t n) {
    size_t i = 0; for (; i + 1 < n && LD16(w + 2 * i); i++) out[i] = (char)LD16(w + 2 * i); out[i] = 0; return i;
}
int rt_module_is_recompiled(const char *name);
static u64 mod_handle(const char *name) {   // GetModuleHandle: a recompiled DLL nobody has loaded yet is not there
    u64 h = rt_module_handle(name);
    if (h) return h;
    if (rt_module_is_recompiled(name)) { last_error = 126; return 0; }   // ERROR_MOD_NOT_FOUND
    return PSEUDO_MODULE;
}
SHIM(GetModuleHandleA) { RET(A1 ? mod_handle((const char *)A1) : rt_image_base); }
SHIM(GetModuleHandleW) { if (!A1) RET(rt_image_base); char b[260]; wide_to_ascii(A1, b, sizeof b); RET(mod_handle(b)); }
SHIM(GetModuleHandleExW) {  // (flags, name-or-address, *module)
    u64 h;
    if (A1 & 4) h = rt_module_base_for(A2, NULL);                     // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
    else if (!A2) h = rt_image_base;
    else { char b[260]; wide_to_ascii(A2, b, sizeof b); h = mod_handle(b); }
    if (A3) ST64(A3, h);
    RET(h != 0);
}
SHIM(GetModuleHandleExA) {
    u64 h = (A1 & 4) ? rt_module_base_for(A2, NULL) : A2 ? mod_handle((const char *)A2) : rt_image_base;
    if (A3) ST64(A3, h); RET(h != 0);
}
static u64 load_library(const char *name) {
    u64 h = rt_module_load(name);
    if (rt_trace_enabled()) rt_log("LoadLibrary(%s) -> %s", name, h ? "recompiled module" : "shim pseudo-module");
    // A PS5 system library (AnyPS5 libs/*.prx, docs/ps5.md) that was not recompiled with the program: fail like a
    // missing DLL. Answering with the shim pseudo-module would bind PS5 imports to our Win64 CRT shims.
    size_t n = strlen(name);
    if (!h && n > 4 && !strcasecmp(name + n - 4, ".prx")) {
        rt_log("LoadLibrary(%s): not recompiled with this program; add it to the recompile command (tools/ps5_deps.py)", name);
        last_error = 126; return 0;   // ERROR_MOD_NOT_FOUND
    }
    return h ? h : PSEUDO_MODULE;
}
SHIM(LoadLibraryA) { RET(load_library((const char *)A1)); }
SHIM(LoadLibraryW) { char b[260]; wide_to_ascii(A1, b, sizeof b); RET(load_library(b)); }
SHIM(LoadLibraryExA) { sh_LoadLibraryA(c); }
SHIM(LoadLibraryExW) { sh_LoadLibraryW(c); }
SHIM(FreeLibrary) { RET(1); }
SHIM(DisableThreadLibraryCalls) { RET(1); }
SHIM(GetProcAddress) {
    u64 mod = A1, t = 0;
    const char *name = A2 >= 0x10000 ? (const char *)A2 : NULL;
    int ours = mod != PSEUDO_MODULE && rt_module_path(mod);
    if (ours) t = rt_module_export(mod, name, name ? 0 : (u16)A2);
    // A recompiled DLL exports only what it exports: AnyPS5's loader probes each library for a name and then for
    // its NID, so answering "puts" from libSceLibcInternal.prx with our Win64 CRT shim would be wrong.
    if (!t && name && mod && !(ours && mod != rt_image_base)) t = rt_thunk_for(NULL, name);   // NULL module: no such DLL
    if (rt_trace_enabled() || (!t && getenv("WINTOMAC_VERBOSE"))) rt_log("GetProcAddress(%s) = %llx", name ? name : "(ordinal)", (unsigned long long)t);
    if (!t) last_error = 127;  // ERROR_PROC_NOT_FOUND
    RET(t);
}
// Guest-visible paths for our modules: C:\wintomac\<file>. (File APIs will translate these back.)
static size_t module_path(u64 mod, char *out, size_t n) {
    const char *p = rt_module_path(mod ? mod : rt_image_base);
    const char *b = p ? strrchr(p, '/') : NULL;
    return (size_t)snprintf(out, n, "C:\\wintomac\\%s", b ? b + 1 : (p ? p : "game.exe"));
}
SHIM(GetModuleFileNameA) {  // (hmod, buf, size)
    char p[1100]; size_t n = module_path(A1, p, sizeof p), sz = (u32)A3;
    if (!sz) RET(0); if (n >= sz) n = sz - 1; memcpy((void *)A2, p, n); ((char *)A2)[n] = 0; RET(n);
}
SHIM(GetModuleFileNameW) {
    char p[1100]; size_t n = module_path(A1, p, sizeof p), sz = (u32)A3;
    if (!sz) RET(0); if (n >= sz) n = sz - 1; for (size_t i = 0; i < n; i++) ST16(A2 + 2 * i, (u8)p[i]); ST16(A2 + 2 * n, 0); RET(n);
}

void gfx_main_service(void);   // ps5_gfx.m: main-thread window work queued by other threads
SHIM(Sleep) { gfx_main_service(); u32 ms = (u32)A1; if (ms == 0) sched_yield(); else usleep(ms * 1000u); RET(0); }

SHIM(QueryPerformanceCounter) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    u64 ns = mach_absolute_time() * tb.numer / tb.denom;
    ST64(A1, ns / 100);  // 10 MHz, like modern Windows
    RET(1);
}
SHIM(QueryPerformanceFrequency) { ST64(A1, 10000000); RET(1); }

SHIM(GetTickCount) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    RET((u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
}
SHIM(GetTickCount64) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    RET((u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

SHIM(GetSystemTimeAsFileTime) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    u64 ft = ((u64)ts.tv_sec + 11644473600ull) * 10000000ull + ts.tv_nsec / 100;
    ST64(A1, ft); RET(0);
}

// Heap: identity-mapped address space means host malloc is directly usable by the guest.
SHIM(GetProcessHeap) { RET(0x7e57); }
// WINTOMAC_HEAP_TRACE=1: log every heap call with the guest caller to wintomac_heap.txt (A = HeapAlloc, F = HeapFree,
// R = HeapReAlloc, L/G = Local/GlobalAlloc, l/g = Local/GlobalFree, f = the CRT free shim in ucrt.c). The variable
// is read once at load time; when tracing is off every shim pays one well-predicted branch on a global.
FILE *rt_heap_trace_f;
__attribute__((constructor)) static void heap_trace_init(void) {
    const char *e = getenv("WINTOMAC_HEAP_TRACE");
    if (e && *e && *e != '0' && (rt_heap_trace_f = fopen("wintomac_heap.txt", "w"))) setvbuf(rt_heap_trace_f, NULL, _IOLBF, 0);
}
#define HTRACE(...) do { if (__builtin_expect(rt_heap_trace_f != NULL, 0)) fprintf(rt_heap_trace_f, __VA_ARGS__); } while (0)
SHIM(HeapAlloc) {
    size_t n = A3 ? A3 : 1;
    void *p = (A2 & 8) ? calloc(1, n) : malloc(n);  // HEAP_ZERO_MEMORY
    HTRACE("A %p %zu %llx\n", p, n, (unsigned long long)LD64(c->r[RSP]));
    RET(p);
}
// Freeing something this heap never handed out (a double free, a static buffer, VirtualAlloc memory) makes
// macOS's malloc abort the process; Windows' HeapFree just fails. Check ownership first and log the culprit.
static int heap_owns(const void *p, const char *what, Ctx *c) {
    if (!p || malloc_size(p)) return 1;
    static int n;
    if (n++ < 10) rt_log("%s(%p): not a heap block (double free or foreign memory); caller ret %llx", what, p,
                         (unsigned long long)LD64(c->r[RSP]));
    if (n == 1) {
        rt_guest_stack(c);
        if (getenv("MallocStackLogging")) {   // who allocated / freed it on the host side (malloc_history needs MallocStackLogging=1)
            char cmd[96]; snprintf(cmd, sizeof cmd, "malloc_history %d %p 1>&2", getpid(), p); system(cmd);
        }
    }
    rt_set_last_error(87);   // ERROR_INVALID_PARAMETER
    return 0;
}
SHIM(HeapFree) {
    HTRACE("F %p %llx\n", (void *)A3, (unsigned long long)LD64(c->r[RSP]));
    if (!heap_owns((void *)A3, "HeapFree", c)) RET(0);
    free((void *)A3); RET(1);
}
SHIM(HeapReAlloc) {
    if (!heap_owns((void *)A3, "HeapReAlloc", c)) RET(0);
    if (A2 & 0x10) RET(malloc_size((void *)A3) >= A4 ? A3 : 0);   // HEAP_REALLOC_IN_PLACE_ONLY
    size_t old = malloc_size((void *)A3);
    void *q = realloc((void *)A3, A4 ? A4 : 1);
    if (q && (A2 & 8) && A4 > old) memset((char *)q + old, 0, A4 - old);   // HEAP_ZERO_MEMORY: zero the growth
    HTRACE("R %p -> %p %llu %llx\n", (void *)A3, q, (unsigned long long)A4, (unsigned long long)LD64(c->r[RSP]));
    RET(q);
}

SHIM(IsDebuggerPresent) { RET(0); }
SHIM(IsProcessorFeaturePresent) {
    switch ((u32)A1) { case 6: case 10: case 13: case 17: RET(1); default: RET(0); }  // SSE, SSE2, SSE3, NX
}

// CRITICAL_SECTION (40 bytes): we keep a host recursive mutex pointer in DebugInfo (offset 0).
static pthread_mutex_t *cs_mutex(u64 cs) {
    pthread_mutex_t *m = (pthread_mutex_t *)LD64(cs);
    if (m) return m;
    static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&init_lock);
    m = (pthread_mutex_t *)LD64(cs);
    if (!m) {
        m = malloc(sizeof *m);
        pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
        ST64(cs, (u64)m);
    }
    pthread_mutex_unlock(&init_lock);
    return m;
}
pthread_mutex_t *rt_cs_mutex(u64 cs) { return cs_mutex(cs); }   // for SleepConditionVariableCS
SHIM(InitializeCriticalSection) { memset((void *)A1, 0, 40); cs_mutex(A1); RET(0); }
SHIM(InitializeCriticalSectionAndSpinCount) { memset((void *)A1, 0, 40); cs_mutex(A1); RET(1); }
SHIM(InitializeCriticalSectionEx) { memset((void *)A1, 0, 40); cs_mutex(A1); RET(1); }
SHIM(EnterCriticalSection) { pthread_mutex_lock(cs_mutex(A1)); RET(0); }
SHIM(TryEnterCriticalSection) { RET(pthread_mutex_trylock(cs_mutex(A1)) == 0); }
SHIM(LeaveCriticalSection) { pthread_mutex_unlock(cs_mutex(A1)); RET(0); }
SHIM(DeleteCriticalSection) {
    pthread_mutex_t *m = (pthread_mutex_t *)LD64(A1);
    if (m) { pthread_mutex_destroy(m); free(m); ST64(A1, 0); }
    RET(0);
}

// TLS: TEB.TlsSlots[64] at +0x1480.
static u64 tls_used;
SHIM(TlsAlloc) { for (int i = 0; i < 64; i++) if (!(tls_used >> i & 1)) { tls_used |= 1ull << i; RET(i); } RET(0xFFFFFFFF); }
SHIM(TlsFree) { u32 i = (u32)A1; if (i < 64) tls_used &= ~(1ull << i); RET(1); }
SHIM(TlsGetValue) { u32 i = (u32)A1; last_error = 0; RET(i < 64 ? LD64(c->gs_base + 0x1480 + 8 * i) : 0); }
SHIM(TlsSetValue) { u32 i = (u32)A1; if (i >= 64) RET(0); ST64(c->gs_base + 0x1480 + 8 * i, A2); RET(1); }
SHIM(FlsAlloc) { sh_TlsAlloc(c); }
SHIM(FlsGetValue) { sh_TlsGetValue(c); }
SHIM(FlsSetValue) { sh_TlsSetValue(c); }
SHIM(FlsFree) { sh_TlsFree(c); }

// Memory: everything the guest sees is RW in our address space; report it that way.
// VirtualQuery / placeholder memory (VirtualAlloc2, MapViewOfFile3) live in ps5_vmem.c: exact region extents,
// MEM_FREE gaps, MEM_IMAGE for recompiled images.
void rt_vmem_query(u64 addr, u64 mbi, u64 imgbase, u64 imgsize, u64 resbase);
int rt_vmem_free(u64 addr, u64 size, u32 type, u64 *ret);
int rt_vmem_commit(u64 addr, u64 size, u32 type, u32 prot, u64 *ret);
int rt_vmem_protect(u64 addr, u64 size, u32 prot, u64 old_out);
SHIM(VirtualQuery) {  // (addr, MEMORY_BASIC_INFORMATION*, len)
    if ((u32)A3 < 48) { last_error = 24; RET(0); }   // ERROR_BAD_LENGTH
    u64 isz = 0, ib = rt_module_base_for(A1 & ~0xFFFull, &isz);
    rt_vmem_query(A1, A2, ib, isz, 0);
    RET(48);
}
// PAGE_EXECUTE* ranges are recorded so the interpreter (runtime/interp.c) runs code generated there.
void rt_interp_note_protect(u64 addr, u64 size, u32 prot, int is_alloc);
void rt_interp_unmark_exec(u64 addr, u64 size);
SHIM(VirtualProtect) { if (rt_vmem_protect(A1, A2, (u32)A3, A4)) RET(1); if (A4) ST32(A4, 4); rt_interp_note_protect(A1, A2, (u32)A3, 0); RET(1); }  // (addr, size, new, *old)
// VirtualAlloc reservations, so that VirtualFree(MEM_RELEASE, size 0) unmaps exactly what was reserved and
// nothing else. Windows semantics that matter here: a reservation at a requested address succeeds there or not
// at all (ERROR_INVALID_ADDRESS), and releasing anything that is not a reservation's base fails. The old shim
// returned whatever mmap picked for a busy hint and munmap()ed 64 KB at any address it was given, so a stale or
// foreign VirtualFree could unmap pages macOS malloc had since put blocks in.
typedef struct { u64 base, size; } VRes;
static VRes *vres; static int nvres, vres_cap;
static pthread_mutex_t vres_lock = PTHREAD_MUTEX_INITIALIZER;
static void vres_add(u64 base, u64 size) {
    pthread_mutex_lock(&vres_lock);
    if (nvres == vres_cap) { int nc = vres_cap ? 2 * vres_cap : 64; VRes *n = realloc(vres, nc * sizeof *n); if (n) { vres = n; vres_cap = nc; } }
    if (nvres < vres_cap) vres[nvres++] = (VRes){ base, size };
    pthread_mutex_unlock(&vres_lock);
}
static int vres_find(u64 a, VRes *out, int take) {   // the reservation containing a (take: and remove it; a must be its base)
    int r = 0;
    pthread_mutex_lock(&vres_lock);
    for (int i = 0; i < nvres; i++)
        if (take ? vres[i].base == a : (a >= vres[i].base && a < vres[i].base + vres[i].size)) {
            if (out) *out = vres[i];
            if (take) vres[i] = vres[--nvres];
            r = 1; break;
        }
    pthread_mutex_unlock(&vres_lock);
    return r;
}
SHIM(VirtualAlloc) {  // (addr, size, type, protect)
    { u64 r; if (rt_vmem_commit(A1, A2, (u32)A3, (u32)A4, &r)) RET(r); }   // inside a ps5_vmem.c reservation
    u64 addr = A1, type = (u32)A3;
    if (addr && !(type & 0x2000)) {   // MEM_COMMIT / MEM_RESET inside a reserved range: pages are already RW
        rt_interp_note_protect(addr, A2, (u32)A4, 1);
        RET(addr);
    }
    u64 base = addr & ~0xFFFFull;                                  // reservations are 64 KB granular
    u64 size = (addr + A2 - base + 0xFFFF) & ~0xFFFFull;
    if (!size) { last_error = 87; RET(0); }
    void *p;
    if (base) {
        // Exactly there or not at all. mach_vm_allocate(VM_FLAGS_FIXED) without VM_FLAGS_OVERWRITE is atomic and never
        // replaces an existing mapping (unlike MAP_FIXED), so a busy range fails instead of landing elsewhere or
        // clobbering host memory.
        mach_vm_address_t a = base;
        if (mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) != KERN_SUCCESS) { last_error = 487; RET(0); }   // ERROR_INVALID_ADDRESS
        p = (void *)a;
    } else {
        // Windows reservation bases are 64 KB aligned (the allocation granularity); mmap only aligns to the 16 KB
        // page. Unaligned bases broke "release, then reserve at the same address" (rounded down onto a neighbour).
        u8 *m = mmap(NULL, size + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) { last_error = 8; RET(0); }           // ERROR_NOT_ENOUGH_MEMORY
        u8 *al = (u8 *)(((u64)m + 0xFFFF) & ~0xFFFFull);
        if (al > m) munmap(m, al - m);
        if (al + size < m + size + 0x10000) munmap(al + size, m + size + 0x10000 - (al + size));
        p = al;
    }
    vres_add((u64)p, size);
    rt_interp_note_protect((u64)p, size, (u32)A4, 1);
    RET(base ? addr : (u64)p);
}
SHIM(VirtualFree) {  // (addr, size, type)
    { u64 r; if (rt_vmem_free(A1, A2, (u32)A3, &r)) RET(r); }   // placeholders / VirtualAlloc2 ranges (ps5_vmem.c)
    u32 type = (u32)A3; VRes r;
    if (type & 0x8000) {   // MEM_RELEASE: addr must be a reservation base and size 0
        if (A2) { last_error = 87; RET(0); }                    // ERROR_INVALID_PARAMETER
        if (!vres_find(A1, &r, 1)) { last_error = 487; RET(0); }   // ERROR_INVALID_ADDRESS: not a base / already released
        rt_interp_unmark_exec(r.base, r.size);
        munmap((void *)r.base, r.size);
        RET(1);
    }
    if (type & 0x4000) {   // MEM_DECOMMIT: the pages read as zero when committed again
        if (!vres_find(A1, &r, 0)) { last_error = 487; RET(0); }
        u64 lo = A1 & ~0xFFFull, hi = A2 ? (A1 + A2 + 0xFFF) & ~0xFFFull : r.base + r.size;
        if (hi > r.base + r.size) hi = r.base + r.size;
        rt_interp_unmark_exec(lo, hi - lo);
        // whole host pages (16 KB) get fresh zero pages (MAP_FIXED is safe: the range is our own reservation);
        // the 4 KB-granular edges are cleared by hand
        u64 hp = (u64)getpagesize(), flo = (lo + hp - 1) & ~(hp - 1), fhi = hi & ~(hp - 1);
        if (flo < fhi) {
            mmap((void *)flo, fhi - flo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
            memset((void *)lo, 0, flo - lo); memset((void *)fhi, 0, hi - fhi);
        } else memset((void *)lo, 0, hi - lo);
        RET(1);
    }
    last_error = 87; RET(0);
}

SHIM(OutputDebugStringA) { fprintf(stderr, "[guest-debug] %s", (const char *)A1); RET(0); }
SHIM(OutputDebugStringW) { char b[1024]; wide_to_ascii(A1, b, sizeof b); fprintf(stderr, "[guest-debug] %s", b); RET(0); }
SHIM(FormatMessageA) { if (A1 & 0x100) RET(0); if ((u32)AN(6)) { snprintf((char *)AN(5), (u32)AN(6), "error %u\r\n", (u32)A3); RET(strlen((char *)AN(5))); } RET(0); }
static void *lg_alloc(Ctx *c, char tag) {   // LocalAlloc / GlobalAlloc (flags, size): LMEM_ZEROINIT/GMEM_ZEROINIT = 0x40
    void *p = (A1 & 0x40) ? calloc(1, A2 ? A2 : 1) : malloc(A2 ? A2 : 1);
    HTRACE("%c %p %llu %llx\n", tag, p, (unsigned long long)A2, (unsigned long long)LD64(c->r[RSP]));
    return p;
}
static u64 lg_free(Ctx *c, const char *what, char tag) {
    HTRACE("%c %p %llx\n", tag, (void *)A1, (unsigned long long)LD64(c->r[RSP]));
    if (!heap_owns((void *)A1, what, c)) return A1;
    free((void *)A1); return 0;
}
SHIM(LocalFree) { RET(lg_free(c, "LocalFree", 'l')); }
SHIM(LocalAlloc) { RET(lg_alloc(c, 'L')); }
SHIM(GlobalAlloc) { RET(lg_alloc(c, 'G')); }
SHIM(GlobalFree) { RET(lg_free(c, "GlobalFree", 'g')); }
SHIM(OpenProcess) { RET(0); }
SHIM(GetSystemInfo) {  // SYSTEM_INFO (48 bytes)
    u64 p = A1; memset((void *)p, 0, 48);
    ST16(p, 9 /* AMD64 */); ST32(p + 4, 0x1000); ST64(p + 8, 0x10000); ST64(p + 16, 0x7FFFFFFEFFFFull);
    ST64(p + 24, 0xFF); ST32(p + 32, (u32)sysconf(_SC_NPROCESSORS_ONLN)); ST32(p + 36, 8664); ST32(p + 40, 0x10000);
    ST16(p + 44, 6); RET(0);
}
SHIM(GetNativeSystemInfo) { sh_GetSystemInfo(c); }
SHIM(GetVersion) { RET(0x23F00206); }  // 6.2 build 9200 (what Windows reports to unmanifested apps)
SHIM(GetEnvironmentVariableA) {
    const char *v = getenv((const char *)A1); if (!v) { last_error = 203; RET(0); }
    size_t n = strlen(v); if (n + 1 > (u32)A3) RET(n + 1); memcpy((void *)A2, v, n + 1); RET(n);
}
static char *cmdline_a(void) {   // "game.exe" arg1 arg2 ... (quoted where needed)
    static char *cl; if (cl) return cl;
    size_t n = 16; for (int i = 1; i < rt_argc; i++) n += strlen(rt_argv[i]) + 3;
    cl = calloc(1, n); strcpy(cl, "game.exe");
    for (int i = 1; i < rt_argc; i++) { strcat(cl, " "); int q = strchr(rt_argv[i], ' ') != NULL; if (q) strcat(cl, "\""); strcat(cl, rt_argv[i]); if (q) strcat(cl, "\""); }
    return cl;
}
SHIM(GetCommandLineA) { RET(cmdline_a()); }
char *rt_cmdline_a(void) { return cmdline_a(); }   // for the CRT's _acmdln
SHIM(GetCommandLineW) {
    static u16 *w; if (!w) { const char *a = cmdline_a(); size_t n = strlen(a); w = calloc(n + 1, 2); for (size_t i = 0; i < n; i++) w[i] = (u8)a[i]; }
    RET(w);
}
SHIM(GetTimeZoneInformation) {  // TIME_ZONE_INFORMATION (172 bytes): only the bias is meaningful here
    memset((void *)A1, 0, 172);
    time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
    ST32(A1, (u32)(i32)(-lt.tm_gmtoff / 60));
    RET(0);  // TIME_ZONE_ID_UNKNOWN
}
SHIM(GetACP) { RET(65001); }
SHIM(GetOEMCP) { RET(437); }
SHIM(GetConsoleMode) { if (A2) ST32(A2, 3); RET(1); }
SHIM(SetConsoleCtrlHandler) { RET(1); }
SHIM(GetFileType) { intptr_t fd; RET(rt_handle_get(A1, H_FILE, &fd) ? (isatty((int)fd) ? 2 : 1) : 0); }
SHIM(SetErrorMode) { RET(0); }
SHIM(EncodePointer) { RET(A1); }
SHIM(DecodePointer) { RET(A1); }
SHIM(InitializeSListHead) { memset((void *)A1, 0, 16); RET(0); }
SHIM(QueryUnbiasedInterruptTime) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); ST64(A1, (u64)ts.tv_sec * 10000000 + ts.tv_nsec / 100); RET(1);
}

const Shim shims_kernel32[] = {
#define E(n) { NULL, #n, sh_##n }  /* kernel32 / kernelbase / api-ms-win-core-* */
    E(GetStdHandle), E(WriteFile), E(ReadFile), E(GetOverlappedResult), E(GetOverlappedResultEx), E(CancelIo), E(CancelIoEx), E(CloseHandle), E(ExitProcess),
    E(GetLastError), E(SetLastError), E(GetCurrentProcessId), E(GetCurrentThreadId),
    E(GetCurrentProcess), E(GetCurrentThread), E(GetModuleHandleA), E(GetModuleHandleW),
    E(Sleep), E(QueryPerformanceCounter), E(QueryPerformanceFrequency), E(GetTickCount),
    E(GetTickCount64), E(GetSystemTimeAsFileTime), E(GetProcessHeap), E(HeapAlloc), E(HeapFree),
    E(HeapReAlloc), E(IsDebuggerPresent), E(IsProcessorFeaturePresent),
    E(InitializeCriticalSection), E(InitializeCriticalSectionAndSpinCount), E(InitializeCriticalSectionEx),
    E(EnterCriticalSection), E(TryEnterCriticalSection), E(LeaveCriticalSection), E(DeleteCriticalSection),
    E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue), E(FlsAlloc), E(FlsGetValue), E(FlsSetValue), E(FlsFree),
    E(VirtualQuery), E(VirtualProtect), E(VirtualAlloc), E(VirtualFree),
    E(GetModuleHandleExW), E(GetModuleHandleExA), E(DisableThreadLibraryCalls), E(LoadLibraryA), E(LoadLibraryW), E(LoadLibraryExA), E(LoadLibraryExW), E(FreeLibrary),
    E(GetProcAddress), E(GetModuleFileNameA), E(GetModuleFileNameW), E(OutputDebugStringA), E(OutputDebugStringW),
    E(FormatMessageA), E(LocalFree), E(LocalAlloc), E(GlobalAlloc), E(GlobalFree), E(OpenProcess),
    E(GetSystemInfo),
    E(GetNativeSystemInfo), E(GetVersion), E(GetEnvironmentVariableA), E(GetCommandLineA), E(GetCommandLineW),
    E(GetTimeZoneInformation), E(GetACP), E(GetOEMCP), E(GetConsoleMode), E(SetConsoleCtrlHandler), E(GetFileType), E(SetErrorMode),
    E(EncodePointer), E(DecodePointer), E(InitializeSListHead), E(QueryUnbiasedInterruptTime),
#undef E
    { 0, 0, 0 }
};
