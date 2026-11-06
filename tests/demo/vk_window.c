// A plain Win32 + Vulkan program: window, message loop, swapchain, spinning triangle.
// Nothing here knows about macOS. Usage: vk_window.exe [frames]  (default: run until closed / Esc)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include "tri_vert.h"
#include "tri_frag.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d\n", #x, r_); ExitProcess(1); } } while (0)
static int running = 1, resized = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: printf("WM_CREATE (param=%s)\n", (const char *)((CREATESTRUCTA *)lp)->lpCreateParams); return 0;
    case WM_SIZE: resized = 1; return 0;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) PostMessageA(h, WM_CLOSE, 0, 0); else printf("key vk=0x%02x\n", (unsigned)wp); return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int main(int argc, char **argv) {
    int max_frames = argc > 1 ? atoi(argv[1]) : 0;
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSEXA wc = { sizeof wc, CS_HREDRAW | CS_VREDRAW, WndProc, 0, 0, hi, NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, "wintomac_demo", NULL };
    if (!RegisterClassExA(&wc)) { printf("RegisterClassEx failed\n"); return 1; }
    RECT r = { 0, 0, 800, 600 }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, "wintomac_demo", "Win32 + Vulkan, recompiled for arm64 macOS", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, "hello-from-createparams");
    if (!hwnd) { printf("CreateWindowEx failed\n"); return 1; }

    const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vk_window", 1, "none", 1, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iext };
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0, hi, hwnd };
    VkSurfaceKHR surf; CHECK(vkCreateWin32SurfaceKHR(inst, &sci, NULL, &surf));
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props); printf("GPU: %s\n", props.deviceName);
    uint32_t qfi = 0, nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    for (uint32_t i = 0; i < nq; i++) { VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &ok); if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) { qfi = i; break; } }
    float prio = 1; const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfi, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dext };
    VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, qfi, 0, &q);

    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL);
    VkSurfaceFormatKHR fmts[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts);
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < nf; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) fmt = fmts[i];

    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof tri_vert, tri_vert };
    VkShaderModule vs, fs; CHECK(vkCreateShaderModule(dev, &smci, NULL, &vs));
    smci.codeSize = sizeof tri_frag; smci.pCode = tri_frag; CHECK(vkCreateShaderModule(dev, &smci, NULL, &fs));
    VkAttachmentDescription att = { 0, fmt.format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                    VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref };
    VkSubpassDependency depd = { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &depd };
    VkRenderPass rp; CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr };
    VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE, 0, 0, 0, 0, 1 };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { 0 }; cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, NULL, &cb, &ds, pl, rp, 0 };
    VkPipeline pipe; CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfi };
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkSemaphoreCreateInfo semci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired; CHECK(vkCreateSemaphore(dev, &semci, NULL, &acquired));
    VkFenceCreateInfo fenci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT };
    VkFence fence; CHECK(vkCreateFence(dev, &fenci, NULL, &fence));

    VkSwapchainKHR sc = VK_NULL_HANDLE; VkImage imgs[8]; VkImageView views[8]; VkFramebuffer fbs[8]; VkSemaphore rendered[8]; uint32_t nimg = 0; VkExtent2D ext = { 0 };
    LARGE_INTEGER freq, t0, now; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    int frames = 0;
    resized = 1;
    while (running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) running = 0; TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (!running) break;
        if (resized) {  // (re)create the swapchain
            vkDeviceWaitIdle(dev);
            VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
            RECT cr; GetClientRect(hwnd, &cr);
            ext = caps.currentExtent.width != 0xFFFFFFFF ? caps.currentExtent : (VkExtent2D){ (uint32_t)cr.right, (uint32_t)cr.bottom };
            if (!ext.width || !ext.height) { Sleep(10); continue; }
            VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surf, caps.minImageCount + 1 > 3 ? 3 : caps.minImageCount + 1,
                                              fmt.format, fmt.colorSpace, ext, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                              caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, sc };
            VkSwapchainKHR nsc; CHECK(vkCreateSwapchainKHR(dev, &swci, NULL, &nsc));
            for (uint32_t i = 0; i < nimg; i++) { vkDestroyFramebuffer(dev, fbs[i], NULL); vkDestroyImageView(dev, views[i], NULL); vkDestroySemaphore(dev, rendered[i], NULL); }
            if (sc) vkDestroySwapchainKHR(dev, sc, NULL);
            sc = nsc; nimg = 8; vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs);
            for (uint32_t i = 0; i < nimg; i++) {
                VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, imgs[i], VK_IMAGE_VIEW_TYPE_2D, fmt.format, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                CHECK(vkCreateImageView(dev, &ivci, NULL, &views[i]));
                VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &views[i], ext.width, ext.height, 1 };
                CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fbs[i]));
                CHECK(vkCreateSemaphore(dev, &semci, NULL, &rendered[i]));
            }
            printf("swapchain %ux%u, %u images\n", ext.width, ext.height, nimg);
            resized = 0;
        }
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        uint32_t idx; VkResult ar = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) { resized = 1; continue; }
        vkResetFences(dev, 1, &fence);
        QueryPerformanceCounter(&now); float t = (float)(now.QuadPart - t0.QuadPart) / (float)freq.QuadPart;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cmd, &cbbi);
        VkClearValue clear = { .color = { { 0.06f, 0.06f, 0.12f + 0.05f * (float)(frames % 60) / 60.f, 1 } } };
        VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fbs[idx], { { 0, 0 }, ext }, 1, &clear };
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        float side = (float)(ext.width < ext.height ? ext.width : ext.height);
        VkViewport vp = { (ext.width - side) / 2, (ext.height - side) / 2, side, side, 0, 1 }; VkRect2D scis = { { 0, 0 }, ext };
        vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &scis);
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &t);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd); vkEndCommandBuffer(cmd);
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acquired, &wait, 1, &cmd, 1, &rendered[idx] };
        CHECK(vkQueueSubmit(q, 1, &si, fence));
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &rendered[idx], 1, &sc, &idx };
        VkResult pr = vkQueuePresentKHR(q, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) resized = 1;
        frames++;
        if (frames % 120 == 0) { QueryPerformanceCounter(&now); printf("%d frames, %.1f fps\n", frames, frames / ((double)(now.QuadPart - t0.QuadPart) / freq.QuadPart)); }
        if (max_frames && frames >= max_frames) running = 0;
    }
    vkDeviceWaitIdle(dev);
    printf("presented %d frames, exiting cleanly\n", frames);
    for (uint32_t i = 0; i < nimg; i++) { vkDestroyFramebuffer(dev, fbs[i], NULL); vkDestroyImageView(dev, views[i], NULL); vkDestroySemaphore(dev, rendered[i], NULL); }
    vkDestroySwapchainKHR(dev, sc, NULL); vkDestroySemaphore(dev, acquired, NULL); vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL); vkDestroyPipeline(dev, pipe, NULL); vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyRenderPass(dev, rp, NULL); vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL);
    vkDestroyDevice(dev, NULL); vkDestroySurfaceKHR(inst, surf, NULL); vkDestroyInstance(inst, NULL);
    return 0;
}
