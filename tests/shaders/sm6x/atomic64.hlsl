// SM 6.6 64-bit atomics on a raw buffer and on groupshared memory (required by SM 6.6).
#include "common.hlsli"
groupshared uint64_t g;
[numthreads(64, 1, 1)]
void main(uint tid : SV_GroupIndex) {
    if (tid == 0) g = 0;
    GroupMemoryBarrierWithGroupSync();
    uint64_t old;
    InterlockedAdd(g, ((uint64_t)1 << 32) + 1);
    o.InterlockedAdd64(0, ((uint64_t)1 << 33) + tid, old);
    o.InterlockedMax64(8, ((uint64_t)tid << 40) | 5);
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) o.Store2(16, uint2((uint)g, (uint)(g >> 32)));
}
