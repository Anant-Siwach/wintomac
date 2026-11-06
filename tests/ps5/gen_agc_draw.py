"""Generate tests/ps5/app0/agc_draw.bin (+ agc_draw.expect.json): the GPU work tests/ps5/agc_draw.c submits
(docs/ps5_graphics.md).

One raw AGC command buffer (PM4: SET_UCONFIG_REG / SET_CONTEXT_REG / SET_SH_REG / DRAW_INDEX_AUTO / DISPATCH_DIRECT /
WRITE_DATA) and RDNA2 (gfx10.3) shader machine code, the way PS5 titles hand work to libSceAgcDriver; AnyPS5
recompiles the shaders to SPIR-V and runs them on Vulkan. Register setup follows AnyPS5's own driver tests
(core/libs/prx/libSceAgcDriver/tests/execution: DrawCacheReuse, VertexUserDataTail, ImageSampleSharedSampler,
DepthFastClearSubmit) and its register decoders (Graphics/src/State.cpp). Shader encodings were produced with
`llvm-mc -arch=amdgcn -mcpu=gfx1030 -show-encoding` (Homebrew LLVM); when llvm-mc is installed this script re-assembles
the sources and checks the stored bytes.

Scene on the 640x360 RGBA8 display buffer (depth: a 640x360 Z32 surface in the data area):
  bg     full-screen triangle, dark blue, depth ALWAYS + write at z 0.9
  vbuf   vertex buffer quad (6 vertices, position + colour per vertex), colours interpolated  x 20..220,  y 20..140
  tex    vertex buffer quad with UVs, image_sample_lz of a 64x64 RGBA8 checker (point sampler)  x 240..368, y 20..148
  mips   4 rects, image_sample_l at LOD 0..3 of the mipmapped texture (one solid colour per mip)  y 170..210
  null   rect sampling an all-zero T# (null descriptor): black                                 x 280..340, y 170..210
  depth  red rect z 0.3 then yellow rect z 0.6, both LESS: the overlap stays red
  cs     compute dispatch, 4 groups x 64 threads: out[i] = i*i + 0x1234 (checked by hash)
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

W, H = 640, 360
f32 = lambda v: struct.unpack("<I", struct.pack("<f", v))[0]
ENDPGM = 0xBF810000

# ---- shaders: (assembly, little-endian bytes) ------------------------------------------------------------------------
SHADERS = {
    # v5 = vertex index f; position (4f(2-f)-1, 2f(f-1)-1, 0, 1): (-1,-1), (3,-1), (-1,3) covers the viewport
    "vs_full": ("""v_cvt_f32_u32 v0, v5
v_sub_f32 v1, 2.0, v0
v_mul_f32 v1, v0, v1
v_mul_f32 v1, 4.0, v1
v_add_f32 v1, -1.0, v1
v_add_f32 v2, -1.0, v0
v_mul_f32 v2, v0, v2
v_mul_f32 v2, 2.0, v2
v_add_f32 v2, -1.0, v2
v_mov_b32 v3, 0
v_mov_b32 v4, 1.0
exp pos0 v1, v2, v3, v4 done
s_endpgm""", "05 0d 00 7e f4 00 02 08 00 03 02 10 f6 02 02 10 f3 02 02 06 f3 00 04 06 00 05 04 10 f4 04 04 10 f3 04 04 06"
             " 80 02 06 7e f2 02 08 7e cf 08 00 f8 01 02 03 04 00 00 81 bf"),
    # AnyPS5's vertex stage (gfx10 merged ES/GS ABI): user SGPRs start at s8, the vertex index is in v5
    # vertex buffer V# in s[8:11]: position xyzw only (AnyPS5 tests/execution/DepthFastClearHarness.hpp VertexCode)
    "vs_vb_pos": ("""buffer_load_dwordx4 v[0:3], v5, s[8:11], 0 idxen
s_waitcnt vmcnt(0)
exp pos0 v0, v1, v2, v3 done
s_endpgm""", "00 20 38 e0 05 00 02 80 70 3f 8c bf cf 08 00 f8 00 01 02 03 00 00 81 bf"),
    # vertex buffer V# in s[8:11] (stride 32): position xyzw, attribute xyzw -> param0
    "vs_vb": ("""buffer_load_dwordx4 v[6:9], v5, s[8:11], 0 idxen
