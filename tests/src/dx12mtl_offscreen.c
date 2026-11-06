// tests/src/dx12_offscreen.c on the experimental Metal backend (make check runs dx12mtl_* with
// WINTOMAC_D3D12_BACKEND=metal). The reference output is the native libvkd3d build of the same source, so the
// Metal path must reproduce the Vulkan/MoltenVK path's pixels exactly.
#include "dx12_offscreen.c"
