// SM 6.5 mesh shader: one triangle.
struct V { float4 pos : SV_Position; };
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(out vertices V v[3], out indices uint3 t[1]) {
    SetMeshOutputCounts(3, 1);
    v[0].pos = float4(-1, -1, 0, 1); v[1].pos = float4(0, 1, 0, 1); v[2].pos = float4(1, -1, 0, 1);
    t[0] = uint3(0, 1, 2);
}
