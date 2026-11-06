// Pipeline-state workarounds for what Metal/MoltenVK lacks (docs/d3d12.md, "Workarounds").
//
// Geometry shaders (MoltenVK: no geometryShader). A PSO with a GS is analysed with vkd3d-shader (signatures +
// disassembly):
//   * pass-through shaped (one primitive in, the same primitive out: maxvertexcount = input vertices, matching
//     output topology, no GS instancing, outputs a subset of the previous stage's, no layer/viewport index):
//     the GS is dropped and the PSO draws without it. Approximation: per-vertex changes the GS makes are lost.
//   * anything else (point sprites, layered rendering, amplification, ...): the PSO is created without the GS
//     and pixel shader, and every draw made with it is skipped (logged once per PSO). No crash, missing geometry.
//   WINTOMAC_D3D12_GS=skip forces the second mode, =strip the first.
// Stream output (no VK_EXT_transform_feedback): the SO declaration is removed. With a rasterized stream the
// draws still render; with D3D12_SO_NO_RASTERIZED_STREAM (capture only) they are skipped. SO buffers stay unwritten.
// Predication (no VK_EXT_conditional_rendering): predicated work always executes (correct images, no culling).
// Samplers (no VK_EXT_custom_border_color): arbitrary border colours snap to the nearest of the three Vulkan ones.
// Draw skipping is tracked per command list: SetPipelineState / Reset / ClearState record whether the PSO is
// drawable, DrawInstanced / DrawIndexedInstanced / ExecuteIndirect consult it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <vkd3d_shader.h>
#include "d3d12_internal.h"

// ---- device capabilities (queried once from the VkPhysicalDevice behind libvkd3d) ---------------------------
static struct { int done, gs, xfb, cond; } caps;
static pthread_mutex_t caps_lock = PTHREAD_MUTEX_INITIALIZER;
static void query_caps(ID3D12Device9 *dev) {
    int d3d12mtl_enabled(void);
    pthread_mutex_lock(&caps_lock);
    if (!caps.done && d3d12mtl_enabled()) caps.done = 1;   // Metal backend: no GS/XFB/conditional rendering either
    if (!caps.done) {
        VkPhysicalDevice pd = vkd3d_get_vk_physical_device((ID3D12Device *)dev);
        VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd, &f);
        caps.gs = f.geometryShader;
        uint32_t n = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
        VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e); vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
        for (uint32_t i = 0; i < n; i++) {
            if (!strcmp(e[i].extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) caps.xfb = 1;
            if (!strcmp(e[i].extensionName, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME)) caps.cond = 1;
        }
        free(e);
        if (getenv("WINTOMAC_D3D12_FAKE_NO_GS")) caps.gs = 0;   // for testing on drivers that do have them
        caps.done = 1;
    }
    pthread_mutex_unlock(&caps_lock);
}

