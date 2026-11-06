// SM 6.2 native 16-bit types (-enable-16bit-types): half/int16 arithmetic and 16-bit loads/stores.
#include "common.hlsli"
[numthreads(1, 1, 1)]
void main() {
    float16_t h = (float16_t)asfloat(inp.Load(4 * 66));            // 1.5
    float16_t2 v = inp.Load<float16_t2>(4 * 67);                    // (2.0, -0.25)
    float16_t x = h * v.x + v.y;                                    // 2.75
    uint16_t u = (uint16_t)65535; u = u + (uint16_t)k.z;            // wraps: 65535 + 2 = 1
    int16_t s = (int16_t)-300 * (int16_t)k.z;                       // -600
    o.Store<float16_t2>(0, float16_t2(x, sqrt(v.x)));
    o.Store<uint16_t2>(4, uint16_t2(u, (uint16_t)s));
    o.Store(8, asuint((float)x));
    o.Store(12, (uint)(int)s);
}
