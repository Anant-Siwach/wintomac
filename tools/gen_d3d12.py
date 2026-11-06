#!/usr/bin/env python3
"""Generate guest-callable (Win64 ABI) COM thunks for Wine's libvkd3d D3D12 objects.

Output: runtime/gen/d3d12_thunks.h, included by runtime/d3d12.c.

Each D3D12 object the guest sees is a small wrapper { guest_vtbl*, host_obj*, magic } (see d3d12.c). For every
method of the most-derived interface libvkd3d implements, we emit one shim that
  * reads the Win64 arguments (rcx/rdx/r8/r9, xmm0-3 for floats, then the stack),
  * replaces wrapper pointers by the host objects (single pointers and `X *const *` arrays),
  * calls the host vtable,
  * wraps interface pointers coming back through `REFIID riid, void **out` and `ID3DBlob **`.

No struct translation is needed otherwise: D3D12 structs use fixed-width types, pointers and enums that have
the same layout on Windows x64 and arm64 macOS (vkd3d's LONG is 32-bit), guest memory is identity mapped, and
both sides use the WIDL "explicit aggregate return" convention (struct results go through a hidden pointer
right after `This`, as in MSVC's C++ ABI). Methods whose argument structs embed interface pointers
(barriers, copy locations, PSO descriptions, render passes) are written by hand in d3d12.c.

Usage: gen_d3d12.py <vkd3d include dir> [out]
"""
import os
import re
import sys

# kind name, most-derived interface libvkd3d 1.19 implements, base IID used to recognise host objects.
# The guest vtable is that interface's, plus GUEST_EXTRA.
KINDS = [
    ("Device", "ID3D12Device9", "ID3D12Device"),
    ("Queue", "ID3D12CommandQueue", "ID3D12CommandQueue"),
    ("Alloc", "ID3D12CommandAllocator", "ID3D12CommandAllocator"),
    ("List", "ID3D12GraphicsCommandList6", "ID3D12GraphicsCommandList"),
    ("Fence", "ID3D12Fence1", "ID3D12Fence"),
    ("Resource", "ID3D12Resource2", "ID3D12Resource"),
    ("Heap", "ID3D12Heap", "ID3D12Heap"),
    ("DescHeap", "ID3D12DescriptorHeap", "ID3D12DescriptorHeap"),
    ("QueryHeap", "ID3D12QueryHeap", "ID3D12QueryHeap"),
    ("RootSig", "ID3D12RootSignature", "ID3D12RootSignature"),
    ("PSO", "ID3D12PipelineState", "ID3D12PipelineState"),
    ("CmdSig", "ID3D12CommandSignature", "ID3D12CommandSignature"),
    ("RSDeser", "ID3D12RootSignatureDeserializer", "ID3D12RootSignatureDeserializer"),
    ("VRSDeser", "ID3D12VersionedRootSignatureDeserializer", "ID3D12VersionedRootSignatureDeserializer"),
    ("CacheSession", "ID3D12ShaderCacheSession", "ID3D12ShaderCacheSession"),
    ("Blob", "ID3D10Blob", "ID3D10Blob"),
]
# Methods of newer interfaces than libvkd3d implements, appended to the guest vtable (order as in Microsoft's
# DirectX-Headers d3d12.h): ID3D12Device10..14, ID3D12GraphicsCommandList7..10, ID3D12Heap1. All hand-written.
GUEST_EXTRA = {
    "Device": ["CreateCommittedResource3", "CreatePlacedResource2", "CreateReservedResource2",   # Device10
               "CreateSampler2",                                                                  # Device11
               "GetResourceAllocationInfo3",                                                      # Device12
               "OpenExistingHeapFromAddress1",                                                    # Device13
               "CreateRootSignatureFromSubobjectInLibrary"],                                      # Device14
    "List": ["Barrier",                                            # List7 (enhanced barriers)
             "OMSetFrontAndBackStencilRef",                        # List8
             "RSSetDepthBias", "IASetIndexBufferStripCutValue",    # List9
             "SetProgram", "DispatchGraph"],                       # List10 (work graphs)
    "Heap": ["GetProtectedResourceSession"],                       # Heap1
}
HAND_WRITTEN = {  # in runtime/d3d12.c
    "Device.CreateGraphicsPipelineState", "Device.CreateComputePipelineState", "Device.CreatePipelineState",
    "List.ResourceBarrier", "Queue.QueryInterface", "Device.CheckFeatureSupport", "List.CopyTextureRegion", "List.BeginRenderPass",
    # geometry shader / stream output fallbacks: draws with an unsupported PSO are skipped
    "List.SetPipelineState", "List.Reset", "List.ClearState", "List.DrawInstanced", "List.DrawIndexedInstanced",
    "List.ExecuteIndirect", "List.SetPredication",
    "Device.CreateSampler", "Device.CreateCommandQueue1",
}
BYVAL_HANDLES = {"D3D12_CPU_DESCRIPTOR_HANDLE", "D3D12_GPU_DESCRIPTOR_HANDLE"}   # 8-byte structs: in a GPR on both ABIs
U64_RET = {"UINT64", "SIZE_T", "D3D12_GPU_VIRTUAL_ADDRESS"}
IFACE = re.compile(r"^(?:const )?(ID3D\w+|IUnknown) \*$")
IFACE_ARRAY = re.compile(r"^(ID3D\w+) \*const \*$")


