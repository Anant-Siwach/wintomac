// tests/src/dx12_tables.c: descriptor tables (CBV + SRV), a sampler table, a static sampler and root constants.
cbuffer C : register(b0) { float4 tint; };
cbuffer R : register(b1) { float4 add; };
Texture2D<float4> tex : register(t0);
SamplerState pt : register(s0);     // sampler heap (table)
SamplerState lin : register(s1);    // static sampler
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID) {
    V v; float2 p = float2((id << 1) & 2, id & 2);
    v.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); v.uv = p; return v;
}
float4 ps_main(V i) : SV_Target {
    return tex.Sample(pt, i.uv) * tint * 0.5 + tex.Sample(lin, i.uv * 1.5) * 0.5 + add;
}
