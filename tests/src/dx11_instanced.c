// Offscreen check of the dx11_window demo's shader: SV_VertexID/SV_InstanceID, no input layout, DrawInstanced,
// dynamic constant buffer. (Regression test for a demo that presented frames but drew nothing.)
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#define W 96
#define H 60
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08lx\n", #x, (unsigned long)hr_); return 1; } } while (0)
static const char hlsl[] =
    "cbuffer Frame : register(b0) { float time; float aspect; float2 pad; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VSOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  float a = time * 0.6 + iid * 0.3926991;                       // 16 instances around a ring\n"
    "  float r = 0.55 + 0.1 * sin(time * 2.0 + iid);\n"
    "  float2 center = float2(cos(a), sin(a)) * r;\n"
    "  float t = -(vid * 2.0943951) + time * (iid % 2 ? 2.0 : -2.0); // clockwise winding: D3D11 culls CCW by default\n"
    "  float2 p = center + float2(cos(t), sin(t)) * 0.12;\n"
    "  VSOut o; o.pos = float4(p.x / aspect, p.y, 0, 1);\n"
    "  float h = frac(iid / 16.0 + time * 0.05);\n"
    "  o.col = float4(saturate(abs(h * 6 - 3) - 1), saturate(2 - abs(h * 6 - 2)), saturate(2 - abs(h * 6 - 4)), 1);\n"
    "  o.col.rgb *= 0.6 + 0.4 * (vid == 0);\n"
    "  return o; }\n"
    "float4 ps_main(VSOut i) : SV_Target { return i.col; }\n";


int main(void) {
    ID3D11Device *dev; ID3D11DeviceContext *ctx; D3D_FEATURE_LEVEL fl;
    CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx));
    ID3DBlob *vsb, *psb, *err = NULL;
    if (FAILED(D3DCompile(hlsl, sizeof hlsl - 1, "demo.hlsl", NULL, NULL, "vs_main", "vs_5_0", 0, 0, &vsb, &err))) { printf("vs: %s\n", (char *)ID3D10Blob_GetBufferPointer(err)); return 1; }
    if (FAILED(D3DCompile(hlsl, sizeof hlsl - 1, "demo.hlsl", NULL, NULL, "ps_main", "ps_5_0", 0, 0, &psb, &err))) { printf("ps: %s\n", (char *)ID3D10Blob_GetBufferPointer(err)); return 1; }
    ID3D11VertexShader *vs; ID3D11PixelShader *ps;
    CHECK(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs));
    CHECK(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps));
    D3D11_BUFFER_DESC cbd = { 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    ID3D11Buffer *cb; CHECK(ID3D11Device_CreateBuffer(dev, &cbd, NULL, &cb));
    D3D11_MAPPED_SUBRESOURCE m; CHECK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    ((float *)m.pData)[0] = 1.0f; ((float *)m.pData)[1] = (float)W / H; ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
    D3D11_TEXTURE2D_DESC td = { W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Texture2D *rt, *st; ID3D11RenderTargetView *rtv;
    CHECK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st));
    CHECK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv));
    float clear[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 }; ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0); ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_DrawInstanced(ctx, 3, 16, 0, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);
    CHECK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m));
    int lit = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t *p = (uint8_t *)m.pData + y * m.RowPitch + 4 * x; if (p[0] + p[1] + p[2] > 60) lit++; }
    printf("lit pixels: %d (%s)\n", lit, lit > 150 && lit < 3000 ? "16 triangles plausible" : "WRONG");
    for (int y = 0; y < H; y += 3) { for (int x = 0; x < W; x += 2) { uint8_t *p = (uint8_t *)m.pData + y * m.RowPitch + 4 * x;
        putchar(p[0] + p[1] + p[2] > 60 ? (p[0] > p[1] && p[0] > p[2] ? 'R' : p[1] > p[2] ? 'G' : 'B') : '.'); } putchar('\n'); }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    return 0;
}
