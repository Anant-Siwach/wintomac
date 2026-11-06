// D3D12 -> Metal backend, shader path: DXBC (SM 4/5) or DXIL (SM 6.x) -> SPIR-V with vkd3d-shader (the same front end
// the vkd3d path uses), then SPIR-V -> MSL with SPIRV-Cross, against the binding model in d3d12mtl.h. The MSL is
// compiled by MTLDevice at runtime (d3d12mtl.m), so no Metal toolchain is needed on the player's machine.
// Why this path and not Metal Shader Converter / vkd3d's MSL backend: docs/d3d12_metal.md.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <spirv_msl.hpp>
#include "d3d12mtl.h"

using namespace SPIRV_CROSS_NAMESPACE;

extern "C" int d3d12mtl_is_dxil(const void *code, size_t size) {
    const uint8_t *p = (const uint8_t *)code;
    if (!p || size < 32 || memcmp(p, "DXBC", 4)) return 0;
    uint32_t count; memcpy(&count, p + 28, 4);
    for (uint32_t i = 0; i < count && 32 + 4 * i + 4 <= size; i++) {
        uint32_t off; memcpy(&off, p + 32 + 4 * i, 4);
        if (off + 4 <= size && !memcmp(p + off, "DXIL", 4)) return 1;
    }
    return 0;
}

static int visible(enum vkd3d_shader_visibility v, enum vkd3d_shader_visibility stage) {
    return v == VKD3D_SHADER_VISIBILITY_ALL || v == stage;
}

extern "C" int d3d12mtl_root_layout(const struct vkd3d_shader_root_signature_desc1 *rs, D3D12MtlRootLayout *out, char *err, size_t errn) {
    memset(out, 0, sizeof *out);
    if (rs->parameter_count > D3D12MTL_MAX_ROOT_PARAMS) { snprintf(err, errn, "%u root parameters", rs->parameter_count); return 0; }
    uint32_t off = 0, slot = 0;
    out->count = rs->parameter_count;
    for (uint32_t i = 0; i < rs->parameter_count; i++) {
        const struct vkd3d_shader_root_parameter1 *p = &rs->parameters[i];
        // MSC-style top-level argument buffer: 8-byte aligned entries; constants inline, the rest 64-bit addresses.
        off = (off + 7) & ~7u;
        out->param[i].offset = off;
        if (p->parameter_type == VKD3D_SHADER_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            out->param[i].slot = -1;
            off += 4 * p->u.constants.value_count;
        } else {
            if (slot >= D3D12MTL_MAX_PARAM_SLOTS) { snprintf(err, errn, "more than %d non-constant root parameters", D3D12MTL_MAX_PARAM_SLOTS); return 0; }
            out->param[i].slot = D3D12MTL_BUF_PARAM0 + slot++;
            off += 8;
        }
    }
    out->size = (off + 15) & ~15u;
    if (!out->size) out->size = 16;
    return 1;
}

static uint32_t range_count(const struct vkd3d_shader_descriptor_range1 *r) {
    return r->descriptor_count == ~0u ? 256 : r->descriptor_count;   // unbounded ranges: prototype cap (see the doc)
}

