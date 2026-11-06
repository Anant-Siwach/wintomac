// Vulkan bridge: guest (Windows x64) Vulkan -> host Vulkan loader -> MoltenVK -> Metal.
//
// Plain commands are generated (runtime/gen/vulkan_thunks.c). Hand-written here: everything where Windows
// and macOS differ (surfaces, portability), where the guest asks for function pointers, or where the
// host must call back into guest code (debug messengers).
#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_bridge.h"

unsigned vk_generation = 1;
static VkInstance g_instance;
static VkDevice g_device;

void (*vk_host_proc(const char *name))(void) {
    PFN_vkVoidFunction f = vkGetInstanceProcAddr(g_instance, name);
    if (!f && g_device) f = vkGetDeviceProcAddr(g_device, name);
    return f;
}

void vk_missing(const char *name) {
    static int warned;
    if (warned++ < 20) rt_log("vulkan: host has no %s", name);
}

static int vk_trace(void) { static int t = -1; if (t < 0) t = getenv("WINTOMAC_VK_TRACE") != NULL; return t; }

// Names the guest may ask for even though the host spells them differently.
static int is_win32_only(const char *n) {
    return !strcmp(n, "vkCreateWin32SurfaceKHR") || !strcmp(n, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
}

// ---- function pointer queries -----------------------------------------------------
SHIM(vkGetInstanceProcAddr) {
    VkInstance inst = (VkInstance)A1; const char *name = (const char *)A2;
    u64 r = 0;
    if (is_win32_only(name) || !strcmp(name, "vkGetInstanceProcAddr") || vkGetInstanceProcAddr(inst, name))
        r = rt_thunk_for(NULL, name);
    if (vk_trace() || (!r && getenv("WINTOMAC_VERBOSE"))) rt_log("vkGetInstanceProcAddr(%s) = %llx", name, (unsigned long long)r);
    RET(r);
}
SHIM(vkGetDeviceProcAddr) {
    VkDevice dev = (VkDevice)A1; const char *name = (const char *)A2;
    u64 r = (vkGetDeviceProcAddr(dev, name) || !strcmp(name, "vkGetDeviceProcAddr")) ? rt_thunk_for(NULL, name) : 0;
    if (vk_trace()) rt_log("vkGetDeviceProcAddr(%s) = %llx", name, (unsigned long long)r);
    RET(r);
}

// ---- instance -------------------------------------------------------------------
static int host_has_instance_ext(const char *name) {
    uint32_t n = 0; vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *p = calloc(n, sizeof *p); vkEnumerateInstanceExtensionProperties(NULL, &n, p);
    int found = 0; for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, name)) found = 1;
    free(p); return found;
}

// Unlink pNext structs we can't pass through (they carry guest callbacks); returns a list to relink after.
typedef struct { VkBaseOutStructure *prev, *node; } Unlinked;
static int strip_pnext(void *head, Unlinked *out, int max) {
    int n = 0;
    VkBaseOutStructure *prev = head, *cur = (VkBaseOutStructure *)((VkBaseOutStructure *)head)->pNext;
    while (cur && n < max) {
        if (cur->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT ||
            cur->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT ||
            cur->sType == VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT) {
            out[n].prev = prev; out[n].node = cur; n++;
            prev->pNext = cur->pNext;
        } else prev = cur;
        cur = prev->pNext;
    }
    return n;
}
static void relink(Unlinked *u, int n) { for (int i = n - 1; i >= 0; i--) u[i].prev->pNext = u[i].node; }

