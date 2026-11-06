// SM 6.6 pack/unpack intrinsics, IsHelperLane, [WaveSize].
#include "common.hlsli"
[WaveSize(32)]
[numthreads(32, 1, 1)]
void main(uint tid : SV_GroupIndex) {
    if (tid != 0) return;
    o.Store(0, (uint)pack_u8(uint32_t4(1, 2, 3, 0x1ff)));
    int32_t4 u = unpack_s8s32((int8_t4_packed)0x80ff7f01u);
    o.Store4(4, (uint4)u);
    o.Store(20, (uint)pack_clamp_s8(int32_t4(-300, 300, -5, 5)));
    o.Store(24, IsHelperLane() ? 1u : 0u);
    o.Store(28, WaveGetLaneCount());
}