// MSL constexpr sampler for a D3D12 static sampler.
static MSLSamplerAddress addr(enum vkd3d_shader_texture_address_mode m) {
    switch (m) {
    case VKD3D_SHADER_TEXTURE_ADDRESS_MODE_WRAP: return MSL_SAMPLER_ADDRESS_REPEAT;
    case VKD3D_SHADER_TEXTURE_ADDRESS_MODE_MIRROR: return MSL_SAMPLER_ADDRESS_MIRRORED_REPEAT;
    case VKD3D_SHADER_TEXTURE_ADDRESS_MODE_BORDER: return MSL_SAMPLER_ADDRESS_CLAMP_TO_BORDER;
    default: return MSL_SAMPLER_ADDRESS_CLAMP_TO_EDGE;   // CLAMP, MIRROR_ONCE (approximation)
    }
}
static MSLSamplerCompareFunc cmp(enum vkd3d_shader_comparison_func f) {
    switch (f) {
    case VKD3D_SHADER_COMPARISON_FUNC_NEVER: return MSL_SAMPLER_COMPARE_FUNC_NEVER;
    case VKD3D_SHADER_COMPARISON_FUNC_LESS: return MSL_SAMPLER_COMPARE_FUNC_LESS;
    case VKD3D_SHADER_COMPARISON_FUNC_EQUAL: return MSL_SAMPLER_COMPARE_FUNC_EQUAL;
    case VKD3D_SHADER_COMPARISON_FUNC_LESS_EQUAL: return MSL_SAMPLER_COMPARE_FUNC_LESS_EQUAL;
    case VKD3D_SHADER_COMPARISON_FUNC_GREATER: return MSL_SAMPLER_COMPARE_FUNC_GREATER;
    case VKD3D_SHADER_COMPARISON_FUNC_NOT_EQUAL: return MSL_SAMPLER_COMPARE_FUNC_NOT_EQUAL;
    case VKD3D_SHADER_COMPARISON_FUNC_GREATER_EQUAL: return MSL_SAMPLER_COMPARE_FUNC_GREATER_EQUAL;
    default: return MSL_SAMPLER_COMPARE_FUNC_ALWAYS;
    }
}
static MSLConstexprSampler static_sampler(const struct vkd3d_shader_static_sampler_desc *s) {
    MSLConstexprSampler c;
    unsigned f = s->filter;   // D3D12_FILTER bits: mip 0x1, mag 0x4, min 0x10, anisotropic 0x40, comparison 0x80
    c.mip_filter = (f & 0x1) ? MSL_SAMPLER_MIP_FILTER_LINEAR : MSL_SAMPLER_MIP_FILTER_NEAREST;
    c.mag_filter = (f & 0x4) ? MSL_SAMPLER_FILTER_LINEAR : MSL_SAMPLER_FILTER_NEAREST;
    c.min_filter = (f & 0x10) ? MSL_SAMPLER_FILTER_LINEAR : MSL_SAMPLER_FILTER_NEAREST;
    if (f & 0x40) {
        c.mip_filter = MSL_SAMPLER_MIP_FILTER_LINEAR; c.mag_filter = c.min_filter = MSL_SAMPLER_FILTER_LINEAR;
        c.anisotropy_enable = s->max_anisotropy > 1; c.max_anisotropy = s->max_anisotropy;
    }
    c.s_address = addr(s->address_u); c.t_address = addr(s->address_v); c.r_address = addr(s->address_w);
    c.compare_enable = (f & 0x80) != 0; c.compare_func = cmp(s->comparison_func);
    c.border_color = s->border_colour == VKD3D_SHADER_STATIC_BORDER_COLOUR_OPAQUE_WHITE ? MSL_SAMPLER_BORDER_COLOR_OPAQUE_WHITE
                   : s->border_colour == VKD3D_SHADER_STATIC_BORDER_COLOUR_OPAQUE_BLACK ? MSL_SAMPLER_BORDER_COLOR_OPAQUE_BLACK
                   : MSL_SAMPLER_BORDER_COLOR_TRANSPARENT_BLACK;
    c.lod_clamp_min = s->min_lod; c.lod_clamp_max = s->max_lod > 1000.0f ? 1000.0f : s->max_lod;
    c.lod_clamp_enable = s->min_lod > 0 || s->max_lod < 1000.0f;
    return c;
}

// Descriptor sets in the SPIR-V we ask vkd3d-shader for:
//   0..6   the descriptor tables visible to this stage, in root-parameter order (SPIRV-Cross argument buffers;
//          SPIR-V binding = descriptor offset in the table)
//   7      discrete: root descriptors (binding = root parameter index), static samplers (binding 64 + index)
enum { SET_DISCRETE = 7, MAX_TABLE_SETS = 7, STATIC_SAMPLER_BINDING = 64 };

