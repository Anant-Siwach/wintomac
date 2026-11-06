// Native d3dcompiler_4x.dll: HLSL -> DXBC / D3D bytecode with vkd3d-shader (the Wine project's shader compiler).
// Same design as Wine's d3dcompiler (which also wraps vkd3d-shader); credits in CREDITS.md.
//
// Also the template for host-implemented COM objects handed to guest code: the object's vtable holds
// *thunk addresses*, so a guest `call [vtbl+N]` lands in a shim here with `this` in rcx.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vkd3d/vkd3d_shader.h>

#include "win32.h"

#define S_OK 0
#define E_FAIL 0x80004005u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000Eu
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u

// ---- ID3DBlob (== ID3D10Blob) --------------------------------------------------------------
// Guest-visible layout: { vtbl*, ... }. Everything after the vtable pointer is ours.
typedef struct { u64 vtbl; u32 refs; u32 pad; u64 size; u8 *data; } Blob;
static u64 blob_vtbl[5];

static const u8 IID_IUnknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
static const u8 IID_ID3D10Blob[16] = { 0x08, 0xFB, 0xA5, 0x8B, 0x95, 0x51, 0xE2, 0x40, 0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02 };

SHIM(Blob_QueryInterface) {  // (this, REFIID, void **out)
    if (!memcmp((void *)A2, IID_IUnknown, 16) || !memcmp((void *)A2, IID_ID3D10Blob, 16)) {
        __atomic_add_fetch(&((Blob *)A1)->refs, 1, __ATOMIC_SEQ_CST); ST64(A3, A1); RET(S_OK);
    }
    ST64(A3, 0); RET(E_NOINTERFACE);
}
SHIM(Blob_AddRef) { RET(__atomic_add_fetch(&((Blob *)A1)->refs, 1, __ATOMIC_SEQ_CST)); }
SHIM(Blob_Release) {
    Blob *b = (Blob *)A1; u32 r = __atomic_sub_fetch(&b->refs, 1, __ATOMIC_SEQ_CST);
    if (!r) { free(b->data); free(b); }
    RET(r);
}
SHIM(Blob_GetBufferPointer) { RET(((Blob *)A1)->data); }
SHIM(Blob_GetBufferSize) { RET(((Blob *)A1)->size); }

static Blob *blob_new(const void *data, size_t size) {
    if (!blob_vtbl[0]) {
        static const char *const m[5] = { "__wintomac_Blob_QueryInterface", "__wintomac_Blob_AddRef", "__wintomac_Blob_Release",
                                          "__wintomac_Blob_GetBufferPointer", "__wintomac_Blob_GetBufferSize" };
        for (int i = 0; i < 5; i++) blob_vtbl[i] = rt_thunk_for(NULL, m[i]);
    }
    Blob *b = calloc(1, sizeof *b);
    b->vtbl = (u64)blob_vtbl; b->refs = 1; b->size = size;
    b->data = malloc(size ? size : 1);
    if (data && size) memcpy(b->data, data, size); else if (size) memset(b->data, 0, size);
    return b;
}
static void put_blob(u64 out, const void *data, size_t size) { if (out) ST64(out, (u64)blob_new(data, size)); }

// ---- includes ------------------------------------------------------------------------
#define D3D_COMPILE_STANDARD_FILE_INCLUDE 1
typedef struct { Ctx *c; u64 include; } IncludeCtx;

static int open_include(const char *filename, bool local, const char *parent_data, void *context, struct vkd3d_shader_code *out) {
    IncludeCtx *ic = context;
    if (ic->include == D3D_COMPILE_STANDARD_FILE_INCLUDE) {
        char path[2048]; rt_host_path(filename, path, sizeof path);
        FILE *f = fopen(path, "rb");
        if (!f) return VKD3D_ERROR;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        void *buf = malloc(n > 0 ? n : 1);
        if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return VKD3D_ERROR; }
        fclose(f);
        out->code = buf; out->size = n;
        return VKD3D_OK;
    }
    // Guest ID3DInclude: vtbl[0] = Open(this, D3D_INCLUDE_TYPE, LPCSTR, LPCVOID parent, LPCVOID *data, UINT *bytes)
    u64 *data = calloc(1, 8); u32 *bytes = calloc(1, 4);
    u64 args[6] = { ic->include, local ? 0 /* D3D_INCLUDE_LOCAL */ : 1 /* D3D_INCLUDE_SYSTEM */, (u64)filename, (u64)parent_data, (u64)data, (u64)bytes };
    u32 hr = (u32)rt_guest_call(ic->c, LD64(LD64(ic->include)), 6, args);
    if (hr & 0x80000000u) { free(data); free(bytes); return VKD3D_ERROR; }
    out->code = (const void *)*data; out->size = *bytes;
    free(data); free(bytes);
    return VKD3D_OK;
}
static void close_include(const struct vkd3d_shader_code *code, void *context) {
    IncludeCtx *ic = context;
    if (ic->include == D3D_COMPILE_STANDARD_FILE_INCLUDE) { free((void *)code->code); return; }
    u64 args[2] = { ic->include, (u64)code->code };  // vtbl[1] = Close(this, LPCVOID data)
    rt_guest_call(ic->c, LD64(LD64(ic->include) + 8), 2, args);
}