SHIM(vkCreateInstance) {  // (const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*)
    VkInstanceCreateInfo ci = *(const VkInstanceCreateInfo *)A1;
    const char **ext = calloc(ci.enabledExtensionCount + 8, sizeof *ext);
    uint32_t n = 0; int want_surface = 0, have_p2 = 0;
    for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) {
        const char *e = ci.ppEnabledExtensionNames[i];
        if (!strcmp(e, "VK_KHR_win32_surface")) { want_surface = 1; continue; }
        if (!strcmp(e, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) have_p2 = 1;
        if (!host_has_instance_ext(e)) { rt_log("vulkan: dropping unsupported instance extension %s", e); continue; }
        ext[n++] = e;
    }
    if (want_surface) ext[n++] = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
    if (host_has_instance_ext(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        ext[n++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    uint32_t api = ci.pApplicationInfo ? ci.pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    if (api < VK_API_VERSION_1_1 && !have_p2) ext[n++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    ci.enabledExtensionCount = n; ci.ppEnabledExtensionNames = ext;
    if (!getenv("WINTOMAC_VK_LAYERS")) { ci.enabledLayerCount = 0; ci.ppEnabledLayerNames = NULL; }
    Unlinked un[8]; int nun = strip_pnext(&ci, un, 8);
    VkResult r = vkCreateInstance(&ci, NULL, (VkInstance *)A3);
    relink(un, nun);
    if (vk_trace() || r) { rt_log("vkCreateInstance = %d (%u extensions)", r, n); for (uint32_t i = 0; i < n; i++) rt_log("  + %s", ext[i]); }
    free(ext);
    if (r == VK_SUCCESS) { g_instance = *(VkInstance *)A3; vk_generation++; }
    RET((i64)r);
}

SHIM(vkDestroyInstance) {
    vkDestroyInstance((VkInstance)A1, NULL);
    if ((VkInstance)A1 == g_instance) { g_instance = NULL; g_device = NULL; vk_generation++; }
    RET(0);
}

// Two-call enumeration idiom shared by the extension queries below.
static VkResult fill_props(VkExtensionProperties *src, uint32_t n, u64 pcount, u64 pprops) {
    if (!pprops) { ST32(pcount, n); return VK_SUCCESS; }
    uint32_t cap = LD32(pcount), k = cap < n ? cap : n;
    memcpy((void *)pprops, src, k * sizeof *src);
    ST32(pcount, k);
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}

SHIM(vkEnumerateInstanceExtensionProperties) {  // (layer, *count, *props)
    uint32_t n = 0; vkEnumerateInstanceExtensionProperties((const char *)A1, &n, NULL);
    VkExtensionProperties *p = calloc(n + 1, sizeof *p);
    vkEnumerateInstanceExtensionProperties((const char *)A1, &n, p);
    int has_metal = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, VK_EXT_METAL_SURFACE_EXTENSION_NAME)) has_metal = 1;
    if (has_metal) { strcpy(p[n].extensionName, "VK_KHR_win32_surface"); p[n].specVersion = 6; n++; }
    VkResult r = fill_props(p, n, A2, A3); free(p); RET((i64)r);
}

SHIM(vkEnumerateDeviceExtensionProperties) {  // (phys, layer, *count, *props)
    uint32_t n = 0; vkEnumerateDeviceExtensionProperties((VkPhysicalDevice)A1, (const char *)A2, &n, NULL);
    VkExtensionProperties *p = calloc(n + 1, sizeof *p);
    vkEnumerateDeviceExtensionProperties((VkPhysicalDevice)A1, (const char *)A2, &n, p);
    VkResult r = fill_props(p, n, A3, A4); free(p); RET((i64)r);
}

// ---- device ---------------------------------------------------------------------
// Translation layers on macOS ask for core features Metal lacks (geometryShader, shaderCullDistance, ...).
// Request only what the device supports; log what we dropped so a later failure is explainable.
static const char *const feature_names[] = {
    "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader",
    "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect",
    "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines",
    "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy", "textureCompressionETC2",
    "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery",
    "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize",
    "shaderImageGatherExtended", "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample",
    "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
    "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance",
    "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency",
    "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D",
    "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples",
    "sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries" };
static void clamp_features(VkPhysicalDevice phys, VkPhysicalDeviceFeatures *want) {
    VkPhysicalDeviceFeatures have; vkGetPhysicalDeviceFeatures(phys, &have);
    VkBool32 *w = (VkBool32 *)want; const VkBool32 *h = (const VkBool32 *)&have;
    for (size_t i = 0; i < sizeof have / sizeof(VkBool32); i++)
        if (w[i] && !h[i]) { w[i] = VK_FALSE; rt_log("vulkan: device feature %s not supported here, not enabling it", i < 55 ? feature_names[i] : "?"); }
}

// Extension feature structs (all VkBool32 after sType/pNext): a request for something MoltenVK lacks
// (robustness2.nullDescriptor, mesh shaders, 64-bit image atomics, ...) fails vkCreateDevice with
// VK_ERROR_FEATURE_NOT_PRESENT, so each is ANDed with what the host reports, like clamp_features.
// Translation layers that query features first (AnyPS5's AgcDriver does) never notice; the log says what was dropped.
static const struct { VkStructureType t; size_t size; const char *name; } feature_structs[] = {
#define F(st, ty) { st, sizeof(ty), #ty }
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, VkPhysicalDeviceRobustness2FeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, VkPhysicalDeviceMeshShaderFeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, VkPhysicalDeviceShaderAtomicInt64Features),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT, VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT, VkPhysicalDeviceImageViewMinLodFeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR, VkPhysicalDeviceMaintenance8FeaturesKHR),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, VkPhysicalDeviceDepthClipControlFeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR, VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicStateFeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, VkPhysicalDeviceTransformFeedbackFeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, VkPhysicalDeviceCustomBorderColorFeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT),
    F(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, VkPhysicalDeviceDescriptorBufferFeaturesEXT),
