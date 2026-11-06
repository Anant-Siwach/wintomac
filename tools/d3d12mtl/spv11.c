// DXBC/DXIL -> SPIR-V with vkd3d-shader for the Vulkan 1.1 environment (wave ops allowed), as the Metal backend's
// shader path compiles them. Helper for tools/d3d12mtl_shader_matrix.py.  Usage: spv11 in.dxbc out.spv
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vkd3d_shader.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc(n); fread(p, 1, n, f); fclose(f);
    int dxil = 0; unsigned count; memcpy(&count, p + 28, 4);
    for (unsigned i = 0; i < count; i++) { unsigned off; memcpy(&off, p + 32 + 4 * i, 4); if (!memcmp(p + off, "DXIL", 4)) dxil = 1; }
    static const enum vkd3d_shader_spirv_extension ext[] = { VKD3D_SHADER_SPIRV_EXTENSION_EXT_DEMOTE_TO_HELPER_INVOCATION,
        VKD3D_SHADER_SPIRV_EXTENSION_EXT_DESCRIPTOR_INDEXING, VKD3D_SHADER_SPIRV_EXTENSION_EXT_STENCIL_EXPORT };
    struct vkd3d_shader_spirv_target_info ti = { VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO };
    ti.environment = VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_1; ti.extensions = ext; ti.extension_count = 3;
    struct vkd3d_shader_compile_info ci = { VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO, &ti };
    const struct vkd3d_shader_compile_option opts[] = { { VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_1_19 },
        { VKD3D_SHADER_COMPILE_OPTION_FEATURE, VKD3D_SHADER_COMPILE_OPTION_FEATURE_INT64 | VKD3D_SHADER_COMPILE_OPTION_FEATURE_WAVE_OPS } };
    ci.options = opts; ci.option_count = 2;
    ci.source.code = p; ci.source.size = n; ci.source_type = dxil ? VKD3D_SHADER_SOURCE_DXBC_DXIL : VKD3D_SHADER_SOURCE_DXBC_TPF;
    ci.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY; ci.log_level = VKD3D_SHADER_LOG_WARNING;
    struct vkd3d_shader_code out; char *msg = NULL;
    int r = vkd3d_shader_compile(&ci, &out, &msg);
    if (msg && *msg) fputs(msg, stderr);
    if (r < 0) { fprintf(stderr, "error: vkd3d_shader_compile %d\n", r); return 1; }
    f = fopen(argv[2], "wb"); fwrite(out.code, 1, out.size, f); fclose(f);
    return 0;
}