static struct vkd3d_shader_macro *convert_macros(u64 defines, unsigned *count) {
    unsigned n = 0;
    if (defines) while (LD64(defines + 16ull * n)) n++;
    struct vkd3d_shader_macro *m = calloc(n + 1, sizeof *m);
    for (unsigned i = 0; i < n; i++) {
        m[i].name = (const char *)LD64(defines + 16ull * i);
        m[i].value = LD64(defines + 16ull * i + 8) ? (const char *)LD64(defines + 16ull * i + 8) : "";
    }
    *count = n;
    return m;
}

// D3DCOMPILE_* flags we map to vkd3d options
#define D3DCOMPILE_PACK_MATRIX_ROW_MAJOR (1u << 3)
#define D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR (1u << 4)
#define D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY (1u << 12)
#define D3DCOMPILE_WARNINGS_ARE_ERRORS (1u << 18)

static u32 compile(Ctx *c, const void *src, size_t size, const char *name, u64 defines, u64 include,
                   const char *entry, const char *target, u32 flags1, u32 flags2, u64 out_code, u64 out_err) {
    (void)flags2;
    if (out_code) ST64(out_code, 0);
    if (out_err) ST64(out_err, 0);
    if (!target) return E_INVALIDARG;
    enum vkd3d_shader_target_type tt;
    int major = 0;
    const char *us = strchr(target, '_');
    if (us) major = atoi(us + 1);
    if (!strncmp(target, "fx_", 3)) tt = VKD3D_SHADER_TARGET_FX;
    else if (major >= 6) {
        const char *msg = "wintomac d3dcompiler: shader model 6 needs dxcompiler (DXIL); not supported by d3dcompiler_47 either\n";
        put_blob(out_err, msg, strlen(msg) + 1);
        return E_FAIL;
    } else if (major >= 4) tt = VKD3D_SHADER_TARGET_DXBC_TPF;
    else tt = VKD3D_SHADER_TARGET_D3D_BYTECODE;

    struct vkd3d_shader_compile_option opts[4]; unsigned nopts = 0;
    opts[nopts++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_1_19 };
    if (flags1 & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR)
        opts[nopts++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER, VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR };
    else if (flags1 & D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR)
        opts[nopts++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER, VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR };
    if (flags1 & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY)
        opts[nopts++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY, VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_MAP_SEMANTIC_NAMES };

    unsigned nmacros; struct vkd3d_shader_macro *macros = convert_macros(defines, &nmacros);
    IncludeCtx ic = { c, include };
    struct vkd3d_shader_preprocess_info pp = { VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO, NULL, macros, nmacros,
                                              include ? open_include : NULL, include ? close_include : NULL, &ic };
    struct vkd3d_shader_hlsl_source_info hlsl = { 0 };
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO; hlsl.next = &pp;
    hlsl.entry_point = entry; hlsl.profile = target;
    struct vkd3d_shader_compile_info ci = { 0 };
    ci.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO; ci.next = &hlsl;
    ci.source.code = src; ci.source.size = size;
    ci.source_type = VKD3D_SHADER_SOURCE_HLSL; ci.target_type = tt;
    ci.options = opts; ci.option_count = nopts;
    ci.log_level = VKD3D_SHADER_LOG_INFO; ci.source_name = name ? name : "<anonymous>";

    struct vkd3d_shader_code code = { 0 }; char *messages = NULL;
    int r = vkd3d_shader_compile(&ci, &code, &messages);
    if ((flags1 & D3DCOMPILE_WARNINGS_ARE_ERRORS) && r >= 0 && messages && strstr(messages, "warning")) r = VKD3D_ERROR;
    if (messages && *messages) put_blob(out_err, messages, strlen(messages) + 1);
    if (rt_trace_enabled() || (r < 0 && getenv("WINTOMAC_VERBOSE")))
        rt_log("D3DCompile(%s, %s, %s) = %d%s%s", name ? name : "?", entry ? entry : "?", target, r, messages ? "\n" : "", messages ? messages : "");
    vkd3d_shader_free_messages(messages);
    free(macros);
    if (r < 0) return E_FAIL;
    put_blob(out_code, code.code, code.size);
    vkd3d_shader_free_shader_code(&code);
    return S_OK;
}

