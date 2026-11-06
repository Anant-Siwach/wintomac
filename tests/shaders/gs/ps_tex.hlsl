// Samples t0 with s0 at a fixed coordinate (border colour / null descriptor tests).
Texture2D<float4> t : register(t0);
SamplerState s : register(s0);
cbuffer K : register(b0) { float z_; float2 uv; };   // z_ is the VS depth
float4 main(float4 pos : SV_Position) : SV_Target { return t.SampleLevel(s, uv, 0); }
