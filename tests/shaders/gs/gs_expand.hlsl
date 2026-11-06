// Point sprite expansion: one point in, a quad out.
struct V { float4 pos : SV_Position; float4 col : COLOR0; };
[maxvertexcount(4)]
void main(point V p[1], inout TriangleStream<V> s) {
    for (int i = 0; i < 4; i++) { V v = p[0]; v.pos.xy += float2(i & 1, i >> 1) * 0.2; s.Append(v); }
}