// D3DCompile(src, size, name, defines, include, entry, target, flags1, flags2, **code, **errors)
SHIM(D3DCompile) { RET(compile(c, (void *)A1, A2, (const char *)A3, A4, AN(5), (const char *)AN(6), (const char *)AN(7), (u32)AN(8), (u32)AN(9), AN(10), AN(11))); }
// D3DCompile2(... flags2, secondary_flags, secondary_data, secondary_size, **code, **errors)
SHIM(D3DCompile2) { RET(compile(c, (void *)A1, A2, (const char *)A3, A4, AN(5), (const char *)AN(6), (const char *)AN(7), (u32)AN(8), (u32)AN(9), AN(13), AN(14))); }
// D3DCompileFromFile(LPCWSTR file, defines, include, entry, target, flags1, flags2, **code, **errors)
SHIM(D3DCompileFromFile) {
    char win[1024]; size_t i = 0; for (; i + 1 < sizeof win && LD16(A1 + 2 * i); i++) win[i] = (char)LD16(A1 + 2 * i); win[i] = 0;
    char path[2048]; rt_host_path(win, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) RET(0x80070002u);  // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1); size_t got = fread(buf, 1, n, f); fclose(f);
    u32 hr = compile(c, buf, got, win, A2, A3, (const char *)A4, (const char *)AN(5), (u32)AN(6), (u32)AN(7), AN(8), AN(9));
    free(buf); RET(hr);
}
// D3DPreprocess(src, size, name, defines, include, **text, **errors)
SHIM(D3DPreprocess) {
    unsigned nmacros; struct vkd3d_shader_macro *macros = convert_macros(A4, &nmacros);
    IncludeCtx ic = { c, AN(5) };
    struct vkd3d_shader_preprocess_info pp = { VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO, NULL, macros, nmacros,
                                              AN(5) ? open_include : NULL, AN(5) ? close_include : NULL, &ic };
    struct vkd3d_shader_compile_info ci = { 0 };
    ci.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO; ci.next = &pp;
    ci.source.code = (void *)A1; ci.source.size = A2; ci.source_type = VKD3D_SHADER_SOURCE_HLSL;
    ci.target_type = VKD3D_SHADER_TARGET_NONE; ci.log_level = VKD3D_SHADER_LOG_INFO;
    ci.source_name = A3 ? (const char *)A3 : "<anonymous>";
    struct vkd3d_shader_code out = { 0 }; char *messages = NULL;
    if (AN(6)) ST64(AN(6), 0);
    if (AN(7)) ST64(AN(7), 0);
    int r = vkd3d_shader_preprocess(&ci, &out, &messages);
    if (messages && *messages) put_blob(AN(7), messages, strlen(messages) + 1);
    vkd3d_shader_free_messages(messages); free(macros);
    if (r < 0) RET(E_FAIL);
    put_blob(AN(6), out.code, out.size);
    vkd3d_shader_free_shader_code(&out);
    RET(S_OK);
}
// D3DDisassemble(src, size, flags, comments, **text)
SHIM(D3DDisassemble) {
    struct vkd3d_shader_compile_info ci = { 0 };
    ci.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    ci.source.code = (void *)A1; ci.source.size = A2;
    ci.source_type = (A2 >= 4 && !memcmp((void *)A1, "DXBC", 4)) ? VKD3D_SHADER_SOURCE_DXBC_TPF : VKD3D_SHADER_SOURCE_D3D_BYTECODE;
    ci.target_type = VKD3D_SHADER_TARGET_D3D_ASM; ci.log_level = VKD3D_SHADER_LOG_INFO;
    struct vkd3d_shader_code out = { 0 }; char *messages = NULL;
    int r = vkd3d_shader_compile(&ci, &out, &messages);
    vkd3d_shader_free_messages(messages);
    if (r < 0) RET(E_FAIL);
    char *text = malloc(out.size + 1); memcpy(text, out.code, out.size); text[out.size] = 0;
    put_blob(AN(5), text, out.size + 1); free(text);
    vkd3d_shader_free_shader_code(&out);
    RET(S_OK);
}
SHIM(D3DCreateBlob) { if (!A2) RET(E_INVALIDARG); put_blob(A2, NULL, A1); RET(S_OK); }