buffer_load_dwordx4 v[10:13], v5, s[8:11], 0 idxen offset:16
s_waitcnt vmcnt(0)
exp param0 v10, v11, v12, v13
exp pos0 v6, v7, v8, v9 done
s_endpgm""", "00 20 38 e0 05 06 02 80 10 20 38 e0 05 0a 02 80 70 3f 8c bf 0f 02 00 f8 0a 0b 0c 0d cf 08 00 f8 06 07 08 09"
             " 00 00 81 bf"),
    # colour = (s0, s1, s2, 1)
    "ps_color": ("""v_mov_b32 v0, s0
v_mov_b32 v1, s1
v_mov_b32 v2, s2
v_mov_b32 v3, 1.0
exp mrt0 v0, v1, v2, v3 done vm
s_endpgm""", "00 02 00 7e 01 02 02 7e 02 02 04 7e f2 02 06 7e 0f 18 00 f8 00 01 02 03 00 00 81 bf"),
    # vs_full position; param0 = buffer V# s[8:11] dwords 0..3 (a buffer read in the vertex stage)
    "vs_full_bufparam": ("""v_mov_b32 v10, 0
buffer_load_dwordx4 v[6:9], v10, s[8:11], 0 offen
v_cvt_f32_u32 v0, v5
v_sub_f32 v1, 2.0, v0
v_mul_f32 v1, v0, v1
v_mul_f32 v1, 4.0, v1
v_add_f32 v1, -1.0, v1
v_add_f32 v2, -1.0, v0
v_mul_f32 v2, v0, v2
v_mul_f32 v2, 2.0, v2
v_add_f32 v2, -1.0, v2
v_mov_b32 v3, 0
v_mov_b32 v4, 1.0
s_waitcnt vmcnt(0)
exp param0 v6, v7, v8, v9
exp pos0 v1, v2, v3, v4 done
s_endpgm""", "80 02 14 7e 00 10 38 e0 0a 06 02 80 05 0d 00 7e f4 00 02 08 00 03 02 10 f6 02 02 10 f3 02 02 06 f3 00 04 06"
             " 00 05 04 10 f4 04 04 10 f3 04 04 06 80 02 06 7e f2 02 08 7e 70 3f 8c bf 0f 02 00 f8 06 07 08 09 cf 08 00 f8"
             " 01 02 03 04 00 00 81 bf"),
    # vs_full position; param0 = constant (1, 0, 1, 1)
    "vs_full_constparam": ("""v_cvt_f32_u32 v0, v5
v_sub_f32 v1, 2.0, v0
v_mul_f32 v1, v0, v1
v_mul_f32 v1, 4.0, v1
v_add_f32 v1, -1.0, v1
v_add_f32 v2, -1.0, v0
v_mul_f32 v2, v0, v2
v_mul_f32 v2, 2.0, v2
v_add_f32 v2, -1.0, v2
v_mov_b32 v3, 0
v_mov_b32 v4, 1.0
exp param0 v4, v3, v4, v4
exp pos0 v1, v2, v3, v4 done
s_endpgm""", "05 0d 00 7e f4 00 02 08 00 03 02 10 f6 02 02 10 f3 02 02 06 f3 00 04 06 00 05 04 10 f4 04 04 10 f3 04 04 06"
             " 80 02 06 7e f2 02 08 7e 0f 02 00 f8 04 03 04 04 cf 08 00 f8 01 02 03 04 00 00 81 bf"),
    # vs_full position; param0 = (s8, s9, s10, 1) from user SGPRs
    "vs_full_userparam": ("""v_mov_b32 v6, s8
