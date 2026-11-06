// SM 6.0 wave intrinsics: 16 words per thread.
#include "common.hlsli"
[numthreads(64, 1, 1)]
void main(uint tid : SV_DispatchThreadID) {
    uint lane = WaveGetLaneIndex(), n = WaveGetLaneCount(), b = tid * 64;
    o.Store(b + 0, n);
    o.Store(b + 4, WaveActiveSum(tid));
    o.Store(b + 8, WavePrefixSum(1u));
    o.Store(b + 12, WaveReadLaneFirst(tid));
    o.Store(b + 16, countbits(WaveActiveBallot((tid & 1) != 0).x));
    o.Store(b + 20, WaveActiveMax(tid));
    o.Store(b + 24, WaveActiveAllTrue(tid < 1000) ? 1u : 0u);
    o.Store(b + 28, WaveReadLaneAt(tid, (lane + 1) % n));
    o.Store(b + 32, WaveActiveBitOr(1u << (lane & 31)));
    o.Store(b + 36, WaveIsFirstLane() ? 1u : 0u);
    o.Store(b + 40, WavePrefixProduct(tid < 4 ? 2u : 1u));
    o.Store(b + 44, WaveActiveCountBits(lane < 5));
    o.Store(b + 48, WaveActiveAnyTrue(lane == 3) ? 1u : 0u);
    o.Store(b + 52, WaveActiveAllEqual(tid / 64) ? 1u : 0u);
    o.Store(b + 56, WavePrefixCountBits(true));
    o.Store(b + 60, lane);
}
