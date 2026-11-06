// DXGI presentation for D3D12 command queues.
//
// Games create D3D12 swapchains through dxgi.dll (we recompile DXVK's, which also gives them adapters, outputs and
// display modes): IDXGIFactory2::CreateSwapChainForHwnd(queue, ...). DXVK 1.10 asks the "device" (our queue
// wrapper) for IWineDXGISwapChainFactory and lets it build the whole IDXGISwapChain4; DXVK only wraps it in its
// dispatcher. That object is implemented here, host-side:
//   * back buffers are VkImages we allocate on libvkd3d's VkDevice and hand to the game as ID3D12Resources
//     (vkd3d_create_image_resource), with D3D12 PRESENT/COMMON mapped to TRANSFER_SRC_OPTIMAL;
//   * Present signals an ID3D12Fence on the game's queue and hands the frame to a presenter thread, which waits
//     for that fence, then blits the back buffer into a VkSwapchainKHR on the window's CAMetalLayer, on libvkd3d's
//     VkQueue (vkd3d_acquire_vk_queue). The game's thread returns at once, so CPU and GPU overlap. It only waits
//     (a) before reusing a back buffer whose blit hasn't been submitted yet and (b) for the frame latency limit
//     (SetMaximumFrameLatency, default 3) unless the game uses the waitable object, which is signalled per
//     presented frame. WINTOMAC_DXGI_SYNC_PRESENT=1 (read at swapchain creation) makes Present synchronous again.
// The design follows Wine's dxgi d3d12 swapchain (wine/dlls/dxgi/swapchain.c, LGPL), including its worker thread.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define VK_USE_PLATFORM_METAL_EXT 1
#include "d3d12_internal.h"

#define S_FALSE_ 1u
#define E_INVALIDARG_ 0x80070057u
#define E_NOINTERFACE_ 0x80004002u
#define E_POINTER_ 0x80004003u
#define DXGI_ERROR_INVALID_CALL_ 0x887A0001u
#define DXGI_ERROR_NOT_FOUND_ 0x887A0002u
#define DXGI_ERROR_UNSUPPORTED_ 0x887A0004u
#define DXGI_ERROR_FRAME_STATISTICS_DISJOINT_ 0x887A000Bu
#define DXGI_PRESENT_TEST_ 1u
#define DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT_ 64u

// DXGI structs (layouts as in dxgi.h / dxgi1_2.h; all fields 32-bit except the HWND)
typedef struct { u32 Width, Height, Format, Stereo, SampleCount, SampleQuality, BufferUsage, BufferCount, Scaling, SwapEffect, AlphaMode, Flags; } Desc1;
typedef struct { u32 RefreshNum, RefreshDen, ScanlineOrdering, Scaling, Windowed; } FsDesc;
typedef struct {
    u32 Width, Height, RefreshNum, RefreshDen, Format, ScanlineOrdering, Scaling;   // DXGI_MODE_DESC
    u32 SampleCount, SampleQuality, BufferUsage, BufferCount;
    u64 OutputWindow; u32 Windowed, SwapEffect, Flags;
} SwapDesc;
_Static_assert(sizeof(Desc1) == 48 && sizeof(FsDesc) == 20 && sizeof(SwapDesc) == 72, "DXGI layouts");

enum { MAX_BUFFERS = 16 };
typedef struct {
    u64 vtbl; u32 refs;
    pthread_mutex_t lock;
    u64 factory;                       // guest IDXGIFactory (DXVK's), referenced
    ID3D12CommandQueue *queue; ID3D12Device *device;
    u64 hwnd; Desc1 desc; FsDesc fs;
    // back buffers
    struct { VkImage img; VkDeviceMemory mem; ID3D12Resource *res; } bb[MAX_BUFFERS];
    u32 cur;
    ID3D12Fence *fence; u64 fence_value;
    // presentation
    VkInstance vki; VkPhysicalDevice pd; VkDevice dev;
    VkSurfaceKHR surface; VkSwapchainKHR sc; VkImage sc_img[MAX_BUFFERS]; u32 n_sc; VkExtent2D sc_extent;
    VkPresentModeKHR sc_mode; int sc_dirty, present_failed;
    VkCommandPool pool; VkCommandBuffer cmd; VkFence vkfence;
    // state
    u32 present_count, max_latency, fullscreen, saved_style; i32 saved_rect[4];
    // presenter thread: a ring of queued frames. pushed/done count frames since creation.
    pthread_t thread; pthread_cond_t cond; int quit, sync_present, has_thread;
    struct { u32 buffer, sync; u64 fence_value; } ring[2 * MAX_BUFFERS + 4];
    u64 pushed, done;
    // frame latency waitable: an auto-reset guest event, signalled while frames are available (avail > 0)
    u64 latency_event; i32 avail;
    u32 bg[4], rotation, src_w, src_h;
} SwapChain;

typedef struct { u64 vtbl; u32 refs; ID3D12CommandQueue *queue; } Factory;

static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID sc_iids[] = {
    { 0xaec22fb8, 0x76f3, 0x4639, { 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e } },   // IDXGIObject
    { 0x3d3e0379, 0xf9de, 0x4d58, { 0xbb, 0x6c, 0x18, 0xd6, 0x29, 0x92, 0xf1, 0xa6 } },   // IDXGIDeviceSubObject
    { 0x310d36a0, 0xd2e7, 0x4c0a, { 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a } },   // IDXGISwapChain
    { 0x790a45f7, 0x0d42, 0x4876, { 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa } },   // IDXGISwapChain1
    { 0xa8be2ac4, 0x199f, 0x4946, { 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7 } },   // IDXGISwapChain2
    { 0x94d99bdb, 0xf1f8, 0x4ab0, { 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1 } },   // IDXGISwapChain3
    { 0x3d585d5a, 0xbd4a, 0x489e, { 0xb1, 0xf4, 0x3d, 0xbc, 0xb6, 0x45, 0x2f, 0xfb } },   // IDXGISwapChain4
};

