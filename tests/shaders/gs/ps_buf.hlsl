Buffer<float4> t : register(t0);
cbuffer K : register(b0) { float z_; float2 uv; };   // z_ is the VS depth
float4 main(float4 pos : SV_Position) : SV_Target { return t.Load(0) + float4(0, 0, 0, 1); }
