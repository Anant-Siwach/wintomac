// Pass-through geometry shader: one triangle in, the same triangle out.
struct V { float4 pos : SV_Position; float4 col : COLOR0; };
[maxvertexcount(3)]
void main(triangle V t[3], inout TriangleStream<V> s) { for (int i = 0; i < 3; i++) s.Append(t[i]); }
