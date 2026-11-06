// SM 6.0 64-bit integer arithmetic (shaderInt64).
#include "common.hlsli"
[numthreads(1, 1, 1)]
void main() {
    uint64_t a = ((uint64_t)k.y << 32) | k.x;             // 0x0000000500000007
    uint64_t b = ((uint64_t)inp.Load(4 * 64 + 4) << 32) | inp.Load(4 * 64);   // 0x123456789abcdef0
    uint64_t r0 = a * b, r1 = b >> 7, r2 = b / a, r3 = b % a;
    int64_t s = -(int64_t)a >> 3;
    uint64_t r[5] = { r0, r1, r2, r3, (uint64_t)s };
    for (uint i = 0; i < 5; i++) { o.Store(8 * i, (uint)r[i]); o.Store(8 * i + 4, (uint)(r[i] >> 32)); }
}