v_mov_b32 v7, s9
v_mov_b32 v8, s10
v_mov_b32 v9, 1.0
v_cvt_f32_u32 v0, v5
v_sub_f32 v1, 2.0, v0
v_mul_f32 v1, v0, v1
v_mul_f32 v1, 4.0, v1
v_add_f32 v1, -1.0, v1
v_add_f32 v2, -1.0, v0
v_mul_f32 v2, v0, v2
v_mul_f32 v2, 2.0, v2
v_add_f32 v2, -1.0, v2
v_mov_b32 v3, 0
v_mov_b32 v4, 1.0
exp param0 v6, v7, v8, v9
exp pos0 v1, v2, v3, v4 done
s_endpgm""", "08 02 0c 7e 09 02 0e 7e 0a 02 10 7e f2 02 12 7e 05 0d 00 7e f4 00 02 08 00 03 02 10 f6 02 02 10 f3 02 02 06"
             " f3 00 04 06 00 05 04 10 f4 04 04 10 f3 04 04 06 80 02 06 7e f2 02 08 7e 0f 02 00 f8 06 07 08 09 cf 08 00 f8"
             " 01 02 03 04 00 00 81 bf"),
    # colour = buffer V# s[0:3] dwords 0..3 (a buffer read in the pixel stage)
    "ps_buf": ("""v_mov_b32 v0, 0
buffer_load_dwordx4 v[4:7], v0, s[0:3], 0 offen
s_waitcnt vmcnt(0)
exp mrt0 v4, v5, v6, v7 done vm
s_endpgm""", "80 02 00 7e 00 10 38 e0 00 04 00 80 70 3f 8c bf 0f 18 00 f8 04 05 06 07 00 00 81 bf"),
    # colour = interpolated param0
    "ps_attr": ("""v_interp_p1_f32 v2, v0, attr0.x
v_interp_p2_f32 v2, v1, attr0.x
v_interp_p1_f32 v3, v0, attr0.y
v_interp_p2_f32 v3, v1, attr0.y
v_interp_p1_f32 v4, v0, attr0.z
v_interp_p2_f32 v4, v1, attr0.z
v_interp_p1_f32 v5, v0, attr0.w
v_interp_p2_f32 v5, v1, attr0.w
exp mrt0 v2, v3, v4, v5 done vm
s_endpgm""", "00 00 08 c8 01 00 09 c8 00 01 0c c8 01 01 0d c8 00 02 10 c8 01 02 11 c8 00 03 14 c8 01 03 15 c8"
             " 0f 18 00 f8 02 03 04 05 00 00 81 bf"),
    # colour = texture(T# s[0:7], S# s[8:11]) at interpolated param0.xy, LOD 0
    "ps_tex_uv": ("""v_interp_p1_f32 v2, v0, attr0.x
v_interp_p2_f32 v2, v1, attr0.x
v_interp_p1_f32 v3, v0, attr0.y
v_interp_p2_f32 v3, v1, attr0.y
image_sample_lz v[4:7], v[2:3], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
exp mrt0 v4, v5, v6, v7 done vm
s_endpgm""", "00 00 08 c8 01 00 09 c8 00 01 0c c8 01 01 0d c8 08 0f 9c f0 02 04 40 00 70 3f 8c bf 0f 18 00 f8 04 05 06 07"
             " 00 00 81 bf"),
    # colour = texture(T# s[0:7], S# s[8:11]) at (0.5, 0.5), LOD s12
    "ps_tex_lod": ("""v_mov_b32 v0, 0.5
v_mov_b32 v1, 0.5
v_mov_b32 v2, s12
image_sample_l v[4:7], v[0:2], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
exp mrt0 v4, v5, v6, v7 done vm
s_endpgm""", "f0 02 00 7e f0 02 02 7e 0c 02 04 7e 08 0f 90 f0 00 04 40 00 70 3f 8c bf 0f 18 00 f8 04 05 06 07 00 00 81 bf"),
    # out V# s[0:3], group id s4: out[g*64 + t] = (g*64+t)^2 + 0x1234
    "cs": ("""s_lshl_b32 s5, s4, 6