// ---- shader analysis ------------------------------------------------------------------------------------
// DXBC container: DXIL chunk -> SM 6 source, else TPF.
static enum vkd3d_shader_source_type source_type(const D3D12_SHADER_BYTECODE *bc) {
    const u8 *p = bc->pShaderBytecode; size_t n = bc->BytecodeLength;
    if (!p || n < 32 || memcmp(p, "DXBC", 4)) return VKD3D_SHADER_SOURCE_DXBC_TPF;
    u32 count; memcpy(&count, p + 28, 4);
    for (u32 i = 0; i < count && 32 + 4 * i + 4 <= n; i++) {
        u32 off; memcpy(&off, p + 32 + 4 * i, 4);
        if (off + 4 <= n && !memcmp(p + off, "DXIL", 4)) return VKD3D_SHADER_SOURCE_DXBC_DXIL;
    }
    return VKD3D_SHADER_SOURCE_DXBC_TPF;
}
typedef struct { struct vkd3d_shader_scan_signature_info sig; int ok; } ShaderSigs;
static void scan(const D3D12_SHADER_BYTECODE *bc, ShaderSigs *s) {
    memset(s, 0, sizeof *s);
    if (!bc->pShaderBytecode || !bc->BytecodeLength) return;
    s->sig.type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
    struct vkd3d_shader_compile_info ci = { VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO, &s->sig };
    ci.source.code = bc->pShaderBytecode; ci.source.size = bc->BytecodeLength;
    ci.source_type = source_type(bc); ci.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY; ci.log_level = VKD3D_SHADER_LOG_NONE;
    s->ok = vkd3d_shader_scan(&ci, NULL) >= 0;
}
// GS properties from vkd3d-shader's disassembly ("dcl_maxout 3", "dcl_inputprimitive trianglelist", ...).
typedef struct { int maxout, instances; char in[32], out[32]; } GsProps;
static int gs_props(const D3D12_SHADER_BYTECODE *bc, GsProps *g) {
    memset(g, 0, sizeof *g); g->instances = 1;
    struct vkd3d_shader_compile_info ci = { VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO };
    ci.source.code = bc->pShaderBytecode; ci.source.size = bc->BytecodeLength;
    ci.source_type = source_type(bc); ci.target_type = VKD3D_SHADER_TARGET_D3D_ASM; ci.log_level = VKD3D_SHADER_LOG_NONE;
    struct vkd3d_shader_code out = { 0 };
    if (vkd3d_shader_compile(&ci, &out, NULL) < 0) return 0;
    char *t = malloc(out.size + 1); memcpy(t, out.code, out.size); t[out.size] = 0;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        while (*l == ' ') l++;
        if (!strncmp(l, "dcl_maxout ", 11)) g->maxout = atoi(l + 11);
        else if (!strncmp(l, "dcl_gs_instances ", 17)) g->instances = atoi(l + 17);
        else if (!strncmp(l, "dcl_inputprimitive ", 19)) snprintf(g->in, sizeof g->in, "%s", l + 19);
        else if (!strncmp(l, "dcl_outputtopology ", 19)) snprintf(g->out, sizeof g->out, "%s", l + 19);
    }
    free(t); vkd3d_shader_free_shader_code(&out);
    return g->maxout > 0;
}
static const struct vkd3d_shader_signature_element *find_elem(const struct vkd3d_shader_signature *s, const char *name, unsigned idx) {
    for (unsigned i = 0; i < s->element_count; i++)
        if (!strcasecmp(s->elements[i].semantic_name, name) && s->elements[i].semantic_index == idx && !s->elements[i].stream_index)
            return &s->elements[i];
    return NULL;
}
// Does this GS just forward one primitive? (see the top of the file)
static int gs_passthrough(const D3D12_SHADER_BYTECODE *gs, const D3D12_SHADER_BYTECODE *prev, char *why, size_t n) {
    GsProps g; ShaderSigs gsig, psig; int ok = 0;
    if (!gs_props(gs, &g)) { snprintf(why, n, "can't disassemble"); return 0; }
    int verts = !strcmp(g.in, "point") ? 1 : !strncmp(g.in, "line", 4) && !strstr(g.in, "adj") ? 2
              : !strcmp(g.in, "triangle") || !strcmp(g.in, "trianglelist") ? 3 : 0;
    const char *want = verts == 1 ? "pointlist" : verts == 2 ? "linestrip" : "trianglestrip";
    snprintf(why, n, "%s -> %s x%d, %d instance(s)", g.in, g.out, g.maxout, g.instances);
    if (!verts || g.maxout != verts || g.instances != 1 || strcmp(g.out, want)) return 0;
    scan(gs, &gsig); scan(prev, &psig);
    if (gsig.ok && psig.ok) {
        ok = 1;
        for (unsigned i = 0; i < gsig.sig.output.element_count && ok; i++) {
            const struct vkd3d_shader_signature_element *e = &gsig.sig.output.elements[i], *p;
            if (e->stream_index) continue;
            if (e->sysval_semantic == VKD3D_SHADER_SV_RENDER_TARGET_ARRAY_INDEX || e->sysval_semantic == VKD3D_SHADER_SV_VIEWPORT_ARRAY_INDEX
                || e->sysval_semantic == VKD3D_SHADER_SV_PRIMITIVE_ID) { snprintf(why + strlen(why), n - strlen(why), ", writes %s", e->semantic_name); ok = 0; }
            else if (!(p = find_elem(&psig.sig.output, e->semantic_name, e->semantic_index)) || (e->mask & ~p->mask)) {
                snprintf(why + strlen(why), n - strlen(why), ", new output %s%u", e->semantic_name, e->semantic_index); ok = 0;
            }
        }
    }
    if (gsig.ok) vkd3d_shader_free_scan_signature_info(&gsig.sig);
    if (psig.ok) vkd3d_shader_free_scan_signature_info(&psig.sig);
    return ok;
}

