// Wave ops in a vertex shader (MoltenVK exposes subgroup ops only in fragment/compute/tess-control).
float4 main(uint i : SV_VertexID) : SV_Position {
    float2 p[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    return float4(p[i], (float)WaveGetLaneIndex() * 0.0, 1);
}
