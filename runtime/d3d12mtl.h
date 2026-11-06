// D3D12 -> Metal backend (WINTOMAC_D3D12_BACKEND=metal): shared between runtime/d3d12mtl.m (the ID3D12* objects
// on Metal) and runtime/d3d12mtl_shader.cpp (DXBC/DXIL -> SPIR-V -> MSL). Design: docs/d3d12_metal.md.
//
// Binding model (per shader stage, Metal buffer argument table):
//   [[buffer(2)]]        root argument block: the root signature's parameters laid out like Metal Shader Converter's
//                        top-level argument buffer (root constants inline, 8-byte GPU addresses for root descriptors
//                        and descriptor tables). Root constants are read from it directly (push-constant block).
//   [[buffer(3 + k)]]    k-th non-constant root parameter: a descriptor table is the shader-visible heap's MTLBuffer at
//                        the table's offset, read as a Tier 2 argument buffer of 24-byte entries (MSC's
//                        IRDescriptorTableEntry: GPU address / MTLResourceID / metadata); a root CBV is the buffer.
//   [[buffer(16 + slot)]] vertex buffers (dynamic stride).
// Static samplers become constexpr samplers in the MSL.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vkd3d_shader.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    D3D12MTL_BUF_ROOT = 2,         // root argument block
    D3D12MTL_BUF_PARAM0 = 3,       // first non-constant root parameter
    D3D12MTL_MAX_PARAM_SLOTS = 12, // 3..14
    D3D12MTL_BUF_VB0 = 16,         // vertex buffer slot 0
    D3D12MTL_MAX_VB = 15,          // 16..30
    D3D12MTL_ENTRY_SIZE = 24,      // CBV_SRV_UAV / sampler heap entry (IRDescriptorTableEntry layout)
    D3D12MTL_MAX_ROOT_PARAMS = 64,
};
// Fields of a heap entry, in 8-byte slots: buffers (CBV, raw/structured views) and samplers put their GPU address /
// MTLResourceID in slot 0, textures (and typed buffer views, which are texture buffers) their MTLResourceID in slot 1.
enum { D3D12MTL_SLOT_ADDR = 0, D3D12MTL_SLOT_TEXTURE = 1, D3D12MTL_SLOT_META = 2 };

typedef struct {
    uint32_t count, size;          // parameters, bytes of the root argument block
    struct { uint32_t offset; int32_t slot; } param[D3D12MTL_MAX_ROOT_PARAMS];   // slot: Metal buffer index or -1 (constants)
} D3D12MtlRootLayout;
// Root argument block layout and buffer slots. Returns 0 if the root signature exceeds the prototype's limits.
int d3d12mtl_root_layout(const struct vkd3d_shader_root_signature_desc1 *rs, D3D12MtlRootLayout *out, char *err, size_t errn);

typedef struct {
    enum vkd3d_shader_visibility stage;           // VERTEX, PIXEL or COMPUTE
    const void *code; size_t size;                // DXBC (TPF or DXIL) container
    const struct vkd3d_shader_root_signature_desc1 *rs;
    const D3D12MtlRootLayout *layout;
    const struct vkd3d_shader_signature *next_input;   // vertex shader: the pixel shader's input signature (varyings)
    int points;                                   // vertex shader of a point-list PSO: keep [[point_size]]
} D3D12MtlShaderIn;
// MSL source (malloc'd) with entry point "main0", or NULL with a message in err.
char *d3d12mtl_translate(const D3D12MtlShaderIn *in, char *err, size_t errn);
// Shader container helpers.
int d3d12mtl_is_dxil(const void *code, size_t size);

#ifdef __cplusplus
}
#endif
