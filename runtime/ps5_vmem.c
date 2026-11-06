// Windows 10 placeholder virtual memory (VirtualAlloc2 / MapViewOfFile3 / UnmapViewOfFile2, MEM_RESERVE_PLACEHOLDER,
// MEM_REPLACE_PLACEHOLDER, MEM_PRESERVE_PLACEHOLDER, MEM_COALESCE_PLACEHOLDERS) and an address-space-accurate
// VirtualQuery. AnyPS5's libc (third_party/anyps5-win, GuestArena/WindowsMappings) builds the PS5 guest address space
// out of placeholders and maps shared "direct memory" sections into them, possibly at several addresses at once.
//
// Model: every range created here is a list of regions {base, size, allocation base, state, type, protection,
// placeholder?}, exactly the attributes VirtualQuery reports. Host side, a placeholder or reserved range is an
// inaccessible anonymous mapping, committed private memory a fresh read-write anonymous mapping, and a view of a
// section is the section's file mapped MAP_SHARED|MAP_FIXED, or, for pagefile-backed sections (one shared anonymous
// block, kernel32x.c), the block re-mapped shared with mach_vm_remap, so all views alias the same pages.
// Semantics follow the Microsoft Learn documentation of VirtualAlloc2, MapViewOfFile3, UnmapViewOfFile2,
// VirtualFree and VirtualQuery. No Wine or Microsoft code copied.
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "winstr.h"

enum { MEM_COMMIT_ = 0x1000, MEM_RESERVE_ = 0x2000, MEM_DECOMMIT_ = 0x4000, MEM_RELEASE_ = 0x8000, MEM_FREE_ = 0x10000,
       MEM_PRIVATE_ = 0x20000, MEM_MAPPED_ = 0x40000, MEM_IMAGE_ = 0x1000000,
       MEM_REPLACE_PLACEHOLDER_ = 0x4000, MEM_RESERVE_PLACEHOLDER_ = 0x40000,
       MEM_COALESCE_PLACEHOLDERS_ = 0x1, MEM_PRESERVE_PLACEHOLDER_ = 0x2,
       PAGE_NOACCESS_ = 1, PAGE_READWRITE_ = 4 };
enum { ERR_ACCESS = 5, ERR_HANDLE = 6, ERR_NOMEM = 8, ERR_PARAM = 87, ERR_INVALID_ADDRESS = 487 };

typedef struct { u64 base, size, abase; u32 state, type, prot, aprot; int ph; } Region;
static Region *rg; static int nrg, cap;
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;
static int vm_used;
static int vm_log(void) { static int on = -1; if (on < 0) on = getenv("WINTOMAC_VMEM_LOG") != NULL; return on; }   // any placeholder API called (VirtualQuery takes the exact path regardless)

int rt_mapping_info(u64 h, int *fd, u8 **anon, u64 *size);   // kernel32x.c: section behind a file-mapping handle