def parse_params(s):
    out = []
    for p in [x.strip() for x in s.split(",")]:
        m = re.match(r"^(.*?)(\w+)(\[\d+\])?$", p)
        typ, name, arr = m.group(1).strip(), m.group(2), m.group(3)
        if arr:
            typ = typ + " *"   # `const FLOAT color[4]` decays to a pointer
        out.append((re.sub(r"\s+", " ", typ).replace(" *", " *").strip(), name))
    return out


def arg(i):  # Win64 integer/pointer argument i (1-based, This = 1)
    return ["A1", "A2", "A3", "A4"][i - 1] if i <= 4 else f"AN({i})"


def float_arg(i):
    return f"XMM_ARG({i}).f[0]" if i <= 4 else f"d3d12_stack_float(AN({i}))"


def gen_method(kind, iface, name, ret, params, out):
    sig = f"__wintomac_D3D12{kind}_{name}"
    fn = f"D3D12{kind}_{name}"
    if name == "AddRef":
        out.append(f"SHIM({fn}) {{ d3d12_addref(c); }}")
        return fn
    if name == "Release":
        out.append(f"SHIM({fn}) {{ d3d12_release(c); }}")
        return fn
    if name == "QueryInterface":   # d3d12.c: newer interface IIDs, helper objects, logging of unknown IIDs
        out.append(f"SHIM({fn}) {{ d3d12_qi(c, \"{kind}\"); }}")
        return fn
    body, call, post = [], [], []
    void_ret = ret == "void"
    bad_ret = "0" if void_ret else ("E_INVALIDARG_" if ret == "HRESULT" else "0")
    body.append(f"    {iface} *h_ = d3d12_host(A1);")
    body.append("    int bad_ = 0; (void)bad_;")
    for i, (t, n) in enumerate(params[1:], start=2):
        a = arg(i)
        bt = t.replace("const ", "")
        if t in ("FLOAT", "float"):
            call.append(float_arg(i))
        elif bt in BYVAL_HANDLES:
            call.append(f"({bt}){{ {a} }}")
        elif IFACE_ARRAY.match(t):
            # count: the nearest UINT parameter whose name says it is a count
            cands = [(abs(j - i), pn) for j, (pt, pn) in enumerate(params[1:], start=2)
                     if pt == "UINT" and ("count" in pn.lower() or pn.lower().startswith("num"))]
            if not cands:
                raise SystemExit(f"{iface}::{name}: no count for array {n}")
            cnt = min(cands)[1]
            ci = [pn for _, pn in params].index(cnt) + 1
            body.append(f"    u32 n_{n} = (u32){arg(ci)}; void **a_{n} = {a} ? alloca(8 * (n_{n} + 1)) : NULL;")
            body.append(f"    if (a_{n}) for (u32 i_ = 0; i_ < n_{n}; i_++) a_{n}[i_] = d3d12_in(LD64({a} + 8 * i_), &bad_);")
            call.append(f"({t})a_{n}")
        elif IFACE.match(t):
            body.append(f"    void *p_{n} = d3d12_in({a}, &bad_);")
            call.append(f"({t})p_{n}")
        else:
            call.append(f"({t}){a}")
        # outputs that carry interface pointers back to the guest
        prev = params[i - 2][0] if i >= 2 else ""
        if (t == "void **" and prev == "REFIID") or t in ("ID3DBlob **", "ID3D10Blob **"):
            post.append(f"    if (SUCCEEDED(r_) && {a}) r_ = d3d12_out({a}, r_);")
    body.append(f"    if (bad_) {{ d3d12_bad_arg(\"{iface}::{name}\"); RET({bad_ret}); }}")
    body.append(f"    D3D12_TRACE(\"{kind}.{name}\");")
    invoke = f"h_->lpVtbl->{name}(h_{''.join(', ' + x for x in call)})"
    if void_ret:
        body.append(f"    {invoke};")
        body.append("    RET(0);")
    else:
        body.append(f"    {ret} r_ = {invoke};")
        if post and ret != "HRESULT":
            raise SystemExit(f"{iface}::{name}: interface output without HRESULT")
        body += post
        if ret in U64_RET or ret.endswith("*"):
            body.append("    RET((u64)r_);")
        else:
            body.append("    RET((u32)r_);")
    out.append(f"SHIM({fn}) {{\n" + "\n".join(body) + "\n}")
    return fn



