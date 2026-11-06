/* PS5 compat: memory: mmap/mprotect/munmap, malloc stress, aligned alloc, realloc, then PS5 direct/flexible memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#ifdef __APPLE__
#define PS5 0
#else
#define PS5 1
int sceKernelAllocateDirectMemory(long start, long end, size_t len, size_t align, int type, long *phys);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, long phys, size_t align);
int sceKernelReleaseDirectMemory(long phys, size_t len);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelMmap(void *addr, size_t len, int prot, int flags, int fd, long off, void **res);
int sceKernelMprotect(const void *addr, size_t len, int prot);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelMapNamedFlexibleMemory(void **addr, size_t len, int prot, int flags, const char *name);
#endif
int main(void) {
    size_t sz = 1 << 20;
#if PS5
    /* the SDK's mmap/mprotect are raw syscall wrappers, which the AnyPS5 relinker rejects: use libkernel's sce* forms */
    uint8_t *m = 0; int mrc = sceKernelMmap(0, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0, (void **)&m);
    printf("mmap ok=%d\n", mrc == 0);
#else
    uint8_t *m = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    printf("mmap ok=%d\n", m != MAP_FAILED);
#endif
    for (size_t i = 0; i < sz; i += 4096) m[i] = (uint8_t)(i >> 12);
    unsigned s = 0; for (size_t i = 0; i < sz; i += 4096) s += m[i];
    printf("pages sum=%u zero-fill=%d\n", s, m[1] == 0 && m[sz - 1] == 0);
#if PS5
    printf("mprotect=%d\n", sceKernelMprotect(m, 4096, PROT_READ));
    printf("munmap=%d\n", sceKernelMunmap(m, sz));
#else
    printf("mprotect=%d\n", mprotect(m, 4096, PROT_READ));
    printf("munmap=%d\n", munmap(m, sz));
#endif
    void *ps[64]; for (int i = 0; i < 64; i++) { ps[i] = malloc(1000 + i * 977); memset(ps[i], i, 1000); }
    int ok = 1; for (int i = 0; i < 64; i++) ok &= ((uint8_t *)ps[i])[999] == i;
    for (int i = 0; i < 64; i++) free(ps[i]);
    printf("malloc stress ok=%d\n", ok);
    void *a; posix_memalign(&a, 65536, 100000); printf("memalign64k ok=%d\n", ((uintptr_t)a & 0xffff) == 0); free(a);
    char *r = malloc(10); strcpy(r, "grow"); r = realloc(r, 1 << 22); printf("realloc keeps=%s\n", r); free(r);
    char *c = calloc(1000, 8); int z = 1; for (int i = 0; i < 8000; i++) z &= c[i] == 0; printf("calloc zero=%d\n", z); free(c);
#if PS5
    size_t dm = sceKernelGetDirectMemorySize(); printf("direct memory size >= 1GiB: %d\n", dm >= (1UL << 30));
    long phys = -1; int rc = sceKernelAllocateDirectMemory(0, (long)dm, 2 << 20, 2 << 20, 3, &phys);
    printf("allocdirect rc=%d\n", rc);
    void *da = 0; rc = sceKernelMapDirectMemory(&da, 2 << 20, 3, 0, phys, 2 << 20);
    printf("mapdirect rc=%d aligned=%d\n", rc, ((uintptr_t)da & ((2 << 20) - 1)) == 0);
    if (da) { memset(da, 0x5a, 2 << 20); printf("direct rw=%d\n", ((uint8_t *)da)[(2 << 20) - 1] == 0x5a); }
    printf("munmap rc=%d\n", sceKernelMunmap(da, 2 << 20));
    printf("release rc=%d\n", sceKernelReleaseDirectMemory(phys, 2 << 20));
    void *fm = 0; rc = sceKernelMapNamedFlexibleMemory(&fm, 1 << 20, 3, 0, "wt-flex");
    printf("flex rc=%d\n", rc);
    if (fm) { memset(fm, 1, 1 << 20); printf("flex rw=%d\n", ((uint8_t *)fm)[12345] == 1); sceKernelMunmap(fm, 1 << 20); }
#endif
    return 0;
}