// DXBC containers: extract signature parts (D3DGetBlobPart) or drop debug/reflection chunks (D3DStripShader).
static u32 dxbc_filter(const void *src, size_t size, int (*keep)(u32 tag, void *ctx), void *ctx, u64 out) {
    struct vkd3d_shader_code in = { src, size };
    struct vkd3d_shader_dxbc_desc desc; char *messages = NULL;
    if (vkd3d_shader_parse_dxbc(&in, 0, &desc, &messages) < 0) { vkd3d_shader_free_messages(messages); return E_FAIL; }
    vkd3d_shader_free_messages(messages);
    struct vkd3d_shader_dxbc_section_desc *kept = calloc(desc.section_count + 1, sizeof *kept);
    size_t n = 0;
    for (size_t i = 0; i < desc.section_count; i++) if (keep(desc.sections[i].tag, ctx)) kept[n++] = desc.sections[i];
    u32 hr = E_FAIL;
    if (n) {
        struct vkd3d_shader_code res = { 0 };
        if (vkd3d_shader_serialize_dxbc(n, kept, &res, NULL) >= 0) { put_blob(out, res.code, res.size); vkd3d_shader_free_shader_code(&res); hr = S_OK; }
    }
    free(kept); vkd3d_shader_free_dxbc(&desc);
    return hr;
}
#define TAG(a, b, c_, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c_) << 16 | (u32)(d) << 24)
static int keep_part(u32 tag, void *ctx) {
    u32 part = *(u32 *)ctx;  // D3D_BLOB_INPUT_SIGNATURE_BLOB=0, OUTPUT=1, INPUT_AND_OUTPUT=2, PATCH_CONSTANT=3
    switch (part) {
    case 0: return tag == TAG('I', 'S', 'G', 'N') || tag == TAG('I', 'S', 'G', '1');
    case 1: return tag == TAG('O', 'S', 'G', 'N') || tag == TAG('O', 'S', 'G', '1') || tag == TAG('O', 'S', 'G', '5');
    case 2: return keep_part(tag, &(u32){0}) || keep_part(tag, &(u32){1});
    case 3: return tag == TAG('P', 'C', 'S', 'G') || tag == TAG('P', 'S', 'G', '1');
    default: return 0;
    }
}
static int keep_stripped(u32 tag, void *ctx) {
    u32 flags = *(u32 *)ctx;  // D3DCOMPILER_STRIP_REFLECTION_DATA=1, STRIP_DEBUG_INFO=2
    if ((flags & 1) && (tag == TAG('R', 'D', 'E', 'F') || tag == TAG('S', 'T', 'A', 'T'))) return 0;
    if ((flags & 2) && (tag == TAG('S', 'D', 'B', 'G') || tag == TAG('S', 'P', 'D', 'B'))) return 0;
    return 1;
}
SHIM(D3DGetBlobPart) { u32 part = (u32)A3; if (A4) RET(E_INVALIDARG); RET(dxbc_filter((void *)A1, A2, keep_part, &part, AN(5))); }
SHIM(D3DGetInputSignatureBlob) { u32 part = 0; RET(dxbc_filter((void *)A1, A2, keep_part, &part, A3)); }
SHIM(D3DGetOutputSignatureBlob) { u32 part = 1; RET(dxbc_filter((void *)A1, A2, keep_part, &part, A3)); }
SHIM(D3DGetInputAndOutputSignatureBlob) { u32 part = 2; RET(dxbc_filter((void *)A1, A2, keep_part, &part, A3)); }
SHIM(D3DStripShader) { u32 flags = (u32)A3; RET(dxbc_filter((void *)A1, A2, keep_stripped, &flags, A4)); }

const Shim shims_d3dcompiler[] = {
#define E(n) { NULL, #n, sh_##n }
    E(D3DCompile), E(D3DCompile2), E(D3DCompileFromFile), E(D3DPreprocess), E(D3DDisassemble), E(D3DCreateBlob),
    E(D3DGetBlobPart), E(D3DGetInputSignatureBlob), E(D3DGetOutputSignatureBlob), E(D3DGetInputAndOutputSignatureBlob),
    E(D3DStripShader),   /* D3DReflect: see d3dreflect.c */
    { NULL, "__wintomac_Blob_QueryInterface", sh_Blob_QueryInterface }, { NULL, "__wintomac_Blob_AddRef", sh_Blob_AddRef },
    { NULL, "__wintomac_Blob_Release", sh_Blob_Release }, { NULL, "__wintomac_Blob_GetBufferPointer", sh_Blob_GetBufferPointer },
    { NULL, "__wintomac_Blob_GetBufferSize", sh_Blob_GetBufferSize },
#undef E
    { 0, 0, 0 }
};