// What to do with a PSO's GS / stream output. Returns flags for the PSO wrapper and edits the stages in place.
typedef struct { D3D12_SHADER_BYTECODE *vs, *ds, *gs, *ps; D3D12_STREAM_OUTPUT_DESC *so; } Stages;
static u32 fix_stages(ID3D12Device9 *dev, Stages *st, char *log, size_t n) {
    u32 flags = 0; *log = 0;
    query_caps(dev);
    int has_gs = st->gs && st->gs->pShaderBytecode && st->gs->BytecodeLength;
    int has_so = st->so && st->so->NumEntries;
    if (has_so && !caps.xfb) {
        int rasterized = st->so->RasterizedStream != D3D12_SO_NO_RASTERIZED_STREAM;
        snprintf(log + strlen(log), n - strlen(log), "stream output removed (%s)", rasterized ? "draws still render" : "capture-only: draws skipped");
        if (!rasterized) flags |= D3D12_PSO_SKIP_DRAWS;
        memset(st->so, 0, sizeof *st->so);
    }
    if (has_gs && !caps.gs) {
        const char *mode = getenv("WINTOMAC_D3D12_GS");
        D3D12_SHADER_BYTECODE *prev = st->ds && st->ds->BytecodeLength ? st->ds : st->vs;
        char why[160] = "";
        int strip = mode && !strcmp(mode, "strip") ? 1 : mode && !strcmp(mode, "skip") ? 0 : gs_passthrough(st->gs, prev, why, sizeof why);
        if (strip && !(flags & D3D12_PSO_SKIP_DRAWS)) {
            flags |= D3D12_PSO_GS_STRIPPED;
            snprintf(log + strlen(log), n - strlen(log), "%spass-through geometry shader dropped (%s)", *log ? "; " : "", why);
        } else {
            flags |= D3D12_PSO_SKIP_DRAWS;
            snprintf(log + strlen(log), n - strlen(log), "%sgeometry shader not emulated (%s): draws skipped", *log ? "; " : "", why);
        }
        memset(st->gs, 0, sizeof *st->gs);
    }
    if ((flags & D3D12_PSO_SKIP_DRAWS) && st->ps) memset(st->ps, 0, sizeof *st->ps);   // never drawn: keep creation simple
    return flags;
}
static u32 finish_pso(u64 out, u32 hr, u32 flags, const char *log) {
    if (SUCCEEDED(hr) && out) {
        hr = d3d12_out(out, hr);
        D3D12Wrap *w = d3d12_wrapper(LD64(out));
        if (w) w->xflags = flags;
    }
    if (flags) {
        static int count; char m[400];
        if (__atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) < 32) { snprintf(m, sizeof m, "d3d12: PSO: %s (hr 0x%08x)", log, hr); rt_log("%s", m); }
    }
    return hr;
}

