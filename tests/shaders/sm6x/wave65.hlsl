// SM 6.5 wave match / multi-prefix.
#include "common.hlsli"
[numthreads(64, 1, 1)]
void main(uint tid : SV_DispatchThreadID) {
    uint key = tid % 3, b = tid * 16;
    uint4 m = WaveMatch(key);
    o.Store(b + 0, countbits(m.x) + countbits(m.y) + countbits(m.z) + countbits(m.w));
    o.Store(b + 4, WaveMultiPrefixSum(1u, m));
    o.Store(b + 8, WaveMultiPrefixCountBits(true, m));
    o.Store(b + 12, WaveGetLaneCount());
}
