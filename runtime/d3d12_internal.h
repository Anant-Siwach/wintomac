// Shared between runtime/d3d12.c (D3D12 object wrappers) and runtime/d3d12_swapchain.c (DXGI presentation).
#pragma once
#ifndef VK_USE_PLATFORM_METAL_EXT
#define VK_USE_PLATFORM_METAL_EXT 1
#endif
#include <vulkan/vulkan.h>
#include <vkd3d.h>
#include "win32.h"

// Every D3D12 object the guest sees: { guest vtable, host libvkd3d object, magic }, cached per host object.
#define D3D12_MAGIC 0x32314433u   // "3D12"
typedef struct { u64 vtbl; void *host; u32 magic; int kind; const void *host_vtbl; u32 xflags; } D3D12Wrap;
enum {
    D3D12_PSO_SKIP_DRAWS = 1,     // ID3D12PipelineState: unsupported geometry shader / stream output: draws are skipped
    D3D12_PSO_GS_STRIPPED = 2,    //   a pass-through-shaped geometry shader was dropped (approximation)
    D3D12_LIST_SKIP_DRAWS = 1,    // ID3D12GraphicsCommandList: the current PSO has D3D12_PSO_SKIP_DRAWS
};
static inline D3D12Wrap *d3d12_wrapper(u64 g) { D3D12Wrap *w = (D3D12Wrap *)g; return w && w->magic == D3D12_MAGIC ? w : NULL; }

#define E_INVALIDARG_ 0x80070057u
#define E_NOINTERFACE_ 0x80004002u
#define E_NOTIMPL_ 0x80004001u
#define E_POINTER_ 0x80004003u

void *d3d12_host(u64 guest_obj);          // host libvkd3d object behind a guest wrapper, NULL if not ours
void *d3d12_in(u64 g, int *bad);          // interface argument: NULL stays NULL, a non-wrapper sets *bad
void d3d12_qi(Ctx *c, const char *kind);  // QueryInterface of every wrapped object (d3d12_ext.c)
void d3d12_trace(const char *what);       // WINTOMAC_D3D12_TRACE=1
extern int d3d12_tracing;
#define D3D12_TRACE(what) do { if (d3d12_tracing) d3d12_trace(what); } while (0)
void d3d12_bad_arg(const char *what);
u64 d3d12_info_queue(void);               // harmless ID3D12InfoQueue1 / DRED objects (d3d12_ext.c)
u64 d3d12_dred_settings(void);
int d3d12_is_dred_settings_iid(u64 g);
u64 d3d12_wrap(void *host);               // guest wrapper for a host object (no reference taken)
u32 d3d12_out(u64 out, u32 hr);           // *out: host object -> guest wrapper
void d3d12_forget(void *host);            // drop the cached wrapper of a destroyed host object
struct vkd3d_instance *d3d12_vkd3d_instance(void);
u64 d3d12_swapchain_factory(void *host_queue);   // IWineDXGISwapChainFactory for a command queue (new ref)
u64 d3d12_debug_object(void);             // harmless ID3D12Debug* (see d3d12.c)
extern const GUID wt_IID_IWineDXGISwapChainFactory;
void d3d12_log_once(const char *what);    // "unimplemented" messages, once per name
