// Probe the host Vulkan driver (MoltenVK) for the features D3D12 translation layers need.
// Native macOS tool, headless (no surface/window):
//   clang -O1 -I$(brew --prefix)/include tools/d3d12_mvk_probe.c -L$(brew --prefix)/lib -lvulkan -o build/d3d12_mvk_probe
// Results are summarised in docs/d3d12.md.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static VkExtensionProperties *exts; static uint32_t next;
static int has(const char *n) { for (uint32_t i = 0; i < next; i++) if (!strcmp(exts[i].extensionName, n)) return 1; return 0; }
#define YN(x) ((x) ? "yes" : "NO")

int main(int argc, char **argv) {
    const char *ie[] = { VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
                                .pApplicationInfo = &ai, .enabledExtensionCount = 1, .ppEnabledExtensionNames = ie };
    VkInstance inst; if (vkCreateInstance(&ci, NULL, &inst)) { puts("vkCreateInstance failed"); return 1; }
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { puts("no device"); return 1; }
    vkEnumerateDeviceExtensionProperties(pd, NULL, &next, NULL);
    exts = calloc(next, sizeof *exts); vkEnumerateDeviceExtensionProperties(pd, NULL, &next, exts);

    VkPhysicalDeviceRobustness2FeaturesEXT rob = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mut = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, &rob };
    VkPhysicalDeviceDescriptorBufferFeaturesEXT db = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, &mut };
    VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &db };
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13 };
    VkPhysicalDeviceVulkan11Features v11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12 };
    VkPhysicalDeviceFeatures2 f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11 };
    vkGetPhysicalDeviceFeatures2(pd, &f);
    VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 p = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12 };
    vkGetPhysicalDeviceProperties2(pd, &p);
    uint32_t v = p.properties.apiVersion;
    printf("device: %s, Vulkan %u.%u.%u, driver 0x%x\n", p.properties.deviceName, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v), p.properties.driverVersion);

    puts("\n-- vkd3d-proton hard requirements --");
    printf("Vulkan 1.3 core                         %s\n", YN(v >= VK_API_VERSION_1_3));
    printf("maxUpdateAfterBindDescriptorsInAllPools %u (needs >= 1000000)\n", p12.maxUpdateAfterBindDescriptorsInAllPools);
    printf("maxDescriptorSetUpdateAfterBindSampledImages %u, StorageImages %u, StorageBuffers %u, Samplers %u\n",
           p12.maxDescriptorSetUpdateAfterBindSampledImages, p12.maxDescriptorSetUpdateAfterBindStorageImages,
           p12.maxDescriptorSetUpdateAfterBindStorageBuffers, p12.maxDescriptorSetUpdateAfterBindSamplers);
    printf("maxPerStageUpdateAfterBindResources     %u\n", p12.maxPerStageUpdateAfterBindResources);
    printf("descriptorIndexing (all features):      UBO-UAB %s SampledImg-UAB %s StorageImg-UAB %s StorageBuf-UAB %s UTB-UAB %s STB-UAB %s partiallyBound %s variableCount %s runtimeArray %s nonUniformIdx(sampled %s storageImg %s)\n",
           YN(v12.descriptorBindingUniformBufferUpdateAfterBind), YN(v12.descriptorBindingSampledImageUpdateAfterBind),
           YN(v12.descriptorBindingStorageImageUpdateAfterBind), YN(v12.descriptorBindingStorageBufferUpdateAfterBind),
           YN(v12.descriptorBindingUniformTexelBufferUpdateAfterBind), YN(v12.descriptorBindingStorageTexelBufferUpdateAfterBind),
           YN(v12.descriptorBindingPartiallyBound), YN(v12.descriptorBindingVariableDescriptorCount), YN(v12.runtimeDescriptorArray),
           YN(v12.shaderSampledImageArrayNonUniformIndexing), YN(v12.shaderStorageImageArrayNonUniformIndexing));
    printf("samplerMirrorClampToEdge                %s\n", YN(v12.samplerMirrorClampToEdge));
    printf("shaderDrawParameters                    %s\n", YN(v11.shaderDrawParameters));
    printf("VK_EXT_robustness2                      %s  (robustBufferAccess2 %s robustImageAccess2 %s nullDescriptor %s)\n",
           YN(has("VK_EXT_robustness2") || has("VK_KHR_robustness2")), YN(rob.robustBufferAccess2), YN(rob.robustImageAccess2), YN(rob.nullDescriptor));
    printf("VK_KHR_push_descriptor                  %s\n", YN(has("VK_KHR_push_descriptor")));

    puts("\n-- vkd3d-proton recommended / commonly needed --");
    printf("VK_EXT_mutable_descriptor_type          %s (feature %s)\n", YN(has("VK_EXT_mutable_descriptor_type") || has("VK_VALVE_mutable_descriptor_type")), YN(mut.mutableDescriptorType));
    printf("VK_EXT_descriptor_buffer                %s (feature %s)\n", YN(has("VK_EXT_descriptor_buffer")), YN(db.descriptorBuffer));
    printf("VK_EXT_image_view_min_lod               %s\n", YN(has("VK_EXT_image_view_min_lod")));
    printf("bufferDeviceAddress                     %s\n", YN(v12.bufferDeviceAddress));
    printf("timelineSemaphore                       %s\n", YN(v12.timelineSemaphore));
    printf("dynamicRendering / synchronization2     %s / %s\n", YN(v13.dynamicRendering), YN(v13.synchronization2));
    printf("shaderDemoteToHelperInvocation          %s\n", YN(v13.shaderDemoteToHelperInvocation));
    printf("shaderInt64 / Int16 / Float16           %s / %s / %s\n", YN(f.features.shaderInt64), YN(f.features.shaderInt16), YN(v12.shaderFloat16));
    printf("geometryShader / tessellationShader     %s / %s\n", YN(f.features.geometryShader), YN(f.features.tessellationShader));
    printf("sparseBinding / sparseResidencyImage2D  %s / %s\n", YN(f.features.sparseBinding), YN(f.features.sparseResidencyImage2D));
    printf("storage read/write without format       %s / %s\n", YN(f.features.shaderStorageImageReadWithoutFormat), YN(f.features.shaderStorageImageWriteWithoutFormat));
    const char *more[] = { "VK_EXT_transform_feedback", "VK_EXT_conditional_rendering", "VK_EXT_depth_clip_enable", "VK_EXT_custom_border_color",
                           "VK_EXT_extended_dynamic_state2", "VK_EXT_extended_dynamic_state3", "VK_EXT_shader_stencil_export",
                           "VK_EXT_fragment_shader_interlock", "VK_EXT_vertex_attribute_divisor", "VK_KHR_vertex_attribute_divisor",
                           "VK_EXT_sampler_filter_minmax", "VK_EXT_shader_viewport_index_layer", "VK_EXT_depth_bias_control",
                           "VK_KHR_maintenance5", "VK_KHR_maintenance6", "VK_EXT_mesh_shader", "VK_KHR_ray_tracing_pipeline",
                           "VK_KHR_fragment_shading_rate", "VK_KHR_shader_draw_parameters", "VK_KHR_maintenance1", "VK_KHR_maintenance2",
                           "VK_KHR_maintenance3", "VK_KHR_timeline_semaphore", "VK_EXT_descriptor_indexing", "VK_KHR_portability_subset", NULL };
    for (int i = 0; more[i]; i++) printf("%-40s%s\n", more[i], YN(has(more[i])));

    // Shader Model 6.x: what the SPIR-V that vkd3d-shader emits for each SM 6 feature needs.
    puts("\n-- Shader Model 6.x prerequisites --");
    VkPhysicalDeviceShaderAtomicInt64Features a64 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES };
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT ia64 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT, &a64 };
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR, &ia64 };
    VkPhysicalDevice16BitStorageFeatures s16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &bary };
    VkPhysicalDeviceRayQueryFeaturesKHR rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &s16 };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &rq };
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkPhysicalDeviceSubgroupProperties sg = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg };
    vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("subgroup size %u, stages %#x, ops %#x (basic/vote/arith/ballot/shuffle/rel/clustered/quad = 1/2/4/8/16/32/64/128)\n",
           sg.subgroupSize, sg.supportedStages, sg.supportedOperations);
    printf("shaderBufferInt64Atomics / Shared       %s / %s\n", YN(a64.shaderBufferInt64Atomics), YN(a64.shaderSharedInt64Atomics));
    printf("shaderImageInt64Atomics                 %s\n", YN(ia64.shaderImageInt64Atomics));
    printf("fragmentShaderBarycentric               %s\n", YN(bary.fragmentShaderBarycentric));
    printf("storageBuffer16BitAccess / uniform      %s / %s\n", YN(s16.storageBuffer16BitAccess), YN(s16.uniformAndStorageBuffer16BitAccess));
    printf("rayQuery                                %s\n", YN(rq.rayQuery));
    printf("VK_KHR_draw_indirect_count              %s\n", YN(has("VK_KHR_draw_indirect_count")));
    if (argc > 1 && !strcmp(argv[1], "--all"))
        for (uint32_t i = 0; i < next; i++) printf("ext %s\n", exts[i].extensionName);
    vkDestroyInstance(inst, NULL);
    return 0;
}