static int find(u64 a) {   // region containing a, or -1
    int lo = 0, hi = nrg - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (a < rg[m].base) hi = m - 1; else if (a >= rg[m].base + rg[m].size) lo = m + 1; else return m; }
    return -1;
}
static int lower(u64 a) { int lo = 0, hi = nrg; while (lo < hi) { int m = (lo + hi) / 2; if (rg[m].base < a) lo = m + 1; else hi = m; } return lo; }
static void insert(Region r) {
    if (nrg == cap) { cap = cap ? 2 * cap : 256; rg = realloc(rg, cap * sizeof *rg); }
    int i = lower(r.base);
    memmove(&rg[i + 1], &rg[i], (nrg - i) * sizeof *rg); rg[i] = r; nrg++;
}
static void erase(int i) { memmove(&rg[i], &rg[i + 1], (nrg - i - 1) * sizeof *rg); nrg--; }
// Split so that a region boundary falls at a (no-op if a is already one or outside every region).
static void cut(u64 a) {
    int i = find(a);
    if (i < 0 || rg[i].base == a) return;
    Region r = rg[i]; u64 left = a - r.base;
    rg[i].size = left; r.base = a; r.size -= left; insert(r);
}
static int covered(u64 a, u64 n) {   // [a, a+n) entirely made of our regions?
    for (u64 p = a; p < a + n;) { int i = find(p); if (i < 0) return 0; p = rg[i].base + rg[i].size; }
    return 1;
}
static int host_prot(u32 p) {
    p &= 0xff;
    if (p == PAGE_NOACCESS_ || !p) return PROT_NONE;
    if (p == 2 || p == 0x20) return PROT_READ;     // READONLY / EXECUTE_READ
    if (p == 0x10) return PROT_READ;               // EXECUTE
    return PROT_READ | PROT_WRITE;                 // READWRITE / WRITECOPY / EXECUTE_READWRITE (no host W+X)
}
static int remap_none(u64 a, u64 n) {   // fresh inaccessible pages (placeholder / reserved)
    return mmap((void *)a, n, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (void *)a;
}
static int remap_zero(u64 a, u64 n, u32 prot) {   // fresh zero pages
    return mmap((void *)a, n, host_prot(prot), MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (void *)a;
}
// Reserve n bytes, at want exactly (0 = anywhere, honouring lo/hi/align from MEM_ADDRESS_REQUIREMENTS).
static u64 reserve_host(u64 want, u64 n, u64 lo, u64 hi, u64 align) {
    if (want) {
        void *p = mmap((void *)want, n, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return 0;
        if ((u64)p != want) { munmap(p, n); return 0; }
        return want;
    }
    if (align < 0x10000) align = 0x10000;
    u64 hint = lo ? (lo + align - 1) & ~(align - 1) : 0;
    void *p = mmap((void *)hint, n + align, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;
    u64 a = ((u64)p + align - 1) & ~(align - 1);
    if (a > (u64)p) munmap(p, a - (u64)p);
    if ((u64)p + n + align > a + n) munmap((void *)(a + n), (u64)p + n + align - (a + n));
    if (hi && a + n - 1 > hi) { munmap((void *)a, n); return 0; }
    return a;
}
static u64 rnd(u64 n) { u64 pg = (u64)getpagesize(); return (n + pg - 1) & ~(pg - 1); }

// ---- VirtualAlloc2(process, addr, size, type, protect, MEM_EXTENDED_PARAMETER *params, count) -------------------
SHIM(VirtualAlloc2) {
    u64 addr = A2, size = rnd(A3); u32 type = (u32)A4, prot = (u32)AN(5); u64 params = AN(6); u32 np = (u32)AN(7);
    u64 lo = 0, hi = 0, align = 0;
    for (u32 i = 0; i < np; i++) {   // MEM_EXTENDED_PARAMETER { u64 Type:8; u64 Reserved:56; union { u64; void *; } }
        u64 t = LD64(params + 16 * i) & 0xff, v = LD64(params + 16 * i + 8);
        if (t == 1 && v) { lo = LD64(v); hi = LD64(v + 8); align = LD64(v + 16); }   // MemExtendedParameterAddressRequirements
    }
    if (!size) { rt_set_last_error(ERR_PARAM); RET(0); }
    pthread_mutex_lock(&vm_lock); vm_used = 1;
    u64 r = 0;
    if (type & MEM_REPLACE_PLACEHOLDER_) {          // placeholder -> private memory, exactly the whole placeholder
        int i = find(addr);
        if (i < 0 || !rg[i].ph || rg[i].base != addr || rg[i].size != size) { rt_set_last_error(ERR_INVALID_ADDRESS); goto out; }
        int commit = (type & MEM_COMMIT_) != 0;
        if (!(commit ? remap_zero(addr, size, prot) : remap_none(addr, size))) { rt_set_last_error(ERR_NOMEM); goto out; }
        rg[i] = (Region){ addr, size, addr, commit ? MEM_COMMIT_ : MEM_RESERVE_, MEM_PRIVATE_, commit ? prot : 0, prot, 0 };
        r = addr;
    } else if (type & MEM_RESERVE_PLACEHOLDER_) {   // a new placeholder
        u64 a = reserve_host(addr, size, lo, hi, align);
        if (!a) { if (getenv("WINTOMAC_VMEM_LOG")) rt_log("vmem: placeholder %llx+%llx failed", (unsigned long long)addr, (unsigned long long)size);
                  rt_set_last_error(addr ? ERR_INVALID_ADDRESS : ERR_NOMEM); goto out; }
        insert((Region){ a, size, a, MEM_RESERVE_, MEM_PRIVATE_, 0, PAGE_NOACCESS_, 1 });
        r = a;
    } else if (addr && (type & MEM_COMMIT_) && covered(addr & ~0xfffull, size)) {   // commit inside our reservation
        u64 a = addr & ~(u64)(getpagesize() - 1), e = rnd(addr + A3);
        cut(a); cut(e);
        for (int i = find(a); i >= 0 && i < nrg && rg[i].base < e; i++) {
            if (rg[i].ph) { rt_set_last_error(ERR_INVALID_ADDRESS); goto out; }
            if (rg[i].state != MEM_COMMIT_) remap_zero(rg[i].base, rg[i].size, prot); else mprotect((void *)rg[i].base, rg[i].size, host_prot(prot));
            rg[i].state = MEM_COMMIT_; rg[i].prot = prot;
        }
        r = addr;
    } else {                                        // an ordinary reservation (and commit)
        u64 a = reserve_host(addr & ~0xffffull, size, lo, hi, align);
        if (!a) { rt_set_last_error(addr ? ERR_INVALID_ADDRESS : ERR_NOMEM); goto out; }
        int commit = (type & MEM_COMMIT_) != 0;
        if (commit) remap_zero(a, size, prot);
        insert((Region){ a, size, a, commit ? MEM_COMMIT_ : MEM_RESERVE_, MEM_PRIVATE_, commit ? prot : 0, prot, 0 });
        r = a;
    }
out:
    pthread_mutex_unlock(&vm_lock);
    if (vm_log()) rt_log("vmem: VirtualAlloc2(%llx, %llx, type %x, prot %x) = %llx", (unsigned long long)addr, (unsigned long long)A3, type, prot, (unsigned long long)r);
    RET(r);
}

// Map [off, off+n) of a section at a (MAP_FIXED over our own reservation).
static int map_section(u64 h, u64 a, u64 off, u64 n, u32 prot) {
    int fd; u8 *anon; u64 sz;
    if (!rt_mapping_info(h, &fd, &anon, &sz)) { rt_set_last_error(ERR_HANDLE); return 0; }
    if (off + n > sz) { rt_set_last_error(ERR_ACCESS); return 0; }
    if (anon) {
        mach_vm_address_t t = a; vm_prot_t cur, max;
        kern_return_t k = mach_vm_remap(mach_task_self(), &t, n, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                        (mach_vm_address_t)(anon + off), FALSE, &cur, &max, VM_INHERIT_SHARE);
        if (k != KERN_SUCCESS || t != a) { rt_set_last_error(ERR_NOMEM); return 0; }
        if (host_prot(prot) != (PROT_READ | PROT_WRITE)) mprotect((void *)a, n, host_prot(prot));
        return 1;
    }
    void *p = mmap((void *)a, n, host_prot(prot) ? host_prot(prot) : PROT_READ, MAP_SHARED | MAP_FIXED, fd, (off_t)off);
    if (p != (void *)a) { rt_set_last_error(ERR_NOMEM); return 0; }
    return 1;
}

// ---- MapViewOfFile3(section, process, addr, offset, size, type, protect, params, count) -------------------------
SHIM(MapViewOfFile3) {
    u64 h = A1, addr = A3, off = A4, size = AN(5); u32 type = (u32)AN(6), prot = (u32)AN(7);
    int fd; u8 *anon; u64 sz;
    if (!rt_mapping_info(h, &fd, &anon, &sz)) { rt_set_last_error(ERR_HANDLE); RET(0); }
    if (!size) size = sz - off;
    size = rnd(size);
    pthread_mutex_lock(&vm_lock); vm_used = 1;
    u64 r = 0;
    if (type & MEM_REPLACE_PLACEHOLDER_) {
        int i = find(addr);
        if (i < 0 || !rg[i].ph || rg[i].base != addr || rg[i].size != size) { rt_set_last_error(ERR_INVALID_ADDRESS); goto out; }
        if (!map_section(h, addr, off, size, prot)) goto out;
        rg[i] = (Region){ addr, size, addr, MEM_COMMIT_, MEM_MAPPED_, prot, prot, 0 };
        r = addr;
    } else {
        u64 a = reserve_host(addr, size, 0, 0, 0);
        if (!a) { rt_set_last_error(addr ? ERR_INVALID_ADDRESS : ERR_NOMEM); goto out; }
        if (!map_section(h, a, off, size, prot)) { munmap((void *)a, size); goto out; }
        insert((Region){ a, size, a, MEM_COMMIT_, MEM_MAPPED_, prot, prot, 0 });
        r = a;
    }
out:
    pthread_mutex_unlock(&vm_lock);
    RET(r);
}

// ---- UnmapViewOfFile2(process, addr, flags) / UnmapViewOfFileEx(addr, flags) --------------------------------------
static int unmap_view(u64 addr, u32 flags) {
    pthread_mutex_lock(&vm_lock);
    int i = find(addr), ok = 0;
    if (i < 0 || rg[i].type != MEM_MAPPED_ || rg[i].abase != addr) { rt_set_last_error(ERR_INVALID_ADDRESS); goto out; }
    u64 a = rg[i].abase, e = a;
    while (i < nrg && rg[i].abase == a) { e = rg[i].base + rg[i].size; erase(i); }
    if (flags & MEM_PRESERVE_PLACEHOLDER_) { remap_none(a, e - a); insert((Region){ a, e - a, a, MEM_RESERVE_, MEM_PRIVATE_, 0, PAGE_NOACCESS_, 1 }); }
    else munmap((void *)a, e - a);
    ok = 1;
out:
    pthread_mutex_unlock(&vm_lock);
    return ok;
}
SHIM(UnmapViewOfFile2) { RET(unmap_view(A2, (u32)A3)); }
SHIM(UnmapViewOfFileEx) { RET(unmap_view(A1, (u32)A2)); }

// ---- CompareObjectHandles(a, b) ------------------------------------------------------------------------------------
SHIM(CompareObjectHandles) {
    int fa, fb; u8 *aa, *ab; u64 sa, sb;
    if (A1 == A2) RET(1);
    if (rt_mapping_info(A1, &fa, &aa, &sa) && rt_mapping_info(A2, &fb, &ab, &sb)) RET(aa ? aa == ab : (fa >= 0 && fa == fb));
    RET(0);
}

// ---- hooks used by kernel32.c's VirtualFree / VirtualAlloc / VirtualProtect / VirtualQuery ---------------------------
// VirtualFree(addr, size, type) on a range this file manages. Returns 1 if handled (*ret = BOOL result).
static int rt_vmem_free_(u64 addr, u64 size, u32 type, u64 *ret);
int rt_vmem_free(u64 addr, u64 size, u32 type, u64 *ret) {
    int h = rt_vmem_free_(addr, size, type, ret);
    if (h && vm_log()) rt_log("vmem: VirtualFree(%llx, %llx, %x) = %llu", (unsigned long long)addr, (unsigned long long)size, type, (unsigned long long)*ret);
    return h;
}
static int rt_vmem_free_(u64 addr, u64 size, u32 type, u64 *ret) {
    pthread_mutex_lock(&vm_lock);
    int i = find(addr);
    if (i < 0) { pthread_mutex_unlock(&vm_lock); return 0; }
    *ret = 0;
    if ((type & MEM_RELEASE_) && (type & MEM_PRESERVE_PLACEHOLDER_)) {
        u64 e = addr + rnd(size);
        if (!size || !covered(addr, e - addr)) { rt_set_last_error(ERR_PARAM); goto out; }
        if (rg[i].ph) {                               // split a placeholder: [addr, e) becomes a placeholder of its own
            if (find(e - 1) != i) { rt_set_last_error(ERR_PARAM); goto out; }
            u64 old = rg[i].abase;
            cut(addr); cut(e);
            rg[find(addr)].abase = addr;
            int k = find(e);
            if (k >= 0 && rg[k].ph && rg[k].abase == old && rg[k].base == e) rg[k].abase = e;
        } else {                                      // private allocation back to a placeholder: whole allocation only
            u64 a = rg[i].abase;
            if (a != addr) { rt_set_last_error(ERR_PARAM); goto out; }
            u64 end = addr; int k = i;
            while (k < nrg && rg[k].abase == a) { end = rg[k].base + rg[k].size; k++; }
            if (end != e) { rt_set_last_error(ERR_PARAM); goto out; }
            while (i < nrg && rg[i].abase == a) erase(i);
            remap_none(addr, e - addr);
            insert((Region){ addr, e - addr, addr, MEM_RESERVE_, MEM_PRIVATE_, 0, PAGE_NOACCESS_, 1 });
        }
        *ret = 1;
    } else if ((type & MEM_RELEASE_) && (type & MEM_COALESCE_PLACEHOLDERS_)) {
        u64 e = addr + rnd(size);
        for (u64 p = addr; p < e;) { int k = find(p); if (k < 0 || !rg[k].ph) { rt_set_last_error(ERR_PARAM); goto out; } p = rg[k].base + rg[k].size; }
        if (rg[i].base != addr || rg[find(e - 1)].base + rg[find(e - 1)].size != e) { rt_set_last_error(ERR_PARAM); goto out; }
        for (int k = i; k < nrg && rg[k].base < e;) erase(k);
        insert((Region){ addr, e - addr, addr, MEM_RESERVE_, MEM_PRIVATE_, 0, PAGE_NOACCESS_, 1 });
        *ret = 1;
    } else if (type & MEM_RELEASE_) {                 // release a whole allocation
        if (size || rg[i].abase != addr) { rt_set_last_error(ERR_PARAM); goto out; }
        u64 e = addr;
        while (i < nrg && rg[i].abase == addr) { e = rg[i].base + rg[i].size; erase(i); }
        munmap((void *)addr, e - addr);
        *ret = 1;
    } else if (type & MEM_DECOMMIT_) {
        u64 a = addr & ~(u64)(getpagesize() - 1), e = size ? rnd(addr + size) : rg[i].base + rg[i].size;
        if (!covered(a, e - a)) { rt_set_last_error(ERR_PARAM); goto out; }
        cut(a); cut(e);
        for (int k = find(a); k >= 0 && k < nrg && rg[k].base < e; k++) {
            if (rg[k].ph || rg[k].type == MEM_MAPPED_) continue;
            remap_none(rg[k].base, rg[k].size); rg[k].state = MEM_RESERVE_; rg[k].prot = 0;
        }
        *ret = 1;
    } else rt_set_last_error(ERR_PARAM);
out:
    pthread_mutex_unlock(&vm_lock);
    return 1;
}
// VirtualAlloc(addr, size, MEM_COMMIT, prot) inside a reservation made here.
int rt_vmem_commit(u64 addr, u64 size, u32 type, u32 prot, u64 *ret) {
    pthread_mutex_lock(&vm_lock);
    int ours = addr && find(addr) >= 0;
    pthread_mutex_unlock(&vm_lock);
    if (!ours) return 0;
    Ctx tmp; memset(&tmp, 0, sizeof tmp);
    u64 stack[8] = { 0 }; stack[5] = prot;
    tmp.r[RCX] = (u64)-1; tmp.r[RDX] = addr; tmp.r[R8] = size; tmp.r[R9] = type; tmp.r[RSP] = (u64)stack;
    sh_VirtualAlloc2(&tmp);
    *ret = tmp.r[RAX];
    return 1;
}
int rt_vmem_protect(u64 addr, u64 size, u32 prot, u64 old_out) {
    pthread_mutex_lock(&vm_lock);
    int i = find(addr);
    if (i < 0) { pthread_mutex_unlock(&vm_lock); return 0; }
    u64 a = addr & ~(u64)(getpagesize() - 1), e = rnd(addr + size);
    if (old_out) ST32(old_out, rg[i].prot ? rg[i].prot : PAGE_NOACCESS_);
    cut(a); cut(e);
    for (int k = find(a); k >= 0 && k < nrg && rg[k].base < e; k++) if (rg[k].state == MEM_COMMIT_) rg[k].prot = prot;
    mprotect((void *)a, e - a, host_prot(prot));
    pthread_mutex_unlock(&vm_lock);
    return 1;
}

// VirtualQuery for any address: our regions exactly; elsewhere the host map (mach_vm_region) with free gaps reported
// as MEM_FREE, so address-space scans advance region by region. imgbase: recompiled image containing a (MEM_IMAGE).
void rt_vmem_query(u64 addr, u64 mbi, u64 imgbase, u64 imgsize, u64 resbase) {
    u64 a = addr & ~0xfffull;
    memset((void *)mbi, 0, 48);
    pthread_mutex_lock(&vm_lock);
    int i = find(a);
    if (i >= 0) {
        Region r = rg[i];
        pthread_mutex_unlock(&vm_lock);
        ST64(mbi, a); ST64(mbi + 8, r.abase); ST32(mbi + 16, r.aprot); ST64(mbi + 24, r.base + r.size - a);
        ST32(mbi + 32, r.state); ST32(mbi + 36, r.state == MEM_COMMIT_ ? r.prot : 0); ST32(mbi + 40, r.type);
        return;
    }
    int nx = lower(a); u64 next_ours = nx < nrg ? rg[nx].base : ~0ull;
    pthread_mutex_unlock(&vm_lock);
    // __PAGEZERO: the low 4 GB of every arm64 process, moved up by the main executable's ASLR slide, so it really
    // ends at the main image's header. No VM entry shows it, and mmap ignores hints inside it.
    u64 zero_end = (u64)_dyld_get_image_header(0) & ~0xffffull;
    if (zero_end < 0x100000000ull) zero_end = 0x100000000ull;
    if (a < zero_end) {
        ST64(mbi, a); ST32(mbi + 16, PAGE_NOACCESS_); ST64(mbi + 24, zero_end - a); ST32(mbi + 32, MEM_RESERVE_); ST32(mbi + 40, MEM_PRIVATE_);
        return;
    }
    mach_vm_address_t ra = a; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t k = mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj);
    if (k != KERN_SUCCESS || ra > a) {   // a is in a gap: free up to the next mapping
        u64 end = k == KERN_SUCCESS ? ra : 0x7FFFFFFF0000ull;
        if (next_ours < end) end = next_ours;
        ST64(mbi, a); ST64(mbi + 24, end - a); ST32(mbi + 32, MEM_FREE_); ST32(mbi + 36, PAGE_NOACCESS_);
        return;
    }
    u64 end = ra + rs; if (next_ours < end) end = next_ours;
    if (imgbase && imgbase + imgsize < end && imgbase + imgsize > a) end = imgbase + imgsize;
    u32 p = info.protection == VM_PROT_NONE ? PAGE_NOACCESS_ : (info.protection & VM_PROT_WRITE) ? PAGE_READWRITE_ :
            (info.protection & VM_PROT_EXECUTE) ? 0x20 : 2;
    if (imgbase) p = PAGE_READWRITE_;   // recompiled images: guest code is never executed from its pages
    ST64(mbi, a);
    ST64(mbi + 8, imgbase ? imgbase : resbase ? resbase : ra);
    ST32(mbi + 16, PAGE_READWRITE_);
    ST64(mbi + 24, end - a);
    if (info.protection == VM_PROT_NONE && !imgbase) { ST32(mbi + 32, MEM_RESERVE_); ST32(mbi + 36, 0); }   // e.g. __PAGEZERO
    else { ST32(mbi + 32, MEM_COMMIT_); ST32(mbi + 36, p); }
    ST32(mbi + 40, imgbase ? MEM_IMAGE_ : MEM_PRIVATE_);
}

const Shim shims_ps5_vmem[] = {
#define E(n) { NULL, #n, sh_##n }
    E(VirtualAlloc2), { NULL, "VirtualAlloc2FromApp", sh_VirtualAlloc2 }, E(MapViewOfFile3), { NULL, "MapViewOfFile3FromApp", sh_MapViewOfFile3 },
    E(UnmapViewOfFile2), E(UnmapViewOfFileEx), E(CompareObjectHandles),
    { NULL, NULL, NULL } };