extern "C" char *d3d12mtl_translate(const D3D12MtlShaderIn *in, char *err, size_t errn) {
    const struct vkd3d_shader_root_signature_desc1 *rs = in->rs;
    std::vector<struct vkd3d_shader_resource_binding> bindings;
    std::vector<struct vkd3d_shader_push_constant_buffer> pcbs;
    int table_set[D3D12MTL_MAX_ROOT_PARAMS]; int nsets = 0;
    for (uint32_t i = 0; i < rs->parameter_count; i++) {
        const struct vkd3d_shader_root_parameter1 *p = &rs->parameters[i];
        table_set[i] = -1;
        if (!visible(p->shader_visibility, in->stage)) continue;
        switch (p->parameter_type) {
        case VKD3D_SHADER_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            struct vkd3d_shader_push_constant_buffer b = {};
            b.register_space = p->u.constants.register_space; b.register_index = p->u.constants.shader_register;
            b.shader_visibility = p->shader_visibility; b.offset = in->layout->param[i].offset; b.size = 4 * p->u.constants.value_count;
            pcbs.push_back(b);
            break;
        }
        case VKD3D_SHADER_ROOT_PARAMETER_TYPE_CBV: case VKD3D_SHADER_ROOT_PARAMETER_TYPE_SRV: case VKD3D_SHADER_ROOT_PARAMETER_TYPE_UAV: {
            struct vkd3d_shader_resource_binding b = {};
            b.type = p->parameter_type == VKD3D_SHADER_ROOT_PARAMETER_TYPE_CBV ? VKD3D_SHADER_DESCRIPTOR_TYPE_CBV
                   : p->parameter_type == VKD3D_SHADER_ROOT_PARAMETER_TYPE_SRV ? VKD3D_SHADER_DESCRIPTOR_TYPE_SRV : VKD3D_SHADER_DESCRIPTOR_TYPE_UAV;
            b.register_space = p->u.descriptor.register_space; b.register_index = p->u.descriptor.shader_register;
            b.shader_visibility = p->shader_visibility; b.flags = VKD3D_SHADER_BINDING_FLAG_BUFFER;
            b.binding.set = SET_DISCRETE; b.binding.binding = i; b.binding.count = 1;
            bindings.push_back(b);
            break;
        }
        case VKD3D_SHADER_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            if (nsets >= MAX_TABLE_SETS) { snprintf(err, errn, "more than %d descriptor tables visible to one stage", MAX_TABLE_SETS); return NULL; }
            table_set[i] = nsets++;
            uint32_t off = 0;
            for (uint32_t r = 0; r < p->u.descriptor_table.descriptor_range_count; r++) {
                const struct vkd3d_shader_descriptor_range1 *rg = &p->u.descriptor_table.descriptor_ranges[r];
                if (rg->descriptor_table_offset != ~0u) off = rg->descriptor_table_offset;   // ~0u: D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                for (int flags = VKD3D_SHADER_BINDING_FLAG_BUFFER; flags <= VKD3D_SHADER_BINDING_FLAG_IMAGE; flags <<= 1) {
                    struct vkd3d_shader_resource_binding b = {};
                    b.type = rg->range_type; b.register_space = rg->register_space; b.register_index = rg->base_shader_register;
                    b.shader_visibility = p->shader_visibility; b.flags = flags;
                    b.binding.set = table_set[i]; b.binding.binding = off; b.binding.count = range_count(rg);
                    bindings.push_back(b);
                }
                off += range_count(rg);
            }
            break;
        }
        default: break;
        }
    }
    for (uint32_t i = 0; i < rs->static_sampler_count; i++) {
        const struct vkd3d_shader_static_sampler_desc *s = &rs->static_samplers[i];
        if (!visible(s->shader_visibility, in->stage)) continue;
        struct vkd3d_shader_resource_binding b = {};
        b.type = VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER; b.register_space = s->register_space; b.register_index = s->shader_register;
        b.shader_visibility = s->shader_visibility;
        b.binding.set = SET_DISCRETE; b.binding.binding = STATIC_SAMPLER_BINDING + i; b.binding.count = 1;
        bindings.push_back(b);
    }

    // ---- DXBC/DXIL -> SPIR-V ----
    struct vkd3d_shader_interface_info ii = {};
    ii.type = VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO;
    ii.bindings = bindings.data(); ii.binding_count = (unsigned)bindings.size();
    ii.push_constant_buffers = pcbs.data(); ii.push_constant_buffer_count = (unsigned)pcbs.size();
    static const enum vkd3d_shader_spirv_extension ext[] = { VKD3D_SHADER_SPIRV_EXTENSION_EXT_DEMOTE_TO_HELPER_INVOCATION,
        VKD3D_SHADER_SPIRV_EXTENSION_EXT_DESCRIPTOR_INDEXING, VKD3D_SHADER_SPIRV_EXTENSION_EXT_STENCIL_EXPORT };
    struct vkd3d_shader_spirv_target_info ti = {};
    ti.type = VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO; ti.next = &ii;
    ti.environment = VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_1; ti.extensions = ext; ti.extension_count = 3;
    // Vertex shader: lay its outputs out where the pixel shader reads them (D3D links by semantic, SPIR-V by location).
    struct vkd3d_shader_varying_map_info vmi = {};
    std::vector<struct vkd3d_shader_varying_map> vmap;
    struct vkd3d_shader_scan_signature_info own = {};
    const void *chain = &ti;
    if (in->stage == VKD3D_SHADER_VISIBILITY_VERTEX && in->next_input) {
        own.type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
        struct vkd3d_shader_compile_info sci = {};
        sci.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO; sci.next = &own;
        sci.source.code = in->code; sci.source.size = in->size;
        sci.source_type = d3d12mtl_is_dxil(in->code, in->size) ? VKD3D_SHADER_SOURCE_DXBC_DXIL : VKD3D_SHADER_SOURCE_DXBC_TPF;
        sci.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY; sci.log_level = VKD3D_SHADER_LOG_NONE;
        if (vkd3d_shader_scan(&sci, NULL) >= 0) {
            vmap.resize(in->next_input->element_count + 1);
            unsigned n = 0;
            vkd3d_shader_build_varying_map(&own.output, in->next_input, &n, vmap.data());
            vmi.type = VKD3D_SHADER_STRUCTURE_TYPE_VARYING_MAP_INFO; vmi.next = &ti;
            vmi.varying_map = vmap.data(); vmi.varying_count = n;
            chain = &vmi;
        }
    }
    const struct vkd3d_shader_compile_option opts[] = {
        { VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_1_19 },
        { VKD3D_SHADER_COMPILE_OPTION_FEATURE, VKD3D_SHADER_COMPILE_OPTION_FEATURE_INT64 | VKD3D_SHADER_COMPILE_OPTION_FEATURE_WAVE_OPS },
    };
    struct vkd3d_shader_compile_info ci = {};
    ci.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO; ci.next = chain;
    ci.source.code = in->code; ci.source.size = in->size;
    ci.source_type = d3d12mtl_is_dxil(in->code, in->size) ? VKD3D_SHADER_SOURCE_DXBC_DXIL : VKD3D_SHADER_SOURCE_DXBC_TPF;
    ci.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY; ci.options = opts; ci.option_count = 2;
    ci.log_level = VKD3D_SHADER_LOG_WARNING;
    struct vkd3d_shader_code spv = {}; char *msgs = NULL;
    int r = vkd3d_shader_compile(&ci, &spv, &msgs);
    if (own.type) vkd3d_shader_free_scan_signature_info(&own);
    if (r < 0) {
        snprintf(err, errn, "vkd3d-shader: %d %s", r, msgs ? msgs : "");
        vkd3d_shader_free_messages(msgs);
        return NULL;
    }
    vkd3d_shader_free_messages(msgs);

    // ---- SPIR-V -> MSL ----
    std::string msl;
    try {
        std::vector<uint32_t> words((const uint32_t *)spv.code, (const uint32_t *)spv.code + spv.size / 4);
        vkd3d_shader_free_shader_code(&spv);
        CompilerMSL c(std::move(words));
        CompilerMSL::Options o;
        o.platform = CompilerMSL::Options::macOS;
        o.set_msl_version(3, 1);
        o.argument_buffers = true;
        o.argument_buffers_tier = CompilerMSL::Options::ArgumentBuffersTier::Tier2;
        o.pad_argument_buffer_resources = true;
        o.enable_decoration_binding = false;
        o.force_active_argument_buffer_resources = false;
        o.vertex_for_tessellation = false;
        o.enable_point_size_builtin = in->points != 0;   // Metal rejects [[point_size]] outside point topologies
        c.set_msl_options(o);
        CompilerGLSL::Options co = c.get_common_options();
        co.vertex.flip_vert_y = false;                // D3D and Metal agree on clip space
        co.vertex.fixup_clipspace = false;
        c.set_common_options(co);
        auto model = c.get_execution_model();
        auto add = [&](uint32_t set, uint32_t binding, uint32_t idx, SPIRType::BaseType bt) {
            MSLResourceBinding b;
            b.stage = model; b.desc_set = set; b.binding = binding; b.count = 1; b.basetype = bt;
            b.msl_buffer = b.msl_texture = b.msl_sampler = idx;
            c.add_msl_resource_binding(b);
        };
        // push constants = the root argument block
        add(kPushConstDescSet, kPushConstBinding, D3D12MTL_BUF_ROOT, SPIRType::UInt);
        c.add_discrete_descriptor_set(SET_DISCRETE);
        // descriptor tables: argument buffer of 24-byte entries; ids are 8-byte slots (3 per entry), unused ids padded
        ShaderResources res = c.get_shader_resources();
        std::vector<std::vector<int8_t>> used(MAX_TABLE_SETS);   // per set: slot -> 1 if a real resource lives there
        auto place = [&](const SmallVector<Resource> &list, int textureish, SPIRType::BaseType bt) {
            for (auto &rr : list) {
                uint32_t set = c.get_decoration(rr.id, spv::DecorationDescriptorSet), b = c.get_decoration(rr.id, spv::DecorationBinding);
                if (set == SET_DISCRETE) {
                    // root descriptor -> its buffer slot; static sampler -> constexpr
                    if (b >= STATIC_SAMPLER_BINDING) c.remap_constexpr_sampler_by_binding(set, b, static_sampler(&rs->static_samplers[b - STATIC_SAMPLER_BINDING]));
                    else add(set, b, in->layout->param[b].slot, bt);
                    continue;
                }
                if (set >= MAX_TABLE_SETS) continue;
                uint32_t slot = 3 * b + (textureish ? D3D12MTL_SLOT_TEXTURE : D3D12MTL_SLOT_ADDR);
                add(set, b, slot, bt);
                if (used[set].size() <= slot) used[set].resize(slot + 1, 0);
                used[set][slot] = 1;
            }
        };
        place(res.uniform_buffers, 0, SPIRType::UInt);
        place(res.storage_buffers, 0, SPIRType::UInt);
        place(res.separate_images, 1, SPIRType::Image);
        place(res.storage_images, 1, SPIRType::Image);
        place(res.separate_samplers, 0, SPIRType::Sampler);
        place(res.sampled_images, 1, SPIRType::SampledImage);
        for (uint32_t i = 0; i < rs->parameter_count; i++) {
            int s = table_set[i];
            if (s < 0) continue;
            add(s, kArgumentBufferBinding, in->layout->param[i].slot, SPIRType::Void);
            for (uint32_t slot = 0; slot < used[s].size(); slot++)
                if (!used[s][slot]) add(s, 0x100000 + slot, slot, SPIRType::UInt);   // padding member (8 bytes)
        }
        // vertex inputs: [[attribute(n)]] = SPIR-V location = input register (default); nothing to do.
        msl = c.compile();
    } catch (const std::exception &e) {
        snprintf(err, errn, "SPIRV-Cross: %s", e.what());
        return NULL;
    }
    return strdup(msl.c_str());
}
