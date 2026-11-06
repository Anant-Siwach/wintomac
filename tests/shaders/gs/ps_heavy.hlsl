// A deliberately slow pixel shader (GPU load for dx12_async_present): k.x iterations; frac() < 1, so the result depends only
// on k.y, so every frame's pixels are known.
cbuffer K : register(b0) { uint iterations; uint frame; };
float4 main(float4 pos : SV_Position) : SV_Target {
    float a = 0;
    for (uint i = 0; i < iterations; i++) a = frac(a * 1.0001 + sin(a + (float)i * 0.001 + pos.x * 0.01));
    return float4((frame & 255) / 255.0, 0.5 + step(2.0, a), (frame % 7) / 7.0, 1);
}
