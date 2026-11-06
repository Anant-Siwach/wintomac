// C++ exceptions through recompiled code: throw/catch across frames, destructors during unwinding,
// rethrow, catch(...), std exceptions, exceptions through function pointers and virtual calls.
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>

static int live = 0, destroyed = 0;
struct Guard { int id; explicit Guard(int i) : id(i) { live++; } ~Guard() { live--; destroyed++; } };

__attribute__((noinline)) static int deep(int n) {
    Guard g(n);
    if (n == 0) throw std::runtime_error("bottom reached");
    return deep(n - 1) + 1;
}
__attribute__((noinline)) static void thrower(int kind) {
    Guard g(100 + kind);
    switch (kind) {
    case 0: throw 42;
    case 1: throw std::string("a string");
    case 2: throw std::out_of_range("index");
    case 3: { std::vector<int> v(3); (void)v.at(10); break; }
    case 4: throw 3.5;
    }
}
struct Base { virtual ~Base() {} virtual void act(int x) = 0; };
struct Impl : Base { void act(int x) override { Guard g(x); if (x > 5) throw std::logic_error("too big: " + std::to_string(x)); } };

int main() {
    try { deep(5); } catch (const std::exception &e) { printf("caught '%s' live=%d destroyed=%d\n", e.what(), live, destroyed); }
    for (int k = 0; k < 5; k++) {
        try { thrower(k); }
        catch (int v) { printf("kind %d: int %d\n", k, v); }
        catch (const std::string &s) { printf("kind %d: string '%s'\n", k, s.c_str()); }
        catch (const std::out_of_range &e) { printf("kind %d: out_of_range\n", k); }
        catch (...) { printf("kind %d: something else\n", k); }
    }
    printf("after kinds: live=%d\n", live);
    int rethrows = 0;
    try { try { throw std::runtime_error("inner"); } catch (...) { rethrows++; throw; } }
    catch (const std::runtime_error &e) { printf("rethrown '%s' rethrows=%d\n", e.what(), rethrows); }
    std::unique_ptr<Base> b(new Impl);
    void (*fp)(int) = [](int k) { if (k == 2) throw k * 10; };
    int caught = 0;
    for (int i = 0; i < 10; i++) {
        try { b->act(i); fp(i % 3); } catch (const std::logic_error &e) { caught++; if (i == 9) printf("last logic_error: %s\n", e.what()); } catch (int v) { caught += 100; }
    }
    printf("loop caught=%d live=%d\n", caught, live);
    try { throw std::runtime_error("final"); } catch (std::exception &e) { printf("done: %s, total destroyed=%d\n", e.what(), destroyed); }
    return 0;
}