// ---- calling the guest's Win32 / COM (user32 shims through their thunks, DXVK's factory) -------------------
static u64 api(Ctx *c, const char *name, int n, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    u64 fn = rt_thunk_for(NULL, name);
    if (!fn) { rt_log("dxgi12: no shim %s", name); return 0; }
    u64 args[7] = { a0, a1, a2, a3, a4, a5, a6 };
    return rt_guest_call(c, fn, n, args);
}
static u64 com_call(Ctx *c, u64 obj, int slot, int n, u64 a1, u64 a2, u64 a3) {
    u64 args[4] = { obj, a1, a2, a3 };
    return rt_guest_call(c, LD64(LD64(obj) + 8ull * slot), n, args);
}
extern void rt_display_mode(int *w, int *h, int *hz);   // user32.c

// ---- formats ---------------------------------------------------------------------------------------
static VkFormat vk_format(u32 dxgi) {
    switch (dxgi) {
    case 28: return VK_FORMAT_R8G8B8A8_UNORM;         case 29: return VK_FORMAT_R8G8B8A8_SRGB;
    case 87: return VK_FORMAT_B8G8R8A8_UNORM;         case 91: return VK_FORMAT_B8G8R8A8_SRGB;
    case 24: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 10: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}
static int is_srgb(u32 dxgi) { return dxgi == 29 || dxgi == 91; }

// ---- back buffers ------------------------------------------------------------------------------------
static void wait_queue_idle(SwapChain *s) {   // everything the game queued so far has finished on the GPU
    s->fence_value++;
    if (SUCCEEDED(s->queue->lpVtbl->Signal(s->queue, s->fence, s->fence_value)))
        s->fence->lpVtbl->SetEventOnCompletion(s->fence, s->fence_value, NULL);
}
static void drain(SwapChain *s) {   // (lock held) the presenter has handled every queued frame
    while (s->done != s->pushed) pthread_cond_wait(&s->cond, &s->lock);
}
static void destroy_buffers(SwapChain *s) {
    wait_queue_idle(s);
    for (u32 i = 0; i < MAX_BUFFERS; i++) {
        if (s->bb[i].res) { void *r = s->bb[i].res; s->bb[i].res->lpVtbl->Release(s->bb[i].res); d3d12_forget(r); }
        if (s->bb[i].img) vkDestroyImage(s->dev, s->bb[i].img, NULL);
        if (s->bb[i].mem) vkFreeMemory(s->dev, s->bb[i].mem, NULL);
        memset(&s->bb[i], 0, sizeof s->bb[i]);
    }
}
static int buffers_in_use(SwapChain *s) {   // the game must release its back buffer references before resizing
    for (u32 i = 0; i < s->desc.BufferCount; i++)
        if (s->bb[i].res) { ULONG r = s->bb[i].res->lpVtbl->AddRef(s->bb[i].res); s->bb[i].res->lpVtbl->Release(s->bb[i].res); if (r > 2) return 1; }
    return 0;
}
int d3d12mtl_enabled(void);                                          // WINTOMAC_D3D12_BACKEND=metal (d3d12mtl.m):
int d3d12mtl_present(void *queue, void *backbuffer, void *layer, int sync);   // back buffers are plain committed
static u32 create_buffers_metal(SwapChain *s) {                     // resources, Present draws into the CAMetalLayer
    if (!s->desc.BufferCount || s->desc.BufferCount > MAX_BUFFERS) return DXGI_ERROR_INVALID_CALL_;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, s->desc.Width, s->desc.Height, 1, 1, (DXGI_FORMAT)s->desc.Format,
                              { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    for (u32 i = 0; i < s->desc.BufferCount; i++) {
        HRESULT hr = s->device->lpVtbl->CreateCommittedResource(s->device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PRESENT, NULL,
                                                                 &IID_ID3D12Resource, (void **)&s->bb[i].res);
        if (FAILED(hr)) return (u32)hr;
    }
    s->cur = 0;
    return S_OK;
}
static u32 create_buffers(SwapChain *s) {
    if (d3d12mtl_enabled()) return create_buffers_metal(s);
    VkFormat f = vk_format(s->desc.Format);
    if (!f) { rt_log("dxgi12: unsupported back buffer format %u", s->desc.Format); return DXGI_ERROR_INVALID_CALL_; }
    if (!s->desc.BufferCount || s->desc.BufferCount > MAX_BUFFERS) return DXGI_ERROR_INVALID_CALL_;
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(s->pd, &mp);
    for (u32 i = 0; i < s->desc.BufferCount; i++) {
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D, f,
                                  { s->desc.Width, s->desc.Height, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
        if (vkCreateImage(s->dev, &ici, NULL, &s->bb[i].img)) return E_INVALIDARG_;
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(s->dev, s->bb[i].img, &mr);
        u32 type = 0;
        for (u32 t = 0; t < mp.memoryTypeCount; t++)
            if ((mr.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = t; break; }
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type };
        if (vkAllocateMemory(s->dev, &mai, NULL, &s->bb[i].mem) || vkBindImageMemory(s->dev, s->bb[i].img, s->bb[i].mem, 0)) return E_INVALIDARG_;
        struct vkd3d_image_resource_create_info ri = { VKD3D_STRUCTURE_TYPE_IMAGE_RESOURCE_CREATE_INFO };
        ri.vk_image = s->bb[i].img;
        ri.desc = (D3D12_RESOURCE_DESC){ D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, s->desc.Width, s->desc.Height, 1, 1, (DXGI_FORMAT)s->desc.Format,
                                         { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
        ri.flags = VKD3D_RESOURCE_INITIAL_STATE_TRANSITION | VKD3D_RESOURCE_PRESENT_STATE_TRANSITION;
        ri.present_state = D3D12_RESOURCE_STATE_COPY_SOURCE;   // PRESENT == TRANSFER_SRC_OPTIMAL: ready to blit
        HRESULT hr = vkd3d_create_image_resource(s->device, &ri, &s->bb[i].res);
        if (FAILED(hr)) { rt_log("dxgi12: vkd3d_create_image_resource = 0x%08x", (u32)hr); return (u32)hr; }
    }
    s->cur = 0;
    return S_OK;
}

// ---- Vulkan swapchain ----------------------------------------------------------------------------------
static void destroy_vk_swapchain(SwapChain *s) {
    if (s->sc) { vkDestroySwapchainKHR(s->dev, s->sc, NULL); s->sc = VK_NULL_HANDLE; }
    s->n_sc = 0;
}
static int ensure_vk_swapchain(SwapChain *s, u32 sync) {
    if (!s->surface) {
        void *layer = user32_metal_layer(s->hwnd);
        PFN_vkCreateMetalSurfaceEXT f = (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(s->vki, "vkCreateMetalSurfaceEXT");
        if (!layer || !f) { if (!s->present_failed++) rt_log("dxgi12: no Metal layer / VK_EXT_metal_surface for HWND %llx", (unsigned long long)s->hwnd); return 0; }
        VkMetalSurfaceCreateInfoEXT mi = { VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, NULL, 0, layer };
        if (f(s->vki, &mi, NULL, &s->surface)) return 0;
        u32 qfi = vkd3d_get_vk_queue_family_index(s->queue);
        VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(s->pd, qfi, s->surface, &ok);
        if (!ok) { rt_log("dxgi12: queue family %u can't present", qfi); return 0; }
    }
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!sync) {
        u32 n = 0; VkPresentModeKHR m[8]; vkGetPhysicalDeviceSurfacePresentModesKHR(s->pd, s->surface, &n, NULL); if (n > 8) n = 8;
        vkGetPhysicalDeviceSurfacePresentModesKHR(s->pd, s->surface, &n, m);
        for (u32 i = 0; i < n; i++) if (m[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m[i];
    }
    if (s->sc && !s->sc_dirty && mode == s->sc_mode) return 1;
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->pd, s->surface, &caps);
    // The game's size, not the layer's: the CAMetalLayer scales (virtual display modes, notch strip, Retina).
    VkExtent2D ext = { s->desc.Width, s->desc.Height };
    if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (caps.maxImageExtent.width && ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
    if (caps.maxImageExtent.height && ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    u32 count = s->desc.BufferCount < caps.minImageCount ? caps.minImageCount : s->desc.BufferCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkFormat fmt = is_srgb(s->desc.Format) ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, s->surface, count, fmt, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                                    ext, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                    VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE, s->sc };
    VkSwapchainKHR sc; VkResult r = vkCreateSwapchainKHR(s->dev, &ci, NULL, &sc);
    destroy_vk_swapchain(s);
    if (r) { if (!s->present_failed++) rt_log("dxgi12: vkCreateSwapchainKHR = %d", r); return 0; }
    s->sc = sc; s->sc_extent = ext; s->sc_mode = mode; s->sc_dirty = 0;
    s->n_sc = MAX_BUFFERS; vkGetSwapchainImagesKHR(s->dev, sc, &s->n_sc, s->sc_img);
    return 1;
}
static void blit_and_present(SwapChain *s, u32 buffer, u32 sync) {
    if (d3d12mtl_enabled()) {
        if (!d3d12mtl_present(s->queue, s->bb[buffer].res, user32_metal_layer(s->hwnd), sync) && !s->present_failed++)
            rt_log("dxgi12: Metal present failed (no layer / drawable) for HWND %llx", (unsigned long long)s->hwnd);
        return;
    }
    if (!ensure_vk_swapchain(s, sync)) return;
    VkQueue q = vkd3d_acquire_vk_queue(s->queue);
    u32 idx = 0;
    VkResult r = vkAcquireNextImageKHR(s->dev, s->sc, 1000000000ull, VK_NULL_HANDLE, s->vkfence, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { s->sc_dirty = 1; vkd3d_release_vk_queue(s->queue); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { if (!s->present_failed++) rt_log("dxgi12: vkAcquireNextImageKHR = %d", r); vkd3d_release_vk_queue(s->queue); return; }
    vkWaitForFences(s->dev, 1, &s->vkfence, VK_TRUE, UINT64_MAX); vkResetFences(s->dev, 1, &s->vkfence);
    VkImage src = s->bb[buffer].img, dst = s->sc_img[idx];
    vkResetCommandBuffer(s->cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(s->cmd, &bi);
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier b[2] = {
        { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, src, rng },
        { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dst, rng } };
    vkCmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
    VkImageBlit blit = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (i32)s->desc.Width, (i32)s->desc.Height, 1 } },
                         { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (i32)s->sc_extent.width, (i32)s->sc_extent.height, 1 } } };
    int same = s->sc_extent.width == s->desc.Width && s->sc_extent.height == s->desc.Height;
    vkCmdBlitImage(s->cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    VkImageMemoryBarrier p = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dst, rng };
    vkCmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &p);
    vkEndCommandBuffer(s->cmd);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &s->cmd };
    vkQueueSubmit(q, 1, &si, s->vkfence);
    vkWaitForFences(s->dev, 1, &s->vkfence, VK_TRUE, UINT64_MAX); vkResetFences(s->dev, 1, &s->vkfence);
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &s->sc, &idx };
    r = vkQueuePresentKHR(q, &pi);
    vkd3d_release_vk_queue(s->queue);
    static int trace = -1; if (trace < 0) trace = getenv("WINTOMAC_DXGI_TRACE") != NULL;
    if (trace) rt_log("dxgi12: present buffer %u -> image %u (%ux%u -> %ux%u, mode %d) = %d", buffer, idx, s->desc.Width, s->desc.Height,
                      s->sc_extent.width, s->sc_extent.height, s->sc_mode, r);
    // SUBOPTIMAL just means the layer's size differs from ours (the layer scales): keep going, like vkQuake.
    if (r == VK_ERROR_OUT_OF_DATE_KHR) s->sc_dirty = 1;
    else if (r && r != VK_SUBOPTIMAL_KHR && !s->present_failed++) rt_log("dxgi12: vkQueuePresentKHR = %d", r);
}

// ---- IDXGISwapChain4 ---------------------------------------------------------------------------------
#define SC ((SwapChain *)A1)
static int sc_iid(u64 g) {
    if (!g) return 0;
    if (!memcmp((void *)g, &IID_IUnknown_, 16)) return 1;
    for (unsigned i = 0; i < sizeof sc_iids / sizeof *sc_iids; i++) if (!memcmp((void *)g, &sc_iids[i], 16)) return 1;
    return 0;
}
SHIM(SC_QueryInterface) {
    if (!A3) RET(E_POINTER_);
    if (sc_iid(A2)) { __atomic_add_fetch(&SC->refs, 1, __ATOMIC_SEQ_CST); ST64(A3, A1); RET(S_OK); }
    ST64(A3, 0); d3d12_log_once("IDXGISwapChain::QueryInterface(other interface)"); RET(E_NOINTERFACE_);
}
SHIM(SC_AddRef) { RET(__atomic_add_fetch(&SC->refs, 1, __ATOMIC_SEQ_CST)); }
SHIM(SC_Release) {
    SwapChain *s = SC; u32 r = __atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST);
    if (r) RET(r);
    if (s->fullscreen) { /* leave fullscreen like DXGI does on release */ }
    pthread_mutex_lock(&s->lock); s->quit = 1; pthread_cond_broadcast(&s->cond); pthread_mutex_unlock(&s->lock);
    if (s->has_thread) pthread_join(s->thread, NULL);
    destroy_buffers(s);
    destroy_vk_swapchain(s);
    if (s->surface) vkDestroySurfaceKHR(s->vki, s->surface, NULL);
    if (s->dev) { vkDestroyFence(s->dev, s->vkfence, NULL); vkDestroyCommandPool(s->dev, s->pool, NULL); }
    s->fence->lpVtbl->Release(s->fence); s->device->lpVtbl->Release(s->device); s->queue->lpVtbl->Release(s->queue);
    // latency_sem belongs to the game once handed out (DXGI: the caller closes it)
    if (s->factory) com_call(c, s->factory, 2, 1, 0, 0, 0);
    pthread_cond_destroy(&s->cond); pthread_mutex_destroy(&s->lock); free(s);
    RET(0);
}
SHIM(SC_SetPrivateData) { RET(S_OK); }   // accepted, not stored
SHIM(SC_SetPrivateDataInterface) { RET(S_OK); }
SHIM(SC_GetPrivateData) { d3d12_log_once("IDXGISwapChain::GetPrivateData"); RET(DXGI_ERROR_NOT_FOUND_); }
SHIM(SC_GetParent) {  // (riid, out) -> the factory that created us
    if (!A3) RET(E_POINTER_);
    ST64(A3, 0);
    RET(SC->factory ? (u32)com_call(c, SC->factory, 0, 3, A2, A3, 0) : E_NOINTERFACE_);
}
SHIM(SC_GetDevice) {  // (riid, out) -> the command queue it was created with
    if (!A3) RET(E_POINTER_);
    u32 hr = SC->queue->lpVtbl->QueryInterface(SC->queue, (REFIID)A2, (void **)A3);
    RET(SUCCEEDED(hr) ? d3d12_out(A3, hr) : hr);
}
// Presenter thread: waits for each queued frame's fence, then blits and presents it.
#define RING (sizeof ((SwapChain *)0)->ring / sizeof ((SwapChain *)0)->ring[0])
static void frame_done(SwapChain *s) {   // (lock held) one more frame presented
    s->done++;
    pthread_cond_broadcast(&s->cond);
    if (s->latency_event && __atomic_add_fetch(&s->avail, 1, __ATOMIC_SEQ_CST) > 0) rt_signal_event(s->latency_event);
}
static void *presenter(void *arg) {
    SwapChain *s = arg;
    pthread_mutex_lock(&s->lock);
    for (;;) {
        while (s->done == s->pushed && !s->quit) pthread_cond_wait(&s->cond, &s->lock);
        if (s->done == s->pushed) break;
        typeof(s->ring[0]) op = s->ring[s->done % RING];
        pthread_mutex_unlock(&s->lock);
        s->fence->lpVtbl->SetEventOnCompletion(s->fence, op.fence_value, NULL);   // the frame's rendering is done
        blit_and_present(s, op.buffer, op.sync);
        pthread_mutex_lock(&s->lock);
        frame_done(s);
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}
static u32 do_present(Ctx *c, SwapChain *s, u32 sync, u32 flags) {
    if (flags & DXGI_PRESENT_TEST_) return S_OK;
    pthread_mutex_lock(&s->lock);
    s->fence_value++;
    s->queue->lpVtbl->Signal(s->queue, s->fence, s->fence_value);   // marks the end of this frame's GPU work
    if (s->sync_present) {
        s->fence->lpVtbl->SetEventOnCompletion(s->fence, s->fence_value, NULL);
        blit_and_present(s, s->cur, sync);
        s->pushed++; frame_done(s);
    } else {
        s->ring[s->pushed % RING].buffer = s->cur; s->ring[s->pushed % RING].sync = sync;
        s->ring[s->pushed % RING].fence_value = s->fence_value;
        s->pushed++;
        pthread_cond_broadcast(&s->cond);
    }
    s->cur = (s->cur + 1) % s->desc.BufferCount;
    s->present_count++;
    // (a) the buffer handed out next was presented BufferCount frames ago: its blit must be on the GPU queue
    //     before the game queues new work on it; (b) frame latency, unless the game paces with the waitable.
    u64 n = s->pushed, reuse = n + 1 > s->desc.BufferCount ? n + 1 - s->desc.BufferCount : 0;
    u64 lat = s->latency_event || n <= s->max_latency ? 0 : n - s->max_latency;
    while (s->done < reuse || s->done < lat) pthread_cond_wait(&s->cond, &s->lock);
    pthread_mutex_unlock(&s->lock);
    if (s->latency_event && __atomic_sub_fetch(&s->avail, 1, __ATOMIC_SEQ_CST) > 0) rt_signal_event(s->latency_event);
    return S_OK;
}
SHIM(SC_Present) { RET(do_present(c, SC, (u32)A2, (u32)A3)); }
SHIM(SC_Present1) { RET(do_present(c, SC, (u32)A2, (u32)A3)); }   // dirty rects / scroll ignored
SHIM(SC_GetBuffer) {  // (index, riid, out)
    SwapChain *s = SC; u32 i = (u32)A2;
    if (!A4) RET(E_POINTER_);
    ST64(A4, 0);
    if (i >= s->desc.BufferCount || !s->bb[i].res) RET(DXGI_ERROR_INVALID_CALL_);
    u32 hr = s->bb[i].res->lpVtbl->QueryInterface(s->bb[i].res, (REFIID)A3, (void **)A4);
    RET(SUCCEEDED(hr) ? d3d12_out(A4, hr) : hr);
}
static u64 containing_output(Ctx *c, SwapChain *s) {  // DXVK factory -> adapter 0 -> output 0
    u64 ad = 0, out = 0;
    if (!s->factory) return 0;
    u64 slot = (u64)&ad;
    if ((u32)com_call(c, s->factory, 7, 3, 0, slot, 0) & 0x80000000u || !ad) return 0;   // EnumAdapters
    com_call(c, ad, 7, 3, 0, (u64)&out, 0);                                               // EnumOutputs
    com_call(c, ad, 2, 1, 0, 0, 0);
    return out;
}
SHIM(SC_GetContainingOutput) {
    if (!A2) RET(E_POINTER_);
    u64 o = containing_output(c, SC); ST64(A2, o);
    RET(o ? S_OK : DXGI_ERROR_NOT_FOUND_);
}
// Fullscreen = a borderless window covering the display mode; user32 turns that into real (virtual-mode)
// fullscreen while the window is visible. Done through the user32 shims, like DXVK's own swapchain.
#define GWL_STYLE_ ((u64)(i64)-16)
SHIM(SC_SetFullscreenState) {  // (BOOL, IDXGIOutput *)
    SwapChain *s = SC; u32 want = (u32)A2 != 0;
    if (want == s->fullscreen) RET(S_OK);
    pthread_mutex_lock(&s->lock); drain(s); pthread_mutex_unlock(&s->lock);
    if (want) {
        s->saved_style = (u32)api(c, "GetWindowLongW", 2, s->hwnd, GWL_STYLE_, 0, 0, 0, 0, 0);
        api(c, "GetWindowRect", 2, s->hwnd, (u64)s->saved_rect, 0, 0, 0, 0, 0);
        int w, h, hz; rt_display_mode(&w, &h, &hz);
        u32 style = (s->saved_style & ~0x00CF0000u) | 0x80000000u;   // -WS_OVERLAPPEDWINDOW +WS_POPUP
        api(c, "SetWindowLongW", 3, s->hwnd, GWL_STYLE_, style, 0, 0, 0, 0);
        api(c, "SetWindowPos", 7, s->hwnd, 0, 0, 0, (u64)w, (u64)h, 0x0014 /* NOZORDER|NOACTIVATE */);
    } else {
        api(c, "SetWindowLongW", 3, s->hwnd, GWL_STYLE_, s->saved_style, 0, 0, 0, 0);
        api(c, "SetWindowPos", 7, s->hwnd, 0, (u64)(i64)s->saved_rect[0], (u64)(i64)s->saved_rect[1],
            (u64)(s->saved_rect[2] - s->saved_rect[0]), (u64)(s->saved_rect[3] - s->saved_rect[1]), 0x0014);
    }
    s->fullscreen = want; s->fs.Windowed = !want;
    RET(S_OK);
}
SHIM(SC_GetFullscreenState) {  // (BOOL *, IDXGIOutput **)
    if (A2) ST32(A2, SC->fullscreen);
    if (A3) ST64(A3, SC->fullscreen ? containing_output(c, SC) : 0);
    RET(S_OK);
}
SHIM(SC_GetDesc) {
    SwapChain *s = SC; if (!A2) RET(E_POINTER_);
    SwapDesc d = { s->desc.Width, s->desc.Height, s->fs.RefreshNum, s->fs.RefreshDen, s->desc.Format, s->fs.ScanlineOrdering, s->fs.Scaling,
                   s->desc.SampleCount, s->desc.SampleQuality, s->desc.BufferUsage, s->desc.BufferCount, s->hwnd, !s->fullscreen,
                   s->desc.SwapEffect, s->desc.Flags };
    memcpy((void *)A2, &d, sizeof d); RET(S_OK);
}
SHIM(SC_GetDesc1) { if (!A2) RET(E_POINTER_); memcpy((void *)A2, &SC->desc, sizeof(Desc1)); RET(S_OK); }
SHIM(SC_GetFullscreenDesc) { if (!A2) RET(E_POINTER_); FsDesc f = SC->fs; f.Windowed = !SC->fullscreen; memcpy((void *)A2, &f, sizeof f); RET(S_OK); }
SHIM(SC_GetHwnd) { if (!A2) RET(E_POINTER_); ST64(A2, SC->hwnd); RET(S_OK); }
SHIM(SC_GetCoreWindow) { if (A3) ST64(A3, 0); RET(DXGI_ERROR_INVALID_CALL_); }
static u32 resize(Ctx *c, SwapChain *s, u32 count, u32 w, u32 h, u32 fmt, u32 flags) {
    pthread_mutex_lock(&s->lock);
    drain(s);
    if (buffers_in_use(s)) { pthread_mutex_unlock(&s->lock); rt_log("dxgi12: ResizeBuffers while back buffers are referenced"); return DXGI_ERROR_INVALID_CALL_; }
    if (!w || !h) {  // 0 = the window's client size
        i32 rc[4] = { 0 }; api(c, "GetClientRect", 2, s->hwnd, (u64)rc, 0, 0, 0, 0, 0);
        if (!w) w = rc[2] - rc[0] > 0 ? (u32)(rc[2] - rc[0]) : 8;
        if (!h) h = rc[3] - rc[1] > 0 ? (u32)(rc[3] - rc[1]) : 8;
    }
    destroy_buffers(s);
    if (count) s->desc.BufferCount = count;
    if (fmt) s->desc.Format = fmt;
    s->desc.Width = w; s->desc.Height = h; s->desc.Flags = flags;
    s->sc_dirty = 1;
    u32 hr = create_buffers(s);
    pthread_mutex_unlock(&s->lock);
    return hr;
}
SHIM(SC_ResizeBuffers) { RET(resize(c, SC, (u32)A2, (u32)A3, (u32)A4, (u32)AN(5), (u32)AN(6))); }
SHIM(SC_ResizeBuffers1) { RET(resize(c, SC, (u32)A2, (u32)A3, (u32)A4, (u32)AN(5), (u32)AN(6))); }   // node masks / queues: single queue
SHIM(SC_ResizeTarget) {  // (const DXGI_MODE_DESC *): windowed -> resize the window's client area
    if (!A2) RET(DXGI_ERROR_INVALID_CALL_);
    if (!SC->fullscreen && LD32(A2) && LD32(A2 + 4))
        api(c, "SetWindowPos", 7, SC->hwnd, 0, 0, 0, LD32(A2), LD32(A2 + 4), 0x0016 /* NOMOVE|NOZORDER|NOACTIVATE */);
    RET(S_OK);
}
SHIM(SC_GetFrameStatistics) { RET(DXGI_ERROR_FRAME_STATISTICS_DISJOINT_); }
SHIM(SC_GetLastPresentCount) { if (!A2) RET(E_POINTER_); ST32(A2, SC->present_count); RET(S_OK); }
SHIM(SC_IsTemporaryMonoSupported) { RET(0); }
SHIM(SC_GetRestrictToOutput) { if (A2) ST64(A2, 0); RET(S_OK); }
SHIM(SC_SetBackgroundColor) { if (A2) memcpy(SC->bg, (void *)A2, 16); RET(S_OK); }
SHIM(SC_GetBackgroundColor) { if (A2) memcpy((void *)A2, SC->bg, 16); RET(S_OK); }
SHIM(SC_SetRotation) { SC->rotation = (u32)A2; RET(S_OK); }
SHIM(SC_GetRotation) { if (A2) ST32(A2, SC->rotation ? SC->rotation : 1); RET(S_OK); }
SHIM(SC_SetSourceSize) {
    if ((u32)A2 > SC->desc.Width || (u32)A3 > SC->desc.Height || !A2 || !A3) RET(E_INVALIDARG_);
    SC->src_w = (u32)A2; SC->src_h = (u32)A3; d3d12_log_once("IDXGISwapChain2::SetSourceSize (stored, not applied)"); RET(S_OK);
}
SHIM(SC_GetSourceSize) { if (A2) ST32(A2, SC->src_w ? SC->src_w : SC->desc.Width); if (A3) ST32(A3, SC->src_h ? SC->src_h : SC->desc.Height); RET(S_OK); }
SHIM(SC_SetMaximumFrameLatency) {
    SwapChain *s = SC;
    if (!A2 || A2 > 16) RET(DXGI_ERROR_INVALID_CALL_);
    if (s->latency_event) {   // more (or fewer) frames may start now
        i32 now = __atomic_add_fetch(&s->avail, (i32)A2 - (i32)s->max_latency, __ATOMIC_SEQ_CST);
        if (now > 0) rt_signal_event(s->latency_event);
    }
    s->max_latency = (u32)A2;
    RET(S_OK);
}
SHIM(SC_GetMaximumFrameLatency) { if (!A2) RET(E_POINTER_); ST32(A2, SC->max_latency); RET(S_OK); }
// Waitable object. DXGI uses a semaphore whose count is the number of frames the game may start; here it is an
// auto-reset event plus a counter (avail = max latency - frames in flight) that the presenter thread can signal
// from host code: the event is set whenever avail > 0, and Present consumes one.
SHIM(SC_GetFrameLatencyWaitableObject) {
    SwapChain *s = SC;
    if (!(s->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT_)) RET(0);
    pthread_mutex_lock(&s->lock);
    if (!s->latency_event) {
        s->avail = (i32)s->max_latency - (i32)(s->pushed - s->done);
        s->latency_event = api(c, "CreateEventW", 4, 0, 0, s->avail > 0, 0, 0, 0, 0);
    }
    pthread_mutex_unlock(&s->lock);
    RET(s->latency_event);
}
SHIM(SC_SetMatrixTransform) { RET(DXGI_ERROR_INVALID_CALL_); }   // composition swapchains only
SHIM(SC_GetMatrixTransform) { RET(DXGI_ERROR_INVALID_CALL_); }
SHIM(SC_GetCurrentBackBufferIndex) { RET(SC->cur); }
SHIM(SC_CheckColorSpaceSupport) {  // (colour space, UINT *flags): sRGB only
    if (!A3) RET(E_POINTER_);
    ST32(A3, (u32)A2 == 0 /* DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 */ ? 1 /* PRESENT */ : 0); RET(S_OK);
}
SHIM(SC_SetColorSpace1) { if ((u32)A2) { d3d12_log_once("IDXGISwapChain3::SetColorSpace1 (HDR)"); RET(E_INVALIDARG_); } RET(S_OK); }
SHIM(SC_SetHDRMetaData) { if ((u32)A2) d3d12_log_once("IDXGISwapChain4::SetHDRMetaData"); RET(S_OK); }

static const char *const sc_methods[] = {
    "QueryInterface", "AddRef", "Release", "SetPrivateData", "SetPrivateDataInterface", "GetPrivateData", "GetParent", "GetDevice",
    "Present", "GetBuffer", "SetFullscreenState", "GetFullscreenState", "GetDesc", "ResizeBuffers", "ResizeTarget", "GetContainingOutput",
    "GetFrameStatistics", "GetLastPresentCount", "GetDesc1", "GetFullscreenDesc", "GetHwnd", "GetCoreWindow", "Present1",
    "IsTemporaryMonoSupported", "GetRestrictToOutput", "SetBackgroundColor", "GetBackgroundColor", "SetRotation", "GetRotation",
    "SetSourceSize", "GetSourceSize", "SetMaximumFrameLatency", "GetMaximumFrameLatency", "GetFrameLatencyWaitableObject",
    "SetMatrixTransform", "GetMatrixTransform", "GetCurrentBackBufferIndex", "CheckColorSpaceSupport", "SetColorSpace1",
    "ResizeBuffers1", "SetHDRMetaData", NULL };

// ---- IWineDXGISwapChainFactory on the command queue ------------------------------------------------------
static const GUID IID_IUnknown2_ = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
SHIM(SCF_QueryInterface) {
    if (!A3) RET(E_POINTER_);
    if (A2 && (!memcmp((void *)A2, &wt_IID_IWineDXGISwapChainFactory, 16) || !memcmp((void *)A2, &IID_IUnknown2_, 16))) {
        __atomic_add_fetch(&((Factory *)A1)->refs, 1, __ATOMIC_SEQ_CST); ST64(A3, A1); RET(S_OK);
    }
    ST64(A3, 0); RET(E_NOINTERFACE_);
}
SHIM(SCF_AddRef) { RET(__atomic_add_fetch(&((Factory *)A1)->refs, 1, __ATOMIC_SEQ_CST)); }
SHIM(SCF_Release) {
    Factory *f = (Factory *)A1; u32 r = __atomic_sub_fetch(&f->refs, 1, __ATOMIC_SEQ_CST);
    if (!r) { f->queue->lpVtbl->Release(f->queue); free(f); }
    RET(r);
}
// (This, IDXGIFactory *factory, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **)
SHIM(SCF_CreateSwapChainForHwnd) {
    Factory *f = (Factory *)A1; u64 out = AN(7);
    if (!out || !A4 || !A3) RET(DXGI_ERROR_INVALID_CALL_);
    ST64(out, 0);
    static u64 *vtbl;
    if (!vtbl) {
        int n = 0; while (sc_methods[n]) n++;
        vtbl = calloc(n + 1, 8);
        for (int i = 0; i < n; i++) {
            char nm[128]; snprintf(nm, sizeof nm, "__wintomac_DXGISwapChain12_%s", sc_methods[i]);
            if (!(vtbl[i] = rt_thunk_for(NULL, nm))) rt_log("dxgi12: missing %s", nm);
        }
    }
    SwapChain *s = calloc(1, sizeof *s);
    s->vtbl = (u64)vtbl; s->refs = 1; pthread_mutex_init(&s->lock, NULL); pthread_cond_init(&s->cond, NULL);
    s->queue = f->queue; s->queue->lpVtbl->AddRef(s->queue);
    if (FAILED(s->queue->lpVtbl->GetDevice(s->queue, &IID_ID3D12Device, (void **)&s->device))) { s->queue->lpVtbl->Release(s->queue); free(s); RET(E_INVALIDARG_); }
    s->hwnd = A3; memcpy(&s->desc, (void *)A4, sizeof s->desc);
    if (AN(5)) memcpy(&s->fs, (void *)AN(5), sizeof s->fs); else s->fs.Windowed = 1;
    s->max_latency = 3;
    if (!s->desc.Width || !s->desc.Height) {
        i32 rc[4] = { 0 }; api(c, "GetClientRect", 2, s->hwnd, (u64)rc, 0, 0, 0, 0, 0);
        if (!s->desc.Width) s->desc.Width = rc[2] - rc[0] > 0 ? (u32)(rc[2] - rc[0]) : 8;
        if (!s->desc.Height) s->desc.Height = rc[3] - rc[1] > 0 ? (u32)(rc[3] - rc[1]) : 8;
    }
    s->device->lpVtbl->CreateFence(s->device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&s->fence);
    if (!d3d12mtl_enabled()) {   // Vulkan presentation objects (the Metal backend presents with Metal)
    s->vki = vkd3d_instance_get_vk_instance(vkd3d_instance_from_device(s->device));
    s->pd = vkd3d_get_vk_physical_device(s->device); s->dev = vkd3d_get_vk_device(s->device);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    vkd3d_get_vk_queue_family_index(s->queue) };
    vkCreateCommandPool(s->dev, &pci, NULL, &s->pool);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    vkAllocateCommandBuffers(s->dev, &cai, &s->cmd);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(s->dev, &fci, NULL, &s->vkfence);
    }
    u32 hr = create_buffers(s);
    const char *sp = getenv("WINTOMAC_DXGI_SYNC_PRESENT");
    s->sync_present = sp && *sp == '1';
    if (SUCCEEDED(hr) && !s->sync_present) s->has_thread = !pthread_create(&s->thread, NULL, presenter, s);
    if (!s->has_thread) s->sync_present = 1;
    s->factory = A2;
    if (s->factory) com_call(c, s->factory, 1, 1, 0, 0, 0);   // AddRef
    if (FAILED(hr)) { c->r[RCX] = (u64)s; sh_SC_Release(c); RET(hr); }   // refs 1 -> 0: tears everything down
    if (rt_trace_enabled()) rt_log("dxgi12: swapchain %ux%u fmt %u x%u for HWND %llx", s->desc.Width, s->desc.Height, s->desc.Format,
                                   s->desc.BufferCount, (unsigned long long)s->hwnd);
    ST64(out, (u64)s);
    RET(S_OK);
}

u64 d3d12_swapchain_factory(void *host_queue) {
    static u64 *vtbl;
    if (!host_queue) return 0;
    if (!vtbl) {
        static const char *const m[] = { "QueryInterface", "AddRef", "Release", "CreateSwapChainForHwnd", NULL };
        vtbl = calloc(5, 8);
        for (int i = 0; m[i]; i++) { char n[96]; snprintf(n, sizeof n, "__wintomac_DXGISwapChainFactory12_%s", m[i]); vtbl[i] = rt_thunk_for(NULL, n); }
    }
    Factory *f = calloc(1, sizeof *f);
    f->vtbl = (u64)vtbl; f->refs = 1; f->queue = host_queue;
    f->queue->lpVtbl->AddRef(f->queue);
    return (u64)f;
}

const Shim shims_dxgi12[] = {
#define F(n) { NULL, "__wintomac_DXGISwapChainFactory12_" #n, sh_SCF_##n }
#define S(n) { NULL, "__wintomac_DXGISwapChain12_" #n, sh_SC_##n }
    F(QueryInterface), F(AddRef), F(Release), F(CreateSwapChainForHwnd),
    S(QueryInterface), S(AddRef), S(Release), S(SetPrivateData), S(SetPrivateDataInterface), S(GetPrivateData), S(GetParent), S(GetDevice),
    S(Present), S(GetBuffer), S(SetFullscreenState), S(GetFullscreenState), S(GetDesc), S(ResizeBuffers), S(ResizeTarget), S(GetContainingOutput),
    S(GetFrameStatistics), S(GetLastPresentCount), S(GetDesc1), S(GetFullscreenDesc), S(GetHwnd), S(GetCoreWindow), S(Present1),
    S(IsTemporaryMonoSupported), S(GetRestrictToOutput), S(SetBackgroundColor), S(GetBackgroundColor), S(SetRotation), S(GetRotation),
    S(SetSourceSize), S(GetSourceSize), S(SetMaximumFrameLatency), S(GetMaximumFrameLatency), S(GetFrameLatencyWaitableObject),
    S(SetMatrixTransform), S(GetMatrixTransform), S(GetCurrentBackBufferIndex), S(CheckColorSpaceSupport), S(SetColorSpace1),
    S(ResizeBuffers1), S(SetHDRMetaData),
#undef F
#undef S
    { 0, 0, 0 }
};
