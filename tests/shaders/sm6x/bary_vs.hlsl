// Full-screen-ish triangle with a per-vertex id for SV_Barycentrics / GetAttributeAtVertex.
struct V { float4 pos : SV_Position; nointerpolation uint id : ID; };
V main(uint i : SV_VertexID) {
    V v; float2 p[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    v.pos = float4(p[i], 0, 1); v.id = 10 + i; return v;
}
