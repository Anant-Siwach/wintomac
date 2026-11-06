struct V { float4 pos : SV_Position; float4 col : COLOR0; };
float4 main(V v) : SV_Target { return v.col; }
