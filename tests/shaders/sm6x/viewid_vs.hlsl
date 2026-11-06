// SM 6.1 SV_ViewID (view instancing).
float4 main(uint i : SV_VertexID, uint view : SV_ViewID) : SV_Position {
    float2 p[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    return float4(p[i] * (view + 1), 0, 1);
}
