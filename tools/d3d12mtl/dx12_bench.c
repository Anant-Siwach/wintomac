// D3D12 CPU-overhead micro-benchmark for the Metal backend spike (docs/d3d12_metal.md, "Overhead"). Windows program,
// recompiled by wintomac and run on both backends (make dx12mtl-bench):
//   per frame: clear, then DRAWS draws of the dx12_offscreen triangle, each with new root constants, into a
//   256x256 target; ExecuteCommandLists + Signal, and wait for the fence (so GPU time is included in "frame").
// Prints the median CPU time to record + submit a frame, the median frame time and a hash of the last frame.
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <d3d12.h>
#include "tri12_vs.h"
#include "tri12_ps.h"

#define W 256
#define H 256
#define FRAMES 120
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08x\n", #x, (unsigned)hr_); return 1; } } while (0)
typedef struct { float x, y, r, g, b, a; } Vtx;

static double ms(LARGE_INTEGER a, LARGE_INTEGER b) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
    int draws = argc > 1 ? atoi(argv[1]) : 2000;
    ID3D12Device *dev; CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *q; CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    ID3D12CommandAllocator *alloc; CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    ID3D12Resource *rt; CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rh; CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rh));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rh);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    static const Vtx tri[3] = { { 0.0f, 0.1f, 1, 0, 0, 1 }, { 0.1f, -0.1f, 0, 1, 0, 1 }, { -0.1f, -0.1f, 0, 0, 1, 1 } };
    D3D12_HEAP_PROPERTIES up = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    ID3D12Resource *vb; CHECK(ID3D12Device_CreateCommittedResource(dev, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&vb));
    void *p; CHECK(ID3D12Resource_Map(vb, 0, NULL, &p)); memcpy(p, tri, sizeof tri); ID3D12Resource_Unmap(vb, 0, NULL);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof tri, sizeof(Vtx) };
    D3D12_ROOT_PARAMETER rp = { D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS };
    rp.Constants.Num32BitValues = 8; rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3D10Blob *blob; CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL));
    ID3D12RootSignature *rs; CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    D3D12_INPUT_ELEMENT_DESC ied[2] = { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                                        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs; pd.VS.pShaderBytecode = tri12_vs; pd.VS.BytecodeLength = sizeof tri12_vs;
    pd.PS.pShaderBytecode = tri12_ps; pd.PS.BytecodeLength = sizeof tri12_ps;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.InputLayout.pInputElementDescs = ied; pd.InputLayout.NumElements = 2;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    ID3D12PipelineState *pso; CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
    QueryPerformanceCounter(&t1);
    printf("PSO creation: %.2f ms\n", ms(t0, t1));
    ID3D12GraphicsCommandList *cl; CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    CHECK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12Fence *fence; CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    double cpu[FRAMES], frame[FRAMES];
    for (int f = 0; f < FRAMES; f++) {
        QueryPerformanceCounter(&t0);
        CHECK(ID3D12CommandAllocator_Reset(alloc)); CHECK(ID3D12GraphicsCommandList_Reset(cl, alloc, pso));
        const float clear[4] = { 0.05f, 0.05f, 0.1f, 1.0f };
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, clear, 0, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
        D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 }; D3D12_RECT sc = { 0, 0, W, H };
        ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp); ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &vbv);
        for (int i = 0; i < draws; i++) {
            float a = (float)i * 0.01f + (float)f * 0.001f;
            float c[8] = { 1.0f - a * a * 0.5f, a, 0, 0, (float)(i & 7) / 7.0f, 0.5f, 1.0f, 1.0f };
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 8, c, 0);
            ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
        }
        CHECK(ID3D12GraphicsCommandList_Close(cl));
        ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
        CHECK(ID3D12CommandQueue_Signal(q, fence, f + 1));
        QueryPerformanceCounter(&t1);
        CHECK(ID3D12Fence_SetEventOnCompletion(fence, f + 1, ev));
        WaitForSingleObject(ev, 10000);
        QueryPerformanceCounter(&t2);
        cpu[f] = ms(t0, t1); frame[f] = ms(t0, t2);
    }
    qsort(cpu, FRAMES, sizeof *cpu, cmpd); qsort(frame, FRAMES, sizeof *frame, cmpd);
    printf("%d draws/frame, %d frames: record+submit median %.3f ms (%.0f ns/draw), frame median %.3f ms\n",
           draws, FRAMES, cpu[FRAMES / 2], cpu[FRAMES / 2] * 1e6 / draws, frame[FRAMES / 2]);
    return 0;
}