#undef F
};
typedef struct { VkBaseOutStructure *node; u8 saved[256]; size_t size; } SavedFeat;
// Clamp every known feature struct in the chain; returns how many were patched (restored by restore_feature_structs).
static int clamp_feature_structs(VkPhysicalDevice phys, const void *chain, SavedFeat *sv, int max) {
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2");
    if (!gf2) return 0;
    int n = 0;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)chain; p && n < max; p = p->pNext)
        for (size_t k = 0; k < sizeof feature_structs / sizeof *feature_structs; k++) {
            if (feature_structs[k].t != p->sType || feature_structs[k].size > sizeof sv->saved) continue;
            u8 have[256] = { 0 };
            ((VkBaseOutStructure *)have)->sType = p->sType;
            VkPhysicalDeviceFeatures2 q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, have };
            gf2(phys, &q);
            sv[n].node = p; sv[n].size = feature_structs[k].size; memcpy(sv[n].saved, p, sv[n].size); n++;
            VkBool32 *w = (VkBool32 *)((u8 *)p + sizeof(VkBaseOutStructure)), *h = (VkBool32 *)(have + sizeof(VkBaseOutStructure));
            for (size_t i = 0; i < (feature_structs[k].size - sizeof(VkBaseOutStructure)) / sizeof(VkBool32); i++)
                if (w[i] && !h[i]) { w[i] = VK_FALSE; rt_log("vulkan: %s member %zu not supported here, not enabling it", feature_structs[k].name, i); }
        }
    return n;
}
static void restore_feature_structs(SavedFeat *sv, int n) {
    for (int i = 0; i < n; i++) { void *next = sv[i].node->pNext; memcpy(sv[i].node, sv[i].saved, sv[i].size); sv[i].node->pNext = next; }
}

SHIM(vkCreateDevice) {  // (phys, const VkDeviceCreateInfo*, alloc, VkDevice*)
    VkPhysicalDevice phys = (VkPhysicalDevice)A1;
    VkDeviceCreateInfo ci = *(const VkDeviceCreateInfo *)A2;
    uint32_t n = 0; vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
    VkExtensionProperties *p = calloc(n, sizeof *p); vkEnumerateDeviceExtensionProperties(phys, NULL, &n, p);
    int portability = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, "VK_KHR_portability_subset")) portability = 1;
    const char **ext = calloc(ci.enabledExtensionCount + 2, sizeof *ext);
    uint32_t k = 0;
    for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) {
        const char *e = ci.ppEnabledExtensionNames[i];
        if (!strcmp(e, "VK_KHR_portability_subset")) portability = 0;  // guest asked already
        int have = 0; for (uint32_t j = 0; j < n; j++) if (!strcmp(p[j].extensionName, e)) have = 1;
        if (!have) { rt_log("vulkan: dropping unsupported device extension %s", e); continue; }
        ext[k++] = e;
    }
    free(p);
    if (portability) ext[k++] = "VK_KHR_portability_subset";  // required by spec when the device exposes it
    ci.enabledExtensionCount = k; ci.ppEnabledExtensionNames = ext;
    ci.enabledLayerCount = 0; ci.ppEnabledLayerNames = NULL;
    VkPhysicalDeviceFeatures core;
    if (ci.pEnabledFeatures) { core = *ci.pEnabledFeatures; clamp_features(phys, &core); ci.pEnabledFeatures = &core; }
    VkPhysicalDeviceFeatures2 *f2 = NULL; VkPhysicalDeviceFeatures saved;  // guest-owned: patch, call, restore
    for (VkBaseOutStructure *n = (VkBaseOutStructure *)ci.pNext; n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) { f2 = (VkPhysicalDeviceFeatures2 *)n; saved = f2->features; clamp_features(phys, &f2->features); }
    SavedFeat sv[24]; int nsv = clamp_feature_structs(phys, ci.pNext, sv, 24);
    VkResult r = vkCreateDevice(phys, &ci, NULL, (VkDevice *)A4);
    restore_feature_structs(sv, nsv);
    if (f2) f2->features = saved;
    if (vk_trace() || r) rt_log("vkCreateDevice = %d (%u extensions)", r, k);
    free(ext);
    if (r == VK_SUCCESS) g_device = *(VkDevice *)A4;
    RET((i64)r);
}

