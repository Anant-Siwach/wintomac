// Triangle in the left half of the target, red to green gradient; z from the root constant (depth-clip test).
cbuffer K : register(b0) { float z; };
struct V { float4 pos : SV_Position; float4 col : COLOR0; };
V main(uint i : SV_VertexID) {
    float2 p[3] = { float2(-0.9, -0.9), float2(-0.5, 0.9), float2(-0.1, -0.9) };
    V v; v.pos = float4(p[i], z, 1); v.col = float4(i == 0, i == 1, i == 2, 1); return v;
}
