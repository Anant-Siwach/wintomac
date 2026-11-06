#!/usr/bin/env python3
"""Build Microsoft's DirectX-Graphics-Samples D3D12HelloTriangle (MIT) as a Windows x64 exe with mingw, as a
real-program target for wintomac's d3d12.dll. Sources: third_party/dx-samples/src (sparse clone, read as data).

The copy in build/samples/HelloTriangle gets small edits (the originals are untouched):
  * portability: DirectX-Headers' d3d12.h/d3dx12.h (third_party/directx-headers) instead of the NuGet Agility
    SDK; WINVER 0x0601, so ReadDataFromFile uses CreateFile (wintomac has no CreateFile2 yet); the struct-returning descriptor-heap methods use mingw's hidden-return-pointer form; a FileHandle
    stand-in for WRL's (mingw lacks it); no <D3Dcompiler.h> (unused, clashes with DirectX-Headers);
  * headless: the window is never shown, 60 frames are rendered from the main loop (hidden windows get no
    WM_PAINT), then the last presented back buffer's centre and corner pixels are printed.
Shaders are compiled to DXIL with DXC (tools/build_dxc.sh) as the Visual Studio project does.

Usage: build_dx_samples.py            -> build/samples/HelloTriangle/D3D12HelloTriangle.exe (+ shaders_*.cso)
Then:  see docs/d3d12.md ("Real programs") for recompiling and running it.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "third_party", "dx-samples", "src", "Samples", "Desktop", "D3D12HelloWorld", "src", "HelloTriangle")
DH = os.path.join(ROOT, "third_party", "directx-headers", "src", "include")
DXC = os.path.join(ROOT, "third_party", "dxc", "build", "bin", "dxc")
OUT = os.path.join(ROOT, "build", "samples", "HelloTriangle")

COMPAT = r'''
// wintomac build: what MSVC's WRL / SDK provide and mingw lacks.
namespace Microsoft { namespace WRL { namespace Wrappers {
class FileHandle {
public:
    explicit FileHandle(HANDLE h) : h_(h) {}
    ~FileHandle() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    HANDLE Get() const { return h_; }
private:
    HANDLE h_;
};
}}}
#define _uuidof __uuidof
static inline D3D12_CPU_DESCRIPTOR_HANDLE wt_cpu_start(ID3D12DescriptorHeap *h) { D3D12_CPU_DESCRIPTOR_HANDLE r; h->GetCPUDescriptorHandleForHeapStart(&r); return r; }
'''

READBACK = r'''
// wintomac: read the last presented back buffer back and print a few pixels (headless check).
void D3D12HelloTriangle::OnCheck()
{
    UINT idx = (m_frameIndex + FrameCount - 1) % FrameCount;
    D3D12_RESOURCE_DESC d; m_renderTargets[idx]->GetDesc(&d);   // mingw: hidden return pointer
    UINT pitch = ((UINT)d.Width * 4 + 255) & ~255u;
    ComPtr<ID3D12Resource> rb;
    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_READBACK);
    CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer((UINT64)pitch * d.Height);
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)));
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[idx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE));
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = d.Format; dst.PlacedFootprint.Footprint.Width = (UINT)d.Width;
    dst.PlacedFootprint.Footprint.Height = d.Height; dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
    src.pResource = m_renderTargets[idx].Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[idx].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT));
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForPreviousFrame();
    UINT8 *p; D3D12_RANGE r = { 0, (SIZE_T)pitch * d.Height };
    ThrowIfFailed(rb->Map(0, &r, reinterpret_cast<void**>(&p)));
    UINT8 *c = p + (d.Height / 2) * pitch + ((UINT)d.Width / 2) * 4, *k = p + 4 * pitch + 4 * 4;
    printf("HelloTriangle: %llux%u, centre %u,%u,%u,%u corner %u,%u,%u,%u\n", (unsigned long long)d.Width, d.Height,
           c[0], c[1], c[2], c[3], k[0], k[1], k[2], k[3]);
    fflush(stdout);
    D3D12_RANGE none = { 0, 0 }; rb->Unmap(0, &none);
}
'''


def edit(path, old, new):
    s = open(path, encoding="utf-8-sig").read()
    if old not in s:
        raise SystemExit(f"{os.path.basename(path)}: expected text not found: {old[:60]!r}")
    open(path, "w", encoding="utf-8").write(s.replace(old, new))


def main():
    if not os.path.isdir(SRC):
        raise SystemExit("missing sources: run the sparse clone in docs/d3d12.md (third_party/dx-samples)")
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(os.path.join(OUT, "include", "d3dx12"))
    for f in os.listdir(SRC):
        if f.endswith((".cpp", ".h", ".hlsl")):
            shutil.copy(os.path.join(SRC, f), OUT)
    open(os.path.join(OUT, "include", "d3dx12", "d3dx12.h"), "w").write("#include <directx/d3dx12.h>\n")
    p = lambda f: os.path.join(OUT, f)
    edit(p("stdafx.h"), "#include <D3Dcompiler.h>", "// #include <D3Dcompiler.h>  (wintomac build: unused, clashes with DirectX-Headers)")
    edit(p("stdafx.h"), "#include <shellapi.h>", "#include <shellapi.h>\n#include <stdio.h>\n#include <dxguids/dxguids.h>   // __uuidof for DirectX-Headers' interfaces under mingw\n" + COMPAT)
    s = open(p("D3D12HelloTriangle.cpp"), encoding="utf-8-sig").read()
    s = s.replace("m_rtvHeap->GetCPUDescriptorHandleForHeapStart()", "wt_cpu_start(m_rtvHeap.Get())") + READBACK
    open(p("D3D12HelloTriangle.cpp"), "w").write(s)
    edit(p("D3D12HelloTriangle.h"), "    virtual void OnDestroy();", "    virtual void OnDestroy();\n    virtual void OnCheck();")
    edit(p("DXSample.h"), "    virtual void OnDestroy() = 0;", "    virtual void OnDestroy() = 0;\n    virtual void OnCheck() {}")
    edit(p("Win32Application.cpp"), "    ShowWindow(m_hwnd, nCmdShow);", "    ShowWindow(m_hwnd, SW_HIDE);   // wintomac: headless, never shown")
    edit(p("Win32Application.cpp"), """    while (msg.message != WM_QUIT)
    {
        // Process any messages in the queue.
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
""", """    // wintomac: 60 frames from the loop (a hidden window gets no WM_PAINT), then check the last one
    for (int frame = 0; frame < 60 && msg.message != WM_QUIT; frame++)
    {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        pSample->OnUpdate();
        pSample->OnRender();
    }
    pSample->OnCheck();
""")
    for entry, profile in (("VSMain", "vs_6_0"), ("PSMain", "ps_6_0")):
        subprocess.run([DXC, "-nologo", "-T", profile, "-E", entry, "-Fo", p(f"shaders_{entry}.cso"), p("shaders.hlsl")], check=True)
    objs = []
    for f in ("D3D12HelloTriangle", "DXSample", "Win32Application", "Main"):
        o = p(f + ".o")
        # -fpermissive: the samples take the address of temporaries (&CD3DX12_RESOURCE_BARRIER::Transition(...)), as MSVC allows
        subprocess.run(["x86_64-w64-mingw32-g++", "-std=c++17", "-O2", "-fpermissive", "-w", "-DWINVER=0x0601", "-D_WIN32_WINNT=0x0601", "-DUNICODE", "-D_UNICODE", "-I" + OUT,
                        "-I" + os.path.join(DH, "directx"), "-I" + DH, "-c", p(f + ".cpp"), "-o", o], check=True)
        objs.append(o)
    subprocess.run(["x86_64-w64-mingw32-g++", "-static", "-mwindows", "-o", p("D3D12HelloTriangle.exe"), *objs,
                    "-ld3d12", "-ldxgi", "-ldxguid"], check=True)
    print(f"[dx-samples] {p('D3D12HelloTriangle.exe')}")


if __name__ == "__main__":
    main()
