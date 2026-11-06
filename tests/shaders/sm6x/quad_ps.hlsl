// SM 6.0 quad ops and SM 6.7 QuadAny in a pixel shader.
float4 main(float4 pos : SV_Position) : SV_Target {
    uint x = (uint)pos.x;
    return float4(QuadReadAcrossX((float)x) / 255.0, QuadReadAcrossDiagonal((float)(uint)pos.y) / 255.0, 0, 1);
}