v_add_nc_u32 v1, s5, v0
v_mul_lo_u32 v2, v1, v1
v_add_nc_u32 v2, 0x1234, v2
v_lshlrev_b32 v3, 2, v1
buffer_store_dword v2, v3, s[0:3], 0 offen
s_endpgm""", "04 86 05 8f 05 00 02 4a 02 00 69 d5 01 03 02 02 ff 04 04 4a 34 12 00 00 82 02 06 34 00 10 70 e0 03 02 00 80"
             " 00 00 81 bf"),
}


def shader_words(name):
    src, hexbytes = SHADERS[name]
    b = bytes(int(x, 16) for x in hexbytes.split())
    mc = shutil.which("llvm-mc") or "/opt/homebrew/opt/llvm/bin/llvm-mc"
    if os.path.exists(mc):
        with tempfile.NamedTemporaryFile("w", suffix=".s", delete=False) as fh:
            fh.write(src + "\n")
        out = subprocess.run([mc, "-arch=amdgcn", "-mcpu=gfx1030", "-show-encoding", fh.name], capture_output=True, text=True).stdout
        os.unlink(fh.name)
        enc = bytes(int(x, 16) for line in out.splitlines() if "encoding:" in line
                    for x in line.split("encoding: [")[1].rstrip("]").split(","))
        assert enc == b, "%s: stored bytes differ from llvm-mc" % name
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


# ---- data area -------------------------------------------------------------------------------------------------------
data = []          # words
relocs = []        # (where, base, offset, shape) ; where < 0 means data index (fixed up at the end)
DATA, FB, LABEL = 0, 1, 2


def shape(shift, nbits, pos=0):
    return shift | pos << 8 | nbits << 16


def align(n):
    while len(data) * 4 % n:
        data.append(0)


def put(words, alignment=256):
    align(alignment)
    off = len(data) * 4
    data.extend(words)
    return off


code = {name: put(shader_words(name)) for name in SHADERS}

# mipmapped texture, 64x64 RGBA8 linear, 4 levels; AnyPS5's linear layout (Graphics/src/TextureTiling.cpp
# ComputeLinearMipLayout): rows padded to 256 bytes, smallest level first.
TEX = 64
LEVELS = 4
MIP_COLORS = [None, (255, 0, 0), (0, 255, 0), (255, 0, 255)]   # level 0 is the checker


def checker(x, y):
    return (255, 255, 255) if ((x // 8) + (y // 8)) % 2 == 0 else (0, 128, 255)


def rgba(c):
    return c[0] | c[1] << 8 | c[2] << 16 | 0xFF << 24


align(65536)
tex_base = len(data) * 4
for level in reversed(range(LEVELS)):
    s = TEX >> level
    pitch = max(64, s)               # texels per padded row (256 bytes / 4)
    for y in range(s):
        row = [rgba(checker(x, y) if level == 0 else MIP_COLORS[level]) for x in range(s)]
        data.extend(row + [0] * (pitch - s))
tex_bytes = len(data) * 4 - tex_base


def ndc(x, y):
    return x / (W / 2) - 1, 1 - y / (H / 2)


def quad(x0, y0, x1, y1, attrs):   # attrs: 4 tuples (top-left, top-right, bottom-left, bottom-right)
    pts = [(x0, y0, attrs[0]), (x1, y0, attrs[1]), (x0, y1, attrs[2]), (x1, y0, attrs[1]), (x1, y1, attrs[3]), (x0, y1, attrs[2])]
    words = []
    for x, y, a in pts:
        nx, ny = ndc(x, y)
        words += [f32(nx), f32(ny), f32(0.5), f32(1.0)] + [f32(v) for v in a]
    return words


VB_COLORS = [(1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 1), (1, 1, 1, 1)]
vb_color = put(quad(20, 20, 220, 140, VB_COLORS))
buf_color = put([f32(0.0), f32(0.5), f32(1.0), f32(1.0)])
vb_tex = put(quad(240, 20, 368, 148, [(0, 0, 0, 0), (1, 0, 0, 0), (0, 1, 0, 0), (1, 1, 0, 0)]))
file_words = len(data)
align(65536)
cs_out = len(data) * 4
CS_GROUPS, CS_THREADS = 4, 64
total = cs_out + CS_GROUPS * CS_THREADS * 4
total = (total + 65535) & ~65535
depth_base = total
total += (W * H * 4 + 65535) & ~65535

# ---- command buffer --------------------------------------------------------------------------------------------------
dcb = []


def packet(op, payload, rel=()):
    dcb.append(0xC0000000 | ((len(payload) - 1) << 16) | (op << 8))
    base = len(dcb)
    dcb.extend(payload)
    for i, b, off, shp in rel:
        relocs.append((base + i, b, off, shp))


def ctx(reg, val, rel=()):
    packet(0x69, [reg, val], [(1,) + r for r in rel])


def sh(reg, val, rel=()):
    packet(0x76, [reg, val], [(1,) + r for r in rel])


def sh_words(reg, words, rels):   # rels: (word index, base, offset, shape)
    packet(0x76, [reg] + words, [(1 + i, b, o, s) for i, b, o, s in rels])


def program(lo, name):
    sh(lo, 0, [(DATA, code[name], shape(8, 32))])
    sh(lo + 1, 0, [(DATA, code[name], shape(40, 8))])


def vsharp(off, stride, records, dword3=0x01016FAC):
    return [0, stride << 16, records, dword3], [(0, DATA, off, shape(0, 32)), (1, DATA, off, shape(32, 16))]


def tsharp(off, levels):
    w = h = TEX
    # word3 LAST_LEVEL and word5 MAX_MIP (AnyPS5 Graphics/src/GuestTextureResource.cpp DecodeTextureResource)
    words = [0, 56 << 20 | ((w - 1) & 3) << 30, ((w - 1) >> 2) | (h - 1) << 14, 0xFAC | (levels - 1) << 16 | 9 << 28, 0, (levels - 1) << 4, 0, 0]
    return words, [(0, DATA, off, shape(8, 32)), (1, DATA, off, shape(40, 8))]


POINT_SAMPLER = [0x00000092, 0x00FFF000, 0x04000000, 0]   # clamp, LOD 0..15, point mag/min/mip


def viewport(x0, y0, x1, y1, z):
    xs, ys = (x1 - x0) / 2, (y1 - y0) / 2
    for reg, v in zip(range(0x10F, 0x115), (xs, x0 + xs, -ys, y0 + ys, 0.0, z)):
        ctx(reg, f32(v))


packet(0x79, [0x242, 4])  # VGT_PRIMITIVE_TYPE = triangle list
for reg, val in [
    (0x2D5, 0x2000), (0x1B6, 0), (0x1B3, 2), (0x1B4, 2), (0x191, 0), (0x207, 0), (0x203, 0x800),
    (0x2DC, 0xAA00), (0x2F8, 0), (0x292, 2), (0x293, 0),
    (0x80, 0), (0x8D, 0), (0x83, 0xFFFF), (0x8C, 0xA),
    (0x2F9, 0x2D), (0x313, 0x6000), (0x30E, 0xFFFFFFFF), (0x30F, 0xFFFFFFFF),
    (0x206, 0x43F), (0x204, 0x80000), (0x205, 0x240),
    (0x8E, 0xF), (0x8F, 0xF), (0x202, 0xCC0010),
    (0x1C4, 0), (0x1C5, 9), (0x1C3, 4), (0x31C, 0x28028),       # CB_COLOR0_INFO: 8_8_8_8 UNORM, standard swap
    (0x31B, 0), (0x31D, 0), (0x3B0, ((W - 1) << 14) | (H - 1)),  # CB_COLOR0_ATTRIB2
    (0x3B8, 0x9000000), (0x1E0, 0),
    (0xC, 0), (0xD, (H << 16) | W), (0x81, 0x80000000), (0x82, (H << 16) | W),
    (0x90, 0x80000000), (0x91, (H << 16) | W), (0x94, 0x80000000), (0x95, (H << 16) | W),
    (0xB4, 0), (0xB5, f32(1.0)), (0x2FE, 0), (0x302, 0), (0x306, 0), (0x30A, 0),
    # depth: Z32_FLOAT surface (DB_Z_INFO format 3), no stencil, W x H
    (0x000, 0), (0x002, 0), (0x007, (W - 1) | (H - 1) << 16), (0x010, 3), (0x011, 0),
]:
    ctx(reg, val)
ctx(0x318, 0, [(FB, 0, shape(8, 32))]); ctx(0x390, 0, [(FB, 0, shape(40, 8))])       # CB_COLOR0_BASE(_EXT)
for lo, hi in ((0x012, 0x01A), (0x014, 0x01C)):                                     # DB_Z_READ/WRITE_BASE(_HI)
    ctx(lo, 0, [(DATA, depth_base, shape(8, 32))]); ctx(hi, 0, [(DATA, depth_base, shape(40, 8))])

DEPTH_ALWAYS, DEPTH_LESS = 0x2 | 0x4 | 7 << 4, 0x2 | 0x4 | 1 << 4


def draw(vs, ps, ps_user=(), ps_rel=(), vs_user=(), vs_rel=(), vp=(0, 0, W, H, 0.9), depth=DEPTH_ALWAYS, vertices=3, interp=0):
    program(0x0C8, vs); sh(0x08B, len(vs_user) << 1)
    if vs_user: sh_words(0x08C, list(vs_user), vs_rel)
    program(0x008, ps); sh(0x00B, len(ps_user) << 1)
    if ps_user: sh_words(0x00C, list(ps_user), ps_rel)
    ctx(0x1B6, interp)
    ctx(0x200, depth)
    viewport(*vp)
    packet(0x2D, [vertices, 2])  # DRAW_INDEX_AUTO


def color(c):
    return [f32(v) for v in c]


expect = {"points": [], "checks": []}


def point(x, y, rgb, tol=2):
    expect["points"].append([x, y, list(rgb), tol])


# bg
draw("vs_full", "ps_color", color((0.1, 0.2, 0.5)), depth=DEPTH_ALWAYS)
for x, y in ((5, 5), (630, 160), (10, 350), (230, 90), (600, 20)):
    point(x, y, (26, 51, 128))
# vbuf: per-vertex colours interpolated
v, vr = vsharp(vb_color, 32, 6)
draw("vs_vb", "ps_attr", vs_user=v, vs_rel=vr, vertices=6, interp=1)


def vb_expected(x, y):   # barycentric interpolation at the pixel centre (w = 1, so perspective = linear)
    u, v = (x + 0.5 - 20) / 200, (y + 0.5 - 20) / 120
    tl, tr, bl, br = VB_COLORS
    if u + v <= 1:
        c = [tl[k] * (1 - u - v) + tr[k] * u + bl[k] * v for k in range(3)]
    else:
        c = [br[k] * (u + v - 1) + tr[k] * (1 - v) + bl[k] * (1 - u) for k in range(3)]
    return tuple(round(255 * k) for k in c)


for px, py in ((21, 21), (218, 22), (22, 138), (218, 138), (60, 40), (150, 110), (110, 30)):
    point(px, py, vb_expected(px, py), 2)
# tex: checker mapped 2x (each texel 2x2 pixels)
v, vr = vsharp(vb_tex, 32, 6)
t, tr = tsharp(tex_base, LEVELS)
draw("vs_vb", "ps_tex_uv", ps_user=t + POINT_SAMPLER, ps_rel=tr, vs_user=v, vs_rel=vr, vertices=6, interp=1)
for px, py in ((241, 21), (259, 25), (257, 38), (300, 70), (367, 147), (330, 100), (275, 120)):
    point(px, py, checker((px - 240) // 2, (py - 20) // 2), 0)
# mips: LOD 0..3 at (0.5, 0.5)
for lod in range(LEVELS):
    x0 = 20 + 60 * lod
    draw("vs_full", "ps_tex_lod", ps_user=t + POINT_SAMPLER + [f32(float(lod))], ps_rel=tr, vp=(x0, 170, x0 + 50, 210, 0.9))
    point(x0 + 25, 190, checker(32, 32) if lod == 0 else MIP_COLORS[lod], 0)
# null descriptor: all-zero T#
draw("vs_full", "ps_tex_lod", ps_user=[0] * 8 + POINT_SAMPLER + [f32(0.0)], vp=(280, 170, 340, 210, 0.9))
point(310, 190, (0, 0, 0), 0)
# buffer read in the pixel shader: colour (0, 0.5, 1) from memory
v, vr = vsharp(buf_color, 0, 16, 0x31016FAC)
draw("vs_full", "ps_buf", ps_user=v, ps_rel=vr, vp=(350, 170, 390, 210, 0.9))
point(370, 190, (0, 128, 255), 1)
# buffer read in the vertex shader, passed to the pixel shader as an interpolated attribute
draw("vs_full_bufparam", "ps_attr", vs_user=v, vs_rel=vr, vp=(400, 300, 440, 340, 0.9), interp=1)
point(420, 320, (0, 128, 255), 1)
# constant attribute from the vertex shader, interpolated
draw("vs_full_constparam", "ps_attr", vp=(450, 300, 470, 340, 0.9), interp=1)
point(460, 320, (255, 0, 255), 1)
# vertex shader user data (SGPRs) to the pixel shader
draw("vs_full_userparam", "ps_attr", vs_user=color((1.0, 0.5, 0.0)), vp=(380, 300, 395, 340, 0.9), interp=1)
point(387, 320, (255, 128, 0), 1)
# depth: red at 0.3, then yellow at 0.6 behind it
draw("vs_full", "ps_color", color((1, 0, 0)), vp=(400, 170, 560, 290, 0.3), depth=DEPTH_LESS)
draw("vs_full", "ps_color", color((1, 1, 0)), vp=(480, 230, 620, 340, 0.6), depth=DEPTH_LESS)
point(420, 200, (255, 0, 0), 0); point(520, 260, (255, 0, 0), 0); point(600, 320, (255, 255, 0), 0); point(500, 330, (255, 255, 0), 0)

# compute: COMPUTE_PGM_LO/HI 0x20c/0x20d, NUM_THREAD_X/Y/Z 0x207-0x209, RSRC2 0x213 (4 user SGPRs, TGID_X_EN),
# USER_DATA 0x240.., DISPATCH_DIRECT (AnyPS5 tests/execution/DepthFastClearSubmit.cpp)
program(0x20C, "cs")
sh(0x207, CS_THREADS); sh(0x208, 1); sh(0x209, 1); sh(0x213, (4 << 1) | 0x80)
v, vr = vsharp(cs_out, 0, CS_GROUPS * CS_THREADS * 4, 0x31016FAC)
sh_words(0x240, v, vr)
packet(0x15, [CS_GROUPS, 1, 1, 0x41])
outw = [(i * i + 0x1234) & 0xFFFFFFFF for i in range(CS_GROUPS * CS_THREADS)]
h = 2166136261
for byte in struct.pack("<%dI" % len(outw), *outw):
    h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
expect["checks"].append({"name": "compute out[i] = i*i + 0x1234", "hash": "%08x" % h})

packet(0x37, [0x500 | (1 << 20), 0, 0, 1], [(1, LABEL, 0, shape(0, 32)), (2, LABEL, 0, shape(32, 32))])  # label = 1

# ---- write -----------------------------------------------------------------------------------------------------------
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "app0", "agc_draw.bin")
checks = [(DATA, cs_out, CS_GROUPS * CS_THREADS * 4)]
with open(out, "wb") as fh:
    fh.write(b"AGC2" + struct.pack("<5I", len(dcb), file_words, total, len(relocs), len(checks)))
    fh.write(struct.pack("<%dI" % len(dcb), *dcb))
    fh.write(struct.pack("<%dI" % file_words, *data[:file_words]))
    for r in relocs:
        fh.write(struct.pack("<4I", *r))
    for c in checks:
        fh.write(struct.pack("<3I", *c))
with open(os.path.splitext(out)[0] + ".expect.json", "w") as fh:
    json.dump(expect, fh, indent=1)
print("gen_agc_draw: %s: %d DCB dwords, %d KiB data (%d KiB in file), %d relocations, %d points, %d checks"
      % (out, len(dcb), total // 1024, file_words // 256, len(relocs), len(expect["points"]), len(checks)))
