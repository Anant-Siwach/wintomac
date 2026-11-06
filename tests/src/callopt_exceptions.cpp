// C++ exceptions unwinding through frames whose registers live in C locals with the selective call-site sync
// (recomp/regsum.py): callee-saved registers modified by intermediate frames, catching frames with lots of
// values live across the try, destructors (cleanup landing pads) in the middle, rethrow, and nested try.
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

static int dtor_count;
struct Guard {
    int id; unsigned long long *acc;
    Guard(int i, unsigned long long *a) : id(i), acc(a) {}
    ~Guard() { dtor_count++; *acc = *acc * 31 + id; }
};

__attribute__((noinline)) static long long thrower(int n) {
    if (n % 7 == 3) throw std::runtime_error("boom " + std::to_string(n));
    if (n % 11 == 5) throw n;
    return n * 5;
}

// No handler of its own, many values in callee-saved registers, modified before the call that throws.
__attribute__((noinline)) static long long middle(int n, long long seed) {
    long long a = seed, b = seed * 3, c = seed ^ 0x1234, d = n, e = n * n, f = seed - n, g = 7, h = 9;
    for (int i = 0; i < n % 5 + 1; i++) { a += b; b ^= c; c += d * 3; d += e; e ^= f; f += g; g *= 3; h += a; }
    long long r = thrower(n);
    return r + a + b + c + d + e + f + g + h;
}

// Cleanup frame: destructors run during unwinding, then the exception continues.
__attribute__((noinline)) static long long with_guards(int n, unsigned long long *acc) {
    Guard g1(n, acc);
    long long x = n * 11, y = n * 13;
    Guard g2(n + 100, acc);
    long long r = middle(n, x ^ y);
    return r + x - y;
}

__attribute__((noinline)) static long long rethrower(int n, unsigned long long *acc) {
    long long keep1 = n * 17, keep2 = n * 19, keep3 = n ^ 0x5a5a;
    try {
        return with_guards(n, acc) + keep1;
    } catch (int v) {
        *acc += v + keep1 + keep2 + keep3;   // values live across the try, read in the handler
        throw;
    }
}

int main() {
    unsigned long long acc = 0; long long total = 0;
    long long live1 = 1111, live2 = 2222, live3 = 3333, live4 = 4444, live5 = 5555;
    std::vector<std::string> msgs;
    for (int n = 0; n < 60; n++) {
        try {
            try {
                total += rethrower(n, &acc);
            } catch (const std::runtime_error &e) {
                msgs.push_back(e.what());
                total += live1 + n;
            }
        } catch (int v) {
            total += v * live2 - live3;
        }
        live1 += live4; live4 ^= live5; live5 += n;
    }
    std::printf("total=%lld acc=%llu dtors=%d msgs=%zu first=%s last=%s\n", total, acc, dtor_count, msgs.size(),
                msgs.front().c_str(), msgs.back().c_str());
    std::printf("live %lld %lld %lld %lld %lld\n", live1, live2, live3, live4, live5);
    return 0;
}
