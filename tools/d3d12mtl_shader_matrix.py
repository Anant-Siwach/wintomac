#!/usr/bin/env python3
"""Shader-path comparison for the D3D12 -> Metal spike (docs/d3d12_metal.md, "Shader translation").

For every test shader (SM 5 DXBC from vkd3d-compiler, SM 6.x DXIL from DXC) try:
  (b) vkd3d-shader's MSL backend   DXBC/DXIL -> MSL           third_party/vkd3d-msl (built with -DVKD3D_SHADER_UNSUPPORTED_MSL)
  (c) vkd3d-shader + SPIRV-Cross   DXBC/DXIL -> SPIR-V -> MSL third_party/vkd3d (live) + third_party/spirv-cross
and compile the MSL with Apple's Metal compiler (xcrun metal). Apple's Metal Shader Converter (a) needs an Apple
developer sign-in to download, so it is not run here (see the doc).

Usage: d3d12mtl_shader_matrix.py [--keep DIR]      prints a Markdown table
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DXC = os.path.join(ROOT, "third_party", "dxc", "build", "bin", "dxc")
VKD3D = os.path.join(ROOT, "third_party", "vkd3d", "install")
VKD3D_MSL = os.path.join(ROOT, "third_party", "vkd3d-msl", "install")
DXSPV = os.path.join(ROOT, "third_party", "dxil-spirv", "build", "dxil-spirv")   # vkd3d-proton's front end (MIT)
SPVC = os.path.join(ROOT, "third_party", "spirv-cross", "install", "bin", "spirv-cross")
SPV11 = os.path.join(ROOT, "build", "d3d12mtl_spv11")   # tools/d3d12mtl/spv11.c
SM6X = os.path.join(ROOT, "tests", "shaders", "sm6x")
GS = os.path.join(ROOT, "tests", "shaders", "gs")
MSL_STD = "metal3.1"   # macOS 14 / Apple7+ (M1): the floor we would ship for

# (label, source file, entry, dxc profile or None, sm5 profile or None, extra dxc flags, feature probed)
SHADERS = [
    ("tri11 vs", "tests/shaders/tri11.hlsl", "vs_main", "vs_6_0", "vs_5_0", [], "dx12_offscreen VS (cbuffer, IA)"),
    ("tri11 ps", "tests/shaders/tri11.hlsl", "ps_main", "ps_6_0", "ps_5_0", [], "dx12_offscreen PS"),
    ("HelloTriangle VS", "build/samples/HelloTriangle/shaders.hlsl", "VSMain", "vs_6_0", "vs_5_0", [], "MS sample"),
    ("HelloTriangle PS", "build/samples/HelloTriangle/shaders.hlsl", "PSMain", "ps_6_0", "ps_5_0", [], "MS sample"),
    ("gs ps_tex", "tests/shaders/gs/ps_tex.hlsl", "main", "ps_6_0", "ps_5_0", [], "Texture2D + sampler table"),
    ("gs ps_buf", "tests/shaders/gs/ps_buf.hlsl", "main", "ps_6_0", "ps_5_0", [], "typed buffer SRV"),
    ("gs ps_cube", "tests/shaders/gs/ps_cube.hlsl", "main", "ps_6_0", "ps_5_0", [], "TextureCube"),
    ("gs gs_pass", "tests/shaders/gs/gs_pass.hlsl", "main", "gs_6_0", "gs_5_0", [], "geometry shader"),
    ("wave60", "tests/shaders/sm6x/wave60.hlsl", "main", "cs_6_0", None, [], "SM6.0 wave ops"),
    ("int64", "tests/shaders/sm6x/int64.hlsl", "main", "cs_6_0", None, [], "SM6.0 int64"),
    ("quad_ps", "tests/shaders/sm6x/quad_ps.hlsl", "main", "ps_6_0", None, [], "SM6.0 quad ops"),
    ("wave_vs", "tests/shaders/sm6x/wave_vs.hlsl", "main", "vs_6_0", None, [], "wave ops in VS"),
    ("bary_ps", "tests/shaders/sm6x/bary_ps.hlsl", "main", "ps_6_1", None, [], "SM6.1 barycentrics"),
    ("viewid_vs", "tests/shaders/sm6x/viewid_vs.hlsl", "main", "vs_6_1", None, [], "SM6.1 SV_ViewID"),
    ("f16", "tests/shaders/sm6x/f16.hlsl", "main", "cs_6_2", None, ["-enable-16bit-types"], "SM6.2 16-bit types"),
    ("dot4", "tests/shaders/sm6x/dot4.hlsl", "main", "cs_6_4", None, ["-enable-16bit-types"], "SM6.4 packed dot"),
    ("wave65", "tests/shaders/sm6x/wave65.hlsl", "main", "cs_6_5", None, [], "SM6.5 WaveMatch/MultiPrefix"),
    ("mesh", "tests/shaders/sm6x/mesh.hlsl", "main", "ms_6_5", None, [], "SM6.5 mesh shader"),
    ("rayquery", "tests/shaders/sm6x/rayquery.hlsl", "main", "cs_6_5", None, [], "SM6.5 RayQuery"),
    ("atomic64", "tests/shaders/sm6x/atomic64.hlsl", "main", "cs_6_6", None, [], "SM6.6 64-bit atomics"),
    ("dynres", "tests/shaders/sm6x/dynres.hlsl", "main", "cs_6_6", None, [], "SM6.6 ResourceDescriptorHeap"),
    ("pack66", "tests/shaders/sm6x/pack66.hlsl", "main", "cs_6_6", None, ["-enable-16bit-types"], "SM6.6 pack/WaveSize"),
    ("csderiv", "tests/shaders/sm6x/csderiv.hlsl", "main", "cs_6_6", None, [], "SM6.6 CS derivatives"),
]


def run(cmd, env=None, cwd=None):
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=cwd)
    return p.returncode, (p.stdout + p.stderr).strip()


def first_error(log):
    for l in log.splitlines():
        if "This is unsupported" in l or "warning" in l.lower():
            continue
        if "error" in l.lower() or "fixme" in l.lower() or "unhandled" in l.lower():
            l = l.split("error:", 1)[-1].strip() if "error:" in l else l.strip()
            return l[:90]
    return log.splitlines()[0][:90] if log else "failed"


def metal(msl, tmp, name):
    rc, log = run(["xcrun", "metal", f"-std={MSL_STD}", "-fmodules-cache-path=" + os.path.join(tmp, "mc"), "-c", msl,
                   "-o", os.path.join(tmp, name + ".air")])
    return rc == 0, log


def try_b(src, tmp, name):
    env = dict(os.environ, DYLD_LIBRARY_PATH=os.path.join(VKD3D_MSL, "lib"))
    out = os.path.join(tmp, name + ".b.metal")
    rc, log = run([os.path.join(VKD3D_MSL, "bin", "vkd3d-compiler"), "-b", "msl", "-o", out, src], env=env)
    if rc:
        return "no: " + first_error(log)
    ok, log = metal(out, tmp, name + ".b")
    return "yes" if ok else "MSL invalid: " + first_error(log)


def try_c(src, tmp, name):
    env = dict(os.environ, DYLD_LIBRARY_PATH=os.path.join(VKD3D, "lib"))
    spv, out = os.path.join(tmp, name + ".spv"), os.path.join(tmp, name + ".c.metal")
    rc, log = run([SPV11, src, spv], env=env)   # Vulkan 1.1 environment, as the runtime compiles
    if rc:
        return "no (vkd3d): " + first_error(log)
    rc, log = run([SPVC, "--msl", "--msl-version", "30100", "--msl-argument-buffers", "--msl-argument-buffer-tier", "1",
                   "--output", out, spv])
    if rc:
        return "no (SPIRV-Cross): " + first_error(log)
    ok, log = metal(out, tmp, name + ".c")
    return "yes" if ok else "MSL invalid: " + first_error(log)


def try_d(src, tmp, name):
    spv, out = os.path.join(tmp, name + ".d.spv"), os.path.join(tmp, name + ".d.metal")
    extra = ["--bindless"] if "dynres" in name else []
    rc, log = run([DXSPV, src, "--output", spv, "--enable-shader-demote", *extra])
    if rc or not os.path.exists(spv):
        return "no (dxil-spirv): " + first_error(log)
    rc, log = run([SPVC, "--msl", "--msl-version", "30100", "--msl-argument-buffers", "--msl-argument-buffer-tier", "1",
                   "--msl-runtime-array-rich-descriptor", "--output", out, spv])
    if rc:
        return "no (SPIRV-Cross): " + first_error(log)
    ok, log = metal(out, tmp, name + ".d")
    return "yes" if ok else "MSL invalid: " + first_error(log)


def main():
    keep = sys.argv[sys.argv.index("--keep") + 1] if "--keep" in sys.argv else None
    tmp = keep or tempfile.mkdtemp()
    os.makedirs(tmp, exist_ok=True)
    rows = []
    for label, path, entry, p6, p5, flags, what in SHADERS:
        src = os.path.join(ROOT, path)
        if not os.path.exists(src):
            continue
        name = label.replace(" ", "_")
        for kind, prof in (("DXBC", p5), ("DXIL", p6)):
            if not prof:
                continue
            blob = os.path.join(tmp, f"{name}.{kind.lower()}")
            if kind == "DXIL":
                rc, log = run([DXC, "-nologo", "-T", prof, "-E", entry, *flags, "-Fo", blob, src], cwd=os.path.dirname(src))
            else:
                env = dict(os.environ, DYLD_LIBRARY_PATH=os.path.join(VKD3D, "lib"))
                rc, log = run([os.path.join(VKD3D, "bin", "vkd3d-compiler"), "-x", "hlsl", "-b", "dxbc-tpf", "-p", prof,
                               "-e", entry, "-o", blob, src], env=env)
            if rc:
                rows.append((label, kind, prof, what, "source didn't compile", "", "", ""))
                continue
            rows.append((label, kind, prof, what, try_b(blob, tmp, f"{name}.{kind}"), try_c(blob, tmp, f"{name}.{kind}"),
                         try_d(blob, tmp, f"{name}.{kind}"),
                         "not run (download needs Apple sign-in)" if kind == "DXIL" else "n/a (DXIL only)"))
    print("| shader | input | profile | probes | (b) vkd3d MSL backend | (c) vkd3d SPIR-V + SPIRV-Cross | (d) dxil-spirv + SPIRV-Cross | (a) Metal Shader Converter |")
    print("|---|---|---|---|---|---|---|---|")
    for r in rows:
        print("| " + " | ".join(x.replace("|", "/") for x in r) + " |")
    b, c, d = (sum(r[i] == "yes" for r in rows) for i in (4, 5, 6))
    print(f"\n{len(rows)} shader builds valid MSL (Metal compiler, -std={MSL_STD}): (b) {b}, (c) {c}, (d) {d}")


if __name__ == "__main__":
    main()
