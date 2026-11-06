// SM 6.5 inline ray tracing (DXR 1.1).
#include "common.hlsli"
RaytracingAccelerationStructure scene : register(t1);
[numthreads(1, 1, 1)]
void main() {
    RayQuery<RAY_FLAG_NONE> q;
    RayDesc r; r.Origin = 0; r.Direction = float3(0, 0, 1); r.TMin = 0; r.TMax = 100;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, r);
    q.Proceed();
    o.Store(0, q.CommittedStatus());
}
