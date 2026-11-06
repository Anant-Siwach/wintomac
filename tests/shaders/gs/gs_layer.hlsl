// Layered rendering: the triangle goes to render target array slice 1.
struct V { float4 pos : SV_Position; float4 col : COLOR0; };
struct L { float4 pos : SV_Position; float4 col : COLOR0; uint layer : SV_RenderTargetArrayIndex; };
[maxvertexcount(3)]
void main(triangle V t[3], inout TriangleStream<L> s) {
    for (int i = 0; i < 3; i++) { L l; l.pos = t[i].pos; l.col = t[i].col; l.layer = 1; s.Append(l); }
}