// ---- Win32 surfaces -> Metal surfaces --------------------------------------------------
// Surfaces created from an HWND report currentExtent = 0xFFFFFFFF ("the swapchain decides").
// The CAMetalLayer's own size rarely equals the size the game believes its window has (virtual
// display modes, the notch strip macOS leaves out in fullscreen, a resize still in flight), and
// games such as vkQuake refuse to build a swapchain until the two match exactly, so they would
// skip every frame. With the "undefined" extent the game uses its own size and the layer scales
// the image to fit. The Vulkan spec requires games to handle this value (Wayland always reports it).
static VkSurfaceKHR g_surfaces[16];
static void surface_remember(VkSurfaceKHR s) {
    for (int i = 0; i < 16; i++) if (!g_surfaces[i] || g_surfaces[i] == s) { g_surfaces[i] = s; return; }
}
static void surface_fix_extent(VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *cp) {
    for (int i = 0; i < 16; i++) if (g_surfaces[i] == s) {
        cp->currentExtent.width = cp->currentExtent.height = 0xFFFFFFFFu;
        if (cp->minImageExtent.width < 1) cp->minImageExtent.width = 1;
        if (cp->minImageExtent.height < 1) cp->minImageExtent.height = 1;
        return;
    }
}
SHIM(vkCreateWin32SurfaceKHR) {  // (instance, const VkWin32SurfaceCreateInfoKHR*, alloc, VkSurfaceKHR*)
    u64 hwnd = LD64(A2 + 32);    // {sType, pNext, flags, hinstance, hwnd}
    void *layer = user32_metal_layer(hwnd);
    if (!layer) { rt_log("vkCreateWin32SurfaceKHR: unknown HWND %llx", (unsigned long long)hwnd); RET((i64)VK_ERROR_INITIALIZATION_FAILED); }
    VkMetalSurfaceCreateInfoEXT mi = { VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, NULL, 0, layer };
    PFN_vkCreateMetalSurfaceEXT f = (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr((VkInstance)A1, "vkCreateMetalSurfaceEXT");
    if (!f) RET((i64)VK_ERROR_EXTENSION_NOT_PRESENT);
    VkResult r = f((VkInstance)A1, &mi, NULL, (VkSurfaceKHR *)A4);
    if (r == VK_SUCCESS) surface_remember(*(VkSurfaceKHR *)A4);
    RET((i64)r);
}
SHIM(vkGetPhysicalDeviceWin32PresentationSupportKHR) { RET(VK_TRUE); }

// ---- debug messengers: host calls back into guest code ---------------------------------
typedef struct { u64 fn, user; } GuestCb;
static VkBool32 VKAPI_CALL debug_tramp(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT types,
                                       const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    GuestCb *g = user;
    if (!rt_cur_ctx) {  // callback on a driver thread with no guest context: just log it
        fprintf(stderr, "[vulkan] %s\n", data && data->pMessage ? data->pMessage : "(no message)");
        return VK_FALSE;
    }
    u64 args[4] = { sev, types, (u64)data, g->user };
    return (VkBool32)rt_guest_call(rt_cur_ctx, g->fn, 4, args);
}
SHIM(vkCreateDebugUtilsMessengerEXT) {  // (instance, const CreateInfo*, alloc, *messenger)
    PFN_vkCreateDebugUtilsMessengerEXT f = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr((VkInstance)A1, "vkCreateDebugUtilsMessengerEXT");
    if (!f) RET((i64)VK_ERROR_EXTENSION_NOT_PRESENT);
    VkDebugUtilsMessengerCreateInfoEXT ci = *(const VkDebugUtilsMessengerCreateInfoEXT *)A2;
    GuestCb *g = malloc(sizeof *g); g->fn = (u64)ci.pfnUserCallback; g->user = (u64)ci.pUserData;
    ci.pfnUserCallback = debug_tramp; ci.pUserData = g;
    RET((i64)f((VkInstance)A1, &ci, NULL, (VkDebugUtilsMessengerEXT *)A4));
}
SHIM(vkDestroyDebugUtilsMessengerEXT) {
    PFN_vkDestroyDebugUtilsMessengerEXT f = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr((VkInstance)A1, "vkDestroyDebugUtilsMessengerEXT");
    if (f) f((VkInstance)A1, (VkDebugUtilsMessengerEXT)A2, NULL);
    RET(0);
}
// Debug report (deprecated): accept and never call back.
SHIM(vkCreateDebugReportCallbackEXT) { static u64 next = 1; ST64(A4, next++); RET(VK_SUCCESS); }
SHIM(vkDestroyDebugReportCallbackEXT) { RET(0); }

// Surface size as MoltenVK reports it (from the CAMetalLayer's drawable size). Logged with WINTOMAC_VK_TRACE.
SHIM(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) {  // (phys, surface, caps*)
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR((VkPhysicalDevice)A1, (VkSurfaceKHR)A2, (VkSurfaceCapabilitiesKHR *)A3);
    VkSurfaceCapabilitiesKHR *cp = (VkSurfaceCapabilitiesKHR *)A3;
    if (vk_trace()) rt_log("surface caps: layer reports %ux%u", cp->currentExtent.width, cp->currentExtent.height);
    if (r == VK_SUCCESS) surface_fix_extent((VkSurfaceKHR)A2, cp);
    if (vk_trace()) rt_log("surface caps = %d: current %ux%u, min %ux%u, max %ux%u, images %u..%u", r,
        cp->currentExtent.width, cp->currentExtent.height, cp->minImageExtent.width, cp->minImageExtent.height,
        cp->maxImageExtent.width, cp->maxImageExtent.height, cp->minImageCount, cp->maxImageCount);
    RET((i64)r);
}
SHIM(vkGetPhysicalDeviceSurfaceCapabilities2KHR) {  // (phys, const SurfaceInfo2*, Capabilities2*)
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR f = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)vk_host_proc("vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    if (!f) RET((i64)VK_ERROR_EXTENSION_NOT_PRESENT);
    const VkPhysicalDeviceSurfaceInfo2KHR *in = (const VkPhysicalDeviceSurfaceInfo2KHR *)A2;
    VkSurfaceCapabilities2KHR *out = (VkSurfaceCapabilities2KHR *)A3;
    VkResult r = f((VkPhysicalDevice)A1, in, out);
    if (r == VK_SUCCESS) surface_fix_extent(in->surface, &out->surfaceCapabilities);
    if (vk_trace()) rt_log("surface caps2 = %d: current %ux%u", r, out->surfaceCapabilities.currentExtent.width, out->surfaceCapabilities.currentExtent.height);
    RET((i64)r);
}

// ---- swapchain / present ------------------------------------------------------------
// The extent we report follows the game's window, which can differ from the CAMetalLayer's
// own size (virtual display modes, a fullscreen transition still in flight). MoltenVK then
// answers VK_SUBOPTIMAL_KHR on every acquire/present; games treat that as "rebuild the
// swapchain" and skip frames forever. The image still presents correctly (the layer scales
// it), so suboptimal is reported as success. Unusual results are logged (the first 40, or all
// with WINTOMAC_VK_TRACE).
static void vk_note(const char *what, VkResult r) {
    static int n;
    if (r != VK_SUCCESS && (vk_trace() || n++ < 40)) rt_log("%s = %d", what, r);
}
SHIM(vkCreateSwapchainKHR) {  // (device, createInfo, alloc, *swapchain)
    PFN_vkCreateSwapchainKHR f = (PFN_vkCreateSwapchainKHR)vk_host_proc("vkCreateSwapchainKHR");
    if (!f) RET((i64)VK_ERROR_EXTENSION_NOT_PRESENT);
    const VkSwapchainCreateInfoKHR *ci = (const VkSwapchainCreateInfoKHR *)A2;
    VkResult r = f((VkDevice)A1, ci, NULL, (VkSwapchainKHR *)A4);
    if (vk_trace() || r) rt_log("vkCreateSwapchainKHR = %d: %ux%u, %u images, present mode %d", r,
        ci->imageExtent.width, ci->imageExtent.height, ci->minImageCount, ci->presentMode);
    RET((i64)r);
}
SHIM(vkAcquireNextImageKHR) {  // (device, swapchain, timeout, semaphore, fence, *index)
    static PFN_vkAcquireNextImageKHR f;
    if (!f) f = (PFN_vkAcquireNextImageKHR)vk_host_proc("vkAcquireNextImageKHR");
    if (!f) RET((i64)VK_ERROR_EXTENSION_NOT_PRESENT);
    VkResult r = f((VkDevice)A1, (VkSwapchainKHR)A2, (uint64_t)A3, (VkSemaphore)A4, (VkFence)AN(5), (uint32_t *)AN(6));
    vk_note("vkAcquireNextImageKHR", r);
    if (r == VK_SUBOPTIMAL_KHR) r = VK_SUCCESS;
    RET((i64)r);
}
SHIM(vkQueuePresentKHR) {  // (queue, presentInfo)
    static PFN_vkQueuePresentKHR f;
    if (!f) f = (PFN_vkQueuePresentKHR)vk_host_proc("vkQueuePresentKHR");
    if (!f) RET((i64)VK_ERROR_EXTENSION_NOT_PRESENT);
    const VkPresentInfoKHR *pi = (const VkPresentInfoKHR *)A2;
    VkResult r = f((VkQueue)A1, pi);
    vk_note("vkQueuePresentKHR", r);
    if (r == VK_SUBOPTIMAL_KHR) r = VK_SUCCESS;
    if (pi->pResults) for (uint32_t i = 0; i < pi->swapchainCount; i++) if (pi->pResults[i] == VK_SUBOPTIMAL_KHR) pi->pResults[i] = VK_SUCCESS;
    RET((i64)r);
}
SHIM(vkWaitForPresent2KHR) {  // (device, swapchain, waitInfo)
    static PFN_vkWaitForPresent2KHR f;
    if (!f) f = (PFN_vkWaitForPresent2KHR)vk_host_proc("vkWaitForPresent2KHR");
    if (!f) RET((i64)VK_ERROR_EXTENSION_NOT_PRESENT);
    VkResult r = f((VkDevice)A1, (VkSwapchainKHR)A2, (const VkPresentWait2InfoKHR *)A3);
    vk_note("vkWaitForPresent2KHR", r);
    if (r == VK_SUBOPTIMAL_KHR) r = VK_SUCCESS;
    RET((i64)r);
}


// ---- push constants (WINTOMAC_VK_TRACE logs them; see the pre-rasterization stage note in docs/ps5_graphics.md) ----
SHIM(vkCreatePipelineLayout) {  // (device, createInfo, alloc, *layout)
    const VkPipelineLayoutCreateInfo *ci = (const VkPipelineLayoutCreateInfo *)A2;
    VkResult r = vkCreatePipelineLayout((VkDevice)A1, ci, NULL, (VkPipelineLayout *)A4);
    if (vk_trace()) {
        rt_log("vkCreatePipelineLayout = %d: %u sets, %u push ranges -> %llx", r, ci->setLayoutCount, ci->pushConstantRangeCount, (unsigned long long)*(VkPipelineLayout *)A4);
        for (uint32_t i = 0; i < ci->pushConstantRangeCount; i++)
            rt_log("  push range stages 0x%x offset %u size %u", ci->pPushConstantRanges[i].stageFlags, ci->pPushConstantRanges[i].offset, ci->pPushConstantRanges[i].size);
    }
    RET((i64)r);
}
SHIM(vkCmdPushConstants) {  // (cmd, layout, stages, offset, size, values)
    static PFN_vkCmdPushConstants f;
    if (!f) f = (PFN_vkCmdPushConstants)vk_host_proc("vkCmdPushConstants");
    VkShaderStageFlags st = (VkShaderStageFlags)A3; uint32_t off = (uint32_t)A4, size = (uint32_t)AN(5); const void *vals = (const void *)AN(6);
    if (vk_trace()) {
        const uint32_t *w = vals; char buf[600]; int n = 0;
        for (uint32_t i = 0; i < size / 4 && n < (int)sizeof buf - 12; i++) n += snprintf(buf + n, sizeof buf - n, " %08x", w[i]);
        rt_log("vkCmdPushConstants(layout %llx, stages 0x%x, offset %u, size %u)%s", (unsigned long long)A2, st, off, size, buf);
    }
    f((VkCommandBuffer)A1, (VkPipelineLayout)A2, st, off, size, vals);
    RET(0);
}

static void trace_stages(const char *what, uint32_t n, const VkPipelineShaderStageCreateInfo *st) {
    for (uint32_t i = 0; i < n; i++) {
        const VkSpecializationInfo *si = st[i].pSpecializationInfo;
        rt_log("  %s stage 0x%x module %llx entry %s spec %u entries %zu bytes", what, st[i].stage, (unsigned long long)st[i].module,
               st[i].pName ? st[i].pName : "?", si ? si->mapEntryCount : 0, si ? si->dataSize : 0);
        for (uint32_t k = 0; si && k < si->mapEntryCount && k < 24; k++) {
            const VkSpecializationMapEntry *e = &si->pMapEntries[k];
            uint32_t v = 0; if (e->offset + 4 <= si->dataSize) memcpy(&v, (const char *)si->pData + e->offset, 4);
            rt_log("    spec id %u offset %u size %zu = %08x", e->constantID, e->offset, e->size, v);
        }
    }
}
SHIM(vkCreateGraphicsPipelines) {  // (device, cache, count, infos, alloc, *pipelines)
    const VkGraphicsPipelineCreateInfo *ci = (const VkGraphicsPipelineCreateInfo *)A4;
    if (vk_trace()) for (uint32_t i = 0; i < (uint32_t)A3; i++) { rt_log("vkCreateGraphicsPipelines[%u] layout %llx", i, (unsigned long long)ci[i].layout); trace_stages("gfx", ci[i].stageCount, ci[i].pStages); }
    VkResult r = vkCreateGraphicsPipelines((VkDevice)A1, (VkPipelineCache)A2, (uint32_t)A3, ci, NULL, (VkPipeline *)AN(6));
    if (vk_trace() || r) rt_log("vkCreateGraphicsPipelines = %d", r);
    RET((i64)r);
}

const Shim shims_vulkan[] = {
#define E(n) { NULL, #n, sh_##n }
    E(vkGetPhysicalDeviceSurfaceCapabilitiesKHR), E(vkGetPhysicalDeviceSurfaceCapabilities2KHR),
    E(vkCreateSwapchainKHR), E(vkAcquireNextImageKHR), E(vkQueuePresentKHR), E(vkWaitForPresent2KHR),
    E(vkGetInstanceProcAddr), E(vkGetDeviceProcAddr), E(vkCreateInstance), E(vkDestroyInstance),
    E(vkEnumerateInstanceExtensionProperties), E(vkEnumerateDeviceExtensionProperties), E(vkCreateDevice),
    E(vkCreateWin32SurfaceKHR), E(vkGetPhysicalDeviceWin32PresentationSupportKHR),
    E(vkCreateDebugUtilsMessengerEXT), E(vkDestroyDebugUtilsMessengerEXT),
    E(vkCreateDebugReportCallbackEXT), E(vkDestroyDebugReportCallbackEXT), E(vkCreatePipelineLayout), E(vkCmdPushConstants), E(vkCreateGraphicsPipelines),
#undef E
    { 0, 0, 0 }
};
