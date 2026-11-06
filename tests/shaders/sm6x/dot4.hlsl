// SM 6.4 packed dot products.
#include "common.hlsli"
[numthreads(1, 1, 1)]
void main() {
    uint a = 0x04030201, b = 0x08070605;
    o.Store(0, dot4add_u8packed(a, b, k.w));                       // 5+12+21+32 + 10 = 80
    o.Store(4, (uint)dot4add_i8packed(0xff020304u, 0x01ff0102u, 1)); // -1 + -2 + 0 + 8 ... see test
    o.Store(8, asuint(dot2add(half2(1.5, 2.0), half2(2.0, -1.0), 0.5f)));   // 3 - 2 + 0.5 = 1.5
}
