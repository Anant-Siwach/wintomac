// SM 6.6 compute-shader derivatives and quad ops (2x2 quads from an 8x8 group).
#include "common.hlsli"
[numthreads(8, 8, 1)]
void main(uint2 id : SV_GroupThreadID) {
    float v = (float)(id.x * id.x + 10 * id.y);
    uint b = (id.y * 8 + id.x) * 12;
    o.Store(b + 0, asuint(ddx_fine(v)));
    o.Store(b + 4, asuint(ddy_fine(v)));
    o.Store(b + 8, QuadReadAcrossX(id.x * 100 + id.y));
}