// ---- PSO creation ----------------------------------------------------------------------------------------
SHIM(D3D12Device_CreateGraphicsPipelineState) {  // (This, desc*, riid, out)
    ID3D12Device9 *h = d3d12_host(A1); int bad = 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *)A2;
    d.pRootSignature = d3d12_in((u64)d.pRootSignature, &bad);
    if (bad) { d3d12_bad_arg("CreateGraphicsPipelineState"); RET(E_INVALIDARG_); }
    D3D12_TRACE("Device.CreateGraphicsPipelineState");
    char log[320]; Stages st = { &d.VS, &d.DS, &d.GS, &d.PS, &d.StreamOutput };
    u32 flags = fix_stages(h, &st, log, sizeof log);
    u32 hr = h->lpVtbl->CreateGraphicsPipelineState(h, &d, (REFIID)A3, (void **)A4);
    RET(finish_pso(A4, hr, flags, log));
}
SHIM(D3D12Device_CreateComputePipelineState) {
    ID3D12Device9 *h = d3d12_host(A1); int bad = 0;
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = *(const D3D12_COMPUTE_PIPELINE_STATE_DESC *)A2;
    d.pRootSignature = d3d12_in((u64)d.pRootSignature, &bad);
    if (bad) { d3d12_bad_arg("CreateComputePipelineState"); RET(E_INVALIDARG_); }
    D3D12_TRACE("Device.CreateComputePipelineState");
    u32 hr = h->lpVtbl->CreateComputePipelineState(h, &d, (REFIID)A3, (void **)A4);
    RET(SUCCEEDED(hr) && A4 ? d3d12_out(A4, hr) : hr);
}
// Pipeline state streams: walk the subobjects (same layout rules as libvkd3d's state.c), unwrap the root
// signature and apply the GS / stream-output fallbacks in a private copy of the stream.
static size_t subobject_size(u32 type, size_t *align) {
#define S(T) (*align = __alignof__(T), sizeof(T))
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return S(ID3D12RootSignature *);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case 24 /* AS */: case 25 /* MS */: return S(D3D12_SHADER_BYTECODE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return S(D3D12_STREAM_OUTPUT_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return S(D3D12_BLEND_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return S(UINT);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return S(D3D12_RASTERIZER_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return S(D3D12_DEPTH_STENCIL_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return S(D3D12_INPUT_LAYOUT_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return S(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return S(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return S(struct D3D12_RT_FORMAT_ARRAY);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return S(DXGI_FORMAT);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return S(DXGI_SAMPLE_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return S(UINT);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return S(D3D12_CACHED_PIPELINE_STATE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return S(D3D12_PIPELINE_STATE_FLAGS);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return S(D3D12_DEPTH_STENCIL_DESC1);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return S(D3D12_VIEW_INSTANCING_DESC);
    default: return 0;
    }
#undef S
}
SHIM(D3D12Device_CreatePipelineState) {  // (This, const D3D12_PIPELINE_STATE_STREAM_DESC *, riid, out)
    ID3D12Device9 *h = d3d12_host(A1); int bad = 0;
    D3D12_PIPELINE_STATE_STREAM_DESC d = *(const D3D12_PIPELINE_STATE_STREAM_DESC *)A2;
    u8 *s = malloc(d.SizeInBytes ? d.SizeInBytes : 1);
    memcpy(s, d.pPipelineStateSubobjectStream, d.SizeInBytes);
    Stages st = { 0 };
    for (size_t i = 0; i + 4 <= d.SizeInBytes; ) {
        u32 type; memcpy(&type, s + i, 4);
        size_t align = 1, size = subobject_size(type, &align);
        if (!size) break;   // unknown subobject: libvkd3d will reject the stream itself
        size_t start = (4 + align - 1) & ~(align - 1);
        if (i + start + size > d.SizeInBytes) break;
        void *body = s + i + start;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
            u64 g; memcpy(&g, body, 8);
            void *rs = d3d12_in(g, &bad); memcpy(body, &rs, 8);
        }
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) st.vs = body;
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS) st.ds = body;
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS) st.gs = body;
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) st.ps = body;
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT) st.so = body;
        i += (start + size + 7) & ~(size_t)7;
    }
    d.pPipelineStateSubobjectStream = s;
    u32 hr = E_INVALIDARG_, flags = 0; char log[320] = "";
    if (bad) d3d12_bad_arg("CreatePipelineState");
    else {
        D3D12_TRACE("Device.CreatePipelineState");
        flags = fix_stages(h, &st, log, sizeof log);
        hr = h->lpVtbl->CreatePipelineState(h, &d, (REFIID)A3, (void **)A4);
    }
    free(s);
    RET(finish_pso(A4, hr, flags, log));
}

// ---- draws with a PSO that can't draw are skipped ------------------------------------------------------------
static void list_set_pso(u64 list, u64 pso) {
    D3D12Wrap *l = d3d12_wrapper(list), *p = d3d12_wrapper(pso);
    if (l) l->xflags = (l->xflags & ~D3D12_LIST_SKIP_DRAWS) | (p && (p->xflags & D3D12_PSO_SKIP_DRAWS) ? D3D12_LIST_SKIP_DRAWS : 0);
}
static int list_skips(u64 list) {
    D3D12Wrap *l = d3d12_wrapper(list);
    if (!l || !(l->xflags & D3D12_LIST_SKIP_DRAWS)) return 0;
    static int n; if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 8) rt_log("d3d12: draw skipped (pipeline uses an unsupported geometry shader / stream output)");
    return 1;
}
SHIM(D3D12List_SetPipelineState) {
    ID3D12GraphicsCommandList6 *h = d3d12_host(A1); int bad = 0;
    void *p = d3d12_in(A2, &bad);
    if (bad) { d3d12_bad_arg("SetPipelineState"); RET(0); }
    D3D12_TRACE("List.SetPipelineState");
    list_set_pso(A1, A2);
    h->lpVtbl->SetPipelineState(h, p);
    RET(0);
}
SHIM(D3D12List_Reset) {   // (This, allocator, initial PSO)
    ID3D12GraphicsCommandList6 *h = d3d12_host(A1); int bad = 0;
    void *a = d3d12_in(A2, &bad), *p = d3d12_in(A3, &bad);
    if (bad) { d3d12_bad_arg("Reset"); RET(E_INVALIDARG_); }
    D3D12_TRACE("List.Reset");
    list_set_pso(A1, A3);
    RET(h->lpVtbl->Reset(h, a, p));
}
SHIM(D3D12List_ClearState) {
    ID3D12GraphicsCommandList6 *h = d3d12_host(A1); int bad = 0;
    void *p = d3d12_in(A2, &bad);
    if (bad) { d3d12_bad_arg("ClearState"); RET(0); }
    list_set_pso(A1, A2);
    h->lpVtbl->ClearState(h, p);
    RET(0);
}
SHIM(D3D12List_DrawInstanced) {   // (This, vertices, instances, first vertex, first instance)
    ID3D12GraphicsCommandList6 *h = d3d12_host(A1);
    D3D12_TRACE("List.DrawInstanced");
    if (!list_skips(A1)) h->lpVtbl->DrawInstanced(h, (UINT)A2, (UINT)A3, (UINT)A4, (UINT)AN(5));
    RET(0);
}
SHIM(D3D12List_DrawIndexedInstanced) {   // (This, indices, instances, first index, base vertex, first instance)
    ID3D12GraphicsCommandList6 *h = d3d12_host(A1);
    D3D12_TRACE("List.DrawIndexedInstanced");
    if (!list_skips(A1)) h->lpVtbl->DrawIndexedInstanced(h, (UINT)A2, (UINT)A3, (UINT)A4, (INT)AN(5), (UINT)AN(6));
    RET(0);
}
SHIM(D3D12List_ExecuteIndirect) {   // (This, signature, max count, args, args offset, count buffer, count offset)
    ID3D12GraphicsCommandList6 *h = d3d12_host(A1); int bad = 0;
    void *sig = d3d12_in(A2, &bad), *args = d3d12_in(A4, &bad), *cnt = d3d12_in(AN(6), &bad);
    if (bad) { d3d12_bad_arg("ExecuteIndirect"); RET(0); }
    D3D12_TRACE("List.ExecuteIndirect");
    if (!list_skips(A1)) h->lpVtbl->ExecuteIndirect(h, sig, (UINT)A3, args, AN(5), cnt, AN(7));
    RET(0);
}
// Predication: without VK_EXT_conditional_rendering libvkd3d ignores it, so everything draws.
SHIM(D3D12List_SetPredication) {   // (This, buffer, offset, op)
    ID3D12GraphicsCommandList6 *h = d3d12_host(A1); int bad = 0;
    void *b = d3d12_in(A2, &bad);
    if (bad) { d3d12_bad_arg("SetPredication"); RET(0); }
    D3D12_TRACE("List.SetPredication");
    if (b && !caps.cond) d3d12_log_once("SetPredication: no conditional rendering in MoltenVK, predicated work always executes");
    h->lpVtbl->SetPredication(h, b, A3, (D3D12_PREDICATION_OP)A4);
    RET(0);
}

// ---- samplers: border colours ----------------------------------------------------------------------------
// libvkd3d maps exact {0,0,0,0} / {0,0,0,1} / {1,1,1,1} to the Vulkan border colours and everything else to
// transparent black. Snap to the nearest one instead (alpha first, then luminance).
void d3d12_create_sampler(ID3D12Device9 *h, const D3D12_SAMPLER_DESC *src, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
    D3D12_SAMPLER_DESC d = *src;
    int border = d.AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER || d.AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER
              || d.AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    const float *b = d.BorderColor;
    int standard = (b[0] == b[1] && b[1] == b[2] && (b[0] == 0 || b[0] == 1) && (b[3] == 0 || b[3] == 1) && !(b[0] == 1 && b[3] == 0));
    if (border && !standard) {
        float lum = 0.2126f * b[0] + 0.7152f * b[1] + 0.0722f * b[2];
        float a = b[3] < 0.5f ? 0.0f : 1.0f, rgb = a == 0.0f ? 0.0f : lum >= 0.5f ? 1.0f : 0.0f;
        d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = rgb; d.BorderColor[3] = a;
        d3d12_log_once("sampler border colour approximated (no VK_EXT_custom_border_color)");
    }
    h->lpVtbl->CreateSampler(h, &d, dst);
}
SHIM(D3D12Device_CreateSampler) {   // (This, const D3D12_SAMPLER_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE)
    D3D12_TRACE("Device.CreateSampler");
    if (A2) d3d12_create_sampler(d3d12_host(A1), (const D3D12_SAMPLER_DESC *)A2, (D3D12_CPU_DESCRIPTOR_HANDLE){ A3 });
    RET(0);
}

const Shim shims_d3d12_pso[] = {
#define M(n) { NULL, "__wintomac_" #n, sh_##n }
    M(D3D12Device_CreateGraphicsPipelineState), M(D3D12Device_CreateComputePipelineState), M(D3D12Device_CreatePipelineState),
    M(D3D12List_SetPipelineState), M(D3D12List_Reset), M(D3D12List_ClearState), M(D3D12List_DrawInstanced),
    M(D3D12List_DrawIndexedInstanced), M(D3D12List_ExecuteIndirect), M(D3D12List_SetPredication), M(D3D12Device_CreateSampler),
#undef M
    { 0, 0, 0 }
};