def main(incdir, outpath):
    src = ""
    for f in ("vkd3d_d3d12.h", "vkd3d_d3dcommon.h"):
        src += open(os.path.join(incdir, f), encoding="utf-8").read()
    out = ["// Generated by tools/gen_d3d12.py from vkd3d's vkd3d_d3d12.h / vkd3d_d3dcommon.h. Do not edit.",
           "// Included by runtime/d3d12.c, which provides d3d12_host/d3d12_in/d3d12_out/d3d12_addref/d3d12_release.", ""]
    guids = {}
    for m in re.finditer(r"DEFINE_GUID\(IID_(\w+),\s*([^)]*)\)", src):
        guids.setdefault(m.group(1), m.group(2))
    tables, entries = [], []
    for kind, iface, base in KINDS:
        m = re.search(r"typedef struct %sVtbl \{(.*?)\} %sVtbl;" % (iface, iface), src, re.S)
        if not m:
            raise SystemExit(f"no vtable for {iface}")
        names = []
        for mm in re.finditer(r"([^;{}]*?)\s*\(STDMETHODCALLTYPE \*(\w+)\)\((.*?)\);", m.group(1), re.S):
            ret = re.sub(r"/\*.*?\*/|BEGIN_INTERFACE", "", mm.group(1), flags=re.S).strip()
            ret = re.sub(r"\s+", " ", ret)
            name, params = mm.group(2), parse_params(mm.group(3))
            names.append(name)
            if f"{kind}.{name}" in HAND_WRITTEN:
                continue
            entries.append(gen_method(kind, iface, name, ret, params, out))
        names += GUEST_EXTRA.get(kind, [])
        tables.append((kind, iface, base, names))
        out.append("")
    # IIDs used to recognise host objects
    for kind, iface, base, names in tables:
        g = guids[base]
        out.append(f"static const GUID WT_IID_{base} = {{ {g.split(',', 3)[0]}, {g.split(',', 3)[1].strip()}, {g.split(',', 3)[2].strip()}, {{ {g.split(',', 3)[3].strip()} }} }};")
    out.append("")
    for kind, iface, base, names in tables:
        out.append(f"static const char *const d3d12_methods_{kind}[] = {{")
        for i in range(0, len(names), 6):
            out.append("    " + " ".join(f'"{n}",' for n in names[i:i + 6]))
        out.append("    NULL };")
    out.append("")
    out.append("static D3D12Kind d3d12_kinds[] = {")
    for kind, iface, base, names in tables:
        out.append(f'    {{ "D3D12{kind}", "{iface}", &WT_IID_{base}, d3d12_methods_{kind}, NULL }},')
    out.append("};")
    out.append("")
    out.append("const Shim shims_d3d12_gen[] = {")
    for e in entries:
        out.append(f'    {{ NULL, "__wintomac_{e}", sh_{e} }},')
    out.append("    { 0, 0, 0 }\n};")
    with open(outpath, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"[gen_d3d12] {len(entries)} methods in {len(tables)} interfaces -> {outpath}")


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "runtime", "gen", "d3d12_thunks.h"))
