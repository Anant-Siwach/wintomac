// Direct3D 11 offscreen triangle on DXVK. Draws twice: once with shaders compiled at runtime by D3DCompile
// (our native d3dcompiler -> vkd3d-shader), once with DXBC compiled ahead of time; the pixels must match.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>   /* define the IIDs used below in this file */
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define VCALL(o, m, ...) ((o)->lpVtbl->m((o), ##__VA_ARGS__))  /* mingw lacks C macros for d3d11shader.h */
#include "tri11_vs.h"
#include "tri11_ps.h"

#define W 64
#define H 64
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08lx\n", #x, (unsigned long)hr_); return 1; } } while (0)

static const char hlsl[] =
    "cbuffer Params : register(b0) { float4 rot; float4 tint; };\n"
    "struct VSIn  { float2 pos : POSITION; float4 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VSOut vs_main(VSIn i) { VSOut o;\n"
    "  o.pos = float4(i.pos.x * rot.x - i.pos.y * rot.y, i.pos.x * rot.y + i.pos.y * rot.x, 0.0, 1.0);\n"
    "  o.col = i.col * tint; return o; }\n"
    "float4 ps_main(VSOut i) : SV_Target { return i.col; }\n";

typedef struct { float x, y, r, g, b, a; } Vtx;
static ID3D11Device *dev; static ID3D11DeviceContext *ctx;
static ID3D11Texture2D *rt, *staging; static ID3D11RenderTargetView *rtv; static ID3D11Buffer *vb, *cb;

static int draw(const void *vs_code, SIZE_T vs_size, const void *ps_code, SIZE_T ps_size, uint64_t *hash, int show) {
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11InputLayout *il;
    CHECK(ID3D11Device_CreateVertexShader(dev, vs_code, vs_size, NULL, &vs));
    CHECK(ID3D11Device_CreatePixelShader(dev, ps_code, ps_size, NULL, &ps));
    D3D11_INPUT_ELEMENT_DESC ied[2] = { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                                        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    CHECK(ID3D11Device_CreateInputLayout(dev, ied, 2, vs_code, vs_size, &il));
    float clear[4] = { 0.05f, 0.05f, 0.1f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    UINT stride = sizeof(Vtx), off = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, il);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 }; ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE m; CHECK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m));
    uint64_t h = 1469598103934665603ULL; int lit = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = (uint8_t *)m.pData + y * m.RowPitch + 4 * x;
        for (int k = 0; k < 4; k++) { h ^= p[k]; h *= 1099511628211ULL; }
        if (p[0] > 40 || p[1] > 40) lit++;
    }
    if (show) {
        uint8_t *c0 = (uint8_t *)m.pData;
        printf("corner rgba=%u,%u,%u,%u lit_pixels_in_range=%d\n", c0[0], c0[1], c0[2], c0[3], lit > 800 && lit < 1300);
        for (int y = 0; y < H; y += 4) {
            for (int x = 0; x < W; x += 2) { uint8_t *p = (uint8_t *)m.pData + y * m.RowPitch + 4 * x;
                putchar(p[0] > 128 && p[0] > p[1] ? 'R' : p[1] > 128 && p[1] > p[2] ? 'G' : p[2] > 128 ? 'B' : (p[0] > 40 || p[1] > 40) ? '+' : '.'); }
            putchar('\n');
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    ID3D11InputLayout_Release(il); ID3D11PixelShader_Release(ps); ID3D11VertexShader_Release(vs);
    *hash = h;
    return 0;
}

int main(void) {
    D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, got;
    CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &want, 1, D3D11_SDK_VERSION, &dev, &got, &ctx));
    printf("feature level %x.%x\n", got >> 12, (got >> 8) & 0xF);
    D3D11_TEXTURE2D_DESC td = { W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    CHECK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging));
    CHECK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv));
    Vtx v[3] = { { 0.0f, 0.75f, 1, .2f, .2f, 1 }, { 0.7f, -0.6f, .2f, 1, .2f, 1 }, { -0.7f, -0.6f, .2f, .3f, 1, 1 } };
    D3D11_BUFFER_DESC bd = { sizeof v, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA sd = { v, 0, 0 };
    CHECK(ID3D11Device_CreateBuffer(dev, &bd, &sd, &vb));
    float params[8] = { 0.95533649f, 0.29552021f, 0, 0, 1, 1, 1, 1 };  // cos/sin(0.3), tint
    D3D11_BUFFER_DESC cbd = { sizeof params, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA cbs = { params, 0, 0 };
    CHECK(ID3D11Device_CreateBuffer(dev, &cbd, &cbs, &cb));

    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    CHECK(D3DCompile(hlsl, sizeof hlsl - 1, "tri11.hlsl", NULL, NULL, "vs_main", "vs_4_0", 0, 0, &vsb, &err));
    CHECK(D3DCompile(hlsl, sizeof hlsl - 1, "tri11.hlsl", NULL, NULL, "ps_main", "ps_4_0", 0, 0, &psb, &err));
    printf("runtime-compiled DXBC: vs %s, ps %s\n", memcmp(ID3D10Blob_GetBufferPointer(vsb), "DXBC", 4) ? "bad" : "ok",
           memcmp(ID3D10Blob_GetBufferPointer(psb), "DXBC", 4) ? "bad" : "ok");
    static const char bad_src[] = "float4 main() : SV_Target { return undefined_thing; }";
    HRESULT bad = D3DCompile(bad_src, sizeof bad_src - 1, "bad.hlsl", NULL, NULL, "main", "ps_4_0", 0, 0, &err, &err);
    printf("bad HLSL rejected: %s, error text: %s\n", FAILED(bad) ? "yes" : "no", err && ID3D10Blob_GetBufferSize(err) > 1 ? "yes" : "no");

    // Reflection of the runtime-compiled vertex shader (D3DReflect -> vkd3d-utils, guest-callable wrappers)
    ID3D11ShaderReflection *refl = NULL;
    CHECK(D3DReflect(ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &IID_ID3D11ShaderReflection, (void **)&refl));
    D3D11_SHADER_DESC sdesc; CHECK(VCALL(refl, GetDesc, &sdesc));
    printf("reflect: cbuffers=%u inputs=%u outputs=%u\n", sdesc.ConstantBuffers, sdesc.InputParameters, sdesc.OutputParameters);
    ID3D11ShaderReflectionConstantBuffer *rcb = VCALL(refl, GetConstantBufferByIndex, 0);
    D3D11_SHADER_BUFFER_DESC bdesc; CHECK(VCALL(rcb, GetDesc, &bdesc));
    printf("reflect: cbuffer %s size=%u vars=%u same_ptr_by_name=%s\n", bdesc.Name, bdesc.Size, bdesc.Variables,
           rcb == VCALL(refl, GetConstantBufferByName, "Params") ? "yes" : "no");
    ID3D11ShaderReflectionVariable *tint = VCALL(rcb, GetVariableByName, "tint");
    D3D11_SHADER_VARIABLE_DESC vdesc; CHECK(VCALL(tint, GetDesc, &vdesc));
    D3D11_SHADER_TYPE_DESC tdesc; CHECK(VCALL(VCALL(tint, GetType), GetDesc, &tdesc));
    printf("reflect: var %s offset=%u size=%u class=%d rows=%u cols=%u\n", vdesc.Name, vdesc.StartOffset, vdesc.Size, tdesc.Class, tdesc.Rows, tdesc.Columns);
    D3D11_SIGNATURE_PARAMETER_DESC pdesc; CHECK(VCALL(refl, GetInputParameterDesc, 1, &pdesc));
    printf("reflect: input[1] %s%u mask=%x\n", pdesc.SemanticName, pdesc.SemanticIndex, pdesc.Mask);
    D3D11_SHADER_INPUT_BIND_DESC rb; CHECK(VCALL(refl, GetResourceBindingDesc, 0, &rb));
    printf("reflect: binding %s type=%d point=%u\n", rb.Name, rb.Type, rb.BindPoint);
    VCALL(refl, Release);

    uint64_t h_rt, h_pre;
    if (draw(ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), &h_rt, 1)) return 1;
    if (draw(tri11_vs, sizeof tri11_vs, tri11_ps, sizeof tri11_ps, &h_pre, 0)) return 1;
    printf("runtime-compiled vs precompiled pixels identical: %s\n", h_rt == h_pre ? "yes" : "no");
    ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb); if (err) ID3D10Blob_Release(err);
    ID3D11Buffer_Release(cb); ID3D11Buffer_Release(vb); ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(staging); ID3D11Texture2D_Release(rt);
    ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev);
    printf("ok\n");
    return 0;
}
