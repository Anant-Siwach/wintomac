TextureCube<float4> t : register(t0);
SamplerState s : register(s0);
cbuffer K : register(b0) { float z_; float2 uv; };   // z_ is the VS depth
float4 main(float4 pos : SV_Position) : SV_Target { return t.SampleLevel(s, float3(uv, 1), 0) + float4(0, 0, 0, 1); }
