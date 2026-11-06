// SM 6.1 SV_Barycentrics + GetAttributeAtVertex.
struct V { float4 pos : SV_Position; nointerpolation uint id : ID; };
float4 main(V v, float3 b : SV_Barycentrics) : SV_Target {
    uint s = GetAttributeAtVertex(v.id, 0) + GetAttributeAtVertex(v.id, 1) * 100 + GetAttributeAtVertex(v.id, 2) * 10000;
    return float4(b, (float)s);
}
