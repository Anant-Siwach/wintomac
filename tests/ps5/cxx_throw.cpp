// PS5 C++ exceptions through AnyPS5's HLE C++ runtime (libc.prx: __cxa_throw -> its DWARF unwinder -> landing pads),
// the way titles use it. PS5_CXXABI: hle   (tools/ps5_build.sh: no static libc++abi/libunwind from the SDK)
// Covers: cleanups (destructors -> _Unwind_Resume) across several frames, catch by type / by base / catch(...),
// rethrow, a throw from inside a catch block, nested try in a cleanup, exceptions thrown inside libc.prx and
// exceptions thrown by a callback that unwind through libc.prx's own frames (qsort), and many throws in a loop.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

static int depth_live = 0;   // live Guard objects: must be 0 after every catch
struct Guard {
    const char *name; int quiet;
    Guard(const char *n, int q = 0) : name(n), quiet(q) { depth_live++; }
    ~Guard() { depth_live--; if (!quiet) printf("  ~Guard %s\n", name); }
};
struct Err { virtual ~Err() {} virtual const char *what() const { return "Err"; } int code = 0; };
struct IoErr : Err { const char *what() const override { return "IoErr"; } };
// AnyPS5 libc.prx has no typeinfo for fundamental types (_ZTIi...): titles throw classes, so does this test
template <class T> struct Box { T v; };

__attribute__((noinline)) static void level3(int v) { Guard g("l3"); if (v > 0) { IoErr e; e.code = v; throw e; } printf("  no throw\n"); }
__attribute__((noinline)) static int level2(int v) { Guard g("l2"); level3(v); return 2; }
__attribute__((noinline)) static int level1(int v) { Guard g("l1"); return level2(v) + 1; }

__attribute__((noinline)) static void rethrower() {
    try { throw Box<int>{42}; }
    catch (Box<int> &b) { int i = b.v; printf("  inner caught %d, rethrowing\n", i); throw; }
}
__attribute__((noinline)) static void throw_in_catch() {
    try { throw Box<double>{1.5}; }
    catch (Box<double> &d) { printf("  caught %.1f, throwing a new one\n", d.v); Guard g("in-catch"); throw Box<long>{7}; }
}
// a cleanup whose destructor runs a try/catch of its own while the outer exception is in flight
struct Noisy { ~Noisy() { try { throw Box<char>{'x'}; } catch (Box<char> &c) { printf("  nested throw '%c' inside a destructor during unwinding\n", c.v); } } };
__attribute__((noinline)) static void nested_in_cleanup() { Noisy n; Guard g("nic"); throw Box<unsigned>{99}; }

static int cmp_calls;
static int cmp_throw(const void *a, const void *b) {
    if (++cmp_calls == 5) throw Err();
    return *(const int *)a - *(const int *)b;
}
static int cmp_ok(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

__attribute__((noinline)) static long recurse(int n) { Guard g("r", 1); if (!n) throw Box<int>{n}; return recurse(n - 1) + 1; }

int main() {
    printf("C++ exceptions via libc.prx\n");
    try { printf("level1 = %d\n", level1(0)); } catch (...) { printf("unexpected\n"); }
    try { level1(5); } catch (IoErr &e) { printf("caught %s code=%d live=%d\n", e.what(), e.code, depth_live); }
    try { level1(6); } catch (Err &e) { printf("caught by base: %s code=%d live=%d\n", e.what(), e.code, depth_live); }
    try { level1(7); } catch (...) { printf("caught (...) live=%d\n", depth_live); }
    try { rethrower(); } catch (Box<int> &i) { printf("outer caught rethrown %d\n", i.v); }
    try { throw_in_catch(); } catch (Box<long> &l) { printf("caught the new one %ld live=%d\n", l.v, depth_live); }
    try { nested_in_cleanup(); } catch (Box<unsigned> &u) { printf("caught %u after nested handling, live=%d\n", u.v, depth_live); }
    // a callback's exception unwinds through libc.prx frames (qsort's sort loop)
    int v[16]; for (int i = 0; i < 16; i++) v[i] = (i * 7) % 16;
    try { qsort(v, 16, sizeof v[0], cmp_throw); printf("qsort returned?\n"); }
    catch (Err &e) { printf("caught %s from the qsort comparator after %d calls\n", e.what(), cmp_calls); }
    qsort(v, 16, sizeof v[0], cmp_ok);
    printf("sorted after: %d %d %d ... %d\n", v[0], v[1], v[2], v[15]);
    // thrown inside libc.prx itself (std::invalid_argument for a null comparator)
    try { qsort(v, 16, sizeof v[0], 0); printf("qsort(NULL) returned?\n"); }
    catch (...) { printf("caught libc.prx's own exception (qsort with no comparator)\n"); }
    // deep recursion and many throws: frame records / scratch must not leak
    long total = 0;
    for (int i = 0; i < 2000; i++) {
        try { total += recurse(i % 50); } catch (Box<int> &n) { total += n.v + 1; }
    }
    printf("2000 throws through up to 50 frames: total=%ld live=%d\n", total, depth_live);
    try { throw Err(); } catch (Err &e) { printf("last: %s\n", e.what()); }
    printf("done\n");
    return 0;
}
