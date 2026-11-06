cbuffer Params : register(b0) { float4 rot; float4 tint; };   // rot.xy = (cos, sin)
struct VSIn  { float2 pos : POSITION; float4 col : COLOR; };
struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };
VSOut vs_main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos.x * rot.x - i.pos.y * rot.y, i.pos.x * rot.y + i.pos.y * rot.x, 0.0, 1.0);
    o.col = i.col * tint;
    return o;
}
float4 ps_main(VSOut i) : SV_Target { return i.col; }
